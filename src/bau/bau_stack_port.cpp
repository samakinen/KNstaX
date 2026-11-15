// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau_stack_port.cpp
 * @brief Lower-stack composition: BusAccessStackPort.
 *
 * Everything here assembles the physical/data-link/network/transport/
 * application chain and presents it to the BAU through one narrow class.
 * It has no knowledge of group objects or device lifecycle — the BAU only ever
 * reaches it through BusAccessStackPort — which is why it is a separate
 * translation unit rather than 1000 lines in the middle of bau.cpp.
 */

#include "knx/bau/stack_port.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#if KNX_ENABLE_TEST_HOOKS
#include "knx/testing/mock_tp1_physical.hpp"
#endif
#include "knx/network/network_layer.hpp"
#include "knx/security/data_secure.hpp"
#include "knx/security/secure_application_layer.hpp"
#include "knx/network/two_port_coupler.hpp"

#include <algorithm>
#include <optional>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace knx {
namespace bau {

namespace {

constexpr const char* TAG = "KNX.BAU.Port";

application::SendOptions toApplicationTransmissionOptions(const BusAccessUnit::TransmissionOptions& options)
{
    application::SendOptions converted;
    converted.maxAttempts = options.maxAttempts;
    converted.retryOnBusy = options.retryOnBusy;
    converted.retryOnTimeout = options.retryOnTimeout;
    converted.retryOnTransmissionFailed = options.retryOnTransmissionFailed;
    return converted;
}

BusAccessUnit::MessageKind toBauMessageKind(application::APCIService service)
{
    switch (service) {
        case application::APCIService::GroupValueWrite:
            return BusAccessUnit::MessageKind::GroupValueWrite;
        case application::APCIService::GroupValueRead:
            return BusAccessUnit::MessageKind::GroupValueRead;
        case application::APCIService::GroupValueResponse:
            return BusAccessUnit::MessageKind::GroupValueResponse;
        default:
            return BusAccessUnit::MessageKind::Unknown;
    }
}

BusAccessUnit::TransmissionOutcome toBauTransmissionOutcome(const application::ASendOutcome& outcome)
{
    BusAccessUnit::TransmissionOutcome converted;
    converted.destination = outcome.destination;
    converted.kind = toBauMessageKind(outcome.service);
    converted.destinationType = outcome.destinationType;
    converted.result = outcome.result;
    converted.attempts = outcome.attempts;
    return converted;
}

#if KNX_SECURE_ENABLED
/// Translate a transport frame into the identity the S-AL binds into CCM.
security::SecureFrameInfo secureFrameInfo(const transport::TDataFrame& frame)
{
    security::SecureFrameInfo info;
    info.source = frame.source;
    info.destination = frame.destination.raw;
    info.destinationType = frame.destinationType;
    info.tpci6 = frame.securityTpci6;
    info.connected = frame.service == transport::TDataService::Connected;
    return info;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// Medium: one physical layer and the data link layer over it
// ---------------------------------------------------------------------------

class BusAccessStackPort::Medium final {
public:
    template <typename PhysicalT>
    Medium(platform::Platform& platform, std::unique_ptr<PhysicalT> physicalLayer)
        : _platform(platform)
    {
        assignPhysical(std::move(physicalLayer));
    }

#if KNX_FEATURE_TP1
    Medium(platform::Platform& platform, std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
        : _platform(platform)
        , _macPhysical(std::make_unique<physical::Tp1MacPhysical>(std::move(mediumBackend)))
    {
        // The MAC measures its own timeouts. Without the platform it falls
        // back to the host's system clock, which a bare-metal target lacks.
        _macPhysical->setTimingPlatform(&_platform);
    }
#endif

    util::Result<void> init(const IndividualAddress& ownAddress,
                            std::span<const GroupAddress> groupAddresses)
    {
        if (!_dataLink) {
            if (false) {
                // Placeholder so the chain below reads the same whichever
                // media this build contains.
#if KNX_FEATURE_TP1
            } else if (_macPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_macPhysical);
#endif
#if KNX_IP_TUNNELING
            } else if (_ipTunnelingPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_ipTunnelingPhysical);
#endif
#if KNX_SECURE_IP_TUNNELING
            } else if (_ipSecureTunnelingPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_ipSecureTunnelingPhysical);
#endif
#if KNX_FEATURE_NETIP
            } else if (_ipRoutingPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_ipRoutingPhysical);
#endif  // KNX_FEATURE_NETIP
#if KNX_ENABLE_TEST_HOOKS
            } else if (_testPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_testPhysical);
#endif
            } else {
                return util::Result<void>::err(util::ErrorCode::InvalidParameter);
            }
        }

