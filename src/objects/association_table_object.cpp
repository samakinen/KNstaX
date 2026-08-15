// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file association_table_object.cpp
 * @brief Association table object implementation (Property Kernel)
 */

#include "knx/objects/association_table_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include <algorithm>

namespace knx {
namespace objects {

template <>
struct ScalarCodec<AssociationEntry> {
    static util::Result<void> write(util::ByteWriter& out, const AssociationEntry& value) {
        auto res = out.u16be(value.addressIndex.value());
        if (res.isError()) {
            return res;
        }
        return out.u16be(value.groupObjectNumber.value());
    }

    static util::Result<AssociationEntry> read(util::ByteReader& in) {
        auto addr = in.u16be();
        if (addr.isError()) {
            return addr.error();
        }
        auto go = in.u16be();
        if (go.isError()) {
            return go.error();
        }
        return AssociationEntry(AddressTableIndex(addr.value()), GroupObjectIndex(go.value()));
    }
};

namespace {
util::Result<uint16_t> getObjectType(const AssociationTableDomain& /*domain*/) {
    return InterfaceObjectType::associationTable().value();
}

util::Result<AssociationEntry> getTableEntry(const AssociationTableDomain& domain, DomainIndex index) {
    return domain.get(index);
}

util::Result<void> setTableEntry(AssociationTableDomain& domain, DomainIndex index, const AssociationEntry& value) {
    return domain.setExpand(index, value);
}

util::Result<void> validateTableEntry(const AssociationTableDomain& /*domain*/, DomainIndex /*index*/, const AssociationEntry& value) {
    if (!value.isValid()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return util::Result<void>::ok();
}

static const ScalarPropertyData<AssociationTableDomain, uint16_t> kObjectTypeData{
    &getObjectType,
    nullptr
};

util::Result<uint32_t> getTableReference(const AssociationTableDomain& /*domain*/) {
    return tableseg::kAssociationTableBase;
}

static const ScalarPropertyData<AssociationTableDomain, uint32_t> kTableReferenceData{
    &getTableReference,
    nullptr
};

/// 03/05/01 §4.12.3: "The maximum number of elements in the Property shall show
/// the maximum size of the table and current length shall show the current
/// usage" — the latter is what a read of array element 0 answers.
util::Result<uint16_t> countTableEntries(const AssociationTableDomain& domain) {
    return domain.size();
}

static const VariableTablePropertyData<AssociationTableDomain, AssociationEntry> kTableData{
    &getTableEntry,
    &setTableEntry,
    &validateTableEntry,
    &countTableEntries
};

static const PropertyHandler kAssociationTableHandlers[] = {
    ScalarProperty<AssociationTableDomain, uint16_t>::make(
        application::PropertyID::ObjectType,
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kObjectTypeData),

    LoadControlProperty<AssociationTableDomain>::make(
        static_cast<application::PropertyID>(AssociationTableProperty::LoadState)),

    // PID_TABLE_REFERENCE: address of the memory-mapped load segment; ETS
    // SystemB streams the table content there with A_Memory_Write.
    ScalarProperty<AssociationTableDomain, uint32_t>::make(
        static_cast<application::PropertyID>(AssociationTableProperty::TableReference),
        application::PropertyDataType::UnsignedLong,
        PropertyCapability::ReadOnly,
        &kTableReferenceData),

    // PDT_GENERIC_04: ETS SystemB derives the association entry format
    // (2-byte TSAP + 2-byte ASAP) from this descriptor.
    VariableTableProperty<AssociationTableDomain, AssociationEntry, AssociationTableDomain::kMaxEntries>::make(
        static_cast<application::PropertyID>(AssociationTableProperty::TableData),
        application::PropertyDataType::Generic04,
        PropertyCapability::ReadWrite,
        &kTableData)
};

constexpr size_t kAssociationTableHandlerCount = sizeof(kAssociationTableHandlers) / sizeof(kAssociationTableHandlers[0]);

util::Result<AssociationTableDomain::Index> toDomainIndex(uint16_t index) {
    if (index >= AssociationTableDomain::kMaxEntries) {
        return util::Result<AssociationTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    return AssociationTableDomain::Index{static_cast<uint16_t>(index)};
}

} // namespace

// === AssociationTableDomain ===

util::Result<void> AssociationTableDomain::add(const AssociationEntry& entry) {
    if (!entry.isValid()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (_entries.size() >= kMaxEntries) {
        return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
    }
    if (hasAssociation(entry.addressIndex, entry.groupObjectNumber)) {
        return util::Result<void>::ok();
    }
    _entries.push_back(entry);
    return util::Result<void>::ok();
}

util::Result<void> AssociationTableDomain::remove(Index index) {
    if (index.value >= _entries.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    _entries.erase(index.value);
    return util::Result<void>::ok();
}

util::Result<void> AssociationTableDomain::set(Index index, const AssociationEntry& entry) {
    if (index.value >= _entries.size() || !entry.isValid()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    _entries[index.value] = entry;
    return util::Result<void>::ok();
}

util::Result<void> AssociationTableDomain::setExpand(Index index, const AssociationEntry& entry) {
    if (index.value >= kMaxEntries) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (_entries.size() <= index.value) {
        _entries.resize(index.value + 1);
    }
    _entries[index.value] = entry;
    return util::Result<void>::ok();
}

AssociationEntry AssociationTableDomain::get(Index index) const {
    if (index.value >= _entries.size()) {
        return AssociationEntry();
    }
    return _entries[index.value];
}

const AssociationEntry* AssociationTableDomain::getPtr(Index index) const {
    if (index.value >= _entries.size()) {
        return nullptr;
    }
    return &_entries[index.value];
}

uint16_t AssociationTableDomain::removeEntriesForAddress(AddressTableIndex addressIndex) {
    size_t removed = 0;
    for (size_t i = 0; i < _entries.size();) {
        if (_entries[i].addressIndex == addressIndex) {
            _entries.erase(i);
            ++removed;
            continue;
        }
        ++i;
    }
    return static_cast<uint16_t>(removed);
}

uint16_t AssociationTableDomain::removeEntriesForGroupObject(GroupObjectIndex groupObjectNumber) {
    size_t removed = 0;
    for (size_t i = 0; i < _entries.size();) {
        if (_entries[i].groupObjectNumber == groupObjectNumber) {
            _entries.erase(i);
            ++removed;
            continue;
        }
        ++i;
    }
    return static_cast<uint16_t>(removed);
}

util::Result<size_t> AssociationTableDomain::findGroupObjects(AddressTableIndex addressIndex,
                                                              std::span<GroupObjectIndex> out) const {
    size_t required = 0;
    for (const auto& entry : _entries) {
        if (entry.addressIndex == addressIndex) {
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
    for (const auto& entry : _entries) {
        if (entry.addressIndex == addressIndex) {
            out[written++] = entry.groupObjectNumber;
        }
    }
    return written;
}

util::Result<size_t> AssociationTableDomain::findAddressIndices(GroupObjectIndex groupObjectNumber,
                                                                std::span<AddressTableIndex> out) const {
    size_t required = 0;
    for (const auto& entry : _entries) {
        if (entry.groupObjectNumber == groupObjectNumber) {
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
    for (const auto& entry : _entries) {
        if (entry.groupObjectNumber == groupObjectNumber) {
            out[written++] = entry.addressIndex;
        }
    }
    return written;
}

bool AssociationTableDomain::hasAssociation(AddressTableIndex addressIndex, GroupObjectIndex groupObjectNumber) const {
    return std::any_of(_entries.begin(), _entries.end(),
                       [addressIndex, groupObjectNumber](const AssociationEntry& e) {
                           return e.addressIndex == addressIndex && e.groupObjectNumber == groupObjectNumber;
                       });
}

util::Result<void> AssociationTableDomain::load(std::span<const AssociationEntry> entries) {
    if (entries.size() > kMaxEntries) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    for (const auto& entry : entries) {
        if (!entry.isValid()) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
    }
    if (!_entries.assign(entries)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return util::Result<void>::ok();
}

void AssociationTableDomain::truncate(uint16_t size) {
    if (size < _entries.size()) {
        _entries.resize(size);
    }
}

void AssociationTableDomain::clear() {
    _entries.clear();
}

void AssociationTableDomain::reserve(uint16_t capacity) {
    (void)capacity;
}

uint16_t AssociationTableDomain::size() const {
    return static_cast<uint16_t>(_entries.size());
}

bool AssociationTableDomain::isValid() const {
    for (const auto& entry : _entries) {
        if (!entry.isValid()) {
            return false;
        }
    }
    return true;
}

// === AssociationTableObject ===

AssociationTableObject::AssociationTableObject() {
    constexpr uint16_t kDefaultReserve = 64;
    _table.reserve(kDefaultReserve);

    const auto validation = validatePropertyTable(kAssociationTableHandlers, kAssociationTableHandlerCount);
    if (validation.isError()) {
        KNX_LOGE("KNX.AssocTable", "Invalid property handler table (err=%d)", static_cast<int>(validation.error()));
    }
}

KernelBinding AssociationTableObject::kernelBinding() const {
    KernelBinding binding;
    binding.handlers = kAssociationTableHandlers;
    binding.handlerCount = kAssociationTableHandlerCount;
    binding.context = PropertyContext{const_cast<AssociationTableDomain*>(&_table), _validationPolicy};
    return binding;
}

util::Result<void> AssociationTableObject::addEntry(const AssociationEntry& entry) {
    return _table.add(entry);
}

util::Result<void> AssociationTableObject::removeEntry(uint16_t index) {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return util::Result<void>::err(domainIndex.error());
    }
    return _table.remove(domainIndex.value());
}

uint16_t AssociationTableObject::removeEntriesForAddress(AddressTableIndex addressIndex) {
    return _table.removeEntriesForAddress(addressIndex);
}

uint16_t AssociationTableObject::removeEntriesForGroupObject(GroupObjectIndex groupObjectNumber) {
    return _table.removeEntriesForGroupObject(groupObjectNumber);
}

void AssociationTableObject::clearEntries() {
    _table.clear();
}

util::Result<void> AssociationTableObject::setEntry(uint16_t index, const AssociationEntry& entry) {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return util::Result<void>::err(domainIndex.error());
    }
    return _table.set(domainIndex.value(), entry);
}

util::Result<void> AssociationTableObject::setEntryExpandUnchecked(uint16_t index, const AssociationEntry& entry) {
    if (index == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const uint16_t zeroBased = static_cast<uint16_t>(index - 1);
    auto domainIndex = toDomainIndex(zeroBased);
    if (domainIndex.isError()) {
        return util::Result<void>::err(domainIndex.error());
    }
    return _table.setExpand(domainIndex.value(), entry);
}

util::Result<size_t> AssociationTableObject::findGroupObjects(AddressTableIndex addressIndex,
                                                              std::span<GroupObjectIndex> out) const {
    return _table.findGroupObjects(addressIndex, out);
}

util::Result<size_t> AssociationTableObject::findAddressIndices(GroupObjectIndex groupObjectNumber,
                                                                std::span<AddressTableIndex> out) const {
    return _table.findAddressIndices(groupObjectNumber, out);
}

bool AssociationTableObject::hasAssociation(AddressTableIndex addressIndex, GroupObjectIndex groupObjectNumber) const {
    return _table.hasAssociation(addressIndex, groupObjectNumber);
}

const AssociationEntry* AssociationTableObject::getEntry(uint16_t index) const {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return nullptr;
    }
    return _table.getPtr(domainIndex.value());
}

util::Result<void> AssociationTableObject::loadTable(std::span<const AssociationEntry> entries) {
    return _table.load(entries);
}

void AssociationTableObject::reserve(uint16_t capacity) {
    _table.reserve(capacity);
}

util::Result<size_t> AssociationTableObject::getAllEntries(std::span<AssociationEntry> out) const {
    const size_t count = _table.entries().size();
    if (out.empty()) return count;
    if (out.size() < count) {
        return util::ErrorCode::BufferTooSmall;
    }
    size_t written = 0;
    for (const auto& entry : _table.entries()) {
        out[written++] = entry;
    }
    return written;
}

bool AssociationTableObject::isValid() const {
    return _table.isValid();
}

} // namespace objects
} // namespace knx
