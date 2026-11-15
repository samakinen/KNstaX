// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau.hpp
 * @brief Base Bus Access Unit (BAU)
 * 
 * The BAU wires together all KNX protocol layers and manages device behavior.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/objects/device_object.hpp"
#include "knx/objects/address_table_object.hpp"
#include "knx/objects/association_table_object.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/group_object_table_object.hpp"
#include "knx/config.hpp"
#if KNX_SECURE_ENABLED
#include "knx/objects/security_interface_object.hpp"
#endif
#if KNX_FEATURE_NETIP
#include "knx/netip/netip_parameter_object.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#endif
#if KNX_IP_TUNNELING
#include "knx/physical/ip_tunneling_physical.hpp"
#endif
#if KNX_SECURE_IP_TUNNELING
#include "knx/physical/ip_secure_tunneling_physical.hpp"
#endif
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/platform/platform.hpp"
#include "knx/application/group_object.hpp"
#include "knx/application/telegram_rate_limiter.hpp"
#include "knx/application/application_layer.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include <concepts>
#include <array>
#include <cstddef>
#include <span>
#include <memory>
#include <functional>
#include <optional>
#include <vector>

namespace knx {
namespace platform {
class Queue;
}

namespace testing {
class MockTp1Physical;
}

namespace network {
class FilterTable;
class FilterTableLoadControl;
class CouplerRoutingPolicy;
class TwoPortCoupler;
}

namespace bau {

class BusAccessStackPort;

// The three components the BAU is composed of (private headers in src/bau/).
class DeviceModel;
class GroupRuntime;
class DeviceManagement;

// Facet views over those components; defined after BusAccessUnit.
class TransmissionControl;
class ConstTransmissionControl;
class ManagementControl;
class ConstManagementControl;
class LinkAccess;
class ConstLinkAccess;

/**
 * @brief Callback for group value write routed to a specific communication object.
 *
 * This is the preferred callback for ETS-commissioned devices.  The
 * GroupObjectIndex identifies the communication object declared by the
 * firmware; the data is the raw APDU payload.
 */
using GroupObjectWriteCallback = util::InplaceFunction<void(GroupObjectIndex, std::span<const uint8_t>), ::knx::config::callbackBytes(64)>;

/**
 * @brief Callback for group value read routed to a specific communication object.
 */
using GroupObjectReadCallback = util::InplaceFunction<void(GroupObjectIndex), ::knx::config::callbackBytes(32)>;

/**
 * @brief A snapshot of what the lower stack has seen arrive
 *
 * Deliberately one struct spanning two layers. The question it answers —
 * "is anything reaching this device at all?" — cannot be answered from either
 * layer alone: the medium counts datagrams, the data link counts the frames it
 * recognised out of them, and a device that is being addressed but not answering
 * looks entirely different depending on which of the two has stopped moving.
 */
struct LinkDiagnostics {
    /// Data link: frames addressed to this device and delivered upward.
    uint32_t rxFrames{0};
    /// Data link: frames that arrived and were not for this device. Movement
    /// here with none in rxFrames means the bus is live and the address filter
    /// is not matching.
    uint32_t rxFilterDropped{0};
    /// Data link: frames lost because the frame pool was exhausted.
    uint32_t rxPoolDropped{0};
    /// Data link: frames that failed to decode.
    uint32_t rxDecodeFailed{0};

    /// Medium: datagrams read off the socket (KNXnet/IP ports only).
    uint64_t mediumDatagramsReceived{0};
    /// Medium: datagrams discarded because the inbound queue was full.
    uint64_t mediumDatagramsDropped{0};
    /// Medium: multicast group memberships re-issued after a long silence.
    uint64_t multicastRefreshes{0};
    /// Medium: milliseconds since the last datagram arrived. Grows without
    /// bound on a device the group is no longer being delivered to.
    uint32_t msSinceLastInbound{0};
};

/**
 * @brief Base Bus Access Unit
 * 
 * Manages protocol stack and device objects.
 *
 * @thread_safety Most methods are NOT thread-safe. Treat the BAU and its
 * group objects as single-owner runtime state. Call `loop()` and other public
 * BAU APIs from one owner context. Inbound group services from lower layers are
 * marshaled onto `loop()` context before they mutate group objects or invoke
 * group-object callbacks.
 */
class BusAccessUnit {
public:
    struct OwnerWorkHint {
        size_t pendingLoopWorkItems{0};
        size_t pendingDeferredWorkItems{0};
        std::optional<uint32_t> maxSleepMs{};

