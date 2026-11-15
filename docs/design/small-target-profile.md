# Design: build-time configuration and the small-target profile

| | |
|---|---|
| Status | **Proposed**, 16 November 2026. Work item C1 (§9) is implemented, see "C1 status"; nothing else here is. |
| Goal | KNstaX runs as a TP1 end device on a Cortex-M with **128 KiB of flash and 20 KiB of RAM**, and every other target chooses, at build time, the features and capacities it needs. |
| Motivating product | H8i8o (GD32F103CB + TP-UART2), the only STM32-class TP-UART board available for testing. Its design is in the h8i8o repository, `docs/design/`. |
| Builds on | [cortex-m-tpuart2-support.md](cortex-m-tpuart2-support.md): K1 (bare-metal build, done), K7 (compact profile, which this document replaces and extends), and the M1 measurement in its §9.1. |
| Project standing | Making KNstaX work on STM32-class devices is a project goal in its own right, at least as important as the H8i8o firmware. Everything here is generic. |

## 1. Why

The first size gate (M1, §9.1 of the Cortex-M design) measured a 60-object TP1 end device at
**132.4 KiB of flash and 66.9 KiB of RAM**, against a budget of 88 KiB and 10 KiB for the KNstaX
share. The plan behind K7 assumed that the gap was mostly feature code and group-object size.
The measurement shows four kinds of cost, and only two of them can be configured away:

| Cost | Example (measured) | Remedy |
|---|---|---|
| Features the device does not use | Coupler and router, Security Interface Object, GO diagnostics, the whole DPT catalogue and soft-float: up to about 17 KiB of flash | **Configuration:** gate them (§4.1) |
| Capacities at their maximums | TP1 receive ring 8.5 KiB, data link layer 5.8 KiB, association table 3.5 KiB, frame buffers sized for 254-byte APDUs | **Configuration:** size them per device (§4.2) |
| Code generated per port | Product runtime templates: about 500 B of flash per port, 32 KB for 60 ports | **Structure:** table-driven runtime (§6.1) |
| Per-object and per-property overhead | 248 B per group object, of which two 64-byte callbacks; map- and vector-based property storage; product runtime 12 KiB of RAM | **Structure:** §6.2 to §6.4 |

Configuration alone would leave roughly 110 KiB of flash and 30 to 40 KiB of RAM (an estimate
from the measured breakdown). Both halves are needed.

## 2. Target tiers

| Profile | Example parts | KNstaX flash | KNstaX RAM (static + heap) | Status of the budget |
|---|---|---:|---:|---|
| `tp1-small` | STM32F103C8/CB, GD32F103CB, STM32G030/G070 (128 KiB / 20 KiB) | ≤ 88 KiB | ≤ 10 KiB | **Hard target.** The H8i8o board, the reference for every change. |
| `tp1-standard` | STM32G0B1, STM32F4, GD32F303CC (256 KiB / 48–64 KiB) | ≤ 140 KiB | ≤ 32 KiB | Guaranteed tier; room for Data Secure and debug logging |
| `full` | ESP32 family, hosts | not gated in this way | | Today's behaviour; the ESP-IDF image gate stays as it is |

The `tp1-small` budget is the same split as the H8i8o plan (h8i8o `01-system-design.md` §4.2): it
leaves 8 KiB for the C/C++ runtime, 12 KiB for the application and 4 KiB for the board
support, with 8 KiB of flash for persistence. Data Secure is a stretch goal on `tp1-small` and
a normal feature on `tp1-standard`.

## 3. Configuration system: one schema for every platform

### 3.1 Kconfig as the only source

Today there are three overlapping mechanisms:

1. Kconfig, for ESP-IDF builds only (`CONFIG_KNX_*`);
2. standalone CMake options (`KNX_FEATURE_*`, `KNX_SECURE_ENABLED` and others);
3. `#ifndef` defaults and fixed `constexpr` values in `config.hpp` and in individual headers.

The proposal is that the existing `Kconfig` file becomes the only schema, for every build:

- **ESP-IDF:** unchanged. `idf.py menuconfig` writes `sdkconfig.h`.
- **Standalone CMake** (bare metal and hosts): the build runs Kconfiglib over the same
  `Kconfig` file, using the profile and the product's overrides, and generates:
  - `knx/autoconf.h`, with the same `CONFIG_KNX_*` macros ESP-IDF produces;
  - `knx_config.cmake`, read back by `CMakeLists.txt` to choose the source groups.

  Kconfiglib is a single Python file under the ISC licence, which is compatible with the
  GPL. It is vendored in `tools/kconfig/` so that the build does not need `pip`. Python is
  already a build requirement for the exporter.
