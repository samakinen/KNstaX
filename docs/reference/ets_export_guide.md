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

There are two ways to declare parameters while the new model is being
introduced ([parameter-model.md](../design/parameter-model.md)):

- **A parameter layout** (`makeCommissionedProduct<kParameters>(...)`): blocks
  that can repeat, almost no cost on the device. New products use this. See
  "Parameter layouts" below.
- **A parameter schema** (`makeParameterSchema(...)`): the older flat list,
  described in the rest of this section. It will be removed once the products
  that use it have moved.

A product uses one of the two; the exporter refuses a file that carries both.

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

## Parameter layouts

```cpp
#include "knx/product/commissioned_product.hpp"

enum class Function : uint8_t { Disabled, Switch, Dimming };
enum class Action : uint8_t { Nothing, On, Off, Toggle };

// An option list is a named constant. Parameters that share it share one ETS
// ParameterType.
inline constexpr auto kActionOptions = params::options<Action>(
    {{Action::Nothing, "Nothing"}, {Action::On, "On"}, {Action::Off, "Off"}, {Action::Toggle, "Toggle"}});

enum class Dev : uint8_t { StartupDelayS, Expert };
enum class In : uint8_t { Function, OnClosing, OnOpening, LongPressMs };

inline constexpr auto kDeviceBlock = params::parameterBlock<Dev>(
    {.key = "device", .name = "Device"},
    params::param<Dev::StartupDelayS, uint8_t>({.name = "Start-up delay", .def = 2, .min = 0, .max = 60, .unit = "s"}),
    params::param<Dev::Expert, bool>({.name = "Show the diagnostics page"}));

inline constexpr auto kInputBlock = params::parameterBlock<In>(
    {.key = "input", .name = "Input {n}"},
    params::param<In::Function, Function>({.name = "Function", .def = Function::Switch, .options = kFunctionOptions}),
    params::param<In::OnClosing, Action>({.name = "On closing", .def = Action::Toggle, .options = kActionOptions,
                                          .visible = params::when<In::Function>(Function::Switch)}),
    params::param<In::OnOpening, Action>({.name = "On opening", .options = kActionOptions,
                                          .visible = params::when<In::Function>(Function::Switch)}),
    params::param<In::LongPressMs, uint16_t>({.name = "Long press after", .def = 500, .min = 300, .max = 3000,
                                              .unit = "ms",
                                              .visible = params::whenNot<In::Function>(Function::Disabled)}));

inline constexpr auto kParameters = params::parameterLayout(kDeviceBlock, params::repeat<8>(kInputBlock));

inline constexpr auto kProduct = makeCommissionedProduct<kParameters>(makeEndpointDefinition<...>(...));
```

What ETS shows:

- **One page per block instance.** `repeat<8>` gives "Input 1" to "Input 8";
  `{n}` in the block name is the instance, counted from 1. A name without
  `{n}` gets the number appended.
- **A drop-down** for a parameter with `options`, **a check box** for a `bool`
  without options, a number field with the unit as suffix otherwise.
- **Conditions.** A parameter or a whole block is shown only while a
  controlling parameter holds certain values:

  | Declaration | Shown while the controlling parameter |
  |---|---|
  | `when<Id>(value)` | equals the value |
  | `whenAny<Id>(a, b, ...)` | equals one of up to 8 values |
  | `whenNot<Id>(value)` | does not equal the value |
  | `whenRange<Id>(low, high)` | is between the two, inclusive |

  The controlling parameter is in the same block (then it is the one of the
  same instance) or in a block that appears once. A block's own condition
  names a parameter of another block. A condition that breaks these rules does
  not compile, and neither does a default outside `min..max` or outside the
  option list.

Conditions are for ETS only. The device does not evaluate them, and a hidden
parameter keeps the value it last had: firmware branches on the controlling
parameter, never on "hidden means default".

Widths in the parameter memory:

| Type | Width |
|---|---|
| `bool`, `uint8_t`, an enum over `uint8_t` | 1 octet |
| `uint16_t`, `int16_t`, an enum over those, `Dpt9Float` | 2 octets |
| `float` | 4 octets |

