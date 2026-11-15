// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_bindings.hpp
 * @brief Business-logic binding layer for the KNstaX endpoint product model.
 *
 * A binding connects a port to the product's own code: a command handler
 * (bus write to a command port), a state-write handler (bus write to a
 * writable state port) or a state provider (answers a bus read). Bindings are
 * type-erased into three-word entries, `{ thunk, manage, ctx }`:
 *
 *  - A callable that is trivially copyable and fits in a pointer (a captureless
 *    lambda, a function pointer, a lambda capturing one pointer, reference or
 *    integer) is stored in `ctx` itself. One thunk exists per callable type.
 *  - Anything larger or non-trivial goes to the heap, and `manage` frees it.
 *    That is the default (`BindingCapacity == 0`), which keeps a product with
 *    fat capture lists (a std::string, a shared_ptr, several references)
 *    working unchanged.
 *  - With `BindingCapacity > 0` the heap is off the table: the parameter is
 *    the maximum capture size in bytes, a callable that does not fit stops the
 *    build with a static_assert, and the bindings never allocate. The inline
 *    limit itself is sizeof(void*) (4 bytes on a Cortex-M), so a useful value
 *    is sizeof(void*) or any larger number as a plain "no heap" switch.
 *
 * Entries exist only for the ports that can use them, indexed by the ranks of
 * the port table (port_table.hpp): receive entries for ports that accept bus
 * writes, state providers for readable ports. Registering a binding a port
 * cannot use is a compile-time error.
 */

#pragma once

#include "knx/application/dpt.hpp"
#include "knx/product/fault_model.hpp"
#include "knx/product/impl/model/endpoint_definition.hpp"
#include "knx/product/impl/model/port_table.hpp"
#include "knx/util/inplace_function.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace knx::product {

using ProgModeCallback = util::InplaceFunction<void(bool active), 32>;

/// Default binding capacity: 0 allows captures of any size (those larger than a
/// pointer are heap-allocated); >0 forbids the heap and is the maximum capture
/// size in bytes (see the file comment).
static constexpr size_t kDefaultBindingCapacity = 0;

namespace detail {

/// Handles a bus write; false when the value is not of the port's type.
using ReceiveThunk = bool (*)(void* self, const application::DptValue& value);
/// Produces the port's current state; false when it cannot.
using ProvideThunk = bool (*)(void* self, application::DptValue& out);
/// Destroys the callable behind `ctx` (heap entries only).
using ManageThunk = void (*)(void* self);

/// One type-erased binding. `self` for every thunk is `&ctx`: an inline
/// callable lives in `ctx` itself (so a mutable lambda keeps its state), a
/// heap callable is the pointer stored there.
template <typename ThunkT>
struct BindingEntry {
    ThunkT invoke{nullptr};
    ManageThunk manage{nullptr};
    void* ctx{nullptr};