        auto dlInit = _dataLink->init(ownAddress);
        if (!dlInit) {
            return dlInit;
        }
        // Asked for before init(), when there was no data link to tell.
        if (_promiscuousMode.has_value()) {
            _dataLink->setPromiscuousMode(*_promiscuousMode);
        }

        for (const auto& address : groupAddresses) {
            auto addResult = _dataLink->addGroupAddress(address);
            if (addResult.isError()) {
                return addResult;
            }
        }

        return util::Result<void>::ok();
    }

    void close() {
        if (_dataLink) {
            _dataLink->close();
        }
    }

    util::Result<void> sendDataLinkFrame(const datalink::LDataFrame& frame) {
        return _dataLink ? _dataLink->sendFrame(frame)
                         : util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    void processBackgroundWork() {
        if (_dataLink) {
            (void)_dataLink->processRxAvailable(0);
        }
    }

    void setDataLinkPromiscuousMode(datalink::PromiscuousMode mode) {
        _promiscuousMode = mode;
        if (_dataLink) {
            _dataLink->setPromiscuousMode(mode);
        }
    }

    util::Result<void> addGroupAddress(const GroupAddress& address) {
        return _dataLink ? _dataLink->addGroupAddress(address)
                         : util::Result<void>::ok();
    }

    datalink::Tp1DataLinkLayer* dataLinkLayer() { return _dataLink.get(); }
    const datalink::Tp1DataLinkLayer* dataLinkLayer() const { return _dataLink.get(); }

#if KNX_FEATURE_NETIP
    /// The routing physical, when this medium was built from one. Null for
    /// every other medium, which is how the caller tells them apart.
    physical::IpRoutingPhysical* ipRoutingPhysical() const { return _ipRoutingPhysical.get(); }
#endif

private:
    template <typename PhysicalT>
    void assignPhysical(std::unique_ptr<PhysicalT> physicalLayer)
    {
        using ConcretePhysical = std::remove_cvref_t<PhysicalT>;

        if constexpr (false) {
#if KNX_FEATURE_TP1
        } else if constexpr (std::is_same_v<ConcretePhysical, physical::Tp1MacPhysical>) {
            _macPhysical = std::move(physicalLayer);
#endif
#if KNX_IP_TUNNELING
        } else if constexpr (std::is_same_v<ConcretePhysical, physical::IpTunnelingPhysical>) {
            _ipTunnelingPhysical = std::move(physicalLayer);
#endif
#if KNX_SECURE_IP_TUNNELING
        } else if constexpr (std::is_same_v<ConcretePhysical, physical::IpSecureTunnelingPhysical>) {
            _ipSecureTunnelingPhysical = std::move(physicalLayer);
#endif
#if KNX_FEATURE_NETIP
        } else if constexpr (std::is_same_v<ConcretePhysical, physical::IpRoutingPhysical>) {
            _ipRoutingPhysical = std::move(physicalLayer);
#endif  // KNX_FEATURE_NETIP
#if KNX_ENABLE_TEST_HOOKS
        } else if constexpr (std::is_same_v<ConcretePhysical, testing::MockTp1Physical>) {
            _testPhysical = std::move(physicalLayer);
#endif
        } else {
            static_assert(!sizeof(ConcretePhysical*), "Unsupported TP1 stack physical type");
        }
    }

    platform::Platform& _platform;
#if KNX_FEATURE_TP1
    std::unique_ptr<physical::Tp1MacPhysical> _macPhysical;
#endif
#if KNX_IP_TUNNELING
    std::unique_ptr<physical::IpTunnelingPhysical> _ipTunnelingPhysical;
#endif
#if KNX_SECURE_IP_TUNNELING
    std::unique_ptr<physical::IpSecureTunnelingPhysical> _ipSecureTunnelingPhysical;
#endif
#if KNX_FEATURE_NETIP
    std::unique_ptr<physical::IpRoutingPhysical> _ipRoutingPhysical;
#endif
#if KNX_ENABLE_TEST_HOOKS
    std::unique_ptr<testing::MockTp1Physical> _testPhysical;
#endif
    std::unique_ptr<datalink::Tp1DataLinkLayer> _dataLink;
    std::optional<datalink::PromiscuousMode> _promiscuousMode;
};

