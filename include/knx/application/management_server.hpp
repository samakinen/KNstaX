// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file management_server.hpp
 * @brief The device's management server: the point-to-point and broadcast
 *        management services of 03/03/07 and their responses.
 *
 * ApplicationLayer decodes a request, applies the communication-mode filter
 * and the Access Policy, and hands it here with a RequestContext saying how it
 * arrived. The server decodes the service's own fields, runs the service
 * object, and encodes the answer back through the ASendPipeline — on the
 * transport connection when the request came on one.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/application/a_send_pipeline.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/address_space.hpp"
#include "knx/application/authorization_service.hpp"
#include "knx/application/device_descriptor.hpp"
#include "knx/application/device_descriptor_service.hpp"
#include "knx/application/function_property_services.hpp"
#include "knx/application/memory_service.hpp"
#include "knx/application/network_parameter_service.hpp"
#include "knx/application/property_ext_services.hpp"
#include "knx/application/property_services.hpp"
#include "knx/application/restart_service.hpp"
#include "knx/config.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace knx {
namespace application {

/**
 * @brief Application layer frame structure
 */
struct ADataFrame {
    IndividualAddress source;      ///< Source individual address
    GroupAddress destination;      ///< Destination address (group or individual)
    APCIService service;            ///< APCI service code
    util::FixedVector<uint8_t, config::MAX_APDU_LENGTH> data; ///< Service data payload (bounded)
    AddressType destinationType;    ///< Destination address type
    /// Low 6 bits of the APCI. Most services leave them zero, but some encode a
    /// parameter there — A_Restart puts restart_type in bit 0 — and masking to
    /// the service group would otherwise discard it.
    uint8_t apciData{0};
    /// How the carrying frame was secured, taken from the transport frame.
    /// The Access Policies of 03/4/1 §6.2 are decided on this.
    RequestSecurity security{};
};

/**
 * @brief How the request being served arrived.
 *
 * Decides how its answers leave: a request that came on the transport
 * connection (T_Data_Connected) is answered on it; every other one —
 * including a connectionless T_Data_Individual received while a connection
 * is open — is answered connectionless. The security decides every Access
 * Policy question the services ask about it.
 */
struct RequestContext {
    IndividualAddress source{0};
    bool connected{false};
    RequestSecurity security{};

    PointToPointMode responseMode() const {
        return connected ? PointToPointMode::Connected : PointToPointMode::Connectionless;
    }
};

class ManagementServer {
public:
    using IndividualAddressUpdateCallback = util::InplaceFunction<void(const IndividualAddress&), ::knx::config::callbackBytes(32)>;

    /**
     * @brief Reads this device's AD converter (03/03/07 §3.5.2).
     *
     * @param channel  channel_nr from the request (6 bits).
     * @param readCount number of consecutive conversions to sum.
     * @return the summed value, or nullopt for "wrong channel number" / a
     *         summation the device cannot produce — which §3.5.2 says is
     *         answered with read_count = 0 rather than with silence.
     *
     * Left unset, every channel answers "not available". That is still an
     * answer: A_ADC_Read arrives on a transport connection, and a management
     * client that gets nothing sits in its timeout and then tears the
     * connection down.
     */
    using AdcReadProvider =
        util::InplaceFunction<std::optional<uint16_t>(uint8_t channel, uint8_t readCount), ::knx::config::callbackBytes(32)>;

    /**
     * @brief Device Resources some management services act on but this server
     *        does not own; the BAU, which holds the interface objects, fills
     *        them in.
     */
    struct Hooks {
        /// Performs Erase Codes 02h…07h of a Master Reset (see RestartService).
        /// The authorisation keys are this server's and are reset here first.
        RestartService::MasterResetCallback masterReset;
        /// PID_DEVICE_CONTROL bit 2, which makes A_Memory_Write confirmed.
        MemoryService::VerifyModeProvider verifyMode;
        /// The authorisation key table changed (A_Key_Write, or a Master
        /// Reset); the owner of persistent storage saves it.
        AuthorizationService::KeyTableChangedCallback authorizationKeysChanged;
    };

    explicit ManagementServer(ASendPipeline& send);

    // The service objects hold callbacks capturing `this`.
    ManagementServer(const ManagementServer&) = delete;
    ManagementServer& operator=(const ManagementServer&) = delete;
    ManagementServer(ManagementServer&&) = delete;
    ManagementServer& operator=(ManagementServer&&) = delete;

