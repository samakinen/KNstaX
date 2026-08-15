// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau_management.cpp
 * @brief BAU wiring for KNX management services that are not property access.
 *
 * Split out of bau.cpp: the Function Property and serial-number-addressed
 * commissioning services are a self-contained concern with no overlap with the
 * group-object runtime or the medium plumbing that dominates bau.cpp.
 */

#include "knx/bau/bau.hpp"

#include "knx/application/function_property_services.hpp"
#include "knx/application/network_parameter_service.hpp"
#include "knx/network/coupler_routing.hpp"
#include "knx/network/two_port_coupler.hpp"
#include "knx/network/routing_table_control.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/security_interface_object.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <span>

namespace knx {
namespace bau {

namespace {

constexpr const char* TAG = "KNX.BAU.Mgmt";

using application::FunctionPropertyInvocation;
using application::FunctionPropertyRequest;
using application::FunctionPropertyResult;
using application::FunctionPropertyReturnCode;

/**
 * @brief Handle the Function Properties of the Security Interface Object.
 *
 * ETS drives KNX Data Secure through Function Properties rather than plain
 * property writes, so this is the difference between a device ETS can put into
 * secure mode and one it cannot.
 *
 * PID_SECURITY_MODE (51) carries Reserved | ServiceID | ServiceInfo, where
 * ServiceInfo is the mode for a Command (0 = off, 1 = on); a State_Read returns
 * the current mode without changing it.
 */
/// Map the classic function-property return codes onto the unified schema the
/// extended services answer in (03/03/07 §3.4.8.3 table).  The two sets are
/// different code spaces, not the same numbers at different widths.
application::KnxReturnCode translateFunctionPropertyReturnCode(FunctionPropertyReturnCode code)
{
    switch (code) {
        case FunctionPropertyReturnCode::Success:
            return application::KnxReturnCode::Success;
        case FunctionPropertyReturnCode::InvalidProperty:
            // Property absent or not PDT_FUNCTION/PDT_CONTROL.
            return application::KnxReturnCode::DataTypeConflict;
        case FunctionPropertyReturnCode::InvalidCommand:
            // E_DATA_VOID: "the service or the function is supported, but the
            // request data is not valid for this receiver" — which is exactly
            // what the classic InvalidCommand means.
            return application::KnxReturnCode::DataVoid;
        case FunctionPropertyReturnCode::AccessDenied:
            return application::KnxReturnCode::AccessDenied;
        default:
            break;
    }
    // PID_GO_DIAGNOSTICS answers in the unified code space already (§4.8.1
    // names E_GD_CONFIG, E_GD_GO_VOID, E_COMMAND_INVALID … as the return codes
    // of the function itself), so those values travel unchanged.
    return static_cast<application::KnxReturnCode>(static_cast<uint8_t>(code));
}

util::Result<FunctionPropertyResult> handleSecurityFunctionProperty(
    objects::SecurityInterfaceObject& securityObject,
    const IndividualAddress& source,
    const FunctionPropertyRequest& request)
{
    using objects::SecurityProperty;

    FunctionPropertyResult result{};

    // 03/05/01 §6.3.4 and §6.3.5 restrict the *command* direction of both
    // function properties of this object to the Role "Tool" with A+C — "with
    // the Tool Access flag set in the S-A_Data-service".  Reading either back
    // is not restricted, and ETS does read both before it has a secure link.
    //
    // §6.3.5 is emphatic that this does not depend on the current mode: "Even
    // if Security Mode is disabled, it shall only be possible to enable it by
    // using secure communication."  Without the check, one plain telegram from
    // anywhere on the bus switches this device's Data Secure off.
    const bool isFunctionProperty =
        request.propertyId == static_cast<application::PropertyID>(SecurityProperty::LoadStateControl)
        || request.propertyId == static_cast<application::PropertyID>(SecurityProperty::SecurityMode);
    if (isFunctionProperty
            && request.invocation == FunctionPropertyInvocation::Command
            && !request.security.toolSecured()) {
        KNX_LOGW(TAG,
                 "Refusing command on Security Interface Object PID %u from 0x%04X: "
                 "§6.3.5 requires secure communication with the Tool Key",
                 static_cast<unsigned>(request.propertyId), source.raw);
        securityObject.logSecurityFailure(objects::SecurityFailure::InvalidSender, source.raw);
        result.returnCode = FunctionPropertyReturnCode::AccessDenied;
        return result;
    }

    // PID_LOAD_STATE_CONTROL is PDT_CONTROL, and 03/03/07 §3.4.5 lets a
    // management client drive a Control property through the Function Property
    // services as well as through a property write. ETS uses both: the plain
    // A_PropertyValue_Write for the objects it addresses by index, and
    // A_FunctionPropertyExtCommand for the Security Interface Object, which it
    // addresses by type. Answering the latter with "not a function property"
    // (E_DATA_TYPE_CONFLICT, 0xFE) is what ETS reports as "an internal device
    // error occurred" at the end of an otherwise complete download.
    if (request.propertyId == static_cast<application::PropertyID>(SecurityProperty::LoadStateControl)) {
        if (request.invocation == FunctionPropertyInvocation::Command) {
            if (request.data.empty()) {
                result.returnCode = FunctionPropertyReturnCode::InvalidCommand;
                return result;
            }
            // The control block is the raw load-control data — unlike
            // PID_SECURITY_MODE, it carries no Reserved/ServiceID header.
            objects::applyLoadControlEvent(securityObject, request.data[0]);
        }

        result.returnCode = FunctionPropertyReturnCode::Success;
        (void)result.data.push_back(securityObject.loadState());
        return result;
    }

    if (request.propertyId != static_cast<application::PropertyID>(SecurityProperty::SecurityMode)) {
        // Not a function property of this object: the caller turns this into
        // the spec's "no return code, no data" response.
        return util::ErrorCode::OperationNotSupported;
    }

    // 03/05/01 §6.3.5: the function data is Reserved (00h) | ServiceID |
    // ServiceInfo, for both the Command and the State_Read direction. Reading
    // the mode out of the first octet treats ETS's "enable" (00 00 01) as
    // "disable" — which silently switches Data Secure off in the middle of a
    // secure download, and every secured telegram after it is refused.
    constexpr size_t kReservedIndex = 0;
    constexpr size_t kServiceIdIndex = 1;
    constexpr size_t kServiceInfoIndex = 2;
    constexpr uint8_t kServiceIdSecurityMode = 0x00;

    if (request.data.size() <= kServiceIdIndex
            || request.data[kReservedIndex] != 0x00
            || request.data[kServiceIdIndex] != kServiceIdSecurityMode) {
        result.returnCode = FunctionPropertyReturnCode::InvalidCommand;
        return result;
    }

    if (request.invocation == FunctionPropertyInvocation::StateRead) {
        // State_Read must not change anything — it only reports.
        // Figure 71: Return Code | ReadServiceID | Security Mode.
        result.returnCode = FunctionPropertyReturnCode::Success;
        (void)result.data.push_back(kServiceIdSecurityMode);
        (void)result.data.push_back(securityObject.isSecurityEnabled() ? 1u : 0u);
        return result;
    }

    if (request.data.size() <= kServiceInfoIndex) {
        result.returnCode = FunctionPropertyReturnCode::InvalidCommand;
        return result;
    }

    const uint8_t requested = request.data[kServiceInfoIndex];
    if (requested > 1u) {
        result.returnCode = FunctionPropertyReturnCode::InvalidCommand;
        return result;
    }

    securityObject.setSecurityMode(requested == 1u ? objects::SecurityMode::Enabled
                                                   : objects::SecurityMode::Disabled);
    KNX_LOGI(TAG, "Security mode set to %s via A_FunctionPropertyCommand",
             requested == 1u ? "Enabled" : "Disabled");

    // Figure 70: the response repeats the WriteServiceID and does *not* carry
    // the resulting mode — the Return Code is what reports success.
    result.returnCode = FunctionPropertyReturnCode::Success;
    (void)result.data.push_back(kServiceIdSecurityMode);
    return result;
}

// KNX 03/05/01 property IDs of the two medium-specific interface objects this
// stack can publish.  They are declared here rather than pulled from the
// reference registry's private enum so the intent of each seeded value is
// visible at the point of use.
enum : uint8_t {
    kPidKnxNetIpProjectInstallationId = 51,
    kPidKnxNetIpIndividualAddress = 52,
    kPidKnxNetIpCurrentIpAddress = 57,
    kPidKnxNetIpMacAddress = 64,
    kPidKnxNetIpDeviceCapabilities = 68,
    kPidKnxNetIpFriendlyName = 76,

