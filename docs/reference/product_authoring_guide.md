# KNstaX Product Authoring Guide

This guide describes how to define, build, and start a KNX endpoint product in KNstaX.

For normal KNX devices, KNstaX has one canonical authoring path:
`startCommissionedProduct(...)` from `include/knx/product/commissioned_product.hpp`.
Other lower-level surfaces are advanced-only and are not the recommended path
for ETS-commissioned product firmware.

---

## Architecture Overview

The product layer has two tiers.

**Tier 1 — Product-definition layer**

Owns the typed model of ports, DPTs, keys, persistence intent, and export
metadata. This is the single source of truth for what a product is. Start here
for every new product.

Headers:
- `include/knx/product/endpoint_definition.hpp`
- `include/knx/product/endpoint_semantics.hpp`
- `include/knx/product/endpoint_export.hpp`
- Umbrella: `include/knx/product/endpoint.hpp`

**Tier 2 — Runtime layer**

Assembles the KNX stack from a product definition and a transport, wires ETS
commissioning, parameter delivery, and persistence, and returns a running
`CommissionedProductRuntime`. This is the intended authoring surface for all
new products.

Headers:
- `include/knx/product/commissioned_product.hpp`
- `include/knx/product/commissioned_product_expert.hpp` (test and demo use only)

---

## What Each Tier Gives You

### Product-definition layer

- Typed logical port IDs checked at compile time.
- Unique logical ID enforcement via `static_assert`.
- DPT, direction, readability, writability, transmit behavior, and persistence
  expressed once in the definition.
- Display name and key captured alongside type — the same definition drives both
  the embedded runtime and the ETS catalog exporter.
- Composable semantic helpers in `endpoint_semantics.hpp` for common signal
  types (switch, percent, temperature, HVAC mode, RGB, and others).
- `makeEndpointDefinition(...)` plus `makeEndpointExportDescriptor(...)` give the
  natural path to ETS catalogue generation.

See `examples/esp_idf_switch/main/product.hpp` for the intended shape: one
`constexpr` definition shared by both the firmware binary and the export build
target. That file is not an illustration — it is the definition the
`tp1_switch_ets_knxprod` target actually exports from.

### Runtime layer

`commissioned_product.hpp` provides:

- `makeCommissionedProduct(...)` — wraps an endpoint definition into one compiled
  object; `makeCommissionedProduct<kParameters>(...)` also names the product's
  parameter layout.
- `makeCommissionedBindings(...)` — a fluent builder for all firmware callbacks:
  `onCommand`, `onStateWrite`, `provideState`, `onParametersChanged`,
  `onProgrammingModeChanged`, `onLifecycleChanged`, `onFault`.

  A binding is stored as a two-word entry, and only for the ports that can
  use it: command handlers for ports that accept bus writes, state-write
  handlers for those that also have a state (readable ports that accept
  writes, such as a state in/out port), state providers for readable ports.
  Registering a handler a port cannot use (`onCommand` on a read-only state
  port, `onStateWrite` on a command port, `provideState` on a port that is not
  readable) is a compile-time error.

  A callable that is trivially copyable and no larger than a pointer (a
  captureless lambda, a function pointer, a lambda capturing one pointer,
  reference or integer) is stored inside the entry and costs no memory beyond
  it. A larger or non-trivial capture (a `std::string`, a `std::shared_ptr`,
  several references) is allocated on the heap, which is the default. Pass a
  non-zero capacity to forbid the heap altogether; the number is then the
  maximum capture size in bytes:

  ```cpp
  makeCommissionedBindings<8>(kMyProduct)   // no heap; captures of at most 8 bytes
      .onCommand<Port::Relay>([](bool v) { gpio_set(PIN, v); })
      .provideState<Port::Relay>([]() -> bool { return gpio_read(PIN); });
  ```

  A compile-time error is emitted for any callable that does not fit inline or
  is larger than N bytes. The inline limit is `sizeof(void*)`, so 4 bytes on a
  Cortex-M: capture a pointer to the state the handler needs rather than the
  state itself. The bindings are move-only.

