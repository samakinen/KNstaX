/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal SEGGER RTT control block, laid out as the host tools expect it
 * (SEGGER RTT documentation, "Implementation"; OpenOCD src/target/rtt.c).
 * See rtt.h. */
#include "rtt.h"

#if defined(__has_include)
#if __has_include("gd32f1_port_config.h")
#include "gd32f1_port_config.h"
#endif
#endif

#ifndef RTT_UP_SIZE
#define RTT_UP_SIZE 1024u
#endif
#define RTT_DOWN_SIZE 16u

typedef struct {
    const char* name;
    char* buffer;
    uint32_t size;
    volatile uint32_t wr_off;
    volatile uint32_t rd_off;
    uint32_t flags;
} rtt_buffer_t;

typedef struct {
    char id[16];
    int32_t max_up;
    int32_t max_down;
    rtt_buffer_t up[1];
    rtt_buffer_t down[1];
} rtt_cb_t;

static char up_storage[RTT_UP_SIZE];
static char down_storage[RTT_DOWN_SIZE];
static uint32_t dropped;

/* Found by the host searching RAM for the ID; the ID is written last. */
volatile rtt_cb_t _SEGGER_RTT;

void rtt_init(void)
{
    _SEGGER_RTT.max_up = 1;
    _SEGGER_RTT.max_down = 1;
    _SEGGER_RTT.up[0].name = "Terminal";
    _SEGGER_RTT.up[0].buffer = up_storage;
    _SEGGER_RTT.up[0].size = RTT_UP_SIZE;
    _SEGGER_RTT.up[0].wr_off = 0;
    _SEGGER_RTT.up[0].rd_off = 0;
    _SEGGER_RTT.up[0].flags = 0; /* no block: skip when full */
    _SEGGER_RTT.down[0].name = "Terminal";
    _SEGGER_RTT.down[0].buffer = down_storage;
    _SEGGER_RTT.down[0].size = RTT_DOWN_SIZE;
    _SEGGER_RTT.down[0].wr_off = 0;
    _SEGGER_RTT.down[0].rd_off = 0;
    _SEGGER_RTT.down[0].flags = 0;

    static const char id[] = "SEGGER RTT";
    __asm volatile("" ::: "memory");
    for (uint32_t i = 1; i < sizeof id; ++i) {
        _SEGGER_RTT.id[i] = id[i];
    }
    __asm volatile("" ::: "memory");
    _SEGGER_RTT.id[0] = id[0];
}

void rtt_write(const char* data, uint32_t length)
{
    volatile rtt_buffer_t* b = &_SEGGER_RTT.up[0];
    uint32_t wr = b->wr_off;
    const uint32_t rd = b->rd_off;
    const uint32_t free_space = (rd > wr) ? (rd - wr - 1u) : (b->size - wr + rd - 1u);
    if (length > free_space) {
        dropped += length;
        return;
    }
    for (uint32_t i = 0; i < length; ++i) {
        b->buffer[wr] = data[i];
        wr = (wr + 1u == b->size) ? 0u : wr + 1u;
    }
    __asm volatile("dmb" ::: "memory");
    b->wr_off = wr;
}

int rtt_read_char(void)
{
    volatile rtt_buffer_t* b = &_SEGGER_RTT.down[0];
    const uint32_t rd = b->rd_off;
    if (rd == b->wr_off) {
        return -1;
    }
    const int c = (unsigned char)b->buffer[rd];
    __asm volatile("dmb" ::: "memory");
    b->rd_off = (rd + 1u == b->size) ? 0u : rd + 1u;
    return c;
}

uint32_t rtt_dropped(void) { return dropped; }
