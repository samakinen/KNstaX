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
#include "knx/objects/security_interface_object.hpp"
#include "knx/config.hpp"
#if KNX_FEATURE_NETIP
#include "knx/netip/netip_parameter_object.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#include "knx/physical/ip_secure_tunneling_physical.hpp"
#include "knx/physical/ip_tunneling_physical.hpp"
#endif
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/platform/platform.hpp"
#include "knx/application/group_object.hpp"
#include "knx/application/telegram_rate_limiter.hpp"
#include "knx/application/application_layer.hpp"
#include "knx/util/inplace_function.hpp"
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
class CouplerRoutingPolicy;
class TwoPortCoupler;
}

namespace bau {

class PropertyAccessBridge;
class BusAccessStackPort;

// Facet views over BusAccessUnit; defined after it, since each holds a
// reference to one and calls through to it.
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
using GroupObjectWriteCallback = util::InplaceFunction<void(GroupObjectIndex, std::span<const uint8_t>), 64>;

/**
 * @brief Callback for group value read routed to a specific communication object.
 */
using GroupObjectReadCallback = util::InplaceFunction<void(GroupObjectIndex), 32>;

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

    using WorkAvailableCallback = util::InplaceFunction<void(), 32>;

    enum class TransmissionProgressState : uint8_t {
        Pending = 0,
        Success,
        Busy,
        TransmissionFailed,
        Timeout,
    };

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

    // Delete copy operations (owns unique resources)
    BusAccessUnit(const BusAccessUnit&) = delete;
    BusAccessUnit& operator=(const BusAccessUnit&) = delete;

    // Allow move operations
    BusAccessUnit(BusAccessUnit&&) = default;
    BusAccessUnit& operator=(BusAccessUnit&&) = default;

    // Initialization
    /// @param persistenceSchemaVersion Layout version of the commissioned state
    ///        this firmware understands.  Persisted state written by a
    ///        different version is discarded rather than reinterpreted: the
    ///        parameter block and interface-object blobs are positional, so
    ///        reading a stale layout mis-parameterises the device silently.
    ///        Bump it whenever a parameter or port is added, removed or
    ///        reordered.
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

    // Security interface object access (for provisioning/tests)
    objects::SecurityInterfaceObject& securityObject() { return _securityObject; }
    const objects::SecurityInterfaceObject& securityObject() const { return _securityObject; }

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
    using TimeSourceFn = util::InplaceFunction<uint32_t(), 32>;
    void setTimeSource(TimeSourceFn timeSource) { _timeSource = std::move(timeSource); }

    /// Configure the global outbound-telegram rate limiter. Shapes only the
    /// device's own unsolicited group sends; read responses are never throttled.
    void setTelegramRateLimit(const application::TelegramRateLimitConfig& config) {
        _txRateLimiter.configure(config);
    }

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
    // The facets are non-owning views over this BAU — a reference and nothing
    // else, so `bau.transmission().setDefaultTransmissionOptions(...)` costs
    // exactly what the direct call did. They must not outlive the BAU.

    TransmissionControl transmission();
    ConstTransmissionControl transmission() const;

    ManagementControl management();
    ConstManagementControl management() const;

    /// Raw data-link access. Frame injection and promiscuous mode bypass the
    /// address filtering and the group-object runtime entirely, so this is for
    /// tests, sniffers and bridges — not for device firmware.
    LinkAccess link();
    ConstLinkAccess link() const;

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

private:
    friend class TransmissionControl;
    friend class ConstTransmissionControl;
    friend class ManagementControl;
    friend class ConstManagementControl;
    friend class LinkAccess;
    friend class ConstLinkAccess;

    // Implementation of the facet surfaces. Private so the only way in is
    // through the facet that names the concern.
    util::Result<void> requestCommissioningMode(bool enabled);
    bool commissioningModeEnabled() const;
    void setDefaultTransmissionOptions(const TransmissionOptions& options);
    const TransmissionOptions& defaultTransmissionOptions() const;
    bool popOutboundTransmissionOutcome(TransmissionOutcome& outcome);
    size_t queuedOutboundTransmissionOutcomeCount() const;
    void setAutoResponseMode(AutoResponseMode mode) { _autoResponseMode = mode; }
    AutoResponseMode autoResponseMode() const { return _autoResponseMode; }
    size_t queuedAutomaticResponseCount() const { return _autoResponseQueueCount; }
    size_t droppedAutomaticResponseCount() const { return _droppedAutomaticResponses; }
    size_t droppedInboundGroupEventCount() const { return _droppedInboundGroupEvents; }
    util::Result<void> beginProcessAutomaticResponse();
    util::Result<TransmissionProgressState> pollProcessAutomaticResponse();