        bool hasImmediateWork() const {
            return pendingLoopWorkItems > 0u || pendingDeferredWorkItems > 0u;
        }

        bool shouldCallLoop() const {
            return pendingLoopWorkItems > 0u;
        }
    };

    using WorkAvailableCallback = util::InplaceFunction<void(), ::knx::config::callbackBytes(32)>;

    using TransmissionProgressState = util::OperationProgressState;

    enum class MessageKind : uint8_t {
        Unknown = 0,
        GroupValueWrite,
        GroupValueRead,
        GroupValueResponse,
    };

    struct TransmissionOutcome {
        GroupAddress destination;
        MessageKind kind{MessageKind::Unknown};
        AddressType destinationType;
        util::ErrorCode result;
        uint8_t attempts{0};
    };

    struct TransmissionOptions {
        uint8_t maxAttempts{1};
        bool retryOnBusy{true};
        bool retryOnTimeout{true};
        bool retryOnTransmissionFailed{false};
    };

    enum class AutoResponseMode : uint8_t {
        Immediate = 0,
        Deferred,
    };

    explicit BusAccessUnit(platform::Platform& platform,
                           std::unique_ptr<physical::Tp1MediumBackend> mediumBackend);
    explicit BusAccessUnit(platform::Platform& platform,
                           std::unique_ptr<BusAccessStackPort> stackPort);
    virtual ~BusAccessUnit();

    // Neither copyable nor movable: callbacks capturing
    // `this` are installed into the stack port and the interface objects.
    BusAccessUnit(const BusAccessUnit&) = delete;
    BusAccessUnit& operator=(const BusAccessUnit&) = delete;
    BusAccessUnit(BusAccessUnit&&) = delete;
    BusAccessUnit& operator=(BusAccessUnit&&) = delete;

    // Initialization
    /**
     * @brief Start the stack and restore the commissioned state.
     *
     * **Configuration before init() — the rule.** Whatever the firmware
     * configures before init() is the device's *factory default*. init() keeps
     * it, and state restored from persistence takes precedence over it where
     * there is any: an ETS download, or what the device stored on an earlier
     * start. Nothing set before init() is dropped without one of the reasons
     * below, and a setter that cannot hold a value before init() returns an
     * error instead of succeeding into nothing. A Factory Reset (Master Reset
     * erase code 02h/07h) returns to the factory defaults.
     *
     * In detail:
     *  - Security Mode, Tool Key, key tables and every other persisted
     *    property (addresses, association table, parameters, ...): the
     *    restored value wins, property by property. An all-zero restored Tool
     *    Key counts as no key, so a factory Tool Key set before init() stays.
     *  - Group and device keys set with SecurityInterfaceObject::setGroupKey /
     *    setDeviceKey: in force while the downloaded PID_GRP_KEY_TABLE /
     *    PID_P2P_KEY_TABLE holds no entries; the downloaded table alone once it
     *    does. They are kept either way (see SecurityInterfaceObject).
     *  - Sequence numbers (Sequence Number Sending, Tool Access, Last Valid
     *    SeqNr per partner): the higher of the set and restored values.
     *  - Not persisted (callbacks, group objects and their values, transmit
     *    shaping, programming mode, device medium, router role, reference
     *    object types, promiscuous mode): kept as set.
     *  - The individual address is @p ownAddress; one set on the Device Object
     *    beforehand is replaced by it (logged). PID_DEVICE_CONTROL is cleared
     *    and the run state set to Running, as 03/05/01 requires at start-up.
     *  - Need the started stack, so refuse before init(): writeProperty /
     *    readProperty and ManagementControl::setReferenceObjectProperty /
     *    applyKnxNetIpConfiguration (NotInitialized), sends (NotInitialized),
     *    adoptRoutingMulticastAddress (nothing adopted, returns 0.0.0.0).
     *
     * @param ownAddress The factory-default individual address; a restored
     *        one takes precedence.
     * @param persistenceSchemaVersion Layout version of the commissioned state
     *        this firmware understands.  Persisted state written by a
     *        different version is discarded rather than reinterpreted: the
     *        parameter block and interface-object blobs are positional, so
     *        reading a stale layout mis-parameterises the device silently.
     *        Bump it whenever a parameter or port is added, removed or
     *        reordered.
     */
    virtual util::Result<void> init(const IndividualAddress& ownAddress,
                                      std::string_view persistenceNamespace = {},
                                      uint16_t persistenceSchemaVersion = 1);
    virtual void close();

