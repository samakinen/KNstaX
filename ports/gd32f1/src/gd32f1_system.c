/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GD32F1 reference port; see gd32f1_system.h. Register addresses and fields:
 * GD32F10x user manual (RCU, FMC, AFIO, GPIO, FWDGT) and the Cortex-M3 core. */
#include "gd32f1_system.h"

#define REG(addr) (*(volatile uint32_t*)(addr))

#define RCU_CTL REG(0x40021000u)
#define RCU_CFG0 REG(0x40021004u)
#define RCU_APB2EN REG(0x40021018u)
#define RCU_RSTSCK REG(0x40021024u)
#define FMC_WS REG(0x40022000u)
#define AFIO_PCF0 REG(0x40010004u)
#define GPIO_BASE(port) (0x40010800u + 0x400u * (port))
#define GPIO_CTL(port, pin) REG(GPIO_BASE(port) + ((pin) < 8u ? 0x00u : 0x04u))
#define GPIO_ISTAT(port) REG(GPIO_BASE(port) + 0x08u)
#define GPIO_OCTL(port) REG(GPIO_BASE(port) + 0x0Cu)
#define GPIO_BOP(port) REG(GPIO_BASE(port) + 0x10u)
#define FWDGT_CTL REG(0x40003000u)
#define FWDGT_PSC REG(0x40003004u)
#define FWDGT_RLD REG(0x40003008u)
#define FWDGT_STAT REG(0x4000300Cu)
#define SYST_CSR REG(0xE000E010u)
#define SYST_RVR REG(0xE000E014u)
#define SYST_CVR REG(0xE000E018u)
#define SCB_ICSR REG(0xE000ED04u)
#define SCB_AIRCR REG(0xE000ED0Cu)
#define SCB_SHPR3 REG(0xE000ED20u)
#define DEMCR REG(0xE000EDFCu)
#define DWT_CTRL REG(0xE0001000u)
#define UID_BASE 0x1FFFF7E8u

#define SYSTICK_LOAD (GD32F1_CORE_HZ / 1000u - 1u)

static volatile uint64_t g_ms;

void SysTick_Handler(void);
void SysTick_Handler(void) { g_ms = g_ms + 1u; }

__attribute__((weak)) void gd32f1_early_init(void) {}

static void clock_init(void)
{
    /* HSI runs from reset. 48 MHz needs one flash wait state (24-48 MHz). */
    FMC_WS = (FMC_WS & ~7u) | 1u;
    /* PLL source HSI/2 (PLLSEL = 0), x12 (PLLMF = 1010b); AHB /1, APB1 /2, APB2 /1. */
    RCU_CFG0 = (RCU_CFG0 & ~((0xFu << 18) | (1u << 27) | (1u << 16) | (0xFu << 4) | (7u << 8) | (7u << 11))) |
               (0xAu << 18) | (4u << 8);
    RCU_CTL |= 1u << 24; /* PLLEN */
    while ((RCU_CTL & (1u << 25)) == 0u) {
    }
    RCU_CFG0 = (RCU_CFG0 & ~3u) | 2u; /* system clock = PLL */
    while (((RCU_CFG0 >> 2) & 3u) != 2u) {
    }
}

void gd32f1_system_init(void)
{
    clock_init();
    RCU_APB2EN |= (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4); /* AFIO, PA, PB, PC */
    /* JTAG off, SWD on: SWJ_CFG = 010. Never 100 (that disables SWD too). */
    AFIO_PCF0 = (AFIO_PCF0 & ~(7u << 24)) | (2u << 24);

    DEMCR |= 1u << 24; /* trace on, for the cycle counter */
    DWT_CTRL |= 1u;

    /* SysTick at the highest priority, so time is current in the UART interrupt. */
    SCB_SHPR3 = SCB_SHPR3 & 0x00FFFFFFu;
    SYST_RVR = SYSTICK_LOAD;
    SYST_CVR = 0u;
    SYST_CSR = 7u;
}

