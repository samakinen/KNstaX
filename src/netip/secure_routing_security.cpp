// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_routing_security.hpp"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/security/data_secure.hpp"
#include "knx/security/secure_memory.hpp"
#include "knx/util/log.hpp"
#include "knx/util/timing_utils.hpp"

#include <algorithm>

namespace knx {
namespace netip {
namespace ip_secure {

static const char* TAG = "KNX.SecureRouting";

namespace {

std::array<uint8_t, SecureWrapper::kSeqLen> encodeTimer(uint64_t timer)
{
    std::array<uint8_t, SecureWrapper::kSeqLen> out{};
    knx::security::writeSecureU48BE(out, timer);
    return out;
}

uint64_t decodeTimer(const std::array<uint8_t, SecureWrapper::kSeqLen>& in)
{
    return knx::security::readSecureU48BE(in);
}

} // namespace

std::array<uint8_t, 4> SecureRoutingSecurity::keyCheckValue(security::Aes128BlockCipher& keyedCipher)
{
    std::array<uint8_t, security::Aes128BlockCipher::kBlockSize> block{};
    block.fill(0xFF);
    std::array<uint8_t, security::Aes128BlockCipher::kBlockSize> out{};
    std::array<uint8_t, 4> check{};
    if (keyedCipher.encryptBlock(block.data(), out.data()).isOk()) {
        std::copy_n(out.begin(), check.size(), check.begin());
    }
    security::secureZero(out);
    return check;
}

SecureRoutingSecurity::SecureRoutingSecurity(const Key& groupKey, const Serial& serial, SecureRoutingOptions options)
    : groupKey_(groupKey)
    , serial_(serial)
    , options_(std::move(options))
{
    (void)cipher_.setKey(groupKey_);
    keyCheck_ = keyCheckValue(cipher_);

    sync_ = std::make_unique<McTimerSync>(serial_, [this]() { return randomU32(options_.random); });
    sync_->setLatencyTolerance(options_.latencyToleranceMs, options_.syncLatencyFraction);

    // §2.2.2.3.2.8: resume from the stored value plus the worst-case offset D
    // — the store may have been written at the very start of the interval
    // that power was lost at the end of (§2.2.4.2). Starting from 0 is only
    // acceptable the first time this key is ever used, which is what a record
    // under a different key check means.
    uint64_t initial = 0;
    if (options_.store != nullptr) {
        if (const auto record = options_.store->load();
            record && security::constantTimeEqual(record->keyCheck, keyCheck_)) {
            initial = (record->timer + kPersistIntervalMs) & McTimerSync::kTimerMask;
        }
    }
    const uint32_t now = nowMs();
    sync_->start(now, initial, options_.startReason);

    // Written at once rather than at the first send: the next restart then
    // resumes from initial + D, above anything this run can send before its
    // first own write.
    std::lock_guard<std::mutex> lock(mutex_);
    persist(initial);
}

SecureRoutingSecurity::~SecureRoutingSecurity()
{
    security::secureZero(groupKey_);
}

uint32_t SecureRoutingSecurity::nowMs() const
{
    return util::nowMs(options_.timing);
}

void SecureRoutingSecurity::persist(uint64_t timer)
{
    if (options_.store == nullptr) {
        return;
    }
    if (options_.store->save(McTimerStore::Record{timer, keyCheck_}).isError()) {
        // Sending on regardless: refusing to transmit because flash is failing
        // turns a storage fault into a dead device. The next frame retries.
        KNX_LOGW(TAG, "Could not persist mc_timer; a restart now could reuse timer values");
        return;
    }
    persisted_ = true;
    persistedTimer_ = timer;
}

void SecureRoutingSecurity::persistIfBeyondInterval(uint64_t timer)
{
    if (options_.store == nullptr) {
        return;
    }
    if (!persisted_ || timer > persistedTimer_ + kPersistIntervalMs) {
        persist(timer);
    }
}

SecureRoutingSecurity::Tag SecureRoutingSecurity::ownTag(uint64_t timer, Tag proposed)
{
    // The nonce is timer | serial | tag, and the timer is the same for every
    // frame sent within one millisecond. Counting up from the last tag inside
    // that millisecond keeps them distinct; a fresh millisecond takes the
    // random draw, as §2.2.2.4.2 asks.
    if (hasLastOwn_ && timer == lastOwnTimer_) {
        const uint16_t next = static_cast<uint16_t>(((lastOwnTag_[0] << 8) | lastOwnTag_[1]) + 1u);
        proposed = {static_cast<uint8_t>(next >> 8), static_cast<uint8_t>(next)};
    }
    hasLastOwn_ = true;
    lastOwnTimer_ = timer;
    lastOwnTag_ = proposed;
    return proposed;
}

util::Result<size_t> SecureRoutingSecurity::protect(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const uint64_t timer = sync_->onTransmit(nowMs());  // E09
    persistIfBeyondInterval(timer);

    const uint32_t draw = randomU32(options_.random);
    const Tag tag = ownTag(timer, Tag{static_cast<uint8_t>(draw >> 8), static_cast<uint8_t>(draw)});

    const std::array<uint8_t, 4> counterSuffix{tag[0], tag[1], 0xFF, 0x00};
    return SecureWrapper::wrap(cipher_,
                               SessionId::invalid(),
                               encodeTimer(timer),
                               serial_,
                               tag,
                               counterSuffix,
                               in,
                               out);
}

util::Result<size_t> SecureRoutingSecurity::unprotect(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // MAC first (§2.2.2.4.4): an unauthenticated timer value must not move the
    // local timer or schedule anything.
    SecureWrapper::Parsed parsed{};
    auto unwrapRes = SecureWrapper::unwrapAndVerify(cipher_, SessionId::invalid(), in, out, &parsed);
    if (unwrapRes.isError()) return unwrapRes.error();

    const uint32_t now = nowMs();
    const uint64_t received = decodeTimer(parsed.seq);
    if (!sync_->onSecureWrapper(received, parsed.serial, parsed.tag, now)) {
        // E08: older than the latency tolerance. Authentic but stale is
        // exactly what a replay looks like.
        security::secureZero(out.first(unwrapRes.value()));
        ++outdatedDiscarded_;
        return util::ErrorCode::OutOfRange;
    }
    persistIfBeyondInterval(sync_->timer(now));
    return unwrapRes;
}

util::Result<void> SecureRoutingSecurity::handleTimerNotify(std::span<const uint8_t> datagram)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto decoded = SecureWrapper::decodeTimerNotify(cipher_, datagram);
    if (decoded.isError()) return decoded.error();