- **`config.hpp`** becomes a thin layer over `CONFIG_KNX_*`. It holds no defaults of its own:
  a missing symbol is a build error, not a silent fallback. The `KNX_FEATURE_*` macros the
  code uses today are derived from the `CONFIG_*` ones there, so the change does not touch
  every `#if`.

The existing standalone options (`-DKNX_FEATURE_NETIP=OFF` and so on) keep working for one
release as aliases that write the matching Kconfig symbol, and then go.

### 3.2 Profiles and products

- **Profiles** are defconfig fragments in `configs/profiles/`: `tp1-small.defconfig`,
  `tp1-standard.defconfig`, `full.defconfig`, `host-test.defconfig`.
- **A product** names a profile and adds its own fragment for anything it changes:

```cmake
knx_configure(PROFILE tp1-small
              OVERRIDES ${CMAKE_CURRENT_SOURCE_DIR}/knx.defconfig)
add_subdirectory(${KNSTAX_DIR} knstax)
```

- `menuconfig` and `guiconfig` from Kconfiglib work on standalone builds too
  (`cmake --build build --target knx_menuconfig`).
- **The resolved configuration is an artefact.** The build writes it next to the image as
  `knx.config`, and the size gate prints it, so a size figure always names the configuration
  it belongs to.

### 3.3 Rules every option follows

1. **Every option has a value in every profile.** No option relies on a fallback in a header.
2. **A capacity is a promise.** When a table or queue is full, the stack returns an error
   (`StorageFull`, `ResourceUnavailable`) and, where KNX defines one, the matching negative
   response. It never overwrites or grows silently. A host test per capacity proves it.
3. **The `.knxprod` exporter and the firmware read the same configuration.** The table sizes
   ETS is told about (address table, association table, group object table, maximum APDU
   length) come from the same symbols as the firmware's buffers. Otherwise ETS downloads a
   table the device cannot hold. The exporter gets the resolved `knx.config` as input.
4. **A feature that is off is absent, not inert.** Its sources are not compiled. The size gate
   checks this with `--forbid` patterns per profile, as the ESP-IDF gate already does.
5. **Values the product already knows come from the product.** See §5.

## 4. Option catalogue

Symbols follow the existing `KNX_` naming. The defaults below are a starting point; the size
gate decides the final `tp1-small` values.

### 4.1 Features (code)

| Symbol | Gates | `tp1-small` | `tp1-standard` | `full` |
|---|---|:-:|:-:|:-:|
| `KNX_MEDIUM_TP1`, `KNX_MEDIUM_IP` | Media (exists) | TP1 | TP1 | both |
| `KNX_TP1_BITBANG`, `KNX_TP1_TPUART` | TP1 backend (choice; TP-UART is K3) | TP-UART | TP-UART | bit-bang |
| `KNX_ENABLE_COUPLER` | Router object, filter and routing tables, coupler routing, two-port coupler. Today it gates only the acknowledge hook. | n | n | y |
| `KNX_ENABLE_SECURITY` | Data Secure, Security Interface Object (exists, widened) | n | y | y |
| `KNX_ENABLE_GO_DIAGNOSTICS` | Group object diagnostics function property | n | y | y |
| `KNX_ENABLE_MEMORY_EXT` | `A_MemoryExtended_*` services | n | y | y |
| `KNX_ENABLE_PROPERTY_EXT` | `A_PropertyExt*` services (needed with Secure) | n | y | y |
| `KNX_ENABLE_NETWORK_PARAMETER` | `A_NetworkParameter_*` | n | y | y |
| `KNX_ENABLE_FUNCTION_PROPERTIES` | Function property services; GO diagnostics and the router need them | y | y | y |
| `KNX_DPT_FLOAT`, `KNX_DPT_TIME`, `KNX_DPT_STRING`, `KNX_DPT_COLOUR`, `KNX_DPT_ACCESS` | DPT codec families. Integer and boolean types are always in. | n (or from the product, §5) | y | y |
| `KNX_ENABLE_DPT_CATALOGUE` | The runtime catalogue of every known DPT. The exporter has its own copy. | n | n | y |
| `KNX_ENABLE_ETS_TOOLING`, `KNX_ENABLE_VIRTUAL_BUS`, `KNX_ENABLE_TEST_HOOKS` | Host-only groups (exist as CMake options) | n | n | host only |
| `KNX_LOG_LEVEL` | Compile-time logging (exists) | 0 | 1 | 3 |

