// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_management.hpp
 * @brief The management services behind BusAccessUnit and ManagementControl.
 *
 * Private to the BAU. Everything a management client or an integrator changes
 * that is not plain property access: programming mode and the device-object
 * wiring to the stack, Master Reset, restart, the Function Properties
 * (Security Interface Object, PID_ROUTETABLE_CONTROL, PID_GO_DIAGNOSTICS,
 * PID_SECURED_SERVICE_FAMILIES), the coupler role, and the KNXnet/IP Parameter
 * Object with its routing multicast address and backbone key.
 *
 * @thread_safety The handlers installed by wireManagementServices() and
 * wireDeviceObject() run on the stack task that handles the request; the rest
 * is owner context.
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/application/function_property_services.hpp"

#include "knx/network/filter_table_load_control.hpp"

#include <array>
#include <memory>
#include <cstdint>
#include <span>
#include <vector>

namespace knx {
namespace bau {

class DeviceModel;
class GroupRuntime;

class DeviceManagement final {
public:
    using RouterRoleConfig = BusAccessUnit::RouterRoleConfig;

    DeviceManagement(platform::Platform& platform,
                     BusAccessStackPort* stackPort,
                     DeviceModel& model,
                     GroupRuntime& groups);

    // Installs handlers capturing `this` into the stack port, the device
    // object and the reference objects.
    DeviceManagement(const DeviceManagement&) = delete;
    DeviceManagement& operator=(const DeviceManagement&) = delete;
    DeviceManagement(DeviceManagement&&) = delete;
    DeviceManagement& operator=(DeviceManagement&&) = delete;

    DeviceModel& model() { return _model; }
    const DeviceModel& model() const { return _model; }

    // --- Start-up (driven by BusAccessUnit::init) ----------------------------

    void setRunning(bool running) { _running = running; }
    /// The address init() was given: the medium's default IA that Erase
    /// Codes 02h and 03h restore (03/05/02 Table 4).
    void setDefaultIndividualAddress(const IndividualAddress& address) { _defaultIndividualAddress = address; }
    /// Push the declared medium to the stack port (it may predate the port's init).
    void applyDeviceMedium();
    /// Keep the stack port and the Device Object in step: programming mode,
    /// serial number and individual address, in both directions.
    void wireDeviceObject();
    /// Install the Function Property, restart and Master Reset handlers and
    /// publish the medium's interface objects.
    void wireManagementServices();
    /// Register and seed the interface objects ETS expects for the medium this
    /// device runs on (KNXnet/IP Parameter Object for IP devices, Router
    /// Object for couplers), and hook the KNXnet/IP write observers.
    void publishMediumInterfaceObjects();

    // --- ManagementControl surface ---------------------------------------------

    void setDeviceMedium(MediumType medium);
    util::Result<void> setCommissioningMode(bool enabled);
    bool commissioningModeEnabled() const;
    util::Result<void> configureRouterRole(const RouterRoleConfig& config);
    util::Result<void> syncRouterRoutingConfig();
    bool hasRouterRole() const { return _routerRole.filterTable != nullptr; }
    network::FilterTableLoadControl* filterTableLoadControl() { return _filterTableLoad.get(); }

#if KNX_FEATURE_NETIP
    util::Result<void> applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config);
    void setBackboneKeyCallback(ManagementControl::BackboneKeyCallback callback);
    void setSecuredServiceFamilyCallback(ManagementControl::SecuredServiceFamilyCallback callback);
    uint8_t securedServiceFamilyVersion(uint8_t serviceFamilyId) const;
    IpAddress adoptRoutingMulticastAddress(IpAddress productDefault, bool productDefaultWins);
    IpAddress routingMulticastAddress() const;
    void setRoutingMulticastAddressCallback(ManagementControl::RoutingMulticastAddressCallback callback);
#endif

private:
    /// Reset the Resources @p eraseCode names and persist the result; the
    /// restart that follows boots into it.
    util::Result<void> applyMasterReset(application::EraseCode eraseCode);

    /// Register and seed the Router Object; called from
    /// publishMediumInterfaceObjects() when a coupler role is declared.
    void publishRouterObject();