    kPidRouterLineStatus = 51,
    kPidRouterMainLcConfig = 52,
    kPidRouterSubLcConfig = 53,
    kPidRouterMainLcGrpConfig = 54,
    kPidRouterSubLcGrpConfig = 55,
    kPidRouterRouteTableControl = 56,
    kPidRouterMaxApduLength = 58,
    kPidRouterL2CouplerType = 59,
    kPidRouterHopCount = 61,
    kPidRouterMedium = 63,
    kPidRouterFilterTableUse = 67,
};

/// Big-endian helpers: KNX properties are always network byte order.
std::array<uint8_t, 2> beU16(uint16_t value)
{
    return {static_cast<uint8_t>((value >> 8) & 0xFFu), static_cast<uint8_t>(value & 0xFFu)};
}

}  // namespace

void BusAccessUnit::publishMediumInterfaceObjects()
{
    // ETS looks for the interface object that matches the medium a device
    // speaks.  Without the object present at all, an IP interface cannot be
    // configured and a coupler's filter table has nowhere to live — so the
    // objects are registered from the transport in use rather than left to
    // every firmware to remember.
    std::vector<InterfaceObjectType> required = referenceInterfaceObjectTypes();

    [[maybe_unused]] const auto require = [&required](InterfaceObjectType type) {
        if (std::find(required.begin(), required.end(), type) == required.end()) {
            required.push_back(type);
        }
    };

#if KNX_FEATURE_NETIP
    // A device reachable over KNXnet/IP must expose the KNXnet/IP Parameter
    // Object (type 11); it is where ETS reads the device's IP identity from.
    require(InterfaceObjectType::knxNetIpParameter());
#endif

    if (required.size() != referenceInterfaceObjectTypes().size()) {
        setReferenceInterfaceObjectTypes(std::move(required));
    }

    // --- Seed the values the device actually knows -------------------------
    // A registered-but-empty object answers every read with zeros, which reads
    // to ETS as "device with no address and no name" — worse than useful.
    // Only consumed by the KNXnet/IP seeding below; a TP1-only build has no
    // medium-specific object that carries the address.
    [[maybe_unused]] const auto address = _deviceObject.getIndividualAddress();

#if KNX_FEATURE_NETIP
    {
        const auto addressBytes = beU16(address.raw);
        (void)setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                         static_cast<application::PropertyID>(
                                             kPidKnxNetIpIndividualAddress),
                                         addressBytes);

        // Project installation id 0 is the documented "not assigned by a
        // project" value, so seeding it explicitly is meaningful, not filler.
        const auto installationId = beU16(0);
        (void)setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                         static_cast<application::PropertyID>(
                                             kPidKnxNetIpProjectInstallationId),
                                         installationId);

