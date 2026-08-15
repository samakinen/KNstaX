// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_object.hpp
 * @brief Group object - represents a KNX group data point
 */

#pragma once

#include "knx/types.hpp"
#include "knx/application/dpt.hpp"
#include "knx/config.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"
#include <span>

namespace knx {
namespace application {

/**
 * @brief Per-object outbound transmit policy.
 *
 * Standard KNX sensor-channel behaviour, evaluated in the publish path:
 *  - send-on-change: suppress a publish whose value has not moved by at least
 *    `changeThreshold` (engineering units) from the last transmitted value;
 *  - cyclic/heartbeat: re-send the current value every `cyclicIntervalMs` even
 *    when unchanged, so receivers can detect liveness;
 *  - per-object floor: never send the same object more often than
 *    `minIntervalMs` apart (defers, keeping the latest value).
 *
 * The all-zero default is inert: every publish is sent immediately, matching the
 * pre-policy behaviour. Applies only to transmit (`transmit == true`) objects
 * and never to read responses.
 */
struct GroupObjectTransmitPolicy {
    bool     onChangeEnabled{false}; ///< true: suppress sub-threshold updates.
    double   changeThreshold{0.0};   ///< Engineering-unit delta counted as a change
                                     ///< (<= 0 with onChangeEnabled means any change).
    uint32_t cyclicIntervalMs{0};    ///< Heartbeat period; 0 = no cyclic send.
    uint32_t minIntervalMs{0};       ///< Minimum spacing between sends; 0 = none.
};

/**
 * @brief Decision returned by GroupObject::decidePublish for a pending publish.
 */
enum class PublishDecision {
    Send,      ///< Transmit now (subject to any global rate limiter).
    Defer,     ///< Meaningful, but too soon — retry from the loop pump (coalesced).
    Suppress,  ///< Sub-threshold change — drop this publish.
};

/**
 * @brief Payload storage for one group object value.
 *
 * Bounded by config::MAX_GROUP_OBJECT_PAYLOAD_BYTES rather than the maximum
 * APDU: each object holds two of these, so the difference is ~500 bytes per
 * object of RAM that a typical 1- or 2-byte datapoint never uses.
 */
using RawValueBuffer = util::FixedVector<uint8_t, config::MAX_GROUP_OBJECT_PAYLOAD_BYTES>;

// The bound is derived from the datapoint catalog, not guessed. Adding a DPT
// wider than the buffer breaks the build here instead of failing at runtime
// with BufferTooSmall on a value the device is supposed to support.
static_assert(config::MAX_GROUP_OBJECT_PAYLOAD_BYTES >= maxCatalogPayloadOctets(),
              "MAX_GROUP_OBJECT_PAYLOAD_BYTES is too small for the widest catalog DPT");

/**
 * @brief KNX group object communication flags.
 *
 * The six flags below are the ones ETS shows on a communication object, and
 * they are the ones encoded in the Group Object Descriptor (KNX 03/05/01
 * §4.12.5.2.4.1, Table 52 — Group Object Table Realisation Type 7, which is
 * what this stack implements for mask 07B0).
 *
 * They are enforced behaviour, not documentation: the inbound dispatch path
 * consults them before touching an object's value or answering a read.
 */
struct GroupObjectFlags {
    /// Communication enable (C).  Master gate — when false the object is
    /// disconnected from the bus entirely, in both directions, regardless of
    /// every other flag.  ETS uses this to switch a channel off.
    bool communication{true};

    /// Read enable (R).  The device answers A_GroupValue_Read for this object.
    bool read{false};

    /// Write enable (W).  An inbound A_GroupValue_Write updates the value.
    bool write{false};

    /// Transmit enable (T).  The device may send this object on the bus.
    bool transmit{false};

    /// Response-Update enable (U).  An inbound A_GroupValue_Response — someone
    /// else's answer to a read — updates the value.  Distinct from W: an
    /// object can accept commands but ignore other devices' responses.
    bool update{false};

    /// Value Read on Initialisation (I).  After reset the device issues an
    /// A_GroupValue_Read to refresh this object.  System B feature; the spec
    /// warns it multiplies bus load after a whole-installation restart, so it
    /// stays off unless the product asks for it.
    bool readOnInit{false};

