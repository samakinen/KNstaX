// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file application_layer.hpp
 * @brief KNX Application Layer - Service Dispatcher
 * 
 * Implements application layer service dispatching and routing.
 * Routes incoming APCI requests to appropriate service handlers.
 * Per KNX spec 03/03/07 (Application Layer).
 */

#pragma once

#include "knx/types.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/device_descriptor_service.hpp"
#include "knx/application/property_services.hpp"
#include "knx/application/property_ext_services.hpp"
#include "knx/application/function_property_services.hpp"
#include "knx/application/network_parameter_service.hpp"
#include "knx/application/memory_service.hpp"
#include "knx/application/authorization_service.hpp"
#include "knx/application/restart_service.hpp"
#include "knx/config.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include <array>
#include <cstddef>
#include <functional>
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

struct ASendOutcome {
    GroupAddress destination;       ///< Destination address (group or individual)
    APCIService service;            ///< APCI service code
    AddressType destinationType;    ///< Destination address type
    util::ErrorCode result;         ///< Lower-layer send result
    uint8_t attempts{0};            ///< Number of send attempts performed
};

struct SendOptions {
    uint8_t maxAttempts{1};
    bool retryOnBusy{true};
    bool retryOnTimeout{true};
    bool retryOnTransmissionFailed{false};
    Priority priority{Priority::Low};
};

enum class SendPolicyPreset : uint8_t {
    SingleAttempt = 0,
    RetryTransientOnce,
    RetryTransientTwice,
};

/**
 * @brief Application layer service dispatcher
 * 
 * Routes incoming service requests to appropriate handlers.
 * Manages response generation and callback routing.
 * 
 * @thread_safety Most methods are NOT thread-safe. sendData() should be called from 
 * the same thread that processes the transport layer. Callbacks are invoked from the 
 * transport layer context. Service handler registration should be done during initialization.
 */