        // The friendly name is what ETS shows in its discovery list.  The
        // device object's order info is the closest thing this stack has to a
        // product name that the integrator will recognise.
        const auto& orderInfo = _deviceObject.getOrderInfo();
        if (!orderInfo.empty()) {
            (void)setReferenceObjectProperty(
                InterfaceObjectType::knxNetIpParameter(),
                static_cast<application::PropertyID>(kPidKnxNetIpFriendlyName),
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(orderInfo.data()),
                                         orderInfo.size()));
        }

        // A KNX serial number is 6 octets, the same width as a MAC address, and
        // this stack derives the serial from the MAC in the first place.
        const auto serial = _deviceObject.getSerialNumber();
        if (serial.size() >= application::kKnxSerialNumberBytes) {
            (void)setReferenceObjectProperty(
                InterfaceObjectType::knxNetIpParameter(),
                static_cast<application::PropertyID>(kPidKnxNetIpMacAddress),
                serial.first(application::kKnxSerialNumberBytes));
        }

        // Device capabilities bit 2 = "Tunnelling", bit 3 = "Object Server".
        // Only claim what the build actually contains.
        const auto capabilities = beU16(0x0004);
        (void)setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                         static_cast<application::PropertyID>(
                                             kPidKnxNetIpDeviceCapabilities),
                                         capabilities);
        (void)kPidKnxNetIpCurrentIpAddress;  // Filled by the IP transport once bound.
    }
