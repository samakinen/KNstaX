// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_runtime.hpp
 * @brief Runtime mediation core: assembles the KNX stack from a typed endpoint definition and transport.
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/platform/platform.hpp"
#include "knx/config.hpp"
#if KNX_FEATURE_NETIP
#include "knx/physical/ip_routing_physical.hpp"
#endif
#if KNX_IP_TUNNELING
#include "knx/physical/ip_tunneling_physical.hpp"
#endif
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/product/impl/bindings/endpoint_bindings.hpp"
#include "knx/product/impl/compiler/endpoint_compiler.hpp"
#include "knx/product/impl/model/port_table.hpp"
#include "knx/product/impl/runtime/product_port_core.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"

#include <algorithm>
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

/// A publish or read response queued before the stack started. The logical id
/// is carried as its 16-bit value (cast it to the port enum to compare). The
/// record is the same for every product; see PendingPortAction.
template <typename PortIdEnum>
using PendingBusAction = PendingPortAction;

/**
 * @brief Runtime mediation for one endpoint definition: a thin shell around ProductPortCore.
 *
 * The shell is what depends on the product: it owns the binding entries, the
 * configured group addresses and the pending-action queue, all sized from the
 * definition's type, and gives the typed API (`publish<Id>(value)` and friends)
 * its compile-time port lookup. Every operation on a port is a call into the
 * core with the port's slot.
 *
 * The pending-action queue holds KNX_PENDING_ACTION_CAPACITY entries, or twice
 * the port count when that symbol is 0 (see Kconfig: Capacities). A request
 * made while the queue is full fails with QueueFull; nothing is dropped.
 */
template <typename DefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class EndpointRuntime {
public:
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_id_type = typename definition_type::port_id_type;
    using bindings_type = EndpointBindings<DefinitionT, BindingCapacity>;
    using medium_backend_type = physical::Tp1MediumBackend;
    using OwnerWorkHint = bau::BusAccessUnit::OwnerWorkHint;
    using WorkAvailableCallback = bau::BusAccessUnit::WorkAvailableCallback;

    static constexpr size_t kPendingActionCapacity =
        knx::config::PENDING_ACTION_CAPACITY != 0u ? knx::config::PENDING_ACTION_CAPACITY
                                                   : definition_type::kPortCount * 2u;

    // Bindings by rvalue reference: the entry arrays are sized by the port
    // count, so a by-value sink would cost a full copy of them on the
    // constructing frame's stack on the way into _bindings.
    EndpointRuntime(const definition_type& definition,
                    bindings_type&& bindings,
                    EndpointInstanceConfig config)
        : _definition(definition)
        , _bindings(std::move(bindings))
        , _config(config)
        , _core(portTable().data(),
                static_cast<uint16_t>(portTable().size()),
                _configuredGroupAddresses.data(),
                _pendingActions.data(),
                static_cast<uint16_t>(_pendingActions.size()),
                PortBindingTables{_bindings.commandEntries(),
                                  _bindings.stateWriteEntries(),
                                  _bindings.stateProviderEntries(),
                                  static_cast<uint16_t>(bindings_type::kStateWriteEntryCount)},
                config.defaultIndividualAddress)
    {
    }

    // start() installs callbacks capturing `this` in the BAU and its objects.
    EndpointRuntime(const EndpointRuntime&) = delete;
    EndpointRuntime& operator=(const EndpointRuntime&) = delete;
    EndpointRuntime(EndpointRuntime&&) = delete;
    EndpointRuntime& operator=(EndpointRuntime&&) = delete;

    /// The compiled definition (runtime and export descriptors), built on
    /// demand from the port tables and returned by value: the runtime keeps
    /// only its identity and a pointer to the rodata port table, not a RAM copy
    /// of every descriptor. A caller that wants a reference binds it to a
    /// local (`const auto compiled = runtime.compiledDefinition();`), which
    /// the copy elision makes free. Diagnostics and tests use this; the
    /// firmware's own paths read the identity or the port table.
    auto compiledDefinition() const
    {
        return compileEndpointDefinition(_definition);
    }

    /// The product identity this runtime was built from.
    const ProductIdentity& identity() const
    {
        return _definition.identity;
    }

    const auto& instanceConfig() const
    {
        return _config;
    }

    template <auto LogicalId>
    util::Result<void> bindGroupAddress(GroupAddress address)
    {
        return _core.bindGroupAddress(slotOfId<LogicalId>(), address);
    }

    util::Result<void> bindGroupAddress(port_id_type logicalId,
                                        GroupAddress address)
    {
        return _core.bindGroupAddress(slotOfLogicalId(logicalId), address);
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
        return _core.setTransmitPolicy(slotOfLogicalId(logicalId), policy);
    }

    /// True when the project (ETS download or applyCommissionedGroupAddress)
    /// linked at least one group address to this port. Publishing on an
    /// unlinked port succeeds but puts nothing on the bus, so this is the way
    /// to tell an unused datapoint from a transmitting one.
    bool isPortLinked(port_id_type logicalId) const
    {
        return _core.isPortLinked(slotOfLogicalId(logicalId));
    }

    template <auto LogicalId>
    bool isPortLinked() const
    {
        return _core.isPortLinked(slotOfId<LogicalId>());
    }

    /// Read back the transmit policy currently applied to a port's object.
    util::Result<application::GroupObjectTransmitPolicy>
    transmitPolicy(port_id_type logicalId) const
    {
        return _core.transmitPolicy(slotOfLogicalId(logicalId));
    }

#if KNX_FEATURE_TP1
    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<medium_backend_type> mediumBackend)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(mediumBackend));
        return finishStart(platform);
    }
