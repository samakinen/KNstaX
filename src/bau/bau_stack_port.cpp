// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau_stack_port.cpp
 * @brief Lower-stack composition behind BusAccessStackPort.
 *
 * Everything here assembles the physical/data-link/network/transport/
 * application chain and presents it to the BAU through one narrow interface.
 * It has no knowledge of group objects or device lifecycle — the BAU only ever
 * reaches it through BusAccessStackPort — which is why it is a separate
 * translation unit rather than 1200 lines in the middle of bau.cpp.
 */

#include "knx/bau/bau.hpp"
#include "bau_internal.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/property_store.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/platform/raii_resources.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/testing/mock_tp1_physical.hpp"
#include "knx/security/data_secure.hpp"
#include "knx/security/secure_application_layer.hpp"
#include "knx/network/two_port_coupler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace knx {
namespace bau {

using StackPort = BusAccessStackPort;

namespace {
constexpr const char* TAG = "KNX.BAU.Port";
}

using detail::PendingInboundGroupEvent;

namespace {

application::SendOptions toApplicationTransmissionOptions(const BusAccessUnit::TransmissionOptions& options)
{
    application::SendOptions converted;
    converted.maxAttempts = options.maxAttempts;
    converted.retryOnBusy = options.retryOnBusy;
    converted.retryOnTimeout = options.retryOnTimeout;
    converted.retryOnTransmissionFailed = options.retryOnTransmissionFailed;
    return converted;
}

BusAccessUnit::TransmissionProgressState toBauTransmissionProgressState(application::ApplicationLayer::SendProgressState state)
{
    switch (state) {
        case application::ApplicationLayer::SendProgressState::Pending:
            return BusAccessUnit::TransmissionProgressState::Pending;
        case application::ApplicationLayer::SendProgressState::Success:
            return BusAccessUnit::TransmissionProgressState::Success;
        case application::ApplicationLayer::SendProgressState::Busy:
            return BusAccessUnit::TransmissionProgressState::Busy;
        case application::ApplicationLayer::SendProgressState::TransmissionFailed:
            return BusAccessUnit::TransmissionProgressState::TransmissionFailed;
        case application::ApplicationLayer::SendProgressState::Timeout:
            return BusAccessUnit::TransmissionProgressState::Timeout;
    }

    return BusAccessUnit::TransmissionProgressState::TransmissionFailed;
}

BusAccessUnit::TransmissionOutcome toBauTransmissionOutcome(const application::ASendOutcome& outcome)
{
    BusAccessUnit::TransmissionOutcome converted;
    converted.destination = outcome.destination;
    switch (outcome.service) {
        case application::APCIService::GroupValueWrite:
            converted.kind = BusAccessUnit::MessageKind::GroupValueWrite;
            break;
        case application::APCIService::GroupValueRead:
            converted.kind = BusAccessUnit::MessageKind::GroupValueRead;
            break;
        case application::APCIService::GroupValueResponse:
            converted.kind = BusAccessUnit::MessageKind::GroupValueResponse;
            break;
        default:
            converted.kind = BusAccessUnit::MessageKind::Unknown;
            break;
    }
    converted.destinationType = outcome.destinationType;
    converted.result = outcome.result;
    converted.attempts = outcome.attempts;
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

class LowerStackLifecycle {
public:
    virtual ~LowerStackLifecycle() = default;

    virtual util::Result<void> init(const IndividualAddress& ownAddress) = 0;
    virtual void close() = 0;
    virtual void executePendingRestart() = 0;
    virtual void setRestartHandler(application::RestartService::RestartCallback handler) = 0;
    virtual util::Result<void> setOwnAddress(const IndividualAddress& ownAddress) = 0;
    virtual void setIndividualAddressUpdateCallback(application::ApplicationLayer::IndividualAddressUpdateCallback callback) = 0;
};

class LowerStackMediumAccess {
public:
    virtual ~LowerStackMediumAccess() = default;

    virtual void processBackgroundWork() = 0;
    virtual util::Result<void> sendDataLinkFrame(const datalink::LDataFrame& frame) = 0;
    virtual void setDataLinkPromiscuousMode(datalink::PromiscuousMode mode) = 0;
};

class LowerStackSecurityConfiguration {
public:
    virtual ~LowerStackSecurityConfiguration() = default;

    virtual void configureDataSecure(objects::SecurityInterfaceObject& securityObject) = 0;
    virtual void setFunctionPropertyHandler(application::FunctionPropertyHandler handler) = 0;
    virtual void setExtendedFunctionPropertyProvider(
        application::PropertyExtServices::FunctionProvider provider) = 0;
    virtual void setCommissioningSerialNumber(const application::KnxSerialNumber& serialNumber) = 0;
};

class LowerStackInboundConfiguration {
public:
    virtual ~LowerStackInboundConfiguration() = default;

    virtual void setInboundCallback(StackPort::InboundCallback callback) = 0;
    virtual void setWorkAvailableCallback(StackPort::WorkAvailableCallback callback) = 0;
};

class LowerStackSubscriptionConfiguration {
public:
    virtual ~LowerStackSubscriptionConfiguration() = default;

    virtual util::Result<void> addGroupAddress(const GroupAddress& address) = 0;
};

class LowerStackPropertyAccess {
public:
    virtual ~LowerStackPropertyAccess() = default;

    virtual void registerPropertyObject(InterfaceObjectType objectType,
                                        InterfaceObjectIndex objectIndex,
                                        StackPort::PropertyRegistration registration) = 0;
    virtual void setPropertyReadProvider(StackPort::PropertyReadProvider provider) = 0;
    virtual void setPropertyWriteProvider(StackPort::PropertyWriteProvider provider) = 0;
    virtual void setPropertyDescriptionProvider(StackPort::PropertyDescriptionProvider provider) = 0;
    virtual util::Result<void> registerMemoryRegion(const application::MemoryRegion& region,
                                                    std::span<uint8_t> storage) = 0;
    virtual bool memoryRegionWritten(MemoryAddress regionStart) const = 0;
};

class LowerStackTransmissionServices {
public:
    virtual ~LowerStackTransmissionServices() = default;

    virtual void processBackgroundWork() = 0;
    virtual util::Result<void> sendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual util::Result<void> beginSendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual util::Result<void> sendGroupValueRead(const GroupAddress& address) = 0;
    virtual util::Result<void> beginSendGroupValueRead(const GroupAddress& address) = 0;
    virtual util::Result<StackPort::TransmissionProgressState> pollTransmissionProgress() = 0;
    virtual bool transmissionInProgress() const = 0;
    virtual void setDefaultTransmissionOptions(const StackPort::TransmissionOptions& options) = 0;
    virtual const StackPort::TransmissionOptions& defaultTransmissionOptions() const = 0;
    virtual bool popTransmissionOutcome(StackPort::TransmissionOutcome& outcome) = 0;
    virtual size_t queuedTransmissionOutcomeCount() const = 0;
    virtual util::Result<void> sendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual util::Result<void> beginSendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual void setProgrammingModeEnabled(bool enabled) = 0;
    virtual bool programmingModeEnabled() const = 0;
};

class Tp1MediumBundle final {
public:
    template <typename PhysicalT>
    Tp1MediumBundle(platform::Platform& platform,
                    std::unique_ptr<PhysicalT> physicalLayer)
        : _platform(platform)
    {
        assignPhysical(std::move(physicalLayer));
    }

    Tp1MediumBundle(platform::Platform& platform,
                    std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
        : _platform(platform)
        , _macPhysical(std::make_unique<physical::Tp1MacPhysical>(std::move(mediumBackend)))
    {
    }

    util::Result<void> init(const IndividualAddress& ownAddress,
                            std::span<const GroupAddress> groupAddresses)
    {
        if (!_dataLink) {
            if (_macPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_macPhysical);
#if KNX_FEATURE_NETIP
            } else if (_ipTunnelingPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_ipTunnelingPhysical);
#if KNX_SECURE_ENABLED
            } else if (_ipSecureTunnelingPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_ipSecureTunnelingPhysical);
#endif
            } else if (_ipRoutingPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_ipRoutingPhysical);
#endif  // KNX_FEATURE_NETIP
            } else if (_testPhysical) {
                _dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(_platform, *_testPhysical);
            } else {
                return util::Result<void>::err(util::ErrorCode::InvalidParameter);
            }
        }