#endif

    publishRouterObject();
}

util::Result<void> BusAccessUnit::configureRouterRole(const RouterRoleConfig& config)
{
    _routerRole = config;

    // When the stack port is a coupler it already owns a filter table and a
    // routing policy, and those are the ones the forwarding path consults.
    // Defaulting to them means a coupler product does not have to hand the same
    // two objects to two different APIs and hope they match.
    if (_stackPort) {
        if (auto* coupler = _stackPort->coupler()) {
            if (_routerRole.filterTable == nullptr) {
                _routerRole.filterTable = &coupler->filterTable();
            }
            if (_routerRole.routingPolicy == nullptr) {
                _routerRole.routingPolicy = &coupler->policy();
            }
        }
    }

    // A Router Object without a filter table is worse than no Router Object:
    // ETS would download a filter table into something no forwarding path ever
    // reads, and the integrator would have no way to tell.
    if (_routerRole.filterTable == nullptr) {
        KNX_LOGE(TAG, "configureRouterRole() requires a filter table");
        return util::ErrorCode::InvalidParameter;
    }

    // When called after init(), publish immediately; otherwise init() picks it
    // up through publishMediumInterfaceObjects().
    if (_initialized) {
        publishRouterObject();
    }
    return util::Result<void>::ok();
}

void BusAccessUnit::publishRouterObject()
{
    if (!hasRouterRole()) {
        return;
    }

    // Register the object if it is not already present.
    std::vector<InterfaceObjectType> required = referenceInterfaceObjectTypes();
    if (std::find(required.begin(), required.end(), InterfaceObjectType::router())
        == required.end()) {
        required.push_back(InterfaceObjectType::router());
        setReferenceInterfaceObjectTypes(std::move(required));
    }

    // Seeding failures are logged, not swallowed. A silently unseeded Router
    // Object answers every read with nothing, which looks to ETS like a coupler
    // that has no configuration rather than a device with a wiring bug.
    const auto seed = [this](uint8_t pid, std::span<const uint8_t> bytes) {
        const auto result = setReferenceObjectProperty(
            InterfaceObjectType::router(), static_cast<application::PropertyID>(pid), bytes);
        if (result.isError()) {
            KNX_LOGE(TAG, "Router Object PID %u could not be seeded: %s",
                     static_cast<unsigned>(pid), util::errorCodeToString(result.error()));
        }
    };
    const auto seedByte = [&seed](uint8_t pid, uint8_t value) {
        const std::array<uint8_t, 1> bytes{value};
        seed(pid, bytes);
    };
    const auto seedWord = [&seed](uint8_t pid, uint16_t value) {
        const auto bytes = beU16(value);
        seed(pid, bytes);
    };

    seedByte(kPidRouterMedium, _routerRole.subMedium);
    seedWord(kPidRouterMaxApduLength, _routerRole.maxRoutedApduLength);
    seedByte(kPidRouterHopCount, _routerRole.hopCount);
    seedByte(kPidRouterL2CouplerType, _routerRole.l2CouplerType);
    seedByte(kPidRouterFilterTableUse, _routerRole.filterTableInUse ? 1u : 0u);

    // Sub-line power is up until the link layer says otherwise; PID_LINE_STATUS
    // bit 0 is POWER_DOWN_SUBLINE, so 0 is the healthy value.
    seedByte(kPidRouterLineStatus, 0u);

    // The coupler configuration bytes. When a routing policy is attached these
    // are seeded from it, so the Router Object reports what the forwarding path
    // will actually do rather than a fixed guess. Otherwise the specification
    // defaults from 03/05/01 §4.4.4 / §4.4.5 apply.
    uint8_t mainLcConfig = network::kDefaultLineCouplerConfig;
    uint8_t subLcConfig = network::kDefaultLineCouplerConfig;
    uint8_t mainLcGrpConfig = network::kDefaultGroupCouplerConfig;
    uint8_t subLcGrpConfig = network::kDefaultGroupCouplerConfig;
    if (_routerRole.routingPolicy != nullptr) {
        const auto& policy = *_routerRole.routingPolicy;
        mainLcConfig = policy.lineConfig(network::CouplerPort::Primary).encode();
        subLcConfig = policy.lineConfig(network::CouplerPort::Secondary).encode();
        mainLcGrpConfig = policy.groupConfig(network::CouplerPort::Primary).encode();
        subLcGrpConfig = policy.groupConfig(network::CouplerPort::Secondary).encode();
    }
    seedByte(kPidRouterMainLcConfig, mainLcConfig);
    seedByte(kPidRouterSubLcConfig, subLcConfig);
    seedByte(kPidRouterMainLcGrpConfig, mainLcGrpConfig);
    seedByte(kPidRouterSubLcGrpConfig, subLcGrpConfig);

    KNX_LOGI(TAG, "Router Object published (medium=%u, hop count=%u, filter table %s)",
             static_cast<unsigned>(_routerRole.subMedium),
             static_cast<unsigned>(_routerRole.hopCount),
             _routerRole.filterTableInUse ? "in use" : "bypassed");
}

