// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_object.hpp
 * @brief KNX Device Object (Interface Object Type 0)
 * 
 * Contains device identification and status information.
 * Implements standard KNX properties per KNX spec 3/7/2.
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/types.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <span>

namespace knx {
namespace objects {

/**
 * @brief Load state per KNX spec (PID_LOAD_STATE_CONTROL)
 */
enum class LoadState : uint8_t {
    Unloaded = 0x00,      // No configuration loaded
    Loading = 0x01,       // Configuration being loaded
    Loaded = 0x02,        // Configuration loaded successfully
    Error = 0x03,         // Load error occurred
    Unloading = 0x04,     // Configuration being unloaded
    LoadCompleting = 0x05 // Load finishing
};

/**
 * @brief Load event for state machine
 */
enum class LoadEvent {
    StartLoad,
    LoadComplete,
    StartUnload,
    UnloadComplete,
    Error
};

/**
 * @brief Run state per KNX spec (PID_RUN_STATE_CONTROL)
 */
enum class RunState : uint8_t {
    Halted = 0x00,        // Application halted
    Running = 0x01,       // Application running
    Ready = 0x02,         // Ready to run
    Terminated = 0x03,    // Terminated with error
    StartingUp = 0x04,    // Starting up
    ShuttingDown = 0x05   // Shutting down
};

/**
 * @brief Error codes for device object
 */
enum class DeviceError : uint8_t {
    None = 0x00,
    LoadError = 0x01,
    RunError = 0x02,
    MemoryError = 0x03,
    ConfigError = 0x04
};

/**
 * @brief Standard KNX device properties (PID_*)
 */
enum class DeviceProperty : uint8_t {
    ObjectType = 1,           // PID_OBJECT_TYPE
    ObjectName = 2,           // PID_OBJECT_NAME
    LoadStateControl = 5,     // PID_LOAD_STATE_CONTROL
    RunStateControl = 6,      // PID_RUN_STATE_CONTROL
    TableReference = 7,       // PID_TABLE_REFERENCE
    ServiceControl = 8,       // PID_SERVICE_CONTROL
    FirmwareRevision = 9,     // PID_FIRMWARE_REVISION
    SerialNumber = 11,        // PID_SERIAL_NUMBER
    ManufacturerId = 12,      // PID_MANUFACTURER_ID
    ProgramVersion = 13,      // PID_PROGRAM_VERSION
    DeviceControl = 14,       // PID_DEVICE_CONTROL
    OrderInfo = 15,           // PID_ORDER_INFO
    PeiType = 16,             // PID_PEI_TYPE
    PortConfiguration = 17,   // PID_PORT_CONFIGURATION
    PollGroupSettings = 18,   // PID_POLL_GROUP_SETTINGS
    ManufacturerData = 19,    // PID_MANUFACTURER_DATA
    Description = 21,         // PID_DESCRIPTION
    Version = 25,             // PID_VERSION
    ProgMode = 54,            // PID_PROG_MODE
    ProductId = 55,           // PID_PRODUCT_ID
    MaxApduLength = 56,       // PID_MAX_APDU_LENGTH
    SubnetAddress = 57,       // PID_SUBNET_ADDRESS (high octet)
    DeviceAddress = 58,       // PID_DEVICE_ADDRESS (low octet)
    RoutingCount = 51,        // PID_ROUTING_COUNT
    MaxRetryCount = 52,       // PID_MAX_RETRY_COUNT
    ErrorFlags = 53,          // PID_ERROR_FLAGS
    HardwareType = 78         // PID_HARDWARE_TYPE
};

/**
 * @brief Callback for load state changes
 */
using LoadStateCallback = std::function<void(LoadState oldState, LoadState newState)>;

/**
 * @brief Callback for programming mode changes
 */
    using ProgModeCallback = std::function<void(Toggle mode)>;

/**
 * @brief Callback for individual address changes
 */
using IndividualAddressCallback = std::function<void(const IndividualAddress& address)>;

using SerialNumberCallback = std::function<void(std::span<const uint8_t> serialNumber)>;

/**
 * @brief Device Object - Interface Object Type 0
 * 
 * Standardized device information per KNX spec.
 * Implements property handling, load state machine, and programming mode.
 */
class DeviceObject : public InterfaceObject {
public:
    DeviceObject();
    ~DeviceObject() override = default;

    // === InterfaceObject interface ===
    InterfaceObjectType objectType() const override { return InterfaceObjectType::device(); }
    KernelBinding kernelBinding() const override;

    // Disable copy, enable move
    DeviceObject(const DeviceObject&) = delete;
    DeviceObject& operator=(const DeviceObject&) = delete;
    DeviceObject(DeviceObject&&) = default;
    DeviceObject& operator=(DeviceObject&&) = default;

