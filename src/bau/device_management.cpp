// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_management.cpp
 * @brief DeviceManagement: KNX management services that are not property
 *        access, and the ManagementControl facet over them.
 *
 * PID_GO_DIAGNOSTICS, the one management service that reaches into the
 * group-object runtime, is in go_diagnostics.cpp.
 */

#include "device_management.hpp"

#include "device_model.hpp"
#include "group_runtime.hpp"

#include "knx/application/function_property_services.hpp"
#include "knx/application/network_parameter_service.hpp"
#include "knx/network/coupler_routing.hpp"
#include "knx/network/two_port_coupler.hpp"
#include "knx/network/routing_table_control.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/security_interface_object.hpp"
#include "knx/util/log.hpp"
#if KNX_FEATURE_NETIP
#include "knx/netip/secured_service_families.hpp"
#endif

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
    kPidKnxNetIpCurrentIpAssignmentMethod = 54,
    kPidKnxNetIpIpAssignmentMethod = 55,
    kPidKnxNetIpCurrentIpAddress = 57,
    kPidKnxNetIpCurrentSubnetMask = 58,
    kPidKnxNetIpCurrentDefaultGateway = 59,
    kPidKnxNetIpMacAddress = 64,
    kPidKnxNetIpSystemSetupMulticastAddress = 65,
    kPidKnxNetIpRoutingMulticastAddress = 66,
    kPidKnxNetIpTtl = 67,
    kPidKnxNetIpDeviceCapabilities = 68,
    kPidKnxNetIpFriendlyName = 76,
    kPidKnxNetIpBackboneKey = 91,
    kPidKnxNetIpSecuredServiceFamilies = 94,

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

#if KNX_FEATURE_NETIP
/// An IPv4 address as the four octets a KNX property carries, most significant
/// first. IpAddress stores them already in that order, so this is a copy with
/// the byte order named rather than assumed.
std::array<uint8_t, 4> ipOctets(IpAddress address)
{
    std::array<uint8_t, 4> octets{};
    address.toOctets(octets[0], octets[1], octets[2], octets[3]);
    return octets;
}

/// PID_ROUTING_MULTICAST_ADDRESS is PDT_UNSIGNED_LONG holding "the KNXnet/IP
/// Routing Multicast Address" (03_08_03 §2.5.17), so anything that is not a
/// four-octet IPv4 multicast address (224.0.0.0/4) is refused rather than
/// stored as a group the device could never join.
bool isRoutingMulticastAddress(std::span<const uint8_t> value)
{
    return value.size() == 4u && (value[0] & 0xF0u) == 0xE0u;
}

IpAddress ipAddressFromOctets(std::span<const uint8_t> value)
{
    return IpAddress::fromOctets(value[0], value[1], value[2], value[3]);
}
#endif

}  // namespace

DeviceManagement::DeviceManagement(platform::Platform& platform,
                                   BusAccessStackPort* stackPort,
                                   DeviceModel& model,
                                   GroupRuntime& groups)
    : _platform(platform)
    , _stackPort(stackPort)
    , _model(model)
    , _groups(groups)
{
}

// ─── Device object ↔ stack port ──────────────────────────────────────────────

void DeviceManagement::applyDeviceMedium()
{
    // Applied here as well as in setDeviceMedium(): a product declares its
    // medium before start(), when there is no stack port yet to tell.
    if (_stackPort != nullptr) {
        _stackPort->setDeviceMedium(_deviceMedium);
    }
}