    bool hasStackPort() const;
    util::Result<void> sendDataLinkFrame(const datalink::LDataFrame& frame);
    void setDataLinkPromiscuousMode(datalink::PromiscuousMode mode);
    network::TwoPortCoupler* stackPortCoupler();
    util::Result<void> refreshStackPortMulticastMembership();
    LinkDiagnostics stackPortLinkDiagnostics() const;
    void setDomainAddress(std::span<const uint8_t> domainAddress);
    void setStackPortDomainAddressWriteCallback(application::DomainAddressWriteCallback callback);
    void setReferenceInterfaceObjectTypes(std::vector<InterfaceObjectType> objectTypes);
    const std::vector<InterfaceObjectType>& referenceInterfaceObjectTypes() const;
    util::Result<void> configureRouterRole(const RouterRoleConfig& config);
    bool hasRouterRole() const { return _routerRole.filterTable != nullptr; }

    /**
     * @brief Read the Router Object's coupler configuration back into the policy.
     *
     * ETS writes PID_*_LCCONFIG and PID_*_LCGRPCONFIG as ordinary properties,
     * which land in the interface object and nowhere else. Call this after a
     * download so the forwarding path starts obeying what was downloaded.
     * No-op when no routing policy was supplied.
     */
    util::Result<void> syncRouterRoutingConfig();

protected:
    platform::Platform& _platform;

    // Interface objects
    objects::DeviceObject _deviceObject;
    objects::AddressTableObject _addressTable;
    objects::AssociationTableObject _associationTable;
    objects::ApplicationProgramObject _applicationProgram;
    objects::GroupObjectTableObject _groupObjectTable;
    objects::SecurityInterfaceObject _securityObject;

    // Callbacks
    GroupObjectWriteCallback _groupObjectWriteCallback;
    GroupObjectReadCallback _groupObjectReadCallback;

    bool _initialized;

private:
    struct PendingAutoResponse {
        GroupAddress destination;
        std::vector<uint8_t> data;
    };

    struct AutoResponseOperationState {
        bool active{false};
    };

    static constexpr size_t AUTO_RESPONSE_QUEUE_CAPACITY = 4u;
    static constexpr size_t INBOUND_GROUP_EVENT_QUEUE_CAPACITY = 8u;

    // Application layer callback
    void handleApplicationFrame(const IndividualAddress& source,
                                const GroupAddress& destination,
                                MessageKind kind,
                                std::span<const uint8_t> data,
                                AddressType destinationType);
    bool enqueueInboundGroupEvent(MessageKind kind,
                                  const GroupAddress& destination,
                                  std::span<const uint8_t> data);
    void processInboundGroupEvents();
    void clearInboundGroupEvents();
    util::Result<void> sendAutomaticResponseImmediate(const GroupAddress& destination,
                                                      std::span<const uint8_t> data);
    void enqueueAutomaticResponse(const GroupAddress& destination, std::span<const uint8_t> data);
    bool popAutomaticResponse(PendingAutoResponse& response);
    size_t queuedInboundGroupEventCount() const;
    void refreshWorkAvailabilityState();
    void notifyWorkAvailableIfTransitioned();

    std::array<PendingAutoResponse, AUTO_RESPONSE_QUEUE_CAPACITY> _autoResponseQueue{};
    size_t _autoResponseQueueHead{0};
    size_t _autoResponseQueueCount{0};
    AutoResponseMode _autoResponseMode{AutoResponseMode::Immediate};
    AutoResponseOperationState _autoResponseOperation{};
    size_t _droppedAutomaticResponses{0};
    size_t _droppedInboundGroupEvents{0};
    WorkAvailableCallback _workAvailableCallback{};
    bool _hadImmediateWork{false};
    bool _restoringPersistentState{false};
    /// The address init() was given: the medium's default IA that Erase
    /// Codes 02h and 03h restore (03/05/02 Table 4).
    IndividualAddress _defaultIndividualAddress{0xFFFFu};
    /// Reset the Resources @p eraseCode names and persist the result; the
    /// restart that follows boots into it. See bau_management.cpp.
    util::Result<void> applyMasterReset(application::EraseCode eraseCode);
    std::unique_ptr<platform::Queue> _inboundGroupEventQueue;
    std::unique_ptr<BusAccessStackPort> _stackPort;
    std::unique_ptr<PropertyAccessBridge> _propertyAccessBridge;

