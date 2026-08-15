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
#include <mutex>

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
     */
    void setToolKey(const std::array<uint8_t, 16>& key);
    void setToolKeySilent(const std::array<uint8_t, 16>& key);
    
    /**
     * @brief Check if tool key is set (non-zero)
     */
    bool hasToolKey() const;

    // === Individual Device Keys ===
    
    /**
     * @brief Set security key for an individual device
     * @param address Individual address
     * @param key 16-byte AES-128 key
     */
    void setDeviceKey(const IndividualAddress& address, const std::array<uint8_t, 16>& key);
    
    /**
     * @brief Get security key for an individual device
     * @param address Individual address
     * @param key Output buffer for key
    * @return Result<void> indicating success or missing key
     */
    util::Result<void> getDeviceKey(const IndividualAddress& address, std::array<uint8_t, 16>& key) const;
    
    /**
     * @brief Remove device key
     */
    void removeDeviceKey(const IndividualAddress& address);
    
    /**
     * @brief Get count of device keys
     */
    uint16_t deviceKeyCount() const;

    // === Group Address Keys ===
    
    /**
     * @brief Set security key for a group address
     * @param address Group address
     * @param key 16-byte AES-128 key
     */
    void setGroupKey(const GroupAddress& address, const std::array<uint8_t, 16>& key);
    
    /**
     * @brief Get security key for a group address
     * @param address Group address
     * @param key Output buffer for key
    * @return Result<void> indicating success or missing key
     */
    util::Result<void> getGroupKey(const GroupAddress& address, std::array<uint8_t, 16>& key) const;
    
    /**
     * @brief Remove group key
     */
    void removeGroupKey(const GroupAddress& address);
    
    /**
     * @brief Get count of group keys
     */
    uint16_t groupKeyCount() const;

    /**
     * @brief Drop every group / device key without touching the Tool Key.
     *
     * The live maps are a derived view of PID_GRP_KEY_TABLE / PID_P2P_KEY_TABLE,
     * which arrive as opaque array properties and are re-resolved against the
     * Group Address and Security Individual Address tables after every
     * download.  Rebuilding starts from empty so an entry ETS removed does not
     * survive; clearAllKeys() cannot be used for that because it also wipes the
     * Tool Key, which is not part of either table.
     */
    void clearGroupKeys();
    void clearDeviceKeys();

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
     * @brief Increment sending sequence (thread-safe)
     * @return New sequence number
     */
    uint64_t incrementSendingSequence();
    
    /**
     * @brief Update receiving sequence if valid (thread-safe)
     * @param seq Received sequence number
     * @return true if sequence is valid and accepted
     */
    util::Result<void> updateReceivingSequence(uint64_t seq);
    
    /**
     * @brief Set sending sequence (for initialization)
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
    /// saved in full at power-down and be restored in full at power-up"; with
    /// no power-fail signal to hand, the BAU checkpoints them periodically and
    /// on the restart path, and uses this flag to skip the write when nothing
    /// secured has been received.
    bool sequenceStateDirty() const;
    void clearSequenceStateDirty();

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
     * @brief Clear all keys
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
    void loadControlReset();

private:
    SecurityMode _securityMode;
    std::array<uint8_t, 16> _toolKey;
    
    // Key storage: address -> key
    std::map<uint16_t, std::array<uint8_t, 16>> _deviceKeys;  // Individual address keys
    std::map<uint16_t, std::array<uint8_t, 16>> _groupKeys;   // Group address keys
    
    // Sequence numbers (48-bit in KNX spec, using 64-bit for safety)
    uint64_t _sendingSequence;
    uint64_t _receivingSequence;
    uint64_t _toolAccessSequence{0};
    std::map<uint16_t, uint64_t> _peerSequences;  // source IA -> last valid SeqNr
    bool _sequenceStateDirty{false};              // a checkpoint is owed
    
    // Security failures
    SecurityFailure _lastFailure;
    uint32_t _failureCount;
    uint16_t _lastFailureSource;
    
    // Thread safety for sequence numbers
    mutable std::mutex _sequenceMutex;

    // Storage for additional secure interface properties (reference use)
    std::map<SecurityProperty, std::vector<uint8_t>> _extraProperties;

    uint8_t _loadState{0};  // loadstate::kUnloaded — key tables come from ETS only
    
    // Helper: check if key is non-zero
    static bool isKeyNonZero(const std::array<uint8_t, 16>& key);

    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
