// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file main.cpp
 * @brief Size gate reference device: starts the stack with a null TP1 backend
 *
 * Built only to be linked and measured (see CMakeLists.txt and
 * scripts/ci/knstax_size_budget.py). It starts the commissioned product with
 * 60 group objects through the real runtime, so the linked image contains the
 * code a TP1 end device needs. The TP-UART backend, the flash persistence and
 * the board support a real product adds are not part of the KNstaX share.
 */

#include "ram_persistence.hpp"
#include "stub_product.hpp"

#include "knx/objects/object_persistence.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/platform/cooperative_platform.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

#if defined(KNX_SIZE_GATE_SEMIHOST)
extern "C" std::size_t g_heap_high_water;  // syscalls.c: peak of the heap break
extern "C" void knx_size_gate_print_profile();  // syscalls.c: malloc size histogram
#endif

namespace {

// SysTick (core peripheral, identical on every Cortex-M).
constexpr uint32_t kSysTickCtrl = 0xE000E010u;
constexpr uint32_t kSysTickLoad = 0xE000E014u;
constexpr uint32_t kCoreClockHz = 8000000u;  // reset-default internal oscillator

volatile uint32_t g_millis = 0;

uint32_t hookMillis(void*) { return g_millis; }
uint64_t hookMicros(void*) { return static_cast<uint64_t>(g_millis) * 1000u; }
void hookIdle(void*) { asm volatile("wfi"); }

std::unique_ptr<knx::objects::ObjectPersistence> makePersistence() {
    return std::make_unique<size_gate::RamPersistence>();
}

#if defined(KNX_SIZE_GATE_SEMIHOST)
// ARM semihosting call (bkpt 0xAB): r0 = operation, r1 = parameter block.
void semihost(int operation, const void* argument) {
    register int r0 asm("r0") = operation;
    register const void* r1 asm("r1") = argument;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
}

// Prints "heap_high_water=<n>" on the emulator's console and ends the run.
// The figure is the peak after the stack has started and run a few loop
// iterations: the init-time allocation the heap policy (K7 step 8) bounds.
std::size_t g_heapBeforeStart = 0;
std::size_t g_heapAfterStart = 0;

void writeNumber(const char* label, std::size_t value) {
    char text[48];
    int length = 0;
    for (const char* c = label; *c != 0; ++c) text[length++] = *c;
    char digits[12];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    while (count > 0) text[length++] = digits[--count];
    text[length++] = static_cast<char>(10);
    text[length] = static_cast<char>(0);
    semihost(0x04, text);  // SYS_WRITE0
}

[[noreturn]] void reportAndExit() {
    writeNumber("heap_before_start=", g_heapBeforeStart);
    writeNumber("heap_after_start=", g_heapAfterStart);
    knx_size_gate_print_profile();
    char text[40] = "heap_high_water=";
    char digits[12];
    int count = 0;
    std::size_t value = g_heap_high_water;
    do {
        digits[count++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    int length = 16;
    while (count > 0) {
        text[length++] = digits[--count];
    }
    text[length++] = '\n';
    text[length] = '\0';
    semihost(0x04, text);                                      // SYS_WRITE0
    semihost(0x18, reinterpret_cast<const void*>(0x20026));    // SYS_EXIT: ApplicationExit
    for (;;) {
    }
}
#endif

}  // namespace

extern "C" void SysTick_Handler() { g_millis = g_millis + 1u; }

int main() {
#if !defined(KNX_SIZE_GATE_SEMIHOST)
    *reinterpret_cast<volatile uint32_t*>(kSysTickLoad) = kCoreClockHz / 1000u - 1u;
    *reinterpret_cast<volatile uint32_t*>(kSysTickCtrl) = 0x7u;  // core clock, interrupt, enable
#endif

    knx::objects::setPersistenceFactory(&makePersistence);
#if defined(KNX_SIZE_GATE_SEMIHOST)
    g_heapBeforeStart = g_heap_high_water;
#endif

    knx::platform::CooperativeHooks hooks{};
    hooks.millis = &hookMillis;
    hooks.micros = &hookMicros;
    hooks.idle = &hookIdle;
    static knx::platform::CooperativePlatform platform(hooks);

    auto started = knx::product::startCommissionedProduct(
        platform,
        size_gate::kProduct,
        knx::product::makeCommissionedBindings(size_gate::kProduct),
        std::unique_ptr<knx::physical::Tp1MediumBackend>(new knx::physical::NullTp1MediumBackend()));
    if (started.isError()) {
        for (;;) {
        }
    }
    auto app = std::move(started.value());
#if defined(KNX_SIZE_GATE_SEMIHOST)
    g_heapAfterStart = g_heap_high_water;
#endif

#if defined(KNX_SIZE_GATE_SEMIHOST)
    for (int i = 0; i < 200; ++i) {
        g_millis = g_millis + 5u;  // no SysTick on the emulator's timer path; advance time by hand
        app->loop();
    }
    reportAndExit();
#else
    for (;;) {
        app->loop();
        asm volatile("wfi");
    }
#endif
}