void DeviceManagement::wireDeviceObject()
{
    if (_stackPort == nullptr) {
        return;
    }
    auto& deviceObject = _model.deviceObject();

    _stackPort->setProgrammingModeEnabled(deviceObject.getProgMode());
    deviceObject.registerProgModeCallback([this](Toggle mode) {
        _stackPort->setProgrammingModeEnabled(mode == Toggle::Enable);
    });
    // Firmware sets the real serial number (MAC-derived here) after start(),
    // which is after wireManagementServices() cached it.  Re-push it so the
    // serial-number-addressed services and the programming-mode scan answer
    // with the device's actual identity instead of the all-zero placeholder.
    deviceObject.registerSerialNumberCallback([this](std::span<const uint8_t> serial) {
        if (serial.size() < application::kKnxSerialNumberBytes) {
            return;
        }
        application::KnxSerialNumber knxSerial{};
        std::copy_n(serial.begin(), application::kKnxSerialNumberBytes, knxSerial.begin());
        _stackPort->setCommissioningSerialNumber(knxSerial);
    });
    deviceObject.registerIndividualAddressCallback([this](const IndividualAddress& newAddress) {
        // The address is stored as two independent properties (PID_SUBNET_ADDRESS
        // and PID_DEVICE_ADDRESS), so restoring it from persistence necessarily
        // arrives in two steps. Propagating the intermediate value would put a
        // half-restored address on the stack — e.g. 1.1.255 between restoring
        // the subnet and the device octet. init() pushes the final address to
        // the stack once the restore completes, so stay quiet until then.
        if (_model.restoring()) {
            return;
        }
        auto res = _stackPort->setOwnAddress(newAddress);
        if (res.isError()) {
            KNX_LOGW(TAG, "Failed to propagate new own address to stack: %s",
                     util::errorCodeToString(res.error()));
        }
        // A failure is logged there and left dirty for the next pump to retry.
        (void)_model.saveToPersistence();
    });
    _stackPort->setIndividualAddressUpdateCallback([this](const IndividualAddress& newAddress) {
        // This fires *after* the application layer has already accepted the new
        // address, so the programming-mode question was settled there and must
        // not be asked a second time: A_IndividualAddress_Write is refused
        // outside programming mode by handleIndividualAddressWrite, while
        // A_IndividualAddressSerialNumber_Write is deliberately not — the serial
        // number already identified the device, which is the whole reason
        // 03/05/03 §1.5.4.3.2 uses it to re-address a device nobody can reach a
        // button on. Routing this through writeIndividualAddress() re-applied
        // the programming-mode gate and dropped the serial-number assignment on
        // the floor: the stack answered from the new address, but the Device
        // Object kept the old one and nothing was persisted, so the Confirmed
        // Restart that ends the commissioning procedure undid the address.
        if (_model.deviceObject().applyIndividualAddress(newAddress).isError()) {
            KNX_LOGW(TAG, "Failed to update BAU device object individual address for 0x%04X",
                     newAddress.raw);
        }
    });
}

util::Result<void> DeviceManagement::setCommissioningMode(bool enabled)
{
    _model.deviceObject().setProgMode(enabled ? Toggle::Enable : Toggle::Disable);
    if (_stackPort != nullptr) {
        _stackPort->setProgrammingModeEnabled(enabled);
    }
    return util::Result<void>::ok();
}

bool DeviceManagement::commissioningModeEnabled() const
{
    return _model.deviceObject().getProgMode();
}

void DeviceManagement::setDomainAddress(std::span<const uint8_t> domainAddress)
{
    if (_stackPort != nullptr) {
        _stackPort->setDomainAddress(domainAddress);
    }
}

// ─── Master Reset ────────────────────────────────────────────────────────────

util::Result<void> DeviceManagement::applyMasterReset(application::EraseCode eraseCode)
{
    using application::EraseCode;

    // 03/05/02 Table 4. Which Resources a factory reset covers is
    // implementation dependent beyond Table 6 and §3.7.1.2.3.2.2; here it is
    // everything ETS downloads, i.e. everything persisted except the
    // Sequence Number Sending, which §5 of 03/03/07 ("Sequence Number Sending
    // and Master Reset") says a Master Reset shall not influence.
    const bool factoryReset = eraseCode == EraseCode::FactoryReset
                           || eraseCode == EraseCode::FactoryResetWithoutIndividualAddress;
    const bool resetAddress = eraseCode == EraseCode::FactoryReset
                           || eraseCode == EraseCode::ResetIndividualAddress;
    const bool resetLinks = factoryReset || eraseCode == EraseCode::ResetLinks;
    // The application program of this stack is firmware; what ETS downloads
    // into it is the parameter block (PID_PROGRAM_DATA). Resetting the
    // program to "the default application" and resetting its parameters
    // therefore come to the same thing.
    const bool resetParameters = factoryReset
                              || eraseCode == EraseCode::ResetApplicationProgram
                              || eraseCode == EraseCode::ResetParameters;

    if (resetLinks) {
        _model.addressTable().clearEntries();
        _model.associationTable().clearEntries();
    }

    if (resetParameters) {
        // An empty block is not applied: the next boot finds nothing stored
        // and keeps the firmware defaults, the same as a device that has
        // never been downloaded.
        _model.applicationProgram().setProgramData(std::span<const uint8_t>{});
    }

    if (factoryReset) {
        // Ex-factory, a Data Secure device has Security Mode off and only its
        // FDSK as Tool Key (03/05/03 §1.5.4.1). A cleared Tool Key is what a
        // product's boot path reads as "no key yet" and replaces with the FDSK
        // (Habinari: installFactoryToolKey). The key tables are dropped whole,
        // which §3.7.1.2.3.2.2 requires so that the FDSK cannot be used to
        // read back what a previous owner downloaded.
        auto& securityObject = _model.securityObject();
        securityObject.loadControlReset();
        securityObject.clearAllKeys();
        securityObject.setSecurityMode(objects::SecurityMode::Disabled);
#if KNX_FEATURE_NETIP
        // Table 6 resets the IP Resources with Erase Code 02h and keeps them
        // with 07h; the backbone key is the KNX IP Secure credential and goes
        // with the other keys either way.
        const std::array<uint8_t, 16> noBackboneKey{};
        (void)_model.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                                static_cast<application::PropertyID>(kPidKnxNetIpBackboneKey),
                                                noBackboneKey);
        _securedServiceFamilyVersions.fill(0u);

        // 03/05/02 Table 6: the "IP multicast address" "shall be reset" by
        // Erase Code 02h and "shall NOT be reset" by 07h. Reset means back to
        // the product's default, which is the group the device then starts on.
        if (eraseCode == EraseCode::FactoryReset && !_routingMulticastDefault.isZero()) {
            const auto octets = ipOctets(_routingMulticastDefault);
            (void)_model.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                                    static_cast<application::PropertyID>(
                                                        kPidKnxNetIpRoutingMulticastAddress),
                                                    octets);
            setDomainAddress(octets);
        }