#endif

    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<bau::BusAccessStackPort> stackPort)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart(platform);
    }

#if KNX_FEATURE_TP1
    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<physical::Tp1MacPhysical> physical)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        auto stackPort = bau::createTp1StackPort(platform, std::move(physical));
        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart(platform);
    }
#endif

#if KNX_IP_TUNNELING
    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<physical::IpTunnelingPhysical> physical)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        auto stackPort = bau::createTp1StackPort(platform, std::move(physical));
        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart(platform);
    }
#endif  // KNX_IP_TUNNELING

#if KNX_FEATURE_NETIP

    util::Result<void> start(platform::Platform& platform,
                             std::unique_ptr<physical::IpRoutingPhysical> physical)
    {
        if (_bau) {
            return util::Result<void>::ok();
        }

        auto stackPort = bau::createTp1StackPort(platform, std::move(physical));
        _bau = std::make_unique<bau::BusAccessUnit>(platform, std::move(stackPort));
        return finishStart(platform);
    }
#endif  // KNX_FEATURE_NETIP

#if KNX_FEATURE_NETIP
    /// Re-issue the routing group membership; see
    /// physical::IpRoutingPhysical::refreshMulticastMembership().
    util::Result<void> refreshMulticastMembership()
    {
        if (!_bau) {
            return util::ErrorCode::NotInitialized;
        }
        return _bau->link().refreshMulticastMembership();
    }

    /// See bau::BusAccessUnit::adoptRoutingMulticastAddress. Needs the
    /// restored KNXnet/IP Parameter Object, so before start() nothing is
    /// adopted and 0.0.0.0 is returned.
    IpAddress adoptRoutingMulticastAddress(IpAddress productDefault, bool productDefaultWins)
    {
        if (!_bau) {
            return IpAddress(0);
        }
        return _bau->management().adoptRoutingMulticastAddress(productDefault, productDefaultWins);
    }

    /// See bau::BusAccessUnit::routingMulticastAddress.
    [[nodiscard]] IpAddress routingMulticastAddress() const
    {
        return _bau ? _bau->management().routingMulticastAddress() : IpAddress(0);
    }

    /// See bau::BusAccessUnit::setRoutingMulticastAddressCallback. Kept when
    /// set before start(), and installed then.
    void setRoutingMulticastAddressCallback(bau::ManagementControl::RoutingMulticastAddressCallback callback)
    {
        if (_bau) {
            _bau->management().setRoutingMulticastAddressCallback(std::move(callback));
            return;
        }
        _pendingRoutingMulticastAddressCallback = std::move(callback);
    }