**Which ETS-mandatory services stay.** A TP1 end device that ETS commissions needs the
individual address services, connection-oriented transport, `A_Memory_*`, `A_PropertyValue_*`,
`A_DeviceDescriptor_Read`, `A_Restart`, `A_Authorize` and the load-state machines. These are
not optional on any profile. Each optional service above is checked against an ETS6 download
on hardware before its `tp1-small` default is fixed at `n`.

### 4.2 Capacities (RAM)

| Symbol | Replaces | `tp1-small` | `tp1-standard` | `full` |
|---|---|---:|---:|---:|
| `KNX_MAX_APDU_LENGTH` | `config::MAX_APDU_LENGTH` (254) | **15** | 55 | 254 |
| `KNX_TP1_RX_QUEUE_FRAMES` | `Tp1MacPhysical::RX_QUEUE_CAPACITY` (32) | 4 | 8 | 32 |
| `KNX_TRANSPORT_RX_QUEUE` | `TransportLayer::RX_QUEUE_CAPACITY` (8) | 2 | 4 | 8 |
| `KNX_ADDRESS_TABLE_ENTRIES` | `AddressTableObject::kMaxEntries` (256) and `Tp1DataLinkLayer::MAX_GROUP_ADDRESSES` (256) | 128 | 255 | 256 |
| `KNX_ASSOCIATION_TABLE_ENTRIES` | `AssociationTableObject::kMaxEntries` (512) | 160 | 400 | 512 |
| `KNX_ADDITIONAL_INDIVIDUAL_ADDRESSES` | `MAX_ADDITIONAL_INDIVIDUAL_ADDRESSES` (16) | 0 | 4 | 16 |
| `KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES` | (exists) | 14 | 14 | 16 |
| `KNX_SEND_PIPELINE_DEPTH` | A_send pipeline queue | 2 | 4 | 8 |
| `KNX_APPLICATION_SEGMENT_BYTES` | Parameter memory held in RAM | from the product (§5) | | |

**Maximum APDU length** is the largest single lever. With `KNX_MAX_APDU_LENGTH = 15` (standard
frames only), every frame buffer, receive ring slot and transport queue entry shrinks from
about 263 bytes to 23. The device reports 15 in `PID_MAX_APDU_LENGTH`, and ETS then downloads
in 12-byte memory writes instead of larger ones: slower, but correct. Extended frames addressed
to the device are rejected at the data link layer; group telegrams up to DPT 16 (14 bytes) still
fit in a standard frame.

**One copy of the group address list.** The address table object and the data link layer each
keep the group addresses today. On `tp1-small` the data link layer reads the address table's
sorted storage directly (the acknowledge lookup needs only a sorted span), which removes one
copy.

### 4.3 Policies

| Symbol | Choices | `tp1-small` |
|---|---|---|
| `KNX_PLATFORM` | `espidf`, `linux`, `macos`, `windows`, `baremetal` (exists as a CMake variable) | `baremetal` |
| `KNX_HEAP_POLICY` | `free` (today), `init_only` (allocation after `heap::lock()` traps in debug builds and fails in release), `arena` (one static arena of `KNX_HEAP_ARENA_BYTES`, no `malloc`) | `init_only` |
| `KNX_PERSISTENCE_BACKEND` | `nvs`, `file`, `flash_log` (K5), `product` (factory only) | `flash_log` |
| `KNX_CALLBACK_CAPTURE_BYTES` | Default capture size of `InplaceFunction` callbacks inside the stack (64 today) | 8 |

## 5. Values that come from the product

The product definition is `constexpr` and already knows a lot. Where it does, the product is
the source and the profile only sets a ceiling:

| Value | From the product | Check |
|---|---|---|
| Group object count | Number of ports | `static_assert(ports ≤ CONFIG_KNX_MAX_GROUP_OBJECTS)` |
| DPT families linked | The DPT ids of its ports | A port whose DPT family is configured out fails to compile, with a message naming the symbol to enable |
| Value bytes per object | DPT size | Sizes the value pool (§6.2) |
| Maximum APDU | `ProductIdentity::maxApduLength` | Must not exceed `CONFIG_KNX_MAX_APDU_LENGTH` |
| Parameter memory | Parameter schema | Sizes the application segment held in RAM |

