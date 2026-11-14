// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/network/filter_table_load_control.hpp"

#include "knx/util/log.hpp"

#include <algorithm>

namespace knx {
namespace network {

namespace {

const char* TAG = "KNX.FilterLoad";

uint32_t be32(std::span<const uint8_t> bytes)
{
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16)
         | (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

uint16_t be16(std::span<const uint8_t> bytes)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

// MCB element layout (03_05_01 §4.2.27, Table 12).
constexpr size_t kMcbSegmentSize = 0;
constexpr size_t kMcbCrcControl = 4;
constexpr size_t kMcbAccess = 5;
constexpr size_t kMcbCrc = 6;

} // namespace

FilterTableLoadControl::FilterTableLoadControl(FilterTable& table, uint16_t memoryAddress)
    : _table(table)
    , _memoryAddress(memoryAddress)
{
}

uint16_t FilterTableLoadControl::crc16Ccitt(std::span<const uint8_t> data) noexcept
{
    // §4.2.27.1.2 names initial value FFFFh and gives "123456789" -> E5CCh.
    // That pair is the augmented (non-direct) formulation: FFFFh in the
    // register, the message shifted through it, then 16 zero bits. The
    // table-free direct form below reaches the same result seeded with
    // 1D0Fh, the register value FFFFh becomes after those 16 zero bits.
    // (Seeding the direct form with FFFFh gives 29B1h, "CRC-16/CCITT-FALSE".)
    uint16_t crc = 0x1D0Fu;
    for (const uint8_t octet : data) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(octet) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) != 0u ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                        : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

void FilterTableLoadControl::setState(State state)
{
    if (state == _state) {
        return;
    }
    _state = state;
    KNX_LOGI(TAG, "Filter Table load state %u", static_cast<unsigned>(state));
    if (_listener) {
        _listener(loaded());
    }
}

void FilterTableLoadControl::applyEvent(std::span<const uint8_t> control)
{
    if (control.empty()) {
        return;
    }

    // Transitions per 03_05_01 §4.23.2.3.3, Table 94 (recommended column
    // where it offers a choice). Unknown events are ignored (§4.23.2.3.2).
    switch (control[0]) {
        case kEventNoOperation:
            break;

        case kEventStartLoading:
            if (_state == State::Error) {
                break;
            }
            if (_state != State::Loading) {
                _mcbWritten = false;
            }
            setState(State::Loading);
            break;

        case kEventLoadCompleted:
            // Unloaded, Loaded and Error stay as they are; only Loading
            // completes.
            if (_state == State::Loading) {
                complete();
            }
            break;

        case kEventAdditionalLoadControls:
            if (_state == State::Loading) {
                additionalLoadControl(control);
            } else if (_state == State::Loaded) {
                setState(State::Error);
            }
            break;

        case kEventUnload:
            // "The loadable data shall be declared as invalid. The data is
            // undefined" (Table 93): clear it, so a later load never starts
            // from an old table, and release the allocation (§4.2.7).
            _table.setAll(false);
            _allocated = false;
            _mcbWritten = false;
            _mcb = Mcb{};
            setState(State::Unloaded);
            break;

        default:
            break;
    }
}

void FilterTableLoadControl::additionalLoadControl(std::span<const uint8_t> control)
{
    if (control.size() < 2u) {
        return;
    }
    const uint8_t subtype = control[1];
    switch (subtype) {
        case kDataRelativeAllocation:
            // 03 0B <size: 4> <mode> <fill> 00 00 (03_05_02 §3.31.3.4).
            if (control.size() >= 8u) {
                allocate(be32(control.subspan(2, 4)), (control[6] & 0x01u) != 0u, control[7]);
            }
            break;
        case kRelativeAllocation:
            // 03 0A <size: 2> 00 ...
            if (control.size() >= 4u) {
                allocate(be16(control.subspan(2, 2)), false, 0u);
            }
            break;
        case kAllocAbsDataSeg:
            // 03 00 <start: 2> <length: 2> <access> <type> <attributes> 00.
            // The table has one fixed place; any other address cannot be
            // honoured.
            if (control.size() >= 6u) {
                if (be16(control.subspan(2, 2)) != _memoryAddress) {
                    _allocated = false;
                    setState(State::Error);
                    break;
                }
                allocate(be16(control.subspan(4, 2)), false, 0u);
            }
            break;
        default:
            // Not a control this table uses: ignored (§4.23.2.3.2).
            break;
    }
}

void FilterTableLoadControl::allocate(uint32_t size, bool fill, uint8_t fillValue)
{
    // "If the requested number of octets is not supported by the Management
    // Server (device) then the Load State Machine of the loadable part shall
    // change to error" (03_05_02 §3.31.3.4).
    if (size == 0u || size > FilterTable::kOctets) {
        KNX_LOGW(TAG, "Filter Table allocation of %u octets refused", static_cast<unsigned>(size));
        _allocated = false;
        setState(State::Error);
        return;
    }
    _allocated = true;
    if (fill) {
        auto octets = _table.octets();
        std::fill(octets.begin(), octets.begin() + static_cast<std::ptrdiff_t>(size), fillValue);
    }
}

util::Result<void> FilterTableLoadControl::writeMcb(std::span<const uint8_t> value)
{
    if (value.size() != kMcbSize) {
        return util::ErrorCode::InvalidParameter;
    }
    // "Writing the Memory Control Block Table shall only have effect in the
    // Load State 'Loading'" (§4.2.27).
    if (_state != State::Loading) {
        return util::ErrorCode::AccessDenied;
    }
    std::copy(value.begin(), value.end(), _mcb.begin());
    _mcbWritten = true;
    return util::Result<void>::ok();
}

void FilterTableLoadControl::complete()
{
    if (!_allocated) {
        KNX_LOGW(TAG, "LoadCompleted without allocated Filter Table memory");
        setState(State::Error);
        return;
    }

    const uint32_t segmentSize = _mcbWritten ? be32(std::span<const uint8_t>(_mcb).subspan(kMcbSegmentSize, 4))
                                             : static_cast<uint32_t>(FilterTable::kOctets);
    if (segmentSize == 0u || segmentSize > FilterTable::kOctets) {
        KNX_LOGW(TAG, "MCB segment size %u does not fit the Filter Table", static_cast<unsigned>(segmentSize));
        setState(State::Error);
        return;
    }

    // §4.2.27.1.2: the CRC is calculated by the device on the transition
    // from Loading to Loaded over the segment's memory content. When the MaC
    // has written the MCB with the CRC it expects, a different result is an
    // error in the data (Table 93, Load Completed).
    const auto octets = _table.octets();
    const uint16_t crc = crc16Ccitt(std::span<const uint8_t>(octets.data(), segmentSize));
    if (_mcbWritten) {
        const uint16_t expected = be16(std::span<const uint8_t>(_mcb).subspan(kMcbCrc, 2));
        if (expected != crc) {
            KNX_LOGW(TAG, "Filter Table CRC %04X, MCB says %04X", static_cast<unsigned>(crc),
                     static_cast<unsigned>(expected));
            setState(State::Error);
            return;
        }
    } else {
        _mcb[kMcbSegmentSize + 0] = static_cast<uint8_t>(segmentSize >> 24);
        _mcb[kMcbSegmentSize + 1] = static_cast<uint8_t>(segmentSize >> 16);
        _mcb[kMcbSegmentSize + 2] = static_cast<uint8_t>(segmentSize >> 8);
        _mcb[kMcbSegmentSize + 3] = static_cast<uint8_t>(segmentSize);
        _mcb[kMcbCrcControl] = 0x00; // the table does not change after download
        _mcb[kMcbAccess] = 0x00;
    }
    _mcb[kMcbCrc] = static_cast<uint8_t>(crc >> 8);
    _mcb[kMcbCrc + 1] = static_cast<uint8_t>(crc);
    _mcbWritten = false;
    setState(State::Loaded);
}

void FilterTableLoadControl::deviceRestart()
{
    if (_state == State::Loading) {
        _allocated = false;
        setState(State::Unloaded);
    }
}

} // namespace network
} // namespace knx