util::Result<void> BusAccessUnit::syncRouterRoutingConfig()
{
    if (!hasRouterRole() || _routerRole.routingPolicy == nullptr) {
        return util::Result<void>::ok();
    }
    // A property that was never written comes back empty. Leaving the policy
    // untouched in that case is deliberate: an absent value means "not
    // downloaded", and overwriting a working configuration with a zero byte
    // would be worse than ignoring it.
    const auto readByte = [this](uint8_t pid) -> std::optional<uint8_t> {
        const auto value = referenceObjectPropertyValue(
            InterfaceObjectType::router(), static_cast<application::PropertyID>(pid));
        if (value.empty()) return std::nullopt;
        return value.front();
    };

    auto& policy = *_routerRole.routingPolicy;
    if (const auto raw = readByte(kPidRouterMainLcConfig)) {
        policy.lineConfig(network::CouplerPort::Primary) =
            network::LineCouplerConfig::decode(*raw);
    }
    if (const auto raw = readByte(kPidRouterSubLcConfig)) {
        policy.lineConfig(network::CouplerPort::Secondary) =
            network::LineCouplerConfig::decode(*raw);
    }
    if (const auto raw = readByte(kPidRouterMainLcGrpConfig)) {
        policy.groupConfig(network::CouplerPort::Primary) =
            network::GroupCouplerConfig::decode(*raw);
    }
    if (const auto raw = readByte(kPidRouterSubLcGrpConfig)) {
        policy.groupConfig(network::CouplerPort::Secondary) =
            network::GroupCouplerConfig::decode(*raw);
    }
    if (const auto raw = readByte(kPidRouterFilterTableUse)) {
        policy.setFilterTableInUse((*raw & 0x01u) != 0u);
    }

    KNX_LOGI(TAG, "Router routing configuration synced from Router Object");
    return util::Result<void>::ok();
}

util::Result<application::FunctionPropertyResult> BusAccessUnit::handleRouterFunctionProperty(
    const application::FunctionPropertyRequest& request)
{
    // Only PID_ROUTETABLE_CONTROL is a function property on this object.
    if (request.propertyId != static_cast<application::PropertyID>(kPidRouterRouteTableControl)) {
        return util::ErrorCode::OperationNotSupported;
    }
    if (!hasRouterRole()) {
        // The object should not be registered without a role, but if it is,
        // answering "not a function property" is safer than pretending to
        // configure a table that does not exist.
        return util::ErrorCode::OperationNotSupported;
    }

    network::RoutingTableControl control(*_routerRole.filterTable);
    return control.invoke(request.invocation, request.data.span());
}

