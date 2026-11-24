# GD32F1 reference port

KNstaX on a GD32F103 or STM32F103 (Cortex-M3) with a Siemens TP-UART2, bare
metal, no vendor HAL. It is the board support of the
[H8i8o firmware](https://github.com/samakinen/h8i8o) made generic. On that
device the same code has completed ETS6 product import, address assignment,
application download and group communication on a live bus, and KNX Data
Secure (secure commissioning, a secured download, secured group
communication). The `example/` firmware built here (one switch output) has
not itself run on hardware.

## What the port provides

| File | What it is |
|---|---|
| `src/gd32f1_startup.c` | Vector table (STM32F103 medium-density names, all weak), `Reset_Handler`, the C runtime start; calls `gd32f1_early_init()` first |
| `link/gd32f103xb.ld` | 128 KiB / 20 KiB part: 120 KiB image, 8 KiB persistence, 2.5 KiB stack reserve |
| `src/gd32f1_syscalls.c` | newlib-nano stubs and `_sbrk` with the heap limit and high-water mark |
| `src/gd32f1_system.c/.h` | 48 MHz clock from the internal oscillator, SysTick, `millis`/`micros`, GPIO helpers, unique-ID serial number, reset cause, watchdog, restart |
| `src/gd32f1_tpuart.c/.h` | USART0 (PA9/PA10) as the stack's `knx_uart_hal_t`, SAVE pin with its edge interrupt, optional octet trace |
| `src/gd32f1_fmc.c/.h` | The flash controller as the stack's `knx_flash_hal_t` for `FlashLog`, erase from RAM with interrupts masked |
| `src/rtt.c/.h` | A minimal SEGGER RTT log and command channel (OpenOCD or J-Link) |
| `port.cmake` | `knx_gd32f1_port(<target>)`: adds all of the above to a firmware target |
| `example/` | A one-channel switch actuator showing the start-up order every product needs |

Defaults are set in `src/gd32f1_config.h`. A product overrides them with a
`gd32f1_port_config.h` on its include path (as `example/` does): the SAVE
pin, the persistence region, the UART interrupt priority, the trace.

## Build the example

In the toolchain image (`tools/docker/Dockerfile.ci`) or with
`arm-none-eabi-gcc` 13 or later:

```bash
cmake -S ports/gd32f1 -B build/gd32f1 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake
cmake --build build/gd32f1
```

The image is `build/gd32f1/knx_gd32f1_example.elf` (and `.bin`), about 90 KB
of flash and 3 KB of static RAM. Program it with any SWD probe, for example
OpenOCD with `target/stm32f1x.cfg`. **The pins are the H8i8o's**: PA0 output,
PA11 button, PA12 LED, PB15 SAVE. Check them against your board in
`example/gd32f1_port_config.h` before you program it; a GD32 erased by a
probe loses its factory firmware for good.

## Use it in a product

```cmake
include(${KNSTAX_DIR}/cmake/knx_config.cmake)
knx_configure(PROFILE tp1-small)
add_subdirectory(${KNSTAX_DIR} knstax EXCLUDE_FROM_ALL)
include(${KNSTAX_DIR}/ports/gd32f1/port.cmake)

add_executable(my_device app/main.cpp)
target_include_directories(my_device PRIVATE app)   # with gd32f1_port_config.h
target_link_libraries(my_device PRIVATE knx_core)
knx_gd32f1_port(my_device)                          # LINKER_SCRIPT my.ld to override
```

Then follow `example/main.cpp`: the order of the steps there matters (outputs
safe first, persistence mounted before the stack, the heap locked after
start-up, the erase guard set).

## Porting to another MCU

The stack needs three things from a board, all C and all narrow:

1. **A UART** to the transceiver, as `knx_uart_hal_t`
   (`include/knx/physical/uart_hal.h`): 19200 baud 8E1, one receive callback
   per octet from the interrupt with a microsecond time stamp, and a
   non-blocking, all-or-nothing `write` that is safe from that callback. Read
   the header's concurrency model first; `src/gd32f1_tpuart.c` is about
   300 lines and a fair template.
2. **Flash for persistence**, as `knx_flash_hal_t`
   (`include/knx/objects/flash_hal.h`): erase a page, program, read. If an
   erase stalls the CPU's own flash (it does on most single-bank parts), run
   the wait from RAM with interrupts masked, and pass the TP-UART backend to
   `FlashLog::setEraseGuard()` so the transceiver answers BUSY meanwhile.
3. **Time**: milliseconds and microseconds since boot (`CooperativeHooks`),
   readable from interrupts.

Then a start-up file, a linker script with a `.ramfunc` section in RAM and a
heap limit below the stack, and `_sbrk`. The size gate
(`test/baremetal/size_gate/`) builds the stack for Cortex-M0+, M3 and M4 in
CI, so the core and profile are covered before you start; what remains is
the board.

What to check on the hardware, in order:

1. The UART: the chip answers `U_Reset.req` (01h) with `U_Reset.ind` (03h).
   [docs/design/cortex-m-tpuart2-support.md](../../docs/design/cortex-m-tpuart2-support.md)
   §3.2 and §3.9 describe what a TP-UART2 does on the wire.
2. Time stamps: the acknowledge decision has to reach the chip within about
   1.7 ms of the frame's last octet. Keep the UART interrupt above everything
   but the tick.
3. Persistence: an individual address set by ETS survives a power cycle.
4. An ETS6 download, then group telegrams both ways.
