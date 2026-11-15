# Design: build-time configuration and the small-target profile

| | |
|---|---|
| Status | **Proposed**, 16 November 2026. Work items C1 and C2 (§9) are implemented, see "C1 status" and "C2 status"; nothing else here is. |
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
| `KNX_ENABLE_COUPLER` | The whole router role: Router object, filter and routing tables, coupler routing, two-port coupler, `CouplerOptions`, the layer-2 group acknowledge hook. Depends on function properties. | n | n | y |
| `KNX_ENABLE_SECURITY` | Data Secure and the Security Interface Object (object 17, absent when off). Selects `KNX_ENABLE_PROPERTY_EXT` and `KNX_ENABLE_FUNCTION_PROPERTIES`. | n | y once K6 provides AES on bare metal; n until then | y |
| `KNX_ENABLE_GO_DIAGNOSTICS` | Group object diagnostics function property (PID 66 of the group object table; not advertised when off). Depends on function properties. | n | y | y |
| `KNX_ENABLE_MEMORY_EXT` | `A_MemoryExtended_*` services | n | y | y |
| `KNX_ENABLE_PROPERTY_EXT` | `A_PropertyExt*` services (needed with Secure) | n | y | y |
| `KNX_ENABLE_NETWORK_PARAMETER` | `A_NetworkParameter_*` | n | y | y |
| `KNX_ENABLE_FUNCTION_PROPERTIES` | Function property services; GO diagnostics and the router need them | y | y | y |
| `KNX_DPT_FLOAT`, `KNX_DPT_TIME`, `KNX_DPT_STRING`, `KNX_DPT_COLOUR`, `KNX_DPT_ACCESS` | DPT codec families: float (9, 14), time and date (10, 11, 19), character and string (4, 16), colour (232, 242-244), access (15). Integer, boolean, scene, dimming and HVAC mode types are always in. A port that uses a family that is off fails to compile and names the symbol. | n (or from the product, §5) | y | y |
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
| `KNX_PENDING_ACTION_CAPACITY` | Product runtime queue of publishes made before start (2 × port count; 0 keeps that) | 8 | 16 | 0 |
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

**Before.** `EndpointRuntime` and the binding machinery were templates instantiated per port: a
write dispatcher (about 200 B) and a read preparer (about 58 B) per port, two 60-entry
member-function-pointer tables of them, three `std::function`s per port in a `std::tuple`, a
run-time linear search of the descriptors in every typed `publish`, and the port keys and display
names as rodata. The runtime object held a RAM copy of the runtime and export descriptors (4 KB) and
a pending-action queue of twice the port count (4.8 KB).

**Implemented** (S1 status, §9, has the measurements):

- **Port table.** The endpoint definition compiles to one `static constexpr` table,
  `kPortTable<Definition>`, of 14-byte `PortDescriptor`s: logical id, DPT, value type, flags
  (readable, writable, transmit, receivable, persisted, read-on-init), priority, direction, and two
  ranks. The slot of a port is its index and its group-object index. The table is rodata and has no
  strings and no `GroupObject` types. Keys and display names are a separate `kPortNames` table that
  only the exporter and the tests read. Encoding and decoding stay the existing dynamic
  `encodeDptValue` / `decodeDptValue` keyed by value type; there is no codec table.
- **Descriptors on demand.** `compiledDefinition()` (and `compiledEndpoint()`) build the runtime and
  export descriptors from the two tables and return them by value; the runtime keeps a copy of the
  small definition (identity, persistence and security policy), not the descriptors. The exporter's
  output is byte-identical.
- **Type-erased bindings.** A binding is `{ thunk, manage, ctx }`. A trivially copyable callable of at
  most a pointer's size is stored in `ctx` with one thunk per callable type; anything else is
  heap-allocated and freed by `manage`, which keeps the default `BindingCapacity` of 0 working. With
  `BindingCapacity > 0` the heap is forbidden and a callable that does not fit is a `static_assert`.
  Entries exist only for the ports that can use them, indexed by the table's ranks: command and
  state-write entries for ports that accept bus writes, state providers for readable ports.
  A binding a port cannot use is a compile-time error.
- **`ProductPortCore`.** One non-template class in `src/product/` (in the core source list) does what
  was per-port code: group-object registration (one loop), address binding, transmit policy,
  `isPortLinked`, seeding of readable state, the write and read callbacks, `dispatchWrite(slot)`,
  publish and read-response start, and the pending-action queue. `EndpointRuntime<Def, Cap>` is a shell:
  the core plus the storage sized from the type, and the typed API as inline wrappers that pass the
  slot as a compile-time constant (`publish<Id>(v)` is one `DptValue` construction and one call). The
  core reaches group objects only through `setGroupObjectValue`, `sendGroupValueForObject` /
  `respondGroupValueForObject`, `groupObject()` and `isGroupObjectLinked`, so S2 replaces the body of
  its registration loop and those calls.