void BusAccessUnit::applySecurityKeyTables()
{
    using objects::SecurityProperty;
    namespace security_ia_table = objects::security_ia_table;

    // Entry sizes of 03/05/01 §6.3.6 Figure 72, §6.3.7 Figure 74 and §6.3.8
    // Figure 75. They match the PDT_GENERIC_nn the properties are declared as.
    constexpr size_t kGroupKeyEntryBytes = 18;   // GA_Index(2) + Key(16)
    constexpr size_t kP2PKeyEntryBytes = 20;     // IA_Index(2) + Key(16) + Roles(2)
    constexpr size_t kKeyBytes = 16;

    const auto readIndex = [](const std::vector<uint8_t>& blob, size_t offset) {
        return static_cast<uint16_t>((static_cast<uint16_t>(blob[offset]) << 8) | blob[offset + 1]);
    };

    // The maps are a derived view: start empty so an entry ETS deleted in the
    // project does not linger as a usable key.
    _securityObject.clearGroupKeys();
    _securityObject.clearDeviceKeys();

    // §6.3.7: the 2-octet field is the GA_Index — the TSAP, i.e. the 1-based
    // index into the Group Address Table — and NOT the group address itself.
    // Resolving it is the whole reason this lives in the BAU rather than in the
    // Security Interface Object, which cannot see the address table.
    size_t groupKeys = 0;
    size_t unresolvedGroupKeys = 0;
    if (const auto* blob = _securityObject.findExtraProperty(SecurityProperty::GroupKeyTable)) {
        for (size_t offset = 0; offset + kGroupKeyEntryBytes <= blob->size();
             offset += kGroupKeyEntryBytes) {
            const uint16_t gaIndex = readIndex(*blob, offset);
            const GroupAddress address = _addressTable.getAddress(AddressTableIndex(gaIndex));
            if (address.raw == 0u) {
                // The group address table is downloaded as its own load
                // procedure, so this is expected mid-download and resolves on
                // the next call; a leftover here means the two tables disagree.
                ++unresolvedGroupKeys;
                continue;
            }
            std::array<uint8_t, kKeyBytes> key{};
            std::copy_n(blob->begin() + static_cast<ptrdiff_t>(offset + 2), kKeyBytes, key.begin());
            _securityObject.setGroupKey(address, key);
            ++groupKeys;
        }
    }

    // §6.3.8: the Security Individual Address Table is the index space the
    // point-to-point key table refers to, so it has to be read first. Its
    // second field is each partner's Last Valid SeqNr; adopting those values is
    // what carries the replay window across a restart, so it happens here too —
    // this function is the one place called whenever the tables change.
    _securityObject.syncSequencesFromAddressTable();

    std::vector<IndividualAddress> securityAddresses;
    if (const auto* blob = _securityObject.findExtraProperty(
            SecurityProperty::SecurityIndividualAddressTable)) {
        const std::span<const uint8_t> table(*blob);
        const size_t entries = security_ia_table::entryCount(table);
        for (size_t index = 0; index < entries; ++index) {
            securityAddresses.push_back(security_ia_table::addressAt(table, index));
        }
    }

    size_t deviceKeys = 0;
    size_t unresolvedDeviceKeys = 0;
    if (const auto* blob = _securityObject.findExtraProperty(SecurityProperty::P2PKeyTable)) {
        for (size_t offset = 0; offset + kP2PKeyEntryBytes <= blob->size();
             offset += kP2PKeyEntryBytes) {
            // IA_Index is 1-based, like every other KNX table index.
            const uint16_t iaIndex = readIndex(*blob, offset);
            if (iaIndex == 0u || iaIndex > securityAddresses.size()) {
                ++unresolvedDeviceKeys;
                continue;
            }
            std::array<uint8_t, kKeyBytes> key{};
            std::copy_n(blob->begin() + static_cast<ptrdiff_t>(offset + 2), kKeyBytes, key.begin());
            // The trailing Roles field is deliberately dropped: this stack has
            // no Role model beyond Tool/Unlisted, and silently honouring a
            // subset of the 16 configured Roles would be worse than ignoring
            // them, because the refusals would look arbitrary.
            _securityObject.setDeviceKey(securityAddresses[iaIndex - 1u], key);
            ++deviceKeys;
        }
    }

    if (groupKeys != 0 || deviceKeys != 0 || unresolvedGroupKeys != 0 || unresolvedDeviceKeys != 0) {
        KNX_LOGI(TAG,
                 "Data Secure key tables applied: %zu group key(s), %zu device key(s), "
                 "%zu security IA entries (%zu group / %zu device entries unresolved)",
                 groupKeys, deviceKeys, securityAddresses.size(),
                 unresolvedGroupKeys, unresolvedDeviceKeys);
    } else if (_securityObject.isSecurityEnabled()) {
        // Worth saying out loud: with Security Mode on and no group key, every
        // group telegram this device sends goes out in plain and every secured
        // one it receives is dropped as "unknown key".
        KNX_LOGW(TAG,
                 "Data Secure is enabled but no group or device keys are configured; "
                 "ETS has not downloaded PID_GRP_KEY_TABLE / PID_P2P_KEY_TABLE");
    }
}

