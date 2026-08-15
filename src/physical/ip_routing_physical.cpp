// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <span>
#include <array>

#include "knx/physical/ip_routing_physical.hpp"

#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/netip/routing_endpoint.hpp"
#include "knx/util/result.hpp"

#include <cstring>

#if KNX_SECURE_ENABLED
#include "knx/netip/ip_secure/secure_routing_security.hpp"
#endif

namespace knx {
namespace physical {

IpRoutingPhysical::IpRoutingPhysical()
    : network_(nullptr)
    , multicastGroup_(IpAddress::fromOctets(224, 0, 23, 12))
    , port_(NetIpPort(netip::config::kDefaultPort))
    , interfaceAddress_(IpAddress(0))
    , multicastTtl_(1)
    , multicastLoopback_(true)
    , maxRxQueueDepth_(32)
    , rxDropped_(0)
    , endpoint_(nullptr)
    , initialized_(false)
    , rxMutex_()
    , rxQueue_()
    , rxCb_(nullptr)
    , rxCtx_(nullptr)
    , state_(PhysicalLayerState::Idle)
{
}

IpRoutingPhysical::~IpRoutingPhysical() {
    close();
}

void IpRoutingPhysical::drainInboundOnce(uint32_t timeoutMs)
{
    if (!initialized_.load() || !endpoint_ || !endpoint_->isOpen()) {
        return;
    }

    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto recvRes = endpoint_->receiveRoutingIndication(cemi, timeoutMs);
    if (!recvRes.isOk()) {
        return;
    }

    datalink::LDataFrame frame;
    uint8_t cemiMessageCode = 0;
    if (netip::decodeCemiLData(std::span<const uint8_t>(cemi).first(recvRes.value()),
                               frame,
                               cemiMessageCode)
            .isError()) {
        return;
    }

    std::array<uint8_t, 64> buf{};
    auto encodeResult = datalink::FrameCodec::encodeFrame(frame, buf);
    if (!encodeResult.isOk()) {
        return;
    }

    const size_t len = encodeResult.value();
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxQueue_.emplace_back(buf.begin(), buf.begin() + len);
        const size_t maxDepth = maxRxQueueDepth_.load();
        while (rxQueue_.size() > maxDepth) {
            rxQueue_.pop_front();
            rxDropped_.fetch_add(1);
        }
    }

    if (rxCb_) {
        rxCb_(rxCtx_);
    }
}

void IpRoutingPhysical::setMulticast(IpAddress multicastGroup,
                                    NetIpPort port,
                                    IpAddress interfaceAddress) {
    if (initialized_.load()) return;
    multicastGroup_ = multicastGroup;
    port_ = port;
    interfaceAddress_ = interfaceAddress;
}

void IpRoutingPhysical::setNetworkInterface(platform::NetworkInterface* network) {
    if (initialized_.load()) return;
    network_ = network;
}

void IpRoutingPhysical::setTimingPlatform(platform::TimingPlatform* timingPlatform) {
    if (initialized_.load()) return;
    timingPlatform_ = timingPlatform;
}

void IpRoutingPhysical::setMulticastSocketOptions(uint8_t ttl, Toggle loopback) {
    if (initialized_.load()) return;
    multicastTtl_ = ttl;
    multicastLoopback_ = isEnabled(loopback);
}

#if KNX_SECURE_ENABLED
void IpRoutingPhysical::setSecureRouting(const std::array<uint8_t, 16>& groupKey,
                                         const std::array<uint8_t, 6>& serial,
                                         const std::array<uint8_t, 2>& tag,
                                         uint64_t initialSeq) {
    if (initialized_.load()) return;
    secureRoutingEnabled_ = true;
    secureRoutingGroupKey_ = groupKey;
    secureRoutingSerial_ = serial;
    secureRoutingTag_ = tag;
    secureRoutingInitialSeq_ = initialSeq;
}
#endif

void IpRoutingPhysical::setMaxRxQueueDepth(size_t maxFrames) {
    if (maxFrames == 0) maxFrames = 1;
    maxRxQueueDepth_.store(maxFrames);
}

util::Result<void> IpRoutingPhysical::init() {
    if (initialized_.load()) return util::Result<void>::ok();

    if (!network_) {
        state_.store(PhysicalLayerState::Error);
        return util::ErrorCode::NotInitialized;
    }

    endpoint_ = std::make_unique<netip::RoutingEndpoint>();
    netip::RoutingEndpoint::Options opt;
    opt.multicastGroup = multicastGroup_;
    opt.port = port_;
    opt.interfaceAddress = interfaceAddress_;
    opt.ttl = multicastTtl_;
    opt.loopback = multicastLoopback_;
    endpoint_->setTimingPlatform(timingPlatform_);
#if KNX_SECURE_ENABLED
    if (secureRoutingEnabled_) {
        routingSecurity_ = std::make_unique<netip::ip_secure::SecureRoutingSecurity>(
            secureRoutingGroupKey_, secureRoutingSerial_, secureRoutingTag_, secureRoutingInitialSeq_);
        opt.security = routingSecurity_.get();
    }
#endif
    auto openRes = endpoint_->open(*network_, opt);
    if (openRes.isError()) {
        state_.store(PhysicalLayerState::Error);
        endpoint_.reset();
        return openRes.error();
    }

    initialized_.store(true);
    state_.store(PhysicalLayerState::Idle);

    return util::Result<void>::ok();
}

void IpRoutingPhysical::close() {
    if (!initialized_.load()) return;
    if (endpoint_) {
        endpoint_->close();
        endpoint_.reset();
    }
#if KNX_SECURE_ENABLED
    routingSecurity_.reset();
#endif
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxQueue_.clear();
    }
    initialized_.store(false);
    state_.store(PhysicalLayerState::Idle);
}

