// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file security_interface_object.hpp
 * @brief KNX Security Interface Object (Interface Object Type 17)
 *
 * Manages security-related configuration, key material, and sequence numbers.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/objects/security_individual_address_table.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <span>
#include "knx/platform/mutex.hpp"

namespace knx {
namespace objects {

/**
 * @brief Security Interface Object properties (KNX spec)
 */
enum class SecurityProperty : uint8_t {
    ObjectType = 1,                 // Interface Object Type (17)
    ObjectName = 2,                 // Name of the object
    LoadStateControl = 5,           // PID_LOAD_STATE_CONTROL
    SecurityMode = 51,              // PID_SECURITY_MODE
    P2PKeyTable = 52,               // PID_P2P_KEY_TABLE
    GroupKeyTable = 53,             // PID_GRP_KEY_TABLE
    SecurityIndividualAddressTable = 54, // PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE
    SecurityFailuresLog = 55,       // PID_SECURITY_FAILURES_LOG
    ToolKey = 56,                   // PID_TOOL_KEY
    SecurityReport = 57,            // PID_SECURITY_REPORT
    SecurityReportControl = 58,     // PID_SECURITY_REPORT_CONTROL
    SequenceNumberSending = 59,     // PID_SEQUENCE_NUMBER_SENDING
    ZoneKeyTable = 60,              // PID_ZONE_KEY_TABLE
    GoSecurityFlags = 61,           // PID_GO_SECURITY_FLAGS
};

/**
 * @brief Security mode enumeration
 */
enum class SecurityMode : uint8_t {
    Disabled = 0,
    Enabled = 1
};

/**
 * @brief Security failure types
 */
enum class SecurityFailure : uint8_t {
    None = 0,
    SequenceNumberInvalid = 1,
    AuthenticationFailed = 2,
    DecryptionFailed = 3,
    UnknownKey = 4,
    InvalidSender = 5
};

/**
 * @brief Security Interface Object (Manufacturer-Specific Type)
 *
 * Central security configuration and key management for KNX Secure.
 * Manages sequence numbers, keys, and security state per KNX Data Security spec.
 *
 * Every table is a fixed array sized by the KNX_SECURITY_* capacities
 * (Kconfig), so nothing here allocates once the object is built, apart from
 * the one-off setGroupObjectCount().
 */
class SecurityInterfaceObject : public InterfaceObject {
public:
    SecurityInterfaceObject();
    ~SecurityInterfaceObject() override = default;

    // === InterfaceObject interface ===
    InterfaceObjectType objectType() const override { return InterfaceObjectType::security(); }
    KernelBinding kernelBinding() const override;

    // The kernel binding and the BAU hold its address.
    SecurityInterfaceObject(const SecurityInterfaceObject&) = delete;
    SecurityInterfaceObject& operator=(const SecurityInterfaceObject&) = delete;
    SecurityInterfaceObject(SecurityInterfaceObject&&) = delete;
    SecurityInterfaceObject& operator=(SecurityInterfaceObject&&) = delete;

    // === Security Mode ===

    /**
     * @brief Get security mode
     */
    SecurityMode getSecurityMode() const { return _securityMode; }

    /**
     * @brief Set security mode
     *
     * Before BusAccessUnit::init() this is the factory default, which a
     * restored PID_SECURITY_MODE takes precedence over; after it, in force at
     * once. See the rule on BusAccessUnit::init().
     */
    void setSecurityMode(SecurityMode mode);
    void setSecurityModeSilent(SecurityMode mode);

    /**
     * @brief Check if security is enabled
     */
    bool isSecurityEnabled() const { return _securityMode == SecurityMode::Enabled; }

    // === Tool Key Management ===

    /**
     * @brief Get security tool key (for ETS/provisioning)
     */
    const std::array<uint8_t, 16>& getToolKey() const { return _toolKey; }

    /**
     * @brief Set security tool key
     *
     * Before BusAccessUnit::init() this is the factory Tool Key (FDSK): in
     * force unless a non-zero PID_TOOL_KEY is restored, and what a Factory
     * Reset returns to. After it, in force at once.
     */
    void setToolKey(const std::array<uint8_t, 16>& key);
    void setToolKeySilent(const std::array<uint8_t, 16>& key);

    /**
     * @brief Check if tool key is set (non-zero)
     */
    bool hasToolKey() const;

