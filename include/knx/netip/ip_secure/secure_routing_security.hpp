// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/routing_security.hpp"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/mc_timer_store.hpp"
#include "knx/netip/ip_secure/mc_timer_sync.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/random_source.hpp"
#include "knx/platform/platform.hpp"
#include "knx/security/aes_block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace knx {
namespace netip {
namespace ip_secure {

/// Everything a Secure Routing context needs besides its key and serial.
struct SecureRoutingOptions {
    /// Clock for mc_timer. Null uses the host's steady clock.
    platform::TimingPlatform* timing{nullptr};
    /// Tags and notify delays. Empty uses the fallback in random_source.hpp.
    RandomBytesSource random{};
    /// Where mc_timer survives a restart. Null keeps it in RAM only, which
    /// is acceptable in a test and nowhere else (03_08_09 §2.2.4.2).
    McTimerStore* store{nullptr};
    /// PowerUp delays the first TIMER_NOTIFY by random(0, 10 s); Reconfigured
    /// (a new backbone key) sends it at once (§2.2.2.3.2.8).
    McTimerSync::StartReason startReason{McTimerSync::StartReason::PowerUp};
    uint16_t latencyToleranceMs{McTimerSync::kDefaultLatencyToleranceMs};
    uint8_t syncLatencyFraction{McTimerSync::kDefaultSyncLatencyFraction};
};

/**
 * @brief KNX IP Secure Routing for one backbone key (03_08_09 §2.2.2).
 *
 * Wraps outbound KNXnet/IP datagrams in a SECURE_WRAPPER (0950h) and unwraps
 * inbound ones, with the sequence information taken from the multicast timer
 * rather than from a counter:
 *
 *  - the nonce is mc_timer | serial | tag. The serial is the device's KNX
 *    serial number; the tag is random per frame, and two frames stamped in the
 *    same millisecond get different tags, so no nonce repeats while mc_timer
 *    never runs backwards — which is what McTimerStore is for;
 *  - an inbound frame's MAC is checked first and only then its timer, which
 *    either moves the local timer forward or, when the frame is older than the
 *    latency tolerance, discards it as a replay (E08);
 *  - TIMER_NOTIFY frames are produced and consumed here (via RoutingSecurity),
 *    so a quiet group stays synchronised.
 *
 * One instance lives exactly as long as its key: a new key is a new context,
 * which is E11 — restart the synchronisation with mc_timer = 0.
 *
 * @thread_safety Thread-safe. protect() runs on the sending task and the rest
 *                on the receiving one; an internal mutex serialises them.
 */
class SecureRoutingSecurity final : public RoutingSecurity {
public:
    using Key = SecureWrapper::Key;
    using Serial = std::array<uint8_t, SecureWrapper::kSerialLen>;
    using Tag = std::array<uint8_t, SecureWrapper::kTagLen>;

    /// D of §2.2.4.2: the longest stretch of mc_timer time between two writes
    /// of the stored value. "Maximum one hour".
    static constexpr uint64_t kPersistIntervalMs = 3'600'000ULL;

    SecureRoutingSecurity(const Key& groupKey, const Serial& serial, SecureRoutingOptions options = {});
    ~SecureRoutingSecurity() override;

    SecureRoutingSecurity(const SecureRoutingSecurity&) = delete;
    SecureRoutingSecurity& operator=(const SecureRoutingSecurity&) = delete;

    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override;
    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override;

    util::Result<void> handleTimerNotify(std::span<const uint8_t> datagram) override;
    util::Result<size_t> takeDueTimerNotify(std::span<uint8_t> out) override;
    uint32_t millisecondsUntilTimerNotify() override;

    /// PID_MULTICAST_LATENCY_TOLERANCE / PID_SYNC_LATENCY_FRACTION changed.
    void setLatencyTolerance(uint16_t latencyToleranceMs, uint8_t syncLatencyFraction);

    /// True when this context was built for `key`. Used to tell a rekey (E11)
    /// from the same key written again, which is not an event.
    [[nodiscard]] bool usesKey(std::span<const uint8_t> key) const;

    /// Current mc_timer. Diagnostic, and asserted in tests.
    [[nodiscard]] uint64_t mcTimer();

    /// Inbound SECURE_WRAPPERs discarded as outdated (E08).
    [[nodiscard]] uint64_t outdatedDiscarded() const;

    /// The check value stored with mc_timer: the first four octets of the key
    /// applied to an all-FFh block, a block no CCM step under this key ever
    /// encrypts (a counter block ends in FF00h and B0 in the payload length).
    static std::array<uint8_t, 4> keyCheckValue(security::Aes128BlockCipher& keyedCipher);

private:
    uint32_t nowMs() const;
    /// §2.2.4.2: write mc_timer before a value beyond T + D leaves the device,
    /// and as soon as one beyond it has been adopted from the group.
    void persistIfBeyondInterval(uint64_t timer);
    void persist(uint64_t timer);
    /// A tag no earlier own frame used with the same timer value.
    Tag ownTag(uint64_t timer, Tag proposed);

    mutable std::mutex mutex_;
    Key groupKey_{};
    Serial serial_{};
    SecureRoutingOptions options_;
    security::Aes128BlockCipher cipher_;
    std::array<uint8_t, 4> keyCheck_{};
    std::unique_ptr<McTimerSync> sync_;

    bool persisted_{false};
    uint64_t persistedTimer_{0};

    bool hasLastOwn_{false};
    uint64_t lastOwnTimer_{0};
    Tag lastOwnTag_{};

    uint64_t outdatedDiscarded_{0};
};

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
