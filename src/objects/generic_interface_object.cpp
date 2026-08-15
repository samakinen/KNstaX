// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file generic_interface_object.cpp
 * @brief Generic Interface Object implementation
 */

#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/object_property_compliance.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/objects/reference_object_registry.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <limits>

namespace knx {
namespace objects {

namespace {
static const char* TAG = "KNX.GenericObj";

util::Result<uint16_t> getObjectType(const GenericInterfaceObject& domain) {
    return domain.objectType().value();
}

static const ScalarPropertyData<GenericInterfaceObject, uint16_t> kObjectTypeData{ &getObjectType, nullptr };
} // namespace

GenericInterfaceObject::GenericInterfaceObject(InterfaceObjectType type)
    : _type(type)
    , _registrations()
    , _values()
    , _handlerInfo()
    , _handlers()
{
    // Pull the schema from the reference-object manifest — single source of truth.
    const auto manifestEntries = referenceObjectPropertyManifestEntries(type);

    // Convert manifest entries to PropertyRegistrationInfo spans expected by the init path.
    util::FixedVector<PropertyRegistrationInfo, kMaxRegistrationCount> regs;
    for (const auto& entry : manifestEntries) {
        if (!regs.push_back(entry.registration)) {
            KNX_LOGE(TAG,
                     "Manifest registration overflow for reference object type %u",
                     static_cast<unsigned>(type.value()));
            _handlersValidated = true;
            _handlersValid = false;
            return;
        }
    }
    initFromRegistrations_(regs.span());
}

void GenericInterfaceObject::initFromRegistrations_(std::span<const PropertyRegistrationInfo> registrations)
{
    auto markInvalid = [this]() {
        _handlersValidated = true;
        _handlersValid = false;
    };

    if (!_registrations.assign(registrations)) {
        KNX_LOGE(TAG,
                 "Property registration count %u exceeds fixed bound %u for object type %u",
                 static_cast<unsigned>(registrations.size()),
                 static_cast<unsigned>(kMaxRegistrationCount),
                 static_cast<unsigned>(_type.value()));
        markInvalid();
        return;
    }

    _values.emplace(application::PropertyID::ObjectType, std::vector<uint8_t>{});

    if (!_handlers.push_back(ScalarProperty<GenericInterfaceObject, uint16_t>::make(
            application::PropertyID::ObjectType,
            application::PropertyDataType::UnsignedInt,
            PropertyCapability::ReadOnly,
            &kObjectTypeData))) {
        KNX_LOGE(TAG, "Failed to stage ObjectType handler for object type %u", static_cast<unsigned>(_type.value()));
        markInvalid();
        return;
    }

    const auto schemaValidation = validateObjectPropertyRegistrations(_type, _registrations.span());
    if (schemaValidation.isError()) {
        KNX_LOGE(TAG,
                 "Property registration schema validation failed for object type %u (err=%d)",
                 static_cast<unsigned>(_type.value()),
                 static_cast<int>(schemaValidation.error()));
        markInvalid();
        return;
    }

    for (const auto& reg : _registrations.span()) {
        if (reg.propertyId == application::PropertyID::ObjectType) {
            continue;
        }

        if (reg.maxElements == 0) {
            KNX_LOGE(TAG, "Skipping PID %u: maxElements must be > 0", static_cast<unsigned>(reg.propertyId));
            continue;
        }

        _values.emplace(reg.propertyId, std::vector<uint8_t>{});

        const uint16_t maxElements = reg.maxElements;
        uint8_t elementSize = propertyElementSize(reg.dataType);
        uint8_t overrideSize = 0;
        if (elementSize == 0) {
            elementSize = 1;
            overrideSize = 1;
        }

        GenericPropertyInfo info;
        info.id = reg.propertyId;
        info.maxElements = maxElements;
        info.elementSize = elementSize;
        info.type = reg.dataType;
        if (!_handlerInfo.push_back(info)) {
            KNX_LOGE(TAG,
                     "Property handler info capacity exceeded for object type %u",
                     static_cast<unsigned>(_type.value()));
            markInvalid();
            return;
        }
        const auto* handlerInfo = &_handlerInfo.back();

        PropertyCapability capability = PropertyCapability::ReadOnly;
        if (reg.access == application::PropertyAccess::ReadWrite) {
            capability = PropertyCapability::ReadWrite;
        } else if (reg.access == application::PropertyAccess::WriteOnly) {
            capability = PropertyCapability::WriteOnly;
        }

        PropertyHandler handler;
        handler.id = reg.propertyId;
        handler.type = reg.dataType;
        handler.capability = capability;
        handler.maxElements = maxElements;
        handler.elementSizeOverride = overrideSize;
        handler.read = canRead(capability) ? &GenericInterfaceObject::readGeneric : nullptr;
        handler.write = canWrite(capability) ? &GenericInterfaceObject::writeGeneric : nullptr;
        handler.userData = handlerInfo;
        if (!_handlers.push_back(handler)) {
            KNX_LOGE(TAG,
                     "Property handler capacity exceeded for object type %u",
                     static_cast<unsigned>(_type.value()));
            markInvalid();
            return;
        }
    }
}

size_t GenericInterfaceObject::getPropertyRegistrations(std::span<PropertyRegistrationInfo> out) const {
    const size_t count = _registrations.size();
    if (out.empty()) return count;
    const size_t toCopy = std::min(count, out.size());
    for (size_t i = 0; i < toCopy; ++i) out[i] = _registrations[i];
    return toCopy;
}

bool GenericInterfaceObject::validateHandlersOnce_() const {
    if (_handlersValidated) {
        return _handlersValid;
    }

    const auto validation = validatePropertyTable(_handlers.data(), _handlers.size());
    _handlersValid = validation.isOk();
    _handlersValidated = true;

    if (!_handlersValid) {
        KNX_LOGE(TAG, "Invalid property handler table for object type %u (err=%d)",
                 static_cast<unsigned>(_type.value()),
                 static_cast<int>(validation.error()));
    }

    return _handlersValid;
}

util::Result<void> GenericInterfaceObject::readGeneric(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* userData)
{
    const auto* domain = static_cast<const GenericInterfaceObject*>(context.domain);
    const auto* info = static_cast<const GenericPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (elementCount == 0 || info->elementSize == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const uint32_t endIndexExclusive = static_cast<uint32_t>(startIndex.value) + static_cast<uint32_t>(elementCount);
    if (endIndexExclusive > info->maxElements) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    if (static_cast<size_t>(startIndex.value) > (std::numeric_limits<size_t>::max() / info->elementSize) ||
        static_cast<size_t>(elementCount) > (std::numeric_limits<size_t>::max() / info->elementSize)) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    const size_t offset = static_cast<size_t>(startIndex.value) * info->elementSize;
    const size_t count = static_cast<size_t>(elementCount) * info->elementSize;

    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(domain->_valuesMutex);
        auto it = domain->_values.find(info->id);
        if (it == domain->_values.end()) {
            return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
        }
        data = it->second;
    }

    if (offset > data.size() || count > (data.size() - offset)) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    return out.writeBytes(std::span<const uint8_t>(data).subspan(offset, count));
}

util::Result<void> GenericInterfaceObject::writeGeneric(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* userData)
{
    auto* domain = static_cast<GenericInterfaceObject*>(context.domain);
    const auto* info = static_cast<const GenericPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (elementCount == 0 || info->elementSize == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const uint32_t endIndexExclusive = static_cast<uint32_t>(startIndex.value) + static_cast<uint32_t>(elementCount);
    if (endIndexExclusive > info->maxElements) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    if (static_cast<size_t>(startIndex.value) > (std::numeric_limits<size_t>::max() / info->elementSize) ||
        static_cast<size_t>(elementCount) > (std::numeric_limits<size_t>::max() / info->elementSize)) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    const size_t offset = static_cast<size_t>(startIndex.value) * info->elementSize;
    const size_t count = static_cast<size_t>(elementCount) * info->elementSize;
    std::vector<uint8_t> chunk(count, 0);
    auto res = in.readBytes(chunk);
    if (res.isError()) {
        return res;
    }

    std::lock_guard<std::mutex> lock(domain->_valuesMutex);
    auto it = domain->_values.find(info->id);
    if (it == domain->_values.end()) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }

    auto& updated = it->second;
    if (offset > updated.size()) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    if (offset + count > updated.size()) {
        updated.resize(offset + count, 0);
    }
    std::copy(chunk.begin(), chunk.end(), updated.begin() + offset);
    return util::Result<void>::ok();
}

KernelBinding GenericInterfaceObject::kernelBinding() const {
    if (!validateHandlersOnce_()) {
        return {};
    }
    KernelBinding binding;
    binding.handlers = _handlers.data();
    binding.handlerCount = _handlers.size();
    binding.context = PropertyContext{const_cast<GenericInterfaceObject*>(this), _validationPolicy};
    return binding;
}

util::Result<void> GenericInterfaceObject::setPropertyValue(application::PropertyID propertyId,
                                                            std::span<const uint8_t> value)
{
    // The property must be part of this object's manifest; seeding an unknown
    // property would create a value no read handler can ever serve.
    const GenericPropertyInfo* info = nullptr;
    for (const auto& candidate : _handlerInfo) {
        if (candidate.id == propertyId) {
            info = &candidate;
            break;
        }
    }
    if (info == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }

    const size_t capacity = static_cast<size_t>(info->maxElements) * info->elementSize;
    if (capacity != 0u && value.size() > capacity) {
        return util::ErrorCode::BufferTooSmall;
    }

    std::lock_guard<std::mutex> lock(_valuesMutex);
    _values[propertyId].assign(value.begin(), value.end());
    return util::Result<void>::ok();
}

std::vector<uint8_t> GenericInterfaceObject::propertyValue(application::PropertyID propertyId) const
{
    std::lock_guard<std::mutex> lock(_valuesMutex);
    const auto it = _values.find(propertyId);
    return it == _values.end() ? std::vector<uint8_t>{} : it->second;
}

} // namespace objects
} // namespace knx