- `CommissionedProductRuntime::compiledEndpoint()` (and
  `EndpointRuntime::compiledDefinition()`) build the compiled definition, the
  runtime and export descriptors with the port keys and display names, on demand
  and return it **by value**. The runtime does not keep a copy; the port table it
  works from is read-only data and carries no strings. Bind the result with
  `const auto compiled = app.compiledEndpoint();` (or `const auto&`, which keeps
  the temporary alive) and use it for diagnostics, such as logging the ports a
  project left unlinked. `EndpointRuntime::identity()` returns the product
  identity without building anything.

  Publishes and read responses requested before the stack has started are queued
  and sent when it starts. The queue is twice the port count by default; the
  `KNX_PENDING_ACTION_CAPACITY` option (Kconfig, menu Capacities) sets a smaller
  fixed size, 8 in the `tp1-small` profile. A request made with the queue full
  fails with `QueueFull`.

  The communication objects are one table, allocated once at start from the port
  table: 16 bytes per port on a 32-bit target plus the datapoint's value bytes.
  The product API does not change. In a profile that sets
  `KNX_CALLBACK_CAPTURE_BYTES` (8 in `tp1-small` and `tp1-standard`), the stack's
  own callbacks keep only that many bytes of capture; the bindings you register
  are not affected (`BindingCapacity` sizes those).

- `startCommissionedProduct(...)` — assembles the stack, wires persistence and
  management, and returns a `CommissionedProductRuntime`.
- `DeviceLifecycleState` — a three-state enum (`Uncommissioned`, `Commissioning`,
  `Operational`) that removes the need for manual individual-address arithmetic
  in firmware loops.
- KNX-state persistence namespace derived automatically from `ProductIdentity`
  and `PersistencePolicy`, with an override path in `CommissionedProductOptions`.

**CO-table size** is fixed at compile time by `kPortCount` — the number of ports
in the endpoint definition. There is no API to add communication objects beyond
the definition, so table overflow is impossible by construction.
`CommissionedProductRuntime::kPortCount` and `registeredGroupObjectCount()` expose
this as a verifiable invariant.

**ETS parameter delivery**: the parameters are one image, a byte array of
exactly the layout's size that the runtime owns. ETS writes into it with
`A_Memory_Write`; at `LoadCompleted` the stack checks every value against its
declared bounds, stores the image with the layout's fingerprint, and calls
`onParametersChanged` with a view over the new values. `parameters()` gives the
same view at any time; reading a parameter is a load at a constant offset.
`expert::loadParameterImage` takes the same path in tests and demos.

---

## Authoring A New Product — Step by Step

**1. Define a logical port enum.**

Use a `uint16_t` underlying type. Values are logical identifiers, not runtime
slot indices; they do not need to be sequential.

```cpp
enum class SwitchPort : uint16_t {
    RelayCommand = 0,
    RelayState   = 1,
};
```

**2. Build the endpoint definition.**

Use semantic helpers from `endpoint_semantics.hpp` where they fit. Use raw
`CommandPort`, `StatePort`, or `StateInOutPort` for types not covered by helpers.

```cpp
constexpr auto kSwitchProduct = makeCommissionedProduct(
    makeEndpointDefinition<
        SwitchPort,
        semantics::SwitchCommand<SwitchPort::RelayCommand, "relay_command", "Relay Command">,
        semantics::SwitchState  <SwitchPort::RelayState,   "relay_state",   "Relay State">>(
        ProductIdentity{
            .productKey         = "my_switch",
            .productDisplayName = "My Switch",
            .manufacturerId     = ManufacturerId(0x00FA),
            .medium             = endpoint::Medium::TP1,
            .applicationNumber  = 1,
            .applicationVersion = 1,
            .firmwareRevision   = 1,
            .maxApduLength      = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "my_switch",
            .schemaVersion   = 1,
            .persistKnxState = true,
        }));
```

Put this `constexpr` in its own header (e.g. `product.hpp`) with no hardware
or platform dependencies so the same object can be used by the firmware binary
and the ETS export build target.

**3. Add parameters if needed (optional).**

```cpp
enum class SwitchParam : uint8_t { DefaultState, OffDelayS };

inline constexpr auto kSwitchParameters = params::parameterLayout(params::parameterBlock<SwitchParam>(
    {.key = "switch", .name = "Switch"},
    params::param<SwitchParam::DefaultState, bool>({.name = "State after start"}),
    params::param<SwitchParam::OffDelayS, uint16_t>(
        {.name = "Switch-off delay", .def = 0, .min = 0, .max = 3600, .unit = "s"})));

constexpr auto kSwitchProduct = makeCommissionedProduct<kSwitchParameters>(
    makeEndpointDefinition<...>(...));
```

The firmware reads them through a view, once after start and again when ETS
has downloaded new ones:

```cpp
bindings.onParametersChanged([](params::ViewFor<kSwitchParameters> p) {
    g_offDelayS = p.block<SwitchParam>().get<SwitchParam::OffDelayS>();
});
// ... after startCommissionedProduct():
g_offDelayS = app->parameters().block<SwitchParam>().get<SwitchParam::OffDelayS>();
```