    // === Group and device keys ===
    //
    // Two sources feed the keys the Secure Application Layer looks up:
    //
    //  - firmware-provided keys, set with setGroupKey() / setDeviceKey(); and
    //  - the downloaded tables PID_GRP_KEY_TABLE / PID_P2P_KEY_TABLE, read in
    //    place on every lookup: a group key through the group address's index
    //    in the address table (setGroupAddressResolver()), a device key
    //    through the partner's index in the Security Individual Address Table.
    //
    // Precedence is per table, the same "factory default vs downloaded" rule
    // as the Tool Key (see BusAccessUnit::init): firmware keys are the
    // factory default, in force while the corresponding downloaded table
    // holds no entries. Once ETS has downloaded entries into it, that table
    // alone is in force — a firmware key for an address the project left
    // plain would otherwise make the device demand security there — and the
    // firmware keys come back only if the table is emptied again (Unload,
    // Master Reset). Firmware keys are kept across BusAccessUnit::init(), so
    // they can be set before or after it.

    /**
     * @brief Set a firmware-provided key for an individual device
     *
     * In force immediately while no downloaded P2P key table is; otherwise
     * recorded and logged, and in force again once that table is emptied.
     * Refused with a log line once KNX_SECURITY_FIRMWARE_KEYS keys are set.
     */
    void setDeviceKey(const IndividualAddress& address, const std::array<uint8_t, 16>& key);

    /**
     * @brief Get the key in force for an individual device
     * @return ok, or ResourceUnavailable when no key is in force
     */
    util::Result<void> getDeviceKey(const IndividualAddress& address, std::array<uint8_t, 16>& key) const;

    /// Remove a firmware-provided device key (downloaded keys are ETS's).
    void removeDeviceKey(const IndividualAddress& address);

    /// Number of device keys in force.
    uint16_t deviceKeyCount() const;

    /**
     * @brief Set a firmware-provided key for a group address
     *
     * In force immediately while no downloaded group key table is; otherwise
     * recorded and logged, and in force again once that table is emptied.
     * Refused with a log line once KNX_SECURITY_FIRMWARE_KEYS keys are set.
     */
    void setGroupKey(const GroupAddress& address, const std::array<uint8_t, 16>& key);

    /**
     * @brief Get the key in force for a group address
     * @return ok, or ResourceUnavailable when no key is in force
     */
    util::Result<void> getGroupKey(const GroupAddress& address, std::array<uint8_t, 16>& key) const;

    /// Remove a firmware-provided group key (downloaded keys are ETS's).
    void removeGroupKey(const GroupAddress& address);

    /// Number of group keys in force.
    uint16_t groupKeyCount() const;

    /// The group address at a GA_Index: the 1-based index into the group
    /// address table (03/05/01 §6.3.7), through which alone the group key
    /// table refers to group addresses. GroupAddress(0) when there is none.
    using GroupAddressResolver = GroupAddress (*)(const void* context, uint16_t gaIndex);

    /// Install the resolver (the BAU passes the address table's). Without
    /// one, no downloaded group key resolves.
    void setGroupAddressResolver(GroupAddressResolver resolver, const void* context);

    /// True while the downloaded group (device) key table, not the firmware
    /// keys, is what is in force: while it holds at least one entry.
    bool downloadedGroupKeysInForce() const { return _groupKeyTable.count != 0u; }
    bool downloadedDeviceKeysInForce() const { return _p2pKeyTable.count != 0u; }

    /// What the downloaded key tables resolve to now, for the log line after a
    /// download or a restore.
    struct KeyTableSummary {
        uint16_t groupKeys{0};            ///< entries whose GA_Index resolves
        uint16_t unresolvedGroupKeys{0};  ///< entries whose GA_Index does not
        uint16_t deviceKeys{0};           ///< entries whose IA_Index resolves
        uint16_t unresolvedDeviceKeys{0}; ///< entries whose IA_Index does not
        uint16_t securityAddresses{0};    ///< Security Individual Address Table entries
    };
    KeyTableSummary keyTableSummary() const;

    // === Sequence Number Management ===

    /**
     * @brief Get sending sequence number
     */
    uint64_t getSendingSequence() const { return _sendingSequence; }
    uint64_t getSendingSequenceThreadSafe() const;

    /**
     * @brief Get receiving sequence number
     */
    uint64_t getReceivingSequence() const { return _receivingSequence; }

    /**
     * @brief Take the next sending sequence number (thread-safe)
     *
     * With a reserve writer installed (setSendingReserveWriter()), a number
     * above the stored ceiling is taken only after a new ceiling has been
     * written. When that write fails the number is not taken and the result is
     * ResourceUnavailable: the caller must not send, since a number that was
     * never reserved could be sent again after a reset.
     *
     * @return The number to send with
     */
    util::Result<uint64_t> incrementSendingSequence();

