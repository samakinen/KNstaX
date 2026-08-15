// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_object.cpp
 * @brief Device object implementation
 */

#include "knx/objects/device_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include <algorithm>
#include <cstring>

namespace knx {
namespace objects {

namespace {

/// PID_LOAD_STATE_CONTROL state names, for logs that a reader can follow
/// without the state table from the spec in front of them.
const char* loadStateName(LoadState state) {
    switch (state) {
        case LoadState::Unloaded:       return "Unloaded";
        case LoadState::Loading:        return "Loading";
        case LoadState::Loaded:         return "Loaded";
        case LoadState::Error:          return "Error";
        case LoadState::Unloading:      return "Unloading";
        case LoadState::LoadCompleting: return "LoadCompleting";
    }
    return "Unknown";
}

} // namespace

DeviceObject::DeviceObject()
    : _manufacturerId()
    , _firmwareRevision(0)
    , _serialNumber(6, 0u)
    , _individualAddress(0)
    // Full L_Data_Extended support: up to 254-octet APDUs.
    , _maxApduLength(254)
    , _loadState(LoadState::Unloaded)
    , _runState(RunState::Halted)
    , _progMode(false)
    , _routingCount(6)
    , _maxRetryCount(3)
    , _errorFlags(0)
    , _progVersion(0)
    , _deviceControl(0)
    , _lastError(DeviceError::None)
{
}

// === Load State Machine ===

util::Result<void> DeviceObject::processLoadEvent(LoadEvent event) {
    LoadState oldState = _loadState;
    LoadState newState = oldState;

    // Validate transition
    auto transitionRes = validateLoadStateTransition(oldState, event);
    if (transitionRes.isError()) {
        KNX_LOGW("KNX.DeviceObj", "Invalid load state transition from %d on event %d", 
                 static_cast<int>(oldState), static_cast<int>(event));
        _lastError = DeviceError::LoadError;
        return transitionRes.error();
    }

    // Execute state transition
    switch (event) {
        case LoadEvent::StartLoad:
            newState = LoadState::Loading;
            break;

        case LoadEvent::LoadComplete:
            if (oldState == LoadState::Loading) {
                newState = LoadState::LoadCompleting;
            } else if (oldState == LoadState::LoadCompleting) {
                newState = LoadState::Loaded;
                _runState = RunState::Ready;
            }
            break;

        case LoadEvent::StartUnload:
            newState = LoadState::Unloading;
            break;

        case LoadEvent::UnloadComplete:
            newState = LoadState::Unloaded;
            _runState = RunState::Halted;
            break;

        case LoadEvent::Error:
            newState = LoadState::Error;
            _lastError = DeviceError::LoadError;
            break;
    }

    if (newState != oldState) {
        _loadState = newState;
        KNX_LOGD("KNX.DeviceObj", "Load state: %s → %s",
                 loadStateName(oldState), loadStateName(newState));

        // Notify callback
        if (_loadStateCallback) {
            _loadStateCallback(oldState, newState);
        }
    }

    return util::Result<void>::ok();
}

void DeviceObject::registerLoadStateCallback(LoadStateCallback callback) {
    _loadStateCallback = std::move(callback);
}

util::Result<void> DeviceObject::validateLoadStateTransition(LoadState from, LoadEvent event) const {
    // Define valid state transitions
    switch (from) {
        case LoadState::Unloaded:
            return (event == LoadEvent::StartLoad)
                ? util::Result<void>::ok()
                : util::Result<void>::err(util::ErrorCode::OperationNotSupported);

        case LoadState::Loading:
            return (event == LoadEvent::LoadComplete || event == LoadEvent::Error)
                ? util::Result<void>::ok()
                : util::Result<void>::err(util::ErrorCode::OperationNotSupported);

        case LoadState::LoadCompleting:
            return (event == LoadEvent::LoadComplete || event == LoadEvent::Error)
                ? util::Result<void>::ok()
                : util::Result<void>::err(util::ErrorCode::OperationNotSupported);

        case LoadState::Loaded:
            return (event == LoadEvent::StartUnload)
                ? util::Result<void>::ok()
                : util::Result<void>::err(util::ErrorCode::OperationNotSupported);

        case LoadState::Unloading:
            return (event == LoadEvent::UnloadComplete || event == LoadEvent::Error)
                ? util::Result<void>::ok()
                : util::Result<void>::err(util::ErrorCode::OperationNotSupported);

        case LoadState::Error:
            return (event == LoadEvent::StartUnload)
                ? util::Result<void>::ok()
                : util::Result<void>::err(util::ErrorCode::OperationNotSupported); // Can recover by unloading

        default:
            return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
}


// === Programming Mode ===

void DeviceObject::setProgMode(Toggle mode) {
    const bool enable = isEnabled(mode);
    if (_progMode == enable) {
        return;
    }

    _progMode = enable;
    KNX_LOGI("KNX.DeviceObj", "Programming mode %s", enable ? "enabled" : "disabled");

    // Control LED indication
    if (_ledCallback) {
        _ledCallback(enable ? Toggle::Enable : Toggle::Disable);
    }

    // Manage timeout timer
    if (enable) {
        startProgModeTimer();
    } else {
        stopProgModeTimer();
    }

    // Notify all registered observers
    for (const auto& callback : _progModeCallbacks) {
        if (callback) {
            callback(enable ? Toggle::Enable : Toggle::Disable);
        }
    }
    if (_internalProgModeCallback) {
        _internalProgModeCallback(enable ? Toggle::Enable : Toggle::Disable);
    }
}

void DeviceObject::setProgModeSilent(Toggle mode) {
    const bool enable = isEnabled(mode);
    if (_progMode == enable) {
        return;
    }
    _progMode = enable;
    // Do not log or notify callbacks in silent mode.
    if (enable) {
        startProgModeTimer();
    } else {
        stopProgModeTimer();
    }
}

void DeviceObject::registerProgModeCallback(ProgModeCallback callback) {
    if (!_progModeCallbacks.push_back(std::move(callback))) {
        KNX_LOGW("KNX.DeviceObj", "ProgMode callback registry full, observer dropped");
    }
}

void DeviceObject::setInternalProgModeCallback(ProgModeCallback callback) {
    _internalProgModeCallback = std::move(callback);
}

void DeviceObject::registerIndividualAddressCallback(IndividualAddressCallback callback) {
    _individualAddressCallback = std::move(callback);
}

void DeviceObject::setSerialNumber(std::span<const uint8_t> serial) {
    _serialNumber.assign(serial.begin(), serial.end());
    if (_serialNumberCallback) {
        _serialNumberCallback(_serialNumber);
    }
}

void DeviceObject::registerSerialNumberCallback(SerialNumberCallback callback) {
    _serialNumberCallback = std::move(callback);
    // Adopt the current value immediately: registration usually happens after
    // the serial number was already set from persistence or product identity.
    if (_serialNumberCallback && !_serialNumber.empty()) {
        _serialNumberCallback(_serialNumber);
    }
}

util::Result<void> DeviceObject::writeIndividualAddress(const IndividualAddress& addr) {
    // Individual address can only be written in programming mode
    if (!_progMode) {
        KNX_LOGW("KNX.DeviceObj", "Individual address write rejected: not in programming mode");
        return util::ErrorCode::OperationNotReady;
    }

    return applyIndividualAddress(addr);
}

util::Result<void> DeviceObject::applyIndividualAddress(const IndividualAddress& addr) {
    if (_individualAddress == addr) {
        return util::Result<void>::ok();
    }

    _individualAddress = addr;
    // Debug, not info: this fires for every intermediate address the object
    // passes through while properties are restored and the load segment is
    // applied. The address that matters is reported once by the BAU when the
    // stack comes up, and by the application layer on an ETS re-address.
    KNX_LOGD("KNX.DeviceObj", "Individual address set to %d.%d.%d",
             addr.area(), addr.line(), addr.device());

    if (_individualAddressCallback) {
        _individualAddressCallback(addr);
    }

    return util::Result<void>::ok();
}

void DeviceObject::startProgModeTimer() {
    // Stop any existing timer first
    stopProgModeTimer();
    
    if (_timerCallback && _progModeTimeoutMs > 0) {
        // Create a lambda that captures 'this' to call timeout handler
        auto timeoutHandler = [this]() {
            handleProgModeTimeout();
        };
        
        // Request platform to start timer
        _timerCallback(_progModeTimeoutMs, timeoutHandler);
        
        KNX_LOGD("KNX.DeviceObj", "Programming mode timeout started: %lu ms",
             static_cast<unsigned long>(_progModeTimeoutMs));
    }
}

void DeviceObject::stopProgModeTimer() {
    // TimerCallback is fire-and-forget: it hands the platform a duration and a
    // handler, and returns no handle, so an armed timeout cannot be cancelled
    // here. A pending timeout therefore still fires; handleProgModeTimeout()
    // re-checks _progMode and does nothing if programming mode already ended.
    //
    // Consequence to be aware of: if programming mode is left and re-entered
    // within one timeout period, the earlier timer fires against the new
    // session and ends it early. Cancelling properly requires a TimerCallback
    // that returns a handle.
    _activeTimer = nullptr;
}

void DeviceObject::handleProgModeTimeout() {
    if (_progMode) {
        KNX_LOGI("KNX.DeviceObj", "Programming mode timeout - auto-exiting");
        setProgMode(Toggle::Disable);
    }
}

namespace {
util::Result<uint16_t> getObjectType(const DeviceObject& /*domain*/) {
    return InterfaceObjectType::device().value();
}

util::Result<uint16_t> getManufacturerId(const DeviceObject& domain) {
    return domain.getManufacturerId().value();
}

util::Result<void> setManufacturerId(DeviceObject& domain, const uint16_t& value) {
    domain.setManufacturerId(ManufacturerId(value));
    return util::Result<void>::ok();
}

util::Result<uint8_t> getFirmwareRevision(const DeviceObject& domain) {
    return domain.getFirmwareRevision();
}

util::Result<uint8_t> getProgMode(const DeviceObject& domain) {
    return domain.getProgMode() ? 0x01 : 0x00;
}

util::Result<void> setProgMode(DeviceObject& domain, const uint8_t& value) {
    // PID_PROGMODE is "Programming Mode - Realisation Type 1" (property based),
    // which Profiles v02.01.01 §4.4.1.1 a) assigns to System B — this device's
    // mask 07B0.  Per Resources v01.10.01 §4.3.5 bit 0 carries the state and
    // bits 1..7 are reserved and shall always be 0, so only bit 0 is read.
    //
    // Notifying, NOT silent.  The silent variant updates only the stored bit,
    // leaving ApplicationLayer's own _programmingModeEnabled (set through the
    // BAU prog-mode callback) still true: the device would keep answering
    // A_IndividualAddress_Read and keep its LED lit after ETS switched
    // programming mode off, which is exactly ETS's "final check of this
    // procedure failed / you may need to switch off programming mode (LED)
    // manually".
    domain.setProgMode((value & 0x01u) != 0 ? Toggle::Enable : Toggle::Disable);
    return util::Result<void>::ok();
}

util::Result<uint16_t> getMaxApduLength(const DeviceObject& domain) {
    return domain.getMaxApduLength();
}

util::Result<uint8_t> getSubnetAddress(const DeviceObject& domain) {
    return static_cast<uint8_t>((domain.readIndividualAddress().raw >> 8) & 0xFFu);
}

util::Result<void> setSubnetAddress(DeviceObject& domain, const uint8_t& value) {
    // Property-service path (PID_SUBNET_ADDRESS) and persistence restore: not
    // gated on programming mode — see DeviceObject::applyIndividualAddress.
    const uint16_t current = domain.readIndividualAddress().raw;
    const uint16_t updated = static_cast<uint16_t>((value << 8) | (current & 0x00FFu));
    return domain.applyIndividualAddress(IndividualAddress(updated));
}

util::Result<uint8_t> getDeviceAddress(const DeviceObject& domain) {
    return static_cast<uint8_t>(domain.readIndividualAddress().raw & 0xFFu);
}

util::Result<void> setDeviceAddress(DeviceObject& domain, const uint8_t& value) {
    const uint16_t current = domain.readIndividualAddress().raw;
    const uint16_t updated = static_cast<uint16_t>((current & 0xFF00u) | value);
    return domain.applyIndividualAddress(IndividualAddress(updated));
}

util::Result<uint8_t> getRoutingCount(const DeviceObject& domain) {
    return domain.getRoutingCount();
}

util::Result<void> setRoutingCount(DeviceObject& domain, const uint8_t& value) {
    domain.setRoutingCount(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getMaxRetryCount(const DeviceObject& domain) {
    return domain.getMaxRetryCount();
}

util::Result<void> setMaxRetryCount(DeviceObject& domain, const uint8_t& value) {
    domain.setMaxRetryCount(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getErrorFlags(const DeviceObject& domain) {
    return domain.getErrorFlags();
}

util::Result<void> setErrorFlags(DeviceObject& domain, const uint8_t& value) {
    domain.setErrorFlags(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getProgVersion(const DeviceObject& domain) {
    return domain.getProgVersion();
}

util::Result<void> setProgVersion(DeviceObject& domain, const uint8_t& value) {
    domain.setProgVersion(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getDeviceControl(const DeviceObject& domain) {
    return domain.getDeviceControl();
}

util::Result<void> setDeviceControl(DeviceObject& domain, const uint8_t& value) {
    domain.setDeviceControl(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getLoadState(const DeviceObject& domain) {
    return static_cast<uint8_t>(domain.getLoadState());
}

util::Result<uint8_t> getRunState(const DeviceObject& domain) {
    return static_cast<uint8_t>(domain.getRunState());
}

util::Result<void> setRunState(DeviceObject& domain, const uint8_t& value) {
    domain.setRunState(static_cast<RunState>(value));
    return util::Result<void>::ok();
}

static const ScalarPropertyData<DeviceObject, uint16_t> kObjectTypeData{ &getObjectType, nullptr };
static const ScalarPropertyData<DeviceObject, uint16_t> kManufacturerData{ &getManufacturerId, &setManufacturerId };
static const ScalarPropertyData<DeviceObject, uint8_t> kFirmwareData{ &getFirmwareRevision, nullptr };
static const ScalarPropertyData<DeviceObject, uint8_t> kProgModeData{ &getProgMode, &setProgMode };
static const ScalarPropertyData<DeviceObject, uint16_t> kMaxApduData{ &getMaxApduLength, nullptr };
static const ScalarPropertyData<DeviceObject, uint8_t> kSubnetData{ &getSubnetAddress, &setSubnetAddress };
static const ScalarPropertyData<DeviceObject, uint8_t> kDeviceAddrData{ &getDeviceAddress, &setDeviceAddress };
static const ScalarPropertyData<DeviceObject, uint8_t> kRoutingData{ &getRoutingCount, &setRoutingCount };
static const ScalarPropertyData<DeviceObject, uint8_t> kRetryData{ &getMaxRetryCount, &setMaxRetryCount };
static const ScalarPropertyData<DeviceObject, uint8_t> kErrorFlagsData{ &getErrorFlags, &setErrorFlags };
static const ScalarPropertyData<DeviceObject, uint8_t> kProgVersionData{ &getProgVersion, &setProgVersion };
static const ScalarPropertyData<DeviceObject, uint8_t> kDeviceControlData{ &getDeviceControl, &setDeviceControl };
static const ScalarPropertyData<DeviceObject, uint8_t> kLoadStateData{ &getLoadState, nullptr };
static const ScalarPropertyData<DeviceObject, uint8_t> kRunStateData{ &getRunState, &setRunState };

static util::Result<void> readSerialNumber(
    const PropertyContext& context,
    DomainIndex /*startIndex*/,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    if (elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto* domain = static_cast<const DeviceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return out.writeBytes(domain->getSerialNumber());
}

static const PropertyHandler kSerialNumberHandler{
    static_cast<application::PropertyID>(DeviceProperty::SerialNumber),
    application::PropertyDataType::Generic06,
    PropertyCapability::ReadOnly,
    1,
    0,
    &readSerialNumber,
    nullptr,
    nullptr,
};

// PID_HARDWARE_TYPE (03/05/01 §4.3.28): six octets, MSB fixed at 00h
// ("Other values for the MSB shall not be used"), the remaining five being a
// manufacturer-specific hardware identifier. ETS uses it to check that an
// application program may be downloaded into this hardware, so a device that
// answers with a short or absent value is answering a question ETS asked.
// Missing content is padded rather than refused: the property is fixed-width.
static util::Result<void> readHardwareType(
    const PropertyContext& context,
    DomainIndex /*startIndex*/,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    if (elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto* domain = static_cast<const DeviceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto value = domain->getHardwareType();
    constexpr size_t kHardwareTypeSize = 6;
    const size_t copied = std::min<size_t>(value.size(), kHardwareTypeSize);
    if (copied > 0) {
        auto res = out.writeBytes(value.subspan(0, copied));
        if (res.isError()) {
            return res;
        }
    }
    for (size_t i = copied; i < kHardwareTypeSize; ++i) {
        auto pad = out.u8(0);
        if (pad.isError()) {
            return pad;
        }
    }
    return util::Result<void>::ok();
}

static const PropertyHandler kHardwareTypeHandler{
    static_cast<application::PropertyID>(DeviceProperty::HardwareType),
    application::PropertyDataType::Generic06,
    PropertyCapability::ReadOnly,
    1,
    0,
    &readHardwareType,
    nullptr,
    nullptr,
};

// PID_ORDER_INFO (03/05/01 §4.2.15): PDT_GENERIC_10, manufacturer-specific
// order information. Fixed ten octets, so a shorter order number is
// zero-padded rather than truncating the response.
static util::Result<void> readOrderInfo(
    const PropertyContext& context,
    DomainIndex /*startIndex*/,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    if (elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto* domain = static_cast<const DeviceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto& order = domain->getOrderInfo();
    constexpr size_t kOrderInfoSize = 10;
    const size_t copied = std::min<size_t>(order.size(), kOrderInfoSize);
    for (size_t i = 0; i < copied; ++i) {
        auto res = out.u8(static_cast<uint8_t>(order[i]));
        if (res.isError()) {
            return res;
        }
    }
    for (size_t i = copied; i < kOrderInfoSize; ++i) {
        auto pad = out.u8(0);
        if (pad.isError()) {
            return pad;
        }
    }
    return util::Result<void>::ok();
}

static const PropertyHandler kOrderInfoHandler{
    static_cast<application::PropertyID>(DeviceProperty::OrderInfo),
    application::PropertyDataType::Generic10,
    PropertyCapability::ReadOnly,
    1,
    0,
    &readOrderInfo,
    nullptr,
    nullptr,
};

util::Result<uint16_t> getVersion(const DeviceObject& domain) {
    return domain.getVersion();
}

static const ScalarPropertyData<DeviceObject, uint16_t> kVersionData{ &getVersion, nullptr };

static const PropertyHandler kDeviceHandlers[] = {
    kSerialNumberHandler,
    kHardwareTypeHandler,
    kOrderInfoHandler,

    ScalarProperty<DeviceObject, uint16_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::Version),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kVersionData),

    ScalarProperty<DeviceObject, uint16_t>::make(
        application::PropertyID::ObjectType,
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kObjectTypeData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::FirmwareRevision),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadOnly,
        &kFirmwareData),

    ScalarProperty<DeviceObject, uint16_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::ManufacturerId),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kManufacturerData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::ProgMode),
        application::PropertyDataType::Bitset8,
        PropertyCapability::ReadWrite,
        &kProgModeData),

    ScalarProperty<DeviceObject, uint16_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::MaxApduLength),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kMaxApduData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::SubnetAddress),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kSubnetData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::DeviceAddress),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kDeviceAddrData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::RoutingCount),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kRoutingData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::MaxRetryCount),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kRetryData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::ErrorFlags),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kErrorFlagsData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::ProgramVersion),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kProgVersionData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::DeviceControl),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kDeviceControlData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::LoadStateControl),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadOnly,
        &kLoadStateData),