The DPT selection works by giving each port its codec at compile time (§6.1), so the dynamic
`encodeDptValue` / `decodeDptValue` dispatch over the whole catalogue is not linked on
`tp1-small` at all.

## 6. Structural changes

These cannot be configured; they change how the code is shaped. All of them are generic and
also reduce the size of the ESP32 builds.

### 6.1 S1: table-driven product runtime (flash)

**Today.** `EndpointRuntime` and the binding machinery are templates instantiated per port, so
each port brings its own copy of the publish, receive, persistence and read-response code:
about 500 B of flash per port.

**Proposed.** The product definition compiles to one `constexpr` array of port descriptors:

```cpp
struct PortDescriptor {
    uint16_t objectNumber;
    application::DptId dpt;
    uint8_t valueBytes;
    uint8_t codec;        // index into the codec table the product links
    uint8_t flags;        // C/R/W/T/U/I + priority
    uint8_t persistence;  // none / state / command
};
```

A single, non-template runtime walks this table. The typed API the product uses
(`publish<Port::X>(bool)`, `onStateWrite<Port::X>(…)`) stays the same, but becomes a thin inline
wrapper that encodes with the port's codec and calls the generic runtime with the port's
index. The per-port binding callbacks move into one array of function pointer plus context.

*Expected effect:* about 25–30 KB of flash for 60 ports. This is the largest single item.
*Compatibility:* the product authoring API is unchanged. The exporter reads the same
descriptors.

### 6.2 S2: compact group objects (RAM; K7 step 4)

**Today.** `GroupObject` is 248 B: configuration with a `double` threshold, two 16-byte value
buffers, and two `InplaceFunction`s of 64 bytes each.

**Proposed.** The runtime owns group-object state as one table:

```cpp
struct GroupObjectState {          // 8 bytes
    uint16_t valueOffset;          // into the value pool
    uint8_t  descriptorIndex;      // into the constexpr PortDescriptor table
    uint8_t  flags;                // valid, pending send, update received, ...
    uint32_t lastSentMs;           // only where a transmit policy is set (side table otherwise)
};
```

- **Values** live in one byte pool sized from the DPTs. The "last sent" copy exists only for
  objects with send-on-change.
- **Transmit policies** (threshold, cyclic, minimum interval) go in a side table for the
  objects that have one. The threshold becomes an integer in the DPT's raw units, which removes
  the soft-float dependency.
- **Callbacks:** one for the whole table, called with the object index.

*Expected effect:* about 13 KiB of RAM for 60 objects (248 B to about 10–16 B each).

### 6.3 S3: interface object property storage (RAM and flash)

**Today.** `GenericInterfaceObject` stores values in
`std::map<PropertyID, std::vector<uint8_t>>`, with registration tables built at run time.

**Proposed.** Property declarations become `constexpr` tables, as most of them already are in
source. Values live in one flat buffer per object, sized from the declarations. Persistence
keys become the existing `u16` ids, and the `std::string` key surface is no longer needed on
small targets. This also removes `std::map` and `std::string` from the steady-state code paths.

### 6.4 S4: allocation policy (RAM; K7 step 8)

With `KNX_HEAP_POLICY=init_only`, all allocation happens while `startCommissionedProduct()`
runs. `knx::heap::lock()` then makes further allocation an error, a trap in debug builds, and
the high-water mark is reported. With `arena`, `operator new` serves from one static block and
there is no `malloc` at all. That keeps newlib's allocator out of the image and makes the RAM
figure a link-time constant.

## 7. Size gate and CI

The gate built for M1 (`test/baremetal/size_gate`, `scripts/ci/knstax_size_gate.py`) becomes a
CI job with one reference device per profile:

| Reference | Profile | Core | Checks |
|---|---|---|---|
| `tp1-small` | `tp1-small` | Cortex-M3 (QEMU `mps2-an385`) | Flash and RAM budget, forbidden objects, heap high-water under QEMU |
| `tp1-standard` | `tp1-standard` + Data Secure | Cortex-M4 | Flash and RAM budget |
| `m0plus-small` | `tp1-small` | Cortex-M0+ | Builds and links (no hardware divide, no unaligned access) |

- **Size change per pull request.** Every change reports its flash and RAM delta per profile.
  A change that grows `tp1-small` explains why in its description.