    // === Sending sequence reserve ===
    //
    // A Data Secure sending number must never be used twice, across any reset.
    // Rather than storing the counter with every telegram, the device stores a
    // ceiling `window` numbers ahead and uses the numbers up to it freely
    // (docs/design/data-secure-sequence-persistence.md §3.1). With no writer
    // installed (no persistence) there is no ceiling and the counter is
    // unlimited, as before.

    /// Stores @p ceiling persistently; false when it could not.
    using SendingReserveWriter = bool (*)(void* context, uint64_t ceiling);

    /// Install the writer and the window (at least 2). Pass nullptr to remove.
    void setSendingReserveWriter(SendingReserveWriter writer, void* context, uint64_t window);

    /// The ceiling up to which numbers may be used without another write.
    uint64_t sendingReserveCeiling() const;

    /// Adopt the ceiling stored before the last reset: every number up to it
    /// may have been used, so the counter moves up to it and nothing beyond it
    /// counts as reserved. The first send then stores a new ceiling.
    void restoreSendingReserveCeiling(uint64_t ceiling);

    /// True once a send has left fewer than half of the window: the BAU then
    /// stores a new ceiling in the background, before the window runs out.
    bool sendingReserveLow() const;

    /// Store a new ceiling (counter + window) now. True when stored, or when
    /// there is nothing to do; false when the write failed.
    bool topUpSendingReserve();

    /**
     * @brief Update receiving sequence if valid (thread-safe)
     * @param seq Received sequence number
     * @return true if sequence is valid and accepted
     */
    util::Result<void> updateReceivingSequence(uint64_t seq);

    /**
     * @brief Set sending sequence (for initialization)
     *
     * Before BusAccessUnit::init(), the higher of this and the restored
     * PID_SEQUENCE_NUMBER_SENDING is kept: a used number is never reused.
     */
    void setSendingSequence(uint64_t seq);
    void setSendingSequenceSilent(uint64_t seq);

    /**
     * @brief Set receiving sequence (for initialization)
     */
    void setReceivingSequence(uint64_t seq);

    // === Last Valid Sequence Numbers of communication partners ===
    //
    // 03/03/07 §5.3.1: a receiver keeps, per communication partner, the last
    // sequence number it accepted from that partner, and rejects anything not
    // strictly greater.  The tool (ETS, talking under the Tool Key) is tracked
    // separately from ordinary partners — it is the "Sequence Number for Tool
    // Access" and it is what the S-A_Sync-service synchronises.

    /// Last valid sequence number received under Tool Access.
    ///
    /// 03/05/01 §6.2 keeps this one *outside* the Security Individual Address
    /// Table — it belongs to whoever holds the Tool Key, not to an IA — and a
    /// Confirmed Restart must leave it unchanged, so it is persisted on its own
    /// (see InterfaceObjectManager::saveSecuritySequenceState).
    uint64_t getToolAccessSequence() const;
    void setToolAccessSequence(uint64_t seq);

    /// Last valid sequence number received from @p address (0 when unknown).
    ///
    /// For a partner listed in PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE it is the
    /// table entry's (§6.3.8.4), read and written in place: the table is a
    /// persisted property, so this is what makes it durable. A sender without
    /// an entry takes one of KNX_SECURITY_UNLISTED_SENDERS slots, which are
    /// not persisted.
    uint64_t getPeerSequence(const IndividualAddress& address) const;

    /// Store the last valid sequence number of @p address.
    /// @return ResourceUnavailable when @p address is not in the table and
    ///         every unlisted slot is taken: the caller must then reject the
    ///         telegram, since its number could not be remembered.
    util::Result<void> setPeerSequence(const IndividualAddress& address, uint64_t seq);

    /// True when @p address has an entry in the Security Individual Address
    /// Table or an unlisted slot, or an unlisted slot is free for it.
    bool canTrackPeer(const IndividualAddress& address) const;

    /// True when an accepted telegram has moved a sequence number since the
    /// last checkpoint. §6.3.8.4 requires the Last Valid SeqNr values to "be
    /// saved in full at power-down and be restored in full at power-up"; the
    /// BAU checkpoints them periodically, on the restart path and when the
    /// product reports a power failure, and uses these flags to skip the write
    /// when nothing secured has been received.
    bool sequenceStateDirty() const;
    void clearSequenceStateDirty();

