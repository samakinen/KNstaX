// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file main.cpp
 * @brief Size gate reference device: starts the stack with the TP-UART backend
 *
 * Built only to be linked and measured (see CMakeLists.txt and
 * scripts/ci/knstax_size_budget.py). It starts the commissioned product with
 * 60 group objects through the real runtime, so the linked image contains the
 * code a TP1 end device needs. The board support a real product adds is not
 * part of the KNstaX share.
 *
 * The image that is measured for flash and static RAM starts the device with the
 * TP-UART backend over a stub board UART (uart_stub.hpp), so the backend is in
 * the KNstaX share; that image is linked, not run. The backend object is on the
 * heap and not in the emulator's figure: the device budget reserves RAM for it
 * ("TP-UART buffers"), and the emulator run prints its size
 * (tpuart_backend_bytes) to compare. The emulator variant (KNX_SIZE_GATE_SEMIHOST) measures
 * the heap: it starts the same device with a scripted backend (traffic.hpp), sets
 * the individual address and the group addresses the way a commissioning would,
 * and runs the bus traffic a device meets in service, so that the high-water mark
 * covers receive and send as well as start-up.
 *
 * Persistence is FlashLog over the board's flash (flash_board.hpp), in the
 * KNstaX share like the backend: a stand-in flash controller in the measured
 * image, flash kept in RAM in the emulator variant.
 */

#include "flash_board.hpp"
#include "stub_product.hpp"

#include "knx/objects/flash_log.hpp"
#include "knx/objects/flash_log_persistence.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/physical/tpuart_medium_backend.hpp"
#include "knx/platform/cooperative_platform.hpp"
#include "knx/util/heap.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#if defined(KNX_SIZE_GATE_SEMIHOST)
extern "C" std::size_t g_heap_high_water;  // syscalls.c: peak of the heap break
extern "C" void knx_size_gate_print_profile();  // syscalls.c: malloc size histogram
extern "C" uint32_t knx_size_gate_malloc_calls();  // syscalls.c: malloc() calls so far
#include "traffic.hpp"
#else
#include "uart_stub.hpp"
#endif