// ---------------------------------------------------------------------------
// Construction and lifecycle
// ---------------------------------------------------------------------------

BusAccessStackPort::BusAccessStackPort(platform::Platform& platform,
                                       std::unique_ptr<Medium> primary,
                                       std::unique_ptr<Medium> secondary)
    : _platform(platform)
    , _medium(std::move(primary))
    // A coupler's second medium is built now but only initialised in init(),
    // once the individual address is known — the coupler's role depends on it.
    , _secondaryMedium(std::move(secondary))
{
}

BusAccessStackPort::~BusAccessStackPort() = default;

util::Result<void> BusAccessStackPort::initApplicationStack(datalink::Tp1DataLinkLayer& dataLink,
                                                            const IndividualAddress& ownAddress)
{
    if (!_network) {
        _network = std::make_unique<network::NetworkLayer>(dataLink);
    }
    auto netInit = _network->init(ownAddress);
    if (!netInit) {
        return netInit;
    }

    if (!_transport) {
        _transport = std::make_unique<transport::TransportLayer>(*_network);
    }
    // Gives the connection-oriented timers a real timebase, so the owner
    // loop's background-work pass retransmits unacknowledged data and
    // reaps stale connections.
    _transport->setClock([clockPtr = static_cast<platform::TimingPlatform*>(&_platform)]() {
        return clockPtr->millis();
    });
    auto tpInit = _transport->init(ownAddress);
    if (!tpInit) {
        return tpInit;
    }

    if (!_application) {
        _application = std::make_unique<application::ApplicationLayer>(*_transport);
    }
#if KNX_SECURE_ENABLED
    _ownAddress = ownAddress;
    if (_secureAl) {
        _secureAl->setOwnAddress(ownAddress);
    }
#endif
    _application->setProgrammingModeEnabled(_programmingModeEnabled);
    _application->setDefaultSendOptions(toApplicationTransmissionOptions(_defaultTransmissionOptions));
    return _application->init(ownAddress);
}

/**
 * Bring up the second port and the routing engine, for a stack port built
 * as a coupler. No-op otherwise.
 *
 * Order matters: this runs *after* the application stack, because
 * TwoPortCoupler::init() takes over both data link layers' receive
 * callbacks — including the one NetworkLayer installed a moment ago. The
 * local-delivery callback hands the displaced frames back, so the device's
 * own stack keeps working while the coupler routes.
 */
util::Result<void> BusAccessStackPort::initCoupler(const IndividualAddress& ownAddress,
                                                   datalink::Tp1DataLinkLayer& primaryDataLink)
{
    if (!_secondaryMedium) {
        return util::Result<void>::ok();
    }

    // The secondary port answers to the same individual address and the
    // same group addresses: a coupler is one device with two attachments,
    // not two devices.
    auto secondaryInit = _secondaryMedium->init(ownAddress, _groupAddresses);
    if (secondaryInit.isError()) {
        return secondaryInit;
    }

    auto* secondaryDataLink = _secondaryMedium->dataLinkLayer();
    if (!secondaryDataLink) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    if (!_coupler) {
        _coupler = std::make_unique<network::TwoPortCoupler>(primaryDataLink, *secondaryDataLink);
    }

    auto* networkLayer = _network.get();
    if (!networkLayer) {
        return util::Result<void>::err(util::ErrorCode::NotInitialized);
    }
    _coupler->setLocalDeliveryCallback(
        [networkLayer](network::CouplerPort, const datalink::LDataFrame& frame) {
            networkLayer->deliverLocalFrame(frame);
        });

    auto addressResult = _coupler->setOwnAddress(ownAddress);
    if (addressResult.isError()) {
        return addressResult;
    }
    return _coupler->init();
}