#endif

    /// Counters for the port; see bau::LinkDiagnostics.
    bau::LinkDiagnostics linkDiagnostics() const
    {
        return _bau ? _bau->link().diagnostics() : bau::LinkDiagnostics{};
    }

    /// Raw data-link access; see bau::LinkAccess and the commissioned
    /// runtime's link().
    bau::LinkAccess link()
    {
        return _bau ? _bau->link() : bau::LinkAccess(nullptr);
    }

    void stop()
    {
        if (_bau) {
            _bau->close();
            dropBau();
        }

        _hadImmediateWork = false;
    }

    void loop()
    {
        if (_bau) {
            _core.drainPendingActions();
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

    /// See bau::BusAccessUnit::requestPowerFailSave(). Safe from any context.
    void requestPowerFailSave()
    {
        if (_bau) {
            _bau->requestPowerFailSave();
        }
    }

    OwnerWorkHint ownerWorkHint() const
    {
        OwnerWorkHint hint = _bau ? _bau->ownerWorkHint() : OwnerWorkHint{};
        hint.pendingLoopWorkItems += _core.pendingActionCount();
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
        // The slot is a compile-time constant: the call site is one load, one
        // value construction and one call into the core.
        return publishSlot(slotOfId<LogicalId>(), application::DptValue(value));
    }

    template <typename ValueT>
        requires std::constructible_from<application::DptValue, std::remove_cvref_t<ValueT>>
    util::Result<void> publish(port_id_type logicalId,
                               ValueT value)
    {
        return publishSlot(slotOfLogicalId(logicalId), application::DptValue(value));
    }

    template <auto LogicalId>
    util::Result<void> requestReadResponse()
    {
        const size_t queued = _core.pendingActionCount();
        const auto result = _core.respond(slotOfId<LogicalId>());
        if (_core.pendingActionCount() != queued) {
            notifyWorkAvailableIfTransitioned();
        }
        return result;
    }

    template <auto LogicalId>
    util::Result<void> handleIncomingWrite(port_value_t<definition_type, LogicalId> value)
    {
        return _core.dispatchValue(slotOfId<LogicalId>(), application::DptValue(value));
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

#if KNX_FEATURE_COUPLER
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
#endif  // KNX_FEATURE_COUPLER

#if KNX_FEATURE_NETIP
    /**
     * @brief Publish the device's live IP identity to the KNXnet/IP Parameter Object.
     *
     * The addresses only exist once the interface is up, which is after the
     * stack starts, so this cannot be part of start(). Call it when the lease
     * lands and again whenever it changes.
     */
    util::Result<void> applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config)
    {
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        return _bau->management().applyKnxNetIpConfiguration(config);
    }

    /// Observe ETS downloading the KNX/IP Secure Routing backbone key. Kept
    /// when set before start(), and installed then.
    void setBackboneKeyCallback(bau::ManagementControl::BackboneKeyCallback callback)
    {
        if (_bau) {
            _bau->management().setBackboneKeyCallback(std::move(callback));
            return;
        }
        _pendingBackboneKeyCallback = std::move(callback);
    }

    /// Current value of a property of the KNXnet/IP Parameter Object (or any
    /// reference object); empty before start() or when never written. The
    /// copy is taken under the object's value lock, so this is safe from the
    /// routing receive task.
    std::vector<uint8_t> referenceObjectPropertyValue(InterfaceObjectType objectType,
                                                      application::PropertyID propertyId) const
    {
        if (!_bau) {
            return {};
        }
        return _bau->management().referenceObjectPropertyValue(objectType, propertyId);
    }

    /// Seed a reference object property, through the path ETS writes it.
    util::Result<void> setReferenceObjectProperty(InterfaceObjectType objectType,
                                                  application::PropertyID propertyId,
                                                  std::span<const uint8_t> value)
    {
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        return _bau->management().setReferenceObjectProperty(objectType, propertyId, value);
    }
#endif

#if KNX_FEATURE_COUPLER
    /// The coupler, or nullptr when this is an ordinary single-port device.
    network::TwoPortCoupler* coupler()
    {
        return _bau ? _bau->link().coupler() : nullptr;
    }
#endif

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
        return _core.pendingActionCount();
    }

    const PendingBusAction<port_id_type>* pendingActionAt(size_t index) const
    {
        return _core.pendingActionAt(index);
    }

    void clearPendingActions()
    {
        _core.clearPendingActions();
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

    /// The product's parameter image (docs/design/parameter-model.md §3.4).
    /// Before start(): the stack then lets ETS write into it, stores it with
    /// its fingerprint and restores it.
    void bindParameterImage(const objects::ParameterImageBinding& binding)
    {
        _parameterImage = binding;
    }

    /// Called after ETS completed a parameter download that passed the bounds
    /// check, and after loadParameterImage(). Not called at start.
    void onParameterImageChanged(util::InplaceFunction<void(), 16> callback)
    {
        if (!_bau) {
            return;
        }
        _parameterImageChanged = std::move(callback);
        _bau->applicationProgram().setOnProgramDataChanged([this](std::span<const uint8_t>) {
            if (_parameterImageChanged) {
                _parameterImageChanged();
            }
        });
    }

    /// The product changed a parameter itself: have the image stored.
    void markParameterImageChanged()
    {
        if (_bau) {
            _bau->markParameterImageChanged();
        }
    }

    /// False while ETS has not completed a parameter download for this
    /// layout: the image then holds the defaults or half of a download.
    bool applicationProgramLoaded() const
    {
        return _bau && _bau->applicationProgram().loadState() == objects::loadstate::kLoaded;
    }

    /// For expert/test use: replace the whole parameter image the way a
    /// completed ETS download does. False when the image was refused.
    bool loadParameterImageForExpertUse(std::span<const uint8_t> image)
    {
        if (!_bau) {
            return false;
        }
        auto& program = _bau->applicationProgram();
        if (image.size() != program.parameterImage().size()) {
            return false;
        }
        program.setProgramData(image);
        if (!program.acceptParameterImage()) {
            return false;
        }
        _bau->markParameterImageChanged();
        program.notifyProgramDataChanged();
        return true;
    }

#if KNX_SECURE_ENABLED
    // -----------------------------------------------------------------------
    // Security passthrough helpers
    //
    // These thin wrappers keep the SecurityInterfaceObject internals out of the
    // commissioned_product.hpp layer. They act on the started device and
    // return OperationNotReady before start(); the configuration a device
    // starts with goes through setFactorySecurityDefaults() instead.
    // -----------------------------------------------------------------------

    /**
     * @brief The Security Mode and Tool Key this device has ex-factory.
     *
     * Applied when start() creates the BAU, before init(): they are the
     * factory default, and what init() restores from persistence (an ETS
     * download, or the device's own earlier state) takes precedence. A Factory
     * Reset returns to them. See the rule on bau::BusAccessUnit::init().
     *
     * Only before start(); AlreadyInitialized after it.
     */
    util::Result<void> setFactorySecurityDefaults(bool securityModeEnabled,
                                                  std::optional<std::array<uint8_t, 16>> toolKey)
    {
        if (_bau) {
            return util::ErrorCode::AlreadyInitialized;
        }
        _factorySecurityModeEnabled = securityModeEnabled;
        _factoryToolKey = toolKey;
        return util::Result<void>::ok();
    }

    /// Enable KNX Data Secure mode on the started device's Security Interface Object.
    util::Result<void> enableSecurityMode()
    {
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        _bau->securityObject().setSecurityMode(objects::SecurityMode::Enabled);
        return util::Result<void>::ok();
    }

    /// Put @p key in force as the started device's Tool Key.
    util::Result<void> applyToolKey(const std::array<uint8_t, 16>& key)
    {
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        _bau->securityObject().setToolKey(key);
        return util::Result<void>::ok();
    }

    /// Set a firmware-provided group key on the started device; see
    /// objects::SecurityInterfaceObject::setGroupKey for when it is in force.
    util::Result<void> applyGroupKey(GroupAddress address, const std::array<uint8_t, 16>& key)
    {
        if (!_bau) {
            return util::ErrorCode::OperationNotReady;
        }
        _bau->securityObject().setGroupKey(address, key);
        return util::Result<void>::ok();
    }

    /// Returns true if the ETS tool key has been set (non-zero).
    bool hasToolKey() const
    {
        return _bau && _bau->securityObject().hasToolKey();
    }

    /// Returns true if the Tool Key currently in force is @p key.
    ///
    /// Exists so firmware can tell whether the factory key it holds is the one
    /// the device would actually accept, without a getter that hands the key
    /// itself back out. hasToolKey() only says "some key is set", which cannot
    /// distinguish the factory key from one ETS installed -- and a device
    /// certificate printed for the wrong one sends an installer chasing an
    /// "unknown key" error that no amount of re-entering will fix.
    bool toolKeyMatches(const std::array<uint8_t, 16>& key) const
    {
        if (!_bau) {
            return false;
        }
        const auto& inForce = _bau->securityObject().getToolKey();
        return std::equal(inForce.begin(), inForce.end(), key.begin());
    }

    /// Returns true if KNX Data Secure mode is currently enabled.
    bool isSecurityEnabled() const
    {
        return _bau && _bau->securityObject().isSecurityEnabled();
    }
#endif  // KNX_SECURE_ENABLED

    util::Result<void> finishStart(const platform::Platform& platform)
    {
        _core.attach(_bau.get());

        // The product's APDU length is reported to ETS and written to the
        // .knxprod; the buffers behind it are sized by CONFIG_KNX_MAX_APDU_LENGTH.
        // A constexpr definition cannot get here (makeEndpointDefinition()
        // rejects it at compile time); this catches one built at run time.
        if (_definition.identity.maxApduLength > config::MAX_APDU_LENGTH) {
            KNX_LOGE("KNX.Product", "Product maxApduLength %u exceeds CONFIG_KNX_MAX_APDU_LENGTH %u",
                     static_cast<unsigned>(_definition.identity.maxApduLength),
                     static_cast<unsigned>(config::MAX_APDU_LENGTH));
            dropBau();
            return util::ErrorCode::InvalidParameter;
        }
        applyIdentity(platform);
        applyPreStartConfiguration();

        // Apply any transmit-shaping configured before start() (the BAU only
        // exists now). The time source also drives cyclic sends via BAU loop().
        if (_timeSource) {
            _bau->setTimeSource(_timeSource);
        }
        _bau->setTelegramRateLimit(_telegramRateLimit);

        const auto registerObjects = _core.registerGroupObjects();
        if (registerObjects.isError()) {
            reportFault({FaultCode::InternalError, "EndpointRuntime slot registration failed"});
            dropBau();
            return registerObjects.error();
        }

        _core.wireCallbacks();
        _bau->deviceObject().setInternalProgModeCallback([this](Toggle mode) {
            _programmingModeActive = (mode == Toggle::Enable);
            _bindings.notifyProgrammingModeChanged(_programmingModeActive);
        });

        if (_workAvailableCallback) {
            _bau->setWorkAvailableCallback([this]() { notifyWorkAvailableIfTransitioned(); });
        }

        const auto init = _bau->init(_config.defaultIndividualAddress,
                                     _config.persistenceNamespace,
                                     _config.persistenceSchemaVersion);
        if (init.isError()) {
            reportFault({FaultCode::StartFailed, "EndpointRuntime start failed"});
            dropBau();
            return init.error();
        }

        const auto bindConfigured = _core.applyConfiguredGroupAddresses();
        if (bindConfigured.isError()) {
            reportFault({FaultCode::StartFailed, "EndpointRuntime address binding failed"});
            _bau->close();
            dropBau();
            return bindConfigured.error();
        }

        _core.seedReadableState();
        _core.drainPendingActions();
        refreshWorkAvailabilityState();
        return util::Result<void>::ok();
    }

    /// The BAU goes away: the core must stop pointing at it first.
    void dropBau()
    {
        _core.attach(nullptr);
        _bau.reset();
    }

    void applyIdentity(const platform::Platform& platform)
    {
        // Declared before anything else touches the device object: ETS reads
        // the Device Descriptor before it will download, and the mask version
        // it expects depends on the medium (07B0h on TP1, 57B0h on KNX IP).
        // The product already states which; nothing used to act on it.
        _bau->management().setDeviceMedium(knxMediumType(_definition.identity.medium));

        auto& device = _bau->deviceObject();
        device.setManufacturerId(_definition.identity.manufacturerId);
        device.setFirmwareRevision(_definition.identity.firmwareRevision);
        device.setMaxApduLength(_definition.identity.maxApduLength);

        // PID_HARDWARE_TYPE (03/05/01 §4.3.28): MSB must be 00h; the remaining
        // five octets are the manufacturer's hardware identifier. Encoded here
        // as manufacturer id + hardware serial so it is unique per product and
        // matches Hardware/@SerialNumber in the exported knxprod.
        const uint16_t manufacturerValue = _definition.identity.manufacturerId.value();
        const uint16_t hardwareSerial = _definition.identity.hardwareSerialNumber;
        const uint8_t hardwareType[6] = {
            0x00,
            static_cast<uint8_t>((manufacturerValue >> 8) & 0xFFu),
            static_cast<uint8_t>(manufacturerValue & 0xFFu),
            _definition.identity.hardwareVersion,
            static_cast<uint8_t>((hardwareSerial >> 8) & 0xFFu),
            static_cast<uint8_t>(hardwareSerial & 0xFFu),
        };
        device.setHardwareType(hardwareType);

        // PID_SERIAL_NUMBER (03/05/01 §4.3.11): manufacturer id, then the
        // platform's unique 32-bit number. Before init(), so serial-number
        // addressing and Data Secure start from it. A platform with no unique
        // number (0) leaves the serial at zero; the host may still set one
        // through deviceObject() after start().
        if (const uint32_t unique = platform.uniqueSerialNumber(); unique != 0u) {
            const uint8_t serial[6] = {
                static_cast<uint8_t>((manufacturerValue >> 8) & 0xFFu),
                static_cast<uint8_t>(manufacturerValue & 0xFFu),
                static_cast<uint8_t>((unique >> 24) & 0xFFu),
                static_cast<uint8_t>((unique >> 16) & 0xFFu),
                static_cast<uint8_t>((unique >> 8) & 0xFFu),
                static_cast<uint8_t>(unique & 0xFFu),
            };
            device.setSerialNumber(serial);
        }

        device.setOrderInfo(std::string(_definition.identity.orderNumber));

        // PID_VERSION as DPT_Version: magic 0 (no versioning scheme change),
        // version = hardware version, revision = firmware revision.
        device.setVersion(0,
                          _definition.identity.hardwareVersion,
                          _definition.identity.firmwareRevision);

        auto& program = _bau->applicationProgram();
        program.setApplicationManufacturer(_definition.identity.manufacturerId.value());
        program.setApplicationNumber(_definition.identity.applicationNumber);
        program.setApplicationVersion(static_cast<uint8_t>(_definition.identity.applicationVersion & 0xFFu));
        // PID_PROGRAM_VERSION (PDT_GENERIC_05): manufacturer id, application
        // number, version — ETS overwrites this block during the download.
        const uint16_t manufacturer = _definition.identity.manufacturerId.value();
        const uint16_t appNumber = _definition.identity.applicationNumber;
        const uint8_t programVersionBlock[objects::ApplicationProgramObject::kProgramVersionSize] = {
            static_cast<uint8_t>((manufacturer >> 8) & 0xFFu),
            static_cast<uint8_t>(manufacturer & 0xFFu),
            static_cast<uint8_t>((appNumber >> 8) & 0xFFu),
            static_cast<uint8_t>(appNumber & 0xFFu),
            static_cast<uint8_t>(_definition.identity.applicationVersion & 0xFFu),
        };
        program.setProgramVersionBlock(programVersionBlock);
    }

    // What was configured on the runtime before the BAU existed. Applied
    // before init(): the security values are factory defaults, which init()'s
    // restore takes precedence over.
    void applyPreStartConfiguration()
    {
        if (_parameterImage.defaults != nullptr) {
            _bau->applicationProgram().bindParameterImage(_parameterImage);
        }
#if KNX_SECURE_ENABLED
        if (_factorySecurityModeEnabled) {
            _bau->securityObject().setSecurityMode(objects::SecurityMode::Enabled);
        }
        if (_factoryToolKey.has_value()) {
            _bau->securityObject().setToolKey(*_factoryToolKey);
        }
#endif
        if (_programmingModeActive) {
            (void)_bau->management().setCommissioningMode(true);
        }
#if KNX_FEATURE_NETIP
        if (_pendingBackboneKeyCallback) {
            _bau->management().setBackboneKeyCallback(std::move(_pendingBackboneKeyCallback));
            _pendingBackboneKeyCallback = {};
        }
        if (_pendingRoutingMulticastAddressCallback) {
            _bau->management().setRoutingMulticastAddressCallback(
                std::move(_pendingRoutingMulticastAddressCallback));
            _pendingRoutingMulticastAddressCallback = {};
        }
#endif
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

    /// Send, or queue before start; wakes the owner when the queue gained an entry.
    util::Result<void> publishSlot(uint16_t slot, const application::DptValue& value)
    {
        const size_t queued = _core.pendingActionCount();
        const auto result = _core.publish(slot, value);
        if (_core.pendingActionCount() != queued) {
            notifyWorkAvailableIfTransitioned();
        }
        return result;
    }

    static constexpr const auto& portTable()
    {
        return kPortTable<definition_type>;
    }

    /// The slot of a port known at compile time: its index in the port table.
    template <auto LogicalId>
    static consteval uint16_t slotOfId()
    {
        return portSlotOf<definition_type, LogicalId>();
    }

    /// The slot of a port by run-time id (a scan of the table), or kNoSlot,
    /// which the core answers with InvalidParameter.
    static uint16_t slotOfLogicalId(port_id_type logicalId)
    {
        return findPortSlot(portTable().data(), portTable().size(), static_cast<uint16_t>(logicalId));
    }

    // Storage sized by the product; the core works on it by pointer, so these
    // are declared before it.
    definition_type _definition;
    bindings_type _bindings;
    EndpointInstanceConfig _config{};
    objects::ParameterImageBinding _parameterImage{};
    util::InplaceFunction<void(), 16> _parameterImageChanged{};
    std::array<GroupAddress, definition_type::kPortCount> _configuredGroupAddresses{};
    std::array<PendingBusAction<port_id_type>, kPendingActionCapacity> _pendingActions{};
    ProductPortCore _core;
    bool _programmingModeActive{false};
    WorkAvailableCallback _workAvailableCallback{};
    bool _hadImmediateWork{false};
    bau::BusAccessUnit::TimeSourceFn _timeSource{};
    application::TelegramRateLimitConfig _telegramRateLimit{};
#if KNX_SECURE_ENABLED
    bool _factorySecurityModeEnabled{false};
    std::optional<std::array<uint8_t, 16>> _factoryToolKey{};
#endif
#if KNX_FEATURE_NETIP
    bau::ManagementControl::BackboneKeyCallback _pendingBackboneKeyCallback{};
    bau::ManagementControl::RoutingMulticastAddressCallback _pendingRoutingMulticastAddressCallback{};
#endif
    std::unique_ptr<bau::BusAccessUnit> _bau;
};

} // namespace knx::product