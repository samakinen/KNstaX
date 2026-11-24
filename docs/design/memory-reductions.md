# Design: further RAM and flash reductions for `tp1-small`

| | |
|---|---|
| Status | **Implemented**, 21 November 2026: packages P1–P5 integrated and measured together (§9). Attribution measured; savings in the proposal sections are the estimates made before the work |
| Goal | Room for Data Secure ([data-secure-small-target.md](data-secure-small-target.md)) and for products on 20 KiB parts, and the `tp1-small` flash budget met again. |
| Builds on | [small-target-profile.md](small-target-profile.md) §10, whose heap attribution of 16 November this replaces. |

## 1. Starting point (measured)

Size gate, `tp1-small` reference device (60 group objects), Cortex-M3, LTO. The attribution below
was made at `673a005`; `eb54dfa` (the Data Secure sequence reserve and the SAVE path) changed only
flash: 88 709 B against a raised budget of 90 624 B. The RAM figures are identical.

| | Measured | Budget |
|---|---:|---:|
| KNstaX flash | 88 613 B (`eb54dfa`: 88 709 B) | 88 064 B (`eb54dfa`: 90 624 B) |
| KNstaX static RAM | 1 608 B | |
| Heap high-water | 11 724 B (40 allocations, all at start-up) | |
| KNstaX RAM | 13 332 B | 13 856 B |
| Device RAM headroom (H8i8o reservations) | 1 100 B | |

**Method.** In a scratch copy, `operator new` and `malloc` were wrapped to record the caller, and the
QEMU run of the gate image was traced with GDB over QEMU's gdbstub, one backtrace per allocation.
Callers were attributed on a build without LTO (allocation sizes are identical: 11 724 B either
way). Object layouts come from `ptype /o`. Flash was attributed on the LTO image through the line
table, and per object on the non-LTO map.

## 2. Where the RAM goes

### 2.1 Heap (11 724 B)

| Owner | Bytes | What makes it this size |
|---|---:|---|
| `CommissionedProductRuntime` | 2 272 | bindings 1 424 (commands 44 × 12, state writes 44 × 12 although 8 ports use them, providers 24 × 12); 8 pending actions × 40 B, each a `DptValue` variant; the product definition held twice (2 × 60); two `std::string` |
| `BusAccessUnit` | 1 712 | association table 980 (entries 644 + by-address index 324); device object 284 (six callbacks, `FixedVector<std::function, 4>`, two `std::string`); address table 284; application program object 116 |
| `DeviceModel` | 1 520 | load segments: address table 258, association table 642, group object table 514 (fixed at 256 entries whatever the product has) |
| Group objects | 1 120 | 60 × 16 B descriptors and values; `new[]` cookie |
| Management services | 1 080 | address space 260 (16 regions), authorisation 248 (16 devices), `NetworkParameterService` 136 **built although `KNX_ENABLE_NETWORK_PARAMETER` is off**, the rest mostly callbacks |
| `Tp1DataLinkLayer` | 968 | a 16-frame pool (576 B); frames are taken and released inside one receive pass |
| `ApplicationLayer` | 736 | management server 568: 8 memory segment bindings 328, 32 registered-object slots 132 |
| Persistence record scratch | 648 | a third copy of the association table, used only while saving |
| Transport and connection table | 456 | |
| `GroupRuntime` and its queue | 456 | |
| `Tp1MacPhysical` | 248 | 4-frame receive ring |
| Others | 508 | |
| *of which `malloc` headers* | *328* | *about 8 B per allocation* |

### 2.2 Static RAM (1 608 B)

| Symbol | Bytes |
|---|---:|
| `kDeviceHandlers` | 532 |
| `kAppProgramHandlers` | 448 |
| `kGroupObjectTableHandlers`, `kAssociationTableHandlers`, `kAddressTableHandlers` | 3 × 112 |
| `g_flashLog`, newlib `_impure_data`, platform, gate stub | about 290 |