    explicit operator bool() const { return invoke != nullptr; }
    void* self() { return &ctx; }
    void* self() const { return const_cast<void**>(&ctx); }
};

using ReceiveBinding = BindingEntry<ReceiveThunk>;
using StateBinding = BindingEntry<ProvideThunk>;

template <typename EntryT>
void releaseBinding(EntryT& entry)
{
    if (entry.manage != nullptr) {
        entry.manage(entry.self());
    }
    entry = EntryT{};
}

template <typename EntryT, size_t Count>
void releaseBindings(std::array<EntryT, Count>& entries)
{
    for (auto& entry : entries) {
        releaseBinding(entry);
    }
}

/// A callable lives inside the entry when it is trivially copyable (so the
/// entry can be moved as bits) and fits in a pointer.
template <typename F>
inline constexpr bool kFitsInlineBinding =
    std::is_trivially_copyable_v<F> && sizeof(F) <= sizeof(void*) && alignof(F) <= alignof(void*);

template <typename F>
F& inlineCallable(void* self)
{
    return *std::launder(static_cast<F*>(self));
}

template <typename F>
F& heapCallable(void* self)
{
    return **static_cast<F**>(self);
}

template <typename F>
void destroyHeapCallable(void* self)
{
    delete *static_cast<F**>(self);
}

template <typename F, typename ValueT, bool Inline>
bool receiveThunk(void* self, const application::DptValue& value)
{
    const ValueT* typed = value.template tryGet<ValueT>();
    if (typed == nullptr) {
        return false;
    }
    if constexpr (Inline) {
        std::invoke(inlineCallable<F>(self), *typed);
    } else {
        std::invoke(heapCallable<F>(self), *typed);
    }
    return true;
}

template <typename F, typename ValueT, bool Inline>
bool provideThunk(void* self, application::DptValue& out)
{
    if constexpr (Inline) {
        out = application::DptValue(static_cast<ValueT>(std::invoke(inlineCallable<F>(self))));
    } else {
        out = application::DptValue(static_cast<ValueT>(std::invoke(heapCallable<F>(self))));
    }
    return true;
}

} // namespace detail