    // Outbound transmit shaping.
    TimeSourceFn _timeSource{};
    application::TelegramRateLimiter _txRateLimiter{};

    uint32_t nowMs() const { return _timeSource ? _timeSource() : 0u; }
    /// Unconditional group-value write on all associated addresses (the raw send
    /// primitive behind sendGroupValueForObject, bypassing policy/rate gates).
    /// No associated address means no telegram and an ok result.
    util::Result<void> emitGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data);
    /// Release cyclic-due and deferred (min-interval/rate-limited) sends, honoring
    /// the rate limiter. Called from loop().
    void pumpGroupObjectTransmissions(uint32_t nowMs);

    // Value-Read-on-Initialisation (group object flag I).  Runs once after the
    // device reaches a valid individual address, spread across loop() calls so
    // a whole-installation restart does not turn into a read storm.
    enum class ReadOnInitState : uint8_t { Pending, Running, Done };
    static constexpr size_t kMaxReadOnInitPerLoop = 4u;
    ReadOnInitState _readOnInitState{ReadOnInitState::Pending};
    uint16_t _readOnInitCursor{0};
    void pumpReadOnInit();

    /// Interface object type ETS addresses at `objectIndex`, or nullopt when
    /// the index is out of range.  Defined in bau.cpp, where the property
    /// bridge that owns the mapping is a complete type.
    std::optional<InterfaceObjectType> interfaceObjectTypeForIndex(InterfaceObjectIndex objectIndex) const;

    /// Install handlers for the management services that are not property
    /// access (Function Property, serial-number commissioning), and publish
    /// the standard interface objects the active medium requires.
    /// Implemented in bau_management.cpp.
    void wireManagementServices();

    /// Register and seed the interface objects that ETS expects for the medium
    /// this device runs on (KNXnet/IP Parameter Object for IP devices, Router
    /// Object for couplers).  Implemented in bau_management.cpp.
    void publishMediumInterfaceObjects();

    /// Coupler role, if declared.  A null filterTable means "end device".
    RouterRoleConfig _routerRole{};
    /// Remembered so a stack port attached after the declaration still gets it.
    MediumType _deviceMedium{MediumType::TP1};

    /// Register and seed the Router Object; called from
    /// publishMediumInterfaceObjects() when a coupler role is declared.
    void publishRouterObject();

    /// Dispatch a Function Property invocation on the Router Object
    /// (PID_ROUTETABLE_CONTROL) to the coupler's filter table.
    util::Result<application::FunctionPropertyResult> handleKnxNetIpFunctionProperty(
        const IndividualAddress& source,
        const application::FunctionPropertyRequest& request);
    void setSecuredServiceFamilyVersion(uint8_t serviceFamilyId, uint8_t version);

    util::Result<application::FunctionPropertyResult> handleRouterFunctionProperty(
        const application::FunctionPropertyRequest& request);

    /// Dispatch a Function Property invocation on the Group Object Table Object
    /// (PID_GO_DIAGNOSTICS, 03/05/01 §4.8.1) — the service ETS uses to read or
    /// write a group address through the device, which is the only way it can
    /// touch a Data Secure group address it has no sending sequence number for.
    /// Implemented in bau_go_diagnostics.cpp.
    util::Result<application::FunctionPropertyResult> handleGroupObjectDiagnostics(
        const application::FunctionPropertyRequest& request);

    /// Rebuild the live Data Secure key maps from the array properties ETS
    /// downloads (PID_GRP_KEY_TABLE, PID_P2P_KEY_TABLE, and the Security
    /// Individual Address Table they index into).  Those properties are stored
    /// verbatim; the Secure Application Layer looks keys up by address, so
    /// without this step a downloaded key table has no effect at all.
    /// Idempotent — safe to call after every write that can change either the
    /// key tables or the group address table they resolve against.
    void applySecurityKeyTables();

    /// Validate the Flags + Group Address of the two PID_GO_DIAGNOSTICS
    /// commands that address the bus rather than a group object number.
    /// Returns Success, or the E_DATA_VOID the spec prescribes.
    application::FunctionPropertyReturnCode validateGroupDiagnosticsFlags(
        uint8_t flags, const GroupAddress& destination, bool allowSelector) const;

    /// Seed a live reference interface object property; no-op when the object
    /// is not registered.  Defined in bau.cpp where the bridge is complete.
    /// Current value of a reference object property, empty if never written.
    std::vector<uint8_t> referenceObjectPropertyValue(
        InterfaceObjectType objectType, application::PropertyID propertyId) const;
    /**
     * Declare the medium this device sits on.
     *
     * Reaches the Device Descriptor Type 0 that ETS reads before every
     * download; see BusAccessStackPort::setDeviceMedium. Call it before or
     * just after start(), and only when the device is not on TP1 — that is the
     * default, and re-declaring it changes nothing.
     */
    void setDeviceMedium(MediumType medium);