    ScalarProperty<DeviceObject, uint8_t>::make(
        static_cast<application::PropertyID>(DeviceProperty::RunStateControl),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kRunStateData)
};

constexpr size_t kDeviceHandlerCount = sizeof(kDeviceHandlers) / sizeof(kDeviceHandlers[0]);
} // namespace

KernelBinding DeviceObject::kernelBinding() const {
    static bool validated = false;
    if (!validated) {
        const auto validation = validatePropertyTable(kDeviceHandlers, kDeviceHandlerCount);
        if (validation.isError()) {
            KNX_LOGE("KNX.DeviceObj", "Invalid property handler table (err=%d)", static_cast<int>(validation.error()));
        }
        validated = true;
    }
    KernelBinding binding;
    binding.handlers = kDeviceHandlers;
    binding.handlerCount = kDeviceHandlerCount;
    binding.context = PropertyContext{const_cast<DeviceObject*>(this), _validationPolicy};
    return binding;
}

// === Validation ===

bool DeviceObject::isValid() const {
    // Device must have:
    // 1. Valid manufacturer ID
    // 2. Individual address set
    // 3. No error state
    return _manufacturerId.value() != 0 && 
           _individualAddress.raw != 0 &&
           _lastError == DeviceError::None;
}

} // namespace objects
} // namespace knx
