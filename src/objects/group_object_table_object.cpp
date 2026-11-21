// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_object_table_object.cpp
 * @brief Group object table implementation (Property Kernel)
 */

#include "knx/config.hpp"
#include "knx/objects/group_object_table_object.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include <algorithm>
#include <set>

namespace knx {
namespace objects {

namespace {

constexpr const char* TAG = "KNX.Obj.GroupObjectTable";

// PID_TABLE of the Group Object Table Object (Object Type 9), 03/05/01
// §4.18.6.2.4 "Group Object Table - Realisation Type 7" (System B, mask 07B0):
//
//   "Table  23 = PID_TABLE  PDT_GENERIC_02[]  Group Object table Format 1"
//   (Table 85), and Table 86: "Property array index […] 0: Length
//   (2 octets); 1: Group Object descriptor (2 octets) […] n".
//
// So it is an array of 2-octet elements: element n is the descriptor of group
// object n, and element 0 is the length, which the kernel answers through
// `count` like for any other array ("The current length shall be the number of
// entries in the Group Object Table"). A descriptor carries only communication
// policy plus the value width (Table 87): neither the group address (that is in
// the Address Table, referenced through the Association Table) nor the value.

size_t descriptorCount(const GroupObjectTableDomain& domain) {
    return domain.size();
}

util::Result<uint16_t> getObjectType(const GroupObjectTableDomain& /*domain*/) {
    return InterfaceObjectType::groupObjectTable().value();
}

util::Result<uint16_t> tableLength(const PropertyContext& context, const void* /*userData*/)
{
    const auto* domain = static_cast<const GroupObjectTableDomain*>(context.domain);
    if (!domain) {
        return util::Result<uint16_t>(util::ErrorCode::InvalidParameter);
    }
    return static_cast<uint16_t>(descriptorCount(*domain));
}

// A write of the length (element 0). ETS sets it to 0 to clear a table before
// downloading it again (see resizeProperty), and refusing that would fail the
// download. The group objects are defined by the firmware, so the write is
// accepted and not applied, as the streamed PID_TABLE_REFERENCE content is: a
// read of element 0 keeps answering the real number of group objects.
util::Result<void> acceptTableLength(const PropertyContext& /*context*/, uint16_t elementCount,
                                     const void* /*userData*/)
{
    KNX_LOGD(TAG, "PID_TABLE length write (%u) accepted, not applied: group objects are firmware-defined",
             static_cast<unsigned>(elementCount));
    return util::Result<void>::ok();
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

    // "The content of this Property shall only be valid in the load state
    // Loaded"; elements past the current length have no content at all.
    const size_t first = startIndex.value;
    if (first + elementCount > descriptorCount(*domain)) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    for (size_t slot = first; slot < first + elementCount; ++slot) {
        const auto* obj = domain->get(GroupObjectTableDomain::Index{static_cast<uint16_t>(slot)});
        auto res = out.u16be(obj->descriptor());
        if (res.isError()) {
            return res;
        }
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

    // ETS downloads descriptors to apply the flags an integrator set on each
    // communication object. Only the policy bits are adopted: the Value Field
    // Type is a property of the firmware's datapoint, not something a download
    // may redefine, so a mismatch is reported rather than silently applied.
    // A descriptor for a slot the firmware has no object for changes nothing.
    for (uint16_t i = 0; i < elementCount; ++i) {
        auto word = in.u16be();
        if (word.isError()) {
            return word.error();
        }

        const size_t slot = static_cast<size_t>(startIndex.value) + i;
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

static constexpr ScalarPropertyData<GroupObjectTableDomain, uint16_t> kObjectTypeData{
    &getObjectType,
    nullptr
};

util::Result<uint32_t> getTableReference(const GroupObjectTableDomain& /*domain*/) {
    return tableseg::kGroupObjectTableBase;
}

static constexpr ScalarPropertyData<GroupObjectTableDomain, uint32_t> kTableReferenceData{
    &getTableReference,
    nullptr
};

static constexpr PropertyHandler kGroupObjectTableHandlers[] = {
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

    // PID_TABLE, writeable: "Writing shall only have effect in the load state
    // Loading" (03/05/01 §4.18.6.2.4.1.1) presupposes a write access. The
    // maximum length "shall be calculated via the Load Control Data Relative
    // Allocation or shall be fixed in the device"; here it is fixed at the
    // capacity of the table. The current length belongs to the firmware, which
    // defines the group objects; a write to element 0 is accepted and not
    // applied (acceptTableLength).
    {
        static_cast<application::PropertyID>(GroupObjectTableProperty::TableData),
        application::PropertyDataType::Generic02,
        PropertyCapability::ReadWrite,
        GroupObjectTableDomain::kMaxObjects,
        0,
        &readTableData,
        &writeTableData,
        nullptr,
        &tableLength,
        &acceptTableLength
    }
#if KNX_FEATURE_GO_DIAGNOSTICS
    ,

    // PID_GO_DIAGNOSTICS (03/05/01 §4.8.1): the Group Object Diagnostics
    // function itself lives in the BAU (src/bau/go_diagnostics.cpp) and is reached
    // through the Function Property services. Declaring it here is what makes
    // A_PropertyDescription_Read answer PDT_FUNCTION for it, which is how ETS
    // decides the device supports Group Object Diagnostics.
    FunctionProperty::make(
        static_cast<application::PropertyID>(GroupObjectTableProperty::GoDiagnostics))
#endif
};

constexpr size_t kGroupObjectTableHandlerCount = sizeof(kGroupObjectTableHandlers) / sizeof(kGroupObjectTableHandlers[0]);
static_assert(validatePropertyTable(kGroupObjectTableHandlers, kGroupObjectTableHandlerCount).isOk(),
              "kGroupObjectTableHandlers breaks the property kernel's rules");

util::Result<GroupObjectTableDomain::Index> toDomainIndex(GroupObjectIndex index) {
    if (!index.isValid() || index.value() >= GroupObjectTableDomain::kMaxObjects) {
        return util::Result<GroupObjectTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    return GroupObjectTableDomain::Index{static_cast<uint16_t>(index.value())};
}

} // namespace

// === GroupObjectTableDomain ===

util::Result<GroupObjectTableDomain::Index> GroupObjectTableDomain::add(const application::GroupObjectConfig& config) {
    if (_store->append(config) == nullptr) {
        return util::Result<GroupObjectTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    return Index{static_cast<uint16_t>(_store->size() - 1u)};
}

util::Result<GroupObjectTableDomain::Index> GroupObjectTableDomain::add(std::unique_ptr<application::GroupObject> obj) {
    if (!obj || _store->appendCopy(*obj) == nullptr) {
        return util::Result<GroupObjectTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    return Index{static_cast<uint16_t>(_store->size() - 1u)};
}

util::Result<void> GroupObjectTableDomain::remove(Index index) {
    if (!_store->erase(index.value)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return util::Result<void>::ok();
}

application::GroupObject* GroupObjectTableDomain::get(Index index) {
    return _store->at(index.value);
}

const application::GroupObject* GroupObjectTableDomain::get(Index index) const {
    return _store->at(index.value);
}

uint16_t GroupObjectTableDomain::size() const {
    return _store->size();
}

void GroupObjectTableDomain::clear() {
    _store->clear();
}

void GroupObjectTableDomain::reserve(uint16_t objects, size_t valueBytes) {
    _store->reserve(objects, valueBytes);
}

util::Result<void> GroupObjectTableDomain::load(std::vector<std::unique_ptr<application::GroupObject>> objects) {
    if (objects.size() > kMaxObjects) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    _store->clear();
    for (auto& obj : objects) {
        if (!obj || _store->appendCopy(*obj) == nullptr) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
    }
    return util::Result<void>::ok();
}

util::Result<size_t> GroupObjectTableDomain::findAllByAddress(const GroupAddress& address, std::span<GroupObjectIndex> out) const {
    size_t required = 0;
    for (uint16_t i = 0; i < _store->size(); ++i) {
        if (_store->at(i)->getAddress() == address) {
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
    for (uint16_t i = 0; i < _store->size(); ++i) {
        if (_store->at(i)->getAddress() == address) {
            out[written++] = GroupObjectIndex(i);
        }
    }
    return written;
}

util::Result<size_t> GroupObjectTableDomain::getAllAddresses(std::span<GroupAddress> out) const {
    const size_t count = _store->size();
    if (out.empty()) return count;
    if (out.size() < count) {
        return util::ErrorCode::BufferTooSmall;
    }
    for (uint16_t i = 0; i < count; ++i) {
        out[i] = _store->at(i)->getAddress();
    }
    return count;
}

bool GroupObjectTableDomain::isValid() const {
    for (uint16_t i = 0; i < _store->size(); ++i) {
        if (!_store->at(i)->isValid()) {
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

    for (uint16_t i = 0; i < _store->size(); ++i) {
        const auto* obj = _store->at(i);
        if (obj->isValid()) {
            stats.validObjects++;
            if (!obj->getRawValue().empty()) {
                stats.activeObjects++;
            }
        }
    }

    stats.uniqueAddresses = countUniqueAddresses();
    return stats;
}

uint16_t GroupObjectTableDomain::countUniqueAddresses() const {
    std::set<uint16_t> uniqueAddrs;
    for (uint16_t i = 0; i < _store->size(); ++i) {
        uniqueAddrs.insert(_store->at(i)->getAddress().raw);
    }
    return static_cast<uint16_t>(uniqueAddrs.size());
}

// === GroupObjectTableObject ===

GroupObjectTableObject::GroupObjectTableObject() = default;

KernelBinding GroupObjectTableObject::kernelBinding() const {
    KernelBinding binding;
    binding.handlers = kGroupObjectTableHandlers;
    binding.handlerCount = kGroupObjectTableHandlerCount;
    binding.context = PropertyContext{const_cast<GroupObjectTableDomain*>(&_table), _validationPolicy};
    return binding;
}

GroupObjectIndex GroupObjectTableObject::addGroupObject(const application::GroupObjectConfig& config) {
    auto res = _table.add(config);
    if (res.isError()) {
        KNX_LOGW("KNX.GOTable", "Group object table full");
        return GroupObjectIndex::invalid();
    }
    return GroupObjectIndex(res.value().value);
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

util::Result<size_t> GroupObjectTableObject::findAllByAddress(const GroupAddress& address,
                                                              std::span<GroupObjectIndex> out) const {
    return _table.findAllByAddress(address, out);
}

util::Result<void> GroupObjectTableObject::loadTable(std::vector<std::unique_ptr<application::GroupObject>> objects) {
    return _table.load(std::move(objects));
}

void GroupObjectTableObject::reserve(uint16_t objects, size_t valueBytes) {
    _table.reserve(objects, valueBytes);
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
