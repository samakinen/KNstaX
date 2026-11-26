# ETS export workflow

How a `constexpr` product definition becomes a `.knxprod` ETS can import.

## The pipeline

```
product.hpp  (constexpr product definition — the single source of truth)
     │
     │  a host-side exporter binary from cmake/knxprod_gen_main.cpp.in,
     │  compiled with the firmware's KNstaX configuration
     ▼
<name>.json             exportCommissionedProductToJson()
     │
     │  tools/knxprod_exporter/exporter.py
     ▼
<name>.knxprod.xml      KNX project schema v23, self-contained (with MasterData)
<name>.ets.xml          the same without MasterData: the packer's input
     │
     │  tools/ets_product/ets_product.py: validate, lock, pack, dist
     ▼
<name>.knxprod          packed and signed, on a PC with ETS
```

A firmware declares its product once, in the firmware's own CMake project:

```cmake
include(${KNSTAX_DIR}/cmake/knx_ets_product.cmake)
knx_ets_product(my_product
    DEFINITION ${CMAKE_SOURCE_DIR}/main/product.hpp
    SYMBOL     my_product::kProduct
    LOCK       ${CMAKE_SOURCE_DIR}/product.lock
    DIST_DIR   ${CMAKE_BINARY_DIR}/dist
    DIST_FILES ${CMAKE_BINARY_DIR}/my_firmware.bin
    DIST_DEPENDS my_firmware
)
```

The point of the design is that the runtime and the ETS catalogue entry are
derived from the *same* declaration and the *same* configuration. There is no
second place to update when a port is added or a table grows, and no way for the
two to drift. `cmake/knx_ets_product.cmake` documents every argument;
`knx_commissioned_product()` (`cmake/knx_product.cmake`) is the step that
produces the JSON and the XML, for a host project that wants only those.

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
inline constexpr auto kFunctionOptions = params::options<Function>(
    {{Function::Disabled, "Disabled"}, {Function::Switch, "Switch"}, {Function::Dimming, "Dimming"}});
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

## Object views: objects shown by a parameter

A channel whose function is a parameter uses some objects for one function and
others for another, and one object can mean different things (object A of an
input is a switch for one function, a shutter move for the next). Object views
(`knx/product/object_views.hpp`) tell ETS when to show a port and as what:

```cpp
inline constexpr auto kObjectViews = std::array{
    views::view<Port::In1A>({.instance = 0,
                             .visible = params::when<In::Function>(InputFunction::Switch),
                             .function = "Switch", .dpt = {1, 1}}),
    views::view<Port::In1A>({.instance = 0,
                             .visible = params::when<In::Function>(InputFunction::Shutter),
                             .function = "Move", .dpt = {1, 8}}),
};
```

```cmake
knx_commissioned_product(my_product DEFINITION product.hpp SYMBOL kProduct VIEWS kObjectViews)
```

- A port with views gets one `ComObjectRef` per view, each `ComObjectRefRef`
  inside a `<choose>` on the condition's parameter: alternatives on one object
  number. A port without views is exported as before, always shown.
- `instance` is the instance of the repeated block the condition's parameter
  belongs to; conditions use the same forms as parameter conditions.
- `text` sets `Text` (default: the port's display name) and `function` sets
  `FunctionText` (default: the text); `dpt` sets the datapoint type ETS shows.
- The run time does not change: the port keeps its own datapoint type and value
  type, and a view may only show a type of the same size. That is enough
  because the encoding goes by size: one 1-byte port carries 5.010, 5.001,
  17.001 and 18.001, one 1-bit port every DPT 1.
- The exporter refuses a view of another size, a view of an unknown port or
  block instance, and two views of one port that could be shown at once (they
  must test the same parameter, and no value in 0..255 may satisfy both).
- The list is passed to the export only; the firmware never reads it, so it
  costs a device no flash.

## Building

The product is built with the firmware: target `<name>_ets_product`, in `ALL`.
Its files land in the build directory, or in `OUTPUT_DIR` when the call gives
one, and every build refreshes them, so they are never stale.

**Configuration.** The product file declares the device's capacities (table
sizes, Data Secure key tables, APDU length), which come from the KNstaX
configuration the exporter is compiled with. In a host build that is the
build's own. A bare-metal or ESP-IDF firmware is cross-compiled, so
`knx_ets_product()` runs the exporter in a nested host build
(`cmake/ets_product_host`) whose configuration is:

1. the `host-minimal` profile;
2. the firmware's configuration, imported (`knx_configure(... IMPORT ...)`): the
   `CONFIG_KNX_*` lines of a standalone build's resolved `knx.config` or of an
   ESP-IDF build's `sdkconfig`. What a host build cannot take — the ESP-IDF
   medium choice and AES peripheral, the IP menu behind `KNX_MEDIUM_IP` — is
   listed in the configure output, not applied;