- **Budgets ratchet.** The budget files hold the current figure plus a small margin, and the
  margin shrinks as the work lands, until the §2 target is the budget.
- **The heap figure includes traffic.** The emulator variant feeds recorded telegrams through a
  scripted backend, so the measured high-water mark covers receive and send, not only start-up.
- **The resolved `knx.config` is printed** with every result.

## 8. STM32 as a first-class target

- **Board interfaces are C and narrow:** the UART (K2), flash (K5) and a 1 ms tick. No vendor
  HAL is required or included.
- **Reference port.** KNstaX gets a small `ports/stm32f1/` example with startup, linker script,
  clock set-up and the three interfaces over CMSIS registers. It is tested on the H8i8o board
  (GD32F103, register-compatible for these peripherals). It is an example, not a BSP library;
  products copy it.
- **TP-UART2 and NCN5120** through the K3 backend. The chip simulator (K4) runs the same tests on
  the host.
- **CI builds** for Cortex-M0+, M3 and M4 so that architecture assumptions are caught early.
- **Documentation:** a "porting to a new MCU" section in the product authoring guide once the
  reference port runs on hardware.

## 9. Work plan

| Order | Item | Replaces | Gate after it |
|---|---|---|---|
| C1 | Kconfig for standalone builds: vendored Kconfiglib, `autoconf.h`, `knx_configure()`, profiles, `config.hpp` without defaults | — | Host suite unchanged; `full` profile builds identical to today |
| C2 | Capacities as options (§4.2), including `KNX_MAX_APDU_LENGTH`, with a full-capacity test per table | K7.5 | **Measure `tp1-small`** |
| C3 | Feature gates (§4.1): coupler, Security Interface Object, GO diagnostics, optional management services, DPT families | K7.1–K7.3 | **Measure** |
| C4 | Size gate in CI, per profile, with ratcheting budgets | K7.7 | — |
| S1 | Table-driven product runtime | — | **Measure: decision point** (§10) |
| S2 | Compact group objects | K7.4 | Measure |
| S3 | Interface object property storage | — | Measure |
| S4 | Allocation policy | K7.8 | Measure; `tp1-small` budget enforced |
| P1 | Reference STM32F1 port, run on the H8i8o board | — | First image on hardware |

The TP-UART backend and its simulator (K2–K4), flash persistence (K5) and the H8i8o's M2
bring-up do not depend on this work and proceed in parallel. They are needed for P1 in any case.

Each item is a separate change with its own measurement, so the effect of every step is on
record in this document's history and in CI.

### C1 status

C1 is in: behaviour-neutral infrastructure. The host build's compile definitions are
identical to those before it, and the size gate reports the same figures (flash 135585 B, RAM
static 1772 B, heap high-water 66700 B). What landed:

- Kconfiglib 14.1.0 (`kconfiglib.py` and `menuconfig.py`, ISC) vendored in `tools/kconfig/`,
  with `knx_genconfig.py`, which resolves a top Kconfig and an ordered list of defconfig
  fragments into `knx/autoconf.h`, `knx_config.cmake` and `knx.config`. It fails on an unknown
  symbol, a malformed line, or a value Kconfig does not honour (out of range, or hidden by an
  unmet dependency). Each fragment is held to its own assignments, so a later override that
  turns a dependency off (no KNXnet/IP) may take a symbol set earlier (tunnelling) with it.
- `Kconfig.standalone`, sourced only through `tools/kconfig/Kconfig.standalone.top`: the
  platform, `KNX_BUILD_TP1`, `KNX_BUILD_NETIP`, `KNX_BUILD_VIRTUAL_BUS`,
  `KNX_BUILD_ETS_TOOLING`, `KNX_BUILD_TEST_HOOKS` and `KNX_BUILD_ZLIB`. `Kconfig`, which
  ESP-IDF reads, is unchanged.
- `configs/profiles/`: `host-test` (what a host build was before), `full`, `tp1-small` and
  `tp1-standard`. The last two select what is built and the log level only; capacities
  arrive with C2.
- `cmake/knx_config.cmake`: `knx_configure(PROFILE <name> [OVERRIDES <files>...])`, called
  by a product before `add_subdirectory`, or by KNstaX itself with `tp1-small` (baremetal) or
  `host-test`. A `knx_menuconfig` target edits the resolved configuration.
