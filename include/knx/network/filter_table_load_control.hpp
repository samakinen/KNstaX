// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file filter_table_load_control.hpp
 * @brief Memory-mapped (Realisation Type 3) loading of a coupler's Filter
 *        Table through the Router Object (03_05_01 §5.3.1).
 *
 * §5.3.1.5: the Management Client allocates the Filter Table memory with an
 * Additional Load Control of the Router Object's Load State Machine, reads
 * the address from PID_TABLE_REFERENCE and writes the bit field with
 * A_Memory_Write. §5.3.1.4: the Load State Machine is Realisation Type 1
 * (§4.23.2), and the Filter Table evaluation is negative in every state but
 * Loaded. PID_MCB_TABLE (§4.2.27) carries the segment size and the CRC the
 * device computes on Loading -> Loaded.
 *
 * The memory window is the FilterTable's own storage: nothing is staged, so
 * the 8 KB the coupler already owns is the only cost. While the state is not
 * Loaded the coupler does not evaluate the table, so writes during Loading
 * never route on a half-written table.
 *
 * Pure logic, no stack dependencies: DeviceManagement binds it to the Router
 * Object's properties and registers memory(); tests drive it directly.
 */

#pragma once

#include "knx/network/filter_table.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace knx {
namespace network {

class FilterTableLoadControl {
public:
    /// Load states (03_05_01 §4.23.2.3.1, Table 92).
    enum class State : uint8_t { Unloaded = 0, Loaded = 1, Loading = 2, Error = 3 };

    /// Load events (03_05_01 §4.23.2.3.2, Table 93).
    static constexpr uint8_t kEventNoOperation = 0x00;
    static constexpr uint8_t kEventStartLoading = 0x01;
    static constexpr uint8_t kEventLoadCompleted = 0x02;
    static constexpr uint8_t kEventAdditionalLoadControls = 0x03;
    static constexpr uint8_t kEventUnload = 0x04;

    /// Additional Load Control subtypes this table accepts (03_05_02
    /// §3.31.3.4): absolute data allocation, relative allocation and data
    /// relative allocation.
    static constexpr uint8_t kAllocAbsDataSeg = 0x00;
    static constexpr uint8_t kRelativeAllocation = 0x0A;
    static constexpr uint8_t kDataRelativeAllocation = 0x0B;

    /// Where the table appears in the memory map. Below 10000h, so the MaC
    /// writes it with DMP_MemWrite_RCoV (§5.3.1.5), and clear of the
    /// fixed table segments (objects/table_segments.hpp).
    static constexpr uint16_t kDefaultMemoryAddress = 0x8000;

    /// One PID_MCB_TABLE element (§4.2.27, Table 12): segment size (4 octets),
    /// CRC Control Byte, read/write access nibbles, CRC (2 octets).
    static constexpr size_t kMcbSize = 8;
    using Mcb = std::array<uint8_t, kMcbSize>;

    /// PID_ERROR_CODE values (03_05_01 §4.2.28): DPT_ErrorClass_System
    /// (20.011, 03_07_02), the subset this load state machine reports.
    static constexpr uint8_t kErrorNone = 0;
    static constexpr uint8_t kErrorGeneralDevice = 1;
    static constexpr uint8_t kErrorConfiguration = 3;
    static constexpr uint8_t kErrorAllocationSizeZero = 8;
    static constexpr uint8_t kErrorCrc = 9;
    static constexpr uint8_t kErrorMaxTableLength = 13;
    static constexpr uint8_t kErrorUndefinedLoadCommand = 14;

    /// Called after every state change with whether the table is now Loaded.
    using StateListener = util::InplaceFunction<void(bool loaded), 32>;

    /// Persisted record: header, then the table run-length encoded or raw,
    /// whichever is shorter (see serialize()). Worst case is the raw table.
    static constexpr size_t kRecordHeaderSize = 15;
    static constexpr size_t kMaxRecordSize = kRecordHeaderSize + FilterTable::kOctets;

