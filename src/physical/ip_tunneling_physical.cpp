// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <array>
#include <span>

#include "knx/physical/ip_tunneling_physical.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/util/result.hpp"
#include <cstring>

namespace {

knx::util::Result<size_t> finalizeTransmitResult(
                                                 knx::util::Result<knx::physical::IpTunnelingPhysical::ProgressState> pollResult,
                                                 size_t frameSize)
{
    if (pollResult.isError()) {
        return pollResult.error();
    }

    switch (pollResult.value()) {
        case knx::physical::IpTunnelingPhysical::ProgressState::Success:
            return frameSize;
        case knx::physical::IpTunnelingPhysical::ProgressState::Busy:
            return knx::util::ErrorCode::Busy;
        case knx::physical::IpTunnelingPhysical::ProgressState::Timeout:
            return knx::util::ErrorCode::Timeout;
        default:
            return knx::util::ErrorCode::TransmissionFailed;
    }
}

} // namespace


namespace knx {
namespace physical {

IpTunnelingPhysical::IpTunnelingPhysical()
    : host_(IpAddress::fromOctets(127, 0, 0, 1)), port_(NetIpPort(netip::config::kDefaultPort)) {}

IpTunnelingPhysical::~IpTunnelingPhysical() { close(); }

void IpTunnelingPhysical::drainInboundOnce(uint32_t timeoutMs)
{
    if (!initialized_) {
        return;
    }

    (void)client_.poll(static_cast<int>(timeoutMs));
}

void IpTunnelingPhysical::setGateway(IpAddress host, NetIpPort port) {
    if (initialized_) return;
    host_ = host;
    port_ = port;
}

void IpTunnelingPhysical::setNetworkInterface(platform::NetworkInterface* network) {
    if (initialized_) return;
    network_ = network;
}

void IpTunnelingPhysical::setTimingPlatform(platform::TimingPlatform* timingPlatform) {
    if (initialized_) return;
    timingPlatform_ = timingPlatform;
    client_.setTimingPlatform(timingPlatform);
}

void IpTunnelingPhysical::setNetIpSecurity(netip::NetIpSecurity* security) {
    if (initialized_) return;
    security_ = security;
}

util::Result<void> IpTunnelingPhysical::init() {
    if (initialized_) return util::Result<void>::ok();
    if (!network_) {
        state_ = PhysicalLayerState::Error;
        return util::ErrorCode::NotInitialized;
    }
    if (security_) {
        client_.setNetIpSecurity(security_);
    }
    client_.setTimingPlatform(timingPlatform_);
    // Allow ample time for UDP control-plane handshake with minimal gateways.
    auto openRes = client_.open(*network_, host_, port_, 1000);
    if (openRes.isError()) {
        state_ = PhysicalLayerState::Error;
        return openRes.error();
    }
    client_.setReceiveCallback([this](std::span<const uint8_t> cemi){
        datalink::LDataFrame frame;
        uint8_t msg = 0;
        if (netip::decodeCemiLData(cemi, frame, msg).isError()) {
            return;
        }
        // Re-encode into TP1 byte frame for data link layer
        std::array<uint8_t, 64> buf{};
        auto encodeResult = datalink::FrameCodec::encodeFrame(
            frame,
            buf);
        if (!encodeResult.isOk()) {
            return;
        }
        size_t len = encodeResult.value();
        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rxQueue_.emplace_back(buf.begin(), buf.begin() + len);
        }
        if (rxCb_) rxCb_(rxCtx_);
    });
    initialized_ = true;
    state_ = PhysicalLayerState::Idle;
    return util::Result<void>::ok();
}

void IpTunnelingPhysical::close() {
    if (!initialized_) return;
    client_.close();
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxQueue_.clear();
    }
    lastReceivedFrame_.clear();
    pendingTxCemi_.clear();
    txActive_ = false;
    rxActive_ = false;
    initialized_ = false;
    state_ = PhysicalLayerState::Idle;
}

bool IpTunnelingPhysical::isOpen() const { return initialized_ && client_.isOpen(); }