3. `configs/ets-export.defconfig` (and `ets-export-secure.defconfig` with Data
   Secure): the host platform, no media drivers, no mbedTLS. None of it changes
   what the product declares.

The nested build needs a host C++23 compiler (GCC 13 or later) next to the cross
toolchain; `KNX_ETS_HOST_C_COMPILER` and `KNX_ETS_HOST_CXX_COMPILER` name one
explicitly, and `-DKNX_ETS_PRODUCT=OFF` skips the product.

**Validation.** `<name>.knxprod.xml` is validated against the KNX project schema
when Python has `xmlschema`; without it the build says so and goes on.

**Product lock.** With `LOCK`, the build compares the product with its entry in
the lock file, a JSON file the firmware commits:

```json
{
  "my_product": {
    "application": "M-00FA_A-0001",
    "content": "sha256:…",
    "version": 5
  }
}
```

`content` is the SHA-256 of the canonical `<name>.ets.xml` (XML C14N, text
trimmed), so line endings and indentation do not count and any change ETS can
see does. ETS refuses a changed program under an application version it has
already imported ("the product has a different hash than the existing
product"), so:

| The product | Its version | The build |
|---|---|---|
| unchanged | unchanged | passes |
| changed | raised | passes and records the new entry: commit the lock with the change |
| changed | unchanged or lower | **fails**: raise the application version |
| new to the lock | any | records it |

This also catches a KNstaX update that changes the exporter's output: moving
the submodule either passes or names the version to raise. For iterations that
never reach ETS, `-DKNX_ETS_LOCK_MODE=warn` reports instead of failing and
leaves the lock as it is.

**Dist.** With `DIST_DIR`, target `<name>_dist` collects the product files and
`DIST_FILES` (the firmware image) in `DIST_DIR/<name>/`, with a `manifest.json`
naming the application, its version and content hash, the firmware and KNstaX
commits (`git describe`) and the SHA-256 of every file. The image and the
product file that belong together travel together.

CI renders a set of reference products and checks the result:
`test_knxprod_exporter` (structure), `test_knxprod_xsd_validation` (the KNX
project schema), `test_exporter_parameter_blocks`, `test_exporter_medium` and
`test_exporter_object_views` (parameters and the load procedure, the medium,
object views), and `test_ets_product` (the lock, the archive check, packing and
dist).

## Packing and importing into ETS

ETS imports a `.knxprod`: a ZIP of `knx_master.xml`, the manufacturer's
`Catalog.xml`, `Hardware.xml` and application program, and an
`M-xxxx.signature`. Packing computes the program's hash (which replaces the
`0000` placeholder in the ids) and signs the manufacturer folder. Both are done
by the ETS libraries, so packing runs on a PC with ETS installed.

`tools/ets_product/ets_product.py pack` packs through a **packer backend** and
then reads the archive back (`check`): the signature, the parts, an application
id without the placeholder, a `Hash` on the program and on its
`Hardware2Program`, and the application number and version of the product
file. On the PC with ETS:

```
python <KNstaX>/tools/ets_product/ets_product.py pack <build>/dist
```

packs every `*.ets.xml` under the directory, writes `<name>.knxprod` next to it
and records the packer in `manifest.json`. A build that finds a backend packs by
itself (`PACKER auto`, the default); one that does not, such as a Docker build,
says how to pack and goes on.

The backend today is **OpenKNXproducer** (found as `OPENKNXPRODUCER` or on
`PATH`; tested with 4.3.12, other versions are packed with a warning). It is
given `-N` (it otherwise reports the working directory as a missing schema), and
an archive it writes before crashing on its success banner is accepted when the
check passes. A new backend is a class in `ets_product.py` with `find()`,
`describe()` and `pack()`; the firmwares do not change.

`<name>.ets.xml` has no `MasterData`. The self-contained `<name>.knxprod.xml`
carries one, naming the medium, mask version, manufacturer and datapoint types
it uses, so that it can be validated without ETS. A `.knxprod` takes master
data from ETS's own `knx_master.xml`, and a packer that signs with the ETS
libraries copies an input's `MasterData` into every part of the archive, where
the ETS hasher fails on it: `Naming pair for MasterData cannot be found`.
Kaenx-Creator imports either file.

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

What the exporter does not produce yet:

- **English only.** A single `<Language Identifier="en-US">` block; no
  translations. A product sold outside one language market needs them.
- **No channels.** Parameter pages and communication objects are not grouped
  into ETS `<Channel>` elements. A parameter layout gives one page per block
  instance, but the communication objects of a repeated function are still a
  flat list ([parameter-model.md](../design/parameter-model.md) §3.7).
  Objects can be hidden and given alternative datapoint types by a parameter
  (object views, above), but ports are declared one by one.
- **The manufacturer name is taken from the product display name.** Fine for
  development against manufacturer id 0x00FA; a real product needs its own
  allocated manufacturer id and name.
