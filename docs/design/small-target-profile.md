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
| `KNX_HEAP_POLICY` | `free` (today), `init_only` (allocation after `heap::lock()` traps in debug builds and fails in release; built in S4), `arena` (one static arena of `KNX_HEAP_ARENA_BYTES`, no `malloc`; not built) | `init_only` |
| `KNX_PERSISTENCE_BACKEND` | `nvs`, `file`, `flash_log` (K5), `product` (factory only) | `flash_log` |
| `KNX_CALLBACK_CAPTURE_BYTES` | Cap on the inline capture of the stack's `InplaceFunction` callbacks (32 or 64 today; 0 keeps them; never raises a size). Built in S2 as a Capacities symbol, not a policy; the product's own callbacks are not affected | 8 (`tp1-standard` 8; the host profiles 0) |

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

**Was.** A `GroupObject` was a 240 B heap object (248 B with the allocator's header): configuration
with a `double` threshold, two value buffers, two `InplaceFunction`s of 64 bytes, and the transmit
policy state. The group object table object held them through 256 `unique_ptr` slots (1040 B).

**Built.** The group objects of a device are one table, a `GroupObjectStore`
(`include/knx/application/group_object.hpp`), owned by the `GroupObjectTableObject`. It holds:

- **Entries**, one array allocated once. A `GroupObject` *is* an entry: 16 B on a 32-bit target
  (`static_assert`ed): the store pointer, the datapoint type, the address, 11 bits of flags and
  state (C/R/W/T/U/I, priority, valid, pending send), the offset and length of the value, and the
  index of its policy state. The member functions are the old ones, so callers and tests compile
  unchanged. `flags()` and `transmitPolicy()` return by value (they are packed), and the virtual
  destructor is gone.
- **One value pool** (`std::vector<uint8_t>`, reserved once). An object takes
  `groupObjectValueBytes(dpt)` bytes: the datapoint's width in whole octets, or
  `KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES` for a datapoint the stack does not know. `ProductPortCore`
  reserves the entries and the pool from its port table (`registerGroupObjects`); objects added one
  at a time (`BusAccessUnit::addGroupObject`, an ETS-driven table) grow both, with exact-size
  reallocations.
- **A policy side table.** The transmit policy (`GroupObjectTransmitPolicy`, with its threshold
  `double`, or `uint32_t` without `KNX_DPT_FLOAT`), the last-sent time and the last-sent copy of the
  value, only for objects that were given a policy other than the inert default. An object without
  one keeps no transmit history and publishes every value, as the inert policy always did. At most 255
  objects can have a policy (an 8-bit index); the 256th gets `ResourceUnavailable`.
- **One value callback and one read-request sender** for the whole table, instead of two
  `InplaceFunction<…, 64>` per object. The value callback is called with the object's index.
  `GroupRuntime` installs the sender, so product objects (which were registered without one) can
  also issue reads.

An object built on its own (`GroupObject(config)`, used by tests and tools) owns a private one-object
store; `addGroupObject(std::unique_ptr<GroupObject>)` copies its state into the table. New in the
table API: `addGroupObject(const GroupObjectConfig&)`, `reserve(objects, valueBytes)` and
`store()`. `GroupObject*` and the span from `getRawValue()` stay valid until the table grows past
what was reserved; a product that registers everything at start never sees that.

**Changed behaviour.**

- A payload longer than its datapoint's width is refused with `BufferTooSmall`; before, it was
  stored up to `KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES` and failed when decoded.
- An object that transmitted before it was given a policy has no baseline when the policy arrives, so
  its first publish afterwards goes out regardless of send-on-change.
- Per-object `setCallback` and `setReadRequestSender` are gone (nothing in the stack, the tests, the
  examples or the Habinari firmware used them); the store has the table-wide ones.

*Result:* 60 objects take 960 B of entries and a pool of a few hundred bytes instead of 15 440 B
(14 400 B of objects and the 1040 B table). See the S2 status in §9.

### 6.3 S3: interface object property storage (RAM and flash)

**What the attribution found (the premise changed).** The S3 sketch assumed that
`GenericInterfaceObject`'s `std::map<PropertyID, std::vector<uint8_t>>` and its run-time registration
tables were a large part of the `tp1-small` heap. Measured, they are not on that device: a
`GenericInterfaceObject` (2296 B, of which the two fixed 64-entry handler tables are most) exists only
for the *reference* object types a product or the management model asks for, and the 60-object
reference device asks for none, so no property map is built. Where the 1.7 KB in the
`InterfaceObjectManager` went is stated in §9 (S3/S4 status): it was a second, never used set of table
objects.

**Built.**

- The `InterfaceObjectManager` no longer owns a second set of tables. Its default constructor (used
  by tests) allocates them; the BAU's manager, built over the BAU's own objects, owns none: 1668 B.
- Steady-state allocation in the stack's own code was removed where the traffic run found it (§6.4):
  the connection table's entries and the persistence save buffer.

**Not built.** A flat per-object value buffer and `constexpr` handler tables for
`GenericInterfaceObject`. The declarations (`referenceObjectProperties()`) are already `constexpr`
tables, but their `maxElements` is the protocol's limit (0xFFFF for a dozen properties, such as the
cEMI additional info types or the E-Mode lists), reported by the property description service, not a
storage capacity. A buffer sized from them is either huge or needs a *second* number per property, a
declared storage capacity (a capacity is a promise, §3.3 rule 2), which only the product that
instantiates the object can choose. That is a design decision for the products that use reference
objects (the IP and coupler profiles), not a saving on `tp1-small`, and it changes what a write of
more than the capacity returns, so it was not done here. `std::string` keys remain in the persistence
interface (`ObjectPersistence::save/load(const std::string&)`) for the named records; the interface
objects' records already use the `u16` ids on the save path.