uint32_t gd32f1_millis(void) { return (uint32_t)g_ms; }

uint64_t gd32f1_micros(void)
{
    uint64_t ms;
    uint32_t value;
    uint32_t pending;
    do {
        ms = g_ms;
        value = SYST_CVR;
        pending = SCB_ICSR & (1u << 26); /* PENDSTSET: wrapped, handler not yet run */
    } while (ms != g_ms);
    if (pending != 0u && value > SYSTICK_LOAD / 2u) {
        ms += 1u;
    }
    return ms * 1000u + (SYSTICK_LOAD - value) / (GD32F1_CORE_HZ / 1000000u);
}

void gd32f1_gpio_mode(uint32_t port, uint32_t pin, uint32_t mode)
{
    const uint32_t shift = (pin & 7u) * 4u;
    if ((mode & 0x10u) != 0u) {
        GPIO_OCTL(port) |= 1u << pin; /* pull-up */
    } else if ((mode & 0x20u) != 0u) {
        GPIO_OCTL(port) &= ~(1u << pin); /* pull-down */
    }
    GPIO_CTL(port, pin) = (GPIO_CTL(port, pin) & ~(0xFu << shift)) | ((mode & 0xFu) << shift);
}

void gd32f1_gpio_write(uint32_t port, uint32_t pin, bool high)
{
    GPIO_BOP(port) = high ? (1u << pin) : (1u << (pin + 16u));
}

bool gd32f1_gpio_read(uint32_t port, uint32_t pin) { return (GPIO_ISTAT(port) & (1u << pin)) != 0u; }

uint32_t gd32f1_serial_number(void)
{
    const volatile uint32_t* uid = (const volatile uint32_t*)UID_BASE;
    /* FNV-1a over the 12 octets: spreads the lot and wafer fields. */
    uint32_t hash = 2166136261u;
    for (uint32_t w = 0; w < 3u; ++w) {
        uint32_t word = uid[w];
        for (uint32_t i = 0; i < 4u; ++i) {
            hash ^= word & 0xFFu;
            hash *= 16777619u;
            word >>= 8;
        }
    }
    return hash;
}

uint32_t gd32f1_reset_cause(void)
{
    const uint32_t flags = RCU_RSTSCK & 0xFC000000u;
    RCU_RSTSCK |= 1u << 24; /* RSTFC: clear the flags */
    return flags;
}

const char* gd32f1_reset_cause_text(uint32_t cause)
{
    if (cause & (1u << 29)) return "watchdog";
    if (cause & (1u << 28)) return "software";
    if (cause & (1u << 30)) return "window watchdog";
    if (cause & (1u << 31)) return "low power";
    if (cause & (1u << 27)) return "power on";
    if (cause & (1u << 26)) return "reset pin";
    return "unknown";
}

void gd32f1_watchdog_start(void)
{
    /* Start first: that switches IRC40K on, and the PSC and RLD updates
     * (STAT PUD, RUD) only complete on that clock. Until they do, the reset
     * values run: /4 and 4095, about 0.4 s. */
    FWDGT_CTL = 0xCCCCu;
    FWDGT_CTL = 0x5555u; /* unlock PSC and RLD */
    FWDGT_PSC = 3u;      /* IRC40K / 32 = 1250 Hz */
    FWDGT_RLD = 1250u;   /* about 1 s (IRC40K is 30-60 kHz) */
    for (uint32_t spin = 0; (FWDGT_STAT & 3u) != 0u && spin < 1000000u; ++spin) {
    }
    FWDGT_CTL = 0xAAAAu;
}

void gd32f1_watchdog_feed(void) { FWDGT_CTL = 0xAAAAu; }

void gd32f1_restart(void)
{
    __asm volatile("dsb");
    SCB_AIRCR = 0x05FA0004u; /* SYSRESETREQ */
    __asm volatile("dsb");
    for (;;) {
    }
}
