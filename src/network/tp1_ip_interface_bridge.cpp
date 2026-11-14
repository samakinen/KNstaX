// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/network/tp1_ip_interface_bridge.hpp"

#include "knx/netip/cemi.hpp"
#include "knx/util/log.hpp"

#include <array>
#include <vector>

namespace knx {
namespace network {

static const char* TAG = "KNX.Tp1IpIf";

namespace {

// 03_06_03 §4.1.5.3.3 / §4.1.5.3.4 / §4.1.5.3.5.
constexpr uint8_t kCemiLDataReq = 0x11;
constexpr uint8_t kCemiLDataCon = 0x2E;
constexpr uint8_t kCemiLDataInd = 0x29;

bool isGroupDestination(const datalink::LDataFrame& frame)
{
    return frame.destinationType != AddressType::Individual;
}

} // namespace

Tp1IpInterfaceBridge::Tp1IpInterfaceBridge(datalink::Tp1DataLinkLayer& tp1Line,
                                           netip::TunnelingServerEndpoint& tunnelingServer)
    : tp1Line_(tp1Line)
    , tunnelingServer_(tunnelingServer)
{
}

util::Result<void> Tp1IpInterfaceBridge::init()
{
    if (initialized_.exchange(true)) {
        return util::Result<void>::ok();
    }

    // Every tunnel address is a device on the line: acknowledged on the
    // medium whether a connection is open on it or not (03_08_04 §2.2.2).
    auto addressResult = tp1Line_.setAdditionalIndividualAddresses(tunnelingServer_.tunnelAddresses());
    if (addressResult.isError()) {
        initialized_.store(false);
        return addressResult.error();
    }

    tp1Line_.setPromiscuousMode(datalink::PromiscuousMode::Enable);
    tp1Line_.setReceiveCallback([this](const datalink::LDataFrame& frame) {
        onTp1Frame(frame);
    });

    tunnelingServer_.setReceiveCallback([this](ChannelId channelId, std::span<const uint8_t> cemi) {
        onTunnelingCemi(channelId, cemi);
    });

    return util::Result<void>::ok();
}

void Tp1IpInterfaceBridge::close()
{
    if (!initialized_.exchange(false)) {
        return;
    }

    tp1Line_.setReceiveCallback(nullptr);
    tunnelingServer_.setReceiveCallback({});
    (void)tp1Line_.setAdditionalIndividualAddresses({});
    requestHead_ = 0;
    requestCount_ = 0;
    transmitting_ = false;
}

Tp1IpInterfaceBridge::Statistics Tp1IpInterfaceBridge::statistics() const
{
    return Statistics{
        .tp1ToIpForwarded = tp1ToIpForwarded_.load(),
        .ipToTp1Forwarded = ipToTp1Forwarded_.load(),
        .tp1ToIpDropped = tp1ToIpDropped_.load(),
        .ipToTp1Dropped = ipToTp1Dropped_.load(),
        .positiveConfirmations = positiveConfirmations_.load(),
        .negativeConfirmations = negativeConfirmations_.load(),
    };
}

void Tp1IpInterfaceBridge::poll()
{
    if (!initialized_.load()) {
        return;
    }
    drainIndications();
    pumpTransmit();
}

void Tp1IpInterfaceBridge::onTp1Frame(const datalink::LDataFrame& frame)
{
    const size_t tail = indicationTail_.load(std::memory_order_relaxed);
    const size_t next = (tail + 1u) % kIndicationQueueDepth;
    if (next == indicationHead_.load(std::memory_order_acquire)) {
        tp1ToIpDropped_.fetch_add(1);
        return;
    }
    indications_[tail] = frame;
    indicationTail_.store(next, std::memory_order_release);
}

void Tp1IpInterfaceBridge::drainIndications()
{
    size_t head = indicationHead_.load(std::memory_order_relaxed);
    while (head != indicationTail_.load(std::memory_order_acquire)) {
        indicate(indications_[head], ChannelId::invalid());
        head = (head + 1u) % kIndicationQueueDepth;
        indicationHead_.store(head, std::memory_order_release);
    }
}

void Tp1IpInterfaceBridge::indicate(const datalink::LDataFrame& received, ChannelId exceptChannel)
{
    datalink::LDataFrame frame = received;
    frame.confirmation = false; // C is don't care in L_Data.ind (03_06_03 §4.1.5.3.5)

    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto encodeResult = netip::encodeCemiLData(frame, kCemiLDataInd, cemi);
    if (encodeResult.isError()) {
        tp1ToIpDropped_.fetch_add(1);
        return;
    }
    const auto payload = std::span<const uint8_t>(cemi.data(), encodeResult.value());

    if (!isGroupDestination(frame)) {
        // Point-to-point: only the connection that owns the destination
        // address (03_08_04 §2.2.2).
        const ChannelId target = tunnelingServer_.channelForTunnelAddress(IndividualAddress(frame.destination.raw));
        if (!target.isValid() || target == exceptChannel) {
            return;
        }
        if (tunnelingServer_.sendCemi(target, payload).isOk()) {
            tp1ToIpForwarded_.fetch_add(1);
        } else {
            tp1ToIpDropped_.fetch_add(1);
        }
        return;
    }

    if (!exceptChannel.isValid()) {
        const auto sendResult = tunnelingServer_.sendCemiToAll(payload);
        if (sendResult.isOk()) {
            tp1ToIpForwarded_.fetch_add(sendResult.value());
        } else {
            tp1ToIpDropped_.fetch_add(1);
        }
        return;
    }

    for (const auto& channel : tunnelingServer_.activeChannels()) {
        if (channel.channelId == exceptChannel) {
            continue;
        }
        if (tunnelingServer_.sendCemi(channel.channelId, payload).isOk()) {
            tp1ToIpForwarded_.fetch_add(1);
        } else {
            tp1ToIpDropped_.fetch_add(1);
        }
    }
}

void Tp1IpInterfaceBridge::onTunnelingCemi(ChannelId channelId, std::span<const uint8_t> cemi)
{
    PendingRequest request{};
    request.channelId = channelId;
    uint8_t messageCode = 0;
    auto decodeResult = netip::decodeCemiLData(cemi, request.frame, messageCode);
    if (decodeResult.isError() || messageCode != kCemiLDataReq) {
        // The client sends L_Data.req only (03_08_04 §2.4).
        ipToTp1Dropped_.fetch_add(1);
        KNX_LOGW(TAG, "IP->TP1 channel=%u: not an L_Data.req (code=0x%02X, %d)",
                 static_cast<unsigned>(channelId.value()),
                 static_cast<unsigned>(messageCode),
                 decodeResult.isError() ? static_cast<int>(decodeResult.error()) : 0);
        return;
    }

    // 03_08_04 §2.2.2: Source Address 0000h is replaced by the tunnel
    // address; any other source goes onto the line unchanged.
    if (request.frame.source.raw == 0u) {
        request.frame.source = tunnelingServer_.tunnelAddressOf(channelId);
    }
    request.frame.repeated = false;

    if (requestCount_ >= kRequestQueueDepth) {
        ipToTp1Dropped_.fetch_add(1);
        KNX_LOGW(TAG, "IP->TP1 channel=%u: request queue full",
                 static_cast<unsigned>(channelId.value()));
        sendConfirmation(request, false);
        return;
    }

    requests_[(requestHead_ + requestCount_) % kRequestQueueDepth] = request;
    ++requestCount_;
    pumpTransmit();
}

void Tp1IpInterfaceBridge::pumpTransmit()
{
    while (transmitting_ || requestCount_ > 0u) {
        if (!transmitting_) {
            auto beginResult = tp1Line_.beginTransmit(requests_[requestHead_].frame);
            if (beginResult.isError()) {
                if (beginResult.error() == util::ErrorCode::Busy) {
                    return; // The line is sending something else; next poll.
                }
                KNX_LOGW(TAG, "IP->TP1 send failed: %d", static_cast<int>(beginResult.error()));
                finishRequest(false);
                continue;
            }
            transmitting_ = true;
        }

        auto progress = tp1Line_.pollTransmit();
        if (progress.isError()) {
            transmitting_ = false;
            finishRequest(false);
            continue;
        }
        switch (progress.value()) {
            case datalink::Tp1DataLinkLayer::TxProgressState::Pending:
                return;
            case datalink::Tp1DataLinkLayer::TxProgressState::Success:
                transmitting_ = false;
                finishRequest(true);
                break;
            case datalink::Tp1DataLinkLayer::TxProgressState::Busy:
            case datalink::Tp1DataLinkLayer::TxProgressState::TransmissionFailed:
            case datalink::Tp1DataLinkLayer::TxProgressState::Timeout:
                transmitting_ = false;
                finishRequest(false);
                break;
        }
    }
}

void Tp1IpInterfaceBridge::finishRequest(bool success)
{
    const PendingRequest request = requests_[requestHead_];
    requestHead_ = (requestHead_ + 1u) % kRequestQueueDepth;
    --requestCount_;

    if (success) {
        ipToTp1Forwarded_.fetch_add(1);
    } else {
        ipToTp1Dropped_.fetch_add(1);
    }
    sendConfirmation(request, success);
    if (success) {
        indicate(request.frame, request.channelId);
    }
}

void Tp1IpInterfaceBridge::sendConfirmation(const PendingRequest& request, bool success)
{
    // 03_06_03 §4.1.5.3.4: the requested frame with Confirm flag 0 (no error)
    // or 1 (error); LDataFrame::confirmation carries that bit as is.
    datalink::LDataFrame frame = request.frame;
    frame.confirmation = !success;

    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto encodeResult = netip::encodeCemiLData(frame, kCemiLDataCon, cemi);
    if (encodeResult.isError()) {
        return;
    }
    if (tunnelingServer_.sendCemi(request.channelId,
                                  std::span<const uint8_t>(cemi.data(), encodeResult.value())).isError()) {
        KNX_LOGW(TAG, "L_Data.con to channel=%u not sent",
                 static_cast<unsigned>(request.channelId.value()));
        return;
    }
    (success ? positiveConfirmations_ : negativeConfirmations_).fetch_add(1);
}

} // namespace network
} // namespace knx