    /**
     * @brief Progress deferred stack work and deliver queued inbound group events
     * @thread_safety NOT thread-safe - call from the BAU owner context only
     */
    virtual void loop();

    /**
     * @brief Write any coalesced persistent state to storage immediately
     *
     * Property writes mark state dirty and let loop() batch the NVS flush.
     * Call this from paths that must not defer — notably just before a
     * restart, which would otherwise drop the pending burst.
     * @thread_safety NOT thread-safe - call from the BAU owner context only
     */
    void flushPendingPersistence();

    /**
     * @brief Report currently pending owner-context work relevant to low-power scheduling.
     *
     * `pendingLoopWorkItems` are drained by `loop()`. `pendingDeferredWorkItems`
     * are owner-context items available through other BAU APIs, such as deferred
     * automatic responses.
     */
    OwnerWorkHint ownerWorkHint() const;

    /**
     * @brief Register an idle-to-work transition notifier.
     *
     * The callback is invoked when the BAU transitions from no immediate
     * owner-context work to some immediate owner-context work. It may run from a
     * lower async context and must remain non-blocking.
     */
    void setWorkAvailableCallback(WorkAvailableCallback callback);

    // Device object access
    objects::DeviceObject& deviceObject() { return _deviceObject; }
    const objects::DeviceObject& deviceObject() const { return _deviceObject; }

#if KNX_SECURE_ENABLED
    // Security interface object access (for provisioning/tests)
    objects::SecurityInterfaceObject& securityObject() { return _securityObject; }
    const objects::SecurityInterfaceObject& securityObject() const { return _securityObject; }
#endif

    // Application program object access
    objects::ApplicationProgramObject& applicationProgram() { return _applicationProgram; }
    const objects::ApplicationProgramObject& applicationProgram() const { return _applicationProgram; }

    // Address table
    objects::AddressTableObject& addressTable() { return _addressTable; }
    const objects::AddressTableObject& addressTable() const { return _addressTable; }

    // Group object table
    objects::GroupObjectTableObject& groupObjectTable() { return _groupObjectTable; }
    const objects::GroupObjectTableObject& groupObjectTable() const { return _groupObjectTable; }

    // Group object by index
    application::GroupObject* groupObject(GroupObjectIndex index);
    const application::GroupObject* groupObject(GroupObjectIndex index) const;

    // Add group object (returns index).
    //
    // The flags overload is the canonical one: it takes the full KNX flag set
    // (C/R/W/T/U/I + priority) so read-on-init and per-object priority can be
    // expressed.  The four-bool overload is a convenience for the common
    // R/W/T/U case and leaves Communication enabled, read-on-init off and
    // priority Low.
    GroupObjectIndex addGroupObject(const GroupAddress& address,
                                    application::DptId dpt,
                                    const application::GroupObjectFlags& flags);
    GroupObjectIndex addGroupObject(const GroupAddress& address,
                                    application::DptId dpt,
                                    bool readable,
                                    bool writable,
                                    bool transmit,
                                    bool receivable);
    util::Result<void> bindGroupObjectToAddress(GroupObjectIndex index,
                                                const GroupAddress& address);
    bool isGroupObjectValid(GroupObjectIndex index) const;

    /// Number of group addresses ETS associated with a communication object.
    size_t groupObjectAssociationCount(GroupObjectIndex index) const;

    /// True when the object has at least one associated group address. Sending
    /// on an unlinked object succeeds as a no-op, so use this to tell "sent"
    /// from "nothing to send" (e.g. to report unused datapoints at startup).
    bool isGroupObjectLinked(GroupObjectIndex index) const;
    util::Result<application::DptValue> groupObjectValue(GroupObjectIndex index) const;
    util::Result<void> setGroupObjectValue(GroupObjectIndex index, const application::DptValue& value);
    util::Result<void> requestGroupObjectValue(GroupObjectIndex index);

