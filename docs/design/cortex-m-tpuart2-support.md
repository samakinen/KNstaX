# Design: Cortex-M bare-metal targets and the TP-UART2 backend

| | |
|---|---|
| Status | **Accepted** 15 November 2026, including the compact profile (K7) as KNstaX scope even though it changes core structures such as group object storage. The first target runs bare metal with no RTOS. **K1 is implemented** (§6.3). **K7 is started and the size gate (M1) is missed** (§9.1). Everything else is not implemented. Per DEVELOPMENT_GUIDELINES, README.md and the conformance status must not claim more than ships with tests. |
| Motivating product | H8i8o 8DI/8DO module: GD32F103CB (Cortex-M3, 128 KiB flash, 20 KiB RAM) + Siemens TP-UART2. Its design lives in the h8i8o repository, `docs/design/`. |
| Principle | Everything here is generic. A different board, MCU, I/O count or TP-UART-family chip should be able to use it unchanged. Board facts (pins, clocks, flash geometry) stay in the product repository. |

## 1. Why

KNstaX today runs on ESP-IDF with FreeRTOS and a bit-bang TP1 backend. A large class of KNX
devices instead combines a small Cortex-M (STM32F0/F1/G0/L0 and their GD32 clones) with a
TP-UART-family transceiver (Siemens TP-UART2, onsemi NCN5120/5121/5130, Elmos E981.03). Supporting
that class needs four things KNstaX does not have:

1. A build and runtime model without an RTOS and without threads.
2. A transceiver backend for the TP-UART host protocol.
3. Persistence without NVS.
4. A footprint that fits 128 KiB of flash and 20 KiB of RAM.

### 1.1 Measured starting point

Cross-compiling the TP1 core translation units (`KNX_SOURCES_CORE` + `tp1_mac_physical.cpp`)
with `arm-none-eabi-g++` 13.2 gives these results. Flags: `-mcpu=cortex-m3 -Os -fno-exceptions
-fno-rtti`, no KNXnet/IP, `KNX_FEATURE_COUPLER=0`. Measured at `d8f7eef`.

| | Code |
|---|---:|
| 46 objects, duplicated template code merged (`ld -r`), before `--gc-sections`/LTO | 177.3 KB |
| … plus Data Secure (no AES backend) | 183.6 KB |
| Largest: `dpt.o` 23.8 KB, `management_server.o` 13.2 KB, `security_interface_object.o` 8.9 KB (built without Secure), `interface_object_manager.o` 8.9 KB, `device_model.o` 8.5 KB, `device_management.o` 8.1 KB | |

Further findings from the cross-compile:

- **13 units do not compile** for arm-none-eabi as they are. They use `std::mutex`, which the
  single-threaded libstdc++ does not provide: `tp1_mac_physical.hpp:167`,
  `generic_interface_object.hpp:139`, `security_interface_object.hpp:377`,
  `two_port_coupler.hpp:199` and `util/log.cpp:92`, plus everything that includes them.
- **Standalone CMake** accepts only the linux, macos and windows platforms.
- **The TP1 source list hard-codes** the ESP-IDF bit-bang files.
- **The coupler sources** (`filter_table*`, `routing_table_control`, `coupler_routing`,
  `two_port_coupler`) are in `KNX_SOURCES_CORE`, so `KNX_FEATURE_COUPLER=0` gates behaviour
  but not code.
- **Group objects cost about 288 B each**, dominated by two 16-byte payload buffers. The ETS
  table segments take about 3.3 KB of RAM at their maximum sizes.

## 2. Work items

| ID | Item | Size | Depends on |
|---|---|---|---|
| K1 | Bare-metal Cortex-M build and a single-threaded `CooperativePlatform` | M | — |
| K2 | `knx_uart_hal_t`: a C byte-stream UART HAL | S | — |
| K3 | `TpUartMediumBackend`: TP-UART2, NCN512x-ready | L | K2 |
| K4 | TP-UART chip simulator for host tests | M | K3 |
| K5 | `FlashLogPersistence` over `knx_flash_hal_t` | M | K1 |
| K6 | Portable software AES-128 backend; separate Data Secure from IP-crypto build requirements | S | — |
| K7 | Compact device profile: size reduction and a linked size gate in CI | L | K1 |
| K8 | `knx/blocks`: reusable channel logic (button gestures, binary input, switch output) | M | — |
| K9 | Exporter: conditional communication objects, alternative object definitions, ETS channels | M | — |
| K10 | Power-fail save path for UART transceivers (`SAVE` pin, `U_State`) | S | K3, K5 |