class ApplicationLayer {
public:
    using SendProgressState = util::OperationProgressState;
    using WorkAvailableCallback = util::InplaceFunction<void(), 32>;

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
    ), 64>;
    using IndividualAddressUpdateCallback = util::InplaceFunction<void(const IndividualAddress&), 32>;
    using PropertyRegistration = std::function<void(PropertyStore& store)>;

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
    using SecurityModeProvider = util::InplaceFunction<bool(), 32>;

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
        util::InplaceFunction<std::optional<uint16_t>(uint8_t channel, uint8_t readCount), 32>;

    /**
     * @brief Constructor
     * 
     * @param transport Reference to transport layer
     */
    explicit ApplicationLayer(transport::TransportLayer& transport);

    /**
     * @brief Destructor
     */
    ~ApplicationLayer();

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

    /**
     * @brief Send application data
     * 
     * @param destination Destination address
     * @param service APCI service code
     * @param data Service data
    * @param destinationType Destination address type
    * @return Result indicating success or failure
     * @thread_safety NOT thread-safe - should be called from single thread
     */
    util::Result<void> sendData(const GroupAddress& destination, APCIService service,
                         std::span<const uint8_t> data, AddressType destinationType,
                          const SendOptions& options = {});
    util::Result<void> beginSendData(const GroupAddress& destination, APCIService service,
                             std::span<const uint8_t> data, AddressType destinationType,
                               const SendOptions& options = {});
    util::Result<SendProgressState> pollSendData();
        void processBackgroundWork();

    /**
     * @brief Set callback for application data reception
     * 
     * Used for group value services and other unhandled services.
     * 
     * @param callback Callback function
     * @thread_safety NOT thread-safe - should be called during initialization
     * @warning Callback is invoked from transport layer context - avoid blocking operations
     */
    void setReceiveCallback(DataCallback callback);
    void setReceiveWorkAvailableCallback(WorkAvailableCallback callback);
    void setDefaultSendOptions(const SendOptions& options) { _defaultSendOptions = options; }
    const SendOptions& defaultSendOptions() const { return _defaultSendOptions; }
    void setServiceResponseSendOptions(const SendOptions& options) { _serviceResponseSendOptions = options; }
    const SendOptions& serviceResponseSendOptions() const { return _serviceResponseSendOptions; }
    void setProgrammingModeEnabled(bool enabled) { _programmingModeEnabled = enabled; }
    bool programmingModeEnabled() const { return _programmingModeEnabled; }
    void setOwnAddress(const IndividualAddress& ownAddress);
    void setIndividualAddressUpdateCallback(IndividualAddressUpdateCallback callback);
    bool popReceivedFrame(ADataFrame& frame);
    size_t queuedReceiveCount() const { return _rxQueueCount; }
    size_t droppedReceiveFrameCount() const { return _droppedReceiveFrames; }
    bool popSendOutcome(ASendOutcome& outcome);
    size_t queuedSendOutcomeCount() const { return _txOutcomeQueueCount; }
    size_t droppedSendOutcomeCount() const { return _droppedSendOutcomes; }
    bool transmissionInProgress() const { return _sendOperation.active; }

    /**
     * @brief Send group value write
     * 
     * Convenience method for A_GroupValue_Write service.
     * 
     * @param destination Group address
     * @param data Value data to write
    * @return Result indicating success or failure
     */
    util::Result<void> sendGroupValueWrite(const GroupAddress& destination, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValueWrite(const GroupAddress& destination, std::span<const uint8_t> data);

    /**
     * @brief Send group value read request
     * 
     * Convenience method for A_GroupValue_Read service.
     * 
     * @param destination Group address
    * @return Result indicating success or failure
     */
    util::Result<void> sendGroupValueRead(const GroupAddress& destination);
    util::Result<void> beginSendGroupValueRead(const GroupAddress& destination);

    /**
     * @brief Send group value response
     * 
     * Convenience method for A_GroupValue_Response service.
     * 
     * @param destination Group address
     * @param data Value data to respond with
    * @return Result indicating success or failure
     */
    util::Result<void> sendGroupValueResponse(const GroupAddress& destination, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValueResponse(const GroupAddress& destination, std::span<const uint8_t> data);

    static SendOptions optionsForPreset(SendPolicyPreset preset);

    /**
     * @brief Get device descriptor service handler
     * @return Reference to device descriptor service
     */
    DeviceDescriptorService& deviceDescriptorService() { return *_deviceDescriptorService; }

    void registerPropertyObject(InterfaceObjectType objectType,
                                InterfaceObjectIndex objectIndex,
                                PropertyRegistration registration);
    void setPropertyReadProvider(PropertyReadProvider provider);
    void setPropertyWriteConsumer(PropertyWriteConsumer consumer);
    void setPropertyWriteProvider(PropertyWriteProvider provider);
    void setPropertyDescriptionProvider(PropertyDescriptionProvider provider);

    /// Whether any memory write has landed in the region starting at @p regionStart.
    /// False means the management client never downloaded that segment, which is
    /// not the same as downloading zeros.
    bool memoryRegionWritten(MemoryAddress regionStart) const;

    /**
     * @brief Get property services handler
     * @return Reference to property services
     */
    PropertyServices& propertyServices() { return *_propertyServices; }
    PropertyExtServices& propertyExtServices() { return *_propertyExtServices; }

    /**
     * @brief Get property store manager
     *
     * Exposed so higher layers (e.g. BAU/facade) can register interface objects
     * and properties that Property Services operate on.
     */
    PropertyStoreManager& propertyStoreManager() { return *_propertyStoreManager; }

    /**
     * @brief Get memory service handler
     * @return Reference to memory service
     */
    MemoryService& memoryService() { return *_memoryService; }

    /**
     * @brief Get authorization service handler
     * @return Reference to authorization service
     */
    AuthorizationService& authorizationService() { return *_authorizationService; }

    /**
     * @brief Get restart service handler
     * @return Reference to restart service
     */
    RestartService& restartService() { return *_restartService; }

    /**
     * @brief Get Function Property services handler (A_FunctionPropertyCommand
     *        / A_FunctionPropertyState_Read).
     *
     * Interface objects that expose PDT_FUNCTION properties install their
     * handler here.  This is the path ETS uses to switch KNX Data Secure on.
     */
    FunctionPropertyServices& functionPropertyServices() { return *_functionPropertyServices; }

    /**
     * @brief Get the serial-number / network-parameter broadcast service.
     */
    NetworkParameterService& networkParameterService() { return *_networkParameterService; }

    /**
     * @brief Get address space manager
     *
     * Exposed so higher layers and integration tests can configure memory regions
     * for Memory Services.
     */
    AddressSpace& addressSpace() { return *_addressSpace; }

    /**
     * @brief Register a memory-mapped region backed by caller-owned storage.
     *
     * Used by the ETS SystemB download procedure: loadable table segments are
     * exposed via PID_TABLE_REFERENCE and transferred with A_Memory_Write /
     * A_Memory_Read within the registered address range. The storage must
     * outlive the application layer.
     */
    util::Result<void> registerMemoryRegion(const MemoryRegion& region, std::span<uint8_t> storage);

    /**
     * @brief Install the PID_SECURITY_MODE source used by the Access Policies.
     */
    void setSecurityModeProvider(SecurityModeProvider provider) {
        _securityModeProvider = std::move(provider);
    }

    /**
     * @brief Install the AD converter read-out behind A_ADC_Read.
     */
    void setAdcReadProvider(AdcReadProvider provider) {
        _adcReadProvider = std::move(provider);
    }

private:
    /**
     * @brief Handle transport layer reception
     * 
     * @param frame Transport data frame
     */
    void handleTransportRx(const transport::TDataFrame& frame);

    /**
     * @brief Dispatch service request to appropriate handler
     * 
     * @param aFrame Application frame
     */
    void dispatchService(const ADataFrame& aFrame);

    /**
     * @brief Access Policy gate applied to every inbound service (03/4/1 §6.2).
     *
     * Returns false when the request must not be executed. A denied request is
     * dropped rather than answered: the policies describe which indications are
     * "accepted", and a device that answers anyway tells an attacker exactly
     * which services exist and confirms it is listening.
     */
    bool isRequestPermitted(const ADataFrame& aFrame) const;

    /// Access Policy gate for the properties of one interface object, used by
    /// the four property services once they have resolved what is addressed.
    bool isPropertyAccessPermitted(uint16_t objectType,
                                   uint16_t propertyId,
                                   bool write,
                                   const RequestSecurity& security) const;

    /// Object type behind an object *index*, for the classic services that
    /// address by index. nullopt when no such object is registered.
    std::optional<uint16_t> objectTypeForIndex(InterfaceObjectIndex objectIndex) const;

    // Transport layer reference
    transport::TransportLayer& _transport;

    // Own individual address
    IndividualAddress _ownAddress;

    // Service handler dependencies (owned by application layer)
    std::unique_ptr<DeviceDescriptor> _deviceDescriptor;
    std::unique_ptr<PropertyStoreManager> _propertyStoreManager;
    std::unique_ptr<AddressSpace> _addressSpace;

    // Memory-mapped regions registered via registerMemoryRegion(); the memory
    // service read/write callbacks route A_Memory accesses into these spans.
    struct MemorySegmentBinding {
        MemoryRegion region;
        std::span<uint8_t> storage;
        /// Set once a memory write lands anywhere in this region. Lets callers
        /// tell "ETS downloaded zeros" from "ETS never touched this segment" —
        /// the buffer reads all-zero either way.
        bool written{false};
    };
    static constexpr size_t kMaxMemorySegments = 8;
    util::FixedVector<MemorySegmentBinding, kMaxMemorySegments> _memorySegments;

    // Service handlers (initialized in init())
    std::unique_ptr<DeviceDescriptorService> _deviceDescriptorService;
    std::unique_ptr<PropertyServices> _propertyServices;
    std::unique_ptr<PropertyExtServices> _propertyExtServices;
    std::unique_ptr<MemoryService> _memoryService;
    std::unique_ptr<AuthorizationService> _authorizationService;
    std::unique_ptr<RestartService> _restartService;
    std::unique_ptr<FunctionPropertyServices> _functionPropertyServices;
    std::unique_ptr<NetworkParameterService> _networkParameterService;

    // Application data callback (for group value services)
    DataCallback _rxCallback;
    WorkAvailableCallback _receiveWorkAvailableCallback;
    IndividualAddressUpdateCallback _individualAddressUpdateCallback;

    static constexpr size_t RX_QUEUE_CAPACITY = 8u;
    std::array<ADataFrame, RX_QUEUE_CAPACITY> _rxQueue{};
    size_t _rxQueueHead{0};
    size_t _rxQueueCount{0};
    size_t _droppedReceiveFrames{0};

    static constexpr size_t TX_OUTCOME_QUEUE_CAPACITY = 8u;
    std::array<ASendOutcome, TX_OUTCOME_QUEUE_CAPACITY> _txOutcomeQueue{};
    size_t _txOutcomeQueueHead{0};
    size_t _txOutcomeQueueCount{0};
    size_t _droppedSendOutcomes{0};

    // Initialization state
    bool _initialized;
    bool _programmingModeEnabled{false};

    // True while dispatching a request that arrived on a transport connection
    // (T_Data_Connected). Responses built during that dispatch go back on the
    // connection; everything else — including answers to connectionless
    // T_Data_Individual requests received while a connection is open — is sent
    // connectionless. See TDataFrame::service.
    bool _handlingConnectedRequest{false};

    // How the request currently being dispatched was secured, and where the
    // current Security Mode comes from. Together these decide every Access
    // Policy question the service handlers ask.
    RequestSecurity _requestSecurity{};
    SecurityModeProvider _securityModeProvider{};
    AdcReadProvider _adcReadProvider{};

    SendOptions _defaultSendOptions{};
    SendOptions _serviceResponseSendOptions{};

    struct SendOperationState {
        bool active{false};
        transport::TDataFrame frame{};
        ASendOutcome outcome{};
        SendOptions options{};
        uint8_t attemptsStarted{0};
    };
    SendOperationState _sendOperation{};

    void enqueueReceivedFrame(const ADataFrame& frame);
    void enqueueSendOutcome(const ASendOutcome& outcome);
    void finishSendOperation();
    util::Result<void> startSendAttempt();
    bool shouldRetrySendError(util::ErrorCode error, const SendOptions& options) const;
    SendProgressState mapSendErrorToProgressState(util::ErrorCode error) const;
    util::Result<void> buildTransportTxFrame(const GroupAddress& destination,
                                             APCIService service,
                                             std::span<const uint8_t> data,
                                             AddressType destinationType,
                                             transport::TDataFrame& tFrame,
                                             ASendOutcome& outcome);

    void wireServiceCallbacks();
    void wireAccessPolicy();
    void wireTransportReceiveCallback();

    util::Result<void> sendToIndividual(IndividualAddress destination,
                                        APCIService service,
                                        std::span<const uint8_t> data,
                                        const SendOptions& options);
    util::Result<void> sendToGroup(GroupAddress destination,
                                   APCIService service,
                                   std::span<const uint8_t> data,
                                   const SendOptions& options);
    util::Result<void> sendBroadcast(APCIService service,
                                     std::span<const uint8_t> data,
                                     const SendOptions& options);

    void handleDeviceDescriptorRead(const ADataFrame& frame);
    void handlePropertyValueRead(const ADataFrame& frame);
    void handlePropertyValueWrite(const ADataFrame& frame);
    void handlePropertyDescriptionRead(const ADataFrame& frame);
    void handleFunctionProperty(const ADataFrame& frame);
    void handleIndividualAddressSerialNumberRead(const ADataFrame& frame);
    void handleIndividualAddressSerialNumberWrite(const ADataFrame& frame);
    void handleNetworkParameterRead(const ADataFrame& frame);
    void handleNetworkParameterWrite(const ADataFrame& frame);
    void handleMemoryRead(const ADataFrame& frame);
    void handleMemoryWrite(const ADataFrame& frame);
    void handleMemoryExtendedRead(const ADataFrame& frame);
    void handleMemoryExtendedWrite(const ADataFrame& frame);
    void handlePropertyExtValueRead(const ADataFrame& frame);
    void handlePropertyExtValueWrite(const ADataFrame& frame, bool confirmed);
    void handlePropertyExtDescriptionRead(const ADataFrame& frame);
    void handleFunctionPropertyExt(const ADataFrame& frame, FunctionPropertyInvocation invocation);
    void handleAuthorizeRequest(const ADataFrame& frame);
    void handleRestart(const ADataFrame& frame);
    void handleAdcRead(const ADataFrame& frame);
    void handleIndividualAddressRead(const ADataFrame& frame);
    void handleSystemNetworkParameterRead(const ADataFrame& frame);
    void handleIndividualAddressWrite(const ADataFrame& frame);
    void forwardToUser(const ADataFrame& frame);
};

} // namespace application
} // namespace knx