        auto dlInit = _dataLink->init(ownAddress);
        if (!dlInit) {
            return dlInit;
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
        if (_dataLink) {
            _dataLink->setPromiscuousMode(mode);
        }
    }

    util::Result<void> addGroupAddress(const GroupAddress& address) {
        return _dataLink ? _dataLink->addGroupAddress(address)
                         : util::Result<void>::ok();
    }

    datalink::Tp1DataLinkLayer* dataLinkLayer() {
        return _dataLink.get();
    }

private:
    template <typename PhysicalT>
    void assignPhysical(std::unique_ptr<PhysicalT> physicalLayer)
    {
        using ConcretePhysical = std::remove_cvref_t<PhysicalT>;

        if constexpr (std::is_same_v<ConcretePhysical, physical::Tp1MacPhysical>) {
            _macPhysical = std::move(physicalLayer);
#if KNX_FEATURE_NETIP
        } else if constexpr (std::is_same_v<ConcretePhysical, physical::IpTunnelingPhysical>) {
            _ipTunnelingPhysical = std::move(physicalLayer);
#if KNX_SECURE_ENABLED
        } else if constexpr (std::is_same_v<ConcretePhysical, physical::IpSecureTunnelingPhysical>) {
            _ipSecureTunnelingPhysical = std::move(physicalLayer);
#endif
        } else if constexpr (std::is_same_v<ConcretePhysical, physical::IpRoutingPhysical>) {
            _ipRoutingPhysical = std::move(physicalLayer);
#endif  // KNX_FEATURE_NETIP
        } else if constexpr (std::is_same_v<ConcretePhysical, testing::MockTp1Physical>) {
            _testPhysical = std::move(physicalLayer);
        } else {
            static_assert(!sizeof(ConcretePhysical*), "Unsupported TP1 stack physical type");
        }
    }

    platform::Platform& _platform;
    std::unique_ptr<physical::Tp1MacPhysical> _macPhysical;
#if KNX_FEATURE_NETIP
    std::unique_ptr<physical::IpTunnelingPhysical> _ipTunnelingPhysical;
#if KNX_SECURE_ENABLED
    std::unique_ptr<physical::IpSecureTunnelingPhysical> _ipSecureTunnelingPhysical;
#endif
    std::unique_ptr<physical::IpRoutingPhysical> _ipRoutingPhysical;
#endif
    std::unique_ptr<testing::MockTp1Physical> _testPhysical;
    std::unique_ptr<datalink::Tp1DataLinkLayer> _dataLink;
};

class Tp1ApplicationStackBundle final {
public:
    util::Result<void> init(datalink::Tp1DataLinkLayer& dataLink,
                            const IndividualAddress& ownAddress,
                            const StackPort::TransmissionOptions& defaultTransmissionOptions)
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
        auto tpInit = _transport->init(ownAddress);
        if (!tpInit) {
            return tpInit;
        }

        if (!_application) {
            _application = std::make_unique<application::ApplicationLayer>(*_transport);
        }
        _ownAddress = ownAddress;
#if KNX_SECURE_ENABLED
        if (_secureAl) {
            _secureAl->setOwnAddress(ownAddress);
        }
#endif
        _application->setProgrammingModeEnabled(_programmingModeEnabled);
        _application->setDefaultSendOptions(toApplicationTransmissionOptions(defaultTransmissionOptions));
        return _application->init(ownAddress);
    }

    void close() {
        if (_application) {
            _application->close();
        }
        if (_transport) {
            _transport->close();
        }
        if (_network) {
            _network->close();
        }
    }

    void executePendingRestart() {
        if (_application) {
            _application->restartService().executePendingRestart();
        }
    }