    // === Standard KNX Properties ===

    // PID_MANUFACTURER_ID (11)
    void setManufacturerId(ManufacturerId id) { _manufacturerId = id; }
    ManufacturerId getManufacturerId() const { return _manufacturerId; }

    // PID_HARDWARE_TYPE (78)
    void setHardwareType(std::span<const uint8_t> type) { _hardwareType.assign(type.begin(), type.end()); }
    std::span<const uint8_t> getHardwareType() const { return _hardwareType; }

    // PID_FIRMWARE_REVISION (9)
    void setFirmwareRevision(uint8_t revision) { _firmwareRevision = revision; }
    uint8_t getFirmwareRevision() const { return _firmwareRevision; }

    // PID_SERIAL_NUMBER (11)
    //
    // Notifying: the serial-number-addressed commissioning services and the
    // programming-mode scan cache their own copy, and firmware typically sets
    // the real (e.g. MAC-derived) serial only *after* the stack has started.
    // Without the callback those services keep answering with the all-zero
    // placeholder captured at init.
    void setSerialNumber(std::span<const uint8_t> serial);
    std::span<const uint8_t> getSerialNumber() const { return _serialNumber; }
    void registerSerialNumberCallback(SerialNumberCallback callback);

    // PID_ORDER_INFO (15)
    void setOrderInfo(const std::string& order) { _orderInfo = order; }
    const std::string& getOrderInfo() const { return _orderInfo; }

    // PID_VERSION (25), encoded as DPT_Version (217.001): U5 magic,
    // U5 version, U6 revision packed big-endian into two octets
    // (03/07/02 §3.42). 03/05/01 §4.2.25: in the Device Object this is the
    // version of the device itself.
    static constexpr uint16_t packVersion(uint8_t magic, uint8_t version, uint8_t revision) {
        return static_cast<uint16_t>(((magic & 0x1Fu) << 11) |
                                     ((version & 0x1Fu) << 6) |
                                     (revision & 0x3Fu));
    }
    void setVersion(uint16_t packedVersion) { _version = packedVersion; }
    void setVersion(uint8_t magic, uint8_t version, uint8_t revision) {
        _version = packVersion(magic, version, revision);
    }
    uint16_t getVersion() const { return _version; }

    // PID_SUBNET_ADDRESS (57) + PID_DEVICE_ADDRESS (58)
    void setIndividualAddress(const IndividualAddress& addr) { _individualAddress = addr; }
    IndividualAddress getIndividualAddress() const { return _individualAddress; }

    // PID_MAX_APDU_LENGTH (58)
    void setMaxApduLength(uint16_t length) { _maxApduLength = length; }
    uint16_t getMaxApduLength() const { return _maxApduLength; }

    // PID_DESCRIPTION (21)
    void setDescription(const std::string& desc) { _description = desc; }
    const std::string& getDescription() const { return _description; }

    // PID_ROUTING_COUNT (66)
    void setRoutingCount(uint8_t count) { _routingCount = count; }
    uint8_t getRoutingCount() const { return _routingCount; }

    // PID_MAX_RETRY_COUNT (67)
    void setMaxRetryCount(uint8_t count) { _maxRetryCount = count; }
    uint8_t getMaxRetryCount() const { return _maxRetryCount; }

    // PID_ERROR_FLAGS (68)
    void setErrorFlags(uint8_t flags) { _errorFlags = flags; }
    uint8_t getErrorFlags() const { return _errorFlags; }

    // PID_PROG_VERSION (69)
    void setProgVersion(uint8_t version) { _progVersion = version; }
    uint8_t getProgVersion() const { return _progVersion; }

    // PID_DEVICE_CONTROL (12)
    void setDeviceControl(uint8_t control) { _deviceControl = control; }
    uint8_t getDeviceControl() const { return _deviceControl; }

    // === Load State Machine ===

    // PID_LOAD_STATE_CONTROL (5)
    LoadState getLoadState() const { return _loadState; }
    util::Result<void> processLoadEvent(LoadEvent event);
    void registerLoadStateCallback(LoadStateCallback callback);

    // === Run State Management ===

    // PID_RUN_STATE_CONTROL (6)
    void setRunState(RunState state) { _runState = state; }
    RunState getRunState() const { return _runState; }

    // === Programming Mode ===

    // PID_PROG_MODE (54)
    void setProgMode(Toggle mode);
    void setProgModeSilent(Toggle mode);
    bool getProgMode() const { return _progMode; }
    void registerProgModeCallback(ProgModeCallback callback);

