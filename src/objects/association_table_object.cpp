// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file association_table_object.cpp
 * @brief Association table object implementation (Property Kernel)
 */

#include "knx/objects/association_table_object.hpp"
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

static constexpr ScalarPropertyData<AssociationTableDomain, uint16_t> kObjectTypeData{
    &getObjectType,
    nullptr
};

util::Result<uint32_t> getTableReference(const AssociationTableDomain& /*domain*/) {
    return tableseg::kAssociationTableBase;
}

static constexpr ScalarPropertyData<AssociationTableDomain, uint32_t> kTableReferenceData{
    &getTableReference,
    nullptr
};

/// 03/05/01 §4.12.3: "The maximum number of elements in the Property shall show
/// the maximum size of the table and current length shall show the current
/// usage" — the latter is what a read of array element 0 answers.
util::Result<uint16_t> countTableEntries(const AssociationTableDomain& domain) {
    return domain.size();
}

static constexpr VariableTablePropertyData<AssociationTableDomain, AssociationEntry> kTableData{
    &getTableEntry,
    &setTableEntry,
    &validateTableEntry,
    &countTableEntries
};

static constexpr PropertyHandler kAssociationTableHandlers[] = {
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
static_assert(validatePropertyTable(kAssociationTableHandlers, kAssociationTableHandlerCount).isOk(),
              "kAssociationTableHandlers breaks the property kernel's rules");

util::Result<AssociationTableDomain::Index> toDomainIndex(uint16_t index) {
    if (index >= AssociationTableDomain::kMaxEntries) {
        return util::Result<AssociationTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    return AssociationTableDomain::Index{static_cast<uint16_t>(index)};
}

} // namespace

// === AssociationTableDomain ===

static_assert(AssociationTableDomain::kImageBytes == tableseg::kAssociationTableSize,
              "the association table is its load segment");

void AssociationTableDomain::put(uint16_t position, const AssociationEntry& entry) {
    const size_t offset = 2u + kEntryBytes * position;
    _image[offset] = static_cast<uint8_t>(entry.addressIndex.raw >> 8);
    _image[offset + 1u] = static_cast<uint8_t>(entry.addressIndex.raw & 0xFFu);
    _image[offset + 2u] = static_cast<uint8_t>(entry.groupObjectNumber.raw >> 8);
    _image[offset + 3u] = static_cast<uint8_t>(entry.groupObjectNumber.raw & 0xFFu);
}

void AssociationTableDomain::setSize(uint16_t size) {
    // The count octets follow the table, so the segment reads back as the
    // table in force once a load is over, after a restore or a firmware edit.
    _size = size;
    _image[0] = static_cast<uint8_t>(size >> 8);
    _image[1] = static_cast<uint8_t>(size & 0xFFu);
}

bool AssociationTableDomain::loading() const {
    return _loadState == loadstate::kLoading;
}

util::Result<void> AssociationTableDomain::add(const AssociationEntry& entry) {
    if (!entry.isValid()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (loading()) {
        // The append would overwrite the octets ETS has streamed so far, and
        // LoadCompleted replaces the table anyway (it did so before as well,
        // only after accepting the entry).
        return util::Result<void>::err(util::ErrorCode::Busy);
    }
    if (_size >= kMaxEntries) {
        return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
    }
    if (hasAssociation(entry.addressIndex, entry.groupObjectNumber)) {
        return util::Result<void>::ok();
    }
    put(_size, entry);
    setSize(static_cast<uint16_t>(_size + 1u));
    return util::Result<void>::ok();
}

util::Result<void> AssociationTableDomain::remove(Index index) {
    if (index.value >= _size) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const size_t offset = 2u + kEntryBytes * index.value;
    const size_t end = 2u + kEntryBytes * _size;
    std::copy(_image.begin() + static_cast<std::ptrdiff_t>(offset + kEntryBytes),
              _image.begin() + static_cast<std::ptrdiff_t>(end),
              _image.begin() + static_cast<std::ptrdiff_t>(offset));
    setSize(static_cast<uint16_t>(_size - 1u));
    return util::Result<void>::ok();
}

util::Result<void> AssociationTableDomain::set(Index index, const AssociationEntry& entry) {
    if (index.value >= _size || !entry.isValid()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    put(index.value, entry);
    return util::Result<void>::ok();
}

util::Result<void> AssociationTableDomain::setExpand(Index index, const AssociationEntry& entry) {
    if (index.value >= kMaxEntries) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    // Entries skipped over read as invalid until they are written, as when
    // the table was a vector of default-constructed entries.
    for (uint16_t position = _size; position < index.value; ++position) {
        put(position, AssociationEntry());
    }
    put(index.value, entry);
    if (index.value >= _size) {
        setSize(static_cast<uint16_t>(index.value + 1u));
    }
    return util::Result<void>::ok();
}

AssociationEntry AssociationTableDomain::get(Index index) const {
    if (index.value >= _size) {
        return AssociationEntry();
    }
    return at(index.value);
}

uint16_t AssociationTableDomain::removeEntriesForAddress(AddressTableIndex addressIndex) {
    uint16_t kept = 0;
    for (uint16_t position = 0; position < _size; ++position) {
        if (tsapAt(position) == addressIndex.raw) {
            continue;
        }
        if (kept != position) {
            put(kept, at(position));
        }
        ++kept;
    }
    const auto removed = static_cast<uint16_t>(_size - kept);
    setSize(kept);
    return removed;
}

uint16_t AssociationTableDomain::removeEntriesForGroupObject(GroupObjectIndex groupObjectNumber) {
    uint16_t kept = 0;
    for (uint16_t position = 0; position < _size; ++position) {
        if (asapAt(position) == groupObjectNumber.raw) {
            continue;
        }
        if (kept != position) {
            put(kept, at(position));
        }
        ++kept;
    }
    const auto removed = static_cast<uint16_t>(_size - kept);
    setSize(kept);
    return removed;
}

bool AssociationTableDomain::hasGroupObjects(AddressTableIndex addressIndex) const {
    for (uint16_t position = 0; position < _size; ++position) {
        if (tsapAt(position) == addressIndex.raw) {
            return true;
        }
    }
    return false;
}

util::Result<size_t> AssociationTableDomain::findGroupObjects(AddressTableIndex addressIndex,
                                                              std::span<GroupObjectIndex> out) const {
    size_t required = 0;
    forEachGroupObject(addressIndex, [&required](GroupObjectIndex) { ++required; });
    if (out.empty()) {
        return required;
    }
    if (out.size() < required) {
        return util::ErrorCode::BufferTooSmall;
    }
    size_t written = 0;
    forEachGroupObject(addressIndex, [&](GroupObjectIndex groupObject) { out[written++] = groupObject; });
    return written;
}

util::Result<size_t> AssociationTableDomain::findAddressIndices(GroupObjectIndex groupObjectNumber,
                                                                std::span<AddressTableIndex> out) const {
    size_t required = 0;
    for (uint16_t position = 0; position < _size; ++position) {
        if (asapAt(position) == groupObjectNumber.raw) {
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
    for (uint16_t position = 0; position < _size; ++position) {
        if (asapAt(position) == groupObjectNumber.raw) {
            out[written++] = AddressTableIndex(tsapAt(position));
        }
    }
    return written;
}

bool AssociationTableDomain::hasAssociation(AddressTableIndex addressIndex, GroupObjectIndex groupObjectNumber) const {
    for (uint16_t position = 0; position < _size; ++position) {
        if (tsapAt(position) == addressIndex.raw && asapAt(position) == groupObjectNumber.raw) {
            return true;
        }
    }
    return false;
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
    if (loading()) {
        return util::Result<void>::err(util::ErrorCode::Busy);  // as add()
    }
    for (size_t position = 0; position < entries.size(); ++position) {
        put(static_cast<uint16_t>(position), entries[position]);
    }
    setSize(static_cast<uint16_t>(entries.size()));
    return util::Result<void>::ok();
}

util::Result<uint16_t> AssociationTableDomain::applyLoadSegment() {
    const uint16_t count = u16At(0);
    if (count > kMaxEntries) {
        clear();
        return util::ErrorCode::InvalidParameter;
    }
    // Compacted in place. Finding the repeats is O(n^2), once per download:
    // about 13 000 comparisons for 160 entries.
    uint16_t kept = 0;
    for (uint16_t position = 0; position < count; ++position) {
        const AssociationEntry entry = at(position);
        if (!entry.isValid()) {
            continue;
        }
        bool repeat = false;
        for (uint16_t earlier = 0; earlier < kept && !repeat; ++earlier) {
            repeat = at(earlier) == entry;
        }
        if (repeat) {
            continue;
        }
        if (kept != position) {
            put(kept, entry);
        }
        ++kept;
    }
    setSize(kept);
    return kept;
}

void AssociationTableDomain::truncate(uint16_t size) {
    if (size < _size) {
        setSize(size);
    }
}

void AssociationTableDomain::clear() {
    // Only the count: the octets past it are not in force, and while ETS
    // loads they are what it has written.
    setSize(0);
}

void AssociationTableDomain::reserve(uint16_t capacity) {
    (void)capacity;
}

uint16_t AssociationTableDomain::size() const {
    return _size;
}

bool AssociationTableDomain::isValid() const {
    for (uint16_t position = 0; position < _size; ++position) {
        if (!at(position).isValid()) {
            return false;
        }
    }
    return true;
}

// === AssociationTableObject ===

AssociationTableObject::AssociationTableObject() {
    constexpr uint16_t kDefaultReserve = 64;
    _table.reserve(kDefaultReserve);
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

std::optional<AssociationEntry> AssociationTableObject::getEntry(uint16_t index) const {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError() || domainIndex.value().value >= _table.size()) {
        return std::nullopt;
    }
    return _table.get(domainIndex.value());
}

util::Result<void> AssociationTableObject::loadTable(std::span<const AssociationEntry> entries) {
    return _table.load(entries);
}

void AssociationTableObject::reserve(uint16_t capacity) {
    _table.reserve(capacity);
}

util::Result<size_t> AssociationTableObject::getAllEntries(std::span<AssociationEntry> out) const {
    const size_t count = _table.size();
    if (out.empty()) return count;
    if (out.size() < count) {
        return util::ErrorCode::BufferTooSmall;
    }
    for (uint16_t position = 0; position < count; ++position) {
        out[position] = _table.get(AssociationTableDomain::Index{position});
    }
    return count;
}

bool AssociationTableObject::isValid() const {
    return _table.isValid();
}

} // namespace objects
} // namespace knx