    // Callbacks (communication-object-index-based, preferred for ETS-commissioned devices)
    // Invoked from loop() owner context after the inbound event has been marshaled
    // out of the lower-layer callback thread.
    void setGroupObjectWriteCallback(GroupObjectWriteCallback callback);
    void setGroupObjectReadCallback(GroupObjectReadCallback callback);

    // Send group value
    util::Result<void> sendGroupValue(const GroupAddress& address, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValue(const GroupAddress& address, std::span<const uint8_t> data);

    /// Send a group value write on all group addresses associated with a
    /// communication object index.  This is the preferred send path for
    /// ETS-commissioned devices where group addresses are not known at
    /// compile time.  An object the project left unlinked has no associated
    /// address: nothing goes on the bus and the call succeeds (see
    /// isGroupObjectLinked).
    util::Result<void> sendGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data);

    /// Send a group value response on all group addresses associated with a
    /// communication object index.  Unlinked objects are a no-op, as above.
    util::Result<void> respondGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data);

    // ── Outbound transmit shaping (send-on-change / cyclic / rate limit) ──────
    // Monotonic millisecond time source used to drive per-object cyclic sends,
    // min-interval floors, and the telegram rate limiter. Without it (default),
    // those time-based behaviours are inert and every publish is sent at once.
    using TimeSourceFn = util::InplaceFunction<uint32_t(), ::knx::config::callbackBytes(32)>;
    void setTimeSource(TimeSourceFn timeSource);

    /// Configure the global outbound-telegram rate limiter. Shapes only the
    /// device's own unsolicited group sends; read responses are never throttled.
    void setTelegramRateLimit(const application::TelegramRateLimitConfig& config);

    /// Set the send-on-change / cyclic / min-interval policy for one object.
    util::Result<void> setGroupObjectTransmitPolicy(GroupObjectIndex objectIndex,
                                                    const application::GroupObjectTransmitPolicy& policy);

    /// Read back the transmit policy currently applied to one object.
    util::Result<application::GroupObjectTransmitPolicy>
    groupObjectTransmitPolicy(GroupObjectIndex objectIndex) const;

    // Request group value (A_GroupValue_Read)
    util::Result<void> requestGroupValue(const GroupAddress& address);
    util::Result<void> beginRequestGroupValue(const GroupAddress& address);
    util::Result<TransmissionProgressState> pollOutboundTransmission();
    util::Result<size_t> readProperty(InterfaceObjectType objectType,
                                      InterfaceObjectInstance objectInstance,
                                      application::PropertyID propertyId,
                                      uint16_t startIndex,
                                      uint8_t elementCount,
                                      std::span<uint8_t> out);
    util::Result<void> writeProperty(InterfaceObjectType objectType,
                                     InterfaceObjectInstance objectInstance,
                                     application::PropertyID propertyId,
                                     uint16_t startIndex,
                                     std::span<const uint8_t> data);
    // ── Grouped surfaces ─────────────────────────────────────────────────────
    //
    // Outbound shaping, commissioning/management and raw link access are three
    // separable concerns that together accounted for over a third of this
    // class's public methods. They are reached through the facets below rather
    // than flattened onto the BAU, so the core surface stays about the device
    // and its group objects.
    //
    // The facets are non-owning views over the BAU's components — a reference
    // and nothing else. They must not outlive the BAU.

    TransmissionControl transmission();
    ConstTransmissionControl transmission() const;

    ManagementControl management();
    ConstManagementControl management() const;

    /// Raw data-link access. Frame injection and promiscuous mode bypass the
    /// address filtering and the group-object runtime entirely, so this is for
    /// tests, sniffers and bridges — not for device firmware.
    LinkAccess link();
    ConstLinkAccess link() const;

#if KNX_FEATURE_COUPLER
    /**
     * @brief Declares this device a line/backbone coupler and publishes the
     *        Router Object (Interface Object Type 6).
     *
     * Opt-in on purpose. A plain end device that advertised a Router Object
     * would be telling ETS it performs routing it does not perform, and the
     * integrator would configure a filter table that nothing consults. Call
     * this only from a runtime that actually forwards frames between two
     * ports.
     *
     * The supplied filter table becomes the live target of
     * PID_ROUTETABLE_CONTROL, so an ETS filter-table download takes effect on
     * the same table the coupler consults. It must outlive the BAU.
     *
     * Call before init(); the object is registered and seeded during init().
     */
    struct RouterRoleConfig {
        /// The coupler's own filter table. Required — without it the Router
        /// Object would be configurable but inert.
        network::FilterTable* filterTable{nullptr};