bool IpRoutingPhysical::isOpen() const {
    return initialized_.load() && endpoint_ && endpoint_->isOpen();
}

util::Result<size_t> IpRoutingPhysical::sendFrame(std::span<const uint8_t> frame) {
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

util::Result<uint32_t> IpRoutingPhysical::beginTransmit(std::span<const uint8_t> frame) {
    if (!initialized_.load() || !endpoint_ || !endpoint_->isOpen()) {
        return util::ErrorCode::NotInitialized;
    }
    if (frame.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    state_.store(PhysicalLayerState::Transmitting);
    pendingTxFrame_.assign(frame.begin(), frame.end());

    datalink::LDataFrame decodedFrame;
    auto decodeResult = datalink::FrameCodec::decodeFrame(
        std::span<const uint8_t>(pendingTxFrame_), decodedFrame);
    if (!decodeResult.isOk()) {
        state_.store(PhysicalLayerState::Error);
        return util::ErrorCode::DecodeFailed;
    }

    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto cemiResult = netip::encodeCemiLData(decodedFrame, 0x29, cemi);
    if (!cemiResult.isOk()) {
        state_.store(PhysicalLayerState::Error);
        pendingTxFrame_.clear();
        return util::ErrorCode::EncodeFailed;
    }

    auto sendRes = endpoint_->sendRoutingIndication(
        std::span<const uint8_t>(cemi).first(cemiResult.value()));
    txState_ = sendRes.isOk() ? ProgressState::Success : util::progressStateFromError(sendRes.error());
    state_.store(sendRes.isOk() ? PhysicalLayerState::Idle : PhysicalLayerState::Error);
    if (sendRes.isError()) {
        pendingTxFrame_.clear();
        return sendRes.error();
    }

    txActive_ = true;
    return ++txSequence_;
}

util::Result<IpRoutingPhysical::ProgressState> IpRoutingPhysical::pollTransmit(uint32_t sequence) {
    if (!txActive_ || sequence != txSequence_) {
        return util::ErrorCode::OperationNotReady;
    }

    txActive_ = false;
    pendingTxFrame_.clear();
    return txState_;
}

util::Result<void> IpRoutingPhysical::beginReceive(uint32_t timeoutMs) {
    if (!initialized_.load()) {
        return util::ErrorCode::NotInitialized;
    }

    rxActive_ = true;
    rxDeadlineMs_ = static_cast<uint64_t>(netip::detail::nowMs(timingPlatform_)) + timeoutMs;
    state_.store(PhysicalLayerState::Receiving);
    return util::Result<void>::ok();
}

util::Result<IpRoutingPhysical::ProgressState> IpRoutingPhysical::pollReceive() {
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
            state_.store(PhysicalLayerState::Idle);
            return ProgressState::Success;
        }
    }

    if (static_cast<uint64_t>(netip::detail::nowMs(timingPlatform_)) >= rxDeadlineMs_) {
        rxActive_ = false;
        state_.store(PhysicalLayerState::Idle);
        return ProgressState::Timeout;
    }

    return ProgressState::Pending;
}

util::Result<std::span<const uint8_t>> IpRoutingPhysical::receivedFrameView() {
    if (lastReceivedFrame_.empty()) {
        return util::ErrorCode::OperationNotReady;
    }

    return std::span<const uint8_t>(lastReceivedFrame_);
}

void IpRoutingPhysical::setReceiveCallback(ReceiveCallback callback, void* context) {
    rxCb_ = callback;
    rxCtx_ = context;
}

PhysicalLayerState IpRoutingPhysical::getState() const {
    return state_.load();
}

util::Result<void> IpRoutingPhysical::setBusMonitorMode(Toggle /*mode*/) {
    // Bus monitor not applicable for routing in this minimal adapter
    return util::ErrorCode::OperationNotSupported;
}

} // namespace physical
} // namespace knx
