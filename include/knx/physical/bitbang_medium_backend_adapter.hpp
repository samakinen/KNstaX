// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bitbang_medium_backend_adapter.hpp
 * @brief TP1 medium backend adapter for BitBangDriverInterface
 */

#pragma once

#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/physical/tp1_medium_backend.hpp"

#include <span>

namespace knx {
namespace physical {

class BitBangMediumBackendAdapter : public Tp1MediumBackend,
                                    public Tp1MediumDiagnostics {
public:
    BitBangMediumBackendAdapter(BitBangDriverInterface& driver, BitBangDriverTp1Interface& tp1Driver);

    util::Result<void> init(const Tp1MediumConfig& config) override;
    void close() override;

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override;

    void setEventCallback(Tp1EventCallback callback, void* context) override;

    Tp1MediumState getState() const override;
    Tp1CapabilityProfile getCapabilities() const override;
    LinkState getLinkState() const override;

    util::Result<void> setBusMonitorMode(bool enabled) override;
    util::Result<void> service() override;
    util::Result<void> setOwnAddress(uint16_t addressRaw) override;

    void setAckGroupAddresses(std::span<const uint16_t> addresses) override;
    void setLocalBusy(bool busy) override;

    Tp1MediumDiagnostics* diagnostics() override { return this; }
    const Tp1MediumDiagnostics* diagnostics() const override { return this; }

    util::Result<void> requestDiagnosticsSnapshot() override;

    void setQueueNotifyCallback(QueueNotifyCallback callback, void* context);

private:
    static void rxShim(std::span<const uint8_t> frame, void* context);
    void onRxFrame(std::span<const uint8_t> frame);

    static Tp1MediumState mapState(DriverState state);

    BitBangDriverInterface& _driver;
    BitBangDriverTp1Interface& _tp1Driver;
    Tp1EventCallback _eventCallback;
    void* _eventContext;
    Tp1MediumConfig _config;
    bool _initialized;
};

} // namespace physical
} // namespace knx