        /// PID_MEDIUM: the medium on the secondary (sub-line) side.
        /// 0 = TP1, 1 = PL110, 2 = RF, 5 = KNXnet/IP (03/05/01 §4.2.x).
        uint8_t subMedium{0};

        /// PID_MAX_APDU_LENGTH the coupler can forward.
        uint16_t maxRoutedApduLength{254};

        /// PID_HOP_COUNT applied to forwarded frames.
        uint8_t hopCount{6};

        /// PID_L2_COUPLER_TYPE bit 0: 1 = the coupler supports the "coupler
        /// mode" behaviour of PID_MAIN_LCCONFIG's PHYS_ROUT setting.
        uint8_t l2CouplerType{0x01};

        /// PID_FILTER_TABLE_USE: whether the filter table is consulted at all.
        bool filterTableInUse{true};

        /**
         * The live routing policy, if the coupler has one.
         *
         * When set, PID_MAIN_LCCONFIG / PID_SUB_LCCONFIG /
         * PID_MAIN_LCGRPCONFIG / PID_SUB_LCGRPCONFIG are seeded *from* the
         * policy rather than from fixed defaults, and
         * @ref syncRouterRoutingConfig() reads them back into it after a
         * download. Without it those four properties are readable and writable
         * but change nothing, which is the state ETS cannot detect and the
         * integrator cannot debug.
         */
        network::CouplerRoutingPolicy* routingPolicy{nullptr};
    };
#endif  // KNX_FEATURE_COUPLER

private:
    platform::Platform& _platform;

    // The system interface objects (six with KNX Secure, five without: the
    // Security Interface Object is absent when it is off). Owned here so the accessors above can
    // hand them out; the components hold references.
    objects::DeviceObject _deviceObject;
    objects::AddressTableObject _addressTable;
    objects::AssociationTableObject _associationTable;
    objects::ApplicationProgramObject _applicationProgram;
    objects::GroupObjectTableObject _groupObjectTable;
#if KNX_SECURE_ENABLED
    objects::SecurityInterfaceObject _securityObject;
#endif

    std::unique_ptr<BusAccessStackPort> _stackPort;
    // Constructed in this order, each from the ones before it; never null.
    std::unique_ptr<DeviceModel> _model;
    std::unique_ptr<GroupRuntime> _groups;
    std::unique_ptr<DeviceManagement> _management;

