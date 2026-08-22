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
#include "knx/netip/system_broadcast.hpp"
#include "knx/util/result.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <cstring>
#include <tuple>

#if KNX_SECURE_ENABLED
#include "knx/netip/ip_secure/secure_routing_security.hpp"
#endif

namespace knx {
namespace physical {

static const char* TAG = "KNX.IpRouting";

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

void IpRoutingPhysical::enqueueFrame(std::span<const uint8_t> cemi)
{
    datalink::LDataFrame frame;
    uint8_t cemiMessageCode = 0;
    if (netip::decodeCemiLData(cemi, frame, cemiMessageCode).isError()) {
        return;
    }

    // A TP1 standard frame tops out at 23 octets, but an L_Data_Extended one
    // carries up to 263 — and every ETS download that moves a key table or a
    // parameter block is extended. A 64-octet buffer silently dropped those.
    std::array<uint8_t, datalink::LDataFrame::MAX_FRAME_SIZE + 8> buf{};
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

/// How long the socket may stay silent before the group membership is re-issued.
///
/// A blind periodic re-join is the wrong shape: refreshMulticastMembership()
/// leaves the group before joining it again, so on a busy group it would open a
/// window in which inbound telegrams are legitimately not delivered. Silence
/// cannot lose what is not arriving, so triggering on it is free — and silence
/// is also the only symptom the device can actually observe when a switch or an
/// access point has aged its snooping entry out and stopped forwarding the
/// group. A device in that state transmits perfectly and hears nothing, which
/// reads from the outside exactly like a firmware that ignores the bus.
static constexpr uint32_t kMembershipRefreshIdleMs = 60000u;

void IpRoutingPhysical::refreshMembershipIfDeaf()
{
    if (!multicastJoined()) {
        return;
    }

    const uint32_t now = netip::detail::nowMs(timingPlatform_);
    if (!lastInboundValid_) {
        // First pass: start the clock rather than treating "never heard
        // anything" as a 1970-length silence.
        lastInboundMs_ = now;
        lastInboundValid_ = true;
        return;
    }

    if ((now - lastInboundMs_) < kMembershipRefreshIdleMs) {
        return;
    }

    // Reset the clock either way. A refresh that failed is not worth retrying
    // twenty times a second, and one that succeeded has not yet been answered.
    lastInboundMs_ = now;
    if (refreshMulticastMembership().isOk()) {
        membershipRefreshes_.fetch_add(1);
        // Deliberately INFO and deliberately not rate-limited beyond the idle
        // window itself: on a group that carries traffic this never prints, and
        // on one that has stopped being delivered it is the only line anywhere
        // that says so.
        KNX_LOGI(TAG, "No inbound datagram for %u ms - re-issued the routing group membership",
                 static_cast<unsigned>(kMembershipRefreshIdleMs));
    }
}

IpRoutingPhysical::LinkStatistics IpRoutingPhysical::linkStatistics() const
{
    LinkStatistics out;
    out.datagramsReceived = rxDatagrams_.load();
    out.datagramsDropped = rxDropped_.load();
    out.membershipRefreshes = membershipRefreshes_.load();
    if (lastInboundValid_) {
        out.msSinceLastInbound = netip::detail::nowMs(timingPlatform_) - lastInboundMs_;
    }
    return out;
}

void IpRoutingPhysical::drainInboundOnce(uint32_t timeoutMs)
{
    if (!initialized_.load() || !endpoint_ || !endpoint_->isOpen()) {
        return;
    }

    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto recvRes = endpoint_->receive(cemi, timeoutMs);
    if (!recvRes.isOk()) {
        refreshMembershipIfDeaf();
        return;
    }

    // Counted before the kind is examined: a datagram that turned out to be a
    // search request, or one this device could not decode, still proves the
    // group is being delivered here.
    rxDatagrams_.fetch_add(1);
    lastInboundMs_ = netip::detail::nowMs(timingPlatform_);
    lastInboundValid_ = true;

    const auto inbound = recvRes.value();
    // Both domains carry an ordinary cEMI L_Data frame; which service type
    // brought it only decides how it must be sent back out, and the layers
    // above address that from the APDU rather than from the transport.
    if (inbound.kind == netip::RoutingEndpoint::InboundKind::None) {
        return;
    }

    enqueueFrame(std::span<const uint8_t>(cemi).first(inbound.cemiLength));
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

void IpRoutingPhysical::setCoreDiscovery(netip::CoreServerResponder::DescriptionProvider provider,
                                         std::span<const netip::ServiceFamilySupport> serviceFamilies) {
    if (initialized_.load()) return;
    coreResponder_.setDescriptionProvider(std::move(provider));
    if (coreResponder_.setServiceFamilies(serviceFamilies).isError()) {
        return;
    }
    coreDiscoveryEnabled_ = true;
}

void IpRoutingPhysical::setRandomSource(netip::RandomUnitSource source) {
    randomSource_ = std::move(source);
}

#if KNX_SECURE_ENABLED
util::Result<void> IpRoutingPhysical::applySecureRouting(std::span<const uint8_t> groupKey,
                                                         uint64_t initialSeq) {
    using netip::ip_secure::SecureWrapper;
    // SecureWrapper names its key by type rather than by length; AES-128 makes
    // that 16 octets, and the compiler holds us to it.
    constexpr size_t kGroupKeyLen = std::tuple_size_v<SecureWrapper::Key>;

    // ETS clears the backbone key by writing zeros; treat an absent or all-zero
    // key as "plain multicast" rather than as a key that happens to be zero.
    const bool clearing = groupKey.empty() ||
        std::all_of(groupKey.begin(), groupKey.end(), [](uint8_t b) { return b == 0; });

    if (!clearing && groupKey.size() != kGroupKeyLen) {
        return util::ErrorCode::InvalidParameter;
    }

    std::lock_guard<std::mutex> lock(secureRoutingMutex_);

    if (clearing) {
        secureRoutingEnabled_ = false;
        secureRoutingGroupKey_.fill(0);
        if (endpoint_) {
            endpoint_->setSecurity(nullptr);
        }
        if (routingSecurity_) {
            retiredRoutingSecurity_.push_back(std::move(routingSecurity_));
        }
        return util::Result<void>::ok();
    }

    SecureWrapper::Key key{};
    std::copy(groupKey.begin(), groupKey.end(), key.begin());

    secureRoutingEnabled_ = true;
    secureRoutingGroupKey_ = key;
    secureRoutingInitialSeq_ = initialSeq;

    // Before init() there is no endpoint to inform; the stored settings are
    // picked up when it opens.
    if (!endpoint_) {
        return util::Result<void>::ok();
    }

    auto security = std::make_unique<netip::ip_secure::SecureRoutingSecurity>(
        key, secureRoutingSerial_, secureRoutingTag_, initialSeq);
    endpoint_->setSecurity(security.get());
    if (routingSecurity_) {
        retiredRoutingSecurity_.push_back(std::move(routingSecurity_));
    }
    routingSecurity_ = std::move(security);
    return util::Result<void>::ok();
}

bool IpRoutingPhysical::isSecureRoutingActive() const {
    std::lock_guard<std::mutex> lock(secureRoutingMutex_);
    return secureRoutingEnabled_;
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

    if (coreDiscoveryEnabled_) {
        // The control endpoint a search response advertises is this socket:
        // the device answers on the address it was found at, which is also the
        // only one it knows to be reachable.
        coreResponder_.setControlEndpoint(endpoint_->localAddress(), port_);
        endpoint_->setControlDatagramHandler(
            [this](std::span<const uint8_t> datagram, IpAddress source, uint16_t sourcePort) {
                // Re-read rather than trust what open() saw. The endpoint may
                // have opened before DHCP handed out a lease, and the address
                // changes again on every renewal into a different subnet — and
                // this HPAI is what ETS connects back to. A stale one lists the
                // device and then fails to reach it.
                if (network_) {
                    const IpAddress current = network_->ipAddress();
                    if (!current.isZero()) {
                        coreResponder_.setControlEndpoint(current, port_);
                    }
                }
                netip::CoreServerResponder::Reply reply;
                if (!coreResponder_.handle(datagram, source, sourcePort, reply)) return;
                if (reply.datagram.empty()) return;
                (void)endpoint_->sendDatagram(reply.address, reply.port, reply.datagram);
            });
    }

    initialized_.store(true);
    state_.store(PhysicalLayerState::Idle);

    return util::Result<void>::ok();
}

util::Result<void> IpRoutingPhysical::refreshMulticastMembership() {
    if (!initialized_.load() || !endpoint_) {
        return util::ErrorCode::NotInitialized;
    }
    return endpoint_->refreshMulticastMembership();
}

bool IpRoutingPhysical::multicastJoined() const {
    return initialized_.load() && endpoint_ && endpoint_->multicastJoined();
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
#if KNX_SECURE_ENABLED
    {
        std::lock_guard<std::mutex> lock(secureRoutingMutex_);
        retiredRoutingSecurity_.clear();
    }
#endif
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

    const auto encodedCemi = std::span<const uint8_t>(cemi).first(cemiResult.value());
    auto sendRes = netip::isSystemBroadcast(decodedFrame)
        ? endpoint_->sendRoutingSystemBroadcast(encodedCemi)
        : endpoint_->sendRoutingIndication(encodedCemi);
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
