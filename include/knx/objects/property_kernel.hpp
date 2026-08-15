// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_kernel.hpp
 * @brief KNX Property Kernel (shared property handlers)
 */

#pragma once

#include "knx/application/property.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/types.hpp"
#include "knx/util/byte_stream.hpp"
#include "knx/util/result.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace knx {
namespace objects {

enum class ValidationPolicy : uint8_t {
    OnWrite,
    OnCommit,
    Manual
};

struct KnxIndex {
    uint16_t value{0};
};

struct DomainIndex {
    uint16_t value{0};
};

enum class PropertyCapability : uint8_t {
    ReadOnly,
    WriteOnly,
    ReadWrite
};

struct PropertyContext {
    void* domain{nullptr};
    ValidationPolicy policy{ValidationPolicy::OnWrite};
};

struct PropertyHandler {
    application::PropertyID id{};
    application::PropertyDataType type{};
    PropertyCapability capability{PropertyCapability::ReadOnly};
    uint16_t maxElements{0};
    uint8_t elementSizeOverride{0};

    util::Result<void> (*read)(const PropertyContext&, DomainIndex startIndex, uint16_t elementCount, util::ByteWriter& out, const void* userData) = nullptr;
    util::Result<void> (*write)(const PropertyContext&, DomainIndex startIndex, uint16_t elementCount, util::ByteReader& in, const void* userData) = nullptr;

    const void* userData{nullptr};