### 6.4 S4: allocation policy (RAM; K7 step 8)

**Built.** `KNX_HEAP_POLICY` (Kconfig choice, menu "Memory"; §4.3): `free` (the default on host,
`full` and ESP-IDF) and `init_only` (`tp1-small`, `tp1-standard`).

- `include/knx/util/heap.hpp`, `src/util/heap.cpp`: `heap::lock()`, `unlock()`, `isLocked()`,
  `allocationCount()`, `violationCount()`, `setViolationHandler()`, `setHighWaterHook()`,
  `highWater()` and the decision function `admit()`.
- With `init_only` the library replaces the allocating `operator new` (and `new[]`, and the
  `nothrow` forms) with ones that ask `admit()` first and then call `malloc`; `operator delete` is the
  toolchain's. Locked, a debug build (no `NDEBUG`) executes `__builtin_trap()`; a release build refuses
  the allocation, which for `operator new` ends in `std::abort()` (the stack is built with
  `-fno-exceptions`, so that is what an exhausted heap does anyway; with exceptions it throws
  `std::bad_alloc`) and for `new (std::nothrow)` is a null return. A violation handler, when installed,
  is told the size and the return address and the allocation goes ahead (the gate uses this to list
  every steady-state allocation).
- **Who locks.** The product calls `heap::lock()` when its own start-up is over, not the runtime: a
  lock inside `startCommissionedProduct()` would also cover whatever the product allocates afterwards
  (a flash driver, an application task), which is the product's to decide. The product authoring
  guide says so.
- **High-water mark.** `_sbrk` belongs to the product, so `highWater()` calls a hook the product
  registers (the gate registers its `_sbrk` mark). No counting allocator is built into the library: an
  unsized `operator delete` cannot know a block's size without a header on every allocation.
- **`arena`** (one static block, no `malloc`) is not implemented; it would also need the `operator
  delete` side and a fixed-block allocator, and nothing has asked for the link-time-constant RAM
  figure yet.

**The check.** The gate image locks the heap after start-up and then runs its whole traffic sequence
(§7). Allocations that run found, and what was done:

| Found | Size | Action |
|---|---|---|
| `ConnectionTable::createConnection` (entry, state machine, vector slot) | 60 + 36 + 4 B per T_Connect | entries and state machines built in `init()`, kept in a spare list |
| `InterfaceObjectManager::saveToPersistence` (a `std::vector` per record) | 1 to 5 B, 35 times | one buffer reserved when persistence starts (642 B; +1 KiB with Data Secure) |
| The gate's persistence stand-in (`std::map` nodes, vector copies) | 32 B nodes and copies, 18 times | not the stack's: the stand-in now keeps its records in static storage, as a flash log would |

After these, the traffic run makes 0 allocations while locked.

**A trade made.** The save buffer is a permanent 642 B (the largest record is the association table
image); the transient vectors it replaces cost less at their peak but cannot exist under the lock.

## 7. Size gate and CI

The gate built for M1 (`test/baremetal/size_gate`, `scripts/ci/knstax_size_gate.py`) becomes a
CI job with one reference device per profile:

| Reference | Profile | Core | Checks |
|---|---|---|---|
| `tp1-small` | `tp1-small` | Cortex-M3 (QEMU `mps2-an385`) | Flash and RAM budget, forbidden objects, heap high-water under QEMU |
| `tp1-standard` | `tp1-standard` + Data Secure | Cortex-M4 (QEMU `mps2-an386`) | Flash and RAM budget, heap high-water under QEMU with the heap locked |
| `m0plus-small` | `tp1-small` | Cortex-M0+ | Builds and links (no hardware divide, no unaligned access); flash and static RAM budget |

