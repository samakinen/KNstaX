// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file application_program_object.cpp
 * @brief Application Program Object implementation
 */

#include "knx/objects/application_program_object.hpp"
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

    // The image has a fixed size: a write lands in it or is refused. A
    // product without parameters has no image, so every write is refused.
    const auto image = domain->parameterImage();
    if (offset + count > image.size()) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    auto res = in.readBytes(image.subspan(offset, count));
    if (res.isError()) {
        return res;
    }
    if (domain->acceptParameterImage()) {
        domain->notifyProgramDataChanged();
    }
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

static constexpr ScalarPropertyData<ApplicationProgramObject, uint16_t> kObjectTypeData{ &getObjectType, nullptr };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint16_t> kApplicationIdData{ &getApplicationId, &setApplicationId };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint8_t> kApplicationVersionData{ &getApplicationVersion, &setApplicationVersion };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint16_t> kApplicationNumberData{ &getApplicationNumber, &setApplicationNumber };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint8_t> kApplicationAreaData{ &getApplicationArea, &setApplicationArea };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint16_t> kApplicationManufacturerData{ &getApplicationManufacturer, &setApplicationManufacturer };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint8_t> kProgramStateData{ &getProgramState, &setProgramState };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint8_t> kProgramControlData{ nullptr, &setProgramControl };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint16_t> kParameterStartData{ &getParameterStart, &setParameterStart };
static constexpr ScalarPropertyData<ApplicationProgramObject, uint16_t> kParameterEndData{ &getParameterEnd, &setParameterEnd };

util::Result<uint32_t> getTableReference(const ApplicationProgramObject& /*object*/) {
    return tableseg::kApplicationCodeBase;
}

static constexpr ScalarPropertyData<ApplicationProgramObject, uint32_t> kTableReferenceData{
    &getTableReference,
    nullptr
};

static constexpr StringPropertyKind kProgramNameKind = StringPropertyKind::ProgramName;
static constexpr StringPropertyKind kProgramDescriptionKind = StringPropertyKind::ProgramDescription;

static constexpr PropertyHandler kAppProgramHandlers[] = {
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
static_assert(validatePropertyTableLayout(kAppProgramHandlers, kAppProgramHandlerCount).isOk(),
              "kAppProgramHandlers breaks the property kernel's rules");
} // namespace

KernelBinding ApplicationProgramObject::kernelBinding() const {
    KernelBinding binding;
    binding.handlers = kAppProgramHandlers;
    binding.handlerCount = kAppProgramHandlerCount;
    binding.context = PropertyContext{const_cast<ApplicationProgramObject*>(this), _validationPolicy};
    return binding;
}

// === Parameter image ===

void ApplicationProgramObject::setProgramData(std::span<const uint8_t> data) {
    if (data.empty()) {
        resetParameterImage();
        return;
    }
    const size_t count = std::min(data.size(), _image.image.size());
    std::copy_n(data.begin(), count, _image.image.begin());
}

void ApplicationProgramObject::bindParameterImage(const ParameterImageBinding& binding) {
    _image = binding;
    resetParameterImage();
}

void ApplicationProgramObject::resetParameterImage() {
    if (hasParameterImage()) {
        std::copy_n(_image.defaults, _image.image.size(), _image.image.begin());
    }
}

bool ApplicationProgramObject::acceptParameterImage() {
    if (parameterImageWithinBounds()) {
        return true;
    }
    KNX_LOGE("KNX.AppProgram", "Parameter image holds a value outside its bounds: refused, defaults in use");
    resetParameterImage();
    _loadState = loadstate::kError;
    return false;
}

void ApplicationProgramObject::appendParameterImageRecord(std::vector<uint8_t>& out) const {
    if (_loadState != loadstate::kLoaded) {
        // Not loaded: the next start must not take the defaults, or half of a
        // download, for a loaded application.
        out.push_back(0u);
        return;
    }
    for (unsigned shift = 32u; shift != 0u; shift -= 8u) {
        out.push_back(static_cast<uint8_t>(_image.fingerprint >> (shift - 8u)));
    }
    out.insert(out.end(), _image.image.begin(), _image.image.end());
}

void ApplicationProgramObject::restoreParameterImageRecord(std::span<const uint8_t> record) {
    const auto stored = record.size() >= kParameterImageRecordOverhead
                            ? (static_cast<uint32_t>(record[0]) << 24u) | (static_cast<uint32_t>(record[1]) << 16u)
                                  | (static_cast<uint32_t>(record[2]) << 8u) | record[3]
                            : 0u;
    const bool sameLayout = record.size() == kParameterImageRecordOverhead + _image.image.size()
                            && stored == _image.fingerprint;
    if (sameLayout) {
        std::copy(record.begin() + kParameterImageRecordOverhead, record.end(), _image.image.begin());
        if (parameterImageWithinBounds()) {
            return;
        }
    }
    KNX_LOGW("KNX.AppProgram", "Stored parameters are %s: defaults in use, application program unloaded",
             record.size() == 1u ? "marked not loaded" : sameLayout ? "outside their bounds" : "of another layout");
    resetParameterImage();
    _loadState = loadstate::kUnloaded;
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