    // Array element 0 — "the current number of elements" (03/03/07 §3.4.4.1),
    // which is a different quantity from maxElements: the descriptor reports
    // how large the array may get, element 0 how much of it is in use.
    //
    // Declared last so the positional aggregate initialisers that build handler
    // tables keep binding userData. A property that leaves `count` null is
    // fixed-size and answers maxElements; one that leaves `resize` null does not
    // let a client change its length, which is the honest answer for anything
    // the firmware derives itself.
    util::Result<uint16_t> (*count)(const PropertyContext&, const void* userData) = nullptr;
    util::Result<void> (*resize)(const PropertyContext&, uint16_t elementCount, const void* userData) = nullptr;
};

struct KernelBinding {
    const PropertyHandler* handlers{nullptr};
    size_t handlerCount{0};
    PropertyContext context{};
};

inline bool canRead(PropertyCapability cap) {
    return cap == PropertyCapability::ReadOnly || cap == PropertyCapability::ReadWrite;
}

inline bool canWrite(PropertyCapability cap) {
    return cap == PropertyCapability::WriteOnly || cap == PropertyCapability::ReadWrite;
}

inline uint8_t propertyElementSize(application::PropertyDataType type) {
    using PDT = application::PropertyDataType;
    switch (type) {
        case PDT::Control:
        case PDT::Char:
        case PDT::UnsignedChar:
        case PDT::BinaryInfo:
        case PDT::Bitset8:
        case PDT::Enum8:
        case PDT::Scaling:
        case PDT::GenericData:
            return 1;

        case PDT::Int:
        case PDT::UnsignedInt:
        case PDT::KnxFloat:
        case PDT::Version:
        case PDT::Bitset16:
        case PDT::Generic02:
            return 2;

        case PDT::Date:
        case PDT::Time:
        case PDT::PollGroupSettings:
        case PDT::Generic03:
            return 3;

        case PDT::Long:
        case PDT::UnsignedLong:
        case PDT::Float:
        case PDT::Generic04:
            return 4;

        case PDT::ShortCharBlock:
        case PDT::Generic05:
            return 5;

        case PDT::AlarmInfo:
        case PDT::Generic06:
            return 6;

        case PDT::Double:
        case PDT::DateTime:
        case PDT::Generic08:
            return 8;

        case PDT::CharBlock:
        case PDT::Generic10:
            return 10;

        case PDT::Generic07:
            return 7;
        case PDT::Generic09:
            return 9;
        case PDT::Generic11:
            return 11;
        case PDT::Generic12:
            return 12;
        case PDT::Generic13:
            return 13;
        case PDT::Generic14:
            return 14;
        case PDT::Generic15:
            return 15;
        case PDT::Generic16:
            return 16;
        case PDT::Generic17:
            return 17;
        case PDT::Generic18:
            return 18;
        case PDT::Generic19:
            return 19;
        case PDT::Generic20:
            return 20;

        case PDT::Utf8:
        case PDT::VariableLength:
        case PDT::Escape:
        default:
            return 0;
    }
}

inline util::Result<void> validatePropertyTable(const PropertyHandler* handlers, size_t count) {
    if (!handlers || count == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    for (size_t i = 0; i < count; ++i) {
        const auto& handler = handlers[i];
        if (handler.maxElements == 0) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (handler.id == static_cast<application::PropertyID>(0)) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        const uint8_t elementSize = propertyElementSize(handler.type);
        if (elementSize == 0 && handler.elementSizeOverride == 0) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (canRead(handler.capability) && handler.read == nullptr) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (!canRead(handler.capability) && handler.read != nullptr) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (canWrite(handler.capability) && handler.write == nullptr) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (!canWrite(handler.capability) && handler.write != nullptr) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        for (size_t j = i + 1; j < count; ++j) {
            if (handlers[j].id == handler.id) {
                return util::Result<void>::err(util::ErrorCode::InvalidParameter);
            }
        }
    }

    return util::Result<void>::ok();
}

inline const PropertyHandler* findHandler(const PropertyHandler* handlers, size_t count, application::PropertyID propertyId) {
    if (!handlers) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        if (handlers[i].id == propertyId) {
            return &handlers[i];
        }
    }
    return nullptr;
}

template <size_t Capacity>
inline util::Result<void> readProperty(
    const PropertyHandler* handlers,
    size_t count,
    const PropertyContext& context,
    application::PropertyID propertyId,
    uint16_t startIndex,
    uint16_t elementCount,
    util::FixedVector<uint8_t, Capacity>& out)
{
    if (startIndex == 0 || elementCount == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto* handler = findHandler(handlers, count, propertyId);
    if (!handler) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
    if (!canRead(handler->capability)) {
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    const uint32_t lastIndex = static_cast<uint32_t>(startIndex) + elementCount - 1;
    if (lastIndex > handler->maxElements) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    uint8_t elementSize = propertyElementSize(handler->type);
    if (elementSize == 0) {
        elementSize = handler->elementSizeOverride;
    }
    if (elementSize == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const size_t byteCount = static_cast<size_t>(elementCount) * elementSize;
    if (byteCount > Capacity) {
        return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
    }

    out.resize(byteCount);

    util::ByteWriter writer(out.span());
    const DomainIndex domainStart{static_cast<uint16_t>(startIndex - 1)};
    auto res = handler->read(context, domainStart, elementCount, writer, handler->userData);
    if (res.isError()) {
        return res;
    }
    if (writer.written() != byteCount) {
        return util::Result<void>::err(util::ErrorCode::EncodeFailed);
    }
    return util::Result<void>::ok();
}

inline util::Result<void> readProperty(
    const PropertyHandler* handlers,
    size_t count,
    const PropertyContext& context,
    application::PropertyID propertyId,
    uint16_t startIndex,
    uint16_t elementCount,
    std::vector<uint8_t>& out)
{
    if (startIndex == 0 || elementCount == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto* handler = findHandler(handlers, count, propertyId);
    if (!handler) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
    if (!canRead(handler->capability)) {
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    const uint32_t lastIndex = static_cast<uint32_t>(startIndex) + elementCount - 1;
    if (lastIndex > handler->maxElements) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    uint8_t elementSize = propertyElementSize(handler->type);
    if (elementSize == 0) {
        elementSize = handler->elementSizeOverride;
    }
    if (elementSize == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const size_t byteCount = static_cast<size_t>(elementCount) * elementSize;
    out.assign(byteCount, 0);

    util::ByteWriter writer(out);
    const DomainIndex domainStart{static_cast<uint16_t>(startIndex - 1)};
    auto res = handler->read(context, domainStart, elementCount, writer, handler->userData);
    if (res.isError()) {
        return res;
    }
    if (writer.written() != byteCount) {
        return util::Result<void>::err(util::ErrorCode::EncodeFailed);
    }
    return util::Result<void>::ok();
}

/// PDT_CONTROL write blocks carry the command octet plus additional control
/// data (KNX 03.03.07: reads return one status octet, writes carry up to 10).
inline constexpr size_t kMaxControlBlockOctets = 10;

inline util::Result<void> writeProperty(
    const PropertyHandler* handlers,
    size_t count,
    const PropertyContext& context,
    application::PropertyID propertyId,
    uint16_t startIndex,
    std::span<const uint8_t> value)
{
    if (startIndex == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto* handler = findHandler(handlers, count, propertyId);
    if (!handler) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
    if (!canWrite(handler->capability)) {
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    // PDT_CONTROL is asymmetric: a read returns a single status octet, while a
    // write carries a control block of 1..10 octets that always addresses ONE
    // logical element. Hand the whole block to the handler as octet stream so
    // Control properties can declare maxElements = 1 truthfully.
    if (handler->type == application::PropertyDataType::Control) {
        if (startIndex != 1u || value.empty() || value.size() > kMaxControlBlockOctets) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        util::ByteReader reader(value);
        return handler->write(context, DomainIndex{0}, static_cast<uint16_t>(value.size()),
                              reader, handler->userData);
    }

    uint8_t elementSize = propertyElementSize(handler->type);
    if (elementSize == 0) {
        elementSize = handler->elementSizeOverride;
    }
    if (elementSize == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (value.empty() || (value.size() % elementSize) != 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const uint16_t elementCount = static_cast<uint16_t>(value.size() / elementSize);
    const uint32_t lastIndex = static_cast<uint32_t>(startIndex) + elementCount - 1;
    if (lastIndex > handler->maxElements) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }

    util::ByteReader reader(value);
    const DomainIndex domainStart{static_cast<uint16_t>(startIndex - 1)};
    return handler->write(context, domainStart, elementCount, reader, handler->userData);
}

/// Current number of elements of @p propertyId (array element 0).
///
/// 03/03/07 §3.4.4.1: a read with start_index = 0 answers "the current number
/// of elements of the Property array" — for a table property that is its
/// current length, not its capacity (03/05/01: "The maximum number of elements
/// in the Property shall show the maximum size of the table and current length
/// shall show the current usage").
inline util::Result<uint16_t> propertyElementCount(
    const PropertyHandler* handlers,
    size_t count,
    const PropertyContext& context,
    application::PropertyID propertyId)
{
    const auto* handler = findHandler(handlers, count, propertyId);
    if (!handler) {
        return util::Result<uint16_t>(util::ErrorCode::OperationNotSupported);
    }
    if (!canRead(handler->capability)) {
        return util::Result<uint16_t>(util::ErrorCode::AccessDenied);
    }
    if (handler->count == nullptr) {
        return util::Result<uint16_t>(handler->maxElements);
    }
    return handler->count(context, handler->userData);
}

/// Set the current number of elements (a write to array element 0).
///
/// ETS uses this to clear a table it is about to re-download; 03/05/01 gives it
/// meaning per property ("If the current length is set to 0 …"). Properties
/// whose length the firmware owns refuse it.
inline util::Result<void> resizeProperty(
    const PropertyHandler* handlers,
    size_t count,
    const PropertyContext& context,
    application::PropertyID propertyId,
    uint16_t elementCount)
{
    const auto* handler = findHandler(handlers, count, propertyId);
    if (!handler) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
    if (!canWrite(handler->capability)) {
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }
    if (handler->resize == nullptr) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
    if (elementCount > handler->maxElements) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    return handler->resize(context, elementCount, handler->userData);
}

inline util::Result<void> describeProperty(
    const PropertyHandler* handlers,
    size_t count,
    application::PropertyID propertyId,
    PropertyIndex propertyIndex,
    application::PropertyID& resolvedPropertyId,
    application::PropertyDataType& type,
    uint16_t& maxElements,
    PropertyCapability& capability)
{
    if (!handlers || count == 0) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }

    const PropertyHandler* handler = nullptr;
    if (propertyIndex.value() == 0) {
        handler = findHandler(handlers, count, propertyId);
    } else if (propertyIndex.value() <= count) {
        handler = &handlers[propertyIndex.value() - 1];
    }

    if (!handler) {
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }

    resolvedPropertyId = handler->id;
    type = handler->type;
    maxElements = handler->maxElements;
    capability = handler->capability;
    return util::Result<void>::ok();
}

// === Scalar property handler ===

template <typename T>
struct ScalarCodec;

template <>
struct ScalarCodec<uint8_t> {
    static util::Result<void> write(util::ByteWriter& out, uint8_t value) {
        return out.u8(value);
    }
    static util::Result<uint8_t> read(util::ByteReader& in) {
        return in.u8();
    }
};

template <>
struct ScalarCodec<uint16_t> {
    static util::Result<void> write(util::ByteWriter& out, uint16_t value) {
        return out.u16be(value);
    }
    static util::Result<uint16_t> read(util::ByteReader& in) {
        return in.u16be();
    }
};

template <>
struct ScalarCodec<uint32_t> {
    static util::Result<void> write(util::ByteWriter& out, uint32_t value) {
        return out.u32be(value);
    }
    static util::Result<uint32_t> read(util::ByteReader& in) {
        return in.u32be();
    }
};

// Specialization for GroupAddress (2 bytes, big-endian)

template <>
struct ScalarCodec<GroupAddress> {
    static util::Result<void> write(util::ByteWriter& out, const GroupAddress& value) {
        return out.u16be(value.raw);
    }
    static util::Result<GroupAddress> read(util::ByteReader& in) {
        auto res = in.u16be();
        if (res.isError()) {
            return res.error();
        }
        return GroupAddress(res.value());
    }
};

template <typename Domain, typename T>
struct ScalarPropertyData {
    util::Result<T> (*get)(const Domain& domain) = nullptr;
    util::Result<void> (*set)(Domain& domain, const T& value) = nullptr;
};

template <typename Domain, typename T>
struct ScalarProperty {
    static PropertyHandler make(
        application::PropertyID id,
        application::PropertyDataType type,
        PropertyCapability capability,
        const ScalarPropertyData<Domain, T>* data)
    {
        PropertyHandler handler;
        handler.id = id;
        handler.type = type;
        handler.capability = capability;
        handler.maxElements = 1;
        handler.read = canRead(capability) ? &read : nullptr;
        handler.write = canWrite(capability) ? &write : nullptr;
        handler.userData = data;
        return handler;
    }

    static util::Result<void> read(
        const PropertyContext& context,
        DomainIndex /*startIndex*/,
        uint16_t elementCount,
        util::ByteWriter& out,
        const void* userData)
    {
        if (elementCount != 1) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        const auto* data = static_cast<const ScalarPropertyData<Domain, T>*>(userData);
        if (!data || !data->get) {
            return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
        }
        const auto* domain = static_cast<const Domain*>(context.domain);
        if (!domain) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        auto value = data->get(*domain);
        if (value.isError()) {
            return util::Result<void>::err(value.error());
        }
        return ScalarCodec<T>::write(out, value.value());
    }

    static util::Result<void> write(
        const PropertyContext& context,
        DomainIndex /*startIndex*/,
        uint16_t elementCount,
        util::ByteReader& in,
        const void* userData)
    {
        if (elementCount != 1) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        auto* domain = static_cast<Domain*>(context.domain);
        if (!domain) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        const auto* data = static_cast<const ScalarPropertyData<Domain, T>*>(userData);
        if (!data || !data->set) {
            return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
        }
        auto value = ScalarCodec<T>::read(in);
        if (value.isError()) {
            return util::Result<void>::err(value.error());
        }
        return data->set(*domain, value.value());
    }
};

// === Variable table handler ===

template <typename Domain, typename T>
struct VariableTablePropertyData {
    util::Result<T> (*get)(const Domain& domain, DomainIndex index) = nullptr;
    util::Result<void> (*set)(Domain& domain, DomainIndex index, const T& value) = nullptr;
    util::Result<void> (*validate)(const Domain& domain, DomainIndex index, const T& value) = nullptr;
    /// Entries currently in use, for the element-0 read. Optional: without it
    /// the property reports its capacity, which is only truthful for a table
    /// that is always full.
    util::Result<uint16_t> (*count)(const Domain& domain) = nullptr;
};

template <typename Domain, typename T, uint16_t Max>
struct VariableTableProperty {
    static PropertyHandler make(
        application::PropertyID id,
        application::PropertyDataType type,
        PropertyCapability capability,
        const VariableTablePropertyData<Domain, T>* data)
    {
        PropertyHandler handler;
        handler.id = id;
        handler.type = type;
        handler.capability = capability;
        handler.maxElements = Max;
        handler.read = canRead(capability) ? &read : nullptr;
        handler.write = canWrite(capability) ? &write : nullptr;
        handler.userData = data;
        handler.count = (data && data->count) ? &countElements : nullptr;
        return handler;
    }

    static util::Result<uint16_t> countElements(const PropertyContext& context, const void* userData)
    {
        const auto* data = static_cast<const VariableTablePropertyData<Domain, T>*>(userData);
        const auto* domain = static_cast<const Domain*>(context.domain);
        if (!data || !data->count || !domain) {
            return util::Result<uint16_t>(util::ErrorCode::InvalidParameter);
        }
        return data->count(*domain);
    }

    static util::Result<void> read(
        const PropertyContext& context,
        DomainIndex startIndex,
        uint16_t elementCount,
        util::ByteWriter& out,
        const void* userData)
    {
        const auto* data = static_cast<const VariableTablePropertyData<Domain, T>*>(userData);
        if (!data || !data->get) {
            return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
        }
        const auto* domain = static_cast<const Domain*>(context.domain);
        if (!domain) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        for (uint16_t i = 0; i < elementCount; ++i) {
            const DomainIndex idx{static_cast<uint16_t>(startIndex.value + i)};
            auto value = data->get(*domain, idx);
            if (value.isError()) {
                return util::Result<void>::err(value.error());
            }
            auto writeRes = ScalarCodec<T>::write(out, value.value());
            if (writeRes.isError()) {
                return writeRes;
            }
        }
        return util::Result<void>::ok();
    }

    static util::Result<void> write(
        const PropertyContext& context,
        DomainIndex startIndex,
        uint16_t elementCount,
        util::ByteReader& in,
        const void* userData)
    {
        auto* domain = static_cast<Domain*>(context.domain);
        if (!domain) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        const auto* data = static_cast<const VariableTablePropertyData<Domain, T>*>(userData);
        if (!data || !data->set) {
            return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
        }

        util::ByteReader validationReader = in;
        if (data->validate) {
            for (uint16_t i = 0; i < elementCount; ++i) {
                const DomainIndex idx{static_cast<uint16_t>(startIndex.value + i)};
                auto value = ScalarCodec<T>::read(validationReader);
                if (value.isError()) {
                    return util::Result<void>::err(value.error());
                }
                auto validation = data->validate(*domain, idx, value.value());
                if (validation.isError()) {
                    return validation;
                }
            }
        }

        for (uint16_t i = 0; i < elementCount; ++i) {
            const DomainIndex idx{static_cast<uint16_t>(startIndex.value + i)};
            auto value = ScalarCodec<T>::read(in);
            if (value.isError()) {
                return util::Result<void>::err(value.error());
            }
            auto setRes = data->set(*domain, idx, value.value());
            if (setRes.isError()) {
                return setRes;
            }
        }

        return util::Result<void>::ok();
    }
};

// === Load state control handler (PID_LOAD_STATE_CONTROL = 5) ===

namespace loadstate {
// Load states per KNX 03.05.02
constexpr uint8_t kUnloaded = 0;
constexpr uint8_t kLoaded = 1;
constexpr uint8_t kLoading = 2;
constexpr uint8_t kError = 3;
// Load events written by the management client (ETS)
constexpr uint8_t kEventNoOperation = 0;
constexpr uint8_t kEventStartLoading = 1;
constexpr uint8_t kEventLoadCompleted = 2;
constexpr uint8_t kEventAdditionalLoadControls = 3;
constexpr uint8_t kEventUnload = 4;
} // namespace loadstate

// ETS writes PID_LOAD_STATE_CONTROL as a PDT_CONTROL block of up to 10 octets
// (the load event followed by additional load-control data); reading returns
// the current load state as a single octet. The kernel's Control write path
// hands the whole block to write() with elementCount = octet count. Requires
// the domain to provide loadState(), setLoadState(uint8_t) and
// loadControlReset() (invoked on StartLoading and Unload to discard loadable
// content).
/// Apply one load-control event to @p domain's load state machine
/// (03/05/01 §4.23.2, Realisation Type 1).
///
/// Shared because ETS drives the same state machine through two services: a
/// PDT_CONTROL property write, and — for the Security Interface Object during a
/// Data Secure download — an A_FunctionPropertyExtCommand.
template <typename Domain>
inline void applyLoadControlEvent(Domain& domain, uint8_t event)
{
    switch (event) {
        case loadstate::kEventNoOperation:
            break;
        case loadstate::kEventStartLoading:
            domain.loadControlReset();
            domain.setLoadState(loadstate::kLoading);
            break;
        case loadstate::kEventLoadCompleted:
            domain.setLoadState(loadstate::kLoaded);
            break;
        case loadstate::kEventAdditionalLoadControls:
            // Segment allocation while loading; state is unchanged.
            break;
        case loadstate::kEventUnload:
            domain.loadControlReset();
            domain.setLoadState(loadstate::kUnloaded);
            break;
        default:
            domain.setLoadState(loadstate::kError);
            break;
    }
}

template <typename Domain>
struct LoadControlProperty {
    static PropertyHandler make(application::PropertyID id) {
        PropertyHandler handler;
        handler.id = id;
        handler.type = application::PropertyDataType::Control;
        handler.capability = PropertyCapability::ReadWrite;
        handler.maxElements = 1;
        handler.read = &read;
        handler.write = &write;
        handler.userData = nullptr;
        return handler;
    }

    static util::Result<void> read(
        const PropertyContext& context,
        DomainIndex /*startIndex*/,
        uint16_t elementCount,
        util::ByteWriter& out,
        const void* /*userData*/)
    {
        const auto* domain = static_cast<const Domain*>(context.domain);
        if (!domain || elementCount == 0) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        auto res = out.u8(domain->loadState());
        if (res.isError()) {
            return res;
        }
        for (uint16_t i = 1; i < elementCount; ++i) {
            auto pad = out.u8(0);
            if (pad.isError()) {
                return pad;
            }
        }
        return util::Result<void>::ok();
    }

    static util::Result<void> write(
        const PropertyContext& context,
        DomainIndex startIndex,
        uint16_t elementCount,
        util::ByteReader& in,
        const void* /*userData*/)
    {
        auto* domain = static_cast<Domain*>(context.domain);
        if (!domain || startIndex.value != 0 || elementCount == 0) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        auto event = in.u8();
        if (event.isError()) {
            return util::Result<void>::err(event.error());
        }
        // Additional load-control data (segment allocation, checksums, …) is
        // accepted and ignored: this device has no dynamically allocated
        // segments; tables live in fixed-capacity storage.
        for (uint16_t i = 1; i < elementCount; ++i) {
            auto skipped = in.u8();
            if (skipped.isError()) {
                return util::Result<void>::err(skipped.error());
            }
        }
        applyLoadControlEvent(*domain, event.value());
        return util::Result<void>::ok();
    }
};

/// A Property of Datatype PDT_FUNCTION (03/03/07 §3.4.8).
///
/// A function property carries no value: it is *invoked* through the Function
/// Property services (A_FunctionPropertyCommand / _State_Read and their
/// extended twins), which this stack routes to a handler in the BAU rather
/// than through the property kernel. It still has to be **declared** here,
/// because §3.4.8.3 requires the description services to answer
/// `type = PDT_FUNCTION, max_nr_of_elem = 1` — that description is how a
/// Management Client discovers the function exists at all. Without a handler
/// the object reports the PID as unknown and ETS concludes the device does not
/// support the function, however well the BAU implements it.
///
/// Value access is refused: an A_PropertyValue_Read/_Write of a function
/// property is a client error, answered with nr_of_elem = 0 (§3.4.4.1
/// "Error handling"), which is what InvalidParameter maps to here.
struct FunctionProperty {
    static PropertyHandler make(application::PropertyID id) {
        PropertyHandler handler;
        handler.id = id;
        handler.type = application::PropertyDataType::Function;
        handler.capability = PropertyCapability::ReadWrite;
        handler.maxElements = 1;
        // PDT_FUNCTION has no fixed element size of its own; the kernel needs a
        // non-zero one to accept the declaration.
        handler.elementSizeOverride = 1;
        handler.read = &refuseValueAccess;
        handler.write = &refuseValueWrite;
        handler.userData = nullptr;
        return handler;
    }

    static util::Result<void> refuseValueAccess(
        const PropertyContext&, DomainIndex, uint16_t, util::ByteWriter&, const void*)
    {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    static util::Result<void> refuseValueWrite(
        const PropertyContext&, DomainIndex, uint16_t, util::ByteReader&, const void*)
    {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
};

} // namespace objects
} // namespace knx
