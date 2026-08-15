// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_runtime.hpp
 * @brief Runtime mediation core: assembles the KNX stack from a typed endpoint definition and transport.
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/platform/platform.hpp"
#include "knx/config.hpp"
#if KNX_FEATURE_NETIP
#include "knx/physical/ip_routing_physical.hpp"
#include "knx/physical/ip_tunneling_physical.hpp"
#endif
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/product/impl/bindings/endpoint_bindings.hpp"
#include "knx/product/impl/compiler/endpoint_compiler.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace knx::product {

struct EndpointInstanceConfig {
    IndividualAddress defaultIndividualAddress{initialIndividualAddress()};
    std::string_view persistenceNamespace{};
    /// Layout version of the persisted commissioned state.  Sourced from the
    /// product's PersistencePolicy::schemaVersion; state written under a
    /// different version is discarded instead of reinterpreted.
    uint16_t persistenceSchemaVersion{1};
    bool restoreKnxStateOnBoot{true};
};

enum class PendingBusActionKind : uint8_t {
    Publish = 0,
    ReadResponse = 1,
};

template <typename PortIdEnum>
struct PendingBusAction {
    PendingBusActionKind kind{PendingBusActionKind::Publish};
    PortIdEnum logicalId{};
    uint16_t slot{0};
    application::DptValue value{};
};

namespace detail {

template <typename ValueT>
    requires std::constructible_from<application::DptValue, ValueT>
constexpr application::DptValue makeRuntimeValue(ValueT value)
{
    return application::DptValue(value);
}

template <typename ValueT>
util::Result<ValueT> extractRuntimeValue(const application::DptValue& value)
{
    if (const auto* stored = value.template tryGet<ValueT>(); stored != nullptr) {
        return *stored;
    }

    return util::ErrorCode::DecodeFailed;
}

} // namespace detail

template <typename DefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class EndpointRuntime {
public:
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_id_type = typename definition_type::port_id_type;
    using bindings_type = EndpointBindings<DefinitionT, BindingCapacity>;
    using medium_backend_type = physical::Tp1MediumBackend;
    using OwnerWorkHint = bau::BusAccessUnit::OwnerWorkHint;
    using WorkAvailableCallback = bau::BusAccessUnit::WorkAvailableCallback;

    static constexpr size_t kPendingActionCapacity = definition_type::kPortCount * 2u;

    // Bindings by rvalue reference: the table is sized by the port count, so a
    // by-value sink would cost a full copy of it on the constructing frame's
    // stack on the way into _bindings.
    EndpointRuntime(const definition_type& definition,
                    bindings_type&& bindings,
                    EndpointInstanceConfig config)
        : _compiled(compileEndpointDefinition(definition))
        , _bindings(std::move(bindings))
        , _config(config)
    {
    }

    const auto& compiledDefinition() const
    {
        return _compiled;
    }

    const auto& instanceConfig() const
    {
        return _config;
    }

    template <auto LogicalId>
    util::Result<void> bindGroupAddress(GroupAddress address)
    {
        return bindGroupAddress(static_cast<port_id_type>(LogicalId), address);
    }

    util::Result<void> bindGroupAddress(port_id_type logicalId,
                                        GroupAddress address)
    {
        if (!address.isValid()) {
            return util::ErrorCode::InvalidAddress;
        }

        const auto* descriptor = _compiled.descriptorFor(logicalId);
        if (descriptor == nullptr) {
            return util::ErrorCode::InvalidParameter;
        }

        _configuredGroupAddresses[descriptor->slot] = address;

        if (_bau) {
            return _bau->bindGroupObjectToAddress(GroupObjectIndex(descriptor->slot), address);
        }

        return util::Result<void>::ok();
    }

    // ── Outbound transmit shaping ────────────────────────────────────────────
    // Provide a monotonic millisecond clock so per-object cyclic sends, min-
    // interval floors, and the rate limiter can run. Without it every publish is
    // sent immediately (legacy behaviour).
    void setTimeSource(bau::BusAccessUnit::TimeSourceFn timeSource)
    {
        _timeSource = std::move(timeSource);
        if (_bau) {
            _bau->setTimeSource(_timeSource);
        }
    }

    /// Global rate limit for this device's own unsolicited group sends.
    /// Requires setTimeSource() to take effect (no clock → no rate limiting).
    void setTelegramRateLimit(const application::TelegramRateLimitConfig& config)
    {
        _telegramRateLimit = config;
        if (_bau) {
            _bau->setTelegramRateLimit(config);
        }
    }

    /// Per-port send-on-change / cyclic / min-interval policy. Requires the BAU
    /// to be started (the group object must exist) — call after start(). The
    /// cyclic and min-interval parts additionally require setTimeSource(); the
    /// send-on-change filter works without a clock.
    util::Result<void> setTransmitPolicy(port_id_type logicalId,
                                         const application::GroupObjectTransmitPolicy& policy)
    {
        const auto* descriptor = _compiled.descriptorFor(logicalId);
        if (descriptor == nullptr) {
            return util::ErrorCode::InvalidParameter;
        }
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        return _bau->setGroupObjectTransmitPolicy(GroupObjectIndex(descriptor->slot), policy);
    }

    /// True when the project (ETS download or applyCommissionedGroupAddress)
    /// linked at least one group address to this port. Publishing on an
    /// unlinked port succeeds but puts nothing on the bus, so this is the way
    /// to tell an unused datapoint from a transmitting one.
    bool isPortLinked(port_id_type logicalId) const
    {
        const auto* descriptor = _compiled.descriptorFor(logicalId);
        if (descriptor == nullptr || !_bau) {
            return false;
        }
        return _bau->isGroupObjectLinked(GroupObjectIndex(descriptor->slot));
    }

    template <auto LogicalId>
    bool isPortLinked() const
    {
        return isPortLinked(static_cast<port_id_type>(LogicalId));
    }

    /// Read back the transmit policy currently applied to a port's object.
    util::Result<application::GroupObjectTransmitPolicy>
    transmitPolicy(port_id_type logicalId) const
    {
        const auto* descriptor = _compiled.descriptorFor(logicalId);
        if (descriptor == nullptr) {
            return util::ErrorCode::InvalidParameter;
        }
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        return _bau->groupObjectTransmitPolicy(GroupObjectIndex(descriptor->slot));
    }

    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<medium_backend_type> mediumBackend)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(mediumBackend));
        return finishStart();
    }

    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<bau::BusAccessStackPort> stackPort)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart();
    }

    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<physical::Tp1MacPhysical> physical)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        auto stackPort = bau::createTp1StackPort(platform, std::move(physical));
        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart();
    }

