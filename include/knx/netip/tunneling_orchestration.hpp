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
#include <optional>
#include <span>
#include <utility>

namespace knx {
namespace netip {

/// TUNNELLING_ACKs received for this client's own TUNNELLING_REQUESTs, keyed
/// by sequence counter, with their status.
class TunnelingAckTracker {
public:
    void record(uint8_t sequence, uint8_t status = control_packet::kStatusNoError)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        acks_.push_back({sequence, status});
        while (acks_.size() > 16) acks_.pop_front();
    }

    /// The status of an ACK for @p expectedSequence, consuming it, if one has
    /// arrived.
    std::optional<uint8_t> take(uint8_t expectedSequence)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = acks_.begin(); it != acks_.end(); ++it) {
            if (it->sequence == expectedSequence) {
                const uint8_t status = it->status;
                acks_.erase(it);
                return status;
            }
        }
        return std::nullopt;
    }

    /// True when a successful ACK for @p expectedSequence has arrived.
    bool consume(uint8_t expectedSequence)
    {
        const auto status = take(expectedSequence);
        return status.has_value() && *status == control_packet::kStatusNoError;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        acks_.clear();
    }

private:
    struct Ack {
        uint8_t sequence;
        uint8_t status;
    };
    std::mutex mutex_{};
    std::deque<Ack> acks_{};
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

    /// Wait for the TUNNELLING_ACK of @p expectedSequence. Ok on an ACK with
    /// E_NO_ERROR, OperationFailed on an ACK that "signals any kind of error
    /// condition" (03/08/04 §2.6.1), Timeout when none arrives.
    template <typename ReceiveFn, typename NowFn>
    static util::Result<void> waitForAck(TunnelingAckTracker& ackTracker,
                                         uint8_t expectedSequence,
                                         int timeoutMs,
                                         ReceiveFn&& receiveFn,
                                         NowFn&& nowMs)
    {
        const auto verdict = [&]() -> std::optional<util::Result<void>> {
            const auto status = ackTracker.take(expectedSequence);
            if (!status.has_value()) return std::nullopt;
            if (*status == control_packet::kStatusNoError) return util::Result<void>::ok();
            return util::Result<void>(util::ErrorCode::OperationFailed);
        };
        if (auto done = verdict()) return *done;

        const uint32_t startMs = nowMs();
        while (true) {
            const uint32_t elapsedMs = nowMs() - startMs;
            if (elapsedMs >= static_cast<uint32_t>(timeoutMs)) return util::ErrorCode::Timeout;

            const int remainingMs = static_cast<int>(static_cast<uint32_t>(timeoutMs) - elapsedMs);
            (void)receiveFn(remainingMs);
            if (auto done = verdict()) return *done;
        }
    }

    /**
     * @brief Handle one inbound frame of an open tunnelling connection.
     *
     * @param expectedRxSequence The sequence counter expected on the next
     *        TUNNELLING_REQUEST from the server, for a connection over UDP;
     *        null for one inside a TCP connection, where "the sequence counter
     *        shall not be evaluated by the receiver", "no TUNNELLING_ACK shall
     *        be sent" and "Received TUNNELLING_ACK Frames shall be ignored"
     *        (03/08/04 §2.6.2).
     *
     * Over UDP (03/08/04 §2.6.1), a TUNNELLING_REQUEST with the expected
     * sequence counter is acknowledged and processed; one with the counter one
     * less is a repetition, acknowledged and discarded; any other is discarded
     * without a reply.
     */
    template <typename SendAckFn, typename OnReceiveFn>
    static util::Result<bool> handleInboundFrame(std::span<const uint8_t> frame,
                                                 NetIpServiceType serviceType,
                                                 ChannelId channelId,
                                                 TunnelingAckTracker& ackTracker,
                                                 TunnelingSequence* expectedRxSequence,
                                                 SendAckFn&& sendAck,
                                                 OnReceiveFn&& onReceive)
    {
        if (serviceType == control_packet::kServiceTunnelingAck) {
            if (expectedRxSequence == nullptr) {
                return true;
            }
            auto ackResult = control_packet::Codec::decodeTunnelingAck(frame);
            if (ackResult.isOk() && ackResult.value().channelId == channelId.value()) {
                ackTracker.record(ackResult.value().sequence, ackResult.value().status);
            }
            return true;
        }

        if (serviceType == control_packet::kServiceTunnelingRequest) {
            auto requestResult = control_packet::Codec::decodeTunnelingRequest(frame);
            if (requestResult.isError()) return true;

            const auto request = requestResult.value();
            if (request.channelId != channelId.value()) return true;

            if (expectedRxSequence == nullptr) {
                onReceive(request.cemi);
                return true;
            }

            const uint8_t expected = expectedRxSequence->value();
            if (request.sequence == expected) {
                auto ackResult = sendAck(request.sequence);
                if (ackResult.isError()) return ackResult.error();
                *expectedRxSequence = TunnelingSequence(static_cast<uint8_t>(expected + 1u));
                onReceive(request.cemi);
                return true;
            }
            if (request.sequence == static_cast<uint8_t>(expected - 1u)) {
                // A repetition of the frame already processed: our ACK was
                // lost. Acknowledge again, do not deliver twice.
                auto ackResult = sendAck(request.sequence);
                if (ackResult.isError()) return ackResult.error();
                return true;
            }
            return true;
        }

        return true;
    }
};

} // namespace netip
} // namespace knx