The offsets are computed by the C++ layout and written into the export; the
Python exporter places each parameter at `base + instance × stride + offset`
and derives nothing itself. The code segment of the product file and the
download are exactly as long as the image.

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

The XML is self-contained: it carries a `MasterData` section naming the medium,
mask version, manufacturer and datapoint types it uses, so it can be validated
and read back (`knxprod2profile.py`) without ETS. A `.knxprod` archive has no
such section — master data is ETS's own `knx_master.xml`. Strip `MasterData`
before handing the file to a packer that signs with the ETS libraries, e.g.
OpenKNXproducer: it splits the input into `Catalog.xml`, `Hardware.xml` and the
application program file while removing only the elements it knows about, so
`MasterData` is copied into all three and the ETS hasher fails on the first one
it reads with `Naming pair for MasterData cannot be found`.

### Registration

ETS will not import a product it considers unregistered unless the installation
holds a manufacturer test licence:

> No valid license found to test the unregistered product(s) in this file.

Registration is declared, not proved, and it has to be declared in two places —
`Hardware/Products/Product/RegistrationInfo` and
`Hardware2Programs/Hardware2Program/RegistrationInfo`, the latter with a
`RegistrationNumber` of `<4 digits>/<hardware version><application version>`.
The exporter emits both. The manufacturer id is not what gates this. `M-00FA`
(250) is `KNX Association` in the KNX master data and the id OpenKNX-style
community products use, but a file carrying it is still refused without the
registration declaration — as is any other id.

### Table capacities

`Static/AddressTable` and `Static/AssociationTable` declare how many group
addresses and associations the device can hold. ETS reads a missing declaration
as zero and refuses the very first link:

> The maximum associations of the device are been reached. No more associations
> are possible.

The numbers are not product data — they are what `AddressTableDomain` and
`AssociationTableDomain` hold, and what the memory segments in
`table_segments.hpp` the ETS download writes into can take. So the C++ exporter
reads them from those domains and puts them in the JSON as
`capacities.addressTableMaxEntries` / `associationTableMaxEntries`, and the
Python exporter refuses to render a product whose JSON predates them rather
than guessing. Do not confuse them with `addressTableEntries` /
`associationEntries` in the same block, which count what this product's own
communication objects need.

### Identifier shapes

ETS ids are structured, and both ETS and the packers parse them back apart, so
the field widths matter:

| Id | Shape | Example |
|---|---|---|
| Application program | `M-<mfr>_A-<number:4hex>-<version:2hex>-<hash:4hex>` | `M-00FA_A-0016-06-0000` |
| Hardware | `M-<mfr>_H-<serial>-<version>` | `M-00FA_H-0002-1` |
| Hardware2Program | `<hardware>_HP-<number:4hex>-<version:2hex>-<hash:4hex>` | `M-00FA_H-0002-1_HP-0016-06-0000` |
| Code segment | `<application program>_RS-<load state machine:2>-<offset:5>` | `M-00FA_A-0016-06-0000_RS-04-00000` |

The code segment id is read back apart during the download, not just matched:
its two groups have to be the `LoadStateMachine` and `Offset` of the segment
they name, or ETS fails Download(All) with `ID '..._RS-0000' is not a valid id
ID` — after the product has imported and linked perfectly well.

The application number is a 16-bit field: four hex digits, never more. The
trailing `0000` is a placeholder the ETS signer replaces with the hash it
computes over the program — that is also how a re-imported product is
recognised as a new version rather than a corrupted old one. Everything inside
the application program is prefixed with the application program id, so a
change to that id renames the whole tree with it.

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
- **No channels.** Parameter pages and communication objects are not grouped
  into ETS `<Channel>` elements. A parameter layout gives one page per block
  instance, but the communication objects of a repeated function are still a
  flat list, and an object cannot be hidden by a parameter (step P4 of
  [parameter-model.md](../design/parameter-model.md)).
- **A parameter schema has equality-only conditions.** Ranges and several
  values need a parameter layout.
- **The manufacturer name is taken from the product display name.** Fine for
  development against manufacturer id 0x00FA; a real product needs its own
  allocated manufacturer id and name.
