// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file application_program_object.cpp
 * @brief Application Program Object implementation
 */

#include "knx/objects/application_program_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include <algorithm>

namespace knx {
namespace objects {

ApplicationProgramObject::ApplicationProgramObject()
    : _programVersion{}
    , _applicationId(0)
    , _applicationVersion(0)
    , _applicationNumber(0)
    , _applicationArea(0)
    , _applicationManufacturer(0)
    , _programState(ProgramState::Inactive)
    , _parameterStart(MemoryAddress(0))
    , _parameterEnd(MemoryAddress(0))
{
}

// === Program State & Control ===

util::Result<void> ApplicationProgramObject::executeProgramControl(ProgramControl control) {
    
    switch (control) {
        case ProgramControl::Start:
            if (_programState == ProgramState::Inactive) {
                _programState = ProgramState::Active;
                KNX_LOGI("KNX.AppProg", "Program started");
                return util::Result<void>::ok();
            }
            KNX_LOGW("KNX.AppProg", "Cannot start program in state %d", static_cast<int>(_programState));
            return util::ErrorCode::OperationFailed;

        case ProgramControl::Stop:
            if (_programState == ProgramState::Active) {
                _programState = ProgramState::Inactive;
                KNX_LOGI("KNX.AppProg", "Program stopped");
                return util::Result<void>::ok();
            }
            KNX_LOGW("KNX.AppProg", "Cannot stop program in state %d", static_cast<int>(_programState));
            return util::ErrorCode::OperationFailed;

        case ProgramControl::Reset:
            _programState = ProgramState::Inactive;
            KNX_LOGI("KNX.AppProg", "Program reset");
            return util::Result<void>::ok();

        case ProgramControl::Reload:
            if (_programState != ProgramState::Loading) {
                ProgramState prevState = _programState;
                _programState = ProgramState::Loading;
                KNX_LOGI("KNX.AppProg", "Program reloading");
                // In real implementation, this would trigger actual reload
                _programState = prevState; // Restore after reload
                return util::Result<void>::ok();
            }
            return util::ErrorCode::OperationFailed;

        case ProgramControl::NoControl:
        default:
            return util::Result<void>::ok(); // No-op
    }
}

namespace {
constexpr uint16_t kMaxProgramDataBytes = 0xFFFFu;
constexpr uint16_t kMaxProgramNameBytes = 0xFFFFu;
constexpr uint16_t kMaxProgramDescriptionBytes = 0xFFFFu;

enum class StringPropertyKind : uint8_t {
    ProgramName,
    ProgramDescription
};

util::Result<uint16_t> getObjectType(const ApplicationProgramObject& /*domain*/) {
    return InterfaceObjectType::applicationProgram().value();
}

// PID_PROGRAM_VERSION is PDT_GENERIC_05 (manufacturer id, application number,
// version); ETS writes the whole 5-octet block during the download.
util::Result<void> readProgramVersion(
    const PropertyContext& context,
    DomainIndex /*startIndex*/,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    const auto* domain = static_cast<const ApplicationProgramObject*>(context.domain);
    if (!domain || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    for (uint8_t byte : domain->getProgramVersionBlock()) {
        auto res = out.u8(byte);
        if (res.isError()) {
            return res;
        }
    }
    return util::Result<void>::ok();
}

util::Result<void> writeProgramVersion(
    const PropertyContext& context,
    DomainIndex /*startIndex*/,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* /*userData*/)
{
    auto* domain = static_cast<ApplicationProgramObject*>(context.domain);
    if (!domain || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    std::array<uint8_t, ApplicationProgramObject::kProgramVersionSize> block{};
    for (auto& byte : block) {
        auto res = in.u8();
        if (res.isError()) {
            return util::Result<void>::err(res.error());
        }
        byte = res.value();
    }
    domain->setProgramVersionBlock(block);
    return util::Result<void>::ok();
}

util::Result<uint16_t> getApplicationId(const ApplicationProgramObject& domain) {
    return domain.getApplicationId();
}

util::Result<void> setApplicationId(ApplicationProgramObject& domain, const uint16_t& value) {
    domain.setApplicationId(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getApplicationVersion(const ApplicationProgramObject& domain) {
    return domain.getApplicationVersion();
}

util::Result<void> setApplicationVersion(ApplicationProgramObject& domain, const uint8_t& value) {
    domain.setApplicationVersion(value);
    return util::Result<void>::ok();
}

util::Result<uint16_t> getApplicationNumber(const ApplicationProgramObject& domain) {
    return domain.getApplicationNumber();
}

util::Result<void> setApplicationNumber(ApplicationProgramObject& domain, const uint16_t& value) {
    domain.setApplicationNumber(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getApplicationArea(const ApplicationProgramObject& domain) {
    return domain.getApplicationArea();
}

util::Result<void> setApplicationArea(ApplicationProgramObject& domain, const uint8_t& value) {
    domain.setApplicationArea(value);
    return util::Result<void>::ok();
}

util::Result<uint16_t> getApplicationManufacturer(const ApplicationProgramObject& domain) {
    return domain.getApplicationManufacturer();
}

util::Result<void> setApplicationManufacturer(ApplicationProgramObject& domain, const uint16_t& value) {
    domain.setApplicationManufacturer(value);
    return util::Result<void>::ok();
}

util::Result<uint8_t> getProgramState(const ApplicationProgramObject& domain) {
    return static_cast<uint8_t>(domain.getProgramState());
}

util::Result<void> setProgramState(ApplicationProgramObject& domain, const uint8_t& value) {
    domain.setProgramState(static_cast<ProgramState>(value));
    return util::Result<void>::ok();
}

util::Result<void> setProgramControl(ApplicationProgramObject& domain, const uint8_t& value) {
    return domain.executeProgramControl(static_cast<ProgramControl>(value));
}

util::Result<uint16_t> getParameterStart(const ApplicationProgramObject& domain) {
    return domain.getParameterStart().value();
}

util::Result<void> setParameterStart(ApplicationProgramObject& domain, const uint16_t& value) {
    domain.setParameterStart(MemoryAddress(value));
    return util::Result<void>::ok();
}

util::Result<uint16_t> getParameterEnd(const ApplicationProgramObject& domain) {
    return domain.getParameterEnd().value();
}

util::Result<void> setParameterEnd(ApplicationProgramObject& domain, const uint16_t& value) {
    domain.setParameterEnd(MemoryAddress(value));
    return util::Result<void>::ok();
}

util::Result<void> readProgramData(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    const auto* domain = static_cast<const ApplicationProgramObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto data = domain->getProgramData();
    const size_t offset = static_cast<size_t>(startIndex.value);
    const size_t count = static_cast<size_t>(elementCount);
    if (offset + count > data.size()) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    return out.writeBytes(data.subspan(offset, count));
}

util::Result<void> writeProgramData(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* /*userData*/)
{
    auto* domain = static_cast<ApplicationProgramObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const size_t offset = static_cast<size_t>(startIndex.value);
    const size_t count = static_cast<size_t>(elementCount);
    if (offset + count > kMaxProgramDataBytes) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    std::vector<uint8_t> chunk(count, 0);
    auto res = in.readBytes(chunk);
    if (res.isError()) {
        return res;
    }

    std::vector<uint8_t> updated(domain->getProgramData().begin(), domain->getProgramData().end());
    if (offset > updated.size()) {
        updated.resize(offset, 0);
    }
    if (offset + count > updated.size()) {
        updated.resize(offset + count, 0);
    }
    std::copy(chunk.begin(), chunk.end(), updated.begin() + offset);
    domain->setProgramData(updated);
    domain->notifyProgramDataChanged();
    return util::Result<void>::ok();
}

const std::string& getStringByKind(const ApplicationProgramObject& domain, StringPropertyKind kind) {
    switch (kind) {
        case StringPropertyKind::ProgramName:
            return domain.getProgramName();
        case StringPropertyKind::ProgramDescription:
        default:
            return domain.getProgramDescription();
    }
}

void setStringByKind(ApplicationProgramObject& domain, StringPropertyKind kind, const std::string& value) {
    switch (kind) {
        case StringPropertyKind::ProgramName:
            domain.setProgramName(value);
            break;
        case StringPropertyKind::ProgramDescription:
        default:
            domain.setProgramDescription(value);
            break;
    }
}

util::Result<void> readStringProperty(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* userData)
{
    const auto* domain = static_cast<const ApplicationProgramObject*>(context.domain);
    const auto* kind = static_cast<const StringPropertyKind*>(userData);
    if (!domain || !kind) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto& str = getStringByKind(*domain, *kind);
    const size_t offset = static_cast<size_t>(startIndex.value);
    const size_t count = static_cast<size_t>(elementCount);
    if (offset + count > str.size()) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    return out.writeBytes(std::as_bytes(std::span<const char>(str).subspan(offset, count)));
}

util::Result<void> writeStringProperty(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* userData)
{
    auto* domain = static_cast<ApplicationProgramObject*>(context.domain);
    const auto* kind = static_cast<const StringPropertyKind*>(userData);
    if (!domain || !kind) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const size_t offset = static_cast<size_t>(startIndex.value);
    const size_t count = static_cast<size_t>(elementCount);
    if (offset + count > kMaxProgramNameBytes) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    std::string chunk(count, '\0');
    auto res = in.readBytes(std::as_writable_bytes(std::span<char>(chunk.data(), chunk.size())));
    if (res.isError()) {
        return res;
    }

    std::string updated = getStringByKind(*domain, *kind);
    if (offset > updated.size()) {
        updated.resize(offset, '\0');
    }
    if (offset + count > updated.size()) {
        updated.resize(offset + count, '\0');
    }
    std::copy(chunk.begin(), chunk.end(), updated.begin() + offset);
    setStringByKind(*domain, *kind, updated);
    return util::Result<void>::ok();
}

static const ScalarPropertyData<ApplicationProgramObject, uint16_t> kObjectTypeData{ &getObjectType, nullptr };
static const ScalarPropertyData<ApplicationProgramObject, uint16_t> kApplicationIdData{ &getApplicationId, &setApplicationId };
static const ScalarPropertyData<ApplicationProgramObject, uint8_t> kApplicationVersionData{ &getApplicationVersion, &setApplicationVersion };
static const ScalarPropertyData<ApplicationProgramObject, uint16_t> kApplicationNumberData{ &getApplicationNumber, &setApplicationNumber };
static const ScalarPropertyData<ApplicationProgramObject, uint8_t> kApplicationAreaData{ &getApplicationArea, &setApplicationArea };
static const ScalarPropertyData<ApplicationProgramObject, uint16_t> kApplicationManufacturerData{ &getApplicationManufacturer, &setApplicationManufacturer };
static const ScalarPropertyData<ApplicationProgramObject, uint8_t> kProgramStateData{ &getProgramState, &setProgramState };
static const ScalarPropertyData<ApplicationProgramObject, uint8_t> kProgramControlData{ nullptr, &setProgramControl };
static const ScalarPropertyData<ApplicationProgramObject, uint16_t> kParameterStartData{ &getParameterStart, &setParameterStart };
static const ScalarPropertyData<ApplicationProgramObject, uint16_t> kParameterEndData{ &getParameterEnd, &setParameterEnd };

util::Result<uint32_t> getTableReference(const ApplicationProgramObject& /*object*/) {
    return tableseg::kApplicationCodeBase;
}

static const ScalarPropertyData<ApplicationProgramObject, uint32_t> kTableReferenceData{
    &getTableReference,
    nullptr
};

static const StringPropertyKind kProgramNameKind = StringPropertyKind::ProgramName;
static const StringPropertyKind kProgramDescriptionKind = StringPropertyKind::ProgramDescription;

static const PropertyHandler kAppProgramHandlers[] = {
    ScalarProperty<ApplicationProgramObject, uint16_t>::make(
        application::PropertyID::ObjectType,
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kObjectTypeData),

    LoadControlProperty<ApplicationProgramObject>::make(
        static_cast<application::PropertyID>(AppProgramProperty::LoadState)),

    // PID_TABLE_REFERENCE: address of the memory-mapped code segment (the
    // knxprod RS-0000 relative segment); ETS writes it via A_Memory_Write.
    ScalarProperty<ApplicationProgramObject, uint32_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::TableReference),
        application::PropertyDataType::UnsignedLong,
        PropertyCapability::ReadOnly,
        &kTableReferenceData),

    {
        static_cast<application::PropertyID>(AppProgramProperty::ProgramVersion),
        application::PropertyDataType::Generic05,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readProgramVersion,
        &writeProgramVersion,
        nullptr
    },

    ScalarProperty<ApplicationProgramObject, uint16_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ApplicationId),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadWrite,
        &kApplicationIdData),

    ScalarProperty<ApplicationProgramObject, uint8_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ApplicationVersion),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kApplicationVersionData),

    ScalarProperty<ApplicationProgramObject, uint16_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ApplicationNumber),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadWrite,
        &kApplicationNumberData),