#if KNX_FEATURE_NETIP
    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<physical::IpTunnelingPhysical> physical)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        auto stackPort = bau::createTp1StackPort(platform, std::move(physical));
        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart();
    }

    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<physical::IpRoutingPhysical> physical)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        auto stackPort = bau::createTp1StackPort(platform, std::move(physical));
        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart();
    }
#endif  // KNX_FEATURE_NETIP

    void stop()
    {
        if (_bau) {
            _bau->close();
            _bau.reset();
        }

        _hadImmediateWork = false;
    }

    void loop()
    {
        if (_bau) {
            drainPendingActions();
            _bau->loop();
            // The runtime reports send results through its own callbacks; the
            // BAU outcome queue has no other consumer, so drain it here to
            // keep it from overflowing during bursts (ETS commissioning).
            bau::BusAccessUnit::TransmissionOutcome outcome;
            while (_bau->transmission().popOutcome(outcome)) {
            }
        }

        refreshWorkAvailabilityState();
    }

    /// Device object access for host-specific identity (e.g. serial number
    /// from a factory-programmed MAC).
    objects::DeviceObject& deviceObject() { return _bau->deviceObject(); }

    OwnerWorkHint ownerWorkHint() const
    {
        OwnerWorkHint hint = _bau ? _bau->ownerWorkHint() : OwnerWorkHint{};
        hint.pendingLoopWorkItems += _pendingActionCount;
        return hint;
    }

    void setWorkAvailableCallback(WorkAvailableCallback callback)
    {
        _workAvailableCallback = std::move(callback);
        if (_bau) {
            _bau->setWorkAvailableCallback([this]() { notifyWorkAvailableIfTransitioned(); });
        }

        refreshWorkAvailabilityState();
    }

    template <auto LogicalId, typename ValueT>
        requires std::same_as<std::remove_cvref_t<ValueT>, port_value_t<definition_type, LogicalId>>
    util::Result<void> publish(ValueT value)
    {
        return publish(static_cast<port_id_type>(LogicalId), value);
    }

    template <typename ValueT>
        requires std::constructible_from<application::DptValue, std::remove_cvref_t<ValueT>>
    util::Result<void> publish(port_id_type logicalId,
                               ValueT value)
    {
        const auto* descriptor = _compiled.descriptorFor(logicalId);
        if (descriptor == nullptr) {
            return util::ErrorCode::InvalidParameter;
        }
        if (!descriptor->transmit) {
            return util::ErrorCode::AccessDenied;
        }
        if (isInitialIndividualAddress(individualAddress())) {
            return util::ErrorCode::OperationNotReady;
        }

        const auto runtimeValue = detail::makeRuntimeValue(value);

        if (_bau) {
            return publishStarted(GroupObjectIndex(descriptor->slot), runtimeValue);
        }

        return enqueue(PendingBusAction<port_id_type>{
            .kind = PendingBusActionKind::Publish,
            .logicalId = logicalId,
            .slot = descriptor->slot,
            .value = runtimeValue,
        });
    }

    template <auto LogicalId>
    util::Result<void> requestReadResponse()
    {
        const auto* descriptor = _compiled.descriptorFor(LogicalId);
        if (descriptor == nullptr) {
            return util::ErrorCode::InvalidParameter;
        }
        if (!descriptor->readable) {
            return util::ErrorCode::AccessDenied;
        }
        if (isInitialIndividualAddress(individualAddress())) {
            return util::ErrorCode::OperationNotReady;
        }

        const auto state = _bindings.template readState<LogicalId>();
        if (!state.has_value()) {
            return util::ErrorCode::OperationNotReady;
        }

        if (_bau) {
            return respondStarted(GroupObjectIndex(descriptor->slot), detail::makeRuntimeValue(state.value()));
        }

        return enqueue(PendingBusAction<port_id_type>{
            .kind = PendingBusActionKind::ReadResponse,
            .logicalId = LogicalId,
            .slot = descriptor->slot,
            .value = detail::makeRuntimeValue(state.value()),
        });
    }

    template <auto LogicalId>
    util::Result<void> handleIncomingWrite(port_value_t<definition_type, LogicalId> value)
    {
        const auto* descriptor = _compiled.descriptorFor(LogicalId);
        if (descriptor == nullptr) {
            return util::ErrorCode::InvalidParameter;
        }
        if (!descriptor->receivable && !descriptor->writable) {
            return util::ErrorCode::AccessDenied;
        }

        bool handled = false;
        handled = _bindings.template dispatchCommand<LogicalId>(value) || handled;
        handled = _bindings.template dispatchStateWrite<LogicalId>(value) || handled;

        if (!handled) {
            return util::ErrorCode::OperationNotReady;
        }

        return util::Result<void>::ok();
    }

    void toggleProgrammingMode()
    {
        if (_bau) {
            const bool current = _bau->management().commissioningModeEnabled();
            (void)_bau->management().setCommissioningMode(!current);
            return;
        }

        _programmingModeActive = !_programmingModeActive;
        _bindings.notifyProgrammingModeChanged(_programmingModeActive);
    }

    /**
     * @brief Publish the Router Object and bind it to the coupler.
     *
     * Only meaningful when the stack port was built as a coupler. The filter
     * table and routing policy come from that coupler, so leave them null in
     * @p config unless the firmware deliberately owns its own.
     */
    util::Result<void> configureRouterRole(const bau::BusAccessUnit::RouterRoleConfig& config)
    {
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        return _bau->management().configureRouterRole(config);
    }

    /**
     * @brief Apply a downloaded coupler configuration to the live routing policy.
     *
     * ETS writes PID_MAIN_LCCONFIG and its siblings as ordinary properties,
     * which land in the Router Object and nowhere else. Call this after a
     * download for them to take effect on the forwarding path.
     */
    util::Result<void> syncRouterRoutingConfig()
    {
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        return _bau->management().syncRouterRoutingConfig();
    }

    /// The coupler, or nullptr when this is an ordinary single-port device.
    network::TwoPortCoupler* coupler()
    {
        return _bau ? _bau->link().coupler() : nullptr;
    }

    bool isProgrammingModeActive() const
    {
        if (_bau) {
            return _bau->management().commissioningModeEnabled();
        }

        return _programmingModeActive;
    }

    IndividualAddress individualAddress() const
    {
        if (_bau) {
            return _bau->deviceObject().getIndividualAddress();
        }

        return _config.defaultIndividualAddress;
    }

    void reportFault(FaultInfo info) const
    {
        _bindings.notifyFault(info);
    }

    size_t pendingActionCount() const
    {
        return _pendingActionCount;
    }

    const PendingBusAction<port_id_type>* pendingActionAt(size_t index) const
    {
        if (index >= _pendingActionCount) {
            return nullptr;
        }

        return &_pendingActions[index];
    }

    void clearPendingActions()
    {
        _pendingActionCount = 0u;
        refreshWorkAvailabilityState();
    }

    /// Number of group objects registered in the CO table after a successful start().
    /// Always equals definition_type::kPortCount — the count is compile-time fixed
    /// by the product definition and cannot overflow regardless of runtime inputs.
    size_t registeredGroupObjectCount() const
    {
        if (!_bau) {
            return 0u;
        }
        return _bau->groupObjectTable().objectCount();
    }

    /// Install a callback that is invoked whenever the KNX management model
    /// writes new program data (ETS parameter block) to the application program
    /// interface object.  The callback fires immediately with the current
    /// program data so the caller can apply any values already stored from a
    /// previous ETS session.
    ///
    /// Must be called after a successful start() so that the BAU and its
    /// ApplicationProgramObject are initialised.
    void wireParameterDataCallback(
        objects::ApplicationProgramObject::ProgramDataChangedCallback callback)
    {
        if (!_bau) {
            return;
        }

        auto& prog = _bau->applicationProgram();
        prog.setOnProgramDataChanged(callback);

        // Apply any program data that was restored from persistence.
        const auto existing = prog.getProgramData();
        if (!existing.empty()) {
            callback(existing);
        }
    }

    /// For expert/test use: simulate ETS writing the application program data
    /// through the KNX management model (A_PropertyValue_Write → PID_PROGRAM_DATA).
    ///
    /// This exercises the same ApplicationProgramObject → notifyProgramDataChanged
    /// → wireParameterDataCallback chain that a live ETS commissioning session
    /// uses.  Must be called after a successful start().
    void simulateEtsProgramDataWriteForExpertUse(std::span<const uint8_t> data)
    {
        if (!_bau) {
            return;
        }
        auto& prog = _bau->applicationProgram();
        prog.setProgramData(data);
        prog.notifyProgramDataChanged();
    }

    /// Persist an encoded parameter byte block to the ApplicationProgramObject
    /// WITHOUT firing the ProgramDataChanged callback.  Use this when parameter
    /// values have already been applied in-memory (e.g. via ParameterState::apply)
    /// and you only need to sync the persisted representation so the values are
    /// restored correctly on the next boot.
    ///
    /// Calling simulateEtsProgramDataWriteForExpertUse would also persist, but it
    /// redundantly re-fires all onParameterChanged callbacks.  This helper avoids
    /// that second pass while still keeping persistence consistent.
    void persistParameterBytesOnly(std::span<const uint8_t> data)
    {
        if (!_bau) {
            return;
        }
        _bau->applicationProgram().setProgramData(data);
    }

    /// Re-point the program-data callback (e.g. after the owner object is moved)
    /// without re-applying any already-stored program data.  The caller is
    /// responsible for ensuring any existing state was already transferred.
    void rewireParameterDataCallbackNoReapply(
        objects::ApplicationProgramObject::ProgramDataChangedCallback callback)
    {
        if (!_bau) {
            return;
        }
        _bau->applicationProgram().setOnProgramDataChanged(std::move(callback));
    }

    void rewireCallbacksAfterMove()
    {
        if (!_bau) {
            return;
        }

        wireCallbacks();

        if (_workAvailableCallback) {
            _bau->setWorkAvailableCallback([this]() { notifyWorkAvailableIfTransitioned(); });
        }

        refreshWorkAvailabilityState();
    }

    // -----------------------------------------------------------------------
    // Security passthrough helpers
    //
    // These thin wrappers keep the SecurityInterfaceObject internals out of the
    // commissioned_product.hpp layer.  Must be called after a successful start().
    // -----------------------------------------------------------------------

    /// Enable KNX Data Secure mode on the Security Interface Object.
    void enableSecurityMode()
    {
        if (_bau) {
            _bau->securityObject().setSecurityMode(objects::SecurityMode::Enabled);
        }
    }

    /// Apply an ETS tool key to the device's Security Interface Object.
    void applyToolKey(const std::array<uint8_t, 16>& key)
    {
        if (_bau) {
            _bau->securityObject().setToolKey(key);
        }
    }

    /// Apply a group key for a specific group address.
    void applyGroupKey(GroupAddress address, const std::array<uint8_t, 16>& key)
    {
        if (_bau) {
            _bau->securityObject().setGroupKey(address, key);
        }
    }

    /// Returns true if the ETS tool key has been set (non-zero).
    bool hasToolKey() const
    {
        return _bau && _bau->securityObject().hasToolKey();
    }

    /// Returns true if KNX Data Secure mode is currently enabled.
    bool isSecurityEnabled() const
    {
        return _bau && _bau->securityObject().isSecurityEnabled();
    }

    using write_dispatch_fn = util::Result<void> (EndpointRuntime::*)(GroupObjectIndex);
    using read_prepare_fn = util::Result<void> (EndpointRuntime::*)(GroupObjectIndex);

    template <size_t... Indices>
    static constexpr auto makeWriteDispatchTable(std::index_sequence<Indices...>)
    {
        return std::array<write_dispatch_fn, sizeof...(Indices)>{
            &EndpointRuntime::template dispatchWriteForSlotIndex<Indices>...,
        };
    }

    template <size_t... Indices>
    static constexpr auto makeReadPrepareTable(std::index_sequence<Indices...>)
    {
        return std::array<read_prepare_fn, sizeof...(Indices)>{
            &EndpointRuntime::template prepareReadForSlotIndex<Indices>...,
        };
    }

    template <size_t Index>
    util::Result<void> dispatchWriteForSlotIndex(GroupObjectIndex slot)
    {
        using port_spec = std::tuple_element_t<Index, typename definition_type::ports_tuple>;
        const auto value = _bau->groupObjectValue(slot);
        if (value.isError()) {
            return value.error();
        }

        const auto typedValue = detail::extractRuntimeValue<typename port_spec::value_type>(value.value());
        if (typedValue.isError()) {
            return typedValue.error();
        }

        return handleIncomingWrite<port_spec::logicalId>(typedValue.value());
    }

    template <size_t Index>
    util::Result<void> prepareReadForSlotIndex(GroupObjectIndex slot)
    {
        using port_spec = std::tuple_element_t<Index, typename definition_type::ports_tuple>;

        if constexpr (!port_spec::readable) {
            return util::Result<void>::ok();
        } else {
            const auto state = _bindings.template readState<port_spec::logicalId>();
            if (!state.has_value()) {
                return util::ErrorCode::OperationNotReady;
            }

            return _bau->setGroupObjectValue(slot, detail::makeRuntimeValue(state.value()));
        }
    }

    util::Result<void> finishStart()
    {
        applyIdentity();

        // Apply any transmit-shaping configured before start() (the BAU only
        // exists now). The time source also drives cyclic sends via BAU loop().
        if (_timeSource) {
            _bau->setTimeSource(_timeSource);
        }
        _bau->setTelegramRateLimit(_telegramRateLimit);

        const auto registerObjects = registerGroupObjects();
        if (registerObjects.isError()) {
            _bau.reset();
            return registerObjects.error();
        }

        wireCallbacks();

        if (_workAvailableCallback) {
            _bau->setWorkAvailableCallback([this]() { notifyWorkAvailableIfTransitioned(); });
        }

        const auto init = _bau->init(_config.defaultIndividualAddress,
                                     _config.persistenceNamespace,
                                     _config.persistenceSchemaVersion);
        if (init.isError()) {
            reportFault({FaultCode::StartFailed, "EndpointRuntime start failed"});
            _bau.reset();
            return init.error();
        }

        const auto bindConfigured = applyConfiguredGroupAddresses();
        if (bindConfigured.isError()) {
                reportFault({FaultCode::StartFailed, "EndpointRuntime address binding failed"});
            _bau->close();
            _bau.reset();
            return bindConfigured.error();
        }

        seedReadableState();
        drainPendingActions();
        refreshWorkAvailabilityState();
        return util::Result<void>::ok();
    }

    void applyIdentity()
    {
        auto& device = _bau->deviceObject();
        device.setManufacturerId(_compiled.identity.manufacturerId);
        device.setFirmwareRevision(_compiled.identity.firmwareRevision);
        device.setMaxApduLength(_compiled.identity.maxApduLength);

        // PID_HARDWARE_TYPE (03/05/01 §4.3.28): MSB must be 00h; the remaining
        // five octets are the manufacturer's hardware identifier. Encoded here
        // as manufacturer id + hardware serial so it is unique per product and
        // matches Hardware/@SerialNumber in the exported knxprod.
        const uint16_t manufacturerValue = _compiled.identity.manufacturerId.value();
        const uint16_t hardwareSerial = _compiled.identity.hardwareSerialNumber;
        const uint8_t hardwareType[6] = {
            0x00,
            static_cast<uint8_t>((manufacturerValue >> 8) & 0xFFu),
            static_cast<uint8_t>(manufacturerValue & 0xFFu),
            _compiled.identity.hardwareVersion,
            static_cast<uint8_t>((hardwareSerial >> 8) & 0xFFu),
            static_cast<uint8_t>(hardwareSerial & 0xFFu),
        };
        device.setHardwareType(hardwareType);

        device.setOrderInfo(std::string(_compiled.identity.orderNumber));

        // PID_VERSION as DPT_Version: magic 0 (no versioning scheme change),
        // version = hardware version, revision = firmware revision.
        device.setVersion(0,
                          _compiled.identity.hardwareVersion,
                          _compiled.identity.firmwareRevision);

        auto& program = _bau->applicationProgram();
        program.setApplicationManufacturer(_compiled.identity.manufacturerId.value());
        program.setApplicationNumber(_compiled.identity.applicationNumber);
        program.setApplicationVersion(static_cast<uint8_t>(_compiled.identity.applicationVersion & 0xFFu));
        // PID_PROGRAM_VERSION (PDT_GENERIC_05): manufacturer id, application
        // number, version — ETS overwrites this block during the download.
        const uint16_t manufacturer = _compiled.identity.manufacturerId.value();
        const uint16_t appNumber = _compiled.identity.applicationNumber;
        const uint8_t programVersionBlock[objects::ApplicationProgramObject::kProgramVersionSize] = {
            static_cast<uint8_t>((manufacturer >> 8) & 0xFFu),
            static_cast<uint8_t>(manufacturer & 0xFFu),
            static_cast<uint8_t>((appNumber >> 8) & 0xFFu),
            static_cast<uint8_t>(appNumber & 0xFFu),
            static_cast<uint8_t>(_compiled.identity.applicationVersion & 0xFFu),
        };
        program.setProgramVersionBlock(programVersionBlock);
    }

    util::Result<void> registerGroupObjects()
    {
        for (const auto& descriptor : _compiled.runtime.communicationObjects) {
            application::GroupObjectConfig config{};
            config.address = _configuredGroupAddresses[descriptor.slot];
            config.dpt = descriptor.dpt;
            config.flags.communication = true;
            config.flags.read = descriptor.readable;
            config.flags.write = descriptor.writable;
            config.flags.transmit = descriptor.transmit;
            config.flags.update = descriptor.receivable;
            config.flags.readOnInit = descriptor.readOnInit;
            config.flags.priority = descriptor.priority;

            auto object = std::make_unique<application::GroupObject>(config);
            const GroupObjectIndex index = _bau->groupObjectTable().addGroupObject(std::move(object));
            if (!index.isValid() || index.value() != descriptor.slot) {
                reportFault({FaultCode::InternalError, "EndpointRuntime slot registration failed"});
                return util::ErrorCode::ResourceUnavailable;
            }
        }

        return util::Result<void>::ok();
    }

    void wireCallbacks()
    {
        _bau->setGroupObjectWriteCallback([this](GroupObjectIndex slot, std::span<const uint8_t> /*data*/) {
            (void)dispatchStartedWrite(slot);
        });

        _bau->setGroupObjectReadCallback([this](GroupObjectIndex slot) {
            (void)prepareStartedRead(slot);
        });

        _bau->deviceObject().setInternalProgModeCallback([this](Toggle mode) {
            _programmingModeActive = (mode == Toggle::Enable);
            _bindings.notifyProgrammingModeChanged(_programmingModeActive);
        });
    }

    util::Result<void> applyConfiguredGroupAddresses()
    {
        for (const auto& descriptor : _compiled.runtime.communicationObjects) {
            const GroupAddress address = _configuredGroupAddresses[descriptor.slot];
            if (!address.isValid()) {
                continue;
            }

            const auto bind = _bau->bindGroupObjectToAddress(GroupObjectIndex(descriptor.slot), address);
            if (bind.isError()) {
                return bind.error();
            }
        }

        return util::Result<void>::ok();
    }

    void seedReadableState()
    {
        for (const auto& descriptor : _compiled.runtime.communicationObjects) {
            (void)prepareStartedRead(GroupObjectIndex(descriptor.slot));
        }
    }

    util::Result<void> dispatchStartedWrite(GroupObjectIndex slot)
    {
        if (!_bau || slot.value() >= kWriteDispatchers.size()) {
            return util::ErrorCode::InvalidAddress;
        }

        return (this->*kWriteDispatchers[slot.value()])(slot);
    }

    util::Result<void> prepareStartedRead(GroupObjectIndex slot)
    {
        if (!_bau || slot.value() >= kReadPreparers.size()) {
            return util::ErrorCode::InvalidAddress;
        }

        return (this->*kReadPreparers[slot.value()])(slot);
    }

    util::Result<void> publishStarted(GroupObjectIndex slot, const application::DptValue& value)
    {
        if (isInitialIndividualAddress(individualAddress())) {
            return util::ErrorCode::OperationNotReady;
        }

        const auto setValue = _bau->setGroupObjectValue(slot, value);
        if (setValue.isError()) {
            return setValue.error();
        }

        const auto* object = _bau->groupObject(slot);
        if (object == nullptr) {
            return util::ErrorCode::InvalidAddress;
        }

        return _bau->sendGroupValueForObject(slot, object->getRawValue());
    }

    util::Result<void> respondStarted(GroupObjectIndex slot, const application::DptValue& value)
    {
        if (isInitialIndividualAddress(individualAddress())) {
            return util::ErrorCode::OperationNotReady;
        }

        const auto setValue = _bau->setGroupObjectValue(slot, value);
        if (setValue.isError()) {
            return setValue.error();
        }

        const auto* object = _bau->groupObject(slot);
        if (object == nullptr) {
            return util::ErrorCode::InvalidAddress;
        }

        return _bau->respondGroupValueForObject(slot, object->getRawValue());
    }

    void drainPendingActions()
    {
        if (!_bau) {
            return;
        }

        while (_pendingActionCount > 0u) {
            const PendingBusAction<port_id_type> action = _pendingActions[0];
            for (size_t index = 1u; index < _pendingActionCount; ++index) {
                _pendingActions[index - 1u] = _pendingActions[index];
            }
            --_pendingActionCount;

            if (action.kind == PendingBusActionKind::Publish) {
                (void)publishStarted(GroupObjectIndex(action.slot), action.value);
            } else {
                (void)respondStarted(GroupObjectIndex(action.slot), action.value);
            }
        }
    }

    util::Result<void> enqueue(PendingBusAction<port_id_type> action)
    {
        if (_pendingActionCount >= _pendingActions.size()) {
            return util::ErrorCode::QueueFull;
        }

        _pendingActions[_pendingActionCount] = std::move(action);
        ++_pendingActionCount;
        notifyWorkAvailableIfTransitioned();
        return util::Result<void>::ok();
    }

    void refreshWorkAvailabilityState()
    {
        _hadImmediateWork = ownerWorkHint().hasImmediateWork();
    }

    void notifyWorkAvailableIfTransitioned()
    {
        const bool hasImmediateWork = ownerWorkHint().hasImmediateWork();
        if (!_hadImmediateWork && hasImmediateWork && _workAvailableCallback) {
            _workAvailableCallback();
        }
        _hadImmediateWork = hasImmediateWork;
    }

    inline static constexpr auto kWriteDispatchers =
        makeWriteDispatchTable(std::make_index_sequence<definition_type::kPortCount>{});
    inline static constexpr auto kReadPreparers =
        makeReadPrepareTable(std::make_index_sequence<definition_type::kPortCount>{});

    CompiledEndpointDefinition<definition_type> _compiled;
    bindings_type _bindings;
    EndpointInstanceConfig _config{};
    bool _programmingModeActive{false};
    std::array<GroupAddress, definition_type::kPortCount> _configuredGroupAddresses{};
    std::array<PendingBusAction<port_id_type>, kPendingActionCapacity> _pendingActions{};
    size_t _pendingActionCount{0u};
    WorkAvailableCallback _workAvailableCallback{};
    bool _hadImmediateWork{false};
    bau::BusAccessUnit::TimeSourceFn _timeSource{};
    application::TelegramRateLimitConfig _telegramRateLimit{};
    std::unique_ptr<bau::BusAccessUnit> _bau;
};

} // namespace knx::product