A product with several identical channels declares the channel's block once
and repeats it: `params::repeat<8>(kInputBlock)`, read with
`parameters().block<In>(channel)`. `setParameter<Id>(value)` changes a
parameter from the device itself. The [ETS export guide](ets_export_guide.md)
describes options, conditions and sections.

**4. Build the bindings and start the product.**

```cpp
auto result = startCommissionedProduct(
    platform,
    kSwitchProduct,
    makeCommissionedBindings(kSwitchProduct)
        .onCommand<SwitchPort::RelayCommand>([&](bool on) {
            relayOn = on;
            relayDirty = true;
        })
        .provideState<SwitchPort::RelayState>([&]() {
            return relayOn;
        })
        .onLifecycleChanged([](DeviceLifecycleState s) {
            // drive a status LED or indicator here
        })
        .onFault([](FaultInfo f) {
            // log or signal fault
        }),
    std::unique_ptr<physical::Tp1MediumBackend>(new physical::NullTp1MediumBackend()));

if (result.isError()) return 1;

// A CommissionedProductHandle<> — a unique_ptr owning the runtime. The runtime
// carries the endpoint and parameter state inline and is far too large to
// return by value through an MCU task's stack frame, so it is built directly
// on the heap. The product model stays entirely compile-time either way.
auto app = std::move(result.value());
```

The last argument to `startCommissionedProduct` accepts any of:
- `std::unique_ptr<physical::Tp1MediumBackend>` — TP1 medium backend
- `std::unique_ptr<physical::Tp1MacPhysical>` — TP1 MAC physical
- `std::unique_ptr<physical::IpTunnelingPhysical>` — KNXnet/IP tunneling
- `std::unique_ptr<physical::IpRoutingPhysical>` — KNXnet/IP routing

**5. Drive the runtime and publish state.**

```cpp
for (;;) {
    app->loop();

    if (relayDirty && app->lifecycleState() == DeviceLifecycleState::Operational) {
        relayDirty = false;
        (void)app->publish<SwitchPort::RelayState>(relayOn);
    }

    platform.delay(5);
    // if (button_pressed()) app->toggleProgrammingMode();
}
```

**6. Keep commissioning data out of normal firmware paths.**

Group addresses are written by ETS and restored automatically from persistence.
Firmware should not contain hardcoded group address bindings in the normal start
path. Only put `applyCommissionedGroupAddress(...)` calls in explicit smoke-test
or demo helpers, clearly labeled as non-production.

---

## Transport Options

`startCommissionedProduct(...)` is transport-neutral. Pass the appropriate
transport object as the last argument:

| Transport | When to use |
|---|---|
| `Tp1MediumBackend` | Most embedded TP1 products |
| `Tp1MacPhysical` | TP1 with direct MAC access |
| `IpTunnelingPhysical` | KNXnet/IP tunneling (e.g. ETS-over-IP connection) |
| `IpRoutingPhysical` | KNXnet/IP routing (IP backbone) |

---

## Bounded Storage for Embedded Targets

On constrained embedded targets where heap allocation must be avoided, pass a
capacity to `makeCommissionedBindings`:

```cpp
makeCommissionedBindings<8>(kMyProduct)
```

The number is the maximum capture size in bytes of a binding, and with it the
bindings never allocate. A callable has to be trivially copyable and fit in a
pointer (`sizeof(void*)`, 4 bytes on a Cortex-M); the compiler emits a
`static_assert` failure for any other. With the default capacity of 0 the same
callables are stored inline and larger ones go to the heap.

The CO-table size is bounded at compile time by the number of ports in the
definition — it is impossible to register more communication objects than the
definition declares.

---

## Choosing Port Types

| Signal | Helper | Raw type |
|---|---|---|
| Boolean command (switch on/off) | `semantics::SwitchCommand<>` | `CommandPort<..., dpt::Switch>` |
| Boolean state (readable) | `semantics::SwitchState<>` | `StatePort<..., dpt::Switch>` |
| Percentage (0–100%) | `semantics::PercentCommand<>` / `semantics::PercentState<>` | — |
| Temperature (DPT 9.001) | `semantics::TemperatureSensor<>` | — |
| HVAC mode | `semantics::HvacMode<>` | — |
| RGB color | `semantics::RgbColor<>` | — |
| Bidirectional state | — | `StateInOutPort<..., Dpt>` |

---

## In-Repo References

**Working examples:**