#endif
    }

    if (resetAddress) {
        // The response has already gone out from the old address (03/05/02
        // §3.7.1.2.2 NOTE 11). Applying it now persists it; the stack itself
        // picks it up when the device comes back from the restart.
        _model.deviceObject().setIndividualAddress(_defaultIndividualAddress);
    }

    KNX_LOGW(TAG, "Master reset, erase code 0x%02X: links%s, parameters%s, security%s, address%s",
             static_cast<unsigned>(eraseCode),
             resetLinks ? " reset" : " kept",
             resetParameters ? " reset" : " kept",
             factoryReset ? " reset" : " kept",
             resetAddress ? " reset" : " kept");

    // Written synchronously: the restart callback runs next. A failure is
    // logged there and stays dirty, so the restart handler's flush retries it.
    (void)_model.saveToPersistence();
    return util::Result<void>::ok();
}

// ─── Medium interface objects and router role ───────────────────────────────

void DeviceManagement::publishMediumInterfaceObjects()
{
    // ETS looks for the interface object that matches the medium a device
    // speaks.  Without the object present at all, an IP interface cannot be
    // configured and a coupler's filter table has nowhere to live — so the
    // objects are registered from the transport in use rather than left to
    // every firmware to remember.
    std::vector<InterfaceObjectType> required = _model.referenceObjectTypes();

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

    if (required.size() != _model.referenceObjectTypes().size()) {
        _model.setReferenceObjectTypes(std::move(required));
    }

    // --- Seed the values the device actually knows -------------------------
    // A registered-but-empty object answers every read with zeros, which reads
    // to ETS as "device with no address and no name" — worse than useful.
    // Only consumed by the KNXnet/IP seeding below; a TP1-only build has no
    // medium-specific object that carries the address.
    [[maybe_unused]] const auto address = _model.deviceObject().getIndividualAddress();

#if KNX_FEATURE_NETIP
    {
        const auto addressBytes = beU16(address.raw);
        (void)_model.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                         static_cast<application::PropertyID>(
                                             kPidKnxNetIpIndividualAddress),
                                         addressBytes);

        // Project installation id 0 is the documented "not assigned by a
        // project" value, so seeding it explicitly is meaningful, not filler.
        const auto installationId = beU16(0);
        (void)_model.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                         static_cast<application::PropertyID>(
                                             kPidKnxNetIpProjectInstallationId),
                                         installationId);

        // The friendly name is what ETS shows in its discovery list.  The
        // device object's order info is the closest thing this stack has to a
        // product name that the integrator will recognise.
        const auto& orderInfo = _model.deviceObject().getOrderInfo();
        if (!orderInfo.empty()) {
            (void)_model.setReferenceObjectProperty(
                InterfaceObjectType::knxNetIpParameter(),
                static_cast<application::PropertyID>(kPidKnxNetIpFriendlyName),
                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(orderInfo.data()),
                                         orderInfo.size()));
        }

        // A KNX serial number is 6 octets, the same width as a MAC address, and
        // this stack derives the serial from the MAC in the first place.
        const auto serial = _model.deviceObject().getSerialNumber();
        if (serial.size() >= application::kKnxSerialNumberBytes) {
            (void)_model.setReferenceObjectProperty(
                InterfaceObjectType::knxNetIpParameter(),
                static_cast<application::PropertyID>(kPidKnxNetIpMacAddress),
                serial.first(application::kKnxSerialNumberBytes));
        }

        // Device capabilities bit 2 = "Tunnelling", bit 3 = "Object Server".
        // Only claim what the build actually contains.
        const auto capabilities = beU16(0x0004);
        (void)_model.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                         static_cast<application::PropertyID>(
                                             kPidKnxNetIpDeviceCapabilities),
                                         capabilities);
        // The address-bearing properties stay untouched here. They are not
        // knowable at this point — the interface may not even be up — and are
        // filled by applyKnxNetIpConfiguration() once the firmware has them.

        // PID_BACKBONE_KEY is the one property in this object that does
        // something when written rather than merely recording it. ETS
        // downloads the KNX/IP Secure Routing group key here, and it is meant
        // to take effect without a restart.
        //
        // PID_ROUTING_MULTICAST_ADDRESS is the other: storing it is most of
        // the job (it becomes active after a reset, 03_08_03 §2.5.17), but a
        // new value is E11 for the multicast timer (03_08_09 §2.2.2.3.2.5),
        // and a value that is not a multicast group is refused outright.
        if (auto* object = _model.referenceObject(InterfaceObjectType::knxNetIpParameter())) {
            object->setWriteValidator([](application::PropertyID propertyId,
                                         std::span<const uint8_t> value) -> util::Result<void> {
                if (static_cast<uint8_t>(propertyId) == kPidKnxNetIpRoutingMulticastAddress &&
                    !isRoutingMulticastAddress(value)) {
                    return util::ErrorCode::InvalidParameter;
                }
                return util::Result<void>::ok();
            });
            object->setWriteObserver([this](application::PropertyID propertyId,
                                            std::span<const uint8_t> previous,
                                            std::span<const uint8_t> value) {
                switch (static_cast<uint8_t>(propertyId)) {
                    case kPidKnxNetIpBackboneKey:
                        if (_backboneKeyCallback) {
                            _backboneKeyCallback(value);
                        }
                        break;
                    case kPidKnxNetIpRoutingMulticastAddress:
                        onRoutingMulticastAddressWritten(previous, value);
                        break;
                    default:
                        break;
                }
            });
        }
    }