    /// Master Reset 02h/07h: put the coupler configuration back to
    /// _routerFactoryConfig and apply it to the routing policy.
    void restoreRouterFactoryConfig();

    /// Restore the Filter Table and its load state from the persisted state,
    /// make them part of every save, bind the Router Object's
    /// PID_LOAD_STATE_CONTROL, PID_TABLE_REFERENCE, PID_ERROR_CODE
    /// and PID_MCB_TABLE to the Filter Table load state machine and register
    /// the table's memory window (03/05/01 §5.3.1).
    void bindFilterTableLoadControl();
    /// Follow a load state change in the forwarding path (§5.3.1.4).
    void applyFilterTableLoadState(bool loaded);

    util::Result<application::FunctionPropertyResult> handleRouterFunctionProperty(
        const application::FunctionPropertyRequest& request);

    /// PID_GO_DIAGNOSTICS on the Group Object Table Object (03/05/01 §4.8.1) —
    /// the service ETS uses to read or write a group address through the
    /// device, which is the only way it can touch a Data Secure group address
    /// it has no sending sequence number for. See go_diagnostics.cpp.
    util::Result<application::FunctionPropertyResult> handleGroupObjectDiagnostics(
        const application::FunctionPropertyRequest& request);

    /// Validate the Flags + Group Address of the two PID_GO_DIAGNOSTICS
    /// commands that address the bus rather than a group object number.
    /// Returns Success, or the E_DATA_VOID the spec prescribes.
    application::FunctionPropertyReturnCode validateGroupDiagnosticsFlags(
        uint8_t flags, const GroupAddress& destination, bool allowSelector) const;

    void setDomainAddress(std::span<const uint8_t> domainAddress);

#if KNX_FEATURE_NETIP
    util::Result<application::FunctionPropertyResult> handleKnxNetIpFunctionProperty(
        const IndividualAddress& source,
        const application::FunctionPropertyRequest& request);
    void setSecuredServiceFamilyVersion(uint8_t serviceFamilyId, uint8_t version);
    /// Store a routing multicast address written by A_DomainAddressSerialNumber_Write.
    util::Result<void> writeRoutingMulticastAddress(std::span<const uint8_t> value);
    /// After PID_ROUTING_MULTICAST_ADDRESS has changed by any remote path.
    void onRoutingMulticastAddressWritten(std::span<const uint8_t> previous,
                                          std::span<const uint8_t> value);
#endif

    platform::Platform& _platform;
    BusAccessStackPort* const _stackPort;
    DeviceModel& _model;
    GroupRuntime& _groups;
    bool _running{false};

    IndividualAddress _defaultIndividualAddress{0xFFFFu};
    /// Remembered so a stack port initialised after the declaration still gets it.
    MediumType _deviceMedium{MediumType::TP1};
    /// Coupler role, if declared.  A null filterTable means "end device".
    RouterRoleConfig _routerRole{};
    /// The Router Object's coupler configuration (PID_MAIN_LCCONFIG,
    /// PID_SUB_LCCONFIG, PID_MAIN_LCGRPCONFIG, PID_SUB_LCGRPCONFIG,
    /// PID_FILTER_TABLE_USE, in kRouterConfigPids order) as the product
    /// declared it in configureRouterRole(): the ex-factory state that seeds a
    /// never-downloaded coupler and that Master Reset 02h/07h return to.
    std::array<uint8_t, 5> _routerFactoryConfig{};
    /// Realisation Type 3 loading of _routerRole.filterTable; couplers only.
    std::unique_ptr<network::FilterTableLoadControl> _filterTableLoad{};

#if KNX_FEATURE_NETIP
    ManagementControl::BackboneKeyCallback _backboneKeyCallback{};
    ManagementControl::SecuredServiceFamilyCallback _securedServiceFamilyCallback{};
    ManagementControl::RoutingMulticastAddressCallback _routingMulticastAddressCallback{};
    /// What adoptRoutingMulticastAddress() was given; restored by Factory Reset.
    IpAddress _routingMulticastDefault{IpAddress(0)};
    /// Indexed by the three securable families; see kSecurableServiceFamilies.
    std::array<uint8_t, 3> _securedServiceFamilyVersions{};
#endif
};

} // namespace bau
} // namespace knx