util::Result<void> BusAccessStackPort::init(const IndividualAddress& ownAddress)
{
    auto mediumInit = _medium->init(ownAddress, _groupAddresses);
    if (mediumInit.isError()) {
        return mediumInit;
    }

    auto* dataLink = _medium->dataLinkLayer();
    if (!dataLink) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    auto appInit = initApplicationStack(*dataLink, ownAddress);
    if (appInit.isError()) {
        return appInit;
    }

#if KNX_SECURE_ENABLED
    configureSecurity();
#endif
    if (_inboundCallback) {
        installInboundCallback();
    }
    if (_workAvailableCallback) {
        _application->setReceiveWorkAvailableCallback(_workAvailableCallback);
    }

    auto couplerInit = initCoupler(ownAddress, *dataLink);
    if (couplerInit.isError()) {
        return couplerInit;
    }

    _initialized = true;
    return util::Result<void>::ok();
}

void BusAccessStackPort::close()
{
    _initialized = false;
    if (_coupler) {
        _coupler->close();
    }
    if (_application) {
        _application->close();
    }
    if (_transport) {
        _transport->close();
    }
    if (_network) {
        _network->close();
    }
    if (_secondaryMedium) {
        _secondaryMedium->close();
    }
    _medium->close();
}

void BusAccessStackPort::executePendingRestart()
{
    if (_application) {
        _application->restartService().executePendingRestart();
    }
}

void BusAccessStackPort::setRestartHandler(application::RestartService::RestartCallback handler)
{
    if (_application) {
        _application->restartService().setRestartCallback(std::move(handler));
    }
}

void BusAccessStackPort::setManagementServerHooks(application::ApplicationLayer::ManagementServerHooks hooks)
{
    if (_application) {
        _application->setManagementServerHooks(std::move(hooks));
    }
}

util::Result<void> BusAccessStackPort::setOwnAddress(const IndividualAddress& ownAddress)
{
    auto* dataLink = _medium->dataLinkLayer();
    if (!dataLink) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    dataLink->setOwnAddress(ownAddress);
    if (!_transport || !_application) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    auto transportResult = _transport->setOwnAddress(ownAddress);
    if (transportResult.isError()) {
        return transportResult.error();
    }
    _application->setOwnAddress(ownAddress);
#if KNX_SECURE_ENABLED
    _ownAddress = ownAddress;
    if (_secureAl) {
        _secureAl->setOwnAddress(ownAddress);
    }
#endif

    // A coupler's role and every point-to-point routing decision are
    // derived from its own address, so an ETS address assignment has to
    // reach the routing policy too — otherwise the device would answer to
    // its new address while still routing for the old one.
    if (_secondaryMedium) {
        if (auto* secondary = _secondaryMedium->dataLinkLayer()) {
            secondary->setOwnAddress(ownAddress);
        }
        if (_coupler) {
            auto couplerRes = _coupler->setOwnAddress(ownAddress);
            if (couplerRes.isError()) {
                return couplerRes;
            }
        }
    }
    return util::Result<void>::ok();
}

void BusAccessStackPort::setIndividualAddressUpdateCallback(
    application::ApplicationLayer::IndividualAddressUpdateCallback callback)
{
    if (_application) {
        _application->setIndividualAddressUpdateCallback(std::move(callback));
    }
}

// ---------------------------------------------------------------------------
// Medium access
// ---------------------------------------------------------------------------

void BusAccessStackPort::processBackgroundWork()
{
    _medium->processBackgroundWork();
    // The second port needs pumping on the same cadence, or traffic
    // arriving on it would only be seen when something else happened to
    // wake the stack.
    if (_secondaryMedium) {
        _secondaryMedium->processBackgroundWork();
    }
    if (_application) {
        _application->processBackgroundWork();
    }
}

util::Result<void> BusAccessStackPort::sendDataLinkFrame(const datalink::LDataFrame& frame)
{
    auto primaryResult = _medium->sendDataLinkFrame(frame);

    // A coupler is reachable from both subnetworks, so its own telegrams
    // have to leave by both. Sending only on the primary would mean a
    // device on the sub line could address the coupler but never get an
    // answer. This is the coupler's own traffic only — forwarded frames go
    // out through TwoPortCoupler, which picks the correct single port.
    if (_secondaryMedium) {
        auto secondaryResult = _secondaryMedium->sendDataLinkFrame(frame);
        if (primaryResult.isError()) {
            return secondaryResult;
        }
    }
    return primaryResult;
}

void BusAccessStackPort::setDataLinkPromiscuousMode(datalink::PromiscuousMode mode)
{
    _medium->setDataLinkPromiscuousMode(mode);
}