    void setRestartHandler(application::RestartService::RestartCallback handler) {
        if (_application) {
            _application->restartService().setRestartCallback(std::move(handler));
        }
    }

    void processBackgroundWork() {
        if (_application) {
            _application->processBackgroundWork();
        }
    }

    void setInboundCallback(StackPort::InboundCallback callback) {
        if (!_application) {
            return;
        }

        _application->setReceiveCallback(
            [callback = std::move(callback)](const IndividualAddress& source,
                                             const GroupAddress& destination,
                                             application::APCIService service,
                                             std::span<const uint8_t> data,
                                             AddressType destinationType) {
                callback(source,
                         destination,
                         toBauMessageKind(service),
                         data,
                         destinationType);
            });
    }

    util::Result<void> setOwnAddress(const IndividualAddress& ownAddress) {
        if (!_transport || !_application) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        auto transportResult = _transport->setOwnAddress(ownAddress);
        if (transportResult.isError()) {
            return transportResult.error();
        }

        _application->setOwnAddress(ownAddress);
        _ownAddress = ownAddress;
#if KNX_SECURE_ENABLED
        if (_secureAl) {
            _secureAl->setOwnAddress(ownAddress);
        }
#endif
        return util::Result<void>::ok();
    }

    void setIndividualAddressUpdateCallback(application::ApplicationLayer::IndividualAddressUpdateCallback callback) {
        if (_application) {
            _application->setIndividualAddressUpdateCallback(std::move(callback));
        }
    }

    void setWorkAvailableCallback(StackPort::WorkAvailableCallback callback) {
        if (_application) {
            _application->setReceiveWorkAvailableCallback(std::move(callback));
        }
    }

    void setProgrammingModeEnabled(bool enabled) {
        _programmingModeEnabled = enabled;
        if (_application) {
            _application->setProgrammingModeEnabled(enabled);
        }
    }