    const auto& notify = decoded.value();
    const uint32_t now = nowMs();
    sync_->onTimerNotify(decodeTimer(notify.timer), notify.serial, notify.tag, now);
    persistIfBeyondInterval(sync_->timer(now));
    return util::Result<void>::ok();
}

util::Result<size_t> SecureRoutingSecurity::takeDueTimerNotify(std::span<uint8_t> out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto due = sync_->poll(nowMs());  // E10
    if (!due) {
        return size_t{0};
    }
    persistIfBeyondInterval(due->timer);

    SecureWrapper::TimerNotify notify{};
    notify.timer = encodeTimer(due->timer);
    notify.serial = due->serial;
    // An update notify repeats the outdated frame's serial and tag (A6); only
    // an own notify (A5) shares this device's nonce space.
    notify.tag = due->update ? due->tag : ownTag(due->timer, due->tag);
    return SecureWrapper::encodeTimerNotify(cipher_, notify, out);
}

uint32_t SecureRoutingSecurity::millisecondsUntilTimerNotify()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sync_->millisecondsUntilNotify(nowMs());
}

void SecureRoutingSecurity::setLatencyTolerance(uint16_t latencyToleranceMs, uint8_t syncLatencyFraction)
{
    std::lock_guard<std::mutex> lock(mutex_);
    options_.latencyToleranceMs = latencyToleranceMs;
    options_.syncLatencyFraction = syncLatencyFraction;
    sync_->setLatencyTolerance(latencyToleranceMs, syncLatencyFraction);
}

bool SecureRoutingSecurity::usesKey(std::span<const uint8_t> key) const
{
    return security::constantTimeEqual(groupKey_, key);
}

void SecureRoutingSecurity::restartSync()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t now = nowMs();
    // The running timer, not the stored one: it is valid for this key and at
    // least as new as anything persisted, so restarting from it loses nothing
    // and adds no needless D (see the header).
    sync_->start(now, sync_->timer(now), McTimerSync::StartReason::Reconfigured);
}

uint64_t SecureRoutingSecurity::mcTimer()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sync_->timer(nowMs());
}

uint64_t SecureRoutingSecurity::outdatedDiscarded() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return outdatedDiscarded_;
}

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