    /// Transmission priority for telegrams this object sends.
    Priority priority{Priority::Low};
};

/**
 * @brief Group object configuration
 */
struct GroupObjectConfig {
    GroupAddress address;      // Group address
    DptId dpt;                 // KNX datapoint type identifier
    GroupObjectFlags flags{};  // KNX communication flags (C/R/W/T/U/I + priority)
    GroupObjectTransmitPolicy transmitPolicy{}; // Send-on-change / cyclic / min-interval
};

/**
 * @brief Encode a value size in octets as a Table 53 "Value Field Type" code.
 *
 * Sub-octet datapoints use codes 0..6 (1..7 bits) and are produced by
 * valueFieldTypeForBits(); this overload covers the octet-sized entries.
 * Returns 0xFF for sizes the table cannot express.
 */
constexpr uint8_t valueFieldTypeForOctets(size_t octets) noexcept {
    switch (octets) {
        case 1:  return 7;
        case 2:  return 8;
        case 3:  return 9;
        case 4:  return 10;
        case 5:  return 15;
        case 6:  return 11;
        case 7:  return 16;
        case 8:  return 12;
        case 9:  return 17;
        case 10: return 13;
        case 11: return 18;
        case 12: return 19;
        case 13: return 20;
        case 14: return 14;
        case 15: return 21;
        case 16: return 22;
        default: break;
    }
    // From 17 octets the table is sequential: code = octets + 6, up to 248
    // octets (code 254).  Code 255 is the single out-of-sequence entry.
    if (octets >= 17 && octets <= 248) {
        return static_cast<uint8_t>(octets + 6u);
    }
    if (octets == 252) {
        return 255;
    }
    return 0xFFu;
}

/// Encode a 1..7 bit datapoint width as a Table 53 Value Field Type code.
constexpr uint8_t valueFieldTypeForBits(size_t bits) noexcept {
    return (bits >= 1 && bits <= 7) ? static_cast<uint8_t>(bits - 1u) : 0xFFu;
}

/**
 * @brief Build the 16-bit Group Object Descriptor of KNX 03/05/01 Table 52.
 *
 * Bit layout (MSB first): Response-Update, Transmit, Read-on-init, Write,
 * Read, Communication, 2-bit priority, then the 8-bit Value Field Type.
 */
constexpr uint16_t encodeGroupObjectDescriptor(const GroupObjectFlags& flags,
                                               uint8_t valueFieldType) noexcept {
    uint16_t descriptor = 0u;
    if (flags.update)        descriptor |= 0x8000u;
    if (flags.transmit)      descriptor |= 0x4000u;
    if (flags.readOnInit)    descriptor |= 0x2000u;
    if (flags.write)         descriptor |= 0x1000u;
    if (flags.read)          descriptor |= 0x0800u;
    if (flags.communication) descriptor |= 0x0400u;
    descriptor = static_cast<uint16_t>(
        descriptor | (static_cast<uint16_t>(static_cast<uint8_t>(flags.priority) & 0x03u) << 8));
    return static_cast<uint16_t>(descriptor | valueFieldType);
}

/// Inverse of encodeGroupObjectDescriptor(); the Value Field Type is returned
/// separately because it describes the datapoint, not the communication policy.
constexpr GroupObjectFlags decodeGroupObjectDescriptor(uint16_t descriptor,
                                                       uint8_t& valueFieldType) noexcept {
    GroupObjectFlags flags{};
    flags.update        = (descriptor & 0x8000u) != 0u;
    flags.transmit      = (descriptor & 0x4000u) != 0u;
    flags.readOnInit    = (descriptor & 0x2000u) != 0u;
    flags.write         = (descriptor & 0x1000u) != 0u;
    flags.read          = (descriptor & 0x0800u) != 0u;
    flags.communication = (descriptor & 0x0400u) != 0u;
    flags.priority      = static_cast<Priority>((descriptor >> 8) & 0x03u);
    valueFieldType      = static_cast<uint8_t>(descriptor & 0x00FFu);
    return flags;
}

/**
 * @brief Group object callback
 */
using GroupObjectCallback = util::InplaceFunction<void(const GroupAddress&, std::span<const uint8_t>), 64>;
using GroupReadRequestCallback = util::InplaceFunction<util::Result<void>(const GroupAddress&), 64>;

/**
 * @brief Group object
 * 
 * Represents a KNX group data point with value storage and communication flags.
 * 
 * @thread_safety Owner-context only. GroupObject is intentionally unsynchronized
 * and should be owned by a single runtime context, typically the BAU or
 * commissioned-product `loop()`.
 */
class GroupObject {
public:
    explicit GroupObject(const GroupObjectConfig& config);
    virtual ~GroupObject();

    DptId dptId() const noexcept { return _config.dpt; }

    /// Full KNX communication-flag set (C/R/W/T/U/I + priority).
    const GroupObjectFlags& flags() const noexcept { return _config.flags; }
    void setFlags(const GroupObjectFlags& flags) noexcept { _config.flags = flags; }

    /// Transmission priority used for telegrams this object originates.
    Priority priority() const noexcept { return _config.flags.priority; }

    // Individual flag accessors.  Each already folds in Communication enable,
    // so callers cannot accidentally honour R/W/T/U on a disabled object.
    bool communicationEnabled() const noexcept { return _config.flags.communication; }
    bool readable() const noexcept { return _config.flags.communication && _config.flags.read; }
    bool writable() const noexcept { return _config.flags.communication && _config.flags.write; }
    bool transmit() const noexcept { return _config.flags.communication && _config.flags.transmit; }
    bool updatable() const noexcept { return _config.flags.communication && _config.flags.update; }
    bool readOnInit() const noexcept { return _config.flags.communication && _config.flags.readOnInit; }

    /// Table 53 Value Field Type code for this object's current payload width.
    uint8_t valueFieldType() const noexcept;

    /// This object's entry in the Group Object Table (03/05/01 Table 52).
    uint16_t descriptor() const noexcept {
        return encodeGroupObjectDescriptor(_config.flags, valueFieldType());
    }
    
