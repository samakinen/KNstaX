// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/uart_hal.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace knx {
namespace physical {

/**
 * @brief Host implementation of knx_uart_hal_t for tests
 *
 * Stands in for a board's UART. The test (or a simulated transceiver) plays
 * the far end: it delivers received octets, which reach the RX callback
 * synchronously as if from the receive interrupt, and it decides when queued
 * TX octets leave the wire, so the TX buffer fills and drains under its
 * control.
 *
 * Time is a counter the test advances; deliverRx() stamps octets with it.
 *
 * @thread_safety NOT thread-safe. Tests drive it from one thread; the RX
 * callback runs inside deliverRx(), and a write() from it is recorded as made
 * from interrupt context.
 */
class UartHalVirtual {
public:
    struct Pins {
        bool reset{false};     ///< Provide set_reset
        bool save{false};      ///< Provide read_save
        bool saveEdge{false};  ///< Provide the SAVE edge interrupt (with save)
    };

    explicit UartHalVirtual(size_t txCapacity = DEFAULT_TX_CAPACITY);

    UartHalVirtual(const UartHalVirtual&) = delete;
    UartHalVirtual& operator=(const UartHalVirtual&) = delete;
    UartHalVirtual(UartHalVirtual&&) = delete;
    UartHalVirtual& operator=(UartHalVirtual&&) = delete;

    /// Fill outHal with this instance's operations. The optional pins are
    /// present only when pins says so; the one-argument form binds neither.
    void bind(knx_uart_hal_t& outHal, Pins pins);
    void bind(knx_uart_hal_t& outHal) { bind(outHal, Pins{}); }

    // --- Far end ----------------------------------------------------------

    /// Deliver one received octet to the RX callback, stamped with nowUs().
    /// Returns false, and delivers nothing, while the port is closed.
    bool deliverRx(uint8_t octet, uint8_t flags = 0);

    /// Move up to maxOctets queued TX octets onto the wire, oldest first, and
    /// return them. They are also appended to transmitted().
    std::vector<uint8_t> transmit(size_t maxOctets);
    std::vector<uint8_t> transmitAll();

    /// Called after every accepted write(), from inside it (and so possibly
    /// from inside the RX callback). A simulated far end uses it to start
    /// clocking octets out without polling. Null clears it.
    using TxListener = void (*)(void* context);
    void setTxListener(TxListener listener, void* context)
    {
        _txListener = listener;
        _txListenerContext = context;
    }

    // --- Observation ------------------------------------------------------

    bool isOpen() const { return _open; }
    std::optional<knx_uart_hal_config_t> config() const { return _config; }
    size_t txCapacity() const { return _txCapacity; }
    size_t txQueued() const { return _txQueue.size(); }
    const std::vector<uint8_t>& transmitted() const { return _transmitted; }
    void clearTransmitted() { _transmitted.clear(); }

    size_t acceptedWrites() const { return _acceptedWrites; }
    size_t rejectedWrites() const { return _rejectedWrites; }
    /// Accepted writes made from inside the RX callback.
    size_t writesFromRxCallback() const { return _writesFromRxCallback; }
    size_t openCount() const { return _openCount; }

    bool resetAsserted() const { return _resetAsserted; }
    /// Change SAVE. A change calls the installed SAVE edge callback, stamped
    /// with nowUs(), synchronously, as if from the edge interrupt.
    void setSave(bool asserted);
    bool saveEdgeInstalled() const { return _saveEdgeCb != nullptr; }

    // --- Time -------------------------------------------------------------

    uint64_t nowUs() const { return _nowUs; }
    void setNowUs(uint64_t nowUs) { _nowUs = nowUs; }
    void advanceUs(uint64_t deltaUs) { _nowUs += deltaUs; }

    static constexpr size_t DEFAULT_TX_CAPACITY = 64;

private:
    size_t _txCapacity;
    bool _open{false};
    std::optional<knx_uart_hal_config_t> _config;
    knx_uart_hal_rx_cb_t _rxCb{nullptr};
    void* _rxContext{nullptr};
    bool _inRxCallback{false};
    TxListener _txListener{nullptr};
    void* _txListenerContext{nullptr};

    std::deque<uint8_t> _txQueue;
    std::vector<uint8_t> _transmitted;
    size_t _acceptedWrites{0};
    size_t _rejectedWrites{0};
    size_t _writesFromRxCallback{0};
    size_t _openCount{0};

    bool _resetAsserted{false};
    bool _saveAsserted{false};
    knx_uart_hal_save_edge_cb_t _saveEdgeCb{nullptr};
    void* _saveEdgeContext{nullptr};
    uint64_t _nowUs{0};

    static bool openShim(void* context,
                         const knx_uart_hal_config_t* config,
                         knx_uart_hal_rx_cb_t rx_cb,
                         void* rx_context);
    static void closeShim(void* context);
    static bool writeShim(void* context, const uint8_t* data, size_t len);
    static size_t txFreeShim(void* context);
    static bool setResetShim(void* context, bool asserted);
    static bool readSaveShim(void* context);
    static bool installSaveEdgeShim(void* context, knx_uart_hal_save_edge_cb_t cb, void* save_context);
    static void removeSaveEdgeShim(void* context);
};

} // namespace physical
} // namespace knx
