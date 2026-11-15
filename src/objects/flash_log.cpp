// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_log.cpp
 * @brief FlashLog: the log format, compaction and recovery (see flash_log.hpp)
 */

#include "knx/objects/flash_log.hpp"

#include "knx/util/log.hpp"

#include <algorithm>
#include <cstring>

static const char* TAG = "KNX.FlashLog";

namespace knx {
namespace objects {

namespace {

constexpr uint32_t BANK_MAGIC = 0x474F4C4Bu;
constexpr uint16_t BANK_FORMAT = 1u;
constexpr uint32_t BANK_HEADER_FIELDS = 12u;  // magic, sequence, format, CRC
constexpr uint32_t RECORD_HEADER = 8u;        // id, length, check, CRC
constexpr uint16_t TOMBSTONE_BIT = 0x8000u;
constexpr uint16_t LENGTH_MASK = 0x7FFFu;
constexpr uint16_t CHECK_SALT = 0x5AA5u;  // makes an erased (all 0xFF) header fail its check
constexpr uint8_t ERASED = 0xFFu;
constexpr uint16_t CRC_INITIAL = 0xFFFFu;
constexpr uint16_t CRC_POLYNOMIAL = 0x1021u;

// CRC-16/CCITT-FALSE, bitwise: a table would cost 512 B of flash for a few
// kilobytes hashed at mount and compaction.
uint16_t crc16(uint16_t crc, std::span<const uint8_t> bytes)
{
    for (const uint8_t octet : bytes) {
        crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(octet) << 8));
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) != 0u ? static_cast<uint16_t>((crc << 1) ^ CRC_POLYNOMIAL)
                                        : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t get32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void put16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t* p, uint32_t v)
{
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

uint16_t headerCheck(uint16_t id, uint16_t lengthField)
{
    return static_cast<uint16_t>(~(id ^ lengthField ^ CHECK_SALT));
}

}  // namespace

FlashLog::FlashLog(const knx_flash_hal_t& hal, FlashLogConfig config) : _hal(hal), _config(config) {}

// ── Geometry ────────────────────────────────────────────────────────────────

uint32_t FlashLog::align(uint32_t bytes) const
{
    const uint32_t unit = _hal.geometry.write_unit;
    return (bytes + unit - 1u) & ~(unit - 1u);
}

uint32_t FlashLog::bankAddress(uint8_t bank) const { return _hal.geometry.base + bank * _bankBytes; }

uint32_t FlashLog::headerBytes() const { return align(BANK_HEADER_FIELDS); }

uint32_t FlashLog::recordBytes(uint16_t length) const { return align(RECORD_HEADER + length); }

uint32_t FlashLog::freeBytes() const { return _writeOffset >= _bankBytes ? 0u : _bankBytes - _writeOffset; }

uint32_t FlashLog::reserveBytes() const
{
    return _config.reserveBytes != 0u ? _config.reserveBytes : (_bankBytes - headerBytes()) / 2u;
}

// ── Raw access ──────────────────────────────────────────────────────────────

bool FlashLog::read(uint32_t addr, uint8_t* out, size_t len)
{
    if (!_hal.ops.read(_hal.context, addr, out, len)) {
        ++_stats.storageErrors;
        return false;
    }
    return true;
}

bool FlashLog::programChecked(uint32_t addr, const uint8_t* data, size_t len)
{
    const uint32_t base = _hal.geometry.base;
    const uint32_t pageSize = _hal.geometry.page_size;
    while (len > 0u) {
        // The HAL takes one page per call; addr and the page size are both
        // multiples of the write unit, so the pieces are too.
        const uint32_t pageEnd = base + ((addr - base) / pageSize + 1u) * pageSize;
        const uint32_t piece = static_cast<uint32_t>(std::min<size_t>(len, pageEnd - addr));
        if (!_hal.ops.program(_hal.context, addr, data, piece)) {
            ++_stats.storageErrors;
            KNX_LOGW(TAG, "Program failed at 0x%08lx", static_cast<unsigned long>(addr));
            return false;
        }
        std::array<uint8_t, STAGE_BYTES> back{};
        for (uint32_t done = 0; done < piece; done += static_cast<uint32_t>(back.size())) {
            const size_t n = std::min<size_t>(back.size(), piece - done);
            if (!read(addr + done, back.data(), n) || std::memcmp(back.data(), data + done, n) != 0) {
                ++_stats.storageErrors;
                KNX_LOGW(TAG, "Verify failed at 0x%08lx", static_cast<unsigned long>(addr + done));
                return false;
            }
        }
        addr += piece;
        data += piece;
        len -= piece;
    }
    return true;
}

void FlashLog::flush(Cursor& cursor, bool final)
{
    size_t n = cursor.fill;
    if (final) {
        const size_t padded = align(static_cast<uint32_t>(n));
        std::fill(cursor.stage.begin() + static_cast<std::ptrdiff_t>(n),
                  cursor.stage.begin() + static_cast<std::ptrdiff_t>(padded), ERASED);
        n = padded;
    }
    if (n == 0u) {
        return;
    }
    if (cursor.ok) {
        cursor.ok = programChecked(cursor.addr, cursor.stage.data(), n);
    }
    cursor.addr += static_cast<uint32_t>(n);
    cursor.fill = 0;
}

void FlashLog::push(Cursor& cursor, std::span<const uint8_t> bytes)
{
    while (!bytes.empty()) {
        const size_t n = std::min(bytes.size(), cursor.stage.size() - cursor.fill);
        std::memcpy(cursor.stage.data() + cursor.fill, bytes.data(), n);
        cursor.fill += n;
        bytes = bytes.subspan(n);
        if (cursor.fill == cursor.stage.size()) {
            flush(cursor, false);  // STAGE_BYTES is a multiple of every write unit
        }
    }
}

bool FlashLog::pageBlank(uint32_t pageAddr)
{
    std::array<uint8_t, STAGE_BYTES> chunk{};
    for (uint32_t done = 0; done < _hal.geometry.page_size; done += chunk.size()) {
        const size_t n = std::min<size_t>(chunk.size(), _hal.geometry.page_size - done);
        if (!read(pageAddr + done, chunk.data(), n)) {
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            if (chunk[i] != ERASED) {
                return false;
            }
        }
    }
    return true;
}

bool FlashLog::bankBlank(uint8_t bank)
{
    const uint32_t pages = _hal.geometry.page_count / 2u;
    for (uint32_t page = 0; page < pages; ++page) {
        if (!pageBlank(bankAddress(bank) + page * _hal.geometry.page_size)) {
            return false;
        }
    }
    return true;
}

bool FlashLog::erasePage(uint32_t pageAddr)
{
    ++_stats.pageErases;
    if (!_hal.ops.erase_page(_hal.context, pageAddr) || !pageBlank(pageAddr)) {
        ++_stats.storageErrors;
        KNX_LOGW(TAG, "Erase failed at 0x%08lx", static_cast<unsigned long>(pageAddr));
        return false;
    }
    return true;
}

bool FlashLog::eraseBank(uint8_t bank)
{
    // The first page first: it holds the header, so a bank whose erase is cut
    // short stops counting as valid at once.
    const uint32_t pages = _hal.geometry.page_count / 2u;
    for (uint32_t page = 0; page < pages; ++page) {
        const uint32_t addr = bankAddress(bank) + page * _hal.geometry.page_size;
        if (!pageBlank(addr) && !erasePage(addr)) {
            return false;
        }
    }
    return true;
}

// ── Format ──────────────────────────────────────────────────────────────────

std::optional<uint32_t> FlashLog::readBankSequence(uint8_t bank)
{
    std::array<uint8_t, BANK_HEADER_FIELDS> h{};
    if (!read(bankAddress(bank), h.data(), h.size())) {
        return std::nullopt;
    }
    if (get32(h.data()) != BANK_MAGIC || get16(h.data() + 8) != BANK_FORMAT ||
        get16(h.data() + 10) != crc16(CRC_INITIAL, std::span<const uint8_t>(h.data(), 10))) {
        return std::nullopt;
    }
    return get32(h.data() + 4);
}

bool FlashLog::writeBankHeader(uint8_t bank, uint32_t sequence)
{
    std::array<uint8_t, BANK_HEADER_FIELDS> h{};
    put32(h.data(), BANK_MAGIC);
    put32(h.data() + 4, sequence);
    put16(h.data() + 8, BANK_FORMAT);
    put16(h.data() + 10, crc16(CRC_INITIAL, std::span<const uint8_t>(h.data(), 10)));
    Cursor cursor;
    cursor.addr = bankAddress(bank);
    push(cursor, h);
    flush(cursor, true);
    return cursor.ok;
}

FlashLog::Step FlashLog::readRecord(uint8_t bank, uint32_t offset, Record& record)
{
    if (offset + RECORD_HEADER > _bankBytes) {
        return Step::End;
    }
    std::array<uint8_t, RECORD_HEADER> h{};
    if (!read(bankAddress(bank) + offset, h.data(), h.size())) {
        return Step::Corrupt;
    }
    if (std::all_of(h.begin(), h.end(), [](uint8_t b) { return b == ERASED; })) {
        return Step::End;
    }
    const uint16_t id = get16(h.data());
    const uint16_t lengthField = get16(h.data() + 2);
    if (get16(h.data() + 4) != headerCheck(id, lengthField)) {
        return Step::Corrupt;
    }
    record.offset = offset;
    record.id = id;
    record.tombstone = (lengthField & TOMBSTONE_BIT) != 0u;
    record.length = static_cast<uint16_t>(lengthField & LENGTH_MASK);
    record.crc = get16(h.data() + 6);
    if ((record.tombstone && record.length != 0u) || offset + recordBytes(record.length) > _bankBytes) {
        return Step::Corrupt;
    }
    return Step::Record;
}

bool FlashLog::recordIntact(uint8_t bank, const Record& record)
{
    std::array<uint8_t, STAGE_BYTES> chunk{};
    const uint16_t lengthField = static_cast<uint16_t>(record.length | (record.tombstone ? TOMBSTONE_BIT : 0u));
    put16(chunk.data(), record.id);
    put16(chunk.data() + 2, lengthField);
    uint16_t crc = crc16(CRC_INITIAL, std::span<const uint8_t>(chunk.data(), 4));
    const uint32_t dataAddr = bankAddress(bank) + record.offset + RECORD_HEADER;
    for (uint32_t done = 0; done < record.length; done += chunk.size()) {
        const size_t n = std::min<size_t>(chunk.size(), record.length - done);
        if (!read(dataAddr + done, chunk.data(), n)) {
            return false;
        }
        crc = crc16(crc, std::span<const uint8_t>(chunk.data(), n));
    }
    return crc == record.crc;
}

std::optional<FlashLog::Record> FlashLog::latest(uint16_t id)
{
    std::optional<Record> found;
    Record r;
    for (uint32_t off = headerBytes(); readRecord(_active, off, r) == Step::Record; off += recordBytes(r.length)) {
        if (r.id == id && recordIntact(_active, r)) {
            found = r;
        }
    }
    return found;
}

bool FlashLog::isLatest(const Record& record)
{
    Record r;
    for (uint32_t off = record.offset + recordBytes(record.length); readRecord(_active, off, r) == Step::Record;
         off += recordBytes(r.length)) {
        if (r.id == record.id && recordIntact(_active, r)) {
            return false;
        }
    }
    return true;
}

uint32_t FlashLog::liveBytes()
{
    uint32_t total = 0;
    Record r;
    for (uint32_t off = headerBytes(); readRecord(_active, off, r) == Step::Record; off += recordBytes(r.length)) {
        if (!r.tombstone && recordIntact(_active, r) && isLatest(r)) {
            total += recordBytes(r.length);
        }
    }
    return total;
}

// ── Public operations ───────────────────────────────────────────────────────

util::Result<void> FlashLog::mount()
{
    _mounted = false;
    _stats = {};
    if (!knx_flash_hal_is_valid(&_hal)) {
        return util::ErrorCode::InvalidParameter;
    }
    _bankBytes = _hal.geometry.page_size * (_hal.geometry.page_count / 2u);
    if (_bankBytes < headerBytes() + recordBytes(0)) {
        return util::ErrorCode::InvalidParameter;
    }

    const auto seq0 = readBankSequence(0);
    const auto seq1 = readBankSequence(1);
    if (!seq0 && !seq1) {
        // A new device, or a region something else overwrote: nothing to keep.
        ++_stats.formats;
        KNX_LOGW(TAG, "No valid bank; formatting");
        if (!eraseBank(0) || !writeBankHeader(0, 1u)) {
            return util::ErrorCode::StorageError;
        }
        _active = 0;
        _sequence = 1u;
    } else {
        _active = (seq1 && (!seq0 || *seq1 > *seq0)) ? 1u : 0u;
        _sequence = _active == 1u ? *seq1 : *seq0;
    }

    Record r;
    uint32_t off = headerBytes();
    Step step;
    while ((step = readRecord(_active, off, r)) == Step::Record) {
        if (!recordIntact(_active, r)) {
            ++_stats.tornRecords;
        }
        off += recordBytes(r.length);
    }
    // Past a header that fails its check nothing can be found again, so the
    // bank takes no more records; the next save compacts.
    _writeOffset = step == Step::Corrupt ? _bankBytes : off;
    if (step == Step::Corrupt) {
        KNX_LOGW(TAG, "Unreadable record header at offset %lu; bank closed", static_cast<unsigned long>(off));
    }

    _spareDirty = !bankBlank(static_cast<uint8_t>(_active ^ 1u));
    _nextErasePage = 0;
    _compactCheckDue = true;
    _mounted = true;
    return util::Result<void>::ok();
}

util::Result<void> FlashLog::save(uint16_t id, std::span<const uint8_t> data)
{
    if (!_mounted) {
        return util::ErrorCode::NotInitialized;
    }
    if (data.size() > MAX_RECORD_LENGTH ||
        recordBytes(static_cast<uint16_t>(data.size())) > _bankBytes - headerBytes()) {
        return util::ErrorCode::InvalidParameter;
    }
    return append(id, static_cast<uint16_t>(data.size()), data);
}

util::Result<void> FlashLog::load(uint16_t id, std::vector<uint8_t>& out)
{
    if (!_mounted) {
        return util::ErrorCode::NotInitialized;
    }
    const auto r = latest(id);
    if (!r || r->tombstone) {
        return util::ErrorCode::NotFound;
    }
    out.resize(r->length);
    if (r->length != 0u && !read(bankAddress(_active) + r->offset + RECORD_HEADER, out.data(), r->length)) {
        return util::ErrorCode::StorageError;
    }
    return util::Result<void>::ok();
}

util::Result<bool> FlashLog::exists(uint16_t id)
{
    if (!_mounted) {
        return util::ErrorCode::NotInitialized;
    }
    const auto r = latest(id);
    return r.has_value() && !r->tombstone;
}

util::Result<void> FlashLog::erase(uint16_t id)
{
    if (!_mounted) {
        return util::ErrorCode::NotInitialized;
    }
    const auto r = latest(id);
    if (!r || r->tombstone) {
        return util::ErrorCode::NotFound;
    }
    return append(id, TOMBSTONE_BIT, {});
}

util::Result<void> FlashLog::eraseAll()
{
    if (!_mounted) {
        return util::ErrorCode::NotInitialized;
    }
    if (_spareDirty) {
        if (!eraseBank(static_cast<uint8_t>(_active ^ 1u))) {
            return util::ErrorCode::StorageError;
        }
        _spareDirty = false;
    }
    return switchToSpare(headerBytes());
}

util::Result<void> FlashLog::listIds(std::vector<uint16_t>& out)
{
    if (!_mounted) {
        return util::ErrorCode::NotInitialized;
    }
    Record r;
    for (uint32_t off = headerBytes(); readRecord(_active, off, r) == Step::Record; off += recordBytes(r.length)) {
        if (!r.tombstone && recordIntact(_active, r) && isLatest(r)) {
            out.push_back(r.id);
        }
    }
    return util::Result<void>::ok();
}

util::Result<void> FlashLog::service()
{
    if (!_mounted) {
        return util::ErrorCode::NotInitialized;
    }
    if (_spareDirty) {
        const uint8_t spare = static_cast<uint8_t>(_active ^ 1u);
        const uint32_t pages = _hal.geometry.page_count / 2u;
        while (_nextErasePage < pages) {
            const uint32_t addr = bankAddress(spare) + _nextErasePage * _hal.geometry.page_size;
            if (pageBlank(addr)) {
                ++_nextErasePage;
                continue;
            }
            if (!erasePage(addr)) {
                return util::ErrorCode::StorageError;  // the same page again next time
            }
            ++_nextErasePage;
            return util::Result<void>::ok();  // one erase per call
        }
        _spareDirty = false;
        return util::Result<void>::ok();
    }
    if (_compactCheckDue && freeBytes() < reserveBytes()) {
        _compactCheckDue = false;
        // Only when compacting brings the reserve back; a bank that is full of
        // live data would otherwise be compacted, and erased, on every call.
        if (_bankBytes - headerBytes() - liveBytes() >= reserveBytes()) {
            return compact();
        }
    }
    return util::Result<void>::ok();
}

FlashLogStatistics FlashLog::statistics() const
{
    FlashLogStatistics s = _stats;
    s.sequence = _sequence;
    s.bankBytes = _bankBytes;
    s.freeBytes = _mounted ? freeBytes() : 0u;
    return s;
}

// ── Writing ─────────────────────────────────────────────────────────────────

util::Result<void> FlashLog::append(uint16_t id, uint16_t lengthField, std::span<const uint8_t> data)
{
    const uint32_t need = recordBytes(static_cast<uint16_t>(lengthField & LENGTH_MASK));
    if (need > freeBytes()) {
        const auto compacted = compact();
        if (compacted.isError()) {
            return compacted;
        }
        if (need > freeBytes()) {
            return util::ErrorCode::StorageFull;
        }
    }

    std::array<uint8_t, RECORD_HEADER> h{};
    put16(h.data(), id);
    put16(h.data() + 2, lengthField);
    put16(h.data() + 4, headerCheck(id, lengthField));
    put16(h.data() + 6, crc16(crc16(CRC_INITIAL, std::span<const uint8_t>(h.data(), 4)), data));

    // The header (with the data's CRC) goes first: a record cut short anywhere
    // after it fails its CRC and is ignored.
    Cursor cursor;
    cursor.addr = bankAddress(_active) + _writeOffset;
    push(cursor, h);
    push(cursor, data);
    flush(cursor, true);
    if (!cursor.ok) {
        _writeOffset = _bankBytes;  // what follows a failed write is not trusted
        return util::ErrorCode::StorageError;
    }
    _writeOffset += need;
    _compactCheckDue = true;
    return util::Result<void>::ok();
}

util::Result<void> FlashLog::compact()
{
    if (_spareDirty) {
        KNX_LOGW(TAG, "Bank full; compaction waits for service() to erase the spare bank");
        return util::ErrorCode::StorageFull;
    }
    const uint8_t spare = static_cast<uint8_t>(_active ^ 1u);
    Cursor cursor;
    cursor.addr = bankAddress(spare) + headerBytes();
    Record r;
    std::array<uint8_t, STAGE_BYTES> chunk{};
    for (uint32_t off = headerBytes(); readRecord(_active, off, r) == Step::Record; off += recordBytes(r.length)) {
        if (r.tombstone || !recordIntact(_active, r) || !isLatest(r)) {
            continue;
        }
        // Copied as stored, padding included, so the copy is word-aligned too.
        const uint32_t size = recordBytes(r.length);
        for (uint32_t done = 0; done < size; done += chunk.size()) {
            const size_t n = std::min<size_t>(chunk.size(), size - done);
            if (!read(bankAddress(_active) + off + done, chunk.data(), n)) {
                cursor.ok = false;
                break;
            }
            push(cursor, std::span<const uint8_t>(chunk.data(), n));
        }
        if (!cursor.ok) {
            break;
        }
    }
    flush(cursor, true);
    if (!cursor.ok) {
        _spareDirty = true;
        _nextErasePage = 0;
        return util::ErrorCode::StorageError;
    }
    return switchToSpare(cursor.addr - bankAddress(spare));
}

util::Result<void> FlashLog::switchToSpare(uint32_t writeOffset)
{
    const uint8_t spare = static_cast<uint8_t>(_active ^ 1u);
    // The header is the commit: until it is written, the old bank is current.
    if (!writeBankHeader(spare, _sequence + 1u)) {
        _spareDirty = true;
        _nextErasePage = 0;
        return util::ErrorCode::StorageError;
    }
    _active = spare;
    ++_sequence;
    _writeOffset = writeOffset;
    _spareDirty = true;  // the old bank, erased by service()
    _nextErasePage = 0;
    _compactCheckDue = false;
    ++_stats.compactions;
    return util::Result<void>::ok();
}

} // namespace objects
} // namespace knx