    bool _initialized{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Facets
//
// Each is a non-owning view holding a single reference to the component that
// implements its concern. They exist to group a concern, not to add a layer:
// every method forwards directly, and the const variants exist so a
// `const BusAccessUnit&` can still be inspected.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Outbound telegram shaping and delivery bookkeeping.
 *
 * Covers the retry/outcome queue and the automatic read-response queue. A
 * device that simply publishes values never needs any of it — the defaults
 * are inert.
 */
class TransmissionControl {
public:
    using ProgressState = BusAccessUnit::TransmissionProgressState;
    using Options = BusAccessUnit::TransmissionOptions;
    using Outcome = BusAccessUnit::TransmissionOutcome;
    using AutoResponseMode = BusAccessUnit::AutoResponseMode;

    explicit TransmissionControl(GroupRuntime& runtime) : _runtime(runtime) {}

    /// Default retry policy applied to sends that do not name their own.
    void setDefaultOptions(const Options& options);
    const Options& defaultOptions() const;

    /// Advance an in-flight send. Returns Pending until the medium reports.
    util::Result<ProgressState> poll();

    /// Drain one completed send outcome; false when the queue is empty.
    bool popOutcome(Outcome& outcome);
    size_t queuedOutcomeCount() const;

    /// Whether A_GroupValue_Read is answered inline or from the owner loop.
    void setAutoResponseMode(AutoResponseMode mode);
    AutoResponseMode autoResponseMode() const;
    util::Result<void> beginAutomaticResponse();
    util::Result<ProgressState> pollAutomaticResponse();
    size_t queuedAutomaticResponseCount() const;

    /// Drop counters. Non-zero means the device is losing traffic it was asked
    /// to handle — worth surfacing rather than leaving buried in the BAU.
    size_t droppedAutomaticResponseCount() const;
    size_t droppedInboundGroupEventCount() const;

private:
    GroupRuntime& _runtime;
};

/// Read-only view of TransmissionControl, for a const BusAccessUnit.
class ConstTransmissionControl {
public:
    using Options = BusAccessUnit::TransmissionOptions;
    using AutoResponseMode = BusAccessUnit::AutoResponseMode;

    explicit ConstTransmissionControl(const GroupRuntime& runtime) : _runtime(runtime) {}

    const Options& defaultOptions() const;
    size_t queuedOutcomeCount() const;
    AutoResponseMode autoResponseMode() const;
    size_t queuedAutomaticResponseCount() const;
    size_t droppedAutomaticResponseCount() const;
    size_t droppedInboundGroupEventCount() const;

private:
    const GroupRuntime& _runtime;
};

/**
 * @brief Commissioning state and device role.
 *
 * Programming mode, the coupler role, and which optional interface objects the
 * device publishes — everything a management client or an integrator changes,
 * as opposed to what the application does.
 */
class ManagementControl {
public:
#if KNX_FEATURE_COUPLER
    using RouterRoleConfig = BusAccessUnit::RouterRoleConfig;
#endif

    explicit ManagementControl(DeviceManagement& management) : _management(management) {}

    /**
     * Declare the medium this device sits on, so its Device Descriptor Type 0
     * (read by ETS before every download) reports the right mask version.
     * Not IP-specific: every device has a medium, and TP1 is only the default.
     * Call it before or just after start(), and only when not on TP1.
     */
    void setDeviceMedium(MediumType medium);

    /// Programming mode. ETS finds an unaddressed device only while this is on.
    util::Result<void> setCommissioningMode(bool enabled);
    bool commissioningModeEnabled() const;

#if KNX_FEATURE_COUPLER
    /// Declare this device a coupler and publish the Router Object. Opt-in:
    /// see BusAccessUnit::RouterRoleConfig for why it is not automatic.
    util::Result<void> configureRouterRole(const RouterRoleConfig& config);

    /**
     * Read the Router Object's coupler configuration back into the policy.
     *
     * ETS writes PID_*_LCCONFIG and PID_*_LCGRPCONFIG as ordinary properties,
     * which land in the interface object and nowhere else. Call this after a
     * download so the forwarding path starts obeying what was downloaded.
     * No-op when no routing policy was supplied.
     */
    util::Result<void> syncRouterRoutingConfig();
    bool hasRouterRole() const;

    /**
     * The Router Object's Filter Table load state machine (Realisation Type
     * 3, 03/05/01 §5.3.1): PID_LOAD_STATE_CONTROL, PID_TABLE_REFERENCE,
     * PID_MCB_TABLE and the memory window the table is written through.
     * Null until the Router Object is published.
     */
    network::FilterTableLoadControl* filterTableLoadControl();
#endif  // KNX_FEATURE_COUPLER

#if KNX_FEATURE_NETIP
    /**
     * Publish the device's live IP identity — address, multicast group, MAC,
     * friendly name — into the KNXnet/IP Parameter Object, where ETS reads it
     * back. None of these values exist at start-up and most change again when
     * a lease is renewed: call it once the interface is up, and again whenever
     * the address changes. See netip::KnxNetIpConfiguration for why leaving it
     * unset is worse than it looks.
     */
    util::Result<void> applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config);

    /**
     * Notified when ETS downloads PID_BACKBONE_KEY into the KNXnet/IP
     * Parameter Object.
     *
     * That property is how a project turns on KNX/IP Secure Routing: the
     * backbone key is written over a secured management connection and takes
     * effect immediately, without a restart. A device that stores it and does
     * nothing else keeps sending plain multicast into an installation whose
     * project says it is encrypted — the failure is silent on the device and
     * invisible in ETS, which is the worst combination.
     *
     * The key is 16 octets; an all-zero value means secure routing is off.
     */
    using BackboneKeyCallback = util::InplaceFunction<void(std::span<const uint8_t> key), ::knx::config::callbackBytes(64)>;
    void setBackboneKeyCallback(BackboneKeyCallback callback);

    /**
     * @brief Accept or refuse a security requirement on a KNXnet/IP service family
     *
     * 03/08/09 §2.3.1.5: ETS uses PID_SECURED_SERVICE_FAMILIES to say which of
     * Device Management (03h), Tunnelling (04h) and Routing (05h) must be
     * encrypted, with version 00h meaning plain is allowed and 01h meaning KNX
     * IP Secure is required.
     *
     * Only the code that implements a service family knows whether it can
     * encrypt it, so requiring security is refused outright unless this is set.
     * Recording the requirement and answering Success would let ETS show an
     * installation as secured while the frames went out in the clear.
     *
     * Return false to refuse; the client is told with E_ERROR.
     */
    using SecuredServiceFamilyCallback =
        util::InplaceFunction<bool(uint8_t serviceFamilyId, uint8_t securityVersion), ::knx::config::callbackBytes(64)>;
    void setSecuredServiceFamilyCallback(SecuredServiceFamilyCallback callback);

    /// The security version currently configured for a KNXnet/IP service
    /// family; 0 when plain communication is allowed. Unknown families read as 0.
    [[nodiscard]] uint8_t securedServiceFamilyVersion(uint8_t serviceFamilyId) const;

    /**
     * Settle the routing multicast group at start-up and return it.
     *
     * PID_ROUTING_MULTICAST_ADDRESS is ETS's to set (03_08_03 §2.5.17: it
     * "may be set at run-time. A changed value shall become active after a
     * reset"), so a value restored with the KNXnet/IP Parameter Object is the
     * one in force and `productDefault` only fills an object that holds none —
     * a device's first start, or the one after a Factory Reset (03/05/02
     * Table 6 resets the IP multicast address with Erase Code 02h).
     * `productDefaultWins` reverses that, for a product whose own
     * configuration channel has set the group since ETS last did; the default
     * is then stored as if written.
     *
     * Also sets the Domain Address, which on KNX IP plain is the routing
     * multicast address (03/03/07 §3.3.7). A restored value is not E11.
     */
    IpAddress adoptRoutingMulticastAddress(IpAddress productDefault, bool productDefaultWins);

    /// The group in PID_ROUTING_MULTICAST_ADDRESS: the one in force after
    /// adoptRoutingMulticastAddress(), or one written since and active after
    /// the next restart. 0.0.0.0 before adoption.
    [[nodiscard]] IpAddress routingMulticastAddress() const;

    /**
     * Notified when PID_ROUTING_MULTICAST_ADDRESS is set to a new and
     * different value, by property write or A_DomainAddressSerialNumber_Write
     * — 03_08_09 §2.2.2.3.2.5 E11. The same value written again is E12 and is
     * not reported. Runs on the stack task that handled the write.
     *
     * The new group is not to be joined here (§2.5.17 above): the receiver of
     * this is the multicast timer synchronisation, which E11 restarts (A7).
     */
    using RoutingMulticastAddressCallback = util::InplaceFunction<void(IpAddress address), ::knx::config::callbackBytes(32)>;
    void setRoutingMulticastAddressCallback(RoutingMulticastAddressCallback callback);
#endif

    /// Write a property of a reference interface object (Router, KNXnet/IP
    /// Parameter, ...). This is the same path ETS writes through, so it is also
    /// how firmware seeds values ETS will later read back.
    util::Result<void> setReferenceObjectProperty(InterfaceObjectType objectType,
                                                  application::PropertyID propertyId,
                                                  std::span<const uint8_t> value);

    /// Current value of a reference object property; empty if never written.
    std::vector<uint8_t> referenceObjectPropertyValue(
        InterfaceObjectType objectType, application::PropertyID propertyId) const;

    /// Optional standard interface objects this device exposes.
    void setReferenceInterfaceObjectTypes(std::vector<InterfaceObjectType> objectTypes);
    const std::vector<InterfaceObjectType>& referenceInterfaceObjectTypes() const;

private:
    DeviceManagement& _management;
};

/// Read-only view of ManagementControl, for a const BusAccessUnit.
class ConstManagementControl {
public:
    explicit ConstManagementControl(const DeviceManagement& management) : _management(management) {}

