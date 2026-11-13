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
#include "knx/security/secure_memory.hpp"
#endif

namespace knx {
namespace physical {

static const char* TAG = "KNX.IpRouting";

IpRoutingPhysical::IpRoutingPhysical()
    : network_(nullptr)
    , multicastGroup_(IpAddress::fromOctets(224, 0, 23, 12).raw)
    , port_(NetIpPort(netip::config::kDefaultPort))
    , interfaceAddress_(IpAddress(0))
    , multicastTtl_(1)
    , multicastLoopback_(true)
    , maxRxQueueDepth_(32)
    , rxDropped_(0)
    , endpoint_(nullptr)
    , initialized_(false)
    , rxMutex_()
    , rxCb_(nullptr)
    , rxCtx_(nullptr)
    , state_(PhysicalLayerState::Idle)
{
}

IpRoutingPhysical::~IpRoutingPhysical() {
    close();
#if KNX_SECURE_ENABLED
    security::secureZero(secureRoutingGroupKey_);
#endif
}

void IpRoutingPhysical::enqueueFrame(std::span<const uint8_t> cemi)
{
    static_assert(kRxSlotSize >= datalink::LDataFrame::MAX_FRAME_SIZE + 8,
                  "an rx slot must hold the largest frame FrameCodec encodes");

    datalink::LDataFrame frame;
    uint8_t cemiMessageCode = 0;
    if (netip::decodeCemiLData(cemi, frame, cemiMessageCode).isError()) {
        return;
    }

    // A TP1 standard frame tops out at 23 octets, but an L_Data_Extended one
    // carries up to 263 — and every ETS download that moves a key table or a
    // parameter block is extended. A 64-octet buffer silently dropped those.
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (!rxRing_) {
            return;
        }
        auto& ring = *rxRing_;
        // Full: the oldest frame makes room, as the deque this replaced did.
        // A routing receiver that falls behind should lose stale frames, not
        // the newest state of a group.
        const size_t maxDepth = maxRxQueueDepth_.load();
        while (rxCount_ >= maxDepth) {
            rxHead_ = (rxHead_ + 1) % kRxQueueCapacity;
            --rxCount_;
            rxDropped_.fetch_add(1);
        }
        RxSlot& slot = ring[(rxHead_ + rxCount_) % kRxQueueCapacity];
        auto encodeResult = datalink::FrameCodec::encodeFrame(frame, slot.bytes);
        if (!encodeResult.isOk()) {
            return;
        }
        slot.length = encodeResult.value();
        ++rxCount_;
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

void IpRoutingPhysical::drainInbound(uint32_t timeoutMs)
{
    if (!initialized_.load() || !endpoint_ || !endpoint_->isOpen()) {
        return;
    }

#if KNX_SECURE_ENABLED
    refreshLatencySettings();
#endif

    // Only the first read may wait. Once one datagram is in hand the rest of
    // the socket's contents are already here, so they are taken now rather
    // than a poll cycle at a time.
    uint32_t waitMs = timeoutMs;

    for (size_t drained = 0; drained < kMaxDrainPerPoll; ++drained) {
        std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
        auto recvRes = endpoint_->receive(cemi, waitMs);
        if (!recvRes.isOk()) {
            // Nothing more to take. Only a drain that read nothing at all says
            // anything about the group still being delivered here.
            if (drained == 0) {
                refreshMembershipIfDeaf();
            }
            return;
        }
        waitMs = 0u;

        // Counted before the kind is examined: a datagram that turned out to be
        // a search request, or one this device could not decode, still proves
        // the group is being delivered here.
        rxDatagrams_.fetch_add(1);
        lastInboundMs_ = netip::detail::nowMs(timingPlatform_);
        lastInboundValid_ = true;

        const auto inbound = recvRes.value();
        // Both domains carry an ordinary cEMI L_Data frame; which service type
        // brought it only decides how it must be sent back out, and the layers
        // above address that from the APDU rather than from the transport.
        if (inbound.kind == netip::RoutingEndpoint::InboundKind::None) {
            continue;
        }

        enqueueFrame(std::span<const uint8_t>(cemi).first(inbound.cemiLength));
    }
}

void IpRoutingPhysical::setMulticast(IpAddress multicastGroup,
                                    NetIpPort port,
                                    IpAddress interfaceAddress) {
    if (initialized_.load()) return;
    multicastGroup_.store(multicastGroup.raw);
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
void IpRoutingPhysical::setSecureRoutingSerial(const std::array<uint8_t, 6>& serial) {
    std::lock_guard<std::mutex> lock(secureRoutingMutex_);
    secureRoutingSerial_ = serial;
}

void IpRoutingPhysical::setMcTimerStore(netip::ip_secure::McTimerStore* store) {
    std::lock_guard<std::mutex> lock(secureRoutingMutex_);
    mcTimerStore_ = store;
}

void IpRoutingPhysical::setLatencySettingsSource(LatencySettingsSource source) {
    if (initialized_.load()) return;
    latencySettingsSource_ = std::move(source);
}

void IpRoutingPhysical::refreshLatencySettings() {
    if (!latencySettingsSource_) {
        return;
    }
    // §2.3.1.6.3 and §2.3.1.7.3: a new value must take effect within a second.
    static constexpr uint32_t kLatencyPollIntervalMs = 1000;
    const uint32_t now = netip::detail::nowMs(timingPlatform_);
    if (latencyPolled_ && (now - latencyPolledMs_) < kLatencyPollIntervalMs) {
        return;
    }
    latencyPolled_ = true;
    latencyPolledMs_ = now;

    const LatencySettings settings = latencySettingsSource_();
    std::lock_guard<std::mutex> lock(secureRoutingMutex_);
    if (settings == latencySettings_) {
        return;
    }
    latencySettings_ = settings;
    if (routingSecurity_) {
        routingSecurity_->setLatencyTolerance(settings.latencyToleranceMs, settings.syncLatencyFraction);
    }
}

uint64_t IpRoutingPhysical::secureRoutingTimer() const {
    std::lock_guard<std::mutex> lock(secureRoutingMutex_);
    return routingSecurity_ ? routingSecurity_->mcTimer() : 0u;
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

void IpRoutingPhysical::setRandomSource(netip::RandomBytesSource source) {
    if (initialized_.load()) return;
    randomSource_ = std::move(source);
}

#if KNX_SECURE_ENABLED
void IpRoutingPhysical::installSecureRoutingContext(KeySource source) {
    netip::ip_secure::SecureRoutingOptions options;
    options.timing = timingPlatform_;
    options.random = [this](std::span<uint8_t> out) {
        // Captures `this` rather than a copy of the source: the source is
        // fixed before init() and lives as long as this object.
        if (randomSource_) {
            randomSource_(out);
            return;
        }
        for (auto& byte : out) {
            byte = static_cast<uint8_t>(netip::detail::fallbackRandom64());
        }
    };
    options.store = mcTimerStore_;
    options.startReason = (source == KeySource::Restored)
        ? netip::ip_secure::McTimerSync::StartReason::PowerUp
        : netip::ip_secure::McTimerSync::StartReason::Reconfigured;
    options.latencyToleranceMs = latencySettings_.latencyToleranceMs;
    options.syncLatencyFraction = latencySettings_.syncLatencyFraction;

    auto security = std::make_unique<netip::ip_secure::SecureRoutingSecurity>(
        secureRoutingGroupKey_, secureRoutingSerial_, std::move(options));
    endpoint_->setSecurity(security.get());
    // setSecurity() has returned, so the endpoint is no longer inside the old
    // context and it can go.
    routingSecurity_ = std::move(security);
}

util::Result<void> IpRoutingPhysical::applySecureRouting(std::span<const uint8_t> groupKey,
                                                         KeySource source) {
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
        security::secureZero(secureRoutingGroupKey_);
        if (endpoint_) {
            endpoint_->setSecurity(nullptr);
        }
        routingSecurity_.reset();
        return util::Result<void>::ok();
    }

    // The key in force, written again — ETS repeating a download, or the
    // product restoring at boot what is already applied — is not E11. Building
    // a new context would restart the multicast timer for nothing.
    if (secureRoutingEnabled_ &&
        security::constantTimeEqual(secureRoutingGroupKey_, groupKey) &&
        (!endpoint_ || routingSecurity_)) {
        return util::Result<void>::ok();
    }

    std::copy(groupKey.begin(), groupKey.end(), secureRoutingGroupKey_.begin());
    secureRoutingEnabled_ = true;
    pendingKeySource_ = source;

    // Before init() there is no endpoint to inform; the stored settings are
    // picked up when it opens.
    if (!endpoint_) {
        return util::Result<void>::ok();
    }

    installSecureRoutingContext(source);
    return util::Result<void>::ok();
}

void IpRoutingPhysical::restartMulticastTimerSync() {
    std::lock_guard<std::mutex> lock(secureRoutingMutex_);
    if (routingSecurity_) {
        routingSecurity_->restartSync();
    }
}

bool IpRoutingPhysical::isSecureRoutingActive() const {
    std::lock_guard<std::mutex> lock(secureRoutingMutex_);
    return secureRoutingEnabled_;
}
#endif

void IpRoutingPhysical::setMaxRxQueueDepth(size_t maxFrames) {
    maxRxQueueDepth_.store(std::clamp<size_t>(maxFrames, 1, kRxQueueCapacity));
}

util::Result<void> IpRoutingPhysical::init() {
    if (initialized_.load()) return util::Result<void>::ok();

    if (!network_) {
        state_.store(PhysicalLayerState::Error);
        return util::ErrorCode::NotInitialized;
    }

    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (!rxRing_) {
            rxRing_ = std::make_unique<std::array<RxSlot, kRxQueueCapacity>>();
        }
        rxHead_ = 0;
        rxCount_ = 0;
    }

#if KNX_SECURE_ENABLED
    // Held across the open so a key arriving from the stack task meanwhile
    // either lands before the context is built here or after it is installed.
    std::lock_guard<std::mutex> secureLock(secureRoutingMutex_);
#endif
    endpoint_ = std::make_unique<netip::RoutingEndpoint>();
    netip::RoutingEndpoint::Options opt;
    opt.multicastGroup = IpAddress(multicastGroup_.load());
    opt.port = port_;
    opt.interfaceAddress = interfaceAddress_;
    opt.ttl = multicastTtl_;
    opt.loopback = multicastLoopback_;
    endpoint_->setTimingPlatform(timingPlatform_);
    endpoint_->setRandomSource([this](std::span<uint8_t> out) {
        if (randomSource_) {
            randomSource_(out);
            return;
        }
        for (auto& byte : out) {
            byte = static_cast<uint8_t>(netip::detail::fallbackRandom64());
        }
    });
    auto openRes = endpoint_->open(*network_, opt);
    if (openRes.isError()) {
        state_.store(PhysicalLayerState::Error);
        endpoint_.reset();
        return openRes.error();
    }
#if KNX_SECURE_ENABLED
    if (secureRoutingEnabled_) {
        installSecureRoutingContext(pendingKeySource_);
    }
#endif

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

util::Result<void> IpRoutingPhysical::setMulticastGroup(IpAddress multicastGroup) {
    if (multicastGroup.isZero()) {
        return util::ErrorCode::InvalidAddress;
    }
    multicastGroup_.store(multicastGroup.raw);
    if (!initialized_.load() || !endpoint_) {
        // Picked up by init().
        return util::Result<void>::ok();
    }
    return endpoint_->setMulticastGroup(multicastGroup);
}

IpAddress IpRoutingPhysical::multicastGroup() const {
    return IpAddress(multicastGroup_.load());
}

bool IpRoutingPhysical::multicastJoined() const {
    return initialized_.load() && endpoint_ && endpoint_->multicastJoined();
}

void IpRoutingPhysical::close() {
    if (!initialized_.load()) return;
    {
#if KNX_SECURE_ENABLED
        std::lock_guard<std::mutex> secureLock(secureRoutingMutex_);
#endif
        if (endpoint_) {
            endpoint_->close();
            endpoint_.reset();
        }
#if KNX_SECURE_ENABLED
        routingSecurity_.reset();
        // A reopen is a restart of the synchronisation from the stored timer.
        pendingKeySource_ = KeySource::Restored;
#endif
    }
    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        rxRing_.reset();
        rxHead_ = 0;
        rxCount_ = 0;
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

    // Spend the caller's deadline inside the socket rather than returning
    // Pending and being asked again. beginReceive() set that deadline and it
    // was ignored here, which is what left the layer above with nothing to do
    // but poll. A zero or already-expired deadline still reads without waiting,
    // so the drain that follows a first hit stays non-blocking.
    const uint64_t now = static_cast<uint64_t>(netip::detail::nowMs(timingPlatform_));
    const uint32_t waitMs =
        (rxDeadlineMs_ > now) ? static_cast<uint32_t>(rxDeadlineMs_ - now) : 0u;
    drainInbound(waitMs);

    {
        std::lock_guard<std::mutex> lock(rxMutex_);
        if (rxRing_ && rxCount_ != 0) {
            lastReceivedFrame_ = (*rxRing_)[rxHead_];
            rxHead_ = (rxHead_ + 1) % kRxQueueCapacity;
            --rxCount_;
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
    if (lastReceivedFrame_.length == 0) {
        return util::ErrorCode::OperationNotReady;
    }

    return std::span<const uint8_t>(lastReceivedFrame_.bytes).first(lastReceivedFrame_.length);
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
