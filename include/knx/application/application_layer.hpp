// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file application_layer.hpp
 * @brief KNX Application Layer - receive, filter and route
 *
 * Decodes the APDU of every frame the transport layer delivers, drops services
 * that arrived on a communication mode they are not defined for (03/03/07
 * Table 1) or that the Access Policy refuses (03/4/1 §6.2), and routes the
 * rest: management services to the ManagementServer, everything else (group
 * communication) to the receive callback. Sending goes through the
 * ASendPipeline, which both halves share.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/types.hpp"
#include "knx/application/a_send_pipeline.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/management_server.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace application {

/**
 * @brief Application layer: receive/dispatch, plus forwarders to the send
 *        pipeline and the management server.
 *
 * @thread_safety Most methods are NOT thread-safe. sendData() should be called from
 * the same thread that processes the transport layer. Callbacks are invoked from the
 * transport layer context. Service handler registration should be done during initialization.
 */
class ApplicationLayer {
public:
    using SendProgressState = util::OperationProgressState;
    using WorkAvailableCallback = util::InplaceFunction<void(), ::knx::config::callbackBytes(32)>;

    /**
     * @brief Callback for application data reception (for group value services)
     *
     * @param source Source individual address
     * @param destination Destination group address
     * @param service Service code
     * @param data Service data
     * @param destinationType Destination address type
     */
    using DataCallback = util::InplaceFunction<void(
        const IndividualAddress& source,
        const GroupAddress& destination,
        APCIService service,
        std::span<const uint8_t> data,
        AddressType destinationType
    ), ::knx::config::callbackBytes(64)>;
    using IndividualAddressUpdateCallback = ManagementServer::IndividualAddressUpdateCallback;

    /**
     * @brief Answers "is PID_SECURITY_MODE enabled?".
     *
     * The Access Policies have a Security Mode Off and a Security Mode On
     * column, so enforcing them needs the current mode. It lives in the
     * Security Interface Object, which the BAU owns — hence a callback rather
     * than a reference from here downwards.
     *
     * Left unset the layer assumes "off", which is what a stack built without
     * KNX Secure is.
     */
    using SecurityModeProvider = util::InplaceFunction<bool(), ::knx::config::callbackBytes(32)>;

    using AdcReadProvider = ManagementServer::AdcReadProvider;
    using ManagementServerHooks = ManagementServer::Hooks;

    explicit ApplicationLayer(transport::TransportLayer& transport);
    ~ApplicationLayer();

    // The transport layer holds callbacks capturing `this`.
    ApplicationLayer(const ApplicationLayer&) = delete;
    ApplicationLayer& operator=(const ApplicationLayer&) = delete;
    ApplicationLayer(ApplicationLayer&&) = delete;
    ApplicationLayer& operator=(ApplicationLayer&&) = delete;

    /**
     * @brief Initialize application layer
     *
     * @param ownAddress Own individual address
     * @return Result indicating success or failure
     */
    util::Result<void> init(const IndividualAddress& ownAddress);

    /**
     * @brief Close application layer
     */
    void close();

    // --- Sending (ASendPipeline) -------------------------------------------

    /**
     * @brief Send application data
     *
     * A frame to an individual address goes out connectionless; answers on a
     * transport connection are the management server's.
     *
     * @thread_safety NOT thread-safe - should be called from single thread
     */
    util::Result<void> sendData(const GroupAddress& destination, APCIService service,
                                std::span<const uint8_t> data, AddressType destinationType,
                                const SendOptions& options = {}) {
        return _send.sendData(destination, service, data, destinationType, options);
    }
    util::Result<void> beginSendData(const GroupAddress& destination, APCIService service,
                                     std::span<const uint8_t> data, AddressType destinationType,
                                     const SendOptions& options = {}) {
        return _send.beginSendData(destination, service, data, destinationType, options);
    }
    util::Result<SendProgressState> pollSendData() { return _send.pollSendData(); }
    void processBackgroundWork() { _transport.processBackgroundWork(); }

    void setDefaultSendOptions(const SendOptions& options) { _defaultSendOptions = options; }
    bool popSendOutcome(ASendOutcome& outcome) { return _send.popSendOutcome(outcome); }
    size_t queuedSendOutcomeCount() const { return _send.queuedSendOutcomeCount(); }
    size_t droppedSendOutcomeCount() const { return _send.droppedSendOutcomeCount(); }
    bool transmissionInProgress() const { return _send.transmissionInProgress(); }
    void setOwnAddress(const IndividualAddress& ownAddress) { _send.setOwnAddress(ownAddress); }

