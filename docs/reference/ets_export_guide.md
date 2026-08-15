# ETS export workflow

How a `constexpr` product definition becomes a `.knxprod` ETS can import.

## The pipeline

```
product.hpp  (constexpr ProductDefinition — the single source of truth)
     │
     │  knx_commissioned_product() CMake function generates a host-side
     │  exporter binary from cmake/knxprod_gen_main.cpp.in
     ▼
<product>_ets.json           exportDescriptorToJson()
     │
     │  tools/knxprod_exporter/exporter.py
     ▼
<product>_ets.knxprod.xml    KNX project schema v23, XSD-validated in CI
```

The point of the design is that the runtime and the ETS catalogue entry are
derived from the *same* declaration. There is no second place to update when a
port is added, and no way for the two to drift.

## Declaring a product

```cpp
#include "knx/product/commissioned_product.hpp"

namespace my_product {

enum class Port : uint16_t {
    Setpoint = 0,
    Temperature = 1,
};

inline constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port,
                           semantics::TemperatureStateInOut<Port::Setpoint, "setpoint", "Setpoint">,
                           semantics::TemperatureState<Port::Temperature, "temperature", "Temperature">>(
        ProductIdentity{
            .productKey         = "my_product",
            .productDisplayName = "My Product",
            .manufacturerId     = ManufacturerId(0x00FA),
            .medium             = endpoint::Medium::TP1,
            .applicationNumber  = 1,
            .applicationVersion = 1,
            .firmwareRevision   = 1,
            .maxApduLength      = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "my_product",
            .schemaVersion   = 1,
            .persistKnxState = true,
        }));

} // namespace my_product
```

Keep this header free of hardware, platform and OS dependencies: it is compiled
both by the firmware and by the host exporter.

## Parameters

Parameters are declared in a schema attached to the product and are downloaded
by ETS into a memory-mapped block, laid out in declaration order.

```cpp
constexpr auto kParameters = makeParameterSchema(
    ParameterDescriptor<Param::HvacMode, uint16_t>{
        .key = "default_hvac_mode",
        .displayName = "Default HVAC operating mode",
        .defaultValue = 1,
        .options = parameterOptions({0, "Auto"},
                                    {1, "Comfort"},
                                    {2, "Standby"},
                                    {3, "Economy"},
                                    {4, "Building Protection"}),
    },
    ParameterDescriptor<Param::Hysteresis, Dpt9Float>{
        .key = "thermostat_hysteresis",
        .displayName = "Thermostat hysteresis",
        .defaultValue = 1.0f,
        .minValue = 0.1,
        .maxValue = 5.0,
        .unit = "K",
    });
```

### `options` is the difference between a usable product and a table lookup

Without `options`, an enumerated parameter exports as a bare number field and
the integrator has to be told out of band that `2` means Standby — which is how
products end up with labels like `"Mode (0=Auto,1=Comfort,2=Standby)"`. With
`options`, the exporter emits a `<TypeRestriction>` with `<Enumeration>`
children and ETS renders a drop-down.

Declare options on every enumerated parameter. It costs one line and it is the
part of the product an integrator actually touches.

### `group`, and conditional visibility

```cpp
ParameterDescriptor<Param::CoolingEnabled, uint16_t>{
    .key = "cooling_enabled", .displayName = "Cooling enabled", .defaultValue = 1,
    .options = parameterOptions({0, "Off"}, {1, "On"}),
    .group = "Cooling",
},
ParameterDescriptor<Param::CoolingGain, Dpt9Float>{
    .key = "cooling_kp", .displayName = "Cooling gain", .defaultValue = 25.0f,
    .unit = "%/K",
    .group = "Cooling",
    // Hidden while cooling is switched off.
    .visibleWhenParameterId = static_cast<uint16_t>(Param::CoolingEnabled),
    .visibleWhenValue = 1,
},
```

`group` puts the parameter in a named ETS section; parameters sharing a group
render together, in declaration order. Products that declare no groups get a
single block exactly as before.