    /// (Re)creates the service objects and wires their responses. Settings
    /// made through the setters below survive it; the service objects' own
    /// configuration does not.
    void init();

    /**
     * @brief Serve one request.
     *
     * @return false when @p frame is not a management service; the caller
     *         passes it on (to the group runtime) instead.
     */
    bool handle(const ADataFrame& frame, const RequestContext& context);

    /// 03/03/07 §3.5.7: "A current access level shall be valid until the
    /// connection is released".
    void connectionClosed(const IndividualAddress& partner);

    void setProgrammingModeEnabled(bool enabled) { _programmingModeEnabled = enabled; }
    bool programmingModeEnabled() const { return _programmingModeEnabled; }
    void setServiceResponseSendOptions(const SendOptions& options) { _serviceResponseSendOptions = options; }
    void setIndividualAddressUpdateCallback(IndividualAddressUpdateCallback callback) {
        _individualAddressUpdateCallback = std::move(callback);
    }
    void setMemoryWriteAccessLevel(uint8_t level) { _memoryWriteAccessLevel = level; }
    void setAdcReadProvider(AdcReadProvider provider) { _adcReadProvider = std::move(provider); }
    void setHooks(Hooks hooks);

    void registerPropertyObject(InterfaceObjectType objectType, InterfaceObjectIndex objectIndex);
    void setPropertyReadProvider(PropertyReadProvider provider);
    void setPropertyWriteProvider(PropertyWriteProvider provider);
    void setPropertyDescriptionProvider(PropertyDescriptionProvider provider);
    util::Result<void> registerMemoryRegion(const MemoryRegion& region, std::span<uint8_t> storage,
                                            MemoryWriteGuard writeGuard = {});

    DeviceDescriptorService& deviceDescriptorService() { return *_deviceDescriptorService; }
    PropertyServices& propertyServices() { return *_propertyServices; }
#if KNX_FEATURE_PROPERTY_EXT
    PropertyExtServices& propertyExtServices() { return *_propertyExtServices; }
#endif
    MemoryService& memoryService() { return *_memoryService; }
    AuthorizationService& authorizationService() { return *_authorizationService; }
    RestartService& restartService() { return *_restartService; }
#if KNX_FEATURE_FUNCTION_PROPERTIES
    FunctionPropertyServices& functionPropertyServices() { return *_functionPropertyServices; }
#endif
    NetworkParameterService& networkParameterService() { return *_networkParameterService; }
    AddressSpace& addressSpace() { return *_addressSpace; }

private:
    void wireServiceCallbacks();
    void wireAccessPolicy();
    /// Runs the handler for @p frame's service; false when there is none.
    bool dispatch(const ADataFrame& frame);

    /// The request being served, while handle() runs; nullptr otherwise. The
    /// service objects report their answers through callbacks that carry only
    /// the destination, so this is how those answers learn how to leave.
    const RequestContext* _activeRequest{nullptr};
    PointToPointMode responseMode() const;
    RequestSecurity requestSecurity() const;

    /// A management server response, sent with the service response options
    /// on the active request's path. A failed send is logged: nothing retries it.
    util::Result<void> sendResponse(IndividualAddress destination, APCIService service,
                                    std::span<const uint8_t> data);
    void sendServiceResponse(IndividualAddress destination, APCIService service, std::span<const uint8_t> data);
    util::Result<void> sendBroadcast(APCIService service, std::span<const uint8_t> data);
    void broadcastServiceResponse(APCIService service, std::span<const uint8_t> data);

    /// Access Policy gate for the properties of one interface object, used by
    /// the four property services once they have resolved what is addressed.
    static bool isPropertyAccessPermitted(uint16_t objectType, uint16_t propertyId, bool write,
                                          const RequestSecurity& security);

    /// Object type behind an object *index*, for the classic services that
    /// address by index. nullopt when no such object is registered.
    std::optional<uint16_t> objectTypeForIndex(InterfaceObjectIndex objectIndex) const;

    void changeOwnAddress(const IndividualAddress& newAddress);