    /// The two records of the checkpoint, apart: the Security Individual
    /// Address Table (partner numbers) and the Sequence Number for Tool Access.
    /// take*() reads and clears in one step, so a number accepted while the
    /// record is being written marks it again; a failed write marks it back.
    bool peerSequencesDirty() const;
    bool toolAccessSequenceDirty() const;
    bool takePeerSequencesDirty();
    bool takeToolAccessSequenceDirty();
    void markPeerSequencesDirty();
    void markToolAccessSequenceDirty();

    // === Security Failures ===

    /**
     * @brief Log a security failure
     */
    void logSecurityFailure(SecurityFailure failure, uint16_t source = 0);

    /**
     * @brief Get failure count
     */
    uint32_t getFailureCount() const { return _failureCount; }

    /**
     * @brief Get last failure
     */
    SecurityFailure getLastFailure() const { return _lastFailure; }
    uint16_t getLastFailureSource() const { return _lastFailureSource; }

    /**
     * @brief Clear failure log
     */
    void clearFailureLog();

    // === Bulk Operations ===

    /**
     * @brief Clear every key: the Tool Key, the downloaded key tables and the
     * firmware-provided keys. A full wipe, not a Master Reset — the BAU's
     * Master Reset goes back to the factory defaults instead (see
     * BusAccessUnit::init for what those are).
     */
    void clearAllKeys();

    /**
     * @brief Validate security configuration
     */
    bool isValid() const;

    // === Array properties as stored ===
    //
    // PID_P2P_KEY_TABLE, PID_GRP_KEY_TABLE, PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE,
    // PID_ZONE_KEY_TABLE, PID_GO_SECURITY_FLAGS, PID_SECURITY_REPORT and
    // PID_SECURITY_REPORT_CONTROL are fixed arrays in their wire format with a
    // current element count. The capacities are the KNX_SECURITY_* settings
    // (and the group object count for the flags, setGroupObjectCount()).
    // Writing past one is refused.

    /// True for the properties above.
    static bool isArrayProperty(SecurityProperty prop);

    /// The current elements of @p prop in wire format (empty when it holds
    /// none, or for any other property).
    std::span<const uint8_t> tableData(SecurityProperty prop) const;

    /// Element size and capacity of @p prop (0 and 0 for any other property).
    uint8_t tableElementSize(SecurityProperty prop) const;
    uint16_t tableCapacity(SecurityProperty prop) const;

    /// Write @p data (whole elements) from element @p firstElement on,
    /// extending the current count when the write ends beyond it (a gap is
    /// zero-filled). OutOfRange when it would pass the capacity.
    util::Result<void> writeTable(SecurityProperty prop, size_t firstElement,
                                  std::span<const uint8_t> data);

    /// Set the current element count to @p count and zero the elements: ETS
    /// empties a table this way before downloading it again. OutOfRange when
    /// @p count passes the capacity.
    util::Result<void> resizeTable(SecurityProperty prop, uint16_t count);

    /// Room for PID_GO_SECURITY_FLAGS: one octet per group object. Called
    /// once, before the heap is locked (the BAU does, with the group object
    /// count); a later call keeps the first size.
    void setGroupObjectCount(uint16_t count);

    /// PID_GO_SECURITY_FLAGS of group object @p number (0 when not written).
    uint8_t goSecurityFlags(uint16_t number) const;

    // === Load state (PID_LOAD_STATE_CONTROL) ===
    //
    // Driven by the ETS download procedure via LoadControlProperty. Unlike the
    // address/association/group-object tables, the loadable content here is not
    // firmware-defined: key tables exist only once ETS has downloaded them, so
    // the object boots Unloaded rather than Loaded.
    uint8_t loadState() const { return _loadState; }
    void setLoadState(uint8_t state) { _loadState = state; }

    // Invoked on StartLoading and Unload. Discards the downloadable key
    // material (every array property above) but deliberately keeps the tool
    // key: the tool key is the credential ETS is *currently* using to talk to
    // this device, and it is provisioned from the device's own RNG/FDSK rather
    // than downloaded. Clearing it here would sever the secure link mid-download.
    // With the tables gone, the firmware-provided group and device keys are
    // the ones in force again. The partners' Last Valid SeqNr values move to
    // the unlisted slots, so downloading the same partners again cannot roll
    // their replay windows back.
    void loadControlReset();

private:
    /// A table in its wire format: up to @p Capacity elements of @p ElementSize.
    template <size_t ElementSize, size_t Capacity>
    struct Table {
        std::array<uint8_t, ElementSize * Capacity> data{};
        uint16_t count{0};