#if KNX_FEATURE_NETIP
    /**
     * Publish this device's live IP identity into the KNXnet/IP Parameter
     * Object, where ETS reads it back.
     *
     * Separate from publishMediumInterfaceObjects(), which runs at start-up:
     * none of these values exist yet at that point, and most of them change
     * again when a lease is renewed. Call it once the interface is up, and
     * again whenever the address changes.
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
    using BackboneKeyCallback = util::InplaceFunction<void(std::span<const uint8_t> key), 64>;
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
        util::InplaceFunction<bool(uint8_t serviceFamilyId, uint8_t securityVersion), 64>;
    void setSecuredServiceFamilyCallback(SecuredServiceFamilyCallback callback);

    /// The security version currently configured for a service family, 0 when
    /// plain communication is allowed. Unknown families read as 0.
    [[nodiscard]] uint8_t securedServiceFamilyVersion(uint8_t serviceFamilyId) const;

    BackboneKeyCallback _backboneKeyCallback{};
    SecuredServiceFamilyCallback _securedServiceFamilyCallback{};

    /// Indexed by the three securable families; see kSecurableServiceFamilies.
    std::array<uint8_t, 3> _securedServiceFamilyVersions{};
#endif

    util::Result<void> setReferenceObjectProperty(InterfaceObjectType objectType,
                                                  application::PropertyID propertyId,
                                                  std::span<const uint8_t> value);

    /// The live reference interface object of `objectType`, or nullptr when it
    /// is not published in this build. Defined in bau.cpp, where the property
    /// bridge is a complete type.
    objects::GenericInterfaceObject* referenceInterfaceObject(InterfaceObjectType objectType);
};

// ─────────────────────────────────────────────────────────────────────────────
// Facets
//
// Each is a non-owning view holding a single reference. They exist to group a
// concern, not to add a layer: every method forwards directly, and the const
// variants exist so a `const BusAccessUnit&` can still be inspected.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Outbound telegram shaping and delivery bookkeeping.
 *
 * Covers the time source that drives cyclic sends and rate limiting, the
 * retry/outcome queue, and the automatic read-response queue. A device that
 * simply publishes values never needs any of it — the defaults are inert.
 */
class TransmissionControl {
public:
    using ProgressState = BusAccessUnit::TransmissionProgressState;
    using Options = BusAccessUnit::TransmissionOptions;
    using Outcome = BusAccessUnit::TransmissionOutcome;
    using AutoResponseMode = BusAccessUnit::AutoResponseMode;

    explicit TransmissionControl(BusAccessUnit& bau) : _bau(bau) {}

    /// Default retry policy applied to sends that do not name their own.
    void setDefaultOptions(const Options& options) { _bau.setDefaultTransmissionOptions(options); }
    const Options& defaultOptions() const { return _bau.defaultTransmissionOptions(); }

    /// Advance an in-flight send. Returns Pending until the medium reports.
    util::Result<ProgressState> poll() { return _bau.pollOutboundTransmission(); }

    /// Drain one completed send outcome; false when the queue is empty.
    bool popOutcome(Outcome& outcome) { return _bau.popOutboundTransmissionOutcome(outcome); }
    size_t queuedOutcomeCount() const { return _bau.queuedOutboundTransmissionOutcomeCount(); }

    /// Whether A_GroupValue_Read is answered inline or from the owner loop.
    void setAutoResponseMode(AutoResponseMode mode) { _bau.setAutoResponseMode(mode); }
    AutoResponseMode autoResponseMode() const { return _bau.autoResponseMode(); }
    util::Result<void> beginAutomaticResponse() { return _bau.beginProcessAutomaticResponse(); }
    util::Result<ProgressState> pollAutomaticResponse() { return _bau.pollProcessAutomaticResponse(); }
    size_t queuedAutomaticResponseCount() const { return _bau.queuedAutomaticResponseCount(); }