    bool commissioningModeEnabled() const;
#if KNX_FEATURE_COUPLER
    bool hasRouterRole() const;
#endif
    const std::vector<InterfaceObjectType>& referenceInterfaceObjectTypes() const;
#if KNX_FEATURE_NETIP
    [[nodiscard]] IpAddress routingMulticastAddress() const;
#endif

private:
    const DeviceManagement& _management;
};

/**
 * @brief Raw data-link access, below the group-object runtime.
 *
 * Frame injection and promiscuous mode bypass address filtering and the group
 * object table entirely. That is exactly what a sniffer, a bridge or an
 * on-wire test wants, and exactly what device firmware should not touch — the
 * separate facet is the warning label.
 */
class LinkAccess {
public:
    /// @p stackPort may be null (a BAU constructed without one).
    explicit LinkAccess(BusAccessStackPort* stackPort) : _stackPort(stackPort) {}

    bool hasStackPort() const { return _stackPort != nullptr; }
    util::Result<void> sendFrame(const datalink::LDataFrame& frame);
    void setPromiscuousMode(datalink::PromiscuousMode mode);

#if KNX_FEATURE_COUPLER
    /// The coupler, when this device's stack port was built as one; nullptr
    /// otherwise. Gives access to the filter table and the routing policy.
    network::TwoPortCoupler* coupler();
#endif

