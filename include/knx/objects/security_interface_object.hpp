// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file security_interface_object.hpp
 * @brief KNX Security Interface Object (Interface Object Type 17)
 * 
 * Manages security-related configuration, key material, and sequence numbers.
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/objects/security_individual_address_table.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <array>
#include <vector>
#include <map>
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
 * Note: Security interface objects are standardized in newer KNX specs.
 */
class SecurityInterfaceObject : public InterfaceObject {
public:
    SecurityInterfaceObject();
    ~SecurityInterfaceObject() override = default;

    // === InterfaceObject interface ===
    InterfaceObjectType objectType() const override { return InterfaceObjectType::security(); }
    KernelBinding kernelBinding() const override;

    // Disable copy, enable move
    SecurityInterfaceObject(const SecurityInterfaceObject&) = delete;
    SecurityInterfaceObject& operator=(const SecurityInterfaceObject&) = delete;
    SecurityInterfaceObject(SecurityInterfaceObject&&) = default;
    SecurityInterfaceObject& operator=(SecurityInterfaceObject&&) = default;

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
    //  - the downloaded tables PID_GRP_KEY_TABLE / PID_P2P_KEY_TABLE, which the
    //    BAU resolves against the address tables after every download and
    //    every restore (applyDownloadedGroupKeys / applyDownloadedDeviceKeys).
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

    using KeyMap = std::map<uint16_t, std::array<uint8_t, 16>>;

    /**
     * @brief Install the keys resolved from PID_GRP_KEY_TABLE.
     *
     * @p tableHasEntries says whether the downloaded table holds any entry at
     * all, resolved or not: a table whose GA_Index values do not resolve yet
     * (mid-download) is still ETS's table, and the firmware keys stay out of
     * force. With no entries, the firmware keys are in force instead.
     */
    void applyDownloadedGroupKeys(KeyMap resolved, bool tableHasEntries);
    /// The same for PID_P2P_KEY_TABLE and the firmware device keys.
    void applyDownloadedDeviceKeys(KeyMap resolved, bool tableHasEntries);

    /// True while the downloaded group (device) key table, not the firmware
    /// keys, is what is in force.
    bool downloadedGroupKeysInForce() const { return _downloadedGroupKeysInForce; }
    bool downloadedDeviceKeysInForce() const { return _downloadedDeviceKeysInForce; }

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
    /// The live map is the working copy; PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE
    /// is its durable form. setPeerSequence() writes through to the table entry
    /// of a known partner (§6.3.8.4), and syncSequencesFromAddressTable() reads
    /// the table back after ETS downloads it or persistence restores it.
    uint64_t getPeerSequence(const IndividualAddress& address) const;
    void setPeerSequence(const IndividualAddress& address, uint64_t seq);

    /// Reconcile the live map with PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE,
    /// keeping the higher value on each side. Call it whenever that property
    /// changes: after a download, and after a restore from persistence.
    void syncSequencesFromAddressTable();

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
     * @brief Clear every key: the Tool Key, the keys in force and the
     * firmware-provided ones. A full wipe, not a Master Reset — the BAU's
     * Master Reset goes back to the factory defaults instead (see
     * BusAccessUnit::init for what those are).
     */
    void clearAllKeys();
    
    /**
     * @brief Validate security configuration
     */
    bool isValid() const;

    const std::vector<uint8_t>* findExtraProperty(SecurityProperty prop) const;
    std::vector<uint8_t>& upsertExtraProperty(SecurityProperty prop);

    // === Load state (PID_LOAD_STATE_CONTROL) ===
    //
    // Driven by the ETS download procedure via LoadControlProperty. Unlike the
    // address/association/group-object tables, the loadable content here is not
    // firmware-defined: key tables exist only once ETS has downloaded them, so
    // the object boots Unloaded rather than Loaded.
    uint8_t loadState() const { return _loadState; }
    void setLoadState(uint8_t state) { _loadState = state; }

    // Invoked on StartLoading and Unload. Discards the downloadable key
    // material (group and device key tables) but deliberately keeps the tool
    // key: the tool key is the credential ETS is *currently* using to talk to
    // this device, and it is provisioned from the device's own RNG/FDSK rather
    // than downloaded. Clearing it here would sever the secure link mid-download.
    // With the tables gone, the firmware-provided group and device keys are
    // the ones in force again.
    void loadControlReset();

private:
    SecurityMode _securityMode;
    std::array<uint8_t, 16> _toolKey;
    
    // Keys in force (looked up by the Secure Application Layer): address -> key.
    KeyMap _deviceKeys;  // Individual address keys
    KeyMap _groupKeys;   // Group address keys
    // Firmware-provided keys: the factory default for each table, in force
    // while the downloaded table holds no entries.
    KeyMap _firmwareDeviceKeys;
    KeyMap _firmwareGroupKeys;
    bool _downloadedDeviceKeysInForce{false};
    bool _downloadedGroupKeysInForce{false};
    
    // Sequence numbers (48-bit in KNX spec, using 64-bit for safety)
    uint64_t _sendingSequence;
    uint64_t _receivingSequence;
    uint64_t _toolAccessSequence{0};
    std::map<uint16_t, uint64_t> _peerSequences;  // source IA -> last valid SeqNr
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
    
    // Thread safety for sequence numbers
    mutable platform::LocalMutex _sequenceMutex;

    // Storage for additional secure interface properties (reference use)
    std::map<SecurityProperty, std::vector<uint8_t>> _extraProperties;

    uint8_t _loadState{0};  // loadstate::kUnloaded — key tables come from ETS only
    
    // Helper: check if key is non-zero
    static bool isKeyNonZero(const std::array<uint8_t, 16>& key);

    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
