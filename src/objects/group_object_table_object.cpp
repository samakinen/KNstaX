// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_object_table_object.cpp
 * @brief Group object table implementation (Property Kernel)
 */

#include "knx/objects/group_object_table_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include <algorithm>
#include <set>

namespace knx {
namespace objects {

namespace {

constexpr const char* TAG = "KNX.Obj.GroupObjectTable";

// PID_TABLE of the Group Object Table Object (Object Type 9) has a normative
// wire format: KNX 03/05/01 §4.12.5.2.4.1, Tables 51 and 52 — "Group Object
// Table - Realisation Type 7", which is the realisation mask 07B0 (System B)
// devices use.
//
//   index 0 : Length, 2 octets  — number of Group Object descriptors
//   index n : Group Object descriptor, 2 octets, big-endian
//
// A descriptor carries only communication policy plus the value width.  It
// deliberately contains neither the group address (that lives in the Address
// Table, referenced through the Association Table) nor the live value.
constexpr size_t kTableLengthFieldBytes = 2u;
constexpr size_t kDescriptorBytes = 2u;

size_t descriptorCount(const GroupObjectTableDomain& domain) {
    size_t count = 0;
    for (const auto& obj : domain.objects()) {
        if (obj) {
            ++count;
        }
    }
    return count;
}

util::Result<uint16_t> getObjectType(const GroupObjectTableDomain& /*domain*/) {
    return InterfaceObjectType::groupObjectTable().value();
}

util::Result<void> readTableData(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    const auto* domain = static_cast<const GroupObjectTableDomain*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const size_t count = descriptorCount(*domain);
    if (count > 0xFFFFu) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const size_t totalSize = kTableLengthFieldBytes + count * kDescriptorBytes;

    const size_t offset = static_cast<size_t>(startIndex.value);
    const size_t requested = static_cast<size_t>(elementCount);
    if (offset + requested > totalSize) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    size_t remaining = requested;
    size_t skip = offset;

    auto emitByte = [&](uint8_t value) -> util::Result<void> {
        if (remaining == 0) {
            return util::Result<void>::ok();
        }
        if (skip > 0) {
            --skip;
            return util::Result<void>::ok();
        }
        auto res = out.u8(value);
        if (res.isError()) {
            return res;
        }
        --remaining;
        return util::Result<void>::ok();
    };

    auto emitWord = [&](uint16_t value) -> util::Result<void> {
        auto res = emitByte(static_cast<uint8_t>((value >> 8) & 0xFFu));
        if (res.isError()) return res;
        return emitByte(static_cast<uint8_t>(value & 0xFFu));
    };

    // Table 51 index 0: number of entries.
    auto res = emitWord(static_cast<uint16_t>(count));
    if (res.isError()) return res;

    for (const auto& obj : domain->objects()) {
        if (!obj) {
            continue;
        }
        if (remaining == 0) {
            break;
        }
        res = emitWord(obj->descriptor());
        if (res.isError()) return res;
    }

    return util::Result<void>::ok();
}

util::Result<void> writeTableData(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* /*userData*/)
{
    auto* domain = const_cast<GroupObjectTableDomain*>(
        static_cast<const GroupObjectTableDomain*>(context.domain));
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (in.remaining() < elementCount) {
        return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
    }

    // ETS downloads descriptors to apply the flags an integrator set on each
    // communication object.  Only the policy bits are adopted: the Value Field
    // Type is a property of the firmware's datapoint, not something a download
    // may redefine, so a mismatch is reported rather than silently applied.
    size_t absolute = static_cast<size_t>(startIndex.value);
    size_t consumed = 0;

    // Skip any part of the 2-octet length field this write covers; it is
    // derived from the live object table and is not settable.
    while (absolute < kTableLengthFieldBytes && consumed < elementCount) {
        auto skipped = in.u8();
        if (skipped.isError()) {
            return skipped.error();
        }
        ++absolute;
        ++consumed;
    }

    // A write that starts or ends mid-descriptor cannot be applied atomically;
    // ETS always writes whole descriptors, so treat anything else as malformed
    // rather than corrupting a flag set with half an update.
    if (((absolute - kTableLengthFieldBytes) % kDescriptorBytes) != 0u) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (((static_cast<size_t>(elementCount) - consumed) % kDescriptorBytes) != 0u) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    while (consumed + kDescriptorBytes <= static_cast<size_t>(elementCount)) {
        auto word = in.u16be();
        if (word.isError()) {
            return word.error();
        }
        consumed += kDescriptorBytes;

        const size_t slot = (absolute - kTableLengthFieldBytes) / kDescriptorBytes;
        absolute += kDescriptorBytes;

        auto* obj = domain->objectAt(slot);
        if (obj == nullptr) {
            continue;
        }

        uint8_t valueFieldType = 0;
        const auto flags = application::decodeGroupObjectDescriptor(word.value(), valueFieldType);
        if (valueFieldType != obj->valueFieldType()) {
            KNX_LOGW(TAG,
                     "Slot %zu descriptor value type 0x%02X != firmware 0x%02X; keeping firmware width",
                     slot, static_cast<unsigned>(valueFieldType),
                     static_cast<unsigned>(obj->valueFieldType()));
        }
        obj->setFlags(flags);
    }

    return util::Result<void>::ok();
}

static const ScalarPropertyData<GroupObjectTableDomain, uint16_t> kObjectTypeData{
    &getObjectType,
    nullptr
};

util::Result<uint32_t> getTableReference(const GroupObjectTableDomain& /*domain*/) {
    return tableseg::kGroupObjectTableBase;
}

static const ScalarPropertyData<GroupObjectTableDomain, uint32_t> kTableReferenceData{
    &getTableReference,
    nullptr
};

static const PropertyHandler kGroupObjectTableHandlers[] = {
    ScalarProperty<GroupObjectTableDomain, uint16_t>::make(
        application::PropertyID::ObjectType,
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kObjectTypeData),

    LoadControlProperty<GroupObjectTableDomain>::make(
        static_cast<application::PropertyID>(GroupObjectTableProperty::LoadStateControl)),

    // PID_TABLE_REFERENCE: address of the memory-mapped load segment; the
    // streamed content is accepted but not applied (firmware-defined objects).
    ScalarProperty<GroupObjectTableDomain, uint32_t>::make(
        static_cast<application::PropertyID>(GroupObjectTableProperty::TableReference),
        application::PropertyDataType::UnsignedLong,
        PropertyCapability::ReadOnly,
        &kTableReferenceData),

    {
        static_cast<application::PropertyID>(GroupObjectTableProperty::TableData),
        application::PropertyDataType::GenericData,
        PropertyCapability::ReadWrite,
        GroupObjectTableDomain::kMaxSerializedBytes,
        1,
        &readTableData,
        &writeTableData,
        nullptr
    },

    // PID_GO_DIAGNOSTICS (03/05/01 §4.8.1): the Group Object Diagnostics
    // function itself lives in the BAU (bau_go_diagnostics.cpp) and is reached
    // through the Function Property services. Declaring it here is what makes
    // A_PropertyDescription_Read answer PDT_FUNCTION for it, which is how ETS
    // decides the device supports Group Object Diagnostics.
    FunctionProperty::make(
        static_cast<application::PropertyID>(GroupObjectTableProperty::GoDiagnostics))
};

constexpr size_t kGroupObjectTableHandlerCount = sizeof(kGroupObjectTableHandlers) / sizeof(kGroupObjectTableHandlers[0]);

util::Result<GroupObjectTableDomain::Index> toDomainIndex(GroupObjectIndex index) {
    if (!index.isValid() || index.value() >= GroupObjectTableDomain::kMaxObjects) {
        return util::Result<GroupObjectTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    return GroupObjectTableDomain::Index{static_cast<uint16_t>(index.value())};
}

} // namespace

// === GroupObjectTableDomain ===

util::Result<GroupObjectTableDomain::Index> GroupObjectTableDomain::add(std::unique_ptr<application::GroupObject> obj) {
    if (_objects.size() >= kMaxObjects || !obj) {
        return util::Result<GroupObjectTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    const Index index{static_cast<uint16_t>(_objects.size())};
    _objects.push_back(std::move(obj));
    return index;
}

util::Result<void> GroupObjectTableDomain::remove(Index index) {
    if (index.value >= _objects.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    _objects.erase(index.value);
    return util::Result<void>::ok();
}

application::GroupObject* GroupObjectTableDomain::get(Index index) {
    if (index.value >= _objects.size()) {
        return nullptr;
    }
    return _objects[index.value].get();
}

const application::GroupObject* GroupObjectTableDomain::get(Index index) const {
    if (index.value >= _objects.size()) {
        return nullptr;
    }
    return _objects[index.value].get();
}

uint16_t GroupObjectTableDomain::size() const {
    return static_cast<uint16_t>(_objects.size());
}

void GroupObjectTableDomain::clear() {
    _objects.clear();
}

void GroupObjectTableDomain::reserve(uint16_t capacity) {
    (void)capacity;
}

util::Result<void> GroupObjectTableDomain::load(std::vector<std::unique_ptr<application::GroupObject>> objects) {
    if (objects.size() > kMaxObjects) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    _objects.clear();
    for (auto& obj : objects) {
        _objects.push_back(std::move(obj));
    }
    return util::Result<void>::ok();
}

application::GroupObject* GroupObjectTableDomain::findByAddress(const GroupAddress& address) {
    for (auto& obj : _objects) {
        if (obj && obj->getAddress() == address) {
            return obj.get();
        }
    }
    return nullptr;
}

const application::GroupObject* GroupObjectTableDomain::findByAddress(const GroupAddress& address) const {
    for (const auto& obj : _objects) {
        if (obj && obj->getAddress() == address) {
            return obj.get();
        }
    }
    return nullptr;
}

util::Result<size_t> GroupObjectTableDomain::findAllByAddress(const GroupAddress& address, std::span<GroupObjectIndex> out) const {
    size_t required = 0;
    for (size_t i = 0; i < _objects.size(); ++i) {
        if (_objects[i] && _objects[i]->getAddress() == address) {
            ++required;
        }
    }

    if (out.empty()) {
        return required;
    }

    if (out.size() < required) {
        return util::ErrorCode::BufferTooSmall;
    }

    size_t written = 0;
    for (size_t i = 0; i < _objects.size(); ++i) {
        if (_objects[i] && _objects[i]->getAddress() == address) {
            out[written++] = GroupObjectIndex(static_cast<uint16_t>(i));
        }
    }
    return written;
}

bool GroupObjectTableDomain::hasAddress(const GroupAddress& address) const {
    return findByAddress(address) != nullptr;
}

util::Result<size_t> GroupObjectTableDomain::getAllAddresses(std::span<GroupAddress> out) const {
    size_t count = 0;
    for (const auto& obj : _objects) {
        if (obj) ++count;
    }
    if (out.empty()) return count;
    if (out.size() < count) {
        return util::ErrorCode::BufferTooSmall;
    }
    size_t written = 0;
    for (const auto& obj : _objects) {
        if (!obj) continue;
        out[written++] = obj->getAddress();
    }
    return written;
}

bool GroupObjectTableDomain::isValid() const {
    for (const auto& obj : _objects) {
        if (!obj || !obj->isValid()) {
            return false;
        }
    }
    return true;
}

GroupObjectTableDomain::Statistics GroupObjectTableDomain::getStatistics() const {
    Statistics stats{};
    stats.totalObjects = size();
    stats.validObjects = 0;
    stats.activeObjects = 0;

    for (const auto& obj : _objects) {
        if (obj) {
            if (obj->isValid()) {
                stats.validObjects++;
                if (!obj->getRawValue().empty()) {
                    stats.activeObjects++;
                }
            }
        }
    }

    stats.uniqueAddresses = countUniqueAddresses();
    return stats;
}

uint16_t GroupObjectTableDomain::countUniqueAddresses() const {
    std::set<uint16_t> uniqueAddrs;
    for (const auto& obj : _objects) {
        if (obj) {
            uniqueAddrs.insert(obj->getAddress().raw);
        }
    }
    return static_cast<uint16_t>(uniqueAddrs.size());
}

// === GroupObjectTableObject ===

GroupObjectTableObject::GroupObjectTableObject() {
    constexpr uint16_t kDefaultReserve = 32;
    _table.reserve(kDefaultReserve);

    const auto validation = validatePropertyTable(kGroupObjectTableHandlers, kGroupObjectTableHandlerCount);
    if (validation.isError()) {
        KNX_LOGE("KNX.GOTable", "Invalid property handler table (err=%d)", static_cast<int>(validation.error()));
    }
}

KernelBinding GroupObjectTableObject::kernelBinding() const {
    KernelBinding binding;
    binding.handlers = kGroupObjectTableHandlers;
    binding.handlerCount = kGroupObjectTableHandlerCount;
    binding.context = PropertyContext{const_cast<GroupObjectTableDomain*>(&_table), _validationPolicy};
    return binding;
}

GroupObjectIndex GroupObjectTableObject::addGroupObject(std::unique_ptr<application::GroupObject> obj) {
    auto res = _table.add(std::move(obj));
    if (res.isError()) {
        KNX_LOGW("KNX.GOTable", "Group object table full or invalid object");
        return GroupObjectIndex::invalid();
    }
    return GroupObjectIndex(res.value().value);
}

util::Result<void> GroupObjectTableObject::removeObject(GroupObjectIndex index) {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return util::Result<void>::err(domainIndex.error());
    }
    return _table.remove(domainIndex.value());
}

application::GroupObject* GroupObjectTableObject::getGroupObject(GroupObjectIndex index) {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return nullptr;
    }
    return _table.get(domainIndex.value());
}

const application::GroupObject* GroupObjectTableObject::getGroupObject(GroupObjectIndex index) const {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return nullptr;
    }
    return _table.get(domainIndex.value());
}

void GroupObjectTableObject::clear() {
    _table.clear();
}

application::GroupObject* GroupObjectTableObject::findByAddress(const GroupAddress& address) {
    return _table.findByAddress(address);
}

const application::GroupObject* GroupObjectTableObject::findByAddress(const GroupAddress& address) const {
    return _table.findByAddress(address);
}

util::Result<size_t> GroupObjectTableObject::findAllByAddress(const GroupAddress& address,
                                                              std::span<GroupObjectIndex> out) const {
    return _table.findAllByAddress(address, out);
}

bool GroupObjectTableObject::hasAddress(const GroupAddress& address) const {
    return _table.hasAddress(address);
}

util::Result<void> GroupObjectTableObject::loadTable(std::vector<std::unique_ptr<application::GroupObject>> objects) {
    return _table.load(std::move(objects));
}

void GroupObjectTableObject::reserve(uint16_t capacity) {
    _table.reserve(capacity);
}

util::Result<size_t> GroupObjectTableObject::getAllAddresses(std::span<GroupAddress> out) const {
    return _table.getAllAddresses(out);
}

bool GroupObjectTableObject::isValid() const {
    return _table.isValid();
}

GroupObjectTableObject::Statistics GroupObjectTableObject::getStatistics() const {
    const auto stats = _table.getStatistics();
    return {stats.totalObjects, stats.validObjects, stats.activeObjects, stats.uniqueAddresses};
}

} // namespace objects
} // namespace knx