    explicit FilterTableLoadControl(FilterTable& table, uint16_t memoryAddress = kDefaultMemoryAddress);

    State state() const noexcept { return _state; }
    bool loaded() const noexcept { return _state == State::Loaded; }

    /// A write to PID_LOAD_STATE_CONTROL: the event in octet 0, additional
    /// load-control data after it (Table 93, Table 94).
    void applyEvent(std::span<const uint8_t> control);

    /// Table 94 "Device Restart": an interrupted Loading becomes Unloaded.
    void deviceRestart();

    /// PID_ERROR_CODE (§4.2.28): why the state is Error; 0 in any other state.
    uint8_t errorCode() const noexcept { return _errorCode; }

    /// Master Reset with a factory-reset Erase Code: the new-device state of
    /// §4.5.3 ("Loaded and the Routing Table cleared"), MCB and error cleared.
    void factoryReset();

    /// The load state, allocation, error code, MCB and table as one record
    /// for the persisted state. Format (all multi-octet fields big-endian):
    ///   0     version (1)
    ///   1     load state (Table 92)
    ///   2     flags: bit 0 = memory allocated
    ///   3     PID_ERROR_CODE
    ///   4-11  PID_MCB_TABLE
    ///   12-13 CRC16-CCITT of the 8192-octet table
    ///   14    encoding: 0 = raw 8192 octets, 1 = runs of {count: 2, value: 1}
    ///   15-   the table
    /// Runs are used when shorter than the raw table, so the record is never
    /// larger than kMaxRecordSize.
    void serialize(std::vector<uint8_t>& out) const;

    /// Take the state from serialize()'s record and apply the Device Restart
    /// transition of Table 94 to it. An empty record (nothing ever stored)
    /// leaves the §4.5.3 default. A record that does not decode, or whose
    /// table does not match its CRC, is the "error detection at start-up"
    /// of Table 94: the table is cleared and the state is Error.
    void restore(std::span<const uint8_t> record);

    /// A record is stored but storage could not read it: start-up error
    /// detection as for a corrupt record, with error code 1.
    void restoreUnreadable();

    /// PID_TABLE_REFERENCE (§4.2.7): the table's address while memory is
    /// allocated, zero once Unloaded or when allocation failed.
    uint32_t tableReference() const noexcept { return _allocated ? _memoryAddress : 0u; }

    /// PID_MCB_TABLE read value.
    Mcb mcb() const noexcept { return _mcb; }
    /// PID_MCB_TABLE write: only has effect while Loading (§4.2.27).
    util::Result<void> writeMcb(std::span<const uint8_t> value);

    /// The memory window the MaC writes (the FilterTable's octets).
    uint16_t memoryAddress() const noexcept { return _memoryAddress; }
    std::span<uint8_t> memory() noexcept { return _table.octets(); }

    void setStateListener(StateListener listener) { _listener = std::move(listener); }

    /// CRC16-CCITT as §4.2.27.1.2 specifies it: polynomial 1021h, initial
    /// value FFFFh (augmented form), no reflection, no final XOR
    /// ("123456789" -> E5CCh, the specification's check value).
    static uint16_t crc16Ccitt(std::span<const uint8_t> data) noexcept;

private:
    void setState(State state);
    void fail(uint8_t errorCode);
    void rejectStoredState(uint8_t errorCode);
    void additionalLoadControl(std::span<const uint8_t> control);
    void complete();
    void allocate(uint32_t size, bool fill, uint8_t fillValue);

    FilterTable& _table;
    uint16_t _memoryAddress;
    // "The default Load State shall be Loaded and the Routing Table cleared"
    // (§4.5.3); the table's memory counts as allocated in that state.
    State _state{State::Loaded};
    bool _allocated{true};
    bool _mcbWritten{false};
    uint8_t _errorCode{kErrorNone};
    Mcb _mcb{};
    StateListener _listener{};
};

} // namespace network
} // namespace knx