- **Pending actions** are a Kconfig capacity, `KNX_PENDING_ACTION_CAPACITY` (§4.2): 0 is twice the port
  count (host-test, full, ESP-IDF), 8 on `tp1-small`, `host-small` and `host-minimal`, 16 on
  `tp1-standard`. A full queue answers `QueueFull`.

*Compatibility.* The typed API keeps its signatures and error results. Changes: `compiledDefinition()`
and `compiledEndpoint()` return by value (a `const auto&` binding still works); `EndpointBindings` is
move-only and `PortBindingSlot` is gone; `BindingCapacity > 0` means "maximum inline capture bytes, no
heap"; `PendingBusAction<Id>` is one record whose `logicalId` is its 16-bit value.

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
| `m0plus-small` | `tp1-small` | Cortex-M0+ | Builds and links (no hardware divide, no unaligned access); flash and static RAM budget |

The gate takes the profile and the core as parameters (`run_gate.sh <build-dir> [profile [cpu]]`,
or `KNX_SIZE_GATE_PROFILE` and the toolchain's `KNX_ARM_CPU`) and reads the budget file
`test/baremetal/size_gate/budgets/<profile>-<core>.json` (`tp1-small-m3`, `tp1-standard-m4`,
`tp1-small-m0plus`). Only the Cortex-M3 device is run under QEMU; the others report "heap not
measured" and their RAM figure is static data only. The CI job is `size-gate` in
`.github/workflows/ci.yml`, one matrix entry per reference device.

- **Size change per pull request.** Every change reports its flash and RAM delta per profile.
  A change that grows `tp1-small` explains why in its description. In CI the `size-gate` job
  measures the base of the pull request first, with the same reference device (the head's gate
  against the base's KNstaX tree), and prints the change of each figure; the job summary and the
  `gate_report.json` artefact carry it.
- **Budgets ratchet.** Each budget file holds, per figure, the enforced `budget` (the figure
  when the file was last tightened, plus about 2 %, rounded up to 256 B) and the design `target`
  (§2). The gate fails only on the budget and prints the distance to the target. A figure more
  than the margin below its budget produces a note to lower the budget; it does not fail. The
  budget moves towards the target as the work lands, until the two are equal.
- **The heap figure includes traffic.** The emulator variant feeds recorded telegrams through a
  scripted backend, so the measured high-water mark covers receive and send, not only start-up.
  (Not done yet: today's run is start-up plus 200 idle loop iterations; see the C4 status.)
- **The resolved `knx.config` is printed** with every result (the `knx-config/knx.config` of the
  gate's build tree).

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
| C3 | Feature gates (§4.1): coupler, Security Interface Object, GO diagnostics, optional management services, DPT families | K7.1–K7.3 | **Done: tp1-small flash 134995 to 99907 B, see C3 status** |
| C4 | Size gate in CI, per profile, with ratcheting budgets | K7.7 | **Done: see C4 status** |
| S1 | Table-driven product runtime | — | **Done: tp1-small flash 108275 to 79925 B, heap 40980 to 31108 B; decision point reached, see S1 status and §10** |
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

### C2 status

C2 is in: the capacities of §4.2 are Kconfig symbols (menu "Capacities", defined in every build, so
`config.hpp` has no fallback for them) with the per-profile values of §4.2. The ESP-IDF defaults and
the `host-test` and `full` profiles carry the values the stack had before: 254, 32, 8, 256, 512, 16,
8 and, for the group object payload, 16. Every new symbol is in `Kconfig` with that default, so an
ESP-IDF build is unchanged (it could not be built here).

How each value is used:

- `KNX_MAX_APDU_LENGTH` replaces `config::MAX_APDU_LENGTH`. Every buffer derived from it follows:
  the TPDU in `LDataFrame` and `TDataFrame`, transport and application buffers, the management
  response, the memory service limit (`min(63, APDU - 3)`), and the frame size. A TP1 frame is the APDU
  length plus the fixed octets of 03/02/02 §2.2: a standard frame (CTRL, source, destination, length
  octet, TPDU of length + 1, FCS) is length + 8 octets, 23 for 15; an extended frame adds CTRLE and is
  length + 9, 263 for 254. `config::MAX_TP1_FRAME_LENGTH` is that, and the TX buffer of the data
  link layer and the receive ring slot of `Tp1MacPhysical` use it.
- The queue and table symbols size `Tp1MacPhysical`, `TransportLayer`, `AddressTableDomain`,
  `AssociationTableDomain`, `Tp1DataLinkLayer` (group list, additional addresses) and the A_send
  outcome queue. The memory-mapped table segments (`table_segments.hpp`) are sized from the table
  capacities, so a write past the end is refused by the memory service with the negative
  A_Memory_Response. `FixedVector` accepts a capacity of 0 (additional addresses on an end device).
- The busy reserve of the receive ring is an eighth of the queue, at least one frame (4 of 32, as before).

What a full capacity does (rule 2). Already errors and left as they were: address table add
(`OutOfRange`), association add (`ResourceUnavailable`), data link group list and additional
addresses (`BufferTooSmall`), transport queue (drop and count), send outcome queue (drops the oldest and
counts, a diagnostic feed by design). Fixed: a loaded table segment whose count exceeds the capacity
was truncated with a warning; it is now refused and the table's load state becomes Error (03/05/01
§4.23), which ETS reads as a failed download. The frame decoder ignored the result of copying the TPDU;
a frame longer than the configured maximum is now `InvalidFrameSize` (counted in `decodeFailed`), and
`Tp1MacPhysical` drops and counts it (`rxOversizedDrops()`), and counts queue overflow
(`rxQueueOverflowDrops()`). A property read whose value does not fit the APDU is answered with the
negative response instead of being built and dropped. Each capacity has a host test that fills it to the
configured value and checks the next one is refused (`test_address_association_tables`,
`test_datalink_edge_cases`, `test_tp1_mac_physical`, `test_transport_connection`,
`test_application_layer`, `test_ets_connected_session_capacity`).

Values from the product (§5). `ProductIdentity::maxApduLength` defaults to the configured maximum and
must not exceed it. A `static_assert` cannot see it, because the identity is a value member of the
endpoint definition and not part of its type; `makeEndpointDefinition()` is `constexpr` and calls a
non-`constexpr` function when the value is too large, so a `constexpr` product fails to compile (the
function's name says why), and `EndpointRuntime::start` returns `InvalidParameter` for a definition built
at run time. The device reports the product's value in `PID_MAX_APDU_LENGTH`; `DeviceObject` clamps to the
configured maximum. The exporter's table ceilings were already `kMaxEntries`, hence the configured
values; it now also reports `maxApduLength` (JSON `capacities`, XML `<MaxApduLength>`).

Deviations and notes:

- **`Tp1MacPhysical::MAX_RX_FRAME_SIZE` was 256 and is now 263 at the full APDU length.** An extended frame
  of 257 to 263 octets was dropped as oversized: a latent bug, fixed by deriving the size.
- The software bit-bang drivers stay sized for 263 octets, 256 group addresses and 16 additional
  addresses; they run on ESP32 and in host simulation, not on the small parts. `static_assert`s check that
  they hold the configured sizes, and the MAC drops longer frames before the upper layers.
- The oversized responses of the extended services (`A_PropertyExt*`, function property) that do not fit
  the APDU were still logged and dropped in `management_server.cpp`; C3 fixed that (see C3 status) and gates those
  services out of `tp1-small`.
- Every `host-small` failure was a test that assumed the full capacity; none was a defect in the stack.
  Three expectations (device object default, address table size, memory read limit) are now written
  against the configuration. The stack fixes above were found by reading the full-capacity paths.

Host suite: `host-test` 153 of 153 (the 152 of before plus the `test_ets_connected_session_capacity`
group). `host-small` (`-DKNX_HOST_PROFILE=host-small`, `host-small.defconfig`: the host features with
the `tp1-small` capacities) excludes tests labelled `needs-full-capacity` and passes 140 of 140; 13
entries carry the label, each failing for a size reason, not a defect:

| Test | Why it needs the full capacity |
|---|---|
| `test_product_commissioned_surface` | Tool Key (16 octets) writes and answers exceed a 15-octet APDU |
| `test_bau_preinit_configuration` | the same Tool Key |
| `test_bau_onwire` | Data Secure sync frames and Tool Key operations exceed the APDU |
| `test_data_secure_virtual_tp1` | secured management frames (counter, MAC) exceed the APDU |
| `test_ets_connected_session` (group `session`) | master-reset tests write a 16-octet Tool Key |
| `test_ets_connected_session_routing` | the routing group written by serial number does not fit a standard frame |
| `test_knx_stack_e2e` | the 15-octet extended property description answer does not fit |
| `test_interface_object_manager`, `test_interface_object_manager_persistence` | the 16-octet KNXnet/IP backbone key does not fit the property buffer |
| `test_memory_service` | A_MemoryExtended writes and reads of 76 octets |
| `test_filter_table_type3_download` | router frames longer than 23 octets |
| `test_tp1_ip_interface`, `test_tp1_ip_interface_bridge` | a bridge needs additional individual addresses (tunnel addresses); `KNX_ADDITIONAL_INDIVIDUAL_ADDRESSES` is 0 |

Size gate, `tp1-small`, same reference device (60 objects, `maxApduLength = 15`), before C2 and after:

| | C1 | C2 |
|---|---:|---:|
| KNstaX flash | 135 585 B | **134 995 B** |
| KNstaX RAM, static | 1 772 B | **1 772 B** |
| Heap high-water after `startCommissionedProduct()` | 66 700 B | **42 500 B** |
| Static + heap | 68 472 B | **44 272 B** |

Allocation profile after C2 (`alloc <count> x <bytes>`): 61 x 248, 12 184, 3 056, 2 944, 1 808, 1 224,
1 040, 528, 488, 416, 384, 344 (the 8 488 B receive ring of `Tp1MacPhysical` is no longer among them; the
largest remaining items are the group objects at 15 128 B and the product runtime at 12 184 B, which are
S1 and S2). The saving is 24.2 KB of RAM and 0.6 KB of flash. The flash figure barely moves because the
capacities size storage, not code.

### C3 status

Done. Every feature in §4.1 except the TP-UART choice is a Kconfig symbol, and a feature that is off is
absent: its sources are not compiled (`cmake/knx_sources.cmake`, `knx_optional_sources()`, used by the
ESP-IDF and the standalone branch alike), its declarations are removed, and the size gate's forbid list
checks the linked image. `include/knx/config.hpp` derives `KNX_FEATURE_*` from the `CONFIG_KNX_*` symbols
(`KNX_FEATURE_COUPLER` and `KNX_SECURE_ENABLED` as before). ESP-IDF defaults are the old behaviour
(every new symbol `y`; coupler and Secure stay `n`); the ESP-IDF branch could not be built here, only
reasoned about.

| Symbol | Gates |
|---|---|
| `KNX_ENABLE_COUPLER` | `filter_table*`, `routing_table_control`, `coupler_routing`, `two_port_coupler` (group `KNX_SOURCES_COUPLER`); `configureRouterRole` and the router PIDs in `DeviceManagement`, `LinkAccess::coupler()`, the coupler stack-port path, `CommissionedProductRuntime::start(CouplerOptions)`, `createTp1CouplerStackPort`, `CouplerOptions`, `CommissionedTransportPath::Tp1Coupler`, the Router Object entry of the reference registry (12 reference types instead of 13), the filter-table banked record |
| `KNX_ENABLE_SECURITY` | `security_interface_object.cpp`; the object as a member of `BusAccessUnit`, `InterfaceObjectManager` (five core objects instead of six) and `DeviceModel`; secure persistence, factory security defaults, `configureDataSecure`, the product runtime's secure wrappers and `SecureCommissioningOptions` |
| `KNX_ENABLE_GO_DIAGNOSTICS` | `go_diagnostics.cpp`, `DeviceManagement::handleGroupObjectDiagnostics`, the PID 66 `FunctionProperty` of the group object table |
| `KNX_ENABLE_FUNCTION_PROPERTIES`, `KNX_ENABLE_PROPERTY_EXT` | `function_property_services.cpp`, `property_ext_services.cpp` and their members, handlers, callbacks and dispatch cases in `ManagementServer`; the stack port setters |
| `KNX_ENABLE_MEMORY_EXT`, `KNX_ENABLE_NETWORK_PARAMETER` | The `A_MemoryExtended_*` and `A_NetworkParameter_*` handlers, response callbacks and dispatch cases. Their classes stay compiled: `memory_service.cpp` also holds `A_Memory_*`, and `network_parameter_service.cpp` also holds the serial number and domain address services that ETS needs. |
| `KNX_DPT_*`, `KNX_ENABLE_DPT_CATALOGUE` | The codecs of each family, their `DptTraits`, the dispatch cases, the catalogue table and its strings. Character DPT 4 is in `KNX_DPT_STRING`: nothing but its own catalogue entry uses it. |

Dependencies: Secure selects property-ext and function properties; the coupler and GO diagnostics depend
on function properties. A profile that asks for the impossible fails in `knx_genconfig.py`.

**What ETS sees when a feature is off.** No Router Object (type 6) without the coupler; no Security
Interface Object (type 17) without Secure. The old non-secure build did expose object 17: it was a core
object whatever `KNX_SECURE_ENABLED` said, so a non-secure device advertised an object nothing used. That
is gone. PID 66 of the group object table is not described without GO diagnostics. A request for a
service that is off gets no answer, as an unknown APCI does: no negative response is defined for an
unsupported extended or network parameter service, so silence is the answer. A service that is present
but whose result does not fit the APDU now answers with the error KNX defines instead of dropping the
request: `A_MemoryExtended_*_Response` with E_EXCEED_MAX_APDU_LENGTH and no data;
`A_PropertyExtValue_Response` with element count 0 and that code as the one data octet (03/03/07
§3.4.5.1); `A_FunctionPropertyExtState_Response` with that code and no data; the classic
`A_FunctionPropertyState_Response` with E_ERROR.

**Profiles.** `tp1-small`: function properties only. `tp1-standard`: everything except the coupler and
the catalogue (its Secure changes from `n` to `y`, as §4.1 says; the bare-metal build then needs mbedTLS,
which the size gate does not link). `full`, `host-test`, `host-small`: everything. New `host-minimal`:
the host platform with the virtual bus and test hooks, with the `tp1-small` features and capacities;
KNXnet/IP, the ETS tooling and zlib stay on as in `host-small` (they are media and host tools, and the
suite's IP and ETS tests need them). A test that needs an absent feature is not registered
(`knx_add_test(... NEEDS <feature>)`); `knx_tests_not_registered.txt` in the build tree lists them.

**Measured (size gate, `tp1-small`, reference device, LTO).**

| | Flash | Static RAM | Heap | Runtime flash |
|---|---:|---:|---:|---:|
| After C2 | 134995 B | 1772 B | 42500 B | 11831 B |
| After coupler, Secure object, GO diagnostics | 117210 B | 1379 B | 41892 B | 11831 B |
| After C3 | 99907 B | 1379 B | 40980 B | 7551 B |

C3 takes 35088 B out of the KNstaX flash (the §9.1 upper bounds of 5.5, 3.2 and 8.5 KiB add up to 17 KiB,
so the gated code carried more with it than those bounds said) and 4280 B out of the C and C++ runtime,
which is now within its 8192 B budget. Flash is still 9795 B over the 90112 B budget. The heap and static
RAM moved only by the objects that went; S2 and S3 own the rest. The forbid list (`budgets/tp1-small-m3.json`,
checked against the ELF's symbols with `nm -C`) passes: no `FilterTable`, `TwoPortCoupler`,
`RoutingTableControl`, `CouplerRouting`, `SecurityInterfaceObject`, group object diagnostics, DPT 9, 14,
10, 16 or 232 codec, or `PropertyExtServices` in the image.

**Deviations and limits.**

- The change threshold is a `double` in `GroupObjectTransmitPolicy`. Without `KNX_DPT_FLOAT` it is a
  `uint32_t` in units of the integer value and compares integers (`dptValueAsInteger`); with it,
  behaviour is as before. `DptValue` still holds a `float` and a `std::string` in its variant; removing
  them belongs to the group object redesign (S2).
- Without the catalogue the dynamic encode and decode take the value type from `dptValueTypeFor()` (one
  definition, shared with the product compiler) instead of from the table. The two agree for every
  catalogue entry and differ only for an id the catalogue does not list (an unknown DPT 21 or 22 sub-type).
- `memory_service.cpp` and `network_parameter_service.cpp` are not removed (see the table).
- `host-minimal` runs 101 tests (the 13 that carry `needs-full-capacity` are left out, as for `host-small`)
  and does not register 36 that need an absent feature (by the first one missing): 9 coupler, 9 Secure, 1 property-ext, 12 float, 5 other DPT
  families or the catalogue. Tests that need only part of a feature (a temperature port in
  a product test) are excluded whole; a port-level split would let them run.

### C4 status

Done, with the limits below. The CI workflow could not be run here (no GitHub Actions); every command
in it was run locally in the `knstax-qemu` and `knstax-host` images, and the YAML was parsed.

- **Gate structure.** `run_gate.sh <build-dir> [profile [cpu]]` builds the reference device for the
  profile and core, runs it under QEMU only for Cortex-M3, prints the resolved `knx.config`, and runs
  `knstax_size_gate.py` with the budget file `budgets/<profile>-<core>.json`. It writes
  `gate_report.txt` and `gate_report.json` and ends with one summary line carrying profile and core.
  Exit status: 0 within budget, 1 over, 2 the gate could not run. `size_budget.json` is gone.
- **Budget format.** Per figure (`flash`, `ram`, `runtime_flash`): `budget` (enforced) and `target`
  (design, never enforced). Budgets are the initial figure plus 2 %, rounded up to 256 B. A figure
  more than the margin below its budget prints a note to lower it. A deliberately lowered budget
  exits 1; the baseline option (`--baseline`, `KNX_SIZE_GATE_BASELINE`) prints the change against an
  earlier `gate_report.json`.

| Reference | Flash (budget; target) | Static RAM | Heap | Static + heap (budget; target) | Runtime flash (budget; target) |
|---|---|---:|---:|---|---|
| `tp1-small`, Cortex-M3 | 99907 B (102144; 90112) | 1379 B | 40980 B | 42359 B (43264; 10240) | 7551 B (7936; 8192) |
| `tp1-standard`, Cortex-M4 | 118646 B (121088; 143360) | 1532 B | not measured | 1532 B static only (1792; 32768) | 16919 B (17408; none) |
| `tp1-small`, Cortex-M0+ | 105522 B (107776; 90112) | 1379 B | not measured | 1379 B static only (1536; 10240) | 8394 B (8704; 8192) |

- **`tp1-standard` has no Data Secure yet.** Its defconfig leaves `KNX_ENABLE_SECURITY` off until the
  portable AES backend (K6) lands, so this reference device is the standard feature set without
  Secure, and its 140 KiB flash target is not yet a statement about the final profile. Its RAM
  budget covers static data only, so it says little until a heap run exists for it. The forbid list
  of this device is the coupler's symbols only.
- **Cortex-M0+ builds and links.** KNstaX needed no change. The gate's own start-up and board code
  needed two things: `std::atomic` read-modify-write on ARMv6-M (no `LDREX`) becomes libatomic calls
  (`__atomic_fetch_add_4`, `__atomic_compare_exchange_1`) that newlib does not provide, so
  `syscalls.c` supplies interrupt-masking versions for `__ARM_ARCH_6M__`; a product for this core
  must do the same, or KNstaX should stop using `std::atomic` read-modify-write on the data-link path
  (`Tp1DataLinkLayer` counters and flags, reached from `Tp1MacPhysical::onTp1Event`). The system-call stubs
  are now `used`, because under LTO the `tp1-standard` link otherwise lost `_isatty` and `_fstat`.
  There is no Cortex-M0+ execution test: it is linked, not run.
- **Pull-request deltas.** Implemented in the job (base measured first, `continue-on-error`). If the
  base does not build with the head's reference device, the step reports no baseline and the job
  carries on; to compare by hand, download the `gate_report.json` artefacts of two runs and pass
  one to the gate with `--baseline`.
- **Host profiles in CI.** `host-small` and `host-minimal` run in a `host-profiles` job
  (`-DKNX_HOST_PROFILE=...`, `ctest -LE needs-full-capacity`), both configured with Debug and
  `BUILD_TESTS=ON` as the main host job is. Both were run here with exactly those commands and pass (exit 0, about 41 s each).
- **Not done.** The heap figure does not yet include traffic (§7): the emulator run is start-up plus
  200 idle loops. No heap run for `tp1-standard` or the Cortex-M0+ (QEMU has no `mps2-an385`
  equivalent in the set used here; `mps2-an386` is Cortex-M4 and could serve `tp1-standard`).

### S1 status

Done (§6.1 describes what was built). Seven changes, each leaving the host suites green and the gate
passing, with the figure of every step measured on the `tp1-small` Cortex-M3 reference device.

**The reference device changed first (step 0).** The gate device started the product with no
bindings and no publishes, so the binding storage and the dispatch code a real product pays for were
only partly measured. `test/baremetal/size_gate/main.cpp` now binds a handler on every port (each
captures a small index and has its own callable type, as per-channel handlers do; command, state write
and state provider as each port allows) and calls `publish` once per datapoint type. That raised the
baseline by 8368 B of flash, and the three budget files with it (`tp1-standard` too, because the three
configurations share the device). The "Before" row below is that baseline; the 99907 B of the C3 status
is the old, less representative device.

| Step | Flash | Static RAM | Heap | Static + heap |
|---|---:|---:|---:|---:|
| C3 device (no bindings), for reference | 99907 B | 1379 B | 40980 B | 42359 B |
| 0. Bindings and publishes in the device (baseline) | 108275 B | 1443 B | 40980 B | 42423 B |
| 1. Port table and names table; descriptors built from them | 108561 B | 1443 B | 40620 B | 42063 B |
| 2. No stored `_compiled`; `compiledDefinition()` by value | 100051 B | 1443 B | 37092 B | 38535 B |
| 3. Type-erased bindings | 95991 B | 1443 B | 35556 B | 36999 B |
| 4. `ProductPortCore`; per-slot templates and tables removed | 80031 B | 1443 B | 35588 B | 37031 B |
| 5. Pending-action capacity (8 on `tp1-small`) | 79925 B | 1443 B | 31108 B | 32551 B |
| 6. Names proven absent (gate check; no code change) | 79925 B | 1443 B | 31108 B | 32551 B |

S1 takes 28350 B (26 %) out of the flash and 9872 B out of the heap, a flash saving at the top of the
25–30 KB the design expected. The runtime flash (C and C++ runtime) stays 7551 B. The other
configurations: Cortex-M0+ flash 114058 to 82683 B; `tp1-standard`, Cortex-M4 flash 127059 to 98911 B
(static RAM 1596 B; its heap is not run). The budgets were lowered to the new figures plus the margin:
`tp1-small` M3 81664 B flash and 33280 B RAM, M0+ 84480 B flash, `tp1-standard` 101120 B flash.

- **What the steps did.** Step 1 adds the 840 B port table (60 × 14 B) and changes nothing else. Step
  2 is the biggest RAM item on its own: the compiled definition (3.9 KB of descriptors copied into the
  runtime object) and, in flash, the export descriptors and the port names it pulled into the image.
  Step 3 replaces the 2960 B of `std::function` slots with 1344 B of entries (44 receive ports × 2 + 24
  readable ports, 12 B each) and the `std::function` managers. Step 4 removes the per-port templates
  (the 12 KB of dispatchers, the read preparers, their two tables) and the run-time descriptor search;
  the core is compiled once (after LTO its largest out-of-line functions are `publishStarted`, 608 B, and
  `drainPendingActions`, 448 B). Step 5 is the queue: 60 × 2 × 40 B = 4800 B becomes 8 × 40 B.
- **Runtime object.** The heap allocation of `CommissionedProductRuntime` went from 12184 B to 2272 B (the
  `EndpointRuntime` inside it is 2088 B, the core 36 B). What is
  left is the 1344 B of binding entries, the 320 B queue, the configured group addresses, the copy of
  the definition (about 120 B) and the parameter state.
- **Call-site cost.** `publish<Id>(v)` builds a `DptValue` and calls the core with the slot. Six extra
  `publish<…>(bool)` call sites in the gate device cost 196 B (33 B each), so a scalar fast path would
  save a few hundred bytes for a product with dozens of sites. Not worth a second code path; not done.
- **Ports that cannot use a binding.** `onCommand` and `onStateWrite` on a port that accepts no bus
  writes, and `provideState` on a port that is not readable, were accepted before and never called.
  They are `static_assert`s now (the conformance evidence for compile-time port direction already
  claimed it). The Habinari firmware's bindings (`knx_group_reads.cpp`, `knx_group_writes.cpp`) and
  every KNstaX test and host example compile with this.
- **Names.** Keys and display names are only in `kPortNames`. The gate's new `forbid_strings` (budget
  file) and `--forbid-string` look for text in the bytes of the ELF; the three budgets forbid one key of
  each port family. At the commit before the port table those strings are in the ELF, so the check does
  fail when they come back. The product's own identity strings (`productKey`, display name, order
  number) are still reachable through the definition copy the runtime keeps, a few tens of bytes.
- **`DptValue` for state providers and writes.** The decode on a bus write and the encode of a provider
  still build a `DptValue`, which holds a `float` and a `std::string` in its variant (32 B on a
  32-bit target). Removing that is S2.
- **Out of scope, noted.** `ParameterState::applyFromBytesAt<I>` is a per-parameter template, so it
  still costs code per parameter (possible S1b). Runtime persistence semantics were not touched
  (`persisted` is still export-only).
- **Behaviour changes to know.** A bus write whose value does not match the port type and for which no
  handler is bound now yields `OperationNotReady`, not `DecodeFailed` (the callback discards the
  result; with a handler bound it is still `DecodeFailed`). A full pending-action queue is `QueueFull`
  as before; only the size differs.
- **Heap profile after S1** (`alloc <count> x <bytes>`): 60 x 240, 2796, 2720, 2272, 1764, 1224, 1032,
  528, 488, 376, 344, 320. The group objects are 14400 B of the 31108 B (46 %); the runtime object is the
  2272. S2 owns the first, S3 and S4 the rest. The 2796, 2720 and 1764 B items are not attributed here.

## 10. Projection and decision points

Estimated `tp1-small` figures for the 60-object reference device after each stage. Only the
first row is measured.

| After | Flash (KNstaX) | RAM (KNstaX) | Measured |
|---|---:|---:|---|
| Today (M1) | 132 KiB, measured | 67 KiB, measured | |
| C2 | | | 131.8 KiB flash, 43.2 KiB RAM (1.7 static + 41.5 heap) |
| C2 + C3 | about 115 KiB | about 30 KiB | 97.6 KiB flash, 41.4 KiB RAM (1.3 static + 40.0 heap) |
| S1 | about 88 KiB | about 18 KiB | **78.1 KiB flash, 31.8 KiB RAM (1.4 static + 30.4 heap)**; on the representative device with bindings, S1 took 108275 B / 42423 B down to 79925 B / 32551 B |
| S2 + S3 + S4 | about 80–85 KiB | about 8–10 KiB | |

**Decision point after S1.** If the measurement is more than about 10 % above the projection
on either axis, stop and review before S2–S4. The options then are deeper reductions in
`BusAccessUnit`, the application layer and the data link layer, or accepting `tp1-standard` as
the smallest supported tier. The H8i8o board would then need the GD32F303CC (h8i8o decision D2).

**Evaluation (after S1).**

- **Flash: inside the projection.** 78.1 KiB against about 88 KiB, 11 % below, and under the 90112 B
  target (88 KiB) by 10 KB. The Cortex-M0+ build is 80.7 KiB and `tp1-standard` 96.6 KiB. The
  projection of 88 KiB assumed S1 would save about 27 KiB; it saved 27.7 KiB on the device that carries
  bindings.
- **RAM: more than 10 % above the projection, by the rule a stop-and-review.** 31.8 KiB against about
  18 KiB is 77 % above. The projection assumed S1 would take about 12 KiB of RAM off; it took 9.6 KiB
  (the study had expected about 10 KB from the runtime object, which the measurement confirms: 12184 B
  to 2272 B, and 4.5 KB of that is the queue). Most of the gap is older than S1: the projection for
  C2 + C3 was about 30 KiB and the measurement was 41.4 KiB, because C2 and C3 move flash and tables,
  not the group objects and the interface object storage that hold the RAM (S2, S3). The expected next
  step is S2: 14400 B of the 31108 B of heap are the 60 group objects (240 B each), and §6.2 expects
  about 13 KiB back, leaving about 19 KiB. S3 and S4 then have to remove roughly another 9 KiB to
  reach the 8–10 KiB projection, from the 16.7 KB of other heap items (2796, 2720, 2272, 1764, ...) that are
  not yet attributed. That is a larger step than the §10 projection implied, and this document does not
  decide whether it is reachable.
- **What this means for the options.** The flash axis no longer needs `BusAccessUnit`, application
  layer or data link layer reductions: the 90112 B flash target is met with 10 KB of margin.
  The RAM axis is what S2–S4 have to carry; the `tp1-small` RAM target of 10240 B is reachable only if
  S2 delivers its 13 KiB and S3 and S4 together remove about 9 KiB more. The review the rule asks for
  is therefore an S2 estimate check (a measured group object size, and an attribution of the 2796, 2720
  and 1764 B allocations) before S3 and S4 are committed to, and, failing that, the GD32F303CC
  fall-back (h8i8o decision D2). The decision is the owner's.

**Review (16 November 2026).** The heap left after S1 is now fully attributed (`sizeof` on the
`tp1-small` configuration, Cortex-M3):

| Heap item | Bytes | Addressed by |
|---|---:|---|
| 60 group objects, 240 B each | 14 400 | S2 |
| `InterfaceObjectManager` | 2 796 | S3 |
| `BusAccessUnit` | 2 720 | S3, callback capture size |
| Product runtime (`CommissionedProductRuntime`) | 2 272 | done in S1 |
| `DeviceModel` | 1 764 | S3 |
| `Tp1DataLinkLayer` | 1 224 | callback capture size |
| Group object table object | 1 040 | S2, S3 |
| `ApplicationLayer` | 1 032 | callback capture size |
| Association table (160 entries) | 980 | capacity (done in C2) |
| `ManagementServer` | 768 | callback capture size |
| `TransportLayer`, `GroupRuntime`, address table, device object, others | about 2 100 | |

The stack's internal callbacks are 65 `InplaceFunction` types with 32 or 64 bytes of inline storage,
almost all of which capture only `this`. Making that storage a build option (§4.3,
`KNX_CALLBACK_CAPTURE_BYTES`) is expected to remove about 2 KiB. With S2 (about 13.6 KiB) the device
is projected at about 16 KiB, which leaves S3 and S4 to find about 4–6 KiB in the interface object
manager, `BusAccessUnit` and `DeviceModel` (8.5 KB between them).

**Which RAM target.** The gate counts the group objects in the KNstaX share. The H8i8o budget
(h8i8o `01-system-design.md` §4.2) lists them separately: KNstaX static and heap at most 10 KiB, and
group objects at most 2 KiB. Measured the way the gate measures, the board's budget is therefore
12 KiB, not 10. The owner should confirm this reading before the gate target is changed.

**Outcome.** The work continues with S2, with the callback capture size added to it. The GD32F303CC
fall-back is not needed on present evidence. The next review is after S2.

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