The gate takes the profile and the core as parameters (`run_gate.sh <build-dir> [profile [cpu]]`,
or `KNX_SIZE_GATE_PROFILE` and the toolchain's `KNX_ARM_CPU`) and reads the budget file
`test/baremetal/size_gate/budgets/<profile>-<core>.json` (`tp1-small-m3`, `tp1-standard-m4`,
`tp1-small-m0plus`). The Cortex-M3 device (`mps2-an385`) and the Cortex-M4 device (`mps2-an386`)
are run under QEMU; the Cortex-M0+ one reports "heap not measured" and its RAM figure is static
data only. The CI job is `size-gate` in
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
  (Done in S3/S4 step 0: `traffic.hpp` in the gate; the run also locks the heap after start-up and reports every allocation after that, see §6.4.)
- **The traffic includes an ETS download.** The run ends with the session ETS makes (Unload,
  StartLoading, A_Memory_Write of the three tables, a verification read each, LoadCompleted),
  group traffic on the downloaded addresses and A_Restart. The gate refuses a heap figure
  from a run whose download did not take effect (`download_applied=1` in the output).
- **The whole device is the binding constraint.** A budget file may carry a `device` section: the
  MCU's flash and RAM, and the reservations of everything that is not KNstaX (the H8i8o budget,
  `01-system-design.md` §4.2: stack 2560 B, TP-UART buffers 512 B, application state 1024 B and
  margin 2560 B of RAM; application 12288 B, board support 4096 B and persistence 8192 B of
  flash). The gate adds the measured KNstaX static data and heap (RAM) and the measured KNstaX
  and C/C++ runtime flash, prints both sums with the headroom, and fails if either is over. It
  is in `tp1-small-m3.json`. The per-share budgets and targets above keep ratcheting; a `target`
  is where a share was meant to be, and for RAM it no longer says whether the device fits (its
  note in the output says so). The runtime's own static data is not in the RAM sum (it comes out
  of the margin) and the gate prints it. Without a heap figure the RAM sum is not made.
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
| C2 | Capacities as options (§4.2), including `KNX_MAX_APDU_LENGTH`, with a full-capacity test per table | K7.5 | **Done: see C2 status** |
| C3 | Feature gates (§4.1): coupler, Security Interface Object, GO diagnostics, optional management services, DPT families | K7.1–K7.3 | **Done: tp1-small flash 134995 to 99907 B, see C3 status** |
| C4 | Size gate in CI, per profile, with ratcheting budgets | K7.7 | **Done: see C4 status** |
| S1 | Table-driven product runtime | — | **Done: tp1-small flash 108275 to 79925 B, heap 40980 to 31108 B; decision point reached, see S1 status and §10** |
| S2 | Compact group objects | K7.4 | **Done: a group object is 16 B; tp1-small heap to 13.5 KB, see S2 status** |
| S3 | Interface object property storage | — | **Done: see S3/S4 status** |
| S4 | Allocation policy | K7.8 | **Done: `init_only`, the heap is locked after start-up in the gate; see S3/S4 status** |
| P1 | Reference STM32F1 port, run on the H8i8o board | — | **Done on hardware** (the H8i8o firmware, ETS6 download on a live bus); the generic part moves into `ports/gd32f1/` (§12, decision 3) |
| L1 | One staging buffer for the table load segments (§12, decision 4) | — | About 1 KB less heap in `tp1-small`; ETS6 download on hardware |

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
- **Out of scope, noted.** The parameter state was a per-parameter template and cost code per
  parameter. It has since been replaced by the parameter layout
  ([parameter-model.md](parameter-model.md)), which costs none. Runtime persistence semantics were not touched
  (`persisted` is still export-only).
- **Behaviour changes to know.** A bus write whose value does not match the port type and for which no
  handler is bound now yields `OperationNotReady`, not `DecodeFailed` (the callback discards the
  result; with a handler bound it is still `DecodeFailed`). A full pending-action queue is `QueueFull`
  as before; only the size differs.
- **Heap profile after S1** (`alloc <count> x <bytes>`): 60 x 240, 2796, 2720, 2272, 1764, 1224, 1032,
  528, 488, 376, 344, 320. The group objects are 14400 B of the 31108 B (46 %); the runtime object is the
  2272. S2 owns the first, S3 and S4 the rest. The 2796, 2720 and 1764 B items are not attributed here.

### S2 status

Done (§6.2 describes what was built), together with the callback capture size of §4.3. Two changes,
each leaving the host suites green (full 153, `host-small` 140, `host-minimal` 101) and the gate
passing. Figures are the `tp1-small` Cortex-M3 reference device.

| Step | Flash | Static RAM | Heap | Static + heap |
|---|---:|---:|---:|---:|
| After S1 (baseline) | 79925 B | 1443 B | 31108 B | 32551 B |
| 1. Group objects as a 16 B entry in one store (pool, policy side table, table callbacks) | 80642 B | 1443 B | 15592 B | 17035 B |
| 2. `KNX_CALLBACK_CAPTURE_BYTES` = 8 | 79335 B | 1443 B | 13488 B | 14931 B |

The other configurations after step 2: Cortex-M0+ flash 82638 B (static RAM 1443 B); `tp1-standard`,
Cortex-M4 flash 97715 B (static RAM 1596 B). The budgets were lowered to the new figures plus the
margin: `tp1-small` M3 81152 B flash and 15360 B RAM, `tp1-standard` 99840 B flash; the M0+ budgets
were already within the margin.

- **Group objects.** `sizeof(GroupObject)` went from 240 B to 16 B (`static_assert` on 32-bit
  targets). For the 60 objects of the reference device that is 14 400 B of objects and 1040 B of
  table slots (15 440 B) replaced by 960 B of entries and a value pool of a few hundred bytes
  (about 20 B per object in all): 15.5 KB of heap, a little more than the 13.6 KiB §6.2 expected,
  because the 1040 B pointer table went too. The flash cost is 717 B: the store, the side-table code
  and the exact-size `std::vector` growth.
- **Callback capture.** Setting the capture to 8 took 2104 B of heap and 1307 B of flash out
  (smaller callback objects need fewer and smaller copy and move thunks). The §10 review expected about
  2 KiB. Only one stack-internal lambda captured more than a pointer: `BusAccessStackPort`'s inbound
  callback copied the product's callback into itself; it now captures `this`. Everything else fits in
  8 bytes, including the `tp1-standard` feature set (Secure, the management services) and, checked by
  compiling the library alone, the `host-small` feature set (netip, coupler, Secure).