- `examples/esp_idf_switch/` — `main/product.hpp` + `main/main.cpp`, the
  canonical shape on real hardware
- `examples/esp_idf_temperature_sensor/`, `examples/esp_idf_low_power_sensor/`
- `examples/tp1_thermostat/main.cpp` — the richest parameter schema in the tree
- `examples/tp1_line_coupler/` — the same authoring path with `CouplerOptions`

See [`../../examples/README.md`](../../examples/README.md) for what each one is for.

**Unit tests:**

- `test/unit/test_product_commissioned_surface.cpp`
- `test/unit/test_product_endpoint_public_surface.cpp`
- `test/unit/test_product_endpoint_semantics.cpp`
- `test/unit/test_product_endpoint_policies.cpp`

**Conformance evidence:**

- `test/conformance/test_conformance_evidence_commissioned.cpp` — CE-2cp through CE-11cp

**DPT reference:**

- `include/knx/application/dpt_catalog.inc` — the catalogue itself, one line per
  sub-type, and the source both the runtime codecs and the exporter read
- `include/knx/product/endpoint_semantics.hpp` — the named helpers, which pick
  the KNX-standard sub-type for each signal so you do not have to

## Port modifiers

Two KNX communication flags are per-object policy that only a minority of ports
want, so they are not parameters on every semantic alias. Reach them with the
transformers instead:

```cpp
// Refresh this object from the bus after a reset (KNX flag I).
semantics::ReadOnInit<semantics::TemperatureStateInOut<Port::Setpoint, "setpoint">>

// Send this object's telegrams at raised priority.
semantics::WithPriority<semantics::AlarmState<Port::Alarm, "alarm">, Priority::Urgent>

// They compose.
semantics::WithPriority<
    semantics::ReadOnInit<semantics::SwitchStateInOut<Port::Mode, "mode">>,
    Priority::Urgent>
```

Both carry through to the runtime *and* to the generated `.knxprod`, so what
ETS shows is what the device does.

Use them sparingly, in both cases for the same reason — they cost bus capacity
that every other device on the line shares:

- **`ReadOnInit`** makes the device issue `A_GroupValue_Read` after reset.
  KNX 03/05/01 §4.12.5.2.4.1.3 explicitly warns against enabling it by default,
  because a whole-installation power-up turns into a read storm. It earns its
  keep on values the device cannot derive locally — a setpoint or operating mode
  owned by another device — not on sensor outputs the device produces itself.
- **`WithPriority`** raises a port above the `Priority::Low` default that is
  correct for routine sensor traffic. A device that sends everything at Urgent
  degrades the line for everyone.

## Group object memory

Each communication object costs about 288 bytes of heap, dominated by two
payload buffers of `config::MAX_GROUP_OBJECT_PAYLOAD_BYTES` (16 by default —
one for the current value, one for the last transmitted value used by
send-on-change).

16 octets covers every datapoint type in the catalog; the widest is DPT 16 at
14. A build-time assertion ties the bound to the catalog, so adding a wider DPT
fails the build rather than failing at runtime.

Raise `CONFIG_KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES` only for a product that uses
group objects as opaque transport for larger blobs, and budget for it: the cost
is paid twice per object, so a 40-object device pays 80× whatever you add.
Oversized payloads are rejected with `BufferTooSmall`, never truncated.

## Heap policy: lock the heap after start-up

In a profile with `KNX_HEAP_POLICY=init_only` (`tp1-small`, `tp1-standard`) the
stack allocates while it starts and not afterwards. Call `knx::heap::lock()`
(`knx/util/heap.hpp`) once your own start-up is over, after
`startCommissionedProduct()` returns and your product has built what it needs.
From then on an allocation through `operator new` is a defect: it traps in a
debug build, and in a release build it fails as an exhausted heap does
(`operator new` aborts, `new (std::nothrow)` returns null). Your own code is
included: the lock covers every allocation in the image, not only the stack's.

- Persistence is part of this. A `Persistence` backend should write to its own
  storage (flash) and not allocate per save; the stack's save buffer is
  reserved when persistence starts.
- Register `knx::heap::setHighWaterHook()` with a function that returns your
  allocator's peak (on newlib, the highest `_sbrk` break minus the heap start);
  `knx::heap::highWater()` then reports it. `_sbrk` is yours, so the stack
  cannot read it.
- `knx::heap::setViolationHandler()` lets a test run on and list every
  allocation after the lock (its size and caller) instead of trapping. The size
  gate (`test/baremetal/size_gate`) does this.
- In a `free` profile (the default on hosts and ESP-IDF) `lock()` only records
  the state; calling it is harmless.
