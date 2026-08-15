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

- `makeCommissionedProduct(...)` — wraps an endpoint definition with an optional
  parameter schema into one compiled object.
- `makeCommissionedBindings(...)` — a fluent builder for all firmware callbacks:
  `onCommand`, `onStateWrite`, `provideState`, `onParameterChanged`,
  `onProgrammingModeChanged`, `onLifecycleChanged`, `onFault`.

  Callback slots default to `std::function` (heap-backed). Pass an explicit
  capacity to use `InplaceFunction<…, N>` (fixed-size, allocation-free):

  ```cpp
  makeCommissionedBindings<64>(kMyProduct)   // 64-byte bounded slots
      .onCommand<Port::Relay>([](bool v) { gpio_set(PIN, v); })
      .provideState<Port::Relay>([]() -> bool { return gpio_read(PIN); });
  ```

  A compile-time error is emitted if any lambda captures more than N bytes.

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

**ETS parameter delivery**: parameter data written by ETS via the KNX management
model (`A_PropertyValue_Write` → `PID_PROGRAM_DATA`) is decoded by
`ApplicationProgramObject`, forwarded through `wireParameterDataCallback`, decoded
by `ParameterState::applyFromBytes`, and delivered to `onParameterChanged`
handlers. The `expert::applyParameterDataBytes` helper exercises the same path
in tests and demos.

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

**3. Add a parameter schema if needed (optional).**

```cpp
enum class SwitchParam : uint16_t {
    DefaultState = 0,
};

constexpr auto kSwitchProduct = makeCommissionedProduct(
    makeEndpointDefinition<...>(...),
    makeParameterSchema(
        parameter<SwitchParam::DefaultState>("default_state", false)));
```

Parameter data is delivered by ETS via the KNX management model and fires
`onParameterChanged` handlers. Parameters can also be applied programmatically
via `applyCommissionedParameter(...)` for testing or startup defaults.

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
makeCommissionedBindings<64>(kMyProduct)
```

Each callback slot uses `InplaceFunction<Signature, 64>` instead of
`std::function`. The compiler emits a `static_assert` failure if any lambda
captures more than 64 bytes.

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
