// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file telegram_rate_limiter.hpp
 * @brief Reusable outbound-telegram rate limiter for protecting the shared bus.
 */

#pragma once

#include <cstdint>

namespace knx {
namespace application {

/**
 * @brief Rate-limit configuration for unsolicited outbound telegrams.
 *
 * Two independent constraints, either of which may be disabled with 0:
 *  - `maxTelegrams` per `perWindowMs`: a token bucket refilled once per window.
 *  - `minGapMs`: a minimum spacing between any two consecutive sends.
 *
 * The default (both limits 0) is fully open, so a limiter left unconfigured
 * never changes behaviour.
 */
struct TelegramRateLimitConfig {
    uint32_t maxTelegrams{0};    ///< Max sends per window; 0 = no window limit.
    uint32_t perWindowMs{1000};  ///< Token-bucket window length (ms).
    uint32_t minGapMs{0};        ///< Minimum gap between sends (ms); 0 = none.
};

/**
 * @brief Token-bucket + minimum-gap limiter for outbound group telegrams.
 *
 * Purpose is bus citizenship: TP1 is a shared ~50 telegram/s medium, so a
 * chatty device must throttle its own unsolicited traffic. This shapes only the
 * caller's own sends; protocol-mandated responses (L_ACK, GroupValue_Response)
 * must bypass it and stay timely.
 *
 * All timing is caller-supplied (`nowMs`), so the limiter is deterministic and
 * host-testable with no clock dependency.
 *
 * @thread_safety Owner-context only; drive from a single runtime context.
 */
class TelegramRateLimiter {
public:
    TelegramRateLimiter() = default;
    explicit TelegramRateLimiter(const TelegramRateLimitConfig& config) { configure(config); }

    void configure(const TelegramRateLimitConfig& config) {
        _config = config;
        reset();
    }

    const TelegramRateLimitConfig& config() const noexcept { return _config; }

    /// Discards accumulated window/gap history; the next send is allowed
    /// immediately (subject to the configured limits from a fresh window).
    void reset() noexcept {
        _initialized = false;
        _hasSent = false;
        _tokens = _config.maxTelegrams;
        _windowStartMs = 0;
        _lastSendMs = 0;
    }

    /// True when no limit is configured (the fast path).
    bool unlimited() const noexcept {
        return _config.maxTelegrams == 0u && _config.minGapMs == 0u;
    }

    /// Whether a send would be permitted at `nowMs`, without consuming a token.
    bool allowed(uint32_t nowMs) const noexcept {
        if (unlimited()) {
            return true;
        }
        if (_config.minGapMs != 0u && _hasSent && (nowMs - _lastSendMs) < _config.minGapMs) {
            return false;
        }
        if (_config.maxTelegrams != 0u) {
            const bool windowElapsed = _initialized && (nowMs - _windowStartMs) >= _config.perWindowMs;
            const uint32_t availableTokens = (!_initialized || windowElapsed) ? _config.maxTelegrams : _tokens;
            if (availableTokens == 0u) {
                return false;
            }
        }
        return true;
    }

    /// Attempt to consume one send slot at `nowMs`. Returns true and records the
    /// send when permitted; false (no state change) when a limit is hit.
    bool tryConsume(uint32_t nowMs) noexcept {
        if (unlimited()) {
            return true;
        }
        if (!_initialized) {
            _windowStartMs = nowMs;
            _tokens = _config.maxTelegrams;
            _initialized = true;
        }
        if (_config.minGapMs != 0u && _hasSent && (nowMs - _lastSendMs) < _config.minGapMs) {
            return false;
        }
        if (_config.maxTelegrams != 0u) {
            if ((nowMs - _windowStartMs) >= _config.perWindowMs) {
                _windowStartMs = nowMs;
                _tokens = _config.maxTelegrams;
            }
            if (_tokens == 0u) {
                return false;
            }
            --_tokens;
        }
        _lastSendMs = nowMs;
        _hasSent = true;
        return true;
    }

private:
    TelegramRateLimitConfig _config{};
    bool     _initialized{false};
    bool     _hasSent{false};
    uint32_t _tokens{0};
    uint32_t _windowStartMs{0};
    uint32_t _lastSendMs{0};
};

} // namespace application
} // namespace knx
