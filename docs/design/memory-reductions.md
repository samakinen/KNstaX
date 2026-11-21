# Design: further RAM and flash reductions for `tp1-small`

| | |
|---|---|
| Status | **Proposed**, 21 November 2026. Attribution measured; savings estimated unless marked. |
| Goal | Room for Data Secure ([data-secure-small-target.md](data-secure-small-target.md)) and for products on 20 KiB parts, and the `tp1-small` flash budget met again. |
| Builds on | [small-target-profile.md](small-target-profile.md) §10, whose heap attribution of 16 November this replaces. |

## 1. Starting point (measured)

Size gate, `tp1-small` reference device (60 group objects), Cortex-M3, LTO, at `673a005`:

| | Measured | Budget |
|---|---:|---:|
| KNstaX flash | 88 613 B | 88 064 B (**over by 549 B**; CI on Ubuntu: 88 562 B) |
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
| R6 | Frame pool 16 → 2 (peak use to be measured first); one frame ring shared by MAC, data link and transport | 520–750 | small | `tp1_data_link_layer.*`, `tp1_mac_physical.*`, `transport_layer.hpp` |
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
  allocated object whose size the linker reports: no heap, and the RAM figure is known at link
  time (the `arena` policy of small-target-profile.md §6.4, reached by removing allocation rather
  than by replacing the allocator).
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