- **Where the option is on.** `tp1-small` and `tp1-standard` set 8. `full`, `host-test`, `host-small`
  and `host-minimal` keep 0: with 8, the tests of the host profiles fail to compile, because they pass
  lambdas that capture several locals (`[&]` over two or more variables, 16 to 60 B) to the public
  setters: 34 sites in 25 test files and the interop peers. Those are callbacks whose capture the
  test controls, so they were not rewritten. The consequence is that the host profiles do not
  themselves check that the stack's own lambdas fit in 8 bytes; the two gate images do, and the
  `host-small` library compiled at 8 when tried by hand. A permanent check would build the library
  alone at 8 for the host-small features. The aliases for the product's callbacks (bindings, endpoint
  policies, lifecycle and fault observers) are not affected by the option in any profile.
- **Heap profile after S2** (`alloc <count> x <bytes>`, Cortex-M3): 2224, 1772, 1764, 1696, 1168, 968,
  792, 328, 296, 260, 248, 240. The 968 is the group object entries (60 × 16 B and the allocator's
  header). The group object table object (1040 B) is gone. The other items were not re-attributed
  one by one; the callback-heavy ones (the interface object manager, `BusAccessUnit`, the data link
  layer) are the ones that shrank.
- **Behaviour changes to know.** A payload longer than its datapoint is refused (`BufferTooSmall`);
  an object that sent before it got a transmit policy publishes once regardless of send-on-change
  after the policy arrives; the group object table's read-request sender is installed by
  `GroupRuntime` for every object, product objects included. The inbound callback of the stack port is
  read when it runs, not copied at install time. Per-object `setCallback` and `setReadRequestSender`
  are removed (no users).
- **Compatibility.** The Habinari firmware's `knx_*.cpp` compile (syntax check against the ESP-IDF
  stubs, both media); it uses no `GroupObject` API. `GroupObject` and the table accept the calls the
  tests use; `flags()` and `transmitPolicy()` return by value now.
- **The threshold is still a `double` with `KNX_DPT_FLOAT`.** §6.2's idea of an integer threshold in
  the datapoint's raw units is not done: it would change what ETS parameters mean for float
  datapoints. Without `KNX_DPT_FLOAT` it is already `uint32_t` and no soft-float is linked
  (the gate forbids `Dpt9::` and `dptValueAsScalar`).

### S3/S4 status

Done in part (§6.3 and §6.4 say what was built and what was not). Step by step, each leaving the host
suites green (full 154, `host-small` 141, `host-minimal` 102) and the gate passing. Figures are the
`tp1-small` Cortex-M3 reference device; heap is the high-water mark of the emulator run.

| Step | Flash | Static RAM | Heap, start-up | Heap, with traffic | Static + heap, with traffic |
|---|---:|---:|---:|---:|---:|
| After S2 (idle run, no traffic) | 79335 B | 1443 B | 13488 B | (13488 B) | 14931 B |
| 0. The gate runs bus traffic through a scripted backend | 79335 B | 1443 B | 13488 B | 14532 B | 15975 B |
| 1. `InterfaceObjectManager` stops owning a second set of tables | 79079 B | 1443 B | 11736 B | 12780 B | 14223 B |
| 2. Connection entries and the save buffer at start-up; `init_only` policy; the gate locks the heap | 79780 B | 1456 B | 12500 B | 12500 B | **13956 B** |

The other configurations after step 2: Cortex-M0+ flash 82857 B (static RAM 1456 B); `tp1-standard`,
Cortex-M4 flash 98020 B (static RAM 1609 B). The `tp1-small` RAM budget went from 15360 B to 16384 B for
step 0 (the honest figure was above the old one) and to 14336 B after step 2.