        /// Empty and zeroed, so no key lingers in RAM.
        void clear() { data.fill(0); count = 0; }

        std::span<uint8_t> used() { return std::span<uint8_t>(data.data(), count * ElementSize); }
        std::span<const uint8_t> used() const {
            return std::span<const uint8_t>(data.data(), count * ElementSize);
        }
    };

    /// Untyped access to one of the tables, for the shared property paths.
    struct TableRef {
        uint8_t* data{nullptr};
        uint16_t* count{nullptr};
        uint16_t capacity{0};
        uint8_t elementSize{0};
    };
    TableRef tableRef(SecurityProperty prop);
    TableRef tableRef(SecurityProperty prop) const {
        return const_cast<SecurityInterfaceObject*>(this)->tableRef(prop);
    }

    /// A key the firmware set (setGroupKey / setDeviceKey).
    struct FirmwareKey {
        std::array<uint8_t, 16> key{};
        uint16_t address{0};
        bool group{false};
    };
    const FirmwareKey* findFirmwareKey(uint16_t address, bool group) const;
    bool setFirmwareKey(uint16_t address, bool group, const std::array<uint8_t, 16>& key);
    void removeFirmwareKey(uint16_t address, bool group);
    uint16_t firmwareKeyCount(bool group) const;

    /// The group address at @p gaIndex, GroupAddress(0) when there is none.
    GroupAddress groupAddressAt(uint16_t gaIndex) const;

    /// Unlisted senders (see getPeerSequence), held as IA table entries.
    /// All of these expect _sequenceMutex held.
    bool parkSequenceLocked(IndividualAddress address, uint64_t seq);
    void parkIaEntriesLocked(size_t first, size_t count);
    void adoptParkedLocked(size_t first, size_t count);
    void removeUnlistedLocked(size_t index);

    SecurityMode _securityMode;
    std::array<uint8_t, 16> _toolKey;

    Table<20, config::SECURITY_P2P_KEYS> _p2pKeyTable;      // IA_Index(2) + key(16) + roles(2)
    Table<18, config::SECURITY_GROUP_KEYS> _groupKeyTable;  // GA_Index(2) + key(16)
    Table<security_ia_table::kEntryBytes, config::SECURITY_IA_ENTRIES> _iaTable;
    Table<19, config::SECURITY_ZONE_KEYS> _zoneKeyTable;
    Table<1, 1> _securityReport;
    Table<1, 1> _securityReportControl;
    // PID_GO_SECURITY_FLAGS, sized once by setGroupObjectCount().
    std::unique_ptr<uint8_t[]> _goSecurityFlags;
    uint16_t _goSecurityFlagsCapacity{0};
    uint16_t _goSecurityFlagsCount{0};

    std::array<FirmwareKey, config::SECURITY_FIRMWARE_KEYS> _firmwareKeys{};
    uint16_t _firmwareKeyCount{0};

    GroupAddressResolver _groupAddressResolver{nullptr};
    const void* _groupAddressResolverContext{nullptr};

    // Sequence numbers (48-bit in KNX spec, using 64-bit for safety)
    uint64_t _sendingSequence;
    uint64_t _receivingSequence;
    uint64_t _toolAccessSequence{0};
    // Senders without an entry in _iaTable, in the same entry format.
    Table<security_ia_table::kEntryBytes, config::SECURITY_UNLISTED_SENDERS> _unlisted;
    bool _peerSequencesDirty{false};              // the partner table is owed a checkpoint
    bool _toolAccessSequenceDirty{false};         // the Tool Access record is owed one

    // Sending reserve (see setSendingReserveWriter()).
    SendingReserveWriter _sendingReserveWriter{nullptr};
    void* _sendingReserveContext{nullptr};
    uint64_t _sendingReserveWindow{0};
    uint64_t _sendingCeiling{0};       // numbers up to this one are reserved
    bool _sendingReserveLow{false};    // less than half of the window left
    bool storeSendingCeilingLocked(uint64_t ceiling);

    // Security failures
    SecurityFailure _lastFailure;
    uint32_t _failureCount;
    uint16_t _lastFailureSource;

    // Guards the sequence numbers, the Security Individual Address Table that
    // holds the partners' numbers, and the unlisted slots.
    mutable platform::LocalMutex _sequenceMutex;

    uint8_t _loadState{0};  // loadstate::kUnloaded — key tables come from ETS only

    // Helper: check if key is non-zero
    static bool isKeyNonZero(const std::array<uint8_t, 16>& key);

    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