    /// A_GroupValue_Write / _Read / _Response with the default send options.
    util::Result<void> sendGroupValueWrite(const GroupAddress& destination, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValueWrite(const GroupAddress& destination, std::span<const uint8_t> data);
    util::Result<void> sendGroupValueRead(const GroupAddress& destination);
    util::Result<void> beginSendGroupValueRead(const GroupAddress& destination);
    util::Result<void> sendGroupValueResponse(const GroupAddress& destination, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValueResponse(const GroupAddress& destination, std::span<const uint8_t> data);

    static SendOptions optionsForPreset(SendPolicyPreset preset) {
        return ASendPipeline::optionsForPreset(preset);
    }

    // --- Receiving ---------------------------------------------------------

    /**
     * @brief Set callback for application data reception
     *
     * Receives group value services and every other service the management
     * server does not serve.
     *
     * @thread_safety NOT thread-safe - should be called during initialization
     * @warning Callback is invoked from transport layer context - avoid blocking operations
     */
    void setReceiveCallback(DataCallback callback) { _rxCallback = std::move(callback); }
    void setReceiveWorkAvailableCallback(WorkAvailableCallback callback) {
        _receiveWorkAvailableCallback = std::move(callback);
    }

    /**
     * @brief Install the PID_SECURITY_MODE source used by the Access Policies.
     */
    void setSecurityModeProvider(SecurityModeProvider provider) {
        _securityModeProvider = std::move(provider);
    }

    // --- Management server -------------------------------------------------

    ManagementServer& managementServer() { return _management; }

    void setServiceResponseSendOptions(const SendOptions& options) {
        _management.setServiceResponseSendOptions(options);
    }
    void setProgrammingModeEnabled(bool enabled) { _management.setProgrammingModeEnabled(enabled); }
    bool programmingModeEnabled() const { return _management.programmingModeEnabled(); }
    void setIndividualAddressUpdateCallback(IndividualAddressUpdateCallback callback) {
        _management.setIndividualAddressUpdateCallback(std::move(callback));
    }

    DeviceDescriptorService& deviceDescriptorService() { return _management.deviceDescriptorService(); }
    PropertyServices& propertyServices() { return _management.propertyServices(); }
#if KNX_FEATURE_PROPERTY_EXT
    PropertyExtServices& propertyExtServices() { return _management.propertyExtServices(); }
#endif
    MemoryService& memoryService() { return _management.memoryService(); }
    AuthorizationService& authorizationService() { return _management.authorizationService(); }
    RestartService& restartService() { return _management.restartService(); }
    /// PDT_FUNCTION properties install their handler here; it is the path ETS
    /// uses to switch KNX Data Secure on.
#if KNX_FEATURE_FUNCTION_PROPERTIES
    FunctionPropertyServices& functionPropertyServices() { return _management.functionPropertyServices(); }
#endif
    NetworkParameterService& networkParameterService() { return _management.networkParameterService(); }
    /// Exposed so higher layers and integration tests can configure memory
    /// regions for Memory Services.
    AddressSpace& addressSpace() { return _management.addressSpace(); }

    /**
     * @brief Announce the interface object at @p objectIndex
     *
     * Records the object's type. The classic property services address objects
     * by index and the extended ones by (type, instance); this mapping is what
     * lets both, and the Access Policy, name the same object.
     */
    void registerPropertyObject(InterfaceObjectType objectType, InterfaceObjectIndex objectIndex) {
        _management.registerPropertyObject(objectType, objectIndex);
    }
    void setPropertyReadProvider(PropertyReadProvider provider) {
        _management.setPropertyReadProvider(std::move(provider));
    }
    void setPropertyWriteProvider(PropertyWriteProvider provider) {
        _management.setPropertyWriteProvider(std::move(provider));
    }
    void setPropertyDescriptionProvider(PropertyDescriptionProvider provider) {
        _management.setPropertyDescriptionProvider(std::move(provider));
    }

    /**
     * @brief Register a memory-mapped region backed by caller-owned storage.
     *
     * Used by the ETS SystemB download procedure: loadable table segments are
     * exposed via PID_TABLE_REFERENCE and transferred with A_Memory_Write /
     * A_Memory_Read within the registered address range. The storage must
     * outlive the application layer. @p writeGuard, when set, is asked
     * before every write into the region.
     */
    util::Result<void> registerMemoryRegion(const MemoryRegion& region, std::span<uint8_t> storage,
                                            MemoryWriteGuard writeGuard = {}) {
        return _management.registerMemoryRegion(region, storage, std::move(writeGuard));
    }

    void setManagementServerHooks(ManagementServerHooks hooks) { _management.setHooks(std::move(hooks)); }

    /**
     * @brief Access level (wire numbering, 0 = maximum) a requester needs for
     *        A_Memory_Write and A_MemoryExtended_Write. Default 2, the write
     *        level 03/05/01 gives the configuration Resources. Only bites once
     *        a level has a key (A_Key_Write, or authorizationService().setKey()).
     */
    void setMemoryWriteAccessLevel(uint8_t level) { _management.setMemoryWriteAccessLevel(level); }

    /**
     * @brief Install the AD converter read-out behind A_ADC_Read.
     */
    void setAdcReadProvider(AdcReadProvider provider) { _management.setAdcReadProvider(std::move(provider)); }

private:
    /// Decode, filter and route one frame from the transport layer.
    void handleTransportRx(const transport::TDataFrame& frame);

    /**
     * @brief Access Policy gate applied to every inbound service (03/4/1 §6.2).
     *
     * Returns false when the request must not be executed. A denied request is
     * dropped rather than answered: the policies describe which indications are
     * "accepted", and a device that answers anyway tells an attacker exactly
     * which services exist and confirms it is listening.
     */
    bool isRequestPermitted(const ADataFrame& aFrame) const;

    void forwardToUser(const ADataFrame& frame);

    transport::TransportLayer& _transport;
    ASendPipeline _send;
    ManagementServer _management;

    DataCallback _rxCallback{};
    WorkAvailableCallback _receiveWorkAvailableCallback{};
    SecurityModeProvider _securityModeProvider{};
    SendOptions _defaultSendOptions{};

    bool _initialized{false};
};

} // namespace application
} // namespace knx