util::Result<size_t> IpTunnelingPhysical::sendFrame(std::span<const uint8_t> frame) {
    auto beginResult = beginTransmit(frame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    return finalizeTransmitResult(pollTransmit(beginResult.value()), frame.size());
}

util::Result<uint32_t> IpTunnelingPhysical::beginTransmit(std::span<const uint8_t> frame) {
    if (!initialized_) return util::ErrorCode::NotInitialized;
    if (frame.empty()) return util::ErrorCode::InvalidParameter;

    pendingTxCemi_.clear();
    state_ = PhysicalLayerState::Transmitting;

    datalink::LDataFrame decodedFrame;
    auto decodeResult = datalink::FrameCodec::decodeFrame(frame, decodedFrame);
    if (!decodeResult.isOk()) {
        state_ = PhysicalLayerState::Error;
        return util::ErrorCode::DecodeFailed;
    }

    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto cemiResult = netip::encodeCemiLData(decodedFrame, 0x11, cemi);
    if (cemiResult.isError()) {
        state_ = PhysicalLayerState::Error;
        return util::ErrorCode::EncodeFailed;
    }

    pendingTxCemi_.assign(cemi.begin(), cemi.begin() + cemiResult.value());
    auto sendRes = client_.sendCemi(std::span<const uint8_t>(pendingTxCemi_.data(), pendingTxCemi_.size()), true, 500);
    txState_ = sendRes.isOk() ? ProgressState::Success : util::progressStateFromError(sendRes.error());
    state_ = sendRes.isOk() ? PhysicalLayerState::Idle : PhysicalLayerState::Error;
    if (sendRes.isError()) {
        pendingTxCemi_.clear();
        return sendRes.error();
    }

    txActive_ = true;
    return ++txSequence_;
}

util::Result<IpTunnelingPhysical::ProgressState> IpTunnelingPhysical::pollTransmit(uint32_t sequence) {
    if (!txActive_ || sequence != txSequence_) {
        return util::ErrorCode::OperationNotReady;
    }

    txActive_ = false;
    pendingTxCemi_.clear();
    return txState_;
}

util::Result<void> IpTunnelingPhysical::beginReceive(uint32_t timeoutMs) {
    if (!initialized_) return util::ErrorCode::NotInitialized;

    rxActive_ = true;
    rxDeadlineMs_ = static_cast<uint64_t>(netip::detail::nowMs(timingPlatform_)) + timeoutMs;
    state_ = PhysicalLayerState::Receiving;
    return util::Result<void>::ok();
}

util::Result<IpTunnelingPhysical::ProgressState> IpTunnelingPhysical::pollReceive() {
    if (!rxActive_) {
        return util::ErrorCode::OperationNotReady;
    }

    drainInboundOnce(0);

    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (!rxQueue_.empty()) {
            lastReceivedFrame_ = std::move(rxQueue_.front());
            rxQueue_.pop_front();
            rxActive_ = false;
            state_ = PhysicalLayerState::Idle;
            return ProgressState::Success;
        }
    }

    if (static_cast<uint64_t>(netip::detail::nowMs(timingPlatform_)) >= rxDeadlineMs_) {
        rxActive_ = false;
        state_ = PhysicalLayerState::Idle;
        return ProgressState::Timeout;
    }

    return ProgressState::Pending;
}

util::Result<std::span<const uint8_t>> IpTunnelingPhysical::receivedFrameView() {
    if (lastReceivedFrame_.empty()) {
        return util::ErrorCode::OperationNotReady;
    }

    return std::span<const uint8_t>(lastReceivedFrame_);
}

void IpTunnelingPhysical::setReceiveCallback(ReceiveCallback callback, void* context) {
    rxCb_ = callback;
    rxCtx_ = context;
}

PhysicalLayerState IpTunnelingPhysical::getState() const { return state_; }

util::Result<void> IpTunnelingPhysical::setBusMonitorMode(Toggle /*mode*/) {
    // Bus monitor not applicable for tunneling in this minimal adapter
    return util::ErrorCode::OperationNotSupported;
}

} // namespace physical
} // namespace knx
