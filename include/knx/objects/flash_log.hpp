// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_log.hpp
 * @brief Log-structured key-value store over a board's flash (knx_flash_hal_t)
 *
 * Storage for MCUs without NVS: records keyed by a 16-bit id, appended to a
 * log in one of two banks, the latest record of an id winning. When the
 * active bank is full, the latest record of every id is copied to the other
 * bank, which then becomes active. FlashLogPersistence puts the stack's
 * ObjectPersistence interface on top.
 *
 * Flash layout (all fields little-endian; W is the board's write unit and
 * every record starts on a multiple of W):
 *
 *   Bank header, at the start of each bank, padded to W:
 *     [0-3]  magic 0x474F4C4B   [4-7] sequence   [8-9] format (1)
 *     [10-11] CRC-16 of octets 0-9
 *   Record, padded with 0xFF to a multiple of W:
 *     [0-1] id   [2-3] length (bit 15: tombstone, the id was erased)
 *     [4-5] check = ~(id ^ length ^ 0x5AA5)   [6-7] CRC-16 of id, length and data
 *     [8-]  data
 *
 * CRC-16 is CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF).
 *
 * Power-cut safety. Programming only ever goes forward into erased space,
 * and every change becomes visible in one final step:
 *   - A record's header carries the CRC of its data and is written first, so
 *     a record cut short fails its CRC and is ignored; the previous record of
 *     the id stays the latest.
 *   - A header that fails its check (cut while the header itself was being
 *     written) ends the readable log. The bank takes no further records, and
 *     the next save compacts.
 *   - Compaction writes the other bank's header last, with the next sequence
 *     number. Until it is complete, the old bank is the valid one. At mount the
 *     valid bank with the higher sequence number wins.
 *   - eraseAll() is a compaction that copies nothing.
 *
 * Erase timing. A save never erases: it programs only, into space erased
 * beforehand. Erasing the bank a compaction left behind is service()'s job,
 * one page per call, so a product controls when the CPU stalls on an erase
 * (on most parts, code running from flash stops for the whole erase). The
 * exceptions are mount(), which formats a region with no valid bank, and
 * eraseAll(), which erases the spare bank first if service() has not.
 *
 * Sizing. The live data (the latest record of each id) must fit in one bank
 * with room for the largest burst of saves made between two service() calls:
 * a save that needs a compaction while the spare bank is still waiting for
 * its erase fails with StorageFull. The stack's InterfaceObjectManager writes
 * the whole commissioned state into a second set of ids and then erases the
 * first, so a bank needs at least twice that state plus the reserve below.
 *
 * @thread_safety NOT thread-safe. Call every method from one context (the
 *                stack's main loop), never from an interrupt.
 */

#pragma once

#include "knx/objects/flash_hal.h"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace knx {
namespace objects {

struct FlashLogConfig {
    /// service() compacts ahead of need once the active bank has fewer free
    /// bytes than this, provided compaction would give back at least this
    /// much. 0 means half a bank.
    uint32_t reserveBytes = 0;
};

struct FlashLogStatistics {
    uint32_t sequence = 0;        ///< sequence number of the active bank
    uint32_t bankBytes = 0;       ///< bytes per bank, header included
    uint32_t freeBytes = 0;       ///< bytes left for records in the active bank
    uint32_t compactions = 0;     ///< since mount
    uint32_t pageErases = 0;      ///< since mount
    uint32_t tornRecords = 0;     ///< records with a bad CRC seen at mount
    uint32_t formats = 0;         ///< mounts that found no valid bank
    uint32_t storageErrors = 0;   ///< erase, program, read or verify failures
};

class FlashLog {
public:
    /// Longest record the format can hold; a bank may limit it further.
    static constexpr uint16_t MAX_RECORD_LENGTH = 0x7FFFu;

    /**
     * @param hal    The board's flash region. Copied; the context it points
     *               to must outlive this object.
     * @param config Compaction reserve.
     */
    explicit FlashLog(const knx_flash_hal_t& hal, FlashLogConfig config = {});

    FlashLog(const FlashLog&) = delete;
    FlashLog& operator=(const FlashLog&) = delete;
    FlashLog(FlashLog&&) = delete;
    FlashLog& operator=(FlashLog&&) = delete;

    /**
     * @brief Find the valid bank and the end of its log
     *
     * Chooses the valid bank with the higher sequence number. A region with no
     * valid bank (a new device, or one whose region was overwritten) is
     * formatted: bank 0 is erased and gets an empty log. A spare bank that is
     * not blank is left for service() to erase. Calling it again re-reads the
     * flash.
     *
     * @return ok; InvalidParameter for an invalid HAL or geometry;
     *         StorageError when the format fails
     */
    util::Result<void> mount();

    [[nodiscard]] bool mounted() const { return _mounted; }

    /**
     * @brief Append a record for @p id
     *
     * Compacts first when the active bank has no room and the spare bank is
     * erased. Never erases.
     *
     * @return ok; NotInitialized before mount(); InvalidParameter when the
     *         record could never fit a bank; StorageFull when there is no room
     *         even after compaction, or compaction must wait for service() to
     *         erase the spare bank; StorageError when programming fails (the
     *         bank then takes no more records, and the next save compacts)
     */
    util::Result<void> save(uint16_t id, std::span<const uint8_t> data);

    /**
     * @brief Read the latest record of @p id into @p out
     * @return ok; NotFound when the id has no record or was erased;
     *         NotInitialized; StorageError
     */
    util::Result<void> load(uint16_t id, std::vector<uint8_t>& out);

    /// @return whether @p id has a record; NotInitialized before mount()
    util::Result<bool> exists(uint16_t id);

    /**
     * @brief Erase @p id by appending a tombstone
     * @return ok; NotFound when there is nothing to erase; otherwise as save()
     */
    util::Result<void> erase(uint16_t id);

    /**
     * @brief Drop every record, atomically
     *
     * Starts an empty log in the spare bank, erasing that bank first if
     * service() has not done so yet: this may block for a bank erase.
     *
     * @return ok; NotInitialized; StorageError
     */
    util::Result<void> eraseAll();

    /// Appends the id of every record that exists to @p out, in log order.
    util::Result<void> listIds(std::vector<uint16_t>& out);

    /**
     * @brief One bounded step of background work
     *
     * Erases one page of the spare bank if it needs it; otherwise compacts
     * when the free space is below the reserve (FlashLogConfig). Call it from
     * the main loop: at most one page erase or one compaction per call.
     *
     * @return ok, also when there was nothing to do; StorageError when an
     *         erase or a compaction failed (it is retried on the next call)
     */
    util::Result<void> service();

    /// @return whether service() has work: a spare bank to erase
    [[nodiscard]] bool servicePending() const { return _mounted && _spareDirty; }

    [[nodiscard]] FlashLogStatistics statistics() const;

private:
    struct Record {
        uint32_t offset = 0;  // from the start of the bank
        uint16_t id = 0;
        uint16_t length = 0;  // data octets
        uint16_t crc = 0;     // as stored in the header
        bool tombstone = false;
    };

    enum class Step : uint8_t { Record, End, Corrupt };

    static constexpr size_t STAGE_BYTES = 32;  // at least the largest write unit

    struct Cursor {
        uint32_t addr = 0;
        std::array<uint8_t, STAGE_BYTES> stage{};
        size_t fill = 0;
        bool ok = true;
    };

    [[nodiscard]] uint32_t align(uint32_t bytes) const;
    [[nodiscard]] uint32_t bankAddress(uint8_t bank) const;
    [[nodiscard]] uint32_t headerBytes() const;
    [[nodiscard]] uint32_t recordBytes(uint16_t length) const;
    [[nodiscard]] uint32_t freeBytes() const;

    bool read(uint32_t addr, uint8_t* out, size_t len);
    bool programChecked(uint32_t addr, const uint8_t* data, size_t len);
    void push(Cursor& cursor, std::span<const uint8_t> bytes);
    void flush(Cursor& cursor, bool final);

    std::optional<uint32_t> readBankSequence(uint8_t bank);
    bool writeBankHeader(uint8_t bank, uint32_t sequence);
    Step readRecord(uint8_t bank, uint32_t offset, Record& record);
    bool recordIntact(uint8_t bank, const Record& record);
    [[nodiscard]] uint32_t reserveBytes() const;
    std::optional<Record> latest(uint16_t id);
    bool isLatest(const Record& record);
    uint32_t liveBytes();

    bool bankBlank(uint8_t bank);
    bool pageBlank(uint32_t pageAddr);
    bool erasePage(uint32_t pageAddr);
    bool eraseBank(uint8_t bank);

    util::Result<void> append(uint16_t id, uint16_t lengthField, std::span<const uint8_t> data);
    util::Result<void> compact();
    util::Result<void> switchToSpare(uint32_t writeOffset);

    knx_flash_hal_t _hal;
    FlashLogConfig _config;
    uint32_t _bankBytes = 0;
    uint32_t _sequence = 0;
    uint32_t _writeOffset = 0;  // end of the log in the active bank
    uint32_t _nextErasePage = 0;
    uint8_t _active = 0;
    bool _mounted = false;
    bool _spareDirty = false;
    bool _compactCheckDue = false;
    FlashLogStatistics _stats{};
};

} // namespace objects
} // namespace knx
