// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file link_monitor.hpp
 * @brief Medium-neutral debounce state machine for hardware link-health signals
 *
 * Pure logic, no platform dependencies: samples go in, debounced LinkStatus
 * transitions come out. Shared by every backend that has such a signal (TP1
 * bitbang KNX_OK, TPUART SAVE, an IP link-up flag) so the debounce semantics
 * are identical regardless of what produced the sample.
 *
 * Sampling rather than edge-latching is deliberate. The edge interrupt exists
 * only to wake the consumer promptly (and, optionally, to fire the pre-debounce
 * power-fail hook); the state itself is derived from levels read at poll time,
 * which cannot be desynchronised by a missed or bouncing edge.
 */

#pragma once

#include "knx/physical/link_state.hpp"

namespace knx {
namespace physical {

class LinkMonitor {
public:
    void configure(const LinkSignalConfig& config) {
        _config = config;
        reset();
    }

    void reset() {
        _state = LinkState::Unknown;
        _seeded = false;
        _candidate = LinkState::Unknown;
        _candidateSinceUs = 0;
        _transitionCount = 0;
        _lastSampleUs = 0;
    }

    [[nodiscard]] bool isConfigured() const { return _config.isConfigured(); }
    [[nodiscard]] LinkState state() const { return _state; }
    [[nodiscard]] uint32_t transitionCount() const { return _transitionCount; }

    [[nodiscard]] LinkStatus status() const {
        LinkStatus out;
        out.state = _state;
        out.kind = LinkEventKind::StateChanged;
        out.transitionCount = _transitionCount;
        out.timestampUs = _lastSampleUs;
        return out;
    }

    /**
     * @brief Translate a raw pin level into the asserted/healthy sense
     */
    [[nodiscard]] bool levelMeansUp(int rawLevel) const {
        return (rawLevel != 0) == _config.activeHigh;
    }

    /**
     * @brief Feed one sample of the signal
     * @param up       Signal currently reads healthy
     * @param nowUs    Sample timestamp in the medium's time domain
     * @param outStatus Receives the new status when the return value is true
     * @return true when this sample produced a debounced transition
     *
     * The first sample after configure()/reset() is adopted immediately: at
     * init there is no prior state to debounce against, and starting from the
     * real level avoids reporting a spurious transition on the first poll.
     */
    bool sample(bool up, uint64_t nowUs, LinkStatus& outStatus) {
        if (!_config.isConfigured()) {
            return false;
        }

        _lastSampleUs = nowUs;
        const LinkState observed = up ? LinkState::Up : LinkState::Down;

        if (!_seeded) {
            _seeded = true;
            _state = observed;
            _candidate = observed;
            _candidateSinceUs = nowUs;
            outStatus = status();
            return true;
        }

        if (observed == _state) {
            // Signal returned to the committed state before its window expired.
            _candidate = observed;
            _candidateSinceUs = nowUs;
            return false;
        }

        if (observed != _candidate) {
            _candidate = observed;
            _candidateSinceUs = nowUs;
            return false;
        }

        const uint32_t window = (observed == LinkState::Down) ? _config.downDebounceUs
                                                              : _config.upDebounceUs;
        if ((nowUs - _candidateSinceUs) < static_cast<uint64_t>(window)) {
            return false;
        }

        _state = observed;
        ++_transitionCount;
        outStatus = status();
        return true;
    }

    /**
     * @brief Whether a change is currently waiting out its debounce window
     *
     * Consumers poll on their own cadence; this tells them a sample is still
     * owed even if nothing else is happening on the medium.
     */
    [[nodiscard]] bool hasPendingChange() const {
        return _seeded && _candidate != _state;
    }

    /**
     * @brief Microseconds until the pending change would commit (0 if none)
     *
     * A consumer whose poll cadence is slower than the debounce window uses
     * this to schedule the one extra sample the decision needs, instead of
     * letting the commit slip to its next scheduled poll.
     */
    [[nodiscard]] uint64_t pendingRemainingUs(uint64_t nowUs) const {
        if (!hasPendingChange()) {
            return 0;
        }

        const uint32_t window = (_candidate == LinkState::Down) ? _config.downDebounceUs
                                                                : _config.upDebounceUs;
        const uint64_t elapsed = nowUs - _candidateSinceUs;
        return (elapsed >= window) ? 0 : (static_cast<uint64_t>(window) - elapsed);
    }

private:
    LinkSignalConfig _config{};
    LinkState _state{LinkState::Unknown};
    LinkState _candidate{LinkState::Unknown};
    bool _seeded{false};
    uint64_t _candidateSinceUs{0};
    uint64_t _lastSampleUs{0};
    uint32_t _transitionCount{0};
};

} // namespace physical
} // namespace knx