    ScalarProperty<ApplicationProgramObject, uint8_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ApplicationArea),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kApplicationAreaData),

    ScalarProperty<ApplicationProgramObject, uint16_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ApplicationManufacturer),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadWrite,
        &kApplicationManufacturerData),

    ScalarProperty<ApplicationProgramObject, uint8_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ProgramState),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kProgramStateData),

    ScalarProperty<ApplicationProgramObject, uint8_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ProgramControl),
        application::PropertyDataType::Control,
        PropertyCapability::WriteOnly,
        &kProgramControlData),

    ScalarProperty<ApplicationProgramObject, uint16_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ParameterStart),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadWrite,
        &kParameterStartData),

    ScalarProperty<ApplicationProgramObject, uint16_t>::make(
        static_cast<application::PropertyID>(AppProgramProperty::ParameterEnd),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadWrite,
        &kParameterEndData),

    {
        static_cast<application::PropertyID>(AppProgramProperty::ProgramData),
        application::PropertyDataType::GenericData,
        PropertyCapability::ReadWrite,
        kMaxProgramDataBytes,
        1,
        &readProgramData,
        &writeProgramData,
        nullptr
    },

    {
        static_cast<application::PropertyID>(AppProgramProperty::ProgramName),
        application::PropertyDataType::GenericData,
        PropertyCapability::ReadWrite,
        kMaxProgramNameBytes,
        1,
        &readStringProperty,
        &writeStringProperty,
        &kProgramNameKind
    },

    {
        static_cast<application::PropertyID>(AppProgramProperty::ProgramDescription),
        application::PropertyDataType::GenericData,
        PropertyCapability::ReadWrite,
        kMaxProgramDescriptionBytes,
        1,
        &readStringProperty,
        &writeStringProperty,
        &kProgramDescriptionKind
    }
};

constexpr size_t kAppProgramHandlerCount = sizeof(kAppProgramHandlers) / sizeof(kAppProgramHandlers[0]);
} // namespace

KernelBinding ApplicationProgramObject::kernelBinding() const {
    static bool validated = false;
    if (!validated) {
        const auto validation = validatePropertyTable(kAppProgramHandlers, kAppProgramHandlerCount);
        if (validation.isError()) {
            KNX_LOGE("KNX.AppProg", "Invalid property handler table (err=%d)", static_cast<int>(validation.error()));
        }
        validated = true;
    }
    KernelBinding binding;
    binding.handlers = kAppProgramHandlers;
    binding.handlerCount = kAppProgramHandlerCount;
    binding.context = PropertyContext{const_cast<ApplicationProgramObject*>(this), _validationPolicy};
    return binding;
}

// === Validation ===

bool ApplicationProgramObject::isValid() const {
    // Application program must have:
    // 1. Valid application ID
    // 2. Valid manufacturer ID
    // 3. Program version set
    return _applicationId != 0 &&
           _applicationManufacturer != 0 &&
           _programVersion[kProgramVersionSize - 1u] != 0;
}

} // namespace objects
} // namespace knx