- Capacities that are promises for the whole run are reserved when the stack
  starts: the transport connection entries (one on an end device), the group
  objects and their value pool, the persistence save buffer. A host-side table
  that grows after start (`BusAccessUnit::addGroupObject`, an ETS-driven table
  beyond what was reserved) allocates and so belongs before `lock()`.

## Persistence on MCUs without NVS: `FlashLog`

ESP-IDF stores commissioned state in NVS and hosts in files. A bare-metal build
has no default and stores through what the product registers with
`objects::setPersistenceFactory()`. For internal flash that is
`objects::FlashLog` (`knx/objects/flash_log.hpp`) behind
`objects::FlashLogPersistence`.

The board describes a region of its flash and implements erase, program and
read for it, in C (`knx_flash_hal_t`, `knx/objects/flash_hal.h`):

```cpp
static knx::objects::FlashLog g_log(board::flashHal());  // two banks, e.g. 2 × 4 KiB

int main() {
    knx::objects::setPersistenceFactory([]() -> std::unique_ptr<knx::objects::ObjectPersistence> {
        return std::make_unique<knx::objects::FlashLogPersistence>(g_log);
    });
    auto app = std::move(knx::product::startCommissionedProduct(/* ... */).value());
    for (;;) {
        app->loop();
        (void)g_log.service();  // erases one page, or compacts, when there is work
    }
}
```

What to know:

- **The region** is reserved for the stack: an even number of pages, split into
  two banks. Keep it out of the linker script's flash.
- **Saves never erase.** They program into space erased beforehand, so a save
  takes microseconds per write unit and can run on a power-fail warning.
  Erasing is `service()`'s job, one page per call, so the CPU stalls on an
  erase only when the main loop chooses. On most parts an erase stops code
  running from flash for milliseconds; put interrupt code that must keep
  running in RAM.
- **Power cuts.** A record cut short is ignored, so each id keeps either its
  old or its new value. The stack's own bank switch on top makes a whole ETS
  download atomic: the next boot has the previous download or the new one,
  never a mixture.
- **Size the banks** for the live state twice over plus a reserve: a full save
  writes every record again before dropping the old ones. A save that needs a
  compaction while the spare bank still waits for its erase fails with
  `StorageFull`, so call `service()` from every loop iteration.
  `statistics().freeBytes` shows the room left after a download.
- **String keys** (`save(std::string, ...)`) are not stored and return
  `OperationNotSupported`. The stack uses the numeric ids only; the string half
  serves the KNXnet/IP multicast timer, which a TP1 device does not have.
- On parts with ECC flash (STM32G0, L4), the HAL's `read` must report an ECC
  error as a failed read instead of faulting.

## Persistence schema version — bump it when the layout changes

```cpp
PersistencePolicy{
    .namespacePrefix = "my_product",
    .schemaVersion   = 1,     // ← increment on any port/parameter change
    .persistKnxState = true,
}
```

Commissioned state is persisted as a **positional byte layout**: the parameter
block is decoded in declaration order, and interface-object blobs are written
back into properties without a length negotiation. A firmware update that adds,
removes or reorders a port or a parameter therefore cannot safely read what the
previous firmware wrote — the bytes land in the wrong fields.

`schemaVersion` is stored alongside the data and compared on every boot. When it
differs, the whole namespace is discarded and the device comes up
uncommissioned, so ETS re-downloads. That is deliberately blunt: a device that
silently comes up mis-parameterised is far worse than one that asks to be
downloaded again.

**Rules of thumb**

- Adding, removing or reordering a **parameter** → bump.
- Adding, removing or reordering a **port** → bump.
- Changing a parameter's *value kind* (and therefore its byte width) → bump.
- Changing only a display name, a group, or a default value → no bump needed;
  the layout is unchanged.

Forgetting to bump is the dangerous direction, and it fails silently. If in
doubt, bump — the cost is one ETS download.

## Reaching the BAU directly

Product firmware should not need to. If you do, note that the advanced
`BusAccessUnit` surface is grouped into facets rather than flattened:

| Facet | Covers |
|---|---|
| `bau.transmission()` | Retry policy, send outcomes, automatic read responses, drop counters |
| `bau.management()` | Programming mode, coupler role, optional interface objects |
| `bau.link()` | Raw frame injection and promiscuous mode — bypasses filtering and the group-object runtime entirely; for sniffers, bridges and tests |

Each is a non-owning view holding one reference, so `bau.transmission().poll()`
costs what the direct call did. They must not outlive the BAU.