The five property handler tables (**1 316 B, 80 % of the static share**) are declared
`static const PropertyHandler[]` but built by non-`constexpr` `ScalarProperty<>::make(...)` calls,
so they are filled at start-up into RAM, by a static initialiser that is itself 1 632 B of flash.

## 3. Where the flash goes (LTO image, by source)

| Area | Bytes |
|---|---:|
| libstdc++ templates (`stl_algobase.h`, `unique_ptr.h` teardown chains, atomics, `std::function`) | 12 900 |
| Interface objects (`property_kernel.hpp` 4 154) | 12 050 |
| `util` (`inplace_function.hpp` 6 698, `fixed_vector.hpp` 1 378) | 9 518 |
| newlib, libgcc, libstdc++ without line information | 7 848 |
| Physical (TP-UART backend, MAC) | 5 388 |
| Product runtime (`endpoint_bindings.hpp` 2 988) | 5 020 |
| BAU, device model, group runtime | 5 018 |
| Management server and services | 4 732 |
| Application layer, group objects, send pipeline | 3 962 |
| Transport / datalink / persistence | 3 418 / 3 272 / 3 234 |
| `std::variant` (`DptValue`) | 3 184 |
| `std::vector` / `std::string` / `std::map` and `std::set` | 2 882 / 1 792 / 532 |
| DPT codecs | 1 764 |
| Static initialiser of the handler tables | 1 632 |

Also linked: printf (about 2.2 KiB, through `CooperativePlatform::log`'s vtable entry even at log
level 0), and 80 destructors (3 024 B) of objects a device never destroys.

## 4. What drives it

Four patterns explain most of the figures above:

1. **Run-time construction of what is known at compile time.** Property handler tables, group
   object descriptors, product bindings and the product definition are built in RAM at start-up,
   although the product is a `constexpr` declaration.
2. **The same table kept several times.** The association table exists as the raw load segment,
   the parsed entries, a by-address index and a persistence scratch copy: about 2.6 KiB for one
   640-octet table.
3. **Capacities that do not follow the configuration or the product.** The 256-entry group object
   segment, the 16-frame pool, 16 address-space regions, 32 object slots, 16 authorised devices,
   44 state-write slots for 8 users, and a service built for a feature that is off.
4. **The heap as the object model.** Every layer is a `unique_ptr`, wired with `InplaceFunction`
   callbacks (about 60 × 16 B) that mostly capture only `this`. That costs `malloc` headers,
   teardown code that never runs, and indirect calls the compiler cannot inline.

## 5. Reductions

Ranked by RAM saved for the work. **M** = measured, otherwise estimated.

