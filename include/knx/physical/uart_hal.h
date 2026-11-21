// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/*
 * Byte-stream UART HAL for transceivers with a host UART (TP-UART2, NCN5120).
 *
 * A C interface so a board can implement it without C++. The stack side is
 * the TP-UART medium backend; the board side is one serial port plus, where
 * the board wires them, the transceiver's reset input and SAVE output.
 *
 * Concurrency model
 * -----------------
 * Two contexts use a HAL instance:
 *
 *   - Interrupt context. The HAL calls the RX callback from its receive
 *     interrupt, once per octet, in the order the octets arrived. The callback
 *     may call write() (the TP-UART acknowledge decision is made there), so
 *     write() must be safe to call from inside the RX callback. Where the
 *     board gives SAVE an edge interrupt, the SAVE edge callback runs there
 *     too and may call read_save().
 *   - Main context. open(), close(), write(), tx_free() and the pin functions.
 *     The stack calls them from one thread only (the cooperative main loop on
 *     bare metal, the stack's task elsewhere).
 *
 * write() is all-or-nothing and atomic with respect to the other context: the
 * octets of one call leave the UART contiguously, never interleaved with the
 * octets of a call made from the other context. A board typically gets this
 * by masking its UART interrupts while it copies into the TX ring.
 *
 * Nothing here allocates, and the RX callback must not block. On targets that
 * run interrupt code from RAM, the board places its receive interrupt there;
 * the stack does the same for the callback (KNX_ISR_RAM).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KNX_UART_HAL_PARITY_NONE = 0,
    KNX_UART_HAL_PARITY_EVEN = 1,
    KNX_UART_HAL_PARITY_ODD = 2,
} knx_uart_hal_parity_t;

/* Error flags passed with a received octet. A board reports what its UART
 * detects and leaves the other bits clear. */
enum {
    KNX_UART_HAL_RX_PARITY_ERROR = 0x01u,  /* the octet failed its parity check */
    KNX_UART_HAL_RX_FRAMING_ERROR = 0x02u, /* no valid stop bit; the octet is suspect */
    KNX_UART_HAL_RX_NOISE = 0x04u,         /* noise detected on a sample; the octet is suspect */
    KNX_UART_HAL_RX_OVERRUN = 0x08u,       /* one or more octets before this one were lost */
};

/* Line format: always 8 data bits and 1 stop bit. TP-UART2 and NCN5120 use
 * 8E1 (TP-UART2 technical manual, "UART interface"). */
typedef struct {
    uint32_t baud;
    knx_uart_hal_parity_t parity;
} knx_uart_hal_config_t;

/* Called from the receive interrupt, once per octet.
 *
 * flags:        KNX_UART_HAL_RX_* bits for this octet.
 * timestamp_us: when the octet was received, from a free-running microsecond
 *               counter that wraps at 2^32. Consumers use unsigned differences
 *               only. The TP-UART backend needs about 100 us resolution to
 *               detect the 2 ms inter-frame gap; a board without a microsecond
 *               timer may pass a coarser clock scaled to microseconds. */
typedef void (*knx_uart_hal_rx_cb_t)(void* rx_context, uint8_t octet, uint8_t flags, uint32_t timestamp_us);

/* Called from an interrupt on an edge of the transceiver's SAVE output.
 *
 * timestamp_us: when the edge was seen, from the same clock as the RX
 *               callback's time stamps. The callback reads the level itself
 *               (read_save), so a board may call it for both edges. It may
 *               call read_save() and must not block. */
typedef void (*knx_uart_hal_save_edge_cb_t)(void* save_context, uint32_t timestamp_us);

