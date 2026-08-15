// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/control_packet_codec.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <utility>

namespace knx {
namespace netip {

class TunnelingAckTracker {
public:
    void record(uint8_t sequence)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sequences_.push_back(sequence);
        while (sequences_.size() > 16) sequences_.pop_front();
    }

    bool consume(uint8_t expectedSequence)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sequences_.begin(); it != sequences_.end(); ++it) {
            if (*it == expectedSequence) {
                sequences_.erase(it);
                return true;
            }
        }
        return false;
    }

private:
    std::mutex mutex_{};
    std::deque<uint8_t> sequences_{};
};

class TunnelingOrchestration {
public:
    static constexpr bool isActivityServiceType(NetIpServiceType serviceType)
    {
        return serviceType == control_packet::kServiceTunnelingAck ||
               serviceType == control_packet::kServiceTunnelingRequest ||
               serviceType == control_packet::kServiceConnectionStateResponse ||
               serviceType == control_packet::kServiceDisconnectResponse ||
               serviceType == control_packet::kServiceConnectionResponse;
    }

    static void resetSession(ChannelId& channelId, TunnelingSequence& sequence)
    {
        channelId = ChannelId::invalid();
        sequence = TunnelingSequence(0);
    }

    static void clearActivity(uint32_t& lastActivityTimeMs)
    {
        lastActivityTimeMs = 0;
    }

    template <typename NowFn>
    static void markActivity(uint32_t& lastActivityTimeMs, NowFn&& nowMs)
    {
        lastActivityTimeMs = nowMs();
    }

    template <typename NowFn>
    static void markConnected(ChannelId& channelId,
                              TunnelingSequence& sequence,
                              uint32_t& lastActivityTimeMs,
                              ChannelId connectedChannelId,
                              NowFn&& nowMs)
    {
        channelId = connectedChannelId;
        sequence = TunnelingSequence(0);
        markActivity(lastActivityTimeMs, std::forward<NowFn>(nowMs));
    }

    template <typename SendFn, typename ReceiveFn>
    static util::Result<control_packet::ChannelStatus> exchangeChannelStatus(NetIpServiceType expectedServiceType,
                                                                             SendFn&& sendRequest,
                                                                             ReceiveFn&& receiveResponse)
    {
        auto sendResult = sendRequest();
        if (sendResult.isError()) return sendResult.error();

        auto receiveResult = receiveResponse();
        if (receiveResult.isError()) return receiveResult.error();

        return control_packet::Codec::decodeChannelStatusResponse(receiveResult.value(), expectedServiceType);
    }

    template <typename SendFn, typename ReceiveFn>
    static util::Result<ChannelId> establishChannel(SendFn&& sendRequest, ReceiveFn&& receiveResponse)
    {
        auto channelStatusResult = exchangeChannelStatus(control_packet::kServiceConnectionResponse,
                                                         std::forward<SendFn>(sendRequest),
                                                         std::forward<ReceiveFn>(receiveResponse));
        if (channelStatusResult.isError()) return channelStatusResult.error();

        const auto channelStatus = channelStatusResult.value();
        if (channelStatus.status != 0x00 || channelStatus.channelId == 0x00) {
            return util::ErrorCode::OperationFailed;
        }

        return ChannelId(channelStatus.channelId);
    }

    template <typename SendFn, typename ReceiveFn>
    static util::Result<void> verifyChannelStatus(NetIpServiceType expectedServiceType,
                                                  ChannelId expectedChannelId,
                                                  SendFn&& sendRequest,
                                                  ReceiveFn&& receiveResponse)
    {
        auto channelStatusResult = exchangeChannelStatus(expectedServiceType,
                                                         std::forward<SendFn>(sendRequest),
                                                         std::forward<ReceiveFn>(receiveResponse));
        if (channelStatusResult.isError()) return channelStatusResult.error();

        const auto channelStatus = channelStatusResult.value();
        if (channelStatus.status != 0x00 || channelStatus.channelId != expectedChannelId.value()) {
            return util::ErrorCode::OperationFailed;
        }

        return util::Result<void>::ok();
    }

    template <typename ReceiveFn, typename NowFn>
    static util::Result<void> waitForAck(TunnelingAckTracker& ackTracker,
                                         uint8_t expectedSequence,
                                         int timeoutMs,
                                         ReceiveFn&& receiveFn,
                                         NowFn&& nowMs)
    {
        if (ackTracker.consume(expectedSequence)) return util::Result<void>::ok();

        const uint32_t startMs = nowMs();
        while (true) {
            const uint32_t elapsedMs = nowMs() - startMs;
            if (elapsedMs >= static_cast<uint32_t>(timeoutMs)) return util::ErrorCode::Timeout;

            const int remainingMs = static_cast<int>(static_cast<uint32_t>(timeoutMs) - elapsedMs);
            (void)receiveFn(remainingMs);
            if (ackTracker.consume(expectedSequence)) return util::Result<void>::ok();
        }
    }

    template <typename SendAckFn, typename OnReceiveFn>
    static util::Result<bool> handleInboundFrame(std::span<const uint8_t> frame,
                                                 NetIpServiceType serviceType,
                                                 ChannelId channelId,
                                                 TunnelingAckTracker& ackTracker,
                                                 SendAckFn&& sendAck,
                                                 OnReceiveFn&& onReceive)
    {
        if (serviceType == control_packet::kServiceTunnelingAck) {
            auto ackResult = control_packet::Codec::decodeTunnelingAck(frame);
            if (ackResult.isOk() &&
                ackResult.value().channelId == channelId.value() &&
                ackResult.value().status == 0x00) {
                ackTracker.record(ackResult.value().sequence);
            }
            return true;
        }

        if (serviceType == control_packet::kServiceTunnelingRequest) {
            auto requestResult = control_packet::Codec::decodeTunnelingRequest(frame);
            if (requestResult.isError()) return true;

            const auto request = requestResult.value();
            if (request.channelId != channelId.value()) return true;

            auto ackResult = sendAck(request.sequence);
            if (ackResult.isError()) return ackResult.error();

            onReceive(request.cemi);
            return true;
        }

        if (serviceType == control_packet::kServiceConnectionStateResponse ||
            serviceType == control_packet::kServiceDisconnectResponse ||
            serviceType == control_packet::kServiceConnectionResponse) {
            return true;
        }

        return true;
    }
};

} // namespace netip
} // namespace knx