| # | Change | RAM | Flash | Where |
|---|---|---:|---:|---|
| R1 | `constexpr` property handler tables (`make()` `constexpr`, tables in `.rodata`) | **1 316 M** | about 1 100 | `property_kernel.hpp`, the five object `.cpp` files |
| R2 | One copy of each group table: persist from the segment (no scratch), size the group object segment by the product, keep either the raw segment or the parsed form, drop the by-address index | 1 900–2 300 | small | `device_model.*`, `table_segments.hpp`, `association_table_object.*`, `address_table_object.*`, `interface_object_manager.*` |
| R3 | Product bindings by compile-time dispatch from the `constexpr` product (no per-port tables in RAM); at least size state writes by the ports that use them | 430–1 300 | some | `endpoint_bindings.hpp`, `product_port_core.*` |
| R4 | Group object descriptors (DPT, length, offset, policy) as a `constexpr` table; RAM keeps flags and values only | 700–850 | small | `group_object.*`, `group_object_table_object.*` |
| R5 | Direct calls instead of run-time callback wiring between layers; no `NetworkParameterService` when its feature is off | 700–900 | 1 000–2 000 | management server and services, `bau.hpp`, transport, network, datalink |
| R6 | **Done:** the frame pool replaced by one receive frame (peak use measured at 1: a pass decodes, delivers and releases each frame before reading the next). The transport receive queue is only used when no callback is registered, which a device never is: a candidate for removal | **568 M** (`tp1-standard` 1 168 M) | **172 M** | `tp1_data_link_layer.*`; `transport_layer.hpp` open |
| R7 | Address space and memory segment bindings merged; registered objects and authorised devices sized by configuration | about 600 | small | `management_server.hpp`, `address_space.hpp`, `authorization_service.hpp` |
| R8 | Pending actions as a dirty bitset, values read back when drained, instead of 8 `DptValue` copies | about 300 | small | `endpoint_runtime.hpp`, `product_port_core.*` |
| R9 | One statically allocated device (no heap, no `unique_ptr`, no teardown) | **328 M** (headers) + 32 | 3 000–4 000 | `bau.cpp`, `bau_stack_port.cpp`, `commissioned_product.hpp`, `management_server.cpp` |
| R10 | No `std::string` (six fields, flash log keys); the product definition held once | about 250 | about 1 500 | `device_object`, `application_program_object`, `flash_log_persistence`, `commissioned_product.hpp` |
| F1 | `CooperativePlatform::log` compiled out at log level 0 | 76 | **about 2 200** | `cooperative_platform.cpp` |
| F2 | `DptValue` as a tagged plain struct (DPT + 14 octets) instead of `std::variant` | (part of R8) | about 2 500 | `application/dpt.hpp` |
| F3 | No `std::map` / `std::set` (generic interface object values, the unique-address count, which would also allocate under the lock if it ran) | | about 900 | `generic_interface_object.*`, `group_object_table_object.cpp` |
| F4 | `std::function` in the device object as `InplaceFunction` or a function pointer | 68 | about 900 | `device_object.*` |
| C1 | Capacities by profile or product override: 64 addresses / 96 associations for a device with this many objects (each association about 14 B over its copies, each address about 4 B) | about 1 100 | | profile or product `knx.defconfig` |

**Together:** R1–R10 about 6–8 KiB of the 13 332 B of RAM. R1, R9, R10 and F1–F4 about
12–14 KiB of flash, which clears today's 549 B overrun many times over.

## 6. Direction

Since the stack has no external users to keep compatible, the reductions point at one design rather
than a list of patches:

- **The device is a `constexpr` description plus one static object.** Handler tables, group
  object descriptors, bindings and capacities come from the product declaration at compile time
  and live in flash. The run-time state (tables ETS loads, values, queues) is one statically
  allocated object whose size the linker reports, and the RAM figure is known at link time. This
  is not the `arena` heap policy that small-target-profile.md §12 (decision 2) declined: the
  allocator and `init_only` stay. P6 removes the allocations themselves, which is what takes the
  `malloc` headers, the teardown code and the indirect calls with them.
- **Every loaded table exists once,** in the form ETS writes it, and is read in place.
- **Layers call each other directly.** A callback remains only where a product supplies behaviour.
- **Capacities are configuration,** checked against the product at compile time.

Order: R1 and F1 first (small, independent, measured, and enough to bring flash back under
budget), then R2 with C1, then R9 with R5 (the structural change, which R3, R4 and R8 then fall
out of), then the rest. Each step reruns the size gate and lowers the budgets.

## 7. With Data Secure

The measured device headroom is 1 100 B. Data Secure as designed (fixed tables, in-place CCM,
APDU 55) needs about 2 KiB of RAM and about 200 B of stack (data-secure-small-target.md §6,
software-aes.md §5). R1 and R2 alone (3–3.5 KiB) cover it on the reference device without any
product-side saving.

## 8. Implementation plan

### 8.1 Rules every change follows

- **One behaviour.** No change alters what ETS or a bus partner sees, except where §5 names it.
  The size gate's traffic run (an ETS download of three tables, group traffic, the ports checked
  against what the tables route) and the host tests are the evidence.