    /// Re-issue the routing group membership, for a stack port that has one.
    /// OperationNotSupported on any other medium.
    util::Result<void> refreshMulticastMembership();

    /// Counters for the port. See LinkDiagnostics for what they distinguish.
    LinkDiagnostics diagnostics() const;

private:
    BusAccessStackPort* _stackPort;
};

/// Read-only view of LinkAccess, for a const BusAccessUnit.
class ConstLinkAccess {
public:
    explicit ConstLinkAccess(const BusAccessStackPort* stackPort) : _stackPort(stackPort) {}
    bool hasStackPort() const { return _stackPort != nullptr; }

private:
    const BusAccessStackPort* _stackPort;
};

#if KNX_FEATURE_TP1
// TP1 entry points. Gated for the mirror of the reason the IP ones are: an
// IP-only firmware compiles no TP1 physical, so declaring these would leave
// bau.hpp promising symbols the library does not contain.
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::Tp1MacPhysical> physicalLayer);

#if KNX_FEATURE_COUPLER
/**
 * @brief A stack port that is also a KNX coupler.
 *
 * The device's own stack runs on @p primaryPhysical exactly as it does for an
 * end device — same application layer, same interface objects, same ETS
 * commissioning. On top of that, frames are routed between the two ports
 * according to 03/03/03 §2.4.2.4.
 *
 * @param primaryPhysical   the upstream side (main line, or backbone for a
 *                          backbone coupler)
 * @param secondaryPhysical the downstream subnetwork
 *
 * The coupler role is derived from the individual address ETS assigns, so an
 * uncommissioned coupler behaves as a repeater until it has one. Reach the
 * filter table and routing policy through `BusAccessStackPort::coupler()`
 * (knx/bau/stack_port.hpp).
 */
std::unique_ptr<BusAccessStackPort> createTp1CouplerStackPort(
    platform::Platform& platform,
    std::unique_ptr<physical::Tp1MacPhysical> primaryPhysical,
    std::unique_ptr<physical::Tp1MacPhysical> secondaryPhysical);
#endif  // KNX_FEATURE_COUPLER
#endif  // KNX_FEATURE_TP1

#if KNX_FEATURE_NETIP
// KNXnet/IP entry points.  Gated so a TP1-only firmware neither compiles nor
// links the IP stack: without this the tunnelling client, routing endpoint and
// their codecs stay reachable from bau.hpp and the linker cannot drop them.
// The tunnelling (client-role) overloads exist only with KNX_IP_TUNNELING.
#if KNX_IP_TUNNELING
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpTunnelingPhysical> physicalLayer);
#endif

#if KNX_SECURE_IP_TUNNELING
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpSecureTunnelingPhysical> physicalLayer);
#endif

std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpRoutingPhysical> physicalLayer);
#endif  // KNX_FEATURE_NETIP

#if KNX_FEATURE_TP1
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::Tp1MediumBackend> mediumBackend);
#endif

namespace detail {
std::unique_ptr<BusAccessStackPort> createTp1MockTestStackPort(
    platform::Platform& platform,
    std::unique_ptr<testing::MockTp1Physical> physicalLayer);
}

} // namespace bau
} // namespace knx

// The stack port's definition needs BusAccessUnit's types, so it comes last.
#include "knx/bau/stack_port.hpp"