util::Result<void> BusAccessStackPort::addGroupAddress(const GroupAddress& address)
{
    const auto existing = std::find(_groupAddresses.begin(), _groupAddresses.end(), address);
    if (existing == _groupAddresses.end()) {
        if (!_groupAddresses.push_back(address)) {
            return util::ErrorCode::BufferTooSmall;
        }
    }

    if (!_initialized) {
        return util::Result<void>::ok();
    }

    auto primaryResult = _medium->addGroupAddress(address);
    if (primaryResult.isError()) {
        return primaryResult;
    }

    // Both ports must acknowledge this device's group addresses; a coupler
    // subscribes as one device however many attachments it has.
    if (_secondaryMedium) {
        return _secondaryMedium->addGroupAddress(address);
    }
    return primaryResult;
}

util::Result<void> BusAccessStackPort::refreshMulticastMembership()
{
#if KNX_FEATURE_NETIP
    if (auto* routing = _medium->ipRoutingPhysical()) {
        return routing->refreshMulticastMembership();
    }
#endif
    return util::ErrorCode::OperationNotSupported;
}

LinkDiagnostics BusAccessStackPort::linkDiagnostics() const
{
    LinkDiagnostics out;
    if (const auto* dataLink = _medium->dataLinkLayer()) {
        const auto stats = dataLink->getStatistics();
        out.rxFrames = stats.rxFrames;
        out.rxFilterDropped = stats.filterDropped;
        out.rxPoolDropped = stats.rxDropped;
        out.rxDecodeFailed = stats.decodeFailed;
    }

#if KNX_FEATURE_NETIP
    // Only a routing port keeps these; every other medium leaves them zero,
    // which is the honest answer rather than a fabricated one.
    if (const auto* routing = _medium->ipRoutingPhysical()) {
        const auto link = routing->linkStatistics();
        out.mediumDatagramsReceived = link.datagramsReceived;
        out.mediumDatagramsDropped = link.datagramsDropped;
        out.multicastRefreshes = link.membershipRefreshes;
        out.msSinceLastInbound = link.msSinceLastInbound;
    }
#endif
    return out;
}

// ---------------------------------------------------------------------------
// Security and device configuration
// ---------------------------------------------------------------------------