    /// Drop counters. Non-zero means the device is losing traffic it was asked
    /// to handle — worth surfacing rather than leaving buried in the BAU.
    size_t droppedAutomaticResponseCount() const { return _bau.droppedAutomaticResponseCount(); }
    size_t droppedInboundGroupEventCount() const { return _bau.droppedInboundGroupEventCount(); }

private:
    BusAccessUnit& _bau;
};

/// Read-only view of TransmissionControl, for a const BusAccessUnit.
class ConstTransmissionControl {
public:
    using Options = BusAccessUnit::TransmissionOptions;
    using AutoResponseMode = BusAccessUnit::AutoResponseMode;

    explicit ConstTransmissionControl(const BusAccessUnit& bau) : _bau(bau) {}

    const Options& defaultOptions() const { return _bau.defaultTransmissionOptions(); }
    size_t queuedOutcomeCount() const { return _bau.queuedOutboundTransmissionOutcomeCount(); }
    AutoResponseMode autoResponseMode() const { return _bau.autoResponseMode(); }
    size_t queuedAutomaticResponseCount() const { return _bau.queuedAutomaticResponseCount(); }
    size_t droppedAutomaticResponseCount() const { return _bau.droppedAutomaticResponseCount(); }
    size_t droppedInboundGroupEventCount() const { return _bau.droppedInboundGroupEventCount(); }

private:
    const BusAccessUnit& _bau;
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
    using RouterRoleConfig = BusAccessUnit::RouterRoleConfig;

    explicit ManagementControl(BusAccessUnit& bau) : _bau(bau) {}

    /// Declare which medium this device is on, so its Device Descriptor
    /// reports the right mask version. See BusAccessUnit::setDeviceMedium.
    /// Not IP-specific: every device has a medium, and TP1 is only the default.
    void setDeviceMedium(MediumType medium) { _bau.setDeviceMedium(medium); }

    /// Programming mode. ETS finds an unaddressed device only while this is on.
    util::Result<void> setCommissioningMode(bool enabled) {
        return _bau.requestCommissioningMode(enabled);
    }
    bool commissioningModeEnabled() const { return _bau.commissioningModeEnabled(); }

    /// Declare this device a coupler and publish the Router Object. Opt-in:
    /// see BusAccessUnit::RouterRoleConfig for why it is not automatic.
    util::Result<void> configureRouterRole(const RouterRoleConfig& config) {
        return _bau.configureRouterRole(config);
    }

    /// Apply a downloaded coupler configuration to the live routing policy.
    util::Result<void> syncRouterRoutingConfig() { return _bau.syncRouterRoutingConfig(); }

#if KNX_FEATURE_NETIP
    /// Publish the device's live IP identity — address, multicast group, MAC,
    /// friendly name — into the KNXnet/IP Parameter Object. Call it once the
    /// interface is up and again whenever the address changes; see
    /// netip::KnxNetIpConfiguration for why leaving it unset is worse than it
    /// looks.
    util::Result<void> applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config) {
        return _bau.applyKnxNetIpConfiguration(config);
    }

    /// See BusAccessUnit::setBackboneKeyCallback. Named here so callers do not
    /// have to reach into the private section of the unit to spell the type.
    using BackboneKeyCallback = BusAccessUnit::BackboneKeyCallback;

    /// Observe ETS downloading the KNX/IP Secure Routing backbone key. See
    /// BusAccessUnit::setBackboneKeyCallback for why storing it is not enough.
    void setBackboneKeyCallback(BusAccessUnit::BackboneKeyCallback callback) {
        _bau.setBackboneKeyCallback(std::move(callback));
    }

    /// See BusAccessUnit::setSecuredServiceFamilyCallback.
    using SecuredServiceFamilyCallback = BusAccessUnit::SecuredServiceFamilyCallback;

    /// Decide whether this device can encrypt a KNXnet/IP service family ETS
    /// asks to secure. Without one, requiring security is refused — see
    /// BusAccessUnit::setSecuredServiceFamilyCallback for why that is the safe
    /// default rather than the unhelpful one.
    void setSecuredServiceFamilyCallback(BusAccessUnit::SecuredServiceFamilyCallback callback) {
        _bau.setSecuredServiceFamilyCallback(std::move(callback));
    }

    /// The security version currently configured for a KNXnet/IP service
    /// family; 0 when plain communication is allowed.
    [[nodiscard]] uint8_t securedServiceFamilyVersion(uint8_t serviceFamilyId) const {
        return _bau.securedServiceFamilyVersion(serviceFamilyId);
    }

    /// See application::DomainAddressWriteCallback.
    using DomainAddressWriteCallback = application::DomainAddressWriteCallback;

    /**
     * @brief Decide whether a Domain Address written by serial number is applied.
     *
     * On KNX IP the Domain Address is the routing multicast address, so
     * accepting one means re-joining a different group. Only the owner knows
     * whether it can do that, and returning an error refuses the change — which
     * is the honest answer for a device whose group is fixed at start-up.
     * Without a callback the write is refused.
     */
    void setDomainAddressWriteCallback(DomainAddressWriteCallback callback) {
        _bau.setStackPortDomainAddressWriteCallback(std::move(callback));
    }
