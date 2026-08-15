// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <array>
#include <span>

#include "knx/physical/ip_secure_tunneling_physical.hpp"
#include "knx/util/result.hpp"

#if KNX_SECURE_ENABLED

#include "knx/datalink/frame_codec.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/netip_config.hpp"

#include <cstring>

#include "knx/util/operation_progress.hpp"

namespace knx {
namespace physical {

IpSecureTunnelingPhysical::IpSecureTunnelingPhysical()
    : host_(IpAddress::fromOctets(127, 0, 0, 1))
    , port_(NetIpPort(netip::config::kDefaultPort))
{
    options_.host = host_;
    options_.port = port_;
    options_.userId = UserId(1);
    options_.initialSeq = 1;
}

IpSecureTunnelingPhysical::~IpSecureTunnelingPhysical() { close(); }

void IpSecureTunnelingPhysical::drainInboundOnce(uint32_t timeoutMs)
{
    if (!initialized_) {
        return;
    }

    (void)client_.poll(static_cast<int>(timeoutMs));
}

void IpSecureTunnelingPhysical::setNetworkInterface(platform::NetworkInterface* network)
{
    if (initialized_) return;
    network_ = network;
}

void IpSecureTunnelingPhysical::setTimingPlatform(platform::TimingPlatform* timingPlatform)
{
    if (initialized_) return;
    timingPlatform_ = timingPlatform;
    client_.setTimingPlatform(timingPlatform);
}

void IpSecureTunnelingPhysical::setGateway(IpAddress host, NetIpPort port)
{
    if (initialized_) return;
    host_ = host;
    port_ = port;
    options_.host = host_;
    options_.port = port_;
}

void IpSecureTunnelingPhysical::setCredentials(UserId userId,
                                              std::span<const uint8_t> passwordLatin1,
                                              const std::array<uint8_t, 32>& clientPrivateKey,
                                              const std::array<uint8_t, 6>& clientSerial,
                                              uint64_t initialSeq)
{
    if (initialized_) return;
    options_.userId = userId;
    options_.passwordLatin1.assign(passwordLatin1.begin(), passwordLatin1.end());
    options_.clientPrivateKey = clientPrivateKey;
    options_.clientSerial = clientSerial;
    options_.initialSeq = initialSeq;
}

util::Result<void> IpSecureTunnelingPhysical::init()
{
    if (initialized_) return util::Result<void>::ok();
    if (!network_) {
        state_ = PhysicalLayerState::Error;
        return util::ErrorCode::NotInitialized;
    }

    client_.setTimingPlatform(timingPlatform_);

    auto openRes = client_.beginOpen(*network_, options_);
    if (openRes.isError()) {
        state_ = PhysicalLayerState::Error;
        return openRes.error();
    }

    auto openWait = netip::detail::waitForTerminalProgress(timingPlatform_, [this]() {
        return client_.pollOpen();
    });
    if (openWait.isError()) {
        state_ = PhysicalLayerState::Error;
        return openWait.error();
    }
    auto openCompletion = netip::detail::completionResult(openWait.value());
    if (openCompletion.isError()) {
        state_ = PhysicalLayerState::Error;
        return openCompletion.error();
    }

    auto connectRes = client_.beginConnectTunneling(1000);
    if (connectRes.isError()) {
        state_ = PhysicalLayerState::Error;
        client_.close();
        return connectRes.error();
    }

    auto connectWait = netip::detail::waitForTerminalProgress(timingPlatform_, [this]() {
        return client_.pollConnectTunneling();
    });
    if (connectWait.isError()) {
        state_ = PhysicalLayerState::Error;
        client_.close();
        return connectWait.error();
    }
    auto connectCompletion = netip::detail::completionResult(connectWait.value());
    if (connectCompletion.isError()) {
        state_ = PhysicalLayerState::Error;
        client_.close();
        return connectCompletion.error();
    }

    client_.setReceiveCallback(
        [this](std::span<const uint8_t> cemi)
    {
        datalink::LDataFrame frame;
        uint8_t msg = 0;

        if (netip::decodeCemiLData(cemi, frame, msg).isError()) {
            return;
        }

        std::array<uint8_t, 64> buf{};
        auto encodeResult = datalink::FrameCodec::encodeFrame(frame, buf);

        if (!encodeResult.isOk() || encodeResult.value() > buf.size()) {
            return;
        }

        const std::size_t len = encodeResult.value();
        const std::span<const uint8_t> encoded(buf.data(), len);

        {
            std::lock_guard<std::mutex> lock(rxMutex_);
            rxQueue_.emplace_back(encoded.data(),
                                encoded.data() + encoded.size());
        }

        auto cb  = rxCb_;
        auto ctx = rxCtx_;
        if (cb) {
            cb(ctx);
        }
    });
    initialized_ = true;
    state_ = PhysicalLayerState::Idle;

    return util::Result<void>::ok();
}

void IpSecureTunnelingPhysical::close()
{
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

bool IpSecureTunnelingPhysical::isOpen() const
{
    return initialized_ && client_.isOpen() && client_.isTunnelingConnected();
}

util::Result<size_t> IpSecureTunnelingPhysical::sendFrame(std::span<const uint8_t> frame)
{
    auto beginResult = beginTransmit(frame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    auto pollResult = pollTransmit(beginResult.value());
    if (pollResult.isError()) {
        return pollResult.error();
    }

    switch (pollResult.value()) {
        case ProgressState::Success:
            return frame.size();
        case ProgressState::Busy:
            return util::ErrorCode::Busy;
        case ProgressState::Timeout:
            return util::ErrorCode::Timeout;
        default:
            return util::ErrorCode::TransmissionFailed;
    }
}

util::Result<uint32_t> IpSecureTunnelingPhysical::beginTransmit(std::span<const uint8_t> frame)
{
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
    const uint8_t msgCode = decodedFrame.confirmation ? 0x2E : 0x11;
    auto cemiResult = netip::encodeCemiLData(decodedFrame, msgCode, cemi);
    if (cemiResult.isError()) {
        state_ = PhysicalLayerState::Error;
        return util::ErrorCode::EncodeFailed;
    }

    pendingTxCemi_.assign(cemi.begin(), cemi.begin() + cemiResult.value());
    auto sendRes = client_.sendCemi(pendingTxCemi_, true, 500);
    txState_ = sendRes.isOk() ? ProgressState::Success : util::progressStateFromError(sendRes.error());
    state_ = sendRes.isOk() ? PhysicalLayerState::Idle : PhysicalLayerState::Error;
    if (sendRes.isError()) {
        pendingTxCemi_.clear();
        return sendRes.error();
    }

    txActive_ = true;
    return ++txSequence_;
}

util::Result<IpSecureTunnelingPhysical::ProgressState> IpSecureTunnelingPhysical::pollTransmit(uint32_t sequence)
{
    if (!txActive_ || sequence != txSequence_) {
        return util::ErrorCode::OperationNotReady;
    }

    txActive_ = false;
    pendingTxCemi_.clear();
    return txState_;
}

util::Result<void> IpSecureTunnelingPhysical::beginReceive(uint32_t timeoutMs)
{
    if (!initialized_) return util::ErrorCode::NotInitialized;

    rxActive_ = true;
    rxDeadlineMs_ = static_cast<uint64_t>(netip::detail::nowMs(timingPlatform_)) + timeoutMs;
    state_ = PhysicalLayerState::Receiving;
    return util::Result<void>::ok();
}

util::Result<IpSecureTunnelingPhysical::ProgressState> IpSecureTunnelingPhysical::pollReceive()
{
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

util::Result<std::span<const uint8_t>> IpSecureTunnelingPhysical::receivedFrameView()
{
    if (lastReceivedFrame_.empty()) {
        return util::ErrorCode::OperationNotReady;
    }

    return std::span<const uint8_t>(lastReceivedFrame_.data(), lastReceivedFrame_.size());
}

void IpSecureTunnelingPhysical::setReceiveCallback(ReceiveCallback callback, void* context)
{
    rxCb_ = callback;
    rxCtx_ = context;
}

PhysicalLayerState IpSecureTunnelingPhysical::getState() const { return state_; }

util::Result<void> IpSecureTunnelingPhysical::setBusMonitorMode(Toggle /*mode*/)
{
    // Bus monitor not applicable for secure tunneling in this minimal adapter
    return util::ErrorCode::OperationNotSupported;
}

} // namespace physical
} // namespace knx

#endif // KNX_SECURE_ENABLED
