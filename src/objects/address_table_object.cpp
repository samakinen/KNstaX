// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file address_table_object.cpp
 * @brief Address table object implementation (Property Kernel)
 */

#include "knx/objects/address_table_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include <algorithm>

namespace knx {
namespace objects {

namespace {
util::Result<uint16_t> getObjectType(const AddressTableDomain& /*domain*/) {
    return InterfaceObjectType::addressTable().value();
}

util::Result<GroupAddress> getTableEntry(const AddressTableDomain& domain, DomainIndex index) {
    return domain.get(index);
}

util::Result<void> setTableEntry(AddressTableDomain& domain, DomainIndex index, const GroupAddress& value) {
    return domain.setExpand(index, value);
}

util::Result<void> validateTableEntry(const AddressTableDomain& /*domain*/, DomainIndex /*index*/, const GroupAddress& value) {
    if (!value.isValid()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return util::Result<void>::ok();
}

static const ScalarPropertyData<AddressTableDomain, uint16_t> kObjectTypeData{
    &getObjectType,
    nullptr
};

util::Result<uint32_t> getTableReference(const AddressTableDomain& /*domain*/) {
    return tableseg::kGroupAddressTableBase;
}

static const ScalarPropertyData<AddressTableDomain, uint32_t> kTableReferenceData{
    &getTableReference,
    nullptr
};

/// 03/05/01 §4.9.2: the maximum number of elements shows the table's capacity
/// and the current length its usage, which is what a read of array element 0
/// answers.
util::Result<uint16_t> countTableEntries(const AddressTableDomain& domain) {
    return domain.size();
}

static const VariableTablePropertyData<AddressTableDomain, GroupAddress> kTableData{
    &getTableEntry,
    &setTableEntry,
    &validateTableEntry,
    &countTableEntries
};

static const PropertyHandler kAddressTableHandlers[] = {
    ScalarProperty<AddressTableDomain, uint16_t>::make(
        application::PropertyID::ObjectType,
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kObjectTypeData),

    LoadControlProperty<AddressTableDomain>::make(
        static_cast<application::PropertyID>(AddressTableProperty::LoadState)),

    // PID_TABLE_REFERENCE: address of the memory-mapped load segment; ETS
    // SystemB streams the table content there with A_Memory_Write.
    ScalarProperty<AddressTableDomain, uint32_t>::make(
        static_cast<application::PropertyID>(AddressTableProperty::TableReference),
        application::PropertyDataType::UnsignedLong,
        PropertyCapability::ReadOnly,
        &kTableReferenceData),

    // PDT_UNSIGNED_INT: ETS SystemB group address table format (2-byte GAs).
    VariableTableProperty<AddressTableDomain, GroupAddress, AddressTableDomain::kMaxEntries>::make(
        static_cast<application::PropertyID>(AddressTableProperty::TableData),
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadWrite,
        &kTableData)
};

constexpr size_t kAddressTableHandlerCount = sizeof(kAddressTableHandlers) / sizeof(kAddressTableHandlers[0]);

util::Result<AddressTableDomain::Index> toDomainIndex(AddressTableIndex index) {
    if (!index.isValid()) {
        return util::Result<AddressTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    const uint16_t raw = index.raw;
    if (raw == 0 || raw > AddressTableDomain::kMaxEntries) {
        return util::Result<AddressTableDomain::Index>(util::ErrorCode::InvalidParameter);
    }
    return AddressTableDomain::Index{static_cast<uint16_t>(raw - 1)};
}

} // namespace

// === AddressTableDomain ===

util::Result<AddressTableDomain::Index> AddressTableDomain::add(const GroupAddress& address) {
    if (_entries.size() >= kMaxEntries) {
        return util::Result<AddressTableDomain::Index>(util::ErrorCode::OutOfRange);
    }

    const Index existing = findIndex(address);
    if (isValidIndex(existing)) {
        return existing;
    }

    _entries.push_back(address);
    return Index{static_cast<uint16_t>(_entries.size() - 1)};
}

util::Result<void> AddressTableDomain::remove(Index index) {
    if (index.value >= _entries.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    _entries.erase(index.value);
    return util::Result<void>::ok();
}

util::Result<void> AddressTableDomain::set(Index index, const GroupAddress& address) {
    if (index.value >= _entries.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    _entries[index.value] = address;
    return util::Result<void>::ok();
}

util::Result<void> AddressTableDomain::setExpand(Index index, const GroupAddress& address) {
    if (index.value >= kMaxEntries) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (_entries.size() <= index.value) {
        _entries.resize(index.value + 1, GroupAddress(GroupAddress::invalidValue()));
    }
    _entries[index.value] = address;
    return util::Result<void>::ok();
}

GroupAddress AddressTableDomain::get(Index index) const {
    if (index.value >= _entries.size()) {
        return GroupAddress(GroupAddress::invalidValue());
    }
    return _entries[index.value];
}

AddressTableDomain::Index AddressTableDomain::findIndex(const GroupAddress& address) const {
    for (size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i].raw == address.raw) {
            return Index{static_cast<uint16_t>(i)};
        }
    }
    return invalidIndex();
}

util::Result<void> AddressTableDomain::load(std::span<const GroupAddress> addresses) {
    if (addresses.size() > kMaxEntries) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (!_entries.assign(addresses)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return util::Result<void>::ok();
}

void AddressTableDomain::truncate(uint16_t size) {
    if (size < _entries.size()) {
        _entries.resize(size);
    }
}

void AddressTableDomain::clear() {
    _entries.clear();
}

void AddressTableDomain::reserve(uint16_t capacity) {
    (void)capacity;
}

uint16_t AddressTableDomain::size() const {
    return static_cast<uint16_t>(_entries.size());
}

bool AddressTableDomain::isValid() const {
    if (_entries.empty()) {
        return true;
    }
    for (const auto& addr : _entries) {
        if (!addr.isValid()) {
            return false;
        }
    }
    return true;
}

// === AddressTableObject ===

AddressTableObject::AddressTableObject() {
    constexpr uint16_t kDefaultReserve = 32;
    _table.reserve(kDefaultReserve);

    const auto validation = validatePropertyTable(kAddressTableHandlers, kAddressTableHandlerCount);
    if (validation.isError()) {
        KNX_LOGE("KNX.AddrTable", "Invalid property handler table (err=%d)", static_cast<int>(validation.error()));
    }
}

KernelBinding AddressTableObject::kernelBinding() const {
    KernelBinding binding;
    binding.handlers = kAddressTableHandlers;
    binding.handlerCount = kAddressTableHandlerCount;
    binding.context = PropertyContext{const_cast<AddressTableDomain*>(&_table), _validationPolicy};
    return binding;
}

AddressTableIndex AddressTableObject::addEntry(const GroupAddress& address) {
    auto res = _table.add(address);
    if (res.isError()) {
        return AddressTableIndex::invalid();
    }
    return AddressTableIndex(static_cast<uint16_t>(res.value().value + 1));
}

util::Result<void> AddressTableObject::removeEntry(AddressTableIndex index) {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return util::Result<void>::err(domainIndex.error());
    }
    return _table.remove(domainIndex.value());
}

void AddressTableObject::clearEntries() {
    _table.clear();
}

util::Result<void> AddressTableObject::setEntry(AddressTableIndex index, const GroupAddress& address) {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return util::Result<void>::err(domainIndex.error());
    }
    return _table.set(domainIndex.value(), address);
}

util::Result<void> AddressTableObject::setEntryExpand(AddressTableIndex index, const GroupAddress& address) {
    auto domainIndex = toDomainIndex(index);
    if (domainIndex.isError()) {
        return util::Result<void>::err(domainIndex.error());
    }
    return _table.setExpand(domainIndex.value(), address);
}

GroupAddress AddressTableObject::getAddress(AddressTableIndex index) const {
    if (!index.isValid()) {
        return GroupAddress(GroupAddress::invalidValue());
    }
    const uint16_t raw = index.raw;
    if (raw == 0 || raw > AddressTableDomain::kMaxEntries) {
        return GroupAddress(GroupAddress::invalidValue());
    }
    return _table.get(AddressTableDomain::Index{static_cast<uint16_t>(raw - 1)});
}

AddressTableIndex AddressTableObject::findIndex(const GroupAddress& address) const {
    const auto idx = _table.findIndex(address);
    if (!AddressTableDomain::isValidIndex(idx)) {
        return AddressTableIndex::invalid();
    }
    return AddressTableIndex(static_cast<uint16_t>(idx.value + 1));
}

util::Result<void> AddressTableObject::loadTable(std::span<const GroupAddress> addresses) {
    return _table.load(addresses);
}

void AddressTableObject::reserve(uint16_t capacity) {
    _table.reserve(capacity);
}

util::Result<size_t> AddressTableObject::getAllAddresses(std::span<GroupAddress> out) const {
    const size_t count = _table.entries().size();
    if (out.empty()) return count;
    if (out.size() < count) {
        return util::ErrorCode::BufferTooSmall;
    }
    size_t written = 0;
    for (const auto& addr : _table.entries()) {
        out[written++] = addr;
    }
    return written;
}

bool AddressTableObject::isValid() const {
    return _table.isValid();
}

} // namespace objects
} // namespace knx