    bool programmingModeEnabled() const {
        return _application ? _application->programmingModeEnabled() : _programmingModeEnabled;
    }

#if KNX_SECURE_ENABLED
    void configureSecurity(objects::SecurityInterfaceObject* securityObject,
                           platform::Platform& platform) {
        if (!_transport || !securityObject) {
            return;
        }

        if (!_secureAl) {
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
#else
    void configureSecurity(objects::SecurityInterfaceObject* /*securityObject*/,
                           const std::shared_ptr<void>& /*sessions*/,
                           platform::Platform& /*platform*/) {
        // Security disabled build: no-op
    }
#endif

    void setFunctionPropertyHandler(application::FunctionPropertyHandler handler) {
        if (_application) {
            _application->functionPropertyServices().setHandler(std::move(handler));
        }
    }

    void setExtendedFunctionPropertyProvider(
        application::PropertyExtServices::FunctionProvider provider) {
        if (_application) {
            _application->propertyExtServices().setFunctionProvider(std::move(provider));
        }
    }

    void setCommissioningSerialNumber(const application::KnxSerialNumber& serialNumber) {
        if (_application) {
            _application->networkParameterService().setSerialNumber(serialNumber);
        }
        _serialNumber = serialNumber;
#if KNX_SECURE_ENABLED
        if (_secureAl) {
            _secureAl->setSerialNumber(serialNumber);
        }
#endif
    }

    void registerPropertyObject(InterfaceObjectType objectType,
                                InterfaceObjectIndex objectIndex,
                                StackPort::PropertyRegistration registration) {
        if (!_application) {
            return;
        }

        _application->registerPropertyObject(objectType, objectIndex, std::move(registration));
    }

    void setPropertyReadProvider(StackPort::PropertyReadProvider provider) {
        if (_application) {
            _application->setPropertyReadProvider(std::move(provider));
        }
    }

    void setPropertyWriteProvider(StackPort::PropertyWriteProvider provider) {
        if (_application) {
            _application->setPropertyWriteProvider(std::move(provider));
        }
    }

    void setPropertyDescriptionProvider(StackPort::PropertyDescriptionProvider provider) {
        if (_application) {
            _application->setPropertyDescriptionProvider(std::move(provider));
        }
    }

    bool memoryRegionWritten(MemoryAddress regionStart) const {
        return _application ? _application->memoryRegionWritten(regionStart) : false;
    }

    util::Result<void> registerMemoryRegion(const application::MemoryRegion& region,
                                            std::span<uint8_t> storage) {
        return _application ? _application->registerMemoryRegion(region, storage)
                            : util::Result<void>::err(util::ErrorCode::NotInitialized);
    }

    util::Result<void> sendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) {
        return _application ? _application->sendGroupValueWrite(address, data)
                            : util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    util::Result<void> beginSendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) {
        return _application ? _application->beginSendGroupValueWrite(address, data)
                            : util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    util::Result<void> sendGroupValueRead(const GroupAddress& address) {
        return _application ? _application->sendGroupValueRead(address)
                            : util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    util::Result<void> beginSendGroupValueRead(const GroupAddress& address) {
        return _application ? _application->beginSendGroupValueRead(address)
                            : util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    util::Result<StackPort::TransmissionProgressState> pollTransmissionProgress() {
        if (!_application) {
            return util::ErrorCode::InvalidParameter;
        }

        const auto progress = _application->pollSendData();
        if (progress.isError()) {
            return progress.error();
        }
        return toBauTransmissionProgressState(progress.value());
    }

    bool transmissionInProgress() const {
        return _application && _application->transmissionInProgress();
    }

    void setDefaultTransmissionOptions(const StackPort::TransmissionOptions& options) {
        if (_application) {
            _application->setDefaultSendOptions(toApplicationTransmissionOptions(options));
        }
    }

    bool popTransmissionOutcome(StackPort::TransmissionOutcome& outcome) {
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

    size_t queuedTransmissionOutcomeCount() const {
        return _application ? _application->queuedSendOutcomeCount() : 0U;
    }

    util::Result<void> sendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) {
        return _application ? _application->sendGroupValueResponse(address, data)
                            : util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    util::Result<void> beginSendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) {
        return _application ? _application->beginSendGroupValueResponse(address, data)
                            : util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

private:
#if KNX_SECURE_ENABLED
    /// Translate a transport frame into the identity the S-AL binds into CCM.
    static security::SecureFrameInfo secureFrameInfo(const transport::TDataFrame& frame) {
        security::SecureFrameInfo info;
        info.source = frame.source;
        info.destination = frame.destination.raw;
        info.destinationType = frame.destinationType;
        info.tpci6 = frame.securityTpci6;
        info.connected = frame.service == transport::TDataService::Connected;
        return info;
    }
#endif

    std::unique_ptr<network::NetworkLayer> _network;
    std::unique_ptr<transport::TransportLayer> _transport;
    std::unique_ptr<application::ApplicationLayer> _application;
#if KNX_SECURE_ENABLED
    std::unique_ptr<security::SecureApplicationLayer> _secureAl;
#endif
    IndividualAddress _ownAddress{};
    application::KnxSerialNumber _serialNumber{};
    bool _programmingModeEnabled{false};

public:
    /// The network layer, so a coupler can hand it locally relevant frames
    /// after taking over the data link layer's receive callback.
    network::NetworkLayer* networkLayer() { return _network.get(); }
};

struct Tp1LifecycleState {
    bool initialized{false};
};

struct Tp1SubscriptionState {
    util::FixedVector<GroupAddress, datalink::Tp1DataLinkLayer::MAX_GROUP_ADDRESSES> groupAddresses;
};

struct Tp1CallbackState {
    StackPort::InboundCallback inboundCallback;
    StackPort::WorkAvailableCallback workAvailableCallback;
};

struct Tp1TransmissionState {
    StackPort::TransmissionOptions defaultTransmissionOptions{};
};

/**
 * @brief The second port and routing engine of a coupler.
 *
 * Present only when the stack port was built as a coupler. The primary medium
 * carries this device's own stack exactly as it does for an end device; this
 * adds the secondary medium and the forwarding decision between the two.
 */
struct Tp1CouplerState {
    std::unique_ptr<Tp1MediumBundle> secondaryMedium;
    std::unique_ptr<network::TwoPortCoupler> coupler;

    bool isCoupler() const { return secondaryMedium != nullptr; }
};

class Tp1LowerStackContext final {
public:
    template <typename PhysicalT>
    Tp1LowerStackContext(platform::Platform& platformRef,
                         std::unique_ptr<PhysicalT> physicalLayer)
        : platform(platformRef)
        , medium(platformRef, std::move(physicalLayer))
    {
    }

    Tp1LowerStackContext(platform::Platform& platformRef,
                         std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
        : platform(platformRef)
        , medium(platformRef, std::move(mediumBackend))
    {
    }

    platform::Platform& platform;
    Tp1MediumBundle medium;
    Tp1CouplerState couplerState;
    Tp1ApplicationStackBundle application;
    Tp1LifecycleState lifecycle;
    Tp1SubscriptionState subscriptions;
    Tp1CallbackState callbacks;
    Tp1TransmissionState transmission;
#if KNX_SECURE_ENABLED
    struct SecurityState {
        objects::SecurityInterfaceObject* securityObject{nullptr};
    } security;
#endif
};

#if KNX_SECURE_ENABLED
void installTp1SecurityTransforms(const std::shared_ptr<Tp1LowerStackContext>& context)
{
    context->application.configureSecurity(context->security.securityObject,
                                           context->platform);
}
#endif

class Tp1LifecycleServices final : public LowerStackLifecycle {
public:
    explicit Tp1LifecycleServices(std::shared_ptr<Tp1LowerStackContext> context)
        : _context(std::move(context))
    {
    }

private:
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
    util::Result<void> initCoupler(const IndividualAddress& ownAddress,
                                   datalink::Tp1DataLinkLayer& primaryDataLink) {
        auto& state = _context->couplerState;
        if (!state.isCoupler()) {
            return util::Result<void>::ok();
        }

        // The secondary port answers to the same individual address and the
        // same group addresses: a coupler is one device with two attachments,
        // not two devices.
        auto secondaryInit = state.secondaryMedium->init(ownAddress,
                                                         _context->subscriptions.groupAddresses);
        if (secondaryInit.isError()) {
            return secondaryInit;
        }

        auto* secondaryDataLink = state.secondaryMedium->dataLinkLayer();
        if (!secondaryDataLink) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        if (!state.coupler) {
            state.coupler = std::make_unique<network::TwoPortCoupler>(primaryDataLink,
                                                                      *secondaryDataLink);
        }

        auto* networkLayer = _context->application.networkLayer();
        if (!networkLayer) {
            return util::Result<void>::err(util::ErrorCode::NotInitialized);
        }
        state.coupler->setLocalDeliveryCallback(
            [networkLayer](network::CouplerPort, const datalink::LDataFrame& frame) {
                networkLayer->deliverLocalFrame(frame);
            });

        auto addressResult = state.coupler->setOwnAddress(ownAddress);
        if (addressResult.isError()) {
            return addressResult;
        }
        return state.coupler->init();
    }

public:

    util::Result<void> init(const IndividualAddress& ownAddress) override {
        auto mediumInit = _context->medium.init(ownAddress,
                                                _context->subscriptions.groupAddresses);
        if (mediumInit.isError()) {
            return mediumInit;
        }

        auto* dataLink = _context->medium.dataLinkLayer();
        if (!dataLink) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        auto appInit = _context->application.init(*dataLink,
                                                  ownAddress,
                                                  _context->transmission.defaultTransmissionOptions);
        if (appInit.isError()) {
            return appInit;
        }

    #if KNX_SECURE_ENABLED
        installTp1SecurityTransforms(_context);
    #endif
        if (_context->callbacks.inboundCallback) {
            _context->application.setInboundCallback(_context->callbacks.inboundCallback);
        }
        if (_context->callbacks.workAvailableCallback) {
            _context->application.setWorkAvailableCallback(_context->callbacks.workAvailableCallback);
        }

        auto couplerInit = initCoupler(ownAddress, *dataLink);
        if (couplerInit.isError()) {
            return couplerInit;
        }

        _context->lifecycle.initialized = true;
        return util::Result<void>::ok();
    }

    void close() override {
        _context->lifecycle.initialized = false;
        if (_context->couplerState.coupler) {
            _context->couplerState.coupler->close();
        }
        _context->application.close();
        if (_context->couplerState.secondaryMedium) {
            _context->couplerState.secondaryMedium->close();
        }
        _context->medium.close();
    }

    void executePendingRestart() override {
        _context->application.executePendingRestart();
    }

    void setRestartHandler(application::RestartService::RestartCallback handler) override {
        _context->application.setRestartHandler(std::move(handler));
    }

    util::Result<void> setOwnAddress(const IndividualAddress& ownAddress) override {
        auto* dataLink = _context->medium.dataLinkLayer();
        if (!dataLink) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        dataLink->setOwnAddress(ownAddress);
        auto res = _context->application.setOwnAddress(ownAddress);
        if (res.isError()) {
            return res;
        }

        // A coupler's role and every point-to-point routing decision are
        // derived from its own address, so an ETS address assignment has to
        // reach the routing policy too — otherwise the device would answer to
        // its new address while still routing for the old one.
        auto& state = _context->couplerState;
        if (state.isCoupler()) {
            if (auto* secondary = state.secondaryMedium->dataLinkLayer()) {
                secondary->setOwnAddress(ownAddress);
            }
            if (state.coupler) {
                auto couplerRes = state.coupler->setOwnAddress(ownAddress);
                if (couplerRes.isError()) {
                    return couplerRes;
                }
            }
        }
        return util::Result<void>::ok();
    }

    void setIndividualAddressUpdateCallback(application::ApplicationLayer::IndividualAddressUpdateCallback callback) override {
        _context->application.setIndividualAddressUpdateCallback(std::move(callback));
    }

private:
    std::shared_ptr<Tp1LowerStackContext> _context;
};

class Tp1MediumAccess final : public LowerStackMediumAccess {
public:
    explicit Tp1MediumAccess(std::shared_ptr<Tp1LowerStackContext> context)
        : _context(std::move(context))
    {
    }

    void processBackgroundWork() override {
        _context->medium.processBackgroundWork();
        // The second port needs pumping on the same cadence, or traffic
        // arriving on it would only be seen when something else happened to
        // wake the stack.
        if (_context->couplerState.secondaryMedium) {
            _context->couplerState.secondaryMedium->processBackgroundWork();
        }
    }

    util::Result<void> sendDataLinkFrame(const datalink::LDataFrame& frame) override {
        auto primaryResult = _context->medium.sendDataLinkFrame(frame);

        // A coupler is reachable from both subnetworks, so its own telegrams
        // have to leave by both. Sending only on the primary would mean a
        // device on the sub line could address the coupler but never get an
        // answer. This is the coupler's own traffic only — forwarded frames go
        // out through TwoPortCoupler, which picks the correct single port.
        auto& state = _context->couplerState;
        if (state.isCoupler()) {
            auto secondaryResult = state.secondaryMedium->sendDataLinkFrame(frame);
            if (primaryResult.isError()) {
                return secondaryResult;
            }
        }
        return primaryResult;
    }

    void setDataLinkPromiscuousMode(datalink::PromiscuousMode mode) override {
        _context->medium.setDataLinkPromiscuousMode(mode);
    }

private:
    std::shared_ptr<Tp1LowerStackContext> _context;
};

class Tp1SecurityConfiguration final : public LowerStackSecurityConfiguration {
public:
    explicit Tp1SecurityConfiguration(std::shared_ptr<Tp1LowerStackContext> context)
        : _context(std::move(context))
    {
    }

    void setFunctionPropertyHandler(application::FunctionPropertyHandler handler) override {
        _context->application.setFunctionPropertyHandler(std::move(handler));
    }

    void setExtendedFunctionPropertyProvider(
        application::PropertyExtServices::FunctionProvider provider) override {
        _context->application.setExtendedFunctionPropertyProvider(std::move(provider));
    }

    void setCommissioningSerialNumber(const application::KnxSerialNumber& serialNumber) override {
        _context->application.setCommissioningSerialNumber(serialNumber);
    }

    void configureDataSecure(objects::SecurityInterfaceObject& securityObject) override {
#if KNX_SECURE_ENABLED
        _context->security.securityObject = &securityObject;
        _context->application.configureSecurity(_context->security.securityObject,
                            _context->platform);
#else
        (void)securityObject;
#endif
    }

private:
    std::shared_ptr<Tp1LowerStackContext> _context;
};

class Tp1InboundConfiguration final : public LowerStackInboundConfiguration {
public:
    explicit Tp1InboundConfiguration(std::shared_ptr<Tp1LowerStackContext> context)
        : _context(std::move(context))
    {
    }

    void setInboundCallback(StackPort::InboundCallback callback) override {
        _context->callbacks.inboundCallback = std::move(callback);
        if (_context->callbacks.inboundCallback) {
            _context->application.setInboundCallback(_context->callbacks.inboundCallback);
        }
    }

    void setWorkAvailableCallback(StackPort::WorkAvailableCallback callback) override {
        _context->callbacks.workAvailableCallback = std::move(callback);
        _context->application.setWorkAvailableCallback(_context->callbacks.workAvailableCallback);
    }

private:
    std::shared_ptr<Tp1LowerStackContext> _context;
};

class Tp1SubscriptionConfiguration final : public LowerStackSubscriptionConfiguration {
public:
    explicit Tp1SubscriptionConfiguration(std::shared_ptr<Tp1LowerStackContext> context)
        : _context(std::move(context))
    {
    }

    util::Result<void> addGroupAddress(const GroupAddress& address) override {
        const auto existing = std::find(_context->subscriptions.groupAddresses.begin(),
                                        _context->subscriptions.groupAddresses.end(),
                                        address);
        if (existing == _context->subscriptions.groupAddresses.end()) {
            if (!_context->subscriptions.groupAddresses.push_back(address)) {
                return util::ErrorCode::BufferTooSmall;
            }
        }

        if (!_context->lifecycle.initialized) {
            return util::Result<void>::ok();
        }

        auto primaryResult = _context->medium.addGroupAddress(address);
        if (primaryResult.isError()) {
            return primaryResult;
        }

        // Both ports must acknowledge this device's group addresses; a coupler
        // subscribes as one device however many attachments it has.
        auto& state = _context->couplerState;
        if (state.isCoupler()) {
            return state.secondaryMedium->addGroupAddress(address);
        }
        return primaryResult;
    }

private:
    std::shared_ptr<Tp1LowerStackContext> _context;
};

class Tp1PropertyAccessServices final : public LowerStackPropertyAccess {
public:
    explicit Tp1PropertyAccessServices(std::shared_ptr<Tp1LowerStackContext> context)
        : _context(std::move(context))
    {
    }

    void registerPropertyObject(InterfaceObjectType objectType,
                                InterfaceObjectIndex objectIndex,
                                StackPort::PropertyRegistration registration) override {
        _context->application.registerPropertyObject(objectType, objectIndex, std::move(registration));
    }

    void setPropertyReadProvider(StackPort::PropertyReadProvider provider) override {
        _context->application.setPropertyReadProvider(std::move(provider));
    }

    void setPropertyWriteProvider(StackPort::PropertyWriteProvider provider) override {
        _context->application.setPropertyWriteProvider(std::move(provider));
    }

    void setPropertyDescriptionProvider(StackPort::PropertyDescriptionProvider provider) override {
        _context->application.setPropertyDescriptionProvider(std::move(provider));
    }

    util::Result<void> registerMemoryRegion(const application::MemoryRegion& region,
                                            std::span<uint8_t> storage) override {
        return _context->application.registerMemoryRegion(region, storage);
    }

    bool memoryRegionWritten(MemoryAddress regionStart) const override {
        return _context->application.memoryRegionWritten(regionStart);
    }

private:
    std::shared_ptr<Tp1LowerStackContext> _context;
};

class Tp1TransmissionServices final : public LowerStackTransmissionServices {
public:
    explicit Tp1TransmissionServices(std::shared_ptr<Tp1LowerStackContext> context)
        : _context(std::move(context))
    {
    }

    void processBackgroundWork() override {
        _context->application.processBackgroundWork();
    }

    util::Result<void> sendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _context->application.sendGroupValueWrite(address, data);
    }

    util::Result<void> beginSendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _context->application.beginSendGroupValueWrite(address, data);
    }

    util::Result<void> sendGroupValueRead(const GroupAddress& address) override {
        return _context->application.sendGroupValueRead(address);
    }

    util::Result<void> beginSendGroupValueRead(const GroupAddress& address) override {
        return _context->application.beginSendGroupValueRead(address);
    }

    util::Result<StackPort::TransmissionProgressState> pollTransmissionProgress() override {
        return _context->application.pollTransmissionProgress();
    }

    bool transmissionInProgress() const override {
        return _context->application.transmissionInProgress();
    }

    void setDefaultTransmissionOptions(const StackPort::TransmissionOptions& options) override {
        _context->transmission.defaultTransmissionOptions = options;
        _context->application.setDefaultTransmissionOptions(options);
    }

    const StackPort::TransmissionOptions& defaultTransmissionOptions() const override {
        return _context->transmission.defaultTransmissionOptions;
    }

    bool popTransmissionOutcome(StackPort::TransmissionOutcome& outcome) override {
        return _context->application.popTransmissionOutcome(outcome);
    }

    size_t queuedTransmissionOutcomeCount() const override {
        return _context->application.queuedTransmissionOutcomeCount();
    }

    util::Result<void> sendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _context->application.sendGroupValueResponse(address, data);
    }

    util::Result<void> beginSendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _context->application.beginSendGroupValueResponse(address, data);
    }

    void setProgrammingModeEnabled(bool enabled) override {
        _context->application.setProgrammingModeEnabled(enabled);
    }

    bool programmingModeEnabled() const override {
        return _context->application.programmingModeEnabled();
    }

private:
    std::shared_ptr<Tp1LowerStackContext> _context;
};

class Tp1StackPort final : public StackPort {
public:
    Tp1StackPort(platform::Platform& platform,
                 std::unique_ptr<physical::Tp1MacPhysical> physicalLayer)
        : _lowerStack(createTp1LowerStackRoles(platform, std::move(physicalLayer)))
    {
    }

    /// Coupler form: the device's own stack runs on @p primaryPhysical, and
    /// @p secondaryPhysical becomes the sub-line the coupler routes to.
    Tp1StackPort(platform::Platform& platform,
                 std::unique_ptr<physical::Tp1MacPhysical> primaryPhysical,
                 std::unique_ptr<physical::Tp1MacPhysical> secondaryPhysical)
        : _lowerStack(createTp1LowerStackRoles(platform, std::move(primaryPhysical)))
    {
        // The medium bundle is built now but only initialised in init(), once
        // the individual address is known — the coupler's role depends on it.
        _lowerStack.context->couplerState.secondaryMedium =
            std::make_unique<Tp1MediumBundle>(platform, std::move(secondaryPhysical));
    }

    network::TwoPortCoupler* coupler() override {
        return _lowerStack.context ? _lowerStack.context->couplerState.coupler.get() : nullptr;
    }

#if KNX_FEATURE_NETIP
    Tp1StackPort(platform::Platform& platform,
                 std::unique_ptr<physical::IpTunnelingPhysical> physicalLayer)
        : _lowerStack(createTp1LowerStackRoles(platform, std::move(physicalLayer)))
    {
    }

#if KNX_SECURE_ENABLED
    Tp1StackPort(platform::Platform& platform,
                 std::unique_ptr<physical::IpSecureTunnelingPhysical> physicalLayer)
        : _lowerStack(createTp1LowerStackRoles(platform, std::move(physicalLayer)))
    {
    }
#endif

    Tp1StackPort(platform::Platform& platform,
                 std::unique_ptr<physical::IpRoutingPhysical> physicalLayer)
        : _lowerStack(createTp1LowerStackRoles(platform, std::move(physicalLayer)))
    {
    }
#endif  // KNX_FEATURE_NETIP

    Tp1StackPort(platform::Platform& platform,
                 std::unique_ptr<testing::MockTp1Physical> physicalLayer)
        : _lowerStack(createTp1LowerStackRoles(platform, std::move(physicalLayer)))
    {
    }

    Tp1StackPort(platform::Platform& platform,
                 std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
        : _lowerStack(createTp1LowerStackRoles(platform, std::move(mediumBackend)))
    {
    }

    util::Result<void> init(const IndividualAddress& ownAddress) override {
        return _lowerStack.lifecycle->init(ownAddress);
    }

    void close() override {
        _lowerStack.lifecycle->close();
    }

    void executePendingRestart() override {
        _lowerStack.lifecycle->executePendingRestart();
    }

    void setRestartHandler(application::RestartService::RestartCallback handler) override {
        _lowerStack.lifecycle->setRestartHandler(std::move(handler));
    }

    void processBackgroundWork() override {
        _lowerStack.medium->processBackgroundWork();
        _lowerStack.transmission->processBackgroundWork();
    }

    util::Result<void> sendDataLinkFrame(const datalink::LDataFrame& frame) override {
        return _lowerStack.medium->sendDataLinkFrame(frame);
    }

    void setDataLinkPromiscuousMode(datalink::PromiscuousMode mode) override {
        _lowerStack.medium->setDataLinkPromiscuousMode(mode);
    }

    util::Result<void> addGroupAddress(const GroupAddress& address) override {
        return _lowerStack.subscriptions->addGroupAddress(address);
    }

    util::Result<void> setOwnAddress(const IndividualAddress& ownAddress) override {
        return _lowerStack.lifecycle->setOwnAddress(ownAddress);
    }

    void setIndividualAddressUpdateCallback(application::ApplicationLayer::IndividualAddressUpdateCallback callback) override {
        _lowerStack.lifecycle->setIndividualAddressUpdateCallback(std::move(callback));
    }

    void configureDataSecure(objects::SecurityInterfaceObject& securityObject) override {
        _lowerStack.security->configureDataSecure(securityObject);
    }

    void setFunctionPropertyHandler(application::FunctionPropertyHandler handler) override {
        _lowerStack.security->setFunctionPropertyHandler(std::move(handler));
    }

    void setExtendedFunctionPropertyProvider(
        application::PropertyExtServices::FunctionProvider provider) override {
        _lowerStack.security->setExtendedFunctionPropertyProvider(std::move(provider));
    }

    void setCommissioningSerialNumber(const application::KnxSerialNumber& serialNumber) override {
        _lowerStack.security->setCommissioningSerialNumber(serialNumber);
    }

    void setInboundCallback(InboundCallback callback) override {
        _lowerStack.inbound->setInboundCallback(std::move(callback));
    }

    void registerPropertyObject(InterfaceObjectType objectType,
                                InterfaceObjectIndex objectIndex,
                                PropertyRegistration registration) override {
        _lowerStack.properties->registerPropertyObject(objectType, objectIndex, std::move(registration));
    }

    void setPropertyReadProvider(PropertyReadProvider provider) override {
        _lowerStack.properties->setPropertyReadProvider(std::move(provider));
    }

    void setPropertyWriteProvider(PropertyWriteProvider provider) override {
        _lowerStack.properties->setPropertyWriteProvider(std::move(provider));
    }

    void setPropertyDescriptionProvider(PropertyDescriptionProvider provider) override {
        _lowerStack.properties->setPropertyDescriptionProvider(std::move(provider));
    }

    util::Result<void> registerMemoryRegion(const application::MemoryRegion& region,
                                            std::span<uint8_t> storage) override {
        return _lowerStack.properties->registerMemoryRegion(region, storage);
    }

    bool memoryRegionWritten(MemoryAddress regionStart) const override {
        return _lowerStack.properties->memoryRegionWritten(regionStart);
    }

    void setWorkAvailableCallback(WorkAvailableCallback callback) override {
        _lowerStack.inbound->setWorkAvailableCallback(std::move(callback));
    }

    util::Result<void> sendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _lowerStack.transmission->sendGroupValueWrite(address, data);
    }

    util::Result<void> beginSendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _lowerStack.transmission->beginSendGroupValueWrite(address, data);
    }

    util::Result<void> sendGroupValueRead(const GroupAddress& address) override {
        return _lowerStack.transmission->sendGroupValueRead(address);
    }

    util::Result<void> beginSendGroupValueRead(const GroupAddress& address) override {
        return _lowerStack.transmission->beginSendGroupValueRead(address);
    }

    util::Result<TransmissionProgressState> pollTransmissionProgress() override {
        return _lowerStack.transmission->pollTransmissionProgress();
    }

    bool transmissionInProgress() const override {
        return _lowerStack.transmission->transmissionInProgress();
    }

    void setDefaultTransmissionOptions(const TransmissionOptions& options) override {
        _lowerStack.transmission->setDefaultTransmissionOptions(options);
    }

    const TransmissionOptions& defaultTransmissionOptions() const override {
        return _lowerStack.transmission->defaultTransmissionOptions();
    }

    bool popTransmissionOutcome(TransmissionOutcome& outcome) override {
        return _lowerStack.transmission->popTransmissionOutcome(outcome);
    }

    size_t queuedTransmissionOutcomeCount() const override {
        return _lowerStack.transmission->queuedTransmissionOutcomeCount();
    }

    util::Result<void> sendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _lowerStack.transmission->sendGroupValueResponse(address, data);
    }