#endif

    /// Write a property of a reference interface object (Router, KNXnet/IP
    /// Parameter, ...). This is the same path ETS writes through, so it is also
    /// how firmware seeds values ETS will later read back.
    util::Result<void> setReferenceObjectProperty(InterfaceObjectType objectType,
                                                  application::PropertyID propertyId,
                                                  std::span<const uint8_t> value) {
        return _bau.setReferenceObjectProperty(objectType, propertyId, value);
    }

    /// Current value of a reference object property; empty if never written.
    std::vector<uint8_t> referenceObjectPropertyValue(
        InterfaceObjectType objectType, application::PropertyID propertyId) const {
        return _bau.referenceObjectPropertyValue(objectType, propertyId);
    }
    bool hasRouterRole() const { return _bau.hasRouterRole(); }

    /// Optional standard interface objects this device exposes.
    void setReferenceInterfaceObjectTypes(std::vector<InterfaceObjectType> objectTypes) {
        _bau.setReferenceInterfaceObjectTypes(std::move(objectTypes));
    }
    const std::vector<InterfaceObjectType>& referenceInterfaceObjectTypes() const {
        return _bau.referenceInterfaceObjectTypes();
    }

private:
    BusAccessUnit& _bau;
};

/// Read-only view of ManagementControl, for a const BusAccessUnit.
class ConstManagementControl {
public:
    explicit ConstManagementControl(const BusAccessUnit& bau) : _bau(bau) {}

    bool commissioningModeEnabled() const { return _bau.commissioningModeEnabled(); }
    bool hasRouterRole() const { return _bau.hasRouterRole(); }
    const std::vector<InterfaceObjectType>& referenceInterfaceObjectTypes() const {
        return _bau.referenceInterfaceObjectTypes();
    }

private:
    const BusAccessUnit& _bau;
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
    explicit LinkAccess(BusAccessUnit& bau) : _bau(bau) {}

    bool hasStackPort() const { return _bau.hasStackPort(); }
    util::Result<void> sendFrame(const datalink::LDataFrame& frame) {
        return _bau.sendDataLinkFrame(frame);
    }
    void setPromiscuousMode(datalink::PromiscuousMode mode) {
        _bau.setDataLinkPromiscuousMode(mode);
    }

    /// The coupler, when this device's stack port was built as one; nullptr
    /// otherwise. Gives access to the filter table and the routing policy.
    network::TwoPortCoupler* coupler() { return _bau.stackPortCoupler(); }

    /// Re-issue the routing group membership, for a stack port that has one.
    /// OperationNotSupported on any other medium.
    util::Result<void> refreshMulticastMembership() {
        return _bau.refreshStackPortMulticastMembership();
    }

    /// Counters for the port. See LinkDiagnostics for what they distinguish.
    LinkDiagnostics diagnostics() const { return _bau.stackPortLinkDiagnostics(); }

private:
    BusAccessUnit& _bau;
};

/// Read-only view of LinkAccess, for a const BusAccessUnit.
class ConstLinkAccess {
public:
    explicit ConstLinkAccess(const BusAccessUnit& bau) : _bau(bau) {}
    bool hasStackPort() const { return _bau.hasStackPort(); }

private:
    const BusAccessUnit& _bau;
};

inline TransmissionControl BusAccessUnit::transmission() { return TransmissionControl(*this); }
inline ConstTransmissionControl BusAccessUnit::transmission() const {
    return ConstTransmissionControl(*this);
}
inline ManagementControl BusAccessUnit::management() { return ManagementControl(*this); }
inline ConstManagementControl BusAccessUnit::management() const {
    return ConstManagementControl(*this);
}
inline LinkAccess BusAccessUnit::link() { return LinkAccess(*this); }
inline ConstLinkAccess BusAccessUnit::link() const { return ConstLinkAccess(*this); }