void BusAccessUnit::wireManagementServices()
{
    publishMediumInterfaceObjects();

    if (!_stackPort) {
        return;
    }

    // --- Function Properties ----------------------------------------------
    _stackPort->setFunctionPropertyHandler(
        [this](const IndividualAddress& source,
               const FunctionPropertyRequest& request) -> util::Result<FunctionPropertyResult> {
            const auto objectType = interfaceObjectTypeForIndex(request.objectIndex);
            if (!objectType.has_value()) {
                return util::ErrorCode::InvalidAddress;
            }

            if (*objectType == InterfaceObjectType::security()) {
                return handleSecurityFunctionProperty(_securityObject, source, request);
            }

            if (*objectType == InterfaceObjectType::router()) {
                return handleRouterFunctionProperty(request);
            }

            if (*objectType == InterfaceObjectType::groupObjectTable()) {
                return handleGroupObjectDiagnostics(request);
            }

            // Any other object: return an error so the caller produces the
            // §3.4.5.3 response telling the management client the property is
            // not a function property.  That is the correct answer, not a
            // placeholder.
            return util::ErrorCode::OperationNotSupported;
        });

    // Same functions reached through the *extended* services, which is how ETS
    // drives Data Secure: Profiles §9.1.2.3 makes A_FunctionPropertyExt* part
    // of the KNX Data Security profile, and ETS reads PID_SECURITY_MODE of the
    // Security Interface Object with A_FunctionPropertyExtState_Read before a
    // secure download.  Without this the provider stayed null and every such
    // request answered E_DATA_TYPE_CONFLICT, which ETS reports as "ETS tried to
    // read a protected or a non-existing memory block".
    //
    // The extended header addresses the object by type + instance, so no index
    // lookup is needed; the return code is translated into the unified schema
    // of 03/03/07 §3.4.8.3.
    _stackPort->setExtendedFunctionPropertyProvider(
        [this](const IndividualAddress& source,
               const application::PropertyExtHeader& header,
               application::FunctionPropertyInvocation invocation,
               const RequestSecurity& security,
               std::span<const uint8_t> input,
               application::FunctionPropertyExtResponse::DataBuffer& output)
            -> std::optional<application::KnxReturnCode> {
            FunctionPropertyRequest request{};
            request.propertyId = static_cast<application::PropertyID>(header.propertyId);
            request.invocation = invocation;
            // The extended encoding is the one ETS actually uses for Data
            // Secure, so the Access Policy has to reach this path too.
            request.security = security;
            if (!request.data.assign(input)) {
                return application::KnxReturnCode::ExceedsMaxApduLength;
            }

            const InterfaceObjectType objectType{static_cast<uint16_t>(header.objectType)};
            util::Result<FunctionPropertyResult> result = util::ErrorCode::OperationNotSupported;
            if (objectType == InterfaceObjectType::security()) {
                result = handleSecurityFunctionProperty(_securityObject, source, request);
            } else if (objectType == InterfaceObjectType::router()) {
                result = handleRouterFunctionProperty(request);
            } else if (objectType == InterfaceObjectType::groupObjectTable()) {
                // ETS reads and writes a *secured* group address through the
                // device rather than sending the telegram itself, and that is
                // PID_GO_DIAGNOSTICS on this object.  Answering it with
                // E_DATA_TYPE_CONFLICT is what ETS reports as "Asking device
                // … to send group message on behalf of ETS failed".
                result = handleGroupObjectDiagnostics(request);
            }

            if (result.isError()) {
                // "Not a function property of this object" — nullopt lets the
                // service answer E_DATA_TYPE_CONFLICT with no data field.
                return std::nullopt;
            }

            // The data field is whatever the handler produced, for every return
            // code. §3.4.8.3's "a negative return code carries no data" is the
            // generic rule, and the function properties this device implements
            // override it: PID_ROUTETABLE_CONTROL §4.4.6.6 echoes the ServiceID
            // back with an error code, and every PID_GO_DIAGNOSTICS answer of
            // §4.8.1 — positive E_GD_CONFIG / E_GD_GO_STATUS_VALUE as much as
            // negative E_GD_GO_VOID / E_COMMAND_INVALID — repeats it too.
            // Clearing the buffer here left the client unable to tell which of
            // several outstanding commands the refusal belonged to.
            const auto code = translateFunctionPropertyReturnCode(result.value().returnCode);
            if (!output.assign(std::span<const uint8_t>(result.value().data.data(),
                                                        result.value().data.size()))) {
                return application::KnxReturnCode::ExceedsMaxApduLength;
            }
            return code;
        });

    // --- Restart ----------------------------------------------------------
    // A_Restart is how a management client ends the individual-address
    // procedure (03/05/02 §2.3 deactivates programming mode "by executing a
    // restart of the device"), so a device that cannot restart cannot be
    // commissioned: ETS reports "ConfirmedRestart failed: 255", 255 being the
    // error code the restart service returns when nothing is registered.
    //
    // Runs after the A_Restart_Response is already on the wire — handleRestart
    // sends it before calling executePendingRestart — so the client has its
    // confirmation before the device goes down.
    _stackPort->setRestartHandler(
        [this](application::RestartType type) -> util::Result<void> {
            // Property writes only mark persistent state dirty; loop() batches
            // the NVS flush. A restart can arrive mid-burst, so force the
            // pending write out before the device goes down.
            flushPendingPersistence();
            KNX_LOGI(TAG, "Restarting on %s request",
                     type == application::RestartType::MasterReset ? "master reset" : "basic restart");
            _platform.restart();
            return util::Result<void>::ok();
        });

    // --- Serial-number-addressed commissioning ----------------------------
    // PID_SERIAL_NUMBER already carries the device's KNX serial number (this
    // stack derives it from the MAC), so the commissioning service can simply
    // adopt it rather than introducing a second source of truth.
    const auto serial = _deviceObject.getSerialNumber();
    if (serial.size() >= application::kKnxSerialNumberBytes) {
        application::KnxSerialNumber knxSerial{};
        std::copy_n(serial.begin(), application::kKnxSerialNumberBytes, knxSerial.begin());
        _stackPort->setCommissioningSerialNumber(knxSerial);
        KNX_LOGD(TAG, "Serial-number-addressed commissioning enabled");
    } else {
        KNX_LOGW(TAG,
                 "Device serial number is %zu bytes (need %zu); "
                 "A_IndividualAddressSerialNumber services stay inactive",
                 serial.size(), application::kKnxSerialNumberBytes);
    }
}

} // namespace bau
} // namespace knx