- **Every target gains.** A reduction is made in the shared code, not behind `#if` for small
  parts. Where a capacity is the lever, it becomes a Kconfig symbol with the old value as the
  `full` default, so ESP32 and host builds keep their behaviour and choose smaller numbers when
  they want them. ESP-IDF reads the same Kconfig, so a new symbol needs a default and help text
  there too.
- **DX does not get worse.** Product code (the `constexpr` product declaration, the callbacks it
  binds, `startCommissionedProduct()`) keeps compiling unchanged. A capacity that is exceeded is
  a compile-time error naming the symbol, or a negative response at run time, never silent.
- **Measured.** Each package states its size gate figures before and after (`tp1-small` M3 and
  `tp1-standard` M4) and lowers the budgets to the new figure plus the gate's margin.

### 8.2 Work packages

Grouped so that packages touch disjoint files and can be built in parallel; P5 depends on the
others being in.

| Package | Items | Main files | ESP32 and host |
|---|---|---|---|
| **P1 Interface objects** | R1 `constexpr` property handler tables; F3 no `std::set` in the unique-address count; F4 no `std::function` in the device object | `property_kernel.hpp`, the object `.cpp` files, `group_object_table_object.cpp`, `device_object.*` | Same saving in internal RAM; less static-initialisation time at boot |
| **P2 Platform and management** | F1 logging compiled out at level 0; `NetworkParameterService` only with its feature; R7 address space, memory segment bindings, registered-object and authorised-device capacities as configuration | `cooperative_platform.cpp`, `management_server.*`, `address_space.hpp`, `authorization_service.hpp`, Kconfig | `full` keeps today's capacities |
| **P3 Group tables** | R2 one copy of each loaded table; the group object segment sized by the product | `device_model.*`, `table_segments.hpp`, `association_table_object.*`, `address_table_object.*`, `interface_object_manager.*` | Same saving everywhere; the persistence format does not change |
| **P4 Frames** | R6 the data link layer's frame pool by configuration, sized from measured peak use | `tp1_data_link_layer.*`, `tp1_mac_physical.*`, Kconfig | `full` keeps 16 |
| **P5 Product runtime** | R3 bindings sized by the product; R4 group object descriptors as a `constexpr` table; R8 pending actions as a bitset | `endpoint_bindings.hpp`, `endpoint_runtime.hpp`, `product_port_core.*`, `group_object.*` | Same saving everywhere; product code unchanged |
| **P6 Static device** | R9 one statically allocated device; R5 direct calls between layers; R10 no `std::string`; F2 `DptValue` without `std::variant` | BAU, stack port, layers, `dpt.hpp` | A device whose RAM is known at link time; ESP32 loses heap fragmentation at boot |

P1–P4 first, in parallel, then P5, then P6, which is the largest and benefits from the others
having removed the duplicate state it would otherwise have to carry.

### 8.3 Verification

On this project's Windows workstation, where the Linux host suite (ucontext fibres) cannot run:

- the bare-metal size gate under QEMU for `tp1-small` (M3) and `tp1-standard` (M4): figures,
  zero allocations after the lock, the download applied and the routed port values observed;
- the host unit tests that build natively with MinGW GCC (41 programs on a `host-minimal`
  profile without the virtual bus and KNXnet/IP), before and after;
- the H8i8o firmware built against the result.

CI runs the full Linux suites, the interop tests and the gates on the pull request.

## 9. Status

### Integrated result, P1–P5, 21 November 2026

All five packages on one branch, measured together (size gate, LTO, Arm GNU Toolchain 13.2.Rel1;
every run: download applied, 15 of 15 routed values observed, zero allocations after the lock):