#if KNX_SECURE_ENABLED
void BusAccessStackPort::configureSecurity()
{
    auto* securityObject = _securityObject;
    if (!_transport || !securityObject) {
        return;
    }

    if (!_secureAl) {
        auto& platform = _platform;
        _secureAl = std::make_unique<security::SecureApplicationLayer>(
            *securityObject,
            [&platform]() { return platform.millis(); },
            [&platform](std::span<uint8_t> out) { platform.randomBytes(out); });
    }
    _secureAl->setOwnAddress(_ownAddress);
    _secureAl->setSerialNumber(_serialNumber);

    // The Access Policies have a Security Mode Off and a Security Mode On
    // column (03/4/1 Table 3); this is what tells the application layer
    // which one it is in. Installed here because this is the one place
    // that holds the Security Interface Object.
    if (_application) {
        _application->setSecurityModeProvider([securityObject]() {
            return securityObject->isSecurityEnabled();
        });
    }

    // The S-AL builds the S-A_Sync_Response itself and needs a way out that
    // does not go back through the TX transform (the APDU is already
    // secured). Sending it straight to the transport keeps the response on
    // the communication mode the request arrived on, as §5.3.2 requires.
    auto* transport = _transport.get();
    _secureAl->setTpciResolver([transport](const security::SecureFrameInfo& info) -> uint8_t {
        if (!info.connected || info.destinationType != AddressType::Individual) {
            return 0;
        }
        return transport->connectedTxTpci6(IndividualAddress(info.destination));
    });
    _secureAl->setFrameSink([transport](const security::SecureFrameInfo& info,
                                        std::span<const uint8_t> tpdu) -> util::Result<void> {
        transport::TDataFrame frame;
        frame.service = info.connected ? transport::TDataService::Connected
                      : info.destinationType == AddressType::Individual
                          ? transport::TDataService::Individual
                          : transport::TDataService::Group;
        frame.source = info.source;
        frame.destination = GroupAddress(info.destination);
        frame.destinationType = info.destinationType;
        frame.priority = Priority::System;
        // Every TX path in the stack clears this, and it is not decoration:
        // Tp1MacPhysical opens an L_ACK window whenever CTRL bit 1 is set,
        // and a (system) broadcast is never acknowledged on TP1. Leaving the
        // TDataFrame default of true made the MAC wait for an ACK nobody
        // sends, repeat the response three times and report the send failed.
        frame.ackRequested = false;
        frame.securityTpci6 = info.tpci6;
        frame.tpdu.assign(tpdu);
        return transport->sendFrame(frame);
    });

    _transport->setTxTransform([this](transport::TDataFrame& frame) -> util::Result<void> {
        if (!_secureAl || frame.tpdu.size() < 2) {
            return util::Result<void>::ok();
        }

        std::array<uint8_t, security::DataSecureSession::kMaxSecureTpduSize> secureTpdu{};
        auto result = _secureAl->processOutgoing(secureFrameInfo(frame), frame.tpdu, secureTpdu);
        if (result.isError()) {
            return result.error();
        }
        if (result.value() != 0) {
            // Refuse the send rather than let a frame that should have been
            // secured go out in plain: assign() leaves the TPDU untouched
            // when the secured form no longer fits.
            if (!frame.tpdu.assign(std::span<const uint8_t>(secureTpdu.data(), result.value()))) {
                KNX_LOGE(TAG, "Secured APDU (%zu octets) exceeds the TPDU buffer", result.value());
                return util::ErrorCode::BufferTooSmall;
            }
        }
        return util::Result<void>::ok();
    });

    _transport->setRxTransform([this](transport::TDataFrame& frame) -> util::Result<void> {
        if (!_secureAl || frame.tpdu.size() < 2) {
            return util::Result<void>::ok();
        }

        std::array<uint8_t, security::DataSecureSession::kMaxPlainApduSize> plainTpdu{};
        const auto result = _secureAl->processIncoming(secureFrameInfo(frame), frame.tpdu, plainTpdu);
        switch (result.disposition) {
            case security::SecureRxDisposition::Plain:
                // Written rather than left at the default: the field only
                // ever grants permissions, so the one place that knows a
                // frame arrived unprotected says so outright.
                frame.security = RequestSecurity{};
                return util::Result<void>::ok();
            case security::SecureRxDisposition::Unwrapped:
                frame.security = result.security;
                if (!frame.tpdu.assign(std::span<const uint8_t>(plainTpdu.data(), result.plainLength))) {
                    // Dropping is the only safe outcome: leaving the frame
                    // alone would deliver the still-encrypted APDU upwards.
                    KNX_LOGE(TAG, "Unwrapped APDU (%zu octets) exceeds the TPDU buffer",
                             result.plainLength);
                    return util::ErrorCode::BufferTooSmall;
                }
                return util::Result<void>::ok();
            case security::SecureRxDisposition::Consumed:
                return util::ErrorCode::FrameConsumed;
            case security::SecureRxDisposition::Rejected:
                break;
        }
        return util::ErrorCode::OperationNotSupported;
    });
}
#endif

void BusAccessStackPort::configureDataSecure(objects::SecurityInterfaceObject& securityObject)
{
#if KNX_SECURE_ENABLED
    _securityObject = &securityObject;
    configureSecurity();
#else
    (void)securityObject;
#endif
}

void BusAccessStackPort::setFunctionPropertyHandler(application::FunctionPropertyHandler handler)
{
    if (_application) {
        _application->functionPropertyServices().setHandler(std::move(handler));
    }
}

void BusAccessStackPort::setExtendedFunctionPropertyProvider(
    application::PropertyExtServices::FunctionProvider provider)
{
    if (_application) {
        _application->propertyExtServices().setFunctionProvider(std::move(provider));
    }
}

void BusAccessStackPort::setCommissioningSerialNumber(const application::KnxSerialNumber& serialNumber)
{
    if (_application) {
        _application->networkParameterService().setSerialNumber(serialNumber);
    }
#if KNX_SECURE_ENABLED
    _serialNumber = serialNumber;
    if (_secureAl) {
        _secureAl->setSerialNumber(serialNumber);
    }
#endif
}

void BusAccessStackPort::setDomainAddress(std::span<const uint8_t> domainAddress)
{
    if (_application) {
        _application->networkParameterService().setDomainAddress(domainAddress);
    }
}