**Step 0: the heap with traffic.** The emulator variant of the gate starts the device with a scripted
`Tp1MediumBackend` (`test/baremetal/size_gate/traffic.hpp`, in the emulator image only). It delivers
telegrams through the backend's event callback, as the driver's receive path does, and answers every
transmission with the ACK event of a TP1 driver; the stack's receive queue, data link, transport,
application layer and management services all run. The sequence: the individual address and seven
group addresses set the way a commissioning leaves them; group writes to each datapoint type, two group
reads the device answers, a read response, the product's publishes (twice), and a management session
from 1.1.11 (T_Connect, A_PropertyValue_Read and _Write, A_Memory_Write and _Read of the application
code and group address table segments, A_DeviceDescriptor_Read, T_Disconnect, with the tool's T_ACKs).
19 frames sent, 19 confirmed; five connected responses (property read and write, two memory
responses to the reads, device descriptor); the two memory writes are not answered, as the service says with verify mode off. The inbound ports observed 8 values. Not in the sequence:
a full ETS download of the tables (the address table is bound through the runtime's binding call, and
a memory write into the table segments is made, but no load-complete event), the device's own read
requests, and data secure. Result: **+1044 B over start-up and 56 allocations after start-up**,
before any change to the stack.

**Step 1: attribution** (`sizeof` on the `tp1-small` configuration, Cortex-M3; the allocation sizes
are from the gate's allocation profile at the end of start-up):

| Heap item | Bytes | Made of |
|---|---:|---|
| `CommissionedProductRuntime` | 2224 | endpoint runtime, bindings, parameter state, publish queue (done in S1) |
| `InterfaceObjectManager` | 1772 | **1668 B: a second, never used set of the five table objects** (removed, step 1); 104 B own state |
| `DeviceModel` | 1764 | the four memory-mapped load segments: 258 (group address table) + 642 (association table) + 514 (group object table) + 256 (application code) = 1670 B; 94 B own state |
| `BusAccessUnit` | 1696 | the table objects it owns: device object 280, address table 272, association table 980, application program 120, group object table 16 (1668 B); 28 B own state |
| `Tp1DataLinkLayer` | 1168 | frame pool 576, group address list 260, last-received cache 36, transmit and receive operations 56 + 20, statistics 28; the rest is own state |
| Group object entries | 968 | 60 x 16 B and the allocator's header |
| `ApplicationLayer` | 792 | inline `ManagementServer` 632 (registered objects 132, memory segment bindings 392), send pipeline 100 |
| `BusAccessStackPort` | 328 | group address list 260 |
| `TransportLayer` | 296 | receive queue 88, connection table 36 |
| `Tp1MacPhysical` | 240 | receive queue and ACK state |
| `GroupRuntime` | 216 | automatic response queue 64, rate limiter 28 |
| `PropertyServices`, `MemoryService`, `NetworkLayer`, others | about 300 | 112, 104, 80 |
| Not attributed one by one | about 800 | allocations of 260, 248, 176 and 136 B, and about 30 under 120 B |

`GenericInterfaceObject` (2296 B) is absent from the profile (§6.3). `DeviceObject` keeps its serial
number, hardware type, order info and description as `std::vector`/`std::string` (the small 6 and 10
byte allocations); `std::map` is not used on the `tp1-small` heap path at all.

**The address-table-related containers** (not deduplicated here; queued separately):

| Container | Owner | Bytes (128 addresses, 160 associations) |
|---|---|---:|
| Group address table | `AddressTableObject` in the BAU | 264 (272 with its object state) |
| Group address list | `BusAccessStackPort` | 260 |
| Group address list | `Tp1DataLinkLayer` | 260 |
| Group address load segment | `DeviceModel` | 258 |
| Association table | `AssociationTableObject` in the BAU (entries 640 + by-address index 320) | 972 (980) |
| Association load segment | `DeviceModel` | 642 |
| Group object table load segment | `DeviceModel` | 514 |
| (The `InterfaceObjectManager`'s unused copies of the first and fifth rows' tables) | removed in step 1 | (264 + 972) |

At that point the group address list existed four times (1042 B; two copies are gone, see the follow-up below) and the association table twice (1614 B). The
load segments (1670 B in all, with the application code segment) are the staging copies of an ETS
download; only the application code segment is a segment of its own.

**Step 2.** The connection table and the save buffer are described in §6.4, with the allocation the
locked run found and the gate's own persistence stand-in. The new unit tests are `test_heap_policy`
(the decision function, the handler contract, the high-water hook) and `test_ReusedEntryStartsClean`
in `test_connection_table`; `test_knx_genconfig` checks the policy per profile.

- **No allocation after start-up.** The gate prints `heap_locked_allocations=0` and
  `heap_high_water` equal to `heap_after_start`. This covers receive and send of group traffic, the
  automatic read responses, the publishes, a management session and the persistence saves the
  commissioning writes cause.
- **What the lock does not prove.** A full ETS download (the load state machine for each table, with
  the table applying 128 + 160 entries), the secure paths and the `tp1-standard` feature set are not
  in the run. The `tp1-standard` profile is set to `init_only` as the design says, but its secure
  persistence records are sized by a guess (+1 KiB) and nothing locks it under traffic; until that is
  run, a `tp1-standard` product that locks the heap is untested.
  (Since then the download and the `tp1-standard` feature set without Secure are in the run; see
  "Follow-up: ETS download, one address list, whole device" below. The secure paths still are not.)
- **Persistence-format impact: none.** The stored keys, values and the schema version are unchanged;
  the save path builds the same records in a different buffer.
- **API changes.** New: `knx/util/heap.hpp`, `KNX_HEAP_POLICY`. Changed: none for products. The
  `InterfaceObjectManager`'s default-constructed objects are behind a pointer (nothing outside the
  manager used the members). `ConnectionTable::init()` now allocates (one entry per allowed
  connection).
- **ESP32 / Habinari.** The `knx_*.cpp` of the Habinari firmware compile (syntax check against the
  ESP-IDF stubs, both media): no change needed. ESP-IDF builds keep `KNX_HEAP_POLICY=free`.
- **Flash.** +701 B over step 1 (79079 B to 79780 B; +445 B over the S2 figure): the policy and its `operator new` about
  140 B, the connection spare list and the changed save path the rest. The gate's flash budget is
  unchanged (81152 B).

### Follow-up: ETS download, one address list, whole device

Figures are the `tp1-small` Cortex-M3 reference device, heap measured with the traffic and locked
after start-up. "Heap, start-up" is the high-water mark when the device has started; "heap, with
download" is the peak at the end of the run, which includes the ETS download, its persistence saves
and the flush at A_Restart.

| Step | Flash | Static RAM | Heap, start-up | Heap, with download (peak) | KNstaX total |
|---|---:|---:|---:|---:|---:|
| Before (S3/S4 step 2; the traffic had no download) | 79780 B | 1456 B | 12500 B | (12500 B) | 13956 B |
| 1. A full ETS download in the traffic | 79780 B | 1456 B | 12500 B | 12500 B | 13956 B |
| 2. One copy of the group address list | 79890 B | 1456 B | 12004 B | 12004 B | **13460 B** |
| 3. Load segments | not done, see below | | | | |

**Step 1: the download.** `traffic.hpp` builds the session from `downloadLikeEts` of
`test_ets_connected_session.cpp`: T_Connect; for the address table (index 1), association table (2)
and group object table (3) in turn Unload, StartLoading, A_Memory_Write in 12-octet frames (the most
a 15-octet APDU carries), one A_Memory_Read of the first chunk, LoadCompleted (frames 40 ms apart);
eight group telegrams on the downloaded addresses (writes, reads the device answers, a read
response); A_Restart, which ends the emulator run through the platform's restart hook. The tables
are those of a real I/O module of this size: 60 group addresses (1/0/1 to 1/0/60, one per object),
80 associations (each object to its address, and 20 objects that also listen to the address of
another object of their type) and 60 group object descriptors. The reference device has no
parameters, so there is no application code segment; Verify Mode is off. 71 more frames reach the
device, and the gate checks that the ports then observed exactly the values the downloaded tables
route (15 in all, 7 after the download) before it accepts the heap figure.
- **Finding: nothing allocates.** Zero allocations with the heap locked through the download, the
  persistence saves it causes (each table's LoadCompleted marks the state dirty and the coalesced
  save follows) and the flush at A_Restart; the peak equals the start-up figure. The connection
  entries and the save buffer moved to start-up earlier (§6.4) are what made that so.
- **Limits.** The restart ends the run, so the group traffic runs before it and the boot that follows
  a restart (the restore from persistence) is not run. The additional load controls of ETS's
  DefaultProcedure (segment allocation, data relative allocation) are not sent: the stack treats the
  event as a no-op and the segments sit at fixed addresses. The associations use the port slot as
  ASAP (0-based), because the stack applies ASAP unchanged as the group object index.

**Step 2: one copy of the address list** (`ARCHITECTURE.md`, "Who owns the group address list").
The list existed in the address table (264 B), the stack port (260 B), the data link layer (260 B) and
the `DeviceModel` load segment (258 B). The stack port's and the data link layer's lists are gone:
the data link layers filter and acknowledge on the address table's own storage through a view
(span and sorted flag) that the stack port publishes after every change of the table. Heap 12500 B
to 12004 B (the two lists, 496 B with their allocator headers), flash +110 B. The acknowledge list the
physical layer takes is still built into a transient array and replaced under the backend's own
lock, so the interrupt-side matcher never reads the table; the publication is a pointer-and-count
store in the stack's own context. A data link layer used on its own keeps `addGroupAddress()` with a
list made on first use (tests). One behaviour changes, deliberately: an address that is no longer
in the table is no longer accepted or acknowledged. The old lists only grew, so after an Unload or a
download with fewer addresses the device went on acknowledging the old ones until the next restart
(test: `test_group_address_filter_follows_the_address_table`).

**Step 3: the load segments, not done (decision for the owner).** What the code does:
- Tables are canonical; the segment is staging. A_Memory_Write lands in the segment; at LoadCompleted
  `applyLoadedTableSegment` parses it into the table (address and association table; the application
  code segment goes to `ApplicationProgramObject::setProgramData`; the group object table segment is
  accepted and not applied, its content being firmware-defined). Only the tables are persisted. Memory
  reads (including Verify Mode's read-back) read the segment, not the table: after a boot the segment
  is zeros, and after the firmware binds an address the segment still holds what ETS wrote.
- So a staging buffer is needed during a download (the table is only built at LoadCompleted, and a
  verification read before it must return what was written; a write is not entry-aligned either: a
  12-octet chunk of the association table straddles entries), but the four copies need not exist
  at once. One buffer of 642 B would replace 1670 B (about 1020 B of heap; the device RAM headroom
  would go from 364 B to about 1380 B).
- What cannot be kept with one shared buffer. (a) Two tables staged at the same time (StartLoading on
  two objects before either is completed) would overwrite each other: it can be refused cleanly (a
  write guard claims the buffer; a refused write and a LoadCompleted without staging fail the load
  state, which ETS reports), but today it works. (b) A memory read of a segment that is not the one
  in the buffer cannot return what was written. For the address and association tables it could be
  served from the table (a different answer after a boot and after a firmware-side change, a better
  one); the group object table segment has no table behind it and would read as zeros once the buffer
  is taken. (c) It needs a read hook on a memory region (the management server only has a write
  guard and a fixed storage span today).
- Whether ETS ever interleaves the loads or reads a segment back after LoadCompleted is not known
  from this repository (the sequence ETS makes in `test_ets_connected_session.cpp` is sequential, with
  the read-back before LoadCompleted). It should be confirmed with an ETS6 download on the H8i8o
  before any of this is built. As a build option (`tp1-small` only, off elsewhere) the deployed
  ESP32 devices would keep today's behaviour exactly. Persistence format and power-cut recovery do
  not depend on any of it.

**Step 4: `tp1-standard`.** `run_gate.sh` runs the Cortex-M4 reference device on `mps2-an386`
with the same traffic and the heap locked. Flash 98264 B, static RAM 1609 B, heap 18304 B at start-up
and at the end (**19913 B in all**), zero allocations after start-up, download applied. The
`init_only` policy of the profile stands, for this feature set. It has no Data Secure yet (K6), so the
secure persistence records and the secure paths are still not under the lock. The RAM budget of the
profile now counts the heap: 20480 B (it was a static-only 1792 B). The Cortex-M0+ device is linked,
not run (flash 82959 B, static RAM 1456 B).

**Step 5: the whole device.** `tp1-small-m3.json` has a `device` section (§7). With the figures above:
RAM 13460 (KNstaX) + 2560 + 512 + 1024 + 2560 = **20116 of 20480 B, headroom 364 B**; flash 79890
(KNstaX) + 7509 (runtime) + 12288 + 4096 + 8192 = **111975 of 131072 B, headroom 19097 B**. The RAM
budget of the per-share check is lowered to 13824 B.

## 10. Projection and decision points

Estimated `tp1-small` figures for the 60-object reference device after each stage. Only the
first row is measured.

| After | Flash (KNstaX) | RAM (KNstaX) | Measured |
|---|---:|---:|---|
| Today (M1) | 132 KiB, measured | 67 KiB, measured | |
| C2 | | | 131.8 KiB flash, 43.2 KiB RAM (1.7 static + 41.5 heap) |
| C2 + C3 | about 115 KiB | about 30 KiB | 97.6 KiB flash, 41.4 KiB RAM (1.3 static + 40.0 heap) |
| S1 | about 88 KiB | about 18 KiB | **78.1 KiB flash, 31.8 KiB RAM (1.4 static + 30.4 heap)**; on the representative device with bindings, S1 took 108275 B / 42423 B down to 79925 B / 32551 B |
| S2 | | | **77.5 KiB flash, 14.6 KiB RAM (1.4 static + 13.2 heap)** (with the callback capture size) |
| S2 + S3 + S4 | about 80–85 KiB | about 8–10 KiB | |
| S3 + S4 (as built) | | | **77.9 KiB flash, 13.6 KiB RAM (1.4 static + 12.2 heap), with traffic and the heap locked** |

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

**Review after S2 (16 November 2026).** Measured on the `tp1-small` Cortex-M3 reference device: flash
77.5 KiB (79335 B), RAM 14.6 KiB (14931 B: 1443 B static and 13488 B heap).

- **RAM: below the review's estimate.** The review projected about 16 KiB after S2 and the callback
  capture size; the measurement is 14.6 KiB. S2 took 15.5 KB off the heap (the estimate was 13.6) and
  the capture size 2.1 KB (the estimate was about 2). The §10 projection for S2 + S3 + S4 was 8–10 KiB,
  so S3 and S4 have to remove 4.6 to 6.6 KiB more from the interface object manager (2224 B),
  `BusAccessUnit`, `DeviceModel` (1764 B) and the rest of the heap list in the S2 status.
- **The board's budget.** The group objects are about 1.2 KB, inside the 2 KiB the H8i8o budget gives
  them separately; the KNstaX share without them is about 13.4 KiB against the board's 10 KiB. The
  owner's reading of "which RAM target" (the Review above) still applies.
- **Flash: unchanged in effect.** 77.5 KiB, 10.5 KiB under the 90112 B target. S2 cost 717 B of flash
  and the capture size gave 1307 B back.
- **Outcome.** The GD32F303CC fall-back is still not needed on present evidence. The work continues
  with S3 (interface object property storage, `DeviceModel`, `BusAccessUnit`) and S4; the next review is
  after S3.

**Review after S3 and S4 (16 November 2026).** Measured on the `tp1-small` Cortex-M3 reference device,
with bus traffic and the heap locked after start-up: flash 77.9 KiB (79780 B), static RAM 1456 B, heap
12500 B (the same at start-up and after the traffic run), **KNstaX RAM 13956 B (13.6 KiB)**.

- **Against 10240 B: 3716 B over.** Not met. The three steps took the figure from 15975 B (the honest
  starting point, with traffic) to 13956 B: 2.0 KB, almost all of it the unused second set of tables in
  the `InterfaceObjectManager`. The property storage S3 was sketched around is not on this device (§6.3).
- **Against 12288 B (the board budget, which counts group objects separately): about 470 B over.**
  The group objects are about 1.2 KB of the figure (entries 960 B and the value pool), so the KNstaX
  share without them is about 12.7 KB. Which of the two targets applies is the owner's decision.
- **The device's constraint: 20 KiB RAM and 128 KiB flash for everything.** KNstaX takes 13.6 KiB of
  the 20 KiB (67 %) and 77.9 KiB of the 128 KiB of flash (61 %), leaving 6.4 KiB of RAM and 50 KiB of
  flash for the product, the board support and the stack. The figure is a high-water mark with
  traffic, not a start-up number, and it is final for the run: with the heap locked it cannot grow.
- **What is left, in order of size** (all attributed in §9; none done here):
  - the load segments (1670 B): staging copies of an ETS download of three tables and the application
    code, next to the tables themselves;
  - the duplicated lists: the group address list four times (1042 B) and the association table twice
    (1614 B). Removing the copies in the stack port and the data link layer and the association
    segment would take about 1.1 KB without touching behaviour that a table load shows; this is queued
    separately;
  - `CommissionedProductRuntime` 2224 B, the data link layer's frame pool (576 B), the management
    server's memory segment bindings (392 B for what is four segments);
  - the property storage rewrite of §6.3, if a product that uses reference objects needs it.
  With the load segments and the two list copies in the stack port and the data link layer (about
  2.2 KB) the figure would be near 11.8 KB: above 10240 B, below 12288 B.
- **Outcome.** The 12288 B reading of the target is reachable with the duplicates removed; the 10240 B
  one is not on present evidence even with the segments removed. The GD32F303CC
  fall-back is not needed for RAM on the 20 KiB device.

**Review after the ETS download, the single address list and the device check (16 November 2026).**
Measured on the `tp1-small` Cortex-M3 reference device with the ETS download in the traffic and the
heap locked: flash 78.0 KiB (79890 B), static RAM 1456 B, heap 12004 B (start-up and peak),
**KNstaX RAM 13460 B (13.1 KiB)**.

- **The download was the open question of the last review, and it is closed:** no allocation under
  the lock through a full download of 60 addresses, 80 associations and 60 group object descriptors,
  its saves and the restart flush. The 12.5 KB figure was not hiding a download peak.
- **The constraint that counts is now measured.** The whole device (KNstaX plus the H8i8o
  reservations, §7) needs 20116 of 20480 B of RAM: **headroom 364 B**, and 19097 B of 131072 B of
  flash. Flash has room (the 8 KiB "Data Secure, stretch" line of the board budget would fit in the headroom). RAM
  has almost none: 364 B for anything the reservations do not name (Data Secure's 1.5 KiB RAM
  stretch does not fit; group objects are inside the KNstaX figure here, so the board's separate
  2 KiB line is not additional).
- **What is left, in order of size.** The load segments (1670 B; about 1020 B recoverable with one
  shared staging buffer, at the cost described under step 3 of the follow-up, which needs the
  owner's decision and an ETS check); `CommissionedProductRuntime` 2224 B; the data link layer's
  frame pool (576 B); the management server's memory segment bindings (392 B); the association
  table's by-address index (320 B).
- **Against 10240 B and 12288 B.** 3220 B over the first, 1172 B over the second. With the shared
  staging buffer the figure would be near 12.4 KB: still above 12288 B by about 150 B, with the
  device headroom near 1.4 KB.
- **`tp1-standard`** (no Secure yet) measures 19913 B under the lock with zero allocations: the
  `init_only` policy stands for it.

## 11. Risks

| Risk | Mitigation |
|---|---|
| Kconfig for standalone builds disturbs the host and ESP-IDF builds | C1 changes no behaviour: the `full` profile must produce the same host test results and an ESP-IDF image of the same size |
| A feature switched off in `tp1-small` turns out to be needed by ETS | Every `n` in §4.1 is confirmed with an ETS6 download on the H8i8o board before it becomes the default |
| A 15-byte APDU makes ETS downloads too slow | Measure the download time on hardware; 55 is the fall-back, at about +1 KiB of RAM |
| S1 changes the product authoring surface | The typed API stays; exporter and runtime read one descriptor table; the existing product tests are the acceptance test |
| Capacity limits cause silent misbehaviour | Rule 2 in §3.3: errors, negative responses and a test per capacity |
| The projection is wrong | The gate measures every step; the decision point in §10 |

## 12. Decisions (21 November 2026)

The open questions of this document are closed as follows.

1. **`tp1-small` keeps the A_send pipeline.** Its queue is already 2 entries deep in this
   profile, so sending synchronously would save tens of bytes, change the timing contract for
   products, and add a code path that exists in one profile only. Not done.
2. **No `arena` heap policy.** `init_only` gives what mattered: nothing allocates after
   start-up, the high-water mark is measured, and the gate enforces both. An arena only helps a
   target without `malloc`, and none exists. Reopened if a port needs it.
3. **The reference port lives in KNstaX**, in `ports/gd32f1/` (GD32F103, register-compatible with
   STM32F103 for the peripherals used), and CI builds its example. It is the H8i8o's board
   code made generic: start-up, linker script, system calls, the UART HAL over a USART, the
   flash HAL over the FMC and an RTT console. A product adds it to its firmware with
   `knx_gd32f1_port()` and configures it with a `gd32f1_port_config.h`, or copies it; products
   keep their application, pin map and product file. Its README is the porting guide §8 asked
   for.
4. **The load segments share one staging buffer** (the follow-up's step 3), as an option that is
   on in `tp1-small` only. A memory read of the address or association table segment is answered
   from the table; a second table that starts loading while another is staged is refused, and
   ETS reports the failed load. It is confirmed with an ETS6 download on the H8i8o before the
   option is relied on: whether ETS ever stages two tables at once or reads a segment back after
   LoadCompleted is not known from this repository.