| | Before (`eb54dfa`) | After | Change |
|---|---:|---:|---:|
| `tp1-small` M3: KNstaX flash | 88 709 B | 86 651 B | −2 058 B |
| `tp1-small` M3: static RAM + heap | 1 608 + 11 724 = 13 332 B | 290 + 7 484 = **7 774 B** | **−5 558 B (−42 %)** |
| `tp1-small` M3: C/C++ runtime flash | 4 935 B | 2 678 B | −2 257 B (no printf) |
| `tp1-small` M3: device RAM headroom (H8i8o reservations) | 1 100 B | 6 658 B | |
| `tp1-standard` M4: KNstaX flash | 107 221 B | 104 761 B | −2 460 B |
| `tp1-standard` M4: static RAM + heap | 20 855 B | **12 361 B** | **−8 494 B (−41 %)** |
| `tp1-small` M0+: KNstaX flash / static RAM | 91 709 / 1 608 B | 89 581 / 290 B | |

`tp1-small` is now below the 10 240 B KNstaX RAM target that §10 of small-target-profile.md had
judged out of reach. The H8i8o firmware built against this branch: flash 97 404 → 93 360 B, static
RAM 5 720 → 4 392 B, product code unchanged.

Budgets lowered to the measured figures plus the gate's 2 % margin: flash and RAM of all three
reference devices. The C/C++ runtime budgets are left as they were: that figure depends on the
toolchain's C library (CI's Ubuntu newlib measured about 2.1 KiB more than the Arm toolchain used
here at the same commit), so lowering them from these figures would fail CI.

Native host tests (MinGW GCC 16, the virtual-bus-free profiles of §8.3): 42 programs on the minimal
profile, 63 on a full-capacity profile and 56 with the DPT families, all passing. The two
`test_knxprod_*` programs fail on Windows on `main` as well; two `needs-full-capacity` programs fail
on the small-capacity profile as on `main` and pass on the full-capacity one.