class BusAccessStackPort {
public:
    using TransmissionProgressState = BusAccessUnit::TransmissionProgressState;
    using TransmissionOptions = BusAccessUnit::TransmissionOptions;
    using TransmissionOutcome = BusAccessUnit::TransmissionOutcome;
    using MessageKind = BusAccessUnit::MessageKind;
    using WorkAvailableCallback = BusAccessUnit::WorkAvailableCallback;
    /// Fires for every inbound group telegram, so it is stored inline: a
    /// std::function here put a heap-allocated indirect call on the busiest
    /// path in the stack.
    using InboundCallback = util::InplaceFunction<void(const IndividualAddress&,
                                                       const GroupAddress&,
                                                       MessageKind,
                                                       std::span<const uint8_t>,
                                                       AddressType), 32>;
    /// Init-time only, so the capacity can stay small.
    using PropertyRegistration = util::InplaceFunction<void(application::PropertyStore&), 32>;
    using PropertyReadProvider = application::PropertyReadProvider;
    using PropertyWriteProvider = application::PropertyWriteProvider;
    using PropertyDescriptionProvider = application::PropertyDescriptionProvider;

    virtual ~BusAccessStackPort() = default;

    virtual util::Result<void> init(const IndividualAddress& ownAddress) = 0;
    virtual void close() = 0;
    virtual void executePendingRestart() = 0;
    /// Install what actually performs A_Restart. Without it the restart service
    /// refuses every request and the device answers a master reset with an
    /// error code instead of restarting.
    virtual void setRestartHandler(application::RestartService::RestartCallback handler) = 0;
    /// Install the device-side Resources behind Master Reset erase codes and
    /// Verify Mode (see ApplicationLayer::ManagementServerHooks).
    virtual void setManagementServerHooks(application::ApplicationLayer::ManagementServerHooks hooks) = 0;
    virtual void processBackgroundWork() = 0;
    virtual util::Result<void> sendDataLinkFrame(const datalink::LDataFrame& frame) = 0;
    virtual void setDataLinkPromiscuousMode(datalink::PromiscuousMode mode) = 0;
    virtual util::Result<void> addGroupAddress(const GroupAddress& address) = 0;
    virtual util::Result<void> setOwnAddress(const IndividualAddress& ownAddress) = 0;
    virtual void setIndividualAddressUpdateCallback(application::ApplicationLayer::IndividualAddressUpdateCallback callback) = 0;
    virtual void configureDataSecure(objects::SecurityInterfaceObject& securityObject) = 0;
    /// Install the handler for A_FunctionPropertyCommand / State_Read.
    virtual void setFunctionPropertyHandler(application::FunctionPropertyHandler handler) = 0;
    /// Install the handler for A_FunctionPropertyExtCommand / ExtState_Read.
    /// Separate from the classic hook because the extended services address the
    /// object by type + instance rather than by index, and answer in the
    /// unified return-code space of 03/03/07 §3.4.8.3 rather than the small
    /// FunctionPropertyReturnCode set.  ETS drives Data Secure through these.
    virtual void setExtendedFunctionPropertyProvider(
        application::PropertyExtServices::FunctionProvider provider) = 0;
    /// Publish this device's 6-octet KNX serial number so it can be addressed
    /// by A_IndividualAddressSerialNumber_Read/_Write without programming mode.
    virtual void setCommissioningSerialNumber(const application::KnxSerialNumber& serialNumber) = 0;

