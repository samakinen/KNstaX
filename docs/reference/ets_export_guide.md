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

Parameters are declared as a *parameter layout*: blocks of parameters, each
block with its own id enum, a block optionally repeated. ETS downloads them
into a memory-mapped image whose layout the C++ declaration decides
([parameter-model.md](../design/parameter-model.md)).

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

### Sections: several pages for one block

A block with many parameters puts some of them on pages of their own. A
section changes the pages only; the parameters stay in declaration order in
the image.

```cpp
inline constexpr auto kRoomBlock = params::parameterBlock<P>(
    {.key = "room"},
    params::section({.name = "Room control - general"},
                    params::param<P::HeatingEnabled, bool>({.name = "Heating", .def = true}),
                    params::param<P::CoolingEnabled, bool>({.name = "Cooling"})),
    params::section({.name = "Cooling control", .visible = params::when<P::CoolingEnabled>(true)},
                    params::param<P::CoolingGain, Dpt9Float>({.name = "Gain", .def = 25, .min = 1, .max = 100})));
```

- A block that appears once and has all its parameters in sections shows only
  the section pages, under their own names. Otherwise a section page is named
  "block: section", after the block's own page.
- The condition is declared once for the page. Its controlling parameter is
  outside the section, in the same block or in a block that appears once.

### Options, bounds and units

Declare `options` on every enumerated parameter. Without them ETS shows a bare
number field and the integrator has to be told out of band that 2 means
Standby. An option list is a named constant, so parameters that offer the same
choices share one ETS type.

`min` and `max` narrow the ETS input field, and the device checks a downloaded
image against the same bounds: an image with a value outside them is refused
as a whole. `unit` is shown as a suffix.

`Dpt9Float` (the KNX 2-octet float) is preferred over `float`: knxprod
`TypeFloat` support is patchy outside ETS itself. Kaenx-Creator ships no float
validation, mis-imports the size as 16 bit, and parses the default value with
the system locale. Keep defaults and bounds integer-valued (`22`, not `22.5`)
where the product allows it, so they parse identically under every locale.

### Conditions are for ETS only

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

The application program is `LoadProcedureStyle="MergedProcedure"`. ETS runs the
load procedure of the mask version from its own master data, and the product
adds only what the master data cannot know: the parameter segment.

The master procedures of MV-07B0 (TP1) and MV-57B0 (IP) contain numbered merge
points (`LdCtrlMerge`). Every procedure that loads the Application Program
("all", "par", "par,grp") runs `Unload 4`, then `Load 4` followed by merge
point 2, and later merge point 4 just before the tables are written. ETS
inserts each product `LoadProcedure` at the merge point with the same
`MergeId`:

```xml
<LoadProcedures>
  <LoadProcedure MergeId="2">
    <LdCtrlRelSegment LsmIdx="4" Size="34" Mode="0" Fill="0"/>
  </LoadProcedure>
  <LoadProcedure MergeId="4">
    <LdCtrlWriteRelMem ObjIdx="4" Offset="0" Size="34" Verify="true"/>
  </LoadProcedure>
</LoadProcedures>
```

A `LoadProcedure` without a `MergeId` has no place in a merged procedure: ETS
skips it, downloads the tables and completes the load, and the parameters
never reach the device.

`LsmIdx=4` is the Application Program interface object as this stack registers
them (0 Device, 1 Address Table, 2 Association Table, 3 Group Object Table,
4 Application Program). **If that registration order changes, this index must
change with it.** `Size` is the exact parameter image the device decodes, and
`Verify="true"` uses the read-back the device already sends in response to
`A_Memory_Write`.

The exporter refuses another mask version: its merge points may be numbered
differently. A product with no serialisable parameters has no
`LoadProcedures` at all; the master procedure covers the rest of the download.

## Current limitations

Honest list of what the exporter does not yet produce:

- **English only.** A single `<Language Identifier="en-US">` block; no
  translations. A product sold outside one language market needs them.
- **No channels.** Parameter pages and communication objects are not grouped
  into ETS `<Channel>` elements. A parameter layout gives one page per block
  instance, but the communication objects of a repeated function are still a
  flat list, and an object cannot be hidden by a parameter (step P4 of
  [parameter-model.md](../design/parameter-model.md)).
- **The manufacturer name is taken from the product display name.** Fine for
  development against manufacturer id 0x00FA; a real product needs its own
  allocated manufacturer id and name.