namespace {

// SysTick (core peripheral, identical on every Cortex-M).
constexpr uint32_t kSysTickCtrl = 0xE000E010u;
constexpr uint32_t kSysTickLoad = 0xE000E014u;
constexpr uint32_t kCoreClockHz = 8000000u;  // reset-default internal oscillator

volatile uint32_t g_millis = 0;
volatile uint32_t g_observed = 0;  // handler invocations (the bindings below)

uint32_t hookMillis(void*) { return g_millis; }
uint64_t hookMicros(void*) { return static_cast<uint64_t>(g_millis) * 1000u; }
void hookIdle(void*) { asm volatile("wfi"); }

knx::objects::FlashLog g_flashLog(size_gate::flash::makeHal());

std::unique_ptr<knx::objects::ObjectPersistence> makePersistence() {
    return std::make_unique<knx::objects::FlashLogPersistence>(g_flashLog);
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

std::uint32_t g_mallocAfterStart = 0;
std::size_t g_heapBeforeDownload = 0;
std::uint32_t g_restartRequested = 0;

// The heap is locked once the device has started (knx::heap::lock()); every
// allocation after that is a violation. The emulator variant records them, so the
// run lists each steady-state allocation (its caller's address, to be looked up in
// the ELF, and its size) instead of stopping at the first.
struct Violation {
    const void* caller;
    std::size_t size;
    std::uint32_t count;
};
Violation g_violations[24] = {};

void recordViolation(std::size_t size, const void* caller) {
    for (auto& v : g_violations) {
        if (v.count == 0u) {
            v = {caller, size, 1u};
            return;
        }
        if (v.caller == caller && v.size == size) {
            ++v.count;
            return;
        }
    }
}

std::size_t readHighWater() { return g_heap_high_water; }

void reportTraffic() {
    using size_gate::traffic::g_stats;
    writeNumber("traffic_injected=", g_stats.injected);
    writeNumber("traffic_tool_acks=", g_stats.toolAcksInjected);
    writeNumber("traffic_sent=", g_stats.sent);
    writeNumber("traffic_confirmed=", g_stats.confirmed);
    writeNumber("traffic_group_writes_sent=", g_stats.groupWrites);
    writeNumber("traffic_group_reads_sent=", g_stats.groupReads);
    writeNumber("traffic_group_responses_sent=", g_stats.groupResponses);
    writeNumber("traffic_link_acks_sent=", g_stats.linkAcks);
    writeNumber("traffic_connected_data_sent=", g_stats.connectedData);
    writeNumber("traffic_other_sent=", g_stats.otherSent);
    for (std::size_t i = 0; i < g_stats.connectedApci.size() && i < g_stats.connectedData; ++i) {
        writeNumber("traffic_connected_apci=", g_stats.connectedApci[i]);
    }
    for (std::size_t i = 0; i < g_stats.sent && i < g_stats.log.size(); ++i) {
        char text[100];
        int n = 0;
        text[n++] = 'f'; text[n++] = 'r'; text[n++] = 'a'; text[n++] = 'm'; text[n++] = 'e'; text[n++] = ' ';
        for (std::size_t b = 0; b < g_stats.log[i].length && n < 94; ++b) {
            text[n++] = "0123456789ABCDEF"[g_stats.log[i].bytes[b] >> 4];
            text[n++] = "0123456789ABCDEF"[g_stats.log[i].bytes[b] & 15];
            text[n++] = ' ';
        }
        text[n++] = '\n';
        text[n] = 0;
        semihost(0x04, text);
    }
    writeNumber("traffic_observed_by_product=", g_observed);
    writeNumber("traffic_observed_expected=", size_gate::traffic::kExpectedObserved);
    writeNumber("download_restart_requested=", g_restartRequested);
    // The download took effect when the group traffic on the downloaded addresses reached the
    // ports it should (the three tables were applied, the group address list followed), and
    // the device ended the session with its restart.
    writeNumber("download_applied=",
                (g_restartRequested != 0u && g_observed == size_gate::traffic::kExpectedObserved) ? 1u : 0u);
    writeNumber("heap_before_download=", g_heapBeforeDownload);
    writeNumber("malloc_calls_after_start=", knx_size_gate_malloc_calls() - g_mallocAfterStart);
    writeNumber("heap_locked_allocations=", static_cast<std::uint32_t>(knx::heap::violationCount()));
    for (const auto& v : g_violations) {
        if (v.count == 0u) break;
        writeNumber("heap_violation_caller=", reinterpret_cast<std::uintptr_t>(v.caller));
        writeNumber("heap_violation_bytes=", v.size);
        writeNumber("heap_violation_count=", v.count);
    }
    writeNumber("heap_api_high_water=", knx::heap::highWater());
}

[[noreturn]] void reportAndExit() {
    writeNumber("tpuart_backend_bytes=", sizeof(knx::physical::TpUartMediumBackend));
    writeNumber("scripted_backend_bytes=", sizeof(size_gate::traffic::ScriptedBackend));
    writeNumber("heap_before_start=", g_heapBeforeStart);
    writeNumber("heap_after_start=", g_heapAfterStart);
    reportTraffic();
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

// ── Bindings ────────────────────────────────────────────────────────────────
//
// A product binds a handler on every port it uses, so the gate does too: the
// binding storage and the dispatch code are part of what a device pays. Each
// handler captures a small index, as a real product's per-channel handlers do
// (a channel number, a pointer to a channel object), and each port gets its own
// callable type. The handlers do nothing but observe the value; what a product
// does with it is not the KNstaX share.

volatile bool g_state[60] = {};

template <typename ValueT>
void observe(std::size_t index, const ValueT& value) {
    asm volatile("" : : "r"(&value), "r"(index) : "memory");
    g_observed = g_observed + 1u;
}

using Bindings = knx::product::CommissionedBindingsBuilder<
    std::remove_cvref_t<decltype(size_gate::kProduct)>, knx::product::kDefaultBindingCapacity>;

template <std::size_t First, std::size_t... I>
void bindCommands(Bindings& bindings, std::index_sequence<I...>) {
    (bindings.template onCommand<static_cast<size_gate::Port>(First + I)>(
         [index = First + I](auto value) { observe(index, value); }),
     ...);
}

template <std::size_t First, std::size_t... I>
void bindStateProviders(Bindings& bindings, std::index_sequence<I...>) {
    (bindings.template provideState<static_cast<size_gate::Port>(First + I)>(
         [index = First + I]() -> bool { return g_state[index]; }),
     ...);
}

template <std::size_t First, std::size_t... I>
void bindStateWrites(Bindings& bindings, std::index_sequence<I...>) {
    (bindings.template onStateWrite<static_cast<size_gate::Port>(First + I)>(
         [index = First + I](bool value) { g_state[index] = value; }),
     ...);
}

// Ports 0-15 switch commands, 16-31 switch states, 32-39 switch state in/out,
// 40-47 percent, 48-51 dimming, 52-55 scene control, 56-59 scene number.
void bindAllPorts(Bindings& bindings) {
    bindCommands<0>(bindings, std::make_index_sequence<16>{});
    bindStateProviders<16>(bindings, std::make_index_sequence<16>{});
    bindStateProviders<32>(bindings, std::make_index_sequence<8>{});
    bindStateWrites<32>(bindings, std::make_index_sequence<8>{});
    bindCommands<40>(bindings, std::make_index_sequence<20>{});
}

// One publish per datapoint type: the typed call sites a product has.
template <typename AppT>
void publishOnce(AppT& app) {
    using size_gate::Port;
    (void)app.template publish<Port::SwState1>(true);
    (void)app.template publish<Port::SwIo1>(false);
    (void)app.template publish<Port::ValueIn1>(static_cast<uint8_t>(50u));
    (void)app.template publish<Port::DimIn1>(knx::application::Dpt3Value{});
    (void)app.template publish<Port::SceneCtl1>(knx::application::Dpt18Value{});
    (void)app.template publish<Port::SceneNum1>(knx::application::Dpt17Value{});
}

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
#if defined(KNX_SIZE_GATE_SEMIHOST)
    // A_Restart ends the ETS download. The device would reset here; the emulator run ends
    // instead (the stack has flushed what is dirty and answered the tool before it calls this).
    hooks.restart = [](void*) {
        g_restartRequested = 1;
        reportAndExit();
    };
#endif
    static knx::platform::CooperativePlatform platform(hooks);

    auto bindings = knx::product::makeCommissionedBindings(size_gate::kProduct);
    bindAllPorts(bindings);
    auto started = knx::product::startCommissionedProduct(
        platform,
        size_gate::kProduct,
        std::move(bindings),
#if defined(KNX_SIZE_GATE_SEMIHOST)
        std::unique_ptr<knx::physical::Tp1MediumBackend>(new size_gate::traffic::ScriptedBackend()));
#else
        std::unique_ptr<knx::physical::Tp1MediumBackend>(
            new knx::physical::TpUartMediumBackend(size_gate::uart::makeHal(), platform)));
#endif
    if (started.isError()) {
        for (;;) {
        }
    }
    auto app = std::move(started.value());
    publishOnce(*app);
#if !defined(KNX_SIZE_GATE_SEMIHOST)
    knx::heap::lock();  // start-up is over: from here an allocation is a defect (KNX_HEAP_POLICY=init_only)
#endif
#if defined(KNX_SIZE_GATE_SEMIHOST)
    g_heapAfterStart = g_heap_high_water;
    g_mallocAfterStart = knx_size_gate_malloc_calls();
    knx::heap::setHighWaterHook(&readHighWater);
    knx::heap::setViolationHandler(&recordViolation);
    knx::heap::lock();
    writeNumber("heap_start_profile:", 0);
    knx_size_gate_print_profile();

    // Commissioning, as the tool leaves it: the individual address and the group
    // addresses of the first port of each datapoint type.
    using size_gate::Port;
    using namespace size_gate::traffic;
    app->deviceObject().setProgModeSilent(knx::Toggle::Enable);  // the address can only be written in programming mode
    (void)app->deviceObject().writeIndividualAddress(knx::IndividualAddress(1, 1, 10));
    app->deviceObject().setProgModeSilent(knx::Toggle::Disable);
    (void)app->applyCommissionedGroupAddresses(
        knx::product::groupAddressBinding(Port::SwIn1, knx::GroupAddress(kGroupSwitchIn)),
        knx::product::groupAddressBinding(Port::SwState1, knx::GroupAddress(kGroupSwitchState)),
        knx::product::groupAddressBinding(Port::SwIo1, knx::GroupAddress(kGroupSwitchIo)),
        knx::product::groupAddressBinding(Port::ValueIn1, knx::GroupAddress(kGroupPercent)),
        knx::product::groupAddressBinding(Port::DimIn1, knx::GroupAddress(kGroupDimming)),
        knx::product::groupAddressBinding(Port::SceneCtl1, knx::GroupAddress(kGroupSceneControl)),
        knx::product::groupAddressBinding(Port::SceneNum1, knx::GroupAddress(kGroupSceneNumber)));
    for (int i = 0; i < 20; ++i) {  // let the stack settle after the configuration
        g_millis = g_millis + 5u;
        app->loop();
        (void)g_flashLog.service();
    }
    for (uint16_t loop = 0; loop < kLoops; ++loop) {
        g_millis = g_millis + 5u;  // no SysTick on the emulator's timer path; advance time by hand
        if (loop == kEtsStartLoop) g_heapBeforeDownload = g_heap_high_water;
        tick(loop);
        if (loop == kPublishAtLoop || loop == kRepublishAtLoop) publishOnce(*app);
        app->loop();
        (void)g_flashLog.service();
    }
    reportAndExit();
#else
    for (;;) {
        app->loop();
        (void)g_flashLog.service();
        asm volatile("wfi");
    }
#endif
}