#endif

    publishRouterObject();
}

util::Result<void> DeviceManagement::configureRouterRole(const RouterRoleConfig& config)
{
    _routerRole = config;

    // When the stack port is a coupler it already owns a filter table and a
    // routing policy, and those are the ones the forwarding path consults.
    // Defaulting to them means a coupler product does not have to hand the same
    // two objects to two different APIs and hope they match.
    if (_stackPort != nullptr) {
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
    if (_running) {
        publishRouterObject();
    }
    return util::Result<void>::ok();
}

void DeviceManagement::publishRouterObject()
{
    if (!hasRouterRole()) {
        return;
    }

    // Register the object if it is not already present.
    std::vector<InterfaceObjectType> required = _model.referenceObjectTypes();
    if (std::find(required.begin(), required.end(), InterfaceObjectType::router())
        == required.end()) {
        required.push_back(InterfaceObjectType::router());
        _model.setReferenceObjectTypes(std::move(required));
    }

    // During BusAccessUnit::init() this first runs in phase 4, before the
    // model has created any reference object: the type is declared now and
    // seeded in phase 7 once the object exists. Seeding here would only fail.
    if (!_model.referenceObjectsInstantiated()) {
        return;
    }

    // Seeding failures are logged, not swallowed. A silently unseeded Router
    // Object answers every read with nothing, which looks to ETS like a coupler
    // that has no configuration rather than a device with a wiring bug.
    const auto seed = [this](uint8_t pid, std::span<const uint8_t> bytes) {
        const auto result = _model.setReferenceObjectProperty(
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

util::Result<void> DeviceManagement::syncRouterRoutingConfig()
{
    if (!hasRouterRole() || _routerRole.routingPolicy == nullptr) {
        return util::Result<void>::ok();
    }
    // A property that was never written comes back empty. Leaving the policy
    // untouched in that case is deliberate: an absent value means "not
    // downloaded", and overwriting a working configuration with a zero byte
    // would be worse than ignoring it.
    const auto readByte = [this](uint8_t pid) -> std::optional<uint8_t> {
        const auto value = _model.referenceObjectPropertyValue(
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

    // The layer-2 group acknowledge is derived from the same configuration.
    if (_stackPort != nullptr) {
        if (auto* coupler = _stackPort->coupler(); coupler != nullptr && &coupler->policy() == &policy) {
            coupler->refreshAckFilters();
        }
    }

    KNX_LOGI(TAG, "Router routing configuration synced from Router Object");
    return util::Result<void>::ok();
}

#if KNX_FEATURE_NETIP

/**
 * @brief PID_SECURED_SERVICE_FAMILIES (03/08/09 §2.3.1.5)
 *
 * ETS drives this at the start of a secure download to say which KNXnet/IP
 * service families must be encrypted. It is a Function Property, so a plain
 * property write is not how it is reached — and answering "not a function
 * property" (E_DATA_TYPE_CONFLICT) left ETS with a bare ResultCode=FE in the
 * middle of a Download All.
 *
 * Requesting security is refused unless the product has said it can honour it.
 * Recording the requirement and answering Success would be the worse failure:
 * ETS would show an installation as secured while the frames went out in the
 * clear, and neither side would ever say otherwise.
 */
util::Result<application::FunctionPropertyResult> DeviceManagement::handleKnxNetIpFunctionProperty(
    const IndividualAddress& source,
    const application::FunctionPropertyRequest& request)
{
    using application::FunctionPropertyInvocation;
    using application::FunctionPropertyReturnCode;

    if (request.propertyId
            != static_cast<application::PropertyID>(kPidKnxNetIpSecuredServiceFamilies)) {
        return util::ErrorCode::OperationNotSupported;
    }

    const bool isCommand = (request.invocation == FunctionPropertyInvocation::Command);

    // §2.3.1.5.2: "Regardless of the device Security Mode, command access to
    // this Property shall only be possible using secure communication." Note
    // "regardless" — unlike the Security Interface Object's properties, this
    // one is not gated on the Security Mode being on. Reading is not
    // restricted that way; a client has to be able to discover the
    // requirements before it can meet them.
    if (isCommand && !request.security.toolSecured()) {
        KNX_LOGW(TAG,
                 "Refusing PID_SECURED_SERVICE_FAMILIES command from 0x%04X: "
                 "§2.3.1.5.2 requires secure communication",
                 source.raw);
        application::FunctionPropertyResult denied;
        denied.returnCode = FunctionPropertyReturnCode::AccessDenied;
        return denied;
    }

    const auto decoded = netip::decodeSecuredServiceFamiliesRequest(request.data.span(), isCommand);

    application::FunctionPropertyResult result;
    const auto answer = [&result, &decoded](FunctionPropertyReturnCode code) {
        result.returnCode = code;
        result.data.clear();
        if (decoded.hasServiceId) {
            (void)result.data.push_back(decoded.serviceId);
        }
        return result;
    };

    if (!decoded.ok()) {
        return answer(decoded.returnCode);
    }

    if (!isCommand) {
        // Figure 27: Return Code | ServiceID | ServiceFamilyID | SecurityVersion.
        answer(FunctionPropertyReturnCode::Success);
        (void)result.data.push_back(decoded.serviceFamilyId);
        (void)result.data.push_back(securedServiceFamilyVersion(decoded.serviceFamilyId));
        return result;
    }

    if (decoded.securityVersion == netip::kSecurityVersionRequired
            && !_securedServiceFamilyCallback) {
        // Nothing in this device can encrypt that family. Saying so is the
        // whole point of the branch: recording the requirement and answering
        // Success would leave ETS showing an installation as secured while the
        // frames went out in the clear, and neither side would ever say
        // otherwise. E_ERROR is Table 4's "failed, no closer indication".
        KNX_LOGW(TAG,
                 "0x%04X asked for security on KNXnet/IP service family 0x%02X, "
                 "which this device cannot enforce",
                 source.raw, static_cast<unsigned>(decoded.serviceFamilyId));
        return answer(FunctionPropertyReturnCode::Error);
    }

    if (_securedServiceFamilyCallback
            && !_securedServiceFamilyCallback(decoded.serviceFamilyId, decoded.securityVersion)) {
        return answer(FunctionPropertyReturnCode::Error);
    }

    setSecuredServiceFamilyVersion(decoded.serviceFamilyId, decoded.securityVersion);
    KNX_LOGI(TAG, "KNXnet/IP service family 0x%02X security version set to %u by 0x%04X",
             static_cast<unsigned>(decoded.serviceFamilyId),
             static_cast<unsigned>(decoded.securityVersion), source.raw);

    // Figure 24: Return Code | ServiceID.
    return answer(FunctionPropertyReturnCode::Success);
}

void DeviceManagement::setSecuredServiceFamilyCallback(ManagementControl::SecuredServiceFamilyCallback callback)
{
    _securedServiceFamilyCallback = std::move(callback);
}

uint8_t DeviceManagement::securedServiceFamilyVersion(uint8_t serviceFamilyId) const
{
    if (!netip::isSecurableServiceFamily(serviceFamilyId)) {
        return netip::kSecurityVersionPlain;
    }
    return _securedServiceFamilyVersions[netip::securableServiceFamilyIndex(serviceFamilyId)];
}

void DeviceManagement::setSecuredServiceFamilyVersion(uint8_t serviceFamilyId, uint8_t version)
{
    if (!netip::isSecurableServiceFamily(serviceFamilyId)) {
        return;
    }
    _securedServiceFamilyVersions[netip::securableServiceFamilyIndex(serviceFamilyId)] = version;
}

#endif  // KNX_FEATURE_NETIP

util::Result<application::FunctionPropertyResult> DeviceManagement::handleRouterFunctionProperty(
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

void DeviceManagement::wireManagementServices()
{
    publishMediumInterfaceObjects();

    if (_stackPort == nullptr) {
        return;
    }

    // --- Function Properties ----------------------------------------------
    _stackPort->setFunctionPropertyHandler(
        [this](const IndividualAddress& source,
               const FunctionPropertyRequest& request) -> util::Result<FunctionPropertyResult> {
            const auto objectType = _model.objectTypeForIndex(request.objectIndex);
            if (!objectType.has_value()) {
                return util::ErrorCode::InvalidAddress;
            }

#if KNX_FEATURE_NETIP
            if (*objectType == InterfaceObjectType::knxNetIpParameter()) {
                return handleKnxNetIpFunctionProperty(source, request);
            }
#endif
            if (*objectType == InterfaceObjectType::security()) {
                return handleSecurityFunctionProperty(_model.securityObject(), source, request);
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
#if KNX_FEATURE_NETIP
            if (objectType == InterfaceObjectType::knxNetIpParameter()) {
                result = handleKnxNetIpFunctionProperty(source, request);
            } else
#endif
            if (objectType == InterfaceObjectType::security()) {
                result = handleSecurityFunctionProperty(_model.securityObject(), source, request);
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
                //
                // Said out loud, because the alternative is decoding a bare
                // "ResultCode=FE" out of an ETS trace and then working out from
                // the object type which capability the client was asking for.
                // ETS drives KNXnet/IP Secure commissioning through function
                // properties of the KNXnet/IP Parameter Object (type 11), and
                // this device answers Secure Routing through PID_BACKBONE_KEY
                // writes instead — so a client that expects the other route
                // gets refused here and had, until this line, no way to know it
                // from the device.
                KNX_LOGW(TAG,
                         "No function property handler for object type %u PID %u "
                         "(from 0x%04X) - answering E_DATA_TYPE_CONFLICT",
                         static_cast<unsigned>(objectType.value()),
                         static_cast<unsigned>(request.propertyId),
                         source.raw);
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
            _model.flushPersistenceIfDirty();
            KNX_LOGI(TAG, "Restarting on %s request",
                     type == application::RestartType::MasterReset ? "master reset" : "basic restart");
            _platform.restart();
            return util::Result<void>::ok();
        });

    // Master Reset erase codes and Verify Mode both act on interface objects,
    // which live here rather than in the application layer.
    application::ApplicationLayer::ManagementServerHooks hooks;
    hooks.masterReset = [this](application::EraseCode eraseCode) {
        return applyMasterReset(eraseCode);
    };
    // 03/05/01 §4.2.14: Verify Mode is bit 2 of PID_DEVICE_CONTROL.
    hooks.verifyMode = [this]() {
        constexpr uint8_t kVerifyModeBit = 0x04u;
        return (_model.deviceObject().getDeviceControl() & kVerifyModeBit) != 0u;
    };
    // The A_Key_Write key table. Saved on the spot: the A_Key_Response that
    // follows tells the client the key is set, and a key lost to a restart
    // before the next coalesced save would leave the client locked out of the
    // level it believes it protected — or the level open that it closed.
    hooks.authorizationKeysChanged = [this](const application::AuthorizationService::KeyTable& keys) {
        _model.saveAuthorizationKeyTable(keys);
    };
    _stackPort->setManagementServerHooks(std::move(hooks));

#if KNX_FEATURE_NETIP
    // 03/03/07 §3.3.7: on KNX IP plain the Domain Address is the routing
    // multicast address, so A_DomainAddressSerialNumber_Write is a second way
    // of writing PID_ROUTING_MULTICAST_ADDRESS and goes down the same path:
    // stored, persisted, E11 when it changes, active after the next restart.
    // The service only hands over a write whose width matches the Domain
    // Address in force, which is empty until a KNX IP product adopts its
    // group (adoptRoutingMulticastAddress), so a TP1 device never gets here.
    _stackPort->setDomainAddressWriteCallback([this](std::span<const uint8_t> domainAddress) {
        return writeRoutingMulticastAddress(domainAddress);
    });
#endif

    // --- Serial-number-addressed commissioning ----------------------------
    // PID_SERIAL_NUMBER already carries the device's KNX serial number (this
    // stack derives it from the MAC), so the commissioning service can simply
    // adopt it rather than introducing a second source of truth.
    const auto serial = _model.deviceObject().getSerialNumber();
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

void DeviceManagement::setDeviceMedium(MediumType medium)
{
    _deviceMedium = medium;
    if (_stackPort != nullptr) {
        _stackPort->setDeviceMedium(medium);
    }
}

#if KNX_FEATURE_NETIP
void DeviceManagement::setBackboneKeyCallback(ManagementControl::BackboneKeyCallback callback)
{
    _backboneKeyCallback = std::move(callback);
}

void DeviceManagement::setRoutingMulticastAddressCallback(ManagementControl::RoutingMulticastAddressCallback callback)
{
    _routingMulticastAddressCallback = std::move(callback);
}

IpAddress DeviceManagement::adoptRoutingMulticastAddress(IpAddress productDefault, bool productDefaultWins)
{
    _routingMulticastDefault = productDefault;

    const auto pid = static_cast<application::PropertyID>(kPidKnxNetIpRoutingMulticastAddress);
    const auto stored = _model.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(), pid);
    // A stored value that is not a multicast group cannot be here: the write
    // validator refuses it on restore as on any other write, and the
    // property then reads as zeros, which fails this test too.
    const bool haveStored = isRoutingMulticastAddress(stored);

    const IpAddress inForce =
        (haveStored && !productDefaultWins) ? ipAddressFromOctets(stored) : productDefault;
    const auto octets = ipOctets(inForce);

    if (!haveStored || !std::equal(octets.begin(), octets.end(), stored.begin(), stored.end())) {
        // Through the seeding path, not the write path: this is the device
        // settling its own configuration, not ETS joining it to a new domain.
        // Persisted now, so the group a device first came up on stays its
        // group until ETS, the product or a Factory Reset changes it - a
        // firmware update with a different default does not move a device
        // off the installation it was commissioned into.
        // Saved at once rather than at the next quiet window: a product
        // that asked for its own group to win clears that request as soon
        // as start-up returns, and a restart before the deferred save would
        // then come back on the old group.
        if (_model.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(), pid, octets).isOk()) {
            _model.markPersistenceDirty();
            _model.flushPersistenceIfDirty();
        }
    }

    // 03/03/07 §3.3.7: "For KNX IP plain this shall be a 4 octet value that
    // shall consist of the IP multicast address only." The Domain Address and
    // the routing multicast address are one value on this medium, so they are
    // set from one place rather than left to drift apart.
    setDomainAddress(octets);
    return inForce;
}

IpAddress DeviceManagement::routingMulticastAddress() const
{
    const auto stored = _model.referenceObjectPropertyValue(
        InterfaceObjectType::knxNetIpParameter(),
        static_cast<application::PropertyID>(kPidKnxNetIpRoutingMulticastAddress));
    return isRoutingMulticastAddress(stored) ? ipAddressFromOctets(stored) : IpAddress(0);
}

util::Result<void> DeviceManagement::writeRoutingMulticastAddress(std::span<const uint8_t> value)
{
    if (!isRoutingMulticastAddress(value)) {
        return util::ErrorCode::InvalidParameter;
    }
    const auto pid = static_cast<application::PropertyID>(kPidKnxNetIpRoutingMulticastAddress);
    const auto previous = _model.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(), pid);
    auto stored = _model.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(), pid, value);
    if (stored.isError()) {
        return stored;
    }
    _model.markPersistenceDirty();
    onRoutingMulticastAddressWritten(previous, value);
    return util::Result<void>::ok();
}

void DeviceManagement::onRoutingMulticastAddressWritten(std::span<const uint8_t> previous,
                                                     std::span<const uint8_t> value)
{
    // Restoring the stored object at start-up writes through the same path,
    // and a restored address is the device's own configuration coming back,
    // not it joining a new domain.
    if (_model.restoring() || !isRoutingMulticastAddress(value)) {
        return;
    }

    // 03/03/07 §3.3.7 (see adoptRoutingMulticastAddress): one value.
    setDomainAddress(value);

    // 03_08_09 §2.2.2.3.2.3: "Setting this to the same value as before
    // generated event E12", for which the transition table has no entry.
    if (std::equal(previous.begin(), previous.end(), value.begin(), value.end())) {
        return;
    }

    const IpAddress address = ipAddressFromOctets(value);
    KNX_LOGI(TAG, "Routing multicast address set to %u.%u.%u.%u; active after the next restart "
                  "(03_08_03 §2.5.17)",
             static_cast<unsigned>(value[0]), static_cast<unsigned>(value[1]),
             static_cast<unsigned>(value[2]), static_cast<unsigned>(value[3]));

    // E11: "Device joins new domain. PID_BACKBONE_KEY and/or
    // PID_MULTICAST_ADDRESS is set to a new and different value (via property
    // access or DomainAddressSerialNumber_Write)."
    if (_routingMulticastAddressCallback) {
        _routingMulticastAddressCallback(address);
    }
}

util::Result<void> DeviceManagement::applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config)
{
    const auto objectType = InterfaceObjectType::knxNetIpParameter();

    const auto write = [&](uint8_t propertyId, std::span<const uint8_t> value) {
        return _model.setReferenceObjectProperty(objectType,
                                          static_cast<application::PropertyID>(propertyId),
                                          value);
    };

    // Failures here are reported rather than fatal: the object is registered by
    // publishMediumInterfaceObjects() and a build that never published it
    // simply has nowhere to put these, which is not a reason to refuse to run.
    util::Result<void> firstError = util::Result<void>::ok();
    const auto record = [&firstError](util::Result<void> result) {
        if (result.isError() && firstError.isOk()) {
            firstError = result;
        }
    };

    record(write(kPidKnxNetIpCurrentIpAddress, ipOctets(config.currentIpAddress)));
    record(write(kPidKnxNetIpCurrentSubnetMask, ipOctets(config.currentSubnetMask)));
    record(write(kPidKnxNetIpCurrentDefaultGateway, ipOctets(config.currentDefaultGateway)));
    record(write(kPidKnxNetIpSystemSetupMulticastAddress, ipOctets(config.systemSetupMulticastAddress)));

    const std::array<uint8_t, 1> ttl{config.timeToLive};
    record(write(kPidKnxNetIpTtl, ttl));

    const std::array<uint8_t, 1> assignment{config.ipAssignmentMethod};
    record(write(kPidKnxNetIpCurrentIpAssignmentMethod, assignment));
    record(write(kPidKnxNetIpIpAssignmentMethod, assignment));

    record(write(kPidKnxNetIpDeviceCapabilities, beU16(config.deviceCapabilities)));

    // The MAC address is a distinct property from the KNX serial number even
    // where a device derives one from the other; seeding it from the serial, as
    // this used to, reports a plausible wrong answer to anything that asks.
    record(write(kPidKnxNetIpMacAddress, config.macAddress));

    if (!config.friendlyName.empty()) {
        // 30 octets on the wire (03/08/02 §7.5.4.2); a longer name is trimmed
        // rather than refused, because a name is not worth failing a boot over.
        constexpr size_t kFriendlyNameLength = 30;
        const size_t length = std::min(config.friendlyName.size(), kFriendlyNameLength);
        record(write(kPidKnxNetIpFriendlyName,
                     std::span<const uint8_t>(
                         reinterpret_cast<const uint8_t*>(config.friendlyName.data()), length)));
    }

    return firstError;
}
#endif  // KNX_FEATURE_NETIP


// ─── ManagementControl facet ─────────────────────────────────────────────────

void ManagementControl::setDeviceMedium(MediumType medium) { _management.setDeviceMedium(medium); }
util::Result<void> ManagementControl::setCommissioningMode(bool enabled) { return _management.setCommissioningMode(enabled); }
bool ManagementControl::commissioningModeEnabled() const { return _management.commissioningModeEnabled(); }
util::Result<void> ManagementControl::configureRouterRole(const RouterRoleConfig& config) { return _management.configureRouterRole(config); }
util::Result<void> ManagementControl::syncRouterRoutingConfig() { return _management.syncRouterRoutingConfig(); }
bool ManagementControl::hasRouterRole() const { return _management.hasRouterRole(); }

util::Result<void> ManagementControl::setReferenceObjectProperty(InterfaceObjectType objectType,
                                                                 application::PropertyID propertyId,
                                                                 std::span<const uint8_t> value)
{
    return _management.model().setReferenceObjectProperty(objectType, propertyId, value);
}

std::vector<uint8_t> ManagementControl::referenceObjectPropertyValue(InterfaceObjectType objectType,
                                                                     application::PropertyID propertyId) const
{
    return _management.model().referenceObjectPropertyValue(objectType, propertyId);
}

void ManagementControl::setReferenceInterfaceObjectTypes(std::vector<InterfaceObjectType> objectTypes)
{
    _management.model().setReferenceObjectTypes(std::move(objectTypes));
}

const std::vector<InterfaceObjectType>& ManagementControl::referenceInterfaceObjectTypes() const
{
    return _management.model().referenceObjectTypes();
}

#if KNX_FEATURE_NETIP
util::Result<void> ManagementControl::applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config)
{
    return _management.applyKnxNetIpConfiguration(config);
}
void ManagementControl::setBackboneKeyCallback(BackboneKeyCallback callback)
{
    _management.setBackboneKeyCallback(std::move(callback));
}
void ManagementControl::setSecuredServiceFamilyCallback(SecuredServiceFamilyCallback callback)
{
    _management.setSecuredServiceFamilyCallback(std::move(callback));
}
uint8_t ManagementControl::securedServiceFamilyVersion(uint8_t serviceFamilyId) const
{
    return _management.securedServiceFamilyVersion(serviceFamilyId);
}
IpAddress ManagementControl::adoptRoutingMulticastAddress(IpAddress productDefault, bool productDefaultWins)
{
    return _management.adoptRoutingMulticastAddress(productDefault, productDefaultWins);
}
IpAddress ManagementControl::routingMulticastAddress() const { return _management.routingMulticastAddress(); }
void ManagementControl::setRoutingMulticastAddressCallback(RoutingMulticastAddressCallback callback)
{
    _management.setRoutingMulticastAddressCallback(std::move(callback));
}
#endif

bool ConstManagementControl::commissioningModeEnabled() const { return _management.commissioningModeEnabled(); }
bool ConstManagementControl::hasRouterRole() const { return _management.hasRouterRole(); }
const std::vector<InterfaceObjectType>& ConstManagementControl::referenceInterfaceObjectTypes() const
{
    return _management.model().referenceObjectTypes();
}
#if KNX_FEATURE_NETIP
IpAddress ConstManagementControl::routingMulticastAddress() const { return _management.routingMulticastAddress(); }
#endif

} // namespace bau
} // namespace knx