    /// The device's Domain Address in its medium's encoding; see
    /// application::NetworkParameterService::setDomainAddress. A port without
    /// an application layer has nowhere to put it.
    virtual void setDomainAddress(std::span<const uint8_t> domainAddress) { (void)domainAddress; }
    virtual void setDomainAddressWriteCallback(application::DomainAddressWriteCallback callback) {
        (void)callback;
    }
    /**
     * Declare which medium this device is on.
     *
     * It reaches the Device Descriptor Type 0 that ETS reads before it will
     * download anything: the mask version's high nibble is the medium
     * (03/05/01 Table 7), so a System B device is 07B0h on TP1 and 57B0h on
     * KNX IP. Defaulting to TP1 is right for most builds and wrong in a way
     * that fails commissioning for the rest, since ETS compares the descriptor
     * against the catalogue entry it is downloading.
     */
    virtual void setDeviceMedium(MediumType medium) = 0;
    virtual void setInboundCallback(InboundCallback callback) = 0;
    virtual void registerPropertyObject(InterfaceObjectType objectType,
                                        InterfaceObjectIndex objectIndex,
                                        PropertyRegistration registration) = 0;
    virtual void setPropertyReadProvider(PropertyReadProvider provider) = 0;
    virtual void setPropertyWriteProvider(PropertyWriteProvider provider) = 0;
    virtual void setPropertyDescriptionProvider(PropertyDescriptionProvider provider) = 0;
    /// Register a memory-mapped region backed by caller-owned storage (used by
    /// the ETS SystemB download: table segments transferred via A_Memory_Write).
    virtual util::Result<void> registerMemoryRegion(const application::MemoryRegion& region,
                                                    std::span<uint8_t> storage) = 0;
    /// Whether a management client has ever written into the region starting
    /// at @p regionStart. Distinguishes "downloaded all zeros" from "never
    /// downloaded" — the backing buffer reads zero in both cases.
    virtual bool memoryRegionWritten(MemoryAddress regionStart) const = 0;
    virtual void setWorkAvailableCallback(WorkAvailableCallback callback) = 0;
    virtual util::Result<void> sendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual util::Result<void> beginSendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual util::Result<void> sendGroupValueRead(const GroupAddress& address) = 0;
    virtual util::Result<void> beginSendGroupValueRead(const GroupAddress& address) = 0;
    virtual util::Result<TransmissionProgressState> pollTransmissionProgress() = 0;
    virtual bool transmissionInProgress() const = 0;
    virtual void setDefaultTransmissionOptions(const TransmissionOptions& options) = 0;
    virtual const TransmissionOptions& defaultTransmissionOptions() const = 0;
    virtual bool popTransmissionOutcome(TransmissionOutcome& outcome) = 0;
    virtual size_t queuedTransmissionOutcomeCount() const = 0;
    virtual util::Result<void> sendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual util::Result<void> beginSendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data) = 0;
    virtual void setProgrammingModeEnabled(bool enabled) = 0;
    virtual bool programmingModeEnabled() const = 0;

    /**
     * @brief The coupler, when this stack port was built as one.
     *
     * Returns nullptr for an ordinary single-port device, which is what makes
     * the coupler discoverable without every caller having to know how the
     * port was constructed. Use it to reach the filter table and the routing
     * policy — see `network::TwoPortCoupler`.
     */
    virtual network::TwoPortCoupler* coupler() { return nullptr; }

    /**
     * @brief Re-issue the multicast membership of an IP routing port.
     *
     * OperationNotSupported for every port that is not KNXnet/IP Routing,
     * which is the honest answer for a TP1 device rather than a silent
     * success. See `physical::IpRoutingPhysical::refreshMulticastMembership()`
     * for why a host has to ask for this at all.
     */
    virtual util::Result<void> refreshMulticastMembership() {
        return util::ErrorCode::OperationNotSupported;
    }

    /**
     * @brief Counters for the port, for telling silence apart from loss.
     *
     * A device that has stopped answering has three possible stories, and a
     * capture taken on the network can only distinguish the first two:
     * the telegram never reached the device, it reached the device and was
     * filtered or dropped, or it was handled and the answer went missing on
     * the way back. These counters separate the middle one from the first.
     *
     * Zero-filled on a port that does not keep the counter concerned.
     */
    virtual LinkDiagnostics linkDiagnostics() const { return {}; }
};

#if KNX_FEATURE_TP1
// TP1 entry points. Gated for the mirror of the reason the IP ones are: an
// IP-only firmware compiles no TP1 physical, so declaring these would leave
// bau.hpp promising symbols the library does not contain.
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::Tp1MacPhysical> physicalLayer);

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
 * filter table and routing policy through `BusAccessStackPort::coupler()`.
 */
std::unique_ptr<BusAccessStackPort> createTp1CouplerStackPort(
    platform::Platform& platform,
    std::unique_ptr<physical::Tp1MacPhysical> primaryPhysical,
    std::unique_ptr<physical::Tp1MacPhysical> secondaryPhysical);
#endif  // KNX_FEATURE_TP1

#if KNX_FEATURE_NETIP
// KNXnet/IP entry points.  Gated so a TP1-only firmware neither compiles nor
// links the IP stack: without this the tunnelling client, routing endpoint and
// their codecs stay reachable from bau.hpp and the linker cannot drop them.
std::unique_ptr<BusAccessStackPort> createTp1StackPort(platform::Platform& platform,
                                                       std::unique_ptr<physical::IpTunnelingPhysical> physicalLayer);

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