template <typename DefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class EndpointBindings {
public:
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_id_type = typename definition_type::port_id_type;

    static constexpr size_t kReceiveEntryCount = kReceivePortCount<definition_type>;
    static constexpr size_t kStateEntryCount = kStatePortCount<definition_type>;

    EndpointBindings() = default;

    // Move-only: an entry may own a heap callable.
    EndpointBindings(const EndpointBindings&) = delete;
    EndpointBindings& operator=(const EndpointBindings&) = delete;

    EndpointBindings(EndpointBindings&& other) noexcept
        : _commands(other._commands)
        , _stateWrites(other._stateWrites)
        , _providers(other._providers)
        , _progModeCallback(std::move(other._progModeCallback))
        , _faultCallback(std::move(other._faultCallback))
    {
        other.disown();
    }

    EndpointBindings& operator=(EndpointBindings&& other) noexcept
    {
        if (this != &other) {
            releaseAll();
            _commands = other._commands;
            _stateWrites = other._stateWrites;
            _providers = other._providers;
            _progModeCallback = std::move(other._progModeCallback);
            _faultCallback = std::move(other._faultCallback);
            other.disown();
        }
        return *this;
    }

    ~EndpointBindings() { releaseAll(); }

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&, port_value_t<definition_type, LogicalId>>
    EndpointBindings& onCommand(Fn&& fn) &
    {
        constexpr uint16_t rank = receiveRank<LogicalId>();
        static_assert(rank != kNoBindingRank,
                      "onCommand: this port cannot receive a write from the bus (it is neither writable nor receivable)");
        assignReceive<port_value_t<definition_type, LogicalId>>(_commands[rank], std::forward<Fn>(fn));
        return *this;
    }

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&> && std::convertible_to<std::invoke_result_t<Fn&>, port_value_t<definition_type, LogicalId>>
    EndpointBindings& provideState(Fn&& fn) &
    {
        constexpr uint16_t rank = stateRank<LogicalId>();
        static_assert(rank != kNoBindingRank,
                      "provideState: this port is not readable, so a state provider would never be asked");
        assignProvider<port_value_t<definition_type, LogicalId>>(_providers[rank], std::forward<Fn>(fn));
        return *this;
    }

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&, port_value_t<definition_type, LogicalId>>
    EndpointBindings& onStateWrite(Fn&& fn) &
    {
        constexpr uint16_t rank = receiveRank<LogicalId>();
        static_assert(rank != kNoBindingRank,
                      "onStateWrite: this port cannot receive a write from the bus (it is neither writable nor receivable)");
        assignReceive<port_value_t<definition_type, LogicalId>>(_stateWrites[rank], std::forward<Fn>(fn));
        return *this;
    }

    EndpointBindings& onProgrammingModeChanged(ProgModeCallback callback) &
    {
        _progModeCallback = std::move(callback);
        return *this;
    }

    EndpointBindings& onFault(FaultCallback callback) &
    {
        _faultCallback = std::move(callback);
        return *this;
    }

    // The same registrations on a temporary keep a chain such as
    // `auto bindings = makeEndpointBindings(def).onCommand<...>(...)...;` working:
    // the chain stays an rvalue and `bindings` is move-constructed.
    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&, port_value_t<definition_type, LogicalId>>
    EndpointBindings&& onCommand(Fn&& fn) &&
    {
        onCommand<LogicalId>(std::forward<Fn>(fn));
        return std::move(*this);
    }

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&> && std::convertible_to<std::invoke_result_t<Fn&>, port_value_t<definition_type, LogicalId>>
    EndpointBindings&& provideState(Fn&& fn) &&
    {
        provideState<LogicalId>(std::forward<Fn>(fn));
        return std::move(*this);
    }

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&, port_value_t<definition_type, LogicalId>>
    EndpointBindings&& onStateWrite(Fn&& fn) &&
    {
        onStateWrite<LogicalId>(std::forward<Fn>(fn));
        return std::move(*this);
    }

    EndpointBindings&& onProgrammingModeChanged(ProgModeCallback callback) &&
    {
        onProgrammingModeChanged(std::move(callback));
        return std::move(*this);
    }

    EndpointBindings&& onFault(FaultCallback callback) &&
    {
        onFault(std::move(callback));
        return std::move(*this);
    }

    template <auto LogicalId>
    bool hasCommandBinding() const
    {
        constexpr uint16_t rank = receiveRank<LogicalId>();
        if constexpr (rank == kNoBindingRank) {
            return false;
        } else {
            return static_cast<bool>(_commands[rank]);
        }
    }

    template <auto LogicalId>
    bool hasStateProvider() const
    {
        constexpr uint16_t rank = stateRank<LogicalId>();
        if constexpr (rank == kNoBindingRank) {
            return false;
        } else {
            return static_cast<bool>(_providers[rank]);
        }
    }

    template <auto LogicalId>
    bool hasStateWriteBinding() const
    {
        constexpr uint16_t rank = receiveRank<LogicalId>();
        if constexpr (rank == kNoBindingRank) {
            return false;
        } else {
            return static_cast<bool>(_stateWrites[rank]);
        }
    }

    template <auto LogicalId>
    bool dispatchCommand(port_value_t<definition_type, LogicalId> value)
    {
        constexpr uint16_t rank = receiveRank<LogicalId>();
        if constexpr (rank == kNoBindingRank) {
            return false;
        } else {
            return dispatchEntry(_commands[rank], value);
        }
    }

    template <auto LogicalId>
    bool dispatchStateWrite(port_value_t<definition_type, LogicalId> value)
    {
        constexpr uint16_t rank = receiveRank<LogicalId>();
        if constexpr (rank == kNoBindingRank) {
            return false;
        } else {
            return dispatchEntry(_stateWrites[rank], value);
        }
    }

    template <auto LogicalId>
    std::optional<port_value_t<definition_type, LogicalId>> readState() const
    {
        using value_type = port_value_t<definition_type, LogicalId>;
        constexpr uint16_t rank = stateRank<LogicalId>();
        if constexpr (rank == kNoBindingRank) {
            return std::nullopt;
        } else {
            const auto& entry = _providers[rank];
            application::DptValue produced;
            if (!entry || !entry.invoke(entry.self(), produced)) {
                return std::nullopt;
            }
            if (const auto* typed = produced.template tryGet<value_type>(); typed != nullptr) {
                return *typed;
            }
            return std::nullopt;
        }
    }

    void notifyProgrammingModeChanged(bool active) const
    {
        if (_progModeCallback) {
            _progModeCallback(active);
        }
    }

    void notifyFault(FaultInfo info) const
    {
        if (_faultCallback) {
            _faultCallback(info);
        }
    }

    // What the runtime core dispatches through: the entry arrays, indexed by
    // the ranks of the port table.
    detail::ReceiveBinding* commandEntries() { return _commands.data(); }
    detail::ReceiveBinding* stateWriteEntries() { return _stateWrites.data(); }
    detail::StateBinding* stateProviderEntries() { return _providers.data(); }

private:
    template <auto LogicalId>
    static consteval uint16_t receiveRank()
    {
        return kPortTable<definition_type>[port_index_v<definition_type, LogicalId>].receiveRank;
    }

    template <auto LogicalId>
    static consteval uint16_t stateRank()
    {
        return kPortTable<definition_type>[port_index_v<definition_type, LogicalId>].stateRank;
    }

    template <typename ValueT, typename Fn>
    static void assignReceive(detail::ReceiveBinding& entry, Fn&& fn)
    {
        using F = std::decay_t<Fn>;
        detail::releaseBinding(entry);
        if constexpr (detail::kFitsInlineBinding<F>) {
            static_assert(BindingCapacity == 0 || sizeof(F) <= BindingCapacity,
                          "binding capture is larger than BindingCapacity bytes");
            ::new (static_cast<void*>(&entry.ctx)) F(std::forward<Fn>(fn));
            entry.invoke = &detail::receiveThunk<F, ValueT, true>;
        } else {
            static_assert(BindingCapacity == 0,
                          "binding capture does not fit inline (a trivially copyable callable of at most "
                          "sizeof(void*) bytes) and BindingCapacity > 0 forbids the heap: capture a pointer "
                          "to your state, or use the default BindingCapacity of 0");
            entry.ctx = new F(std::forward<Fn>(fn));
            entry.invoke = &detail::receiveThunk<F, ValueT, false>;
            entry.manage = &detail::destroyHeapCallable<F>;
        }
    }

    template <typename ValueT, typename Fn>
    static void assignProvider(detail::StateBinding& entry, Fn&& fn)
    {
        using F = std::decay_t<Fn>;
        detail::releaseBinding(entry);
        if constexpr (detail::kFitsInlineBinding<F>) {
            static_assert(BindingCapacity == 0 || sizeof(F) <= BindingCapacity,
                          "binding capture is larger than BindingCapacity bytes");
            ::new (static_cast<void*>(&entry.ctx)) F(std::forward<Fn>(fn));
            entry.invoke = &detail::provideThunk<F, ValueT, true>;
        } else {
            static_assert(BindingCapacity == 0,
                          "binding capture does not fit inline (a trivially copyable callable of at most "
                          "sizeof(void*) bytes) and BindingCapacity > 0 forbids the heap: capture a pointer "
                          "to your state, or use the default BindingCapacity of 0");
            entry.ctx = new F(std::forward<Fn>(fn));
            entry.invoke = &detail::provideThunk<F, ValueT, false>;
            entry.manage = &detail::destroyHeapCallable<F>;
        }
    }

    template <typename ValueT>
    static bool dispatchEntry(detail::ReceiveBinding& entry, const ValueT& value)
    {
        return entry && entry.invoke(entry.self(), application::DptValue(value));
    }

    void releaseAll()
    {
        detail::releaseBindings(_commands);
        detail::releaseBindings(_stateWrites);
        detail::releaseBindings(_providers);
    }

    /// After a move: the entries were copied as bits, so the source must not free them.
    void disown()
    {
        _commands.fill({});
        _stateWrites.fill({});
        _providers.fill({});
    }

    std::array<detail::ReceiveBinding, kReceiveEntryCount> _commands{};
    std::array<detail::ReceiveBinding, kReceiveEntryCount> _stateWrites{};
    std::array<detail::StateBinding, kStateEntryCount> _providers{};
    ProgModeCallback _progModeCallback;
    FaultCallback _faultCallback;
};

} // namespace knx::product