    void handleDeviceDescriptorRead(const ADataFrame& frame);
    void handlePropertyValueRead(const ADataFrame& frame);
    void handlePropertyValueWrite(const ADataFrame& frame);
    void handlePropertyDescriptionRead(const ADataFrame& frame);
#if KNX_FEATURE_FUNCTION_PROPERTIES
    void handleFunctionProperty(const ADataFrame& frame);
#endif
    void handleIndividualAddressSerialNumberRead(const ADataFrame& frame);
    void handleIndividualAddressSerialNumberWrite(const ADataFrame& frame);
    void handleDomainAddressSerialNumberRead(const ADataFrame& frame);
    void handleDomainAddressSerialNumberWrite(const ADataFrame& frame);
#if KNX_FEATURE_NETWORK_PARAMETER
    void handleNetworkParameterRead(const ADataFrame& frame);
    void handleNetworkParameterWrite(const ADataFrame& frame);
#endif
    void handleMemoryRead(const ADataFrame& frame);
    void handleMemoryWrite(const ADataFrame& frame);
#if KNX_FEATURE_MEMORY_EXT
    void handleMemoryExtendedRead(const ADataFrame& frame);
    void handleMemoryExtendedWrite(const ADataFrame& frame);
#endif
#if KNX_FEATURE_PROPERTY_EXT
    void handlePropertyExtValueRead(const ADataFrame& frame);
    void handlePropertyExtValueWrite(const ADataFrame& frame, bool confirmed);
    void handlePropertyExtDescriptionRead(const ADataFrame& frame);
#endif
#if KNX_FEATURE_PROPERTY_EXT && KNX_FEATURE_FUNCTION_PROPERTIES
    void handleFunctionPropertyExt(const ADataFrame& frame, FunctionPropertyInvocation invocation);
#endif
    void handleAuthorizeRequest(const ADataFrame& frame);
    void handleKeyWrite(const ADataFrame& frame);
    void handleRestart(const ADataFrame& frame);
    void handleAdcRead(const ADataFrame& frame);
    void handleIndividualAddressRead(const ADataFrame& frame);
    void handleSystemNetworkParameterRead(const ADataFrame& frame);
    void handleIndividualAddressWrite(const ADataFrame& frame);

    ASendPipeline& _send;

    // Service handler dependencies
    std::unique_ptr<DeviceDescriptor> _deviceDescriptor;
    std::unique_ptr<AddressSpace> _addressSpace;

    /// Index -> type of every object announced through registerPropertyObject().
    struct RegisteredObject {
        InterfaceObjectIndex index;
        InterfaceObjectType type;
    };
    static constexpr size_t kMaxInterfaceObjects = 32;
    util::FixedVector<RegisteredObject, kMaxInterfaceObjects> _registeredObjects;

    // Memory-mapped regions registered via registerMemoryRegion(); the memory
    // service read/write callbacks route A_Memory accesses into these spans.
    struct MemorySegmentBinding {
        MemoryRegion region;
        std::span<uint8_t> storage;
        /// Refuses writes while it returns false (see MemoryWriteGuard).
        MemoryWriteGuard writeGuard{};
    };
    static constexpr size_t kMaxMemorySegments = 8;
    util::FixedVector<MemorySegmentBinding, kMaxMemorySegments> _memorySegments;

    // Service handlers (created in init())
    std::unique_ptr<DeviceDescriptorService> _deviceDescriptorService;
    std::unique_ptr<PropertyServices> _propertyServices;
#if KNX_FEATURE_PROPERTY_EXT
    std::unique_ptr<PropertyExtServices> _propertyExtServices;
#endif
    std::unique_ptr<MemoryService> _memoryService;
    std::unique_ptr<AuthorizationService> _authorizationService;
    std::unique_ptr<RestartService> _restartService;
#if KNX_FEATURE_FUNCTION_PROPERTIES
    std::unique_ptr<FunctionPropertyServices> _functionPropertyServices;
#endif
    std::unique_ptr<NetworkParameterService> _networkParameterService;

    /// The current access level of whoever requested the pending Master
    /// Reset, taken when it was requested: 03/05/02 §3.7.1.2.3.2.3 resets the
    /// authorisation keys from that level up.
    AccessLevel _masterResetRequesterLevel{AuthorizationService::kMinimumAccessLevel};
    /// The owner's part of a Master Reset (Hooks::masterReset).
    RestartService::MasterResetCallback _masterResetHook{};

    IndividualAddressUpdateCallback _individualAddressUpdateCallback{};
    AdcReadProvider _adcReadProvider{};
    uint8_t _memoryWriteAccessLevel{2};
    bool _programmingModeEnabled{false};
    SendOptions _serviceResponseSendOptions{};
};

} // namespace application
} // namespace knx