    /**
     * @brief Get group address
     */
    GroupAddress getAddress() const noexcept { return _config.address; }
    
    /**
     * @brief Get current value as bool
     * @thread_safety Owner-context only; do not call concurrently with mutation.
     */
    bool asBool() const;
    
    /**
     * @brief Get current value as uint8
     * @thread_safety Owner-context only; do not call concurrently with mutation.
     */
    uint8_t asUint8() const;
    
    /**
     * @brief Get current value as float
     * @thread_safety Owner-context only; do not call concurrently with mutation.
     */
    float asFloat() const;
    
    /**
     * @brief Set value from bool
     * @thread_safety Owner-context only.
     */
    util::Result<void> setValue(bool value);
    
    /**
     * @brief Set value from uint8
     * @thread_safety Owner-context only.
     */
    util::Result<void> setValue(uint8_t value);
    
    /**
     * @brief Set value from float
     * @thread_safety Owner-context only.
     */
    util::Result<void> setValue(float value);

    /**
     * @brief Set value from runtime DPT value
     * @thread_safety Owner-context only.
     */
    util::Result<void> setValue(const DptValue& value);

    /**
     * @brief Get current value decoded according to the configured DPT
     * @thread_safety Owner-context only; do not call concurrently with mutation.
     */
    util::Result<DptValue> value() const;
    
    /**
     * @brief Which inbound service delivered a value.
     *
     * The distinction is not cosmetic: Write enable (W) and Response-Update
     * enable (U) are separate KNX flags, so the object must know which one to
     * check before accepting the payload.
     */
    enum class ValueSource : uint8_t {
        Write,     ///< A_GroupValue_Write — gated by Write enable (W).
        Response,  ///< A_GroupValue_Response — gated by Response-Update enable (U).
    };

    /**
     * @brief Receive raw value (from KNX bus)
     *
     * Returns ErrorCode::AccessDenied when the object's communication flags do
     * not permit an update from this service.
     *
     * @thread_safety Owner-context only.
     */
    util::Result<void> receiveValue(std::span<const uint8_t> data,
                                    ValueSource source = ValueSource::Write);
    
    /**
     * @brief Get raw value
     * @thread_safety Owner-context only. The returned span aliases internal storage.
     */
    std::span<const uint8_t> getRawValue() const noexcept { return _rawValue.span(); }
    
    /**
     * @brief Set callback for value changes
     * @thread_safety Owner-context only - register during initialization.
     */
    void setCallback(GroupObjectCallback callback);

    /**
     * @brief Set sender used to issue group-value read requests
     * @thread_safety Owner-context only - register during initialization.
     */
    void setReadRequestSender(GroupReadRequestCallback callback);
    
    /**
     * @brief Request current value from bus
     * @thread_safety Owner-context only.
     */
    util::Result<void> requestValue();
    
    /**
     * @brief Check if value is valid
     * @thread_safety Owner-context only; do not call concurrently with mutation.
     */
    bool isValid() const noexcept { return _valid; }

    // ── Outbound transmit policy (send-on-change / cyclic / min-interval) ─────
    // Decision helpers are pure functions of the current value, the last
    // transmitted value, and a caller-supplied monotonic `nowMs` — no internal
    // clock, so they are fully deterministic and host-testable. The publish path
    // (see BusAccessUnit::sendGroupValueForObject) evaluates them and calls
    // noteTransmitted() when a telegram actually goes on the wire.

    void setTransmitPolicy(const GroupObjectTransmitPolicy& policy) noexcept { _config.transmitPolicy = policy; }
    const GroupObjectTransmitPolicy& transmitPolicy() const noexcept { return _config.transmitPolicy; }

    /// Decide what to do with a publish of the current value at `nowMs`.
    /// @thread_safety Owner-context only.
    PublishDecision decidePublish(uint32_t nowMs) const;

    /// True when a cyclic/heartbeat re-send is due at `nowMs`.
    bool dueForCyclic(uint32_t nowMs) const noexcept;

    /// True when a previously deferred publish is now clear of the min-interval.
    bool readyToSendDeferred(uint32_t nowMs) const noexcept;

    bool pendingSend() const noexcept { return _pendingSend; }
    void setPendingSend(bool pending) noexcept { _pendingSend = pending; }

    /// Snapshot the current value as "last transmitted" and stamp the time.
    /// Call exactly when a telegram for this object is actually emitted.
    /// @thread_safety Owner-context only.
    void noteTransmitted(uint32_t nowMs);

private:
    /// DPT-aware "did the value move enough to count as a change?" comparison
    /// between the current value and the last transmitted value.
    bool changedSignificantly() const;

    GroupObjectConfig _config;
    RawValueBuffer _rawValue;
    bool _valid;
    GroupObjectCallback _callback;
    GroupReadRequestCallback _readRequestSender;

    // Transmit-policy state.
    RawValueBuffer _lastSentRaw;
    bool     _lastSentValid{false};
    uint32_t _lastSentMs{0};
    bool     _pendingSend{false};
};

} // namespace application
} // namespace knx