    util::Result<void> beginSendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) override {
        return _lowerStack.transmission->beginSendGroupValueResponse(address, data);
    }

    void setProgrammingModeEnabled(bool enabled) override {
        _lowerStack.transmission->setProgrammingModeEnabled(enabled);
    }

    bool programmingModeEnabled() const override {
        return _lowerStack.transmission->programmingModeEnabled();
    }

private:
    struct LowerStackRoles {
        /// The shared lower-stack state. Held so the stack port can reach the
        /// coupler, which the role interfaces deliberately do not expose.
        std::shared_ptr<Tp1LowerStackContext> context;
        std::unique_ptr<LowerStackLifecycle> lifecycle;
        std::unique_ptr<LowerStackMediumAccess> mediumOwner;
        LowerStackMediumAccess* medium{nullptr};
        std::unique_ptr<LowerStackSecurityConfiguration> securityOwner;
        LowerStackSecurityConfiguration* security{nullptr};
        std::unique_ptr<LowerStackInboundConfiguration> inboundOwner;
        LowerStackInboundConfiguration* inbound{nullptr};
        std::unique_ptr<LowerStackSubscriptionConfiguration> subscriptionOwner;
        LowerStackSubscriptionConfiguration* subscriptions{nullptr};
        std::unique_ptr<LowerStackPropertyAccess> propertyOwner;
        LowerStackPropertyAccess* properties{nullptr};
        std::unique_ptr<LowerStackTransmissionServices> transmissionOwner;
        LowerStackTransmissionServices* transmission{nullptr};
    };

    template <typename PhysicalT>
    static LowerStackRoles createTp1LowerStackRoles(
        platform::Platform& platform,
        std::unique_ptr<PhysicalT> physicalLayer)
    {
        auto context = std::make_shared<Tp1LowerStackContext>(platform, std::move(physicalLayer));
        auto retainedContext = context;
        auto lifecycleServices = std::make_unique<Tp1LifecycleServices>(context);
        auto mediumServices = std::make_unique<Tp1MediumAccess>(context);
        auto securityServices = std::make_unique<Tp1SecurityConfiguration>(context);
        auto inboundServices = std::make_unique<Tp1InboundConfiguration>(context);
        auto subscriptionServices = std::make_unique<Tp1SubscriptionConfiguration>(context);
        auto propertyServices = std::make_unique<Tp1PropertyAccessServices>(context);
        auto transmissionServices = std::make_unique<Tp1TransmissionServices>(std::move(context));

        LowerStackRoles roles;
        roles.context = std::move(retainedContext);
        roles.lifecycle = std::move(lifecycleServices);
        roles.medium = mediumServices.get();
        roles.mediumOwner = std::move(mediumServices);
        roles.security = securityServices.get();
        roles.securityOwner = std::move(securityServices);
        roles.inbound = inboundServices.get();
        roles.inboundOwner = std::move(inboundServices);
        roles.subscriptions = subscriptionServices.get();
        roles.subscriptionOwner = std::move(subscriptionServices);
        roles.properties = propertyServices.get();
        roles.propertyOwner = std::move(propertyServices);
        roles.transmission = transmissionServices.get();
        roles.transmissionOwner = std::move(transmissionServices);
        return roles;
    }

    static LowerStackRoles createTp1LowerStackRoles(
        platform::Platform& platform,
        std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
    {
        auto context = std::make_shared<Tp1LowerStackContext>(platform, std::move(mediumBackend));
        auto retainedContext = context;
        auto lifecycleServices = std::make_unique<Tp1LifecycleServices>(context);
        auto mediumServices = std::make_unique<Tp1MediumAccess>(context);
        auto securityServices = std::make_unique<Tp1SecurityConfiguration>(context);
        auto inboundServices = std::make_unique<Tp1InboundConfiguration>(context);
        auto subscriptionServices = std::make_unique<Tp1SubscriptionConfiguration>(context);
        auto propertyServices = std::make_unique<Tp1PropertyAccessServices>(context);
        auto transmissionServices = std::make_unique<Tp1TransmissionServices>(std::move(context));

        LowerStackRoles roles;
        roles.context = std::move(retainedContext);
        roles.lifecycle = std::move(lifecycleServices);
        roles.medium = mediumServices.get();
        roles.mediumOwner = std::move(mediumServices);
        roles.security = securityServices.get();
        roles.securityOwner = std::move(securityServices);
        roles.inbound = inboundServices.get();
        roles.inboundOwner = std::move(inboundServices);
        roles.subscriptions = subscriptionServices.get();
        roles.subscriptionOwner = std::move(subscriptionServices);
        roles.properties = propertyServices.get();
        roles.propertyOwner = std::move(propertyServices);
        roles.transmission = transmissionServices.get();
        roles.transmissionOwner = std::move(transmissionServices);
        return roles;
    }

    LowerStackRoles _lowerStack;
};

} // namespace