void BusAccessStackPort::setDomainAddressWriteCallback(application::DomainAddressWriteCallback callback)
{
    if (_application) {
        _application->networkParameterService().setDomainAddressWriteCallback(std::move(callback));
    }
}

void BusAccessStackPort::setAuthorizationKeys(const application::AuthorizationService::KeyTable& keys)
{
    if (_application) {
        _application->authorizationService().restoreKeyTable(keys);
    }
}

void BusAccessStackPort::setDeviceMedium(MediumType medium)
{
    if (!_application) {
        return;
    }

    // Only the medium nibble of the mask version changes: the firmware type
    // and version identify the stack, not the wire it sits on. The same
    // table numbers the medium in the exported catalogue entry, which is
    // what ETS compares this descriptor against.
    const uint16_t mask = application::systemBMaskVersion(medium);
    auto descriptor = application::DeviceDescriptor::createDefault();
    descriptor.type0.maskHigh = static_cast<uint8_t>(mask >> 8);
    descriptor.type0.maskLow = static_cast<uint8_t>(mask & 0xFFu);
    descriptor.type2.mediumType = medium;
    _application->deviceDescriptorService().setDescriptor(descriptor);
}

// ---------------------------------------------------------------------------
// Inbound delivery
// ---------------------------------------------------------------------------

void BusAccessStackPort::installInboundCallback()
{
    _application->setReceiveCallback(
        [callback = _inboundCallback](const IndividualAddress& source,
                                      const GroupAddress& destination,
                                      application::APCIService service,
                                      std::span<const uint8_t> data,
                                      AddressType destinationType) {
            callback(source, destination, toBauMessageKind(service), data, destinationType);
        });
}

void BusAccessStackPort::setInboundCallback(InboundCallback callback)
{
    _inboundCallback = std::move(callback);
    if (_inboundCallback && _application) {
        installInboundCallback();
    }
}

void BusAccessStackPort::setWorkAvailableCallback(WorkAvailableCallback callback)
{
    _workAvailableCallback = std::move(callback);
    if (_application) {
        _application->setReceiveWorkAvailableCallback(_workAvailableCallback);
    }
}

// ---------------------------------------------------------------------------
// Property and memory access
// ---------------------------------------------------------------------------

void BusAccessStackPort::registerPropertyObject(InterfaceObjectType objectType,
                                                InterfaceObjectIndex objectIndex)
{
    if (_application) {
        _application->registerPropertyObject(objectType, objectIndex);
    }
}

void BusAccessStackPort::setPropertyReadProvider(PropertyReadProvider provider)
{
    if (_application) {
        _application->setPropertyReadProvider(std::move(provider));
    }
}

void BusAccessStackPort::setPropertyWriteProvider(PropertyWriteProvider provider)
{
    if (_application) {
        _application->setPropertyWriteProvider(std::move(provider));
    }
}

void BusAccessStackPort::setPropertyDescriptionProvider(PropertyDescriptionProvider provider)
{
    if (_application) {
        _application->setPropertyDescriptionProvider(std::move(provider));
    }
}

util::Result<void> BusAccessStackPort::registerMemoryRegion(const application::MemoryRegion& region,
                                                            std::span<uint8_t> storage,
                                                            application::MemoryWriteGuard writeGuard)
{
    return _application ? _application->registerMemoryRegion(region, storage, std::move(writeGuard))
                        : util::Result<void>::err(util::ErrorCode::NotInitialized);
}

bool BusAccessStackPort::memoryRegionWritten(MemoryAddress regionStart) const
{
    return _application ? _application->memoryRegionWritten(regionStart) : false;
}

// ---------------------------------------------------------------------------
// Group transmission
// ---------------------------------------------------------------------------