`visibleWhenParameterId` / `visibleWhenValue` emit `<choose>/<when>`, so ETS
hides the parameter unless the controlling parameter holds that value. Showing
settings that currently do nothing is a standing source of misconfiguration —
an integrator who tunes a cooling PID on a heating-only unit has no way to know
it had no effect.

### `minValue` / `maxValue` / `unit`

Bounds narrow the generated `ParameterType`, so ETS rejects out-of-range input
at configuration time rather than letting the device receive a value it has to
defend against. `unit` becomes the field's suffix text.

Bounds are only applied when they differ — an all-zero pair means "unset" and
falls back to the value type's full range.

### Value kinds and memory width

| Kind | Width | Notes |
|---|---|---|
| `bool` | 1 octet | |
| `uint8_t` | 1 octet | |
| `uint16_t` / enum | 2 octets | |
| `int16_t` | 2 octets | |
| `Dpt9Float` | 2 octets | KNX-native half-float; **prefer this for fractional values** |
| `float` | 4 octets | IEEE-754. Third-party tooling handles this poorly — see below. |

`Dpt9Float` is preferred over `float` because knxprod `TypeFloat` support is
patchy outside ETS itself: Kaenx-Creator ships no float validation, mis-imports
the size as 16-bit, and parses the default value with the system locale. Keep
exported default values integer-valued (`22`, not `22.0`) so they parse
identically under every locale.

## Communication object flags

Flags come from the port helper's direction and are exported so that what ETS
shows is what the firmware enforces:

| knxprod attribute | Source |
|---|---|
| `CommunicationFlag` | Always Enabled for a declared port |
| `ReadFlag` | Port is readable |
| `WriteFlag` | Port is writable |
| `TransmitFlag` | Port transmits |
| `UpdateFlag` | Port accepts `A_GroupValue_Response` (Response-Update enable) |
| `ReadOnInitFlag` | Port declares `ReadOnInit` |

`ReadOnInitFlag` defaults to Disabled. Enable it deliberately: the KNX spec warns
it multiplies bus load after a whole-installation restart.

## Building

```bash
cmake --build build --target <product>_ets_knxprod
```

The generated XML lands in the build directory. CI validates it against the KNX
XSD (`test_knxprod_xsd_validation`) and round-trips it through the exporter test
(`test_knxprod_exporter`).

## Importing into ETS

The generated file is a `.knxprod.xml`, not a zipped `.knxprod`. Import it
through Kaenx-Creator (which produces the packed archive ETS consumes) or a
comparable tool.

## The load procedure

The exporter emits the standard System B application download:

```xml
<LoadProcedure>
  <LdCtrlConnect/>
  <LdCtrlUnload LsmIdx="4"/>
  <LdCtrlRelSegment LsmIdx="4" Size="34" Mode="0" Fill="0"/>
  <LdCtrlWriteRelMem ObjIdx="4" Offset="0" Size="34" Verify="1"/>
  <LdCtrlLoadCompleted LsmIdx="4"/>
  <LdCtrlDisconnect/>
</LoadProcedure>
```

`LsmIdx=4` is the Application Program interface object as this stack registers
them (0 Device, 1 Address Table, 2 Association Table, 3 Group Object Table,
4 Application Program). **If that registration order changes, this index must
change with it.**

The `Unload` step is not decoration: the load state machine only accepts a
segment allocation from the Unloaded state, so without it a second download of
an already-commissioned device fails. `Size` is the exact parameter block the
device decodes, and `Verify="1"` uses the read-back the device already sends in
response to `A_Memory_Write`.

A product with no serialisable parameters omits the allocate and write steps
rather than allocating a zero-length segment.

## Current limitations

Honest list of what the exporter does not yet produce:

- **English only.** A single `<Language Identifier="en-US">` block; no
  translations. A product sold outside one language market needs them.
- **One channel.** All parameter blocks and communication objects live under a
  single `<Channel>`; a multi-channel product (four independent dimmer
  outputs, say) would want one channel per instance.
- **Visibility conditions are equality-only.** `<choose>/<when>` supports
  ranges and multiple tests; only `parameter == value` is exposed.
- **The manufacturer name is taken from the product display name.** Fine for
  development against manufacturer id 0x00FA; a real product needs its own
  allocated manufacturer id and name.