    // Single-slot, overwrite-on-set callback for the endpoint runtime's own
    // programming-mode observer. Unlike registerProgModeCallback() (append,
    // for independent long-lived observers such as the BAU), this slot is
    // meant to be re-set by EndpointRuntime::rewireCallbacksAfterMove() after
    // a CommissionedProductRuntime move. Using the append API there left a
    // dangling this-capturing closure from the pre-move (stack) instance
    // registered alongside the fresh one, since append never removes the
    // stale entry — the dangling closure crashed the next time programming
    // mode was toggled once its captured stack frame was reused.
    void setInternalProgModeCallback(ProgModeCallback callback);
    void registerIndividualAddressCallback(IndividualAddressCallback callback);
    
    // Convenience methods for programming mode
    void enterProgrammingMode() { setProgMode(Toggle::Enable); }
    void exitProgrammingMode() { setProgMode(Toggle::Disable); }
    bool isProgrammingMode() const { return getProgMode(); }
    
    // Programming mode timeout
    void setProgModeTimeout(uint32_t timeoutMs) { _progModeTimeoutMs = timeoutMs; }
    uint32_t getProgModeTimeout() const { return _progModeTimeoutMs; }
    
    // Timer callback for platform integration (e.g., FreeRTOS, Linux timer)
    using TimerCallback = std::function<void(uint32_t durationMs, std::function<void()> callback)>;
    void setTimerCallback(TimerCallback callback) { _timerCallback = callback; }
    
    // LED callback for visual indication
    using LedCallback = std::function<void(Toggle ledState)>;
    void setLedCallback(LedCallback callback) { _ledCallback = callback; }
    
    // Individual address write via the KNX A_IndividualAddress_Write MANAGEMENT
    // service — requires programming mode (KNX 03/06/03 §3.5).
    util::Result<void> writeIndividualAddress(const IndividualAddress& addr);

    // Apply an individual address WITHOUT the programming-mode gate, for paths
    // whose authorization is enforced elsewhere: PID_SUBNET_ADDRESS /
    // PID_DEVICE_ADDRESS property writes (access-controlled by the property
    // service) and persistence restore at boot (prog mode is always off then).
    // Sets the value and fires the update callback so the running stack stays
    // in sync, but does not re-check programming mode.
    util::Result<void> applyIndividualAddress(const IndividualAddress& addr);
    IndividualAddress readIndividualAddress() const { return _individualAddress; }

    // === Error Handling ===

    void setLastError(DeviceError error) { _lastError = error; }
    DeviceError getLastError() const { return _lastError; }
    void clearError() { _lastError = DeviceError::None; _errorFlags = 0; }

    // === Validation ===

    bool isValid() const;
    bool isConfigured() const { return _loadState == LoadState::Loaded; }
    bool isRunning() const { return _runState == RunState::Running; }

private:
    // Device identification
    ManufacturerId _manufacturerId{};
    std::vector<uint8_t> _hardwareType;
    uint8_t _firmwareRevision{0};
    std::vector<uint8_t> _serialNumber;
    std::string _orderInfo;
    std::string _description;
    uint16_t _version{0};

    // Addressing
    IndividualAddress _individualAddress;
    uint16_t _maxApduLength{254}; // Max APDU (L_Data_Extended supported)

    // State management
    LoadState _loadState{LoadState::Unloaded};
    RunState _runState{RunState::Halted};
    bool _progMode{false};
    uint32_t _progModeTimeoutMs{60000};  // Default 60 seconds
    void* _activeTimer{nullptr};  // Platform-specific timer handle

    // Configuration
    uint8_t _routingCount{6};      // Default hop count
    uint8_t _maxRetryCount{3};     // Default retry count
    uint8_t _errorFlags{0};
    uint8_t _progVersion{0};
    uint8_t _deviceControl{0};

    // Error tracking
    DeviceError _lastError{DeviceError::None};

    // Callbacks
    LoadStateCallback _loadStateCallback;
    // Multiple stack layers observe programming mode (BAU and the endpoint
    // runtime both register); registration is append, not replace.
    util::FixedVector<ProgModeCallback, 4> _progModeCallbacks;
    // Single-slot counterpart for setInternalProgModeCallback(); see its
    // declaration for why the endpoint runtime can't share the append slot.
    ProgModeCallback _internalProgModeCallback;
    IndividualAddressCallback _individualAddressCallback;
    SerialNumberCallback _serialNumberCallback;
    TimerCallback _timerCallback;
    LedCallback _ledCallback;

    // Internal helpers
    util::Result<void> validateLoadStateTransition(LoadState from, LoadEvent event) const;

    void handleProgModeTimeout();
    void startProgModeTimer();
    void stopProgModeTimer();

    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
