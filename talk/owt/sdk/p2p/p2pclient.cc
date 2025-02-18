// Copyright (C) <2018> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0

// Seems we cannot reorder the header inclusion. Otherwise
// SendMessage will fail compilation.
#include <algorithm>
#include <future>
#include "webrtc/api/task_queue/default_task_queue_factory.h"
#include "webrtc/rtc_base/checks.h"
#include "webrtc/rtc_base/logging.h"
#include "webrtc/rtc_base/strings/json.h"
#include "webrtc/rtc_base/task_queue.h"
#include "webrtc/rtc_base/third_party/base64/base64.h"
#include "talk/owt/sdk/base/eventtrigger.h"
#include "talk/owt/sdk/base/stringutils.h"
#include "talk/owt/sdk/include/cpp/owt/base/stream.h"
#include "talk/owt/sdk/include/cpp/owt/p2p/p2pclient.h"
#include "talk/owt/sdk/p2p/p2ppeerconnectionchannel.h"
#include "talk/owt/sdk/p2p/p2ppeerconnectionchannelobservercppimpl.h"
#include "talk/owt/sdk/p2p/p2psignalingsenderimpl.h"

using namespace rtc;
namespace owt {
namespace p2p {
enum IcsP2PError : int {
  kWebrtcIceGatheringPolicyUnsupported = 2601,
};

P2PClient::P2PClient(
    P2PClientConfiguration& configuration,
    std::shared_ptr<P2PSignalingChannelInterface> signaling_channel)
    : signaling_channel_(signaling_channel),
      signaling_sender_(std::make_unique<P2PSignalingSenderImpl>(this)),
      pcc_observer_adapter_(
          std::make_unique<P2PPeerConnectionChannelObserverCppImpl>(*this)),
      configuration_(configuration) {
  RTC_CHECK(signaling_channel_);
  signaling_channel_->AddObserver(*this);
  auto task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
  event_queue_ =
      std::make_unique<rtc::TaskQueue>(task_queue_factory->CreateTaskQueue(
          "P2PClientEventQueue", webrtc::TaskQueueFactory::Priority::NORMAL));
  signaling_queue_ =
      std::make_unique<rtc::TaskQueue>(task_queue_factory->CreateTaskQueue(
          "P2PClientSignalingQueue",
          webrtc::TaskQueueFactory::Priority::NORMAL));
}

P2PClient::~P2PClient() = default;

void P2PClient::Connect(
    const std::string& host,
    const std::string& token,
    std::function<void(const std::string&)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  RTC_CHECK(signaling_channel_);
  std::weak_ptr<P2PClient> weak_this = shared_from_this();
  signaling_channel_->Connect(
      host, token,
      [on_success, weak_this](const std::string& user_id) {
        auto that = weak_this.lock();
        if (that)
          that->SetLocalId(user_id);
        if (on_success)
          on_success(user_id);
      },
      on_failure);
}
void P2PClient::Disconnect(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  RTC_CHECK(signaling_channel_);
  signaling_channel_->Disconnect(on_success, on_failure);
}
void P2PClient::AddAllowedRemoteId(const std::string& target_id) {
  if (std::find(allowed_remote_ids_.begin(), allowed_remote_ids_.end(),
                target_id) != allowed_remote_ids_.end()) {
    RTC_LOG(LS_INFO) << "Adding duplicated remote id.";
    return;
  }
  const std::lock_guard<std::mutex> lock(remote_ids_mutex_);
  allowed_remote_ids_.push_back(target_id);
}
void P2PClient::RemoveAllowedRemoteId(
    const std::string& target_id,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (std::find(allowed_remote_ids_.begin(), allowed_remote_ids_.end(),
                target_id) == allowed_remote_ids_.end()) {
    if (on_failure) {
      event_queue_->PostTask([on_failure] {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kP2PClientRemoteNotExisted,
                          "Trying to delete non-existed remote id."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(remote_ids_mutex_);
    allowed_remote_ids_.erase(std::remove(allowed_remote_ids_.begin(),
                                          allowed_remote_ids_.end(), target_id),
                              allowed_remote_ids_.end());
  }
  Stop(target_id, on_success, on_failure);
}
void P2PClient::Publish(
    const std::string& target_id,
    std::shared_ptr<LocalStream> stream,
    std::function<void(std::shared_ptr<P2PPublication>)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  // Firstly check whether target_id is in the allowed_remote_ids_ list.
  if (std::find(allowed_remote_ids_.begin(), allowed_remote_ids_.end(),
                target_id) == allowed_remote_ids_.end()) {
    if (on_failure) {
      event_queue_->PostTask([on_failure] {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kP2PClientRemoteNotAllowed,
                          "Publishing a stream cannot be done since the remote "
                          "user is not allowed."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  // Secondly use pcc to publish the stream.
  auto pcc = GetPeerConnectionChannel(target_id);
  std::weak_ptr<P2PClient> weak_this = shared_from_this();
  pcc->Publish(stream,
               [on_success, weak_this, target_id, stream]() {
                 if (!on_success)
                   return;
                 auto that = weak_this.lock();
                 if (!that)
                   return;
                 std::shared_ptr<P2PPublication> publication(
                     new P2PPublication(that, target_id, stream));
                 that->event_queue_->PostTask(
                     [on_success, publication] { on_success(publication); });
               },
               on_failure);
}
void P2PClient::Send(
    const std::string& target_id,
    const std::string& message,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  Send(target_id, message, true, on_success, on_failure);
}

void P2PClient::Send(
    const std::string& target_id,
    const std::string& message,
    bool is_reliable,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  // Firstly check whether target_id is in the allowed_remote_ids_ list.
  if (std::find(allowed_remote_ids_.begin(), allowed_remote_ids_.end(),
                target_id) == allowed_remote_ids_.end()) {
    if (on_failure) {
      event_queue_->PostTask([on_failure] {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kP2PClientRemoteNotAllowed,
                          "Sending a message cannot be done since the remote "
                          "user is not allowed."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  // Secondly use pcc to send the message.
  auto pcc = GetPeerConnectionChannel(target_id);
  pcc->Send(message, is_reliable, on_success, on_failure);
}
void P2PClient::Stop(
    const std::string& target_id,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (!IsPeerConnectionChannelCreated(target_id)) {
    if (on_failure) {
      event_queue_->PostTask([on_failure] {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kP2PClientInvalidState,
                          "Non-existed chat need not be stopped."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  auto pcc = GetPeerConnectionChannel(target_id);
  pcc->Stop(on_success, on_failure);
  {
    const std::lock_guard<std::mutex> lock(pc_channels_mutex_);
    pc_channels_.erase(target_id);
  }
}
void P2PClient::GetConnectionStats(
    const std::string& target_id,
    std::function<void(std::shared_ptr<owt::base::ConnectionStats>)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (!IsPeerConnectionChannelCreated(target_id)) {
    if (on_failure) {
      event_queue_->PostTask([on_failure] {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kP2PClientInvalidState,
                          "Non-existed peer connection cannot provide stats."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  auto pcc = GetPeerConnectionChannel(target_id);
  pcc->GetConnectionStats(on_success, on_failure);
}

void P2PClient::GetConnectionStats(
    const std::string& target_id,
    std::function<void(std::shared_ptr<owt::base::RTCStatsReport>)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (!IsPeerConnectionChannelCreated(target_id)) {
    if (on_failure) {
      event_queue_->PostTask([on_failure] {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kP2PClientInvalidState,
                          "Non-existed peer connection cannot provide stats."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  auto pcc = GetPeerConnectionChannel(target_id);
  pcc->GetConnectionStats(on_success, on_failure);
}

void P2PClient::SetLocalId(const std::string& local_id) {
  local_id_ = local_id;
}
void P2PClient::OnSignalingMessage(const std::string& message,
                                   const std::string& remote_id) {
  RTC_LOG(LS_WARNING) << "Receiving signaling message from remote:" << message;
  std::weak_ptr<P2PClient> weak_this = shared_from_this();
  signaling_queue_->PostTask([weak_this, remote_id, message]() {
    auto that = weak_this.lock();
    // First to check whether remote_id is in the allowed_remote_ids_ list.
    if (std::find(that->allowed_remote_ids_.begin(),
                  that->allowed_remote_ids_.end(),
                  remote_id) == that->allowed_remote_ids_.end()) {
      RTC_LOG(LS_WARNING)
          << "Chat cannot be setup since the remote user is not allowed.";
      return;
    }
    if (!that->IsPeerConnectionChannelCreated(remote_id)) {
      if (message.find("\"type\":\"chat-closed\"") != std::string::npos) {
        RTC_LOG(LS_WARNING) << "Non-existed chat cannot be stopped.";
        return;
      }
    } else if (message.find("\"type\":\"offer\"") != std::string::npos) {
      RTC_LOG(LS_ERROR) << "Received offer from remote.";
      // If we don't have a PC before we receive an offer.
      auto pcc = that->GetPeerConnectionChannel(remote_id);
      if (pcc->HaveLocalOffer() && that->local_id_.compare(remote_id) > 0) {
        // If our ID is larger than remote, make the remote side as the
        // publisher (offerer) In case we already have an offer. So we
        // remove current PCC and create answer.
        std::shared_ptr<LocalStream> stream = pcc->GetLatestLocalStream();
        std::function<void()> success_callback =
            pcc->GetLatestPublishSuccessCallback();
        std::function<void(std::unique_ptr<Exception>)> failure_callback =
            pcc->GetLatestPublishFailureCallback();
        pcc->Stop(nullptr, nullptr);
        {
          // If we already created offer,
          const std::lock_guard<std::mutex> lock(that->pc_channels_mutex_);
          that->pc_channels_.erase(remote_id);
        }
        auto new_pcc = that->GetPeerConnectionChannel(remote_id);
        new_pcc->OnIncomingSignalingMessage(message);
        new_pcc->Publish(stream, success_callback, failure_callback);
        return;
      }
    } else if (message.find("\"type\":\"chat-closed\"") != std::string::npos) {
      RTC_LOG(LS_ERROR) << "Handle the situation that PCC is created by "
                           "received Chat-closed.";
      int code = 0;
      std::string error = "";
      Json::Reader reader;
      Json::Value json_message;
      if (reader.parse(message, json_message)) {
        Json::Value stop_info;
        rtc::GetValueFromJsonObject(json_message, "data", &stop_info);
        rtc::GetIntFromJsonObject(stop_info, "code", &code);
        rtc::GetStringFromJsonObject(stop_info, "message", &error);

        if (code == kWebrtcIceGatheringPolicyUnsupported) {
          auto pcc = that->GetPeerConnectionChannel(remote_id);
          std::shared_ptr<LocalStream> stream = pcc->GetLatestLocalStream();
          std::function<void()> success_callback =
              pcc->GetLatestPublishSuccessCallback();
          std::function<void(std::unique_ptr<Exception>)> failure_callback =
              pcc->GetLatestPublishFailureCallback();
          pcc->SetAbandoned();
          {
            const std::lock_guard<std::mutex> lock(that->pc_channels_mutex_);
            that->pc_channels_.erase(remote_id);
          }

          auto new_pcc = that->GetPeerConnectionChannel(remote_id);
          new_pcc->Publish(stream, success_callback, failure_callback);
          return;
        }
        auto pcc = that->GetPeerConnectionChannel(remote_id);
        // Don't send stop to remote.
        pcc->SetAbandoned();
        {
          const std::lock_guard<std::mutex> lock(that->pc_channels_mutex_);
          that->pc_channels_.erase(remote_id);
        }

        return;
      }
    }
    // Secondly dispatch the message to pcc.
    auto pcc = that->GetPeerConnectionChannel(remote_id);
    pcc->OnIncomingSignalingMessage(message);
  });
}
void P2PClient::OnServerDisconnected() {
  EventTrigger::OnEvent0(observers_, event_queue_,
                         &P2PClientObserver::OnServerDisconnected);
}
void P2PClient::SendSignalingMessage(
    const std::string& message,
    const std::string& remote_id,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  signaling_channel_->SendMessage(message, remote_id, on_success, on_failure);
}
void P2PClient::AddObserver(P2PClientObserver& observer) {
  observers_.push_back(observer);
}
void P2PClient::RemoveObserver(P2PClientObserver& observer) {
  observers_.erase(
      std::find_if(observers_.begin(), observers_.end(),
                   [&](std::reference_wrapper<P2PClientObserver> o) -> bool {
                     return &observer == &(o.get());
                   }));
}
void P2PClient::Unpublish(
    const std::string& target_id,
    std::shared_ptr<LocalStream> stream,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (!IsPeerConnectionChannelCreated(target_id)) {
    if (on_failure) {
      event_queue_->PostTask([on_failure] {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kP2PClientInvalidState,
                          "Non-existed chat need not be unpublished."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  auto pcc = GetPeerConnectionChannel(target_id);
  pcc->Unpublish(stream, on_success, on_failure);
}
bool P2PClient::IsPeerConnectionChannelCreated(const std::string& target_id) {
  const std::lock_guard<std::mutex> lock(pc_channels_mutex_);
  if (pc_channels_.find(target_id) == pc_channels_.end())
    return false;
  return true;
}
std::shared_ptr<P2PPeerConnectionChannel> P2PClient::GetPeerConnectionChannel(
    const std::string& target_id,
    bool replace) {
  const std::lock_guard<std::mutex> lock(pc_channels_mutex_);
  auto pcc_it = pc_channels_.find(target_id);
  // if the channel has already been abandoned
  if (pcc_it != pc_channels_.end() &&
      (replace || pcc_it->second->IsAbandoned())) {
    pc_channels_.erase(target_id);
    pcc_it->second->DisableSendingStop();
    pcc_it = pc_channels_.end();
  }
  // Create new channel if it doesn't exist.
  if (pcc_it == pc_channels_.end()) {
    PeerConnectionChannelConfiguration config =
        GetPeerConnectionChannelConfiguration();
    std::shared_ptr<P2PPeerConnectionChannel> pcc =
        std::shared_ptr<P2PPeerConnectionChannel>(new P2PPeerConnectionChannel(
            config, local_id_, target_id, signaling_sender_.get(),
            event_queue_));
    pcc->AddObserver(pcc_observer_adapter_.get());
    auto pcc_pair =
        std::pair<std::string, std::shared_ptr<P2PPeerConnectionChannel>>(
            target_id, pcc);
    pc_channels_.insert(pcc_pair);
    return pcc;
  } else {
    return pcc_it->second;
  }
}
PeerConnectionChannelConfiguration
P2PClient::GetPeerConnectionChannelConfiguration() {
  PeerConnectionChannelConfiguration config;
  std::vector<webrtc::PeerConnectionInterface::IceServer> ice_servers;
  for (auto it = configuration_.ice_servers.begin();
       it != configuration_.ice_servers.end(); ++it) {
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.urls = (*it).urls;
    ice_server.username = (*it).username;
    ice_server.password = (*it).password;
    ice_servers.push_back(ice_server);
  }
  config.servers = ice_servers;
  config.candidate_network_policy =
      (configuration_.candidate_network_policy ==
       ClientConfiguration::CandidateNetworkPolicy::kLowCost)
          ? webrtc::PeerConnectionInterface::CandidateNetworkPolicy::
                kCandidateNetworkPolicyLowCost
          : webrtc::PeerConnectionInterface::CandidateNetworkPolicy::
                kCandidateNetworkPolicyAll;
  for (auto codec : configuration_.video_encodings) {
    config.video.push_back(VideoEncodingParameters(codec));
  }
  for (auto codec : configuration_.audio_encodings) {
    config.audio.push_back(AudioEncodingParameters(codec));
  }
  // TODO(jianlin): For publisher, peerconnection is created before UA info is
  // received. so signaling protocol change is needed if we would like to remove
  // this HC.
  config.continual_gathering_policy = webrtc::PeerConnectionInterface::
      ContinualGatheringPolicy::GATHER_CONTINUALLY;
  return config;
}
void P2PClient::OnMessageReceived(const std::string& remote_id,
                                  const std::string& message) {
  EventTrigger::OnEvent2(observers_, event_queue_,
                         &P2PClientObserver::OnMessageReceived, remote_id,
                         message);
}
void P2PClient::OnStopped(const std::string& remote_id) {
  // invoked on signaling thread. move to other thread.
  std::thread([this, remote_id]() {
    {
      const std::lock_guard<std::mutex> lock(removed_pc_channels_mutex_);
      removed_pc_channels_.push_back(pc_channels_[remote_id]);
    }
    {
      const std::lock_guard<std::mutex> lock(pc_channels_mutex_);
      pc_channels_.erase(remote_id);
    }
  }).detach();
#ifdef OWT_CLOUD_GAMING
  EventTrigger::OnEvent1(observers_, event_queue_,
                         &P2PClientObserver::OnPeerConnectionClosed, remote_id);
#endif
}
void P2PClient::OnStreamAdded(std::shared_ptr<RemoteStream> stream) {
  EventTrigger::OnEvent1(
      observers_, event_queue_,
      (void (P2PClientObserver::*)(std::shared_ptr<RemoteStream>))(
          &P2PClientObserver::OnStreamAdded),
      stream);
}
}  // namespace p2p
}  // namespace owt
