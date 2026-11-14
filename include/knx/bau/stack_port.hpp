// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file stack_port.hpp
 * @brief The lower stack as the BAU sees it: one concrete class.
 *
 * BusAccessStackPort owns the medium (physical + data link), the network,
 * transport and application layers above it, the Data Secure application
 * layer when the build has one, and — for a coupler — the second medium and
 * the routing engine between the two. The BAU reaches the lower stack only
 * through this class.
 *
 * There is one implementation and no test double, so it is a plain final
 * class rather than an interface. The medium is opaque here (it is defined in
 * bau_stack_port.cpp) and the layers are forward-declared, so including this
 * header pulls in none of the network/coupler/security machinery.
 *
 * Normally reached through bau.hpp, which includes it at its end.
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/util/fixed_vector.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace knx {

namespace network {
class NetworkLayer;
class TwoPortCoupler;
}

namespace security {
class SecureApplicationLayer;
}

namespace bau {

class BusAccessStackPort final {
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
    using PropertyReadProvider = application::PropertyReadProvider;
    using PropertyWriteProvider = application::PropertyWriteProvider;
    using PropertyDescriptionProvider = application::PropertyDescriptionProvider;

    /// One medium: a physical layer and the data link layer over it. Defined
    /// in bau_stack_port.cpp; only the create*StackPort factories make one.
    class Medium;

    /// Build a stack port over @p primary. A non-null @p secondary makes it a
    /// coupler routing between the two. Use the create*StackPort factories.
    BusAccessStackPort(platform::Platform& platform,
                       std::unique_ptr<Medium> primary,
                       std::unique_ptr<Medium> secondary = nullptr);
    ~BusAccessStackPort();

    // The secure transforms installed on the transport capture `this`.
    BusAccessStackPort(const BusAccessStackPort&) = delete;
    BusAccessStackPort& operator=(const BusAccessStackPort&) = delete;
    BusAccessStackPort(BusAccessStackPort&&) = delete;
    BusAccessStackPort& operator=(BusAccessStackPort&&) = delete;

    util::Result<void> init(const IndividualAddress& ownAddress);
    void close();
    void executePendingRestart();
    /// Install what actually performs A_Restart. Without it the restart service
    /// refuses every request and the device answers a master reset with an
    /// error code instead of restarting.
    void setRestartHandler(application::RestartService::RestartCallback handler);
    /// Install the device-side Resources behind Master Reset erase codes and
    /// Verify Mode (see ApplicationLayer::ManagementServerHooks).
    void setManagementServerHooks(application::ApplicationLayer::ManagementServerHooks hooks);
    void processBackgroundWork();
    util::Result<void> sendDataLinkFrame(const datalink::LDataFrame& frame);
    void setDataLinkPromiscuousMode(datalink::PromiscuousMode mode);
    util::Result<void> addGroupAddress(const GroupAddress& address);
    util::Result<void> setOwnAddress(const IndividualAddress& ownAddress);
    void setIndividualAddressUpdateCallback(application::ApplicationLayer::IndividualAddressUpdateCallback callback);
    void configureDataSecure(objects::SecurityInterfaceObject& securityObject);
    /// Install the handler for A_FunctionPropertyCommand / State_Read.
    void setFunctionPropertyHandler(application::FunctionPropertyHandler handler);
    /// Install the handler for A_FunctionPropertyExtCommand / ExtState_Read.
    /// Separate from the classic hook because the extended services address the
    /// object by type + instance rather than by index, and answer in the
    /// unified return-code space of 03/03/07 §3.4.8.3 rather than the small
    /// FunctionPropertyReturnCode set.  ETS drives Data Secure through these.
    void setExtendedFunctionPropertyProvider(application::PropertyExtServices::FunctionProvider provider);
    /// Publish this device's 6-octet KNX serial number so it can be addressed
    /// by A_IndividualAddressSerialNumber_Read/_Write without programming mode.
    void setCommissioningSerialNumber(const application::KnxSerialNumber& serialNumber);
    /// The device's Domain Address in its medium's encoding; see
    /// application::NetworkParameterService::setDomainAddress.
    void setDomainAddress(std::span<const uint8_t> domainAddress);
    /// Adopt the authorisation key table restored from storage (03/03/07
    /// §3.5.8).
    void setAuthorizationKeys(const application::AuthorizationService::KeyTable& keys);
    void setDomainAddressWriteCallback(application::DomainAddressWriteCallback callback);
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
    void setDeviceMedium(MediumType medium);
    void setInboundCallback(InboundCallback callback);
    void registerPropertyObject(InterfaceObjectType objectType, InterfaceObjectIndex objectIndex);
    void setPropertyReadProvider(PropertyReadProvider provider);
    void setPropertyWriteProvider(PropertyWriteProvider provider);
    void setPropertyDescriptionProvider(PropertyDescriptionProvider provider);
    /// Register a memory-mapped region backed by caller-owned storage (used by
    /// the ETS SystemB download: table segments transferred via A_Memory_Write).
    util::Result<void> registerMemoryRegion(const application::MemoryRegion& region,
                                            std::span<uint8_t> storage,
                                            application::MemoryWriteGuard writeGuard = {});
    /// Whether a management client has ever written into the region starting
    /// at @p regionStart. Distinguishes "downloaded all zeros" from "never
    /// downloaded" — the backing buffer reads zero in both cases.
    bool memoryRegionWritten(MemoryAddress regionStart) const;
    void setWorkAvailableCallback(WorkAvailableCallback callback);
    util::Result<void> sendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValueWrite(const GroupAddress& address, std::span<const uint8_t> data);
    util::Result<void> sendGroupValueRead(const GroupAddress& address);
    util::Result<void> beginSendGroupValueRead(const GroupAddress& address);
    util::Result<TransmissionProgressState> pollTransmissionProgress();
    bool transmissionInProgress() const;
    void setDefaultTransmissionOptions(const TransmissionOptions& options);
    const TransmissionOptions& defaultTransmissionOptions() const { return _defaultTransmissionOptions; }
    bool popTransmissionOutcome(TransmissionOutcome& outcome);
    size_t queuedTransmissionOutcomeCount() const;
    util::Result<void> sendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValueResponse(const GroupAddress& address, std::span<const uint8_t> data);
    void setProgrammingModeEnabled(bool enabled);
    bool programmingModeEnabled() const;

    /**
     * @brief The coupler, when this stack port was built as one.
     *
     * Returns nullptr for an ordinary single-port device, and for a coupler
     * before init() (its role depends on the individual address). Use it to
     * reach the filter table and the routing policy — see
     * `network::TwoPortCoupler`.
     */
    network::TwoPortCoupler* coupler() { return _coupler.get(); }

    /**
     * @brief Re-issue the multicast membership of an IP routing port.
     *
     * OperationNotSupported for every port that is not KNXnet/IP Routing,
     * which is the honest answer for a TP1 device rather than a silent
     * success. See `physical::IpRoutingPhysical::refreshMulticastMembership()`
     * for why a host has to ask for this at all.
     */
    util::Result<void> refreshMulticastMembership();

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
    LinkDiagnostics linkDiagnostics() const;

private:
    util::Result<void> initApplicationStack(datalink::Tp1DataLinkLayer& dataLink,
                                            const IndividualAddress& ownAddress);
    util::Result<void> initCoupler(const IndividualAddress& ownAddress,
                                   datalink::Tp1DataLinkLayer& primaryDataLink);
    void installInboundCallback();
#if KNX_SECURE_ENABLED
    void configureSecurity();
#endif

    platform::Platform& _platform;

    // Declaration order is destruction order in reverse: the application
    // stack goes first, then the coupler, then the media it sits on.
    std::unique_ptr<Medium> _medium;
    /// Present only for a coupler: the sub-line the coupler routes to.
    std::unique_ptr<Medium> _secondaryMedium;
    std::unique_ptr<network::TwoPortCoupler> _coupler;

    std::unique_ptr<network::NetworkLayer> _network;
    std::unique_ptr<transport::TransportLayer> _transport;
    std::unique_ptr<application::ApplicationLayer> _application;
#if KNX_SECURE_ENABLED
    std::unique_ptr<security::SecureApplicationLayer> _secureAl;
    objects::SecurityInterfaceObject* _securityObject{nullptr};
    IndividualAddress _ownAddress{};
    application::KnxSerialNumber _serialNumber{};
#endif

    bool _initialized{false};
    bool _programmingModeEnabled{false};
    util::FixedVector<GroupAddress, datalink::Tp1DataLinkLayer::MAX_GROUP_ADDRESSES> _groupAddresses;
    InboundCallback _inboundCallback;
    WorkAvailableCallback _workAvailableCallback;
    TransmissionOptions _defaultTransmissionOptions{};
};

} // namespace bau
} // namespace knx