Order: K1 and K7 first. They decide whether the target class is reachable at all, and they are
milestone M1, the size gate of the H8i8o plan. K2–K4 can proceed in parallel on the host.

## 3. K3: `TpUartMediumBackend`

### 3.1 Position in the stack

It is another `Tp1MediumBackend`, as ARCHITECTURE.md already anticipates ("a transceiver with
its own MAC … would be integrated as another `Tp1MediumBackend`").

- No change to `Tp1MacPhysical` or the single-TP1-architecture invariant.
- The factory gains a compile-time choice: Kconfig `KNX_TP1_TPUART` beside `KNX_TP1_BITBANG`,
  and a CMake equivalent.

```text
Tp1DataLinkLayer
  └ Tp1MacPhysical
      └ TpUartMediumBackend            (new; src/physical/tpuart_medium_backend.cpp)
          ├ TpUartRxEngine  (ISR side)  byte state machine, frame assembly, ACK decision
          ├ TpUartTxEngine  (task side) U_L_Data framing, confirmation, timeouts
          └ knx_uart_hal_t  (board)     bytes in from the ISR, bytes out
```

### 3.2 Protocol essentials

The values below are from the Siemens TP-UART2 technical manual. They are cross-checked against
`thelsing/knx`'s `tpuart_data_link_layer.cpp`, which is known to work with a TP-UART2.

| Item | Value |
|---|---|
| Host link | 19200 baud, 8E1 (verify against the chip's configuration on the board) |
| `U_Reset.req` / `.ind` | 0x01 / 0x03 |
| `U_State.req` / `.ind` | 0x02 / `xxxxx111`; bits 7–3 = slave collision, receive error, transmit error, protocol error, temperature warning |
| `U_SetAddress.req` | 0xF1 + address high + low. NCN5120: plus one dummy octet. |
| `U_AckInformation` | 0x10, ORed with: 0x04 = NACK, 0x02 = BUSY, 0x01 = addressed (acknowledge this frame) |
| `U_Busmon.req` | 0x05 |
| `U_L_DataStart` / `_Continue` / `_End` | 0x80, 0x80 \| index (1–63), 0x40 \| length (7–63); one data octet follows each |
| Repetition count | TP-UART2: `U_MXRSTCNT` (0x24) + one octet. NCN5120: `U_SetRepetition` (0xF2) + 3 octets. |
| `L_Data.con` | 0x8B positive, 0x0B negative (`(b & 0x7F) == 0x0B`, bit 7 = success) |
| Frame start (indication) | `(b & 0xD3) == 0x90` standard, `== 0x10` extended |
| Resynchronisation | No octet for more than 2 ms marks a frame end or garbage; discard any partial frame |

The backend is parameterised by a `TpUartFamily` traits type, chosen at compile time
(TP-UART2, NCN5120). It covers the differences: address dummy octet, repetition command and
configuration requests. A new family is then a traits type, not a fork.

### 3.3 Responsibilities

**What moves into the chip.** With a TP-UART, the chip owns everything the bit-bang ISR owns
today:

- bit timing;
- bus access and collision handling;
- sending the L2 acknowledge;
- observing the acknowledge of our own frame;
- **repetition**.

The last one forces a data-link change:

- `Tp1DataLinkLayer::pollTransmit` repeats on NACK, BUSY or a missing acknowledge (up to 3 +
  3 times). The chip repeats on its own as well, so the two together send a frame up to 16
  times.
- New capability: `Tp1CapabilityProfile::transceiverRepeats`. When it is set, the data link
  layer makes exactly one attempt and takes the chip's `L_Data.con` as the final outcome.
- The backend sets the chip's repetition counts to the KNX defaults (3 for BUSY, 3 for NACK).
- The bit-bang backend leaves the flag false and is unaffected.

**The acknowledge decision.**

| Destination | Who decides | How |
|---|---|---|
| Own individual address | Chip | `U_SetAddress`, called from `setOwnAddress()` |
| Group address in our table, or tunnel address | Host | `U_AckInformation` with the addressed bit, sent from the RX interrupt as soon as the destination octets (indication octets 3–4) and the address-type bit (octet 5) are in. The chip's deadline is the end of the frame; this leaves a margin of several octet times. |
| Not ours | Host | Nothing sent; the chip does not acknowledge |
| Local receive queue nearly full | Host | `U_AckInformation` with the busy bit (0x13), from `setLocalBusy()` state |

- **Lookup.** The group address lookup uses the table published through the existing
  `setAckGroupAddresses()`, sorted, with a binary search. At 256 entries that is at most 8
  comparisons. The ISR never takes a lock; publication swaps a pointer between two buffers.
- **Where it lives.** This is the existing contract: "the per-telegram ACK decision … has to be
  made within t_ack … which only the backend's timing domain (ISR, transceiver silicon) can
  meet" (ARCHITECTURE.md). Only the timing domain changes.
- **Coupler.** `TwoPortCoupler::setAckPolicyCallback` can finally be enforced here, because the
  decision is made in software per frame. Wire it as an optional hook. It is not needed for end
  devices.

### 3.4 Receive path (ISR side)

`TpUartRxEngine::onByte(uint8_t, uint32_t timestampUs)` is called from the UART RX interrupt.
It must not allocate, lock or log, and it runs from RAM where the platform requires it (K1,
`KNX_ISR_RAM`).

| State | On byte | Action |
|---|---|---|
| Idle | Frame start | Start a frame |
| Idle | `L_Data.con` | Post `TxAckResponse` with the confirmation result |
| Idle | `U_State.ind` | Post a state event |
| Idle | `U_Reset.ind` | Post a reset event |
| Idle | Anything else | Count it as unexpected |
| InFrame | Each octet | Append. After octet 5, make the ACK decision (§3.3). Once the length is known (octet 5 for standard frames, octet 6 for extended), the frame length is fixed. |
| InFrame | Final octet | Check the checksum. Post `TelegramEnd` with the full frame; a checksum failure posts a receive-integrity diagnostic instead. |
| Any | Gap > 2 ms (checked from the service step using the last byte's timestamp) | Discard any partial frame and go to Idle |

Events go through a fixed single-producer, single-consumer ring of frames: 4 frames × 64 B,
configurable. The main context drains it in `service()`, which emits `Tp1RxEvent`s exactly as
the bit-bang pump does. `Tp1MacPhysical` therefore sees the same event contract.

### 3.5 Transmit path (main context)

`sendFrame()` accepts one frame while the medium is idle. The whole frame is encoded as `U_L_Data`
request pairs into a TX buffer of at most 2 × 64 B, handed to the UART HAL, and the backend
enters Transmitting.

- **Outcome.** The positive or negative `L_Data.con`, posted by the RX engine, becomes the
  `TxAckResponse`.
- **Timeout.** No confirmation within 1 s is reported as a deadline miss. The backend then runs
  the reset sequence, because the chip is assumed lost.
- **Echo.** The TP-UART2 does not echo our own frame as an indication. Busmon mode is the
  exception.

### 3.6 Lifecycle and link state

| Step | Action |
|---|---|
| `init()` | `U_Reset.req`, wait for `U_Reset.ind` (10 ms, 3 attempts), set the address and repetition counts, `U_State.req` |
| Failure | State Error, `LinkState::Down` |
| `service()` | Polls `U_State` every second while idle, maps its warning bits, and runs the gap timeout |
| `LinkState` | `U_State` errors, or no response at all, give Down; the `SAVE` pin (K10), when the board provides one, gives Down. |
| Busmon | `setBusMonitorMode(true)` sends `U_Busmon.req`; leaving it needs a reset |

Capabilities:

```text
supportsHardwareAutoAck   = true
supportsDetailedTxConfirm = true
transceiverRepeats        = true   (new)
supportsLinkStateIndication = true
```

### 3.7 Not reused

The TP-UART backend removed in 5669ac0 is not a base. Review finding B9 found its TX framing,
its `L_Data.con` code and its receive start detection wrong, and it was never run against a
chip. What is worth keeping:

- the shape of its adapter tests;
- its notes mapping `SAVE` and `U_State` to link state, already reflected in `link_state.hpp`.

## 4. K2: UART HAL

A C interface in the style of `timer_gpio_hal.h`, so a board can implement it without C++:

```c
typedef struct knx_uart_hal {
    void*   context;
    int     (*open)(void* ctx, uint32_t baud, knx_uart_parity_t parity,
                    knx_uart_rx_fn on_rx, void* rx_arg);        /* on_rx called from ISR, one octet + error flags */
    int     (*write)(void* ctx, const uint8_t* data, size_t len); /* non-blocking, copies; <0 if no room */
    size_t  (*tx_free)(void* ctx);
    void    (*close)(void* ctx);
    int     (*set_reset_pin)(void* ctx, bool asserted);         /* optional; NULL if not wired */
    int     (*read_save_pin)(void* ctx);                        /* optional; NULL if not wired */
} knx_uart_hal_t;
```

The RX callback carries parity and framing error flags, so the RX engine can count them per
octet. The ESP-IDF build gets an implementation too. That lets an ESP32 + TP-UART or NCN5120
board use the backend, and it is the second user that keeps the interface honest.

### 4.1 Status of K2 (15 November 2026)

Done. The interface is `include/knx/physical/uart_hal.h`; the header, not the sketch above, is
the contract. It differs from the sketch in these ways:

- **Shape.** It follows `timer_gpio_hal.h`: a context plus an ops struct, `bool` results and
  inline helpers that check for a valid HAL. The line format is a `knx_uart_hal_config_t`
  (baud and parity; always 8 data bits and 1 stop bit).
- **Timestamps.** The RX callback receives each octet with its error flags (parity, framing,
  noise, overrun) and a wrapping 32-bit microsecond timestamp taken by the board. The RX engine
  needs it for the 2 ms gap, and the board's interrupt is the only place it is accurate.
- **Pins.** The optional pins are `set_reset(asserted)` and `read_save()`. They are logical
  (the board applies polarity), and either may be NULL.
- **Concurrency.** `write()` is all-or-nothing. It may be called from the RX callback, and the
  octets of one call stay contiguous against the other context.

Tests and implementations:

- `UartHalVirtual` (host only, in the virtual sources) plays the far end. It delivers octets to
  the callback, decides when queued TX octets leave the wire, and records which writes came
  from the RX callback. K4 builds the chip simulator on it.
- `test_uart_hal_virtual` covers the contract. It includes a small board implementation written
  in C (`test/unit/uart_hal_c_board.c`), which proves the header works from C.
- The header also compiles with `-Werror` as C99 and as C++23 for Cortex-M3.

Not done:

- **The ESP-IDF implementation.** A per-octet RX callback needs a custom UART interrupt rather
  than the IDF UART driver, and no ESP32 + TP-UART board exists to test it on. It waits for
  one.

**Constraint for K3.** An octet written from the RX callback queues behind whatever the main
context has already queued. If the TX engine handed a whole frame (up to 2 × 64 octets, about
70 ms at 19200 baud) to the HAL at once, a `U_AckInformation` written while a frame arrives would
reach the chip long after its deadline. The TX engine must therefore feed `U_L_Data` pairs
incrementally and keep only a few octets queued. §3.5 is to be read with this in mind.

## 5. K4: TP-UART chip simulator

`VirtualTpUartChip` sits between the backend's UART HAL and the existing virtual TP1 bus
(`-L tp1-virtual`):

- It reproduces the chip's host protocol: reset, state, address, `U_AckInformation` timing,
  repetitions and confirmations.
- It drives real bus frames on the virtual bus.
- It supports fault injection: lost and corrupted octets, a missing `L_Data.con`, BUSY or NACK
  from bus partners, a late `U_AckInformation`, and a chip reset mid-frame.

Every behaviour in §3 gets a test under a new ctest label `tpuart`. The interop harness can then
run the full stack over the simulated chip against `xknx`, as it does for bit-bang.

## 6. K1: bare-metal build and `CooperativePlatform`

### 6.1 Build

- **Standalone CMake** gains `KNX_PLATFORM=baremetal`.
  - It builds no platform sources and no host-only groups (virtual bus, ETS tooling, test
    hooks).
  - It expects the product to provide the platform hooks.
  - It ships `cmake/toolchains/arm-none-eabi.cmake` as an example; the product chooses its own
    `-mcpu`.
- **Source lists.** `KNX_SOURCES_TP1` splits into a common part (`tp1_mac_physical`,
  `physical_factory`) and per-backend groups:
  - `KNX_SOURCES_TP1_BITBANG_ESPIDF`;
  - `KNX_SOURCES_TP1_TPUART`.

  Only the selected backend's group is built.

### 6.2 Threading

- **Locking.** Replace direct `std::mutex` members with `knx::platform::Mutex`, a type alias the
  build chooses: `std::mutex` on hosts, a FreeRTOS-backed lock on ESP-IDF, and a no-op type on
  single-threaded targets.
- **Interrupt data.** Data shared with an interrupt keeps using critical sections through
  `lockFromTask()`-style policies; no mutex involved.
- **`CooperativePlatform`** implements the narrow interfaces (`TimingPlatform`,
  `QueuePlatform`, `MutexPlatform`) for one thread:
  - queues are fixed rings;
  - mutexes are no-ops;
  - task creation is refused with `ErrorCode::OperationNotSupported`;
  - task notify from an ISR sets a flag the main loop checks.
- **No tasks.** `Tp1DataLinkLayer` runs with `enableRxTask = false` and is driven through
  `processRxAvailable()`. That path exists but is untested today; it gets the same test suite
  as the task path.
- **`KNX_ISR_RAM`.** A new placement macro: `IRAM_ATTR` on ESP-IDF,
  `__attribute__((section(".ramfunc")))` on Cortex-M, empty on hosts. Applied to the TP-UART RX
  engine and the acknowledge lookup.

### 6.3 Status of K1 (16 November 2026)

Implemented and tested:

- `KNX_PLATFORM=baremetal` in the standalone CMake, `cmake/toolchains/arm-none-eabi.cmake`, and the TP1 source list split into `KNX_SOURCES_TP1_COMMON` and `KNX_SOURCES_TP1_BITBANG_ESPIDF`. A bare-metal build contains no host-only groups and no bit-bang backend.
- `platform::LocalMutex` / `LocalLock` in place of `std::mutex` in the core. All 46 core translation units compile for Cortex-M3 without a stand-in.
- `CooperativePlatform` (`test_cooperative_platform`, on the host).
- `Tp1DataLinkLayer` defaults to no RX task in this mode. `processRxAvailable()` was already called from the BAU's background work.
- A product-supplied persistence through `objects::setPersistenceFactory()` (`test_persistence_factory`, on the host). The file backend (`<fstream>`, `<filesystem>`) is no longer compiled in.
- Two defects the cross-build exposed, both fixed for every platform: `Tp1MacPhysical` was never given the platform's clock by the stack port, so on a target without a system clock its timeouts could not expire; and `util::nowMs()` / `delayMs()` fell back to `std::chrono` / `std::this_thread`, which the target lacks.

The host suite (149 tests, excluding the Long, benchmark and interop labels) passes with these changes.

Not done:

- `KNX_ISR_RAM`, which the TP-UART RX engine (K3) needs.
- On-target verification. The reference device has only been run under QEMU (Cortex-M3, `mps2-an385`), to measure heap use.

## 7. K5: `FlashLogPersistence`

An `ObjectPersistence` implementation over:

```c
typedef struct knx_flash_hal {
    void*    context;
    uint32_t page_size;  /* e.g. 1024 */
    uint32_t base;       /* first page of the region */
    uint32_t page_count; /* even; split into two banks */
    uint32_t write_unit; /* 2 on STM32F1/GD32F1, 8 on STM32G0/L4 */
    int (*erase_page)(void* ctx, uint32_t addr);
    int (*program)(void* ctx, uint32_t addr, const void* data, size_t len); /* multiple of write_unit */
    int (*read)(void* ctx, uint32_t addr, void* out, size_t len);
} knx_flash_hal_t;
```

**Log format.** One bank is active and the other is erased. Records are
`{id: u16, length: u16, data, crc16}`, padded to `write_unit`, and the latest record for an id
wins.

- **Compaction.** When the active bank is full, the latest record of every id is copied to the
  other bank, a bank header with a sequence number is written last, and the old bank is erased.
- **Erase timing.** The erase runs from `service()` in its own step, never inside a save. The
  "always pre-erased space" invariant is what makes a power-fail save possible (K10).
- **Recovery.** At boot, choose the valid bank with the highest sequence number, and ignore a
  torn tail record (CRC mismatch).
- **Keys.** Use `saveById`/`loadById` (u16 ids). The `std::string` key surface stays for hosts
  and NVS. `listKeys()` is not needed on this path.
- **Tests.** A host flash simulator cuts power at every program and erase step and checks that
  the last committed state survives.

## 8. K6: software AES-128

- **Backend.** `aes_block.hpp` gains a third backend: a portable, constant-time-leaning,
  encrypt-only AES-128 of about 1.5–2.5 KB, used when neither mbedTLS nor the ESP AES driver is
  present. Data Secure needs only encryption: CCM is built from CBC-MAC and CTR.
- **Validation.** It is checked against the FIPS-197 vectors and the existing 03/03/07 Annex C
  CCM vectors.
- **Build requirements.** The standalone CMake requires mbedTLS PBKDF2 and MD headers as soon as
  `KNX_SECURE_ENABLED` is on. Split the requirement so that Data Secure alone
  (`KNX_SOURCES_SECURE_DATA`) needs only an AES backend, and IP Secure (`SECURE_IP_CRYPTO`)
  needs mbedTLS.
- **Randomness.** `S-A_Sync` challenges need unpredictable values. `Platform::randomBytes()` is
  documented as not for key material. That remains true; boards without a TRNG must seed it
  from real noise, which is the product's job.

## 9. K7: compact device profile

> **Superseded by [small-target-profile.md](small-target-profile.md)** (16 November 2026). After the M1 measurement in §9.1, K7 is replaced by a build-time configuration system (Kconfig for every build, profiles, capacities as options) plus four structural changes. The table below is kept as the original plan.

Goal: a TP1 end device with ≤ 60 group objects links into **≤ 88 KiB of flash and ≤ 10 KiB of
RAM** (static plus heap high-water mark) for the KNstaX share on Cortex-M3 at `-Os` with LTO.
The numbers are those of the H8i8o budget; the mechanisms are generic.

| Step | Change | Expected effect |
|---|---|---|
| 1 | DPT codecs selected from the product definition's DPT set at compile time; the catalogue table stays for the exporter | Up to −20 KB for a float-free product |
| 2 | Coupler, filter table and routing table sources gated by `KNX_FEATURE_COUPLER` | Several KB; exact figure from M1 |
| 3 | `SecurityInterfaceObject` and Group Object diagnostics gated by their features | About −9 KB and −4 KB |
| 4 | Group objects store values in buffers sized by their DPT at compile time; the "last sent" buffer only for objects with send-on-change | 288 B → ≤ 32 B per object |
| 5 | Table capacities (address, association, group object) set by the product, not the maximums | −1 to −2 KB RAM |
| 6 | `KNX_LOG_LEVEL=0` compiles out every log string | Several KB |
| 7 | LTO-clean build (no ODR issues), plus a CI job that links a reference Cortex-M3 device and checks flash and RAM budgets from the ELF | Prevents regressions |
| 8 | Heap policy: allocate during `init()`, then `knx::heap::lock()`. Debug builds trap on any later `operator new` and report the high-water mark. | Bounded RAM, no fragmentation |

`scripts/ci/knstax_size_budget.py` parses ESP-IDF maps today. Step 7 extends it to read an ELF
(`.text`, `.data`, `.bss` per archive member) and to check separate flash and RAM budgets.

All steps keep the ESP32 and host builds behaving the same. They are a feature diet selected by
configuration, not a fork.

### 9.1 M1 result (16 November 2026): the budget is missed

Reference device: `test/baremetal/size_gate`, a TP1 end device with 60 group objects (40 x DPT 1, 8 x DPT 5, 4 x DPT 3, 4 x DPT 17, 4 x DPT 18; no float type) and a null medium backend, started through `startCommissionedProduct()`. Cortex-M3, `-Os`, LTO, `--gc-sections`, newlib-nano, logging compiled out. Run `test/baremetal/size_gate/run_gate.sh`.

Measured at KNstaX `ef4ed60` plus the K1 changes, **before K7 steps 1-5 and 8** (steps 6 and 7 are in the gate's configuration):

| | Measured | Budget | Over by |
|---|---:|---:|---:|
| KNstaX flash (stack, product runtime and start-up glue) | **132.4 KiB** | 88 KiB | +44.4 KiB |
| KNstaX RAM, static | 1.7 KiB | | |
| KNstaX RAM, heap high-water after `startCommissionedProduct()` | **65.1 KiB** | | |
| KNstaX RAM, static + heap | **66.9 KiB** | 10 KiB | +56.9 KiB |
| C/C++ runtime flash | 11.6 KiB | 8 KiB | +3.6 KiB |

Where the heap goes. The high-water mark is reached during start-up and does not grow over 200 further loop iterations with the null backend. Traffic buffers on a real bus are not included.

| Allocation | Bytes |
|---|---:|
| 61 x group object (`sizeof` 248 B) | 15 128 |
| `CommissionedProductRuntime` (60 bindings) | 12 184 |
| `Tp1MacPhysical` (receive frame ring) | 8 488 |
| `Tp1DataLinkLayer` | 5 752 |
| `InterfaceObjectManager` | 5 424 |
| `BusAccessUnit` | 5 312 |
| Association table (maximum capacity) | 3 472 |
| `TransportLayer` | 2 880 |
| Group object table, application layer, other | about 8 000 |

Group objects (step 4) are therefore about a quarter of the heap, not nearly all of it as the plan assumed. The stack's own structures are the larger part, and most of them are sized by the maximum capacities in `config.hpp`, which is step 5's territory.

Flash, from the symbols in the LTO image: the product runtime templates (`EndpointRuntime`, the start-up inlined into `main`, bindings and port specs) come to about 32 KB, roughly 500 B per port. They are instantiated once per port, and none of K7's eight steps addresses them. `BusAccessUnit` (8 KB), `ApplicationLayer` (6 KB), `BusAccessStackPort` (5 KB), `Tp1DataLinkLayer`, `DeviceManagement` and `TransportLayer` (4 KB each) follow.

Upper bounds for the K7 steps not yet applied, from the symbols the LTO image keeps (a step cannot save more than this):

| Step | Flash | RAM |
|---|---:|---:|
| 1. DPT codecs from the product | at most 8.5 KiB (dispatch 2, catalogue 1.7, soft-float 4.8) | |
| 2. Coupler sources gated | at most 5.5 KiB | |
| 3. Security Interface Object and GO diagnostics gated | at most 3.2 KiB | |
| 4. Compact group objects (248 B to 32 B or less) | | about 13 KiB |
| 5. Table and queue capacities from the product | | several KiB (`Tp1MacPhysical`, data link layer, association table) |

Together that is at most about 17 KiB of flash, which would leave about 115 KiB against a budget of 88, and a RAM figure in the tens of KiB against a budget of 10. Steps 1-5 are therefore not enough, and the first decision gate is **no-go for the plan as written**. These are estimates, not measurements. The steps were not applied, because the owner decides what happens after a missed gate (H8i8o design, section 11). The fall-backs in H8i8o design section 4.3 apply.

The probe of 15 November (`tools/size-probe` in h8i8o) was optimistic for two reasons. It passed `-DKNX_LOG_LEVEL=1`, which nothing reads (the macro is `KNX_LOG_LEVEL_COMPILE`), and it measured no RAM at all.

### 9.2 Size gate tooling

- `test/baremetal/size_gate/`: the reference device, memory map, start-up code and one budget file per reference configuration (`budgets/<profile>-<core>.json`: `tp1-small` on Cortex-M3, `tp1-standard` on Cortex-M4, `tp1-small` on Cortex-M0+). Each figure has an enforced `budget` and a design `target`. The memory map is larger than the real part on purpose, so that an over-budget image still links and the script can say by how much.
- `scripts/ci/knstax_size_gate.py`: reads the linker map and splits the KNstaX share from the C/C++ runtime. libc and libstdc++ members stay attributable under LTO; KNstaX does not, so its share includes a few lines of glue. It checks flash and RAM against the budget, prints the distance to the target and a note when a figure is well below its budget (the ratchet), and can print the change against an earlier run (`--baseline`). It is a new script because `knstax_size_budget.py` reads ESP-IDF maps and counts one archive; the ESP-IDF gate is unchanged.
- `run_gate.sh <build-dir> [profile [cpu]]`: builds the LTO image for the profile and core, runs a semihosting variant under QEMU for the heap high-water mark (Cortex-M3 only; with an allocation-size profile), prints the resolved `knx.config`, and runs the script against the matching budget file. It exits 1 when over budget. CI runs it as the `size-gate` job, one matrix entry per reference configuration (small-target-profile.md §7, C4 status).

## 10. K8: `knx/blocks`

Board-independent, host-testable application logic that any KNX I/O product needs. It is driven
by a 1 ms tick and a level or command input, and outputs requests to publish or switch.

| Block | Purpose |
|---|---|
| `ButtonGesture` | Debounced press/release → short, long, long-release and double events, with configurable thresholds. Also serves programming buttons. |
| `BinaryInputChannel` | `ButtonGesture` plus the function logic of a binary input: switch edges, short/long press, one- and two-button dimming and shutter, value, scene, lock, send after recovery |
| `SwitchOutputChannel` | Priority evaluation (forced/lock > staircase > switch/central/scene), on/off delays, staircase timer, behaviour after power failure, recovery and download, rate limit, status |

- **Interfaces.** Blocks talk to the product through small callbacks (`InplaceFunction`) and
  never touch GPIO. Their parameters are plain structs, filled from the product's parameter
  schema.
- **Tests.** Each block gets timeline tests: recorded inputs over virtual time, compared with
  expected telegrams.

## 11. K9: exporter

What a channelised I/O product needs from the `.knxprod` exporter, beyond today's groups,
enumerations and `<choose>` parameter visibility:

1. **Conditional communication objects.** Wrap `ComObjectRefRef` in `<choose>` on a controlling
   parameter, so objects a channel's function does not use are hidden.
2. **Alternative object definitions on one object number.** For example, object A of an input is
   DPT 1.001 for "Switch" and DPT 18.001 for "Scene". The product model declares the
   alternatives; the runtime binds the one the parameter selects at `init()`.
3. **ETS `<Channel>` elements**, per channel, with a text that can include a parameter (for
   example "Input 3: Kitchen"). Today the exporter emits a single `ChannelIndependentBlock`.
4. **A repeated channel template:** declare one channel and instantiate it *n* times with offset
   object numbers and parameter memory, so an 8-channel product is not 8 copies in source.

Each is checked against ETS6 import on hardware, the same bar the existing exporter met.

## 12. K10: power-fail save

For UART transceivers, the `SAVE` output (where wired, read through
`knx_uart_hal_t::read_save_pin` or an edge interrupt the product forwards) and `U_State`
warnings feed `LinkMonitor`.

- **Early warning.** The existing `LinkEventKind::PowerFailImminent` fires from the edge without
  debouncing.
- **Save path.** Products register a small save routine that writes a few records into
  `FlashLogPersistence`'s pre-erased space, with no erase allowed on this path.
- **Data Secure.** The sending sequence number checkpoint (today at most once a minute) should
  also be written here. That closes the "replayable once until the next S-A_Sync" exposure
  documented in the conformance status for devices that have this signal.

## 13. Documentation and conformance updates when items land

| Item lands | Update |
|---|---|
| K3/K4 | `knx_conformance_status.md` Media table (TP1 TP-UART: implemented, host-tested, then HIL); ARCHITECTURE.md "Other TP1 backends"; Kconfig help; board bring-up guide gains a TP-UART section |
| K1 | ARCHITECTURE.md platform boundary and execution model (cooperative mode); README targets |
| K5–K7 | product authoring guide: group object memory, persistence on MCUs without NVS, the compact profile |
| K9 | ETS export guide |