std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::Tp1MacPhysical> physicalLayer)
{
    return std::make_unique<Tp1StackPort>(platform, std::move(physicalLayer));
}

std::unique_ptr<BusAccessStackPort> createTp1CouplerStackPort(
    platform::Platform& platform,
    std::unique_ptr<physical::Tp1MacPhysical> primaryPhysical,
    std::unique_ptr<physical::Tp1MacPhysical> secondaryPhysical)
{
    return std::make_unique<Tp1StackPort>(platform,
                                          std::move(primaryPhysical),
                                          std::move(secondaryPhysical));
}

#if KNX_FEATURE_NETIP
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpTunnelingPhysical> physicalLayer)
{
    return std::make_unique<Tp1StackPort>(platform, std::move(physicalLayer));
}

#if KNX_SECURE_ENABLED
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpSecureTunnelingPhysical> physicalLayer)
{
    return std::make_unique<Tp1StackPort>(platform, std::move(physicalLayer));
}
#endif

std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpRoutingPhysical> physicalLayer)
{
    return std::make_unique<Tp1StackPort>(platform, std::move(physicalLayer));
}
#endif  // KNX_FEATURE_NETIP

std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
{
    return std::make_unique<Tp1StackPort>(platform, std::move(mediumBackend));
}

std::unique_ptr<BusAccessStackPort> detail::createTp1MockTestStackPort(
    platform::Platform& platform,
    std::unique_ptr<testing::MockTp1Physical> physicalLayer)
{
    return std::make_unique<Tp1StackPort>(platform, std::move(physicalLayer));
}
} // namespace bau
} // namespace knx