Not done in P1–P5, carried to P6: R5 (direct calls), R8 (pending actions; the queue semantics are
part of the product contract, so it waits for F2's smaller `DptValue`), R9, R10, F2, and the
transport receive queue that a device never fills (about 2 KiB on `full`).


### P3 Group tables (R2), 21 November 2026

Each loaded table now exists once (ARCHITECTURE.md, "The group tables and their load segments"):

- **Association table: the segment is the table.** `AssociationTableDomain` holds the entries in the
  form ETS writes them (count, then TSAP and ASAP big-endian) and reads them in place. The parsed
  `FixedVector<AssociationEntry, 160>` (644 B), the by-address index (324 B) and `DeviceModel`'s
  segment (642 B) are one 642-octet array. Lookups are a linear scan, O(n) per group telegram.
  What is in force is kept apart from what ETS is writing by the entry count, so the download
  behaves as before: writes straddling entries, the verification read before LoadCompleted, the
  table in force from LoadCompleted, Unload empties it.
- **Persistence without the scratch copy.** `saveToPersistence()` saves each record from where its
  value is: the association entries from their storage, the address table encoded into its own
  segment (owned by `AddressTableObject` now), strings and key tables from their storage, single
  property elements from the stack. `_recordScratch` keeps only the parameter image record and a
  carried-over element: 15 B instead of 642 B (1 666 B with Secure). The stored records are
  unchanged, so are the schema version, the banks and power-cut recovery.
- **Group object segment by the product.** One descriptor per registered group object, allocated in
  `DeviceModel::bind()`: 122 B for the reference device instead of 514 B. A device that registers
  no group object before `init()` keeps room for 256.
- **Not done: the address table's segment.** The data link layers and the TP-UART interrupt read the
  native entries, and a segment that decodes them on access needs a read/write hook on memory
  regions in the management server (P2's files). It stays as staging and save buffer (258 B).

Behaviour differences, none in ETS's sequence: a memory write into the association segment while
the table is Loaded changes the table at once (it used to wait for a LoadCompleted that ETS sends
only after StartLoading); an append by the firmware while the association table is Loading is
refused with `Busy` (it used to be accepted and lost at LoadCompleted); a segment read after a
restore or a firmware change returns the table in force (it used to return zeros or the last
download); an address table saved while it is Loading is stored empty also when the firmware
added entries to it during the download.

| | `tp1-small` M3 before | after | `tp1-standard` M4 before | after |
|---|---:|---:|---:|---:|
| Flash | 88 709 | 87 563 (−1 146) | 107 221 | 106 148 (−1 073) |
| Static RAM | 1 608 | 1 608 | 2 831 | 2 831 |
| Heap (start-up = peak, zero allocations after the lock) | 11 724 | 9 752 (−1 972) | 18 024 | 13 696 (−4 328) |
| KNstaX RAM | 13 332 | 11 360 | 20 855 | 16 527 |

`tp1-standard` gains more because its capacities are larger (255 addresses, 400 associations: the
association segment and the save scratch were 1 602 B each, the index about 800 B). The budgets are left to the integrator of P1–P4.

### P5 Product runtime (R3, R4), 21 November 2026

Done on `p5-product-runtime`, measured on its own against the base of the plan (P1–P4 not
included). Size gate, `tp1-small` Cortex-M3 and `tp1-standard` Cortex-M4; every step passed with
the download applied and no allocation after the heap lock.

| Step | M3 flash | M3 heap | M3 RAM | M4 flash | M4 RAM |
|---|---:|---:|---:|---:|---:|
| Base | 88 709 B | 11 724 B | 13 332 B | 107 221 B | 20 855 B |
| R3 two-word binding entries; state writes only for state ports | 88 875 B | 10 996 B | 12 604 B | 107 383 B | 20 127 B |
| Product definition held once | 88 823 B | 10 932 B | 12 540 B | 107 323 B | 20 063 B |
| R4 datapoint types from the port table (`GroupObject` 16 → 12 B) | 89 391 B | 10 716 B | 12 324 B | 107 969 B | 19 847 B |

Together −1 008 B of RAM on both profiles, for +682 B (M3) and +748 B (M4) of flash. H8i8o
firmware: flash 97 404 → 97 908 B, static RAM 5 720 B unchanged (the saving is heap).

- **R3.** A binding is `{ thunk, ctx }`: the thunk called without a value frees a heap callable, so
  the `manage` pointer went (12 → 8 B). State-write entries exist only for readable ports that take
  writes; they hold the first receive ranks, so no third rank is needed in the 14 B port
  descriptor. On the reference device the bindings fell from 1 424 to 688 B. *API:* `onStateWrite`
  on a command port (no state) is now a `static_assert`, like the other bindings a port cannot use.
  No product (the examples, the gate device, H8i8o) did that; one unit test did and now uses
  `onCommand`. Compile-time dispatch was not done: bindings are registered at run time with
  callables whose captures are run-time values, so the storage cannot come from the type alone.
- **Definition.** `CommissionedProductRuntime` no longer copies the definition; `definition()`
  returns by value, rebuilt from the endpoint runtime's copy.
- **R4.** The datapoint type moved out of the object: `GroupObjectStore::setStaticDpts()` reads it
  from the port table in rodata for the objects a product registers; any other object keeps its type
  in the store, so tables built by tests, tools or `addGroupObject` behave as before. The value
  offset and length, the flags (ETS writes them through `PID_TABLE`), the address and the policy
  index stay. Going to 8 B would need the store pointer or the address out of the object, which
  belongs with P6's static device. The flash cost (about 570 B) is the type side table and its
  lookup.
- **R8 not done.** The pending-action queue is a contract: the guide promises that requests made
  before start are queued and sent, and the tests check distinct entries for the same port in order
  and `QueueFull` at capacity, so a dirty bitset would coalesce what is promised as a queue. The
  record is 40 B because `DptValue` is 32 B; F2 (`DptValue` as a tagged plain struct, P6) shrinks
  it without a second representation here.
- **Not done, noted.** `ResolvedKnxPersistenceIdentity` keeps its two `std::string`s: they are
  public (`knxPersistenceIdentity().storageNamespace.c_str()` in the tests), so they go with R10 in
  P6. The two 40 B product callbacks (programming mode, fault) keep their 32 B capture, which is the
  product's API.