typedef struct {
    /* Configure the port and start receiving. The RX callback may run as soon
     * as open() has enabled the receiver, before open() returns. Returns false
     * for an unsupported configuration, a null callback or a port that is
     * already open. */
    bool (*open)(void* context,
                 const knx_uart_hal_config_t* config,
                 knx_uart_hal_rx_cb_t rx_cb,
                 void* rx_context);

    /* Stop receiving and transmitting and drop any queued TX octets. After
     * close() returns, the RX callback is not called again. */
    void (*close)(void* context);

    /* Queue octets for transmission without blocking. All or nothing: returns
     * false, and queues nothing, when the port is closed, len is 0 or the TX
     * buffer has fewer than len free octets. Safe to call from the RX callback;
     * see the concurrency model above. */
    bool (*write)(void* context, const uint8_t* data, size_t len);

    /* Free octets in the TX buffer now; 0 while the port is closed. A write of
     * up to this many octets from the main context succeeds, unless the RX
     * callback writes in between. */
    size_t (*tx_free)(void* context);

    /* Optional (NULL when the board does not wire the transceiver's reset
     * input to the MCU). asserted = true holds the transceiver in reset. The
     * board applies the pin's polarity. */
    bool (*set_reset)(void* context, bool asserted);

    /* Optional (NULL when the board does not wire the transceiver's SAVE
     * output to the MCU). Returns true while the transceiver signals a bus
     * power failure. The board applies the pin's polarity. The backend samples
     * it from the main context and debounces it (LinkSignalPolicy in the
     * backend's configuration), exactly as the TP1 bitbang driver treats the
     * STKNX KNX_OK output. */
    bool (*read_save)(void* context);

    /* Optional, both or neither (NULL when SAVE has no edge interrupt; it is
     * then only polled). install_save_edge_isr() makes the board call cb from
     * an interrupt on every edge of SAVE, until remove_save_edge_isr(). The
     * backend uses it only for the early power-fail warning
     * (LinkSignalPolicy::notifyPowerFailFromIsr); the debounced state is
     * still taken from read_save(). Returns false if the interrupt cannot be
     * set up. Requires read_save. */
    bool (*install_save_edge_isr)(void* context, knx_uart_hal_save_edge_cb_t cb, void* save_context);
    void (*remove_save_edge_isr)(void* context);
} knx_uart_hal_ops_t;

typedef struct {
    void* context;
    knx_uart_hal_ops_t ops;
} knx_uart_hal_t;

/* The SAVE helpers below are called from the KNX_ISR_RAM edge trampoline, where
 * an out-of-line call into flash needs a veneer the gate rejects: force them inline. */
static inline __attribute__((always_inline)) bool knx_uart_hal_is_valid(const knx_uart_hal_t* hal)
{
    return hal && hal->context && hal->ops.open && hal->ops.close && hal->ops.write && hal->ops.tx_free;
}

static inline bool knx_uart_hal_open(const knx_uart_hal_t* hal,
                                     const knx_uart_hal_config_t* config,
                                     knx_uart_hal_rx_cb_t rx_cb,
                                     void* rx_context)
{
    return knx_uart_hal_is_valid(hal) && config && rx_cb && hal->ops.open(hal->context, config, rx_cb, rx_context);
}

static inline void knx_uart_hal_close(const knx_uart_hal_t* hal)
{
    if (knx_uart_hal_is_valid(hal)) {
        hal->ops.close(hal->context);
    }
}

static inline bool knx_uart_hal_write(const knx_uart_hal_t* hal, const uint8_t* data, size_t len)
{
    return knx_uart_hal_is_valid(hal) && data && len > 0 && hal->ops.write(hal->context, data, len);
}

static inline size_t knx_uart_hal_tx_free(const knx_uart_hal_t* hal)
{
    return knx_uart_hal_is_valid(hal) ? hal->ops.tx_free(hal->context) : 0;
}

static inline bool knx_uart_hal_has_reset(const knx_uart_hal_t* hal)
{
    return knx_uart_hal_is_valid(hal) && hal->ops.set_reset;
}

static inline bool knx_uart_hal_set_reset(const knx_uart_hal_t* hal, bool asserted)
{
    return knx_uart_hal_has_reset(hal) && hal->ops.set_reset(hal->context, asserted);
}

static inline __attribute__((always_inline)) bool knx_uart_hal_has_save(const knx_uart_hal_t* hal)
{
    return knx_uart_hal_is_valid(hal) && hal->ops.read_save;
}

/* False when the board has no SAVE input: without the signal, the stack
 * cannot tell a power failure is coming and does not act on one. */
static inline __attribute__((always_inline)) bool knx_uart_hal_read_save(const knx_uart_hal_t* hal)
{
    return knx_uart_hal_has_save(hal) && hal->ops.read_save(hal->context);
}

static inline bool knx_uart_hal_has_save_edge_isr(const knx_uart_hal_t* hal)
{
    return knx_uart_hal_has_save(hal) && hal->ops.install_save_edge_isr && hal->ops.remove_save_edge_isr;
}

static inline bool knx_uart_hal_install_save_edge_isr(const knx_uart_hal_t* hal,
                                                      knx_uart_hal_save_edge_cb_t cb,
                                                      void* save_context)
{
    return knx_uart_hal_has_save_edge_isr(hal) && cb &&
           hal->ops.install_save_edge_isr(hal->context, cb, save_context);
}

static inline void knx_uart_hal_remove_save_edge_isr(const knx_uart_hal_t* hal)
{
    if (knx_uart_hal_has_save_edge_isr(hal)) {
        hal->ops.remove_save_edge_isr(hal->context);
    }
}

#ifdef __cplusplus
}
#endif