- `config.hpp` and `log.hpp` include `knx/autoconf.h` when it exists, `sdkconfig.h`
  otherwise.
- The size gate is configured with `knx_configure(PROFILE tp1-small)`; its log level comes
  from the profile.
- The legacy options (`-DKNX_FEATURE_*`, `-DKNX_SECURE_ENABLED`, `-DKNX_ZLIB_ENABLED`,
  `-DKNX_ENABLE_TEST_HOOKS`, `-DKNX_PLATFORM`) still work, as a generated override fragment.

Deviations from §3:

- **The medium.** `KNX_MEDIUM` is a choice in `Kconfig` (an ESP-IDF image is one medium); a
  host build has TP1 and KNXnet/IP together. A standalone build leaves `KNX_MEDIUM` at TP1 and
  selects what is compiled with `KNX_BUILD_TP1` and `KNX_BUILD_NETIP`; the generator refuses
  `KNX_MEDIUM_IP` in that mode. `KNX_ENABLE_COUPLER` and `KNX_ENABLE_TUNNELING` are the ESP-IDF
  symbols themselves, given a second definition in `Kconfig.standalone` that is visible
  whenever the standalone switch for their medium is on.
- **`config.hpp` still has some fallbacks.** `#error` replaces the default only where the
  symbol has no dependency and is therefore defined by ESP-IDF as well: the RX task stack, the
  group object payload, the restart time (`config.hpp`) and the log level (`log.hpp`). The
  KNXnet/IP settings sit under the IP-medium menu, which an ESP-IDF TP1 image does not
  define and a standalone build leaves hidden, so they keep their defaults. Bools that are
  `n` are absent by design, so the feature macros keep "absent means off". The
  `KNX_FEATURE_*` macros are still derived as §3.1 says.
- **`full` is not `host-test`.** `host-test` is exactly the previous host default (the
  identity C1 is gated on); `full` is every device feature without the virtual bus and the
  test hooks, at log level 3.
- **Tunnelling follows KNXnet/IP.** With KNXnet/IP off the tunnelling switch is off too, so
  the tunnelling tests are no longer registered in a `-DKNX_FEATURE_NETIP=OFF` host build.
  Before, they were registered there although they cannot build.
- The legacy options are not announced with `message(DEPRECATION)`, which is silent by
  default; the build prints one status line naming the options it applied.

## 10. Projection and decision points

Estimated `tp1-small` figures for the 60-object reference device after each stage. Only the
first row is measured.

| After | Flash (KNstaX) | RAM (KNstaX) |
|---|---:|---:|
| Today (M1) | 132 KiB, measured | 67 KiB, measured |
| C2 + C3 | about 115 KiB | about 30 KiB |
| S1 | about 88 KiB | about 18 KiB |
| S2 + S3 + S4 | about 80–85 KiB | about 8–10 KiB |

**Decision point after S1.** If the measurement is more than about 10 % above the projection
on either axis, stop and review before S2–S4. The options then are deeper reductions in
`BusAccessUnit`, the application layer and the data link layer, or accepting `tp1-standard` as
the smallest supported tier. The H8i8o board would then need the GD32F303CC (h8i8o decision D2).

## 11. Risks

| Risk | Mitigation |
|---|---|
| Kconfig for standalone builds disturbs the host and ESP-IDF builds | C1 changes no behaviour: the `full` profile must produce the same host test results and an ESP-IDF image of the same size |
| A feature switched off in `tp1-small` turns out to be needed by ETS | Every `n` in §4.1 is confirmed with an ETS6 download on the H8i8o board before it becomes the default |
| A 15-byte APDU makes ETS downloads too slow | Measure the download time on hardware; 55 is the fall-back, at about +1 KiB of RAM |
| S1 changes the product authoring surface | The typed API stays; exporter and runtime read one descriptor table; the existing product tests are the acceptance test |
| Capacity limits cause silent misbehaviour | Rule 2 in §3.3: errors, negative responses and a test per capacity |
| The projection is wrong | The gate measures every step; the decision point in §10 |

## 12. Open questions

1. Should `tp1-small` drop the A_send pipeline's queueing entirely and send synchronously?
   That is simpler and smaller, but changes the timing contract for the product.
2. Is `KNX_HEAP_POLICY=arena` worth the extra mode, or is `init_only` enough?
3. Where should the reference STM32 port live: in KNstaX (`ports/`) or in a separate repository
   that depends on it?
