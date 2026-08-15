// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file null_tp1_medium_backend.hpp
 * @brief NullTp1MediumBackend — a no-op TP1 medium backend for host testing and bringup.
 *
 * Use this backend whenever you need a compilable, link-able KNX stack instance
 * without real TP1 hardware.  It silently discards transmitted frames and never
 * generates any receive events.
 *
 * Typical uses:
 * - Host-side unit / integration tests that exercise protocol logic but not the
 *   physical medium.
 * - Initial bring-up of new hardware before a real UART/bitbang driver is ready.
 * - Examples and documentation code that want to focus on application logic.
 *
 * Example:
 *
 *   #include "knx/physical/null_tp1_medium_backend.hpp"
 *   ...
 *   device.start(platform, std::make_unique<knx::physical::NullTp1MediumBackend>());
 */

#pragma once

#include "knx/physical/tp1_medium_backend.hpp"

#include <cstdint>
#include <span>

namespace knx {
namespace physical {

/**
 * @brief No-op TP1 medium backend.
 *
 * All transmitted frames are silently discarded.  No receive events are ever
 * emitted.  All operations return success so the stack initialises cleanly.
 */
class NullTp1MediumBackend final : public Tp1MediumBackend {
public:
    NullTp1MediumBackend() = default;
    ~NullTp1MediumBackend() override = default;

    NullTp1MediumBackend(const NullTp1MediumBackend&) = delete;
    NullTp1MediumBackend& operator=(const NullTp1MediumBackend&) = delete;

    util::Result<void> init(const Tp1MediumConfig& /*config*/) override
    {
        state_ = Tp1MediumState::Idle;
        return util::Result<void>::ok();
    }

    void close() override
    {
        state_ = Tp1MediumState::Uninitialized;
    }

    /// Silently discards the frame.
    util::Result<size_t> sendFrame(std::span<const uint8_t> /*frame*/) override
    {
        return 0;
    }

    void setEventCallback(Tp1EventCallback callback, void* context) override
    {
        callback_ = std::move(callback);
        callbackContext_ = context;
    }

    Tp1MediumState getState() const override { return state_; }

    Tp1CapabilityProfile getCapabilities() const override { return {}; }

    util::Result<void> setBusMonitorMode(bool /*enabled*/) override
    {
        return util::Result<void>::ok();
    }

    util::Result<void> service() override
    {
        return util::Result<void>::ok();
    }

private:
    Tp1MediumState state_{Tp1MediumState::Uninitialized};
    Tp1EventCallback callback_{};
    void* callbackContext_{nullptr};
};

} // namespace physical
} // namespace knx