util::Result<void> BusAccessStackPort::sendGroupValueWrite(const GroupAddress& address,
                                                           std::span<const uint8_t> data)
{
    return _application ? _application->sendGroupValueWrite(address, data)
                        : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

util::Result<void> BusAccessStackPort::beginSendGroupValueWrite(const GroupAddress& address,
                                                                std::span<const uint8_t> data)
{
    return _application ? _application->beginSendGroupValueWrite(address, data)
                        : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

util::Result<void> BusAccessStackPort::sendGroupValueRead(const GroupAddress& address)
{
    return _application ? _application->sendGroupValueRead(address)
                        : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

util::Result<void> BusAccessStackPort::beginSendGroupValueRead(const GroupAddress& address)
{
    return _application ? _application->beginSendGroupValueRead(address)
                        : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

util::Result<void> BusAccessStackPort::sendGroupValueResponse(const GroupAddress& address,
                                                              std::span<const uint8_t> data)
{
    return _application ? _application->sendGroupValueResponse(address, data)
                        : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

util::Result<void> BusAccessStackPort::beginSendGroupValueResponse(const GroupAddress& address,
                                                                   std::span<const uint8_t> data)
{
    return _application ? _application->beginSendGroupValueResponse(address, data)
                        : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

util::Result<BusAccessStackPort::TransmissionProgressState> BusAccessStackPort::pollTransmissionProgress()
{
    if (!_application) {
        return util::ErrorCode::InvalidParameter;
    }
    return _application->pollSendData();
}

bool BusAccessStackPort::transmissionInProgress() const
{
    return _application && _application->transmissionInProgress();
}

void BusAccessStackPort::setDefaultTransmissionOptions(const TransmissionOptions& options)
{
    _defaultTransmissionOptions = options;
    if (_application) {
        _application->setDefaultSendOptions(toApplicationTransmissionOptions(options));
    }
}

bool BusAccessStackPort::popTransmissionOutcome(TransmissionOutcome& outcome)
{
    if (!_application) {
        return false;
    }

    application::ASendOutcome applicationOutcome;
    if (!_application->popSendOutcome(applicationOutcome)) {
        return false;
    }

    outcome = toBauTransmissionOutcome(applicationOutcome);
    return true;
}

size_t BusAccessStackPort::queuedTransmissionOutcomeCount() const
{
    return _application ? _application->queuedSendOutcomeCount() : 0U;
}

void BusAccessStackPort::setProgrammingModeEnabled(bool enabled)
{
    _programmingModeEnabled = enabled;
    if (_application) {
        _application->setProgrammingModeEnabled(enabled);
    }
}

bool BusAccessStackPort::programmingModeEnabled() const
{
    return _application ? _application->programmingModeEnabled() : _programmingModeEnabled;
}

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

namespace {

template <typename PhysicalT>
std::unique_ptr<BusAccessStackPort> makeStackPort(platform::Platform& platform,
                                                  std::unique_ptr<PhysicalT> physicalLayer)
{
    return std::make_unique<BusAccessStackPort>(
        platform,
        std::make_unique<BusAccessStackPort::Medium>(platform, std::move(physicalLayer)));
}

} // namespace

#if KNX_FEATURE_TP1
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::Tp1MacPhysical> physicalLayer)
{
    return makeStackPort(platform, std::move(physicalLayer));
}

std::unique_ptr<BusAccessStackPort> createTp1CouplerStackPort(
    platform::Platform& platform,
    std::unique_ptr<physical::Tp1MacPhysical> primaryPhysical,
    std::unique_ptr<physical::Tp1MacPhysical> secondaryPhysical)
{
    return std::make_unique<BusAccessStackPort>(
        platform,
        std::make_unique<BusAccessStackPort::Medium>(platform, std::move(primaryPhysical)),
        std::make_unique<BusAccessStackPort::Medium>(platform, std::move(secondaryPhysical)));
}

std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
{
    return makeStackPort(platform, std::move(mediumBackend));
}
#endif  // KNX_FEATURE_TP1

#if KNX_IP_TUNNELING
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpTunnelingPhysical> physicalLayer)
{
    return makeStackPort(platform, std::move(physicalLayer));
}
#endif

#if KNX_SECURE_IP_TUNNELING
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpSecureTunnelingPhysical> physicalLayer)
{
    return makeStackPort(platform, std::move(physicalLayer));
}
#endif

#if KNX_FEATURE_NETIP
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpRoutingPhysical> physicalLayer)
{
    return makeStackPort(platform, std::move(physicalLayer));
}
#endif  // KNX_FEATURE_NETIP

#if KNX_ENABLE_TEST_HOOKS
std::unique_ptr<BusAccessStackPort> detail::createTp1MockTestStackPort(
    platform::Platform& platform,
    std::unique_ptr<testing::MockTp1Physical> physicalLayer)
{
    return makeStackPort(platform, std::move(physicalLayer));
}
#endif

} // namespace bau
} // namespace knx
