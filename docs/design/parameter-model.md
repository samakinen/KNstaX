# Design: one parameter model for every target

| | |
|---|---|
| Status | **Built**, 17 November 2026: steps P0 to P3 (§8). The parameter layout is the only parameter model; the old one is removed from KNstaX and Habinari has moved to the new one. Open: an ETS6 import and download on a device (first on the H8i8o), and step P4, channels for communication objects. |
| Goal | ETS parameters cost a product almost nothing at run time, on a 128 KiB Cortex-M as on an ESP32, and a product with *n* identical channels declares a channel once. |
| Replaces | `StaticParameterSchema`, `ParameterState`, `ParameterCallbacks` and the per-parameter `onParameterChanged` in `commissioned_product.hpp`. The project is greenfield, so the old model is removed, not kept beside the new one. |
| Motivating product | H8i8o milestone M6: 8 binary inputs and 8 outputs, about 200 parameters (h8i8o `docs/design/02-application-spec.md`). |
| Builds on | [small-target-profile.md](small-target-profile.md) §5 ("values that come from the product") and S1 (table-driven runtime, names kept out of the image). Covers item K9 of [cortex-m-tpuart2-support.md](cortex-m-tpuart2-support.md) §11. |

## 1. Why

Measured on the H8i8o firmware (`tp1-small`, Cortex-M3, LTO, 16 November 2026):

| Build | Flash | Note |
|---|---:|---|
| 24 objects, no parameters (M5) | 94,916 B | |
| 68 objects, no parameters | 95,616 B | Objects are cheap since S1 and S2: 16 B of flash and about 30 B of heap each |
| 68 objects, 200 parameters (`uint8_t`, 4 options, groups, visibility) | 213,296 B | **About 590 B of flash per parameter** |

What today's model costs, per parameter:

- **Flash.** Each parameter is its own type in a tuple. `applyFromBytesAt<I>` and `appendBytesAt<I>`
  are instantiated per parameter, and the runtime keeps a pointer to the schema, so every
  descriptor is linked: key, display name, group, a 24-entry option array (384 B), bounds.
- **RAM.** A 64-byte `InplaceFunction` and a `std::optional<T>` per parameter. The probe did not
  link, so this is not measured; by the type sizes it is about 70 B per parameter.
- **Three copies of the values.** The download segment in `DeviceModel` (a fixed 256 B array), the
  `std::vector` in `ApplicationProgramObject`, and the typed optionals in `ParameterState`.
- **A fixed 256-byte limit** (`tableseg::kApplicationCodeSize`), which M6 would exceed.
- **No repetition.** Eight channels are eight copies in source, and the exporter puts everything
  under one `ChannelIndependentBlock`.

Habinari's firmware (81 parameters) shows what products want from the API: it built its own table
so that one function loads every parameter into the control state, and it carries a hand-made
"Parameter Layout Version (do not change)" parameter to detect a stale stored block. Both are things
the model should provide.

## 2. Principles

1. **The declaration is the only source.** Firmware layout and `.knxprod` come from the same
   `constexpr` definition, as for ports.
2. **The downloaded bytes are the only run-time state.** One buffer, sized from the product.
   No decoded copy, no per-parameter object.
3. **The firmware links what it reads.** Names, options, sections and conditions are export data.
   The image carries the default values and, optionally, the bounds.
4. **Repetition is part of the model.** A channel is declared once; the instance is a run-time index.
5. **The layout has one owner.** One `consteval` function decides every offset. Firmware and
   exporter never compute an offset themselves.

Not goals: translations, ETS Modules (`ModuleDef`), parameter pages with custom layout, migration
of stored values between application versions.

## 3. Design

### 3.1 Terms

| Term | Meaning |
|---|---|
| Parameter | One value ETS writes: an id, a value type, a default, export data |
| Block | A named list of parameters with its own id enum. One block is one ETS section, or one channel |
| Layout | The product's blocks in order, each with a repeat count. Decides every offset |
| Image | The bytes of the layout: what ETS downloads, what the device stores |
| Default image | The image with every parameter at its default, a constant in flash |
| View | Typed read access to an image |

### 3.2 Declaring parameters

```cpp
enum class InputFunction : uint8_t { Disabled, Switch, ShortLong, Dimming, Shutter, Value, Scene };
enum class Action : uint8_t { Nothing, On, Off, Toggle };

// Option lists are named constants: several parameters share one, and the
// exporter emits one ETS ParameterType for it.
inline constexpr auto kActionOptions = params::options<Action>(
    {{Action::Nothing, "Nothing"}, {Action::On, "On"}, {Action::Off, "Off"}, {Action::Toggle, "Toggle"}});

enum class In : uint8_t { Function, DebounceMs, Rising, Falling, LongPressMs /* ... */ };

inline constexpr auto kInputBlock = parameterBlock<In>(
    {.key = "input", .name = "Input {n}"},
    param<In::Function, InputFunction>({.name = "Function", .def = InputFunction::Disabled,
                                        .options = kInputFunctionOptions}),
    param<In::DebounceMs, uint8_t>({.name = "Debounce time", .def = 30, .min = 10, .max = 200, .unit = "ms"}),
    param<In::Rising, Action>({.name = "On closing", .def = Action::Toggle, .options = kActionOptions,
                               .visible = when<In::Function>(InputFunction::Switch)}),
    param<In::LongPressMs, uint16_t>({.name = "Long press after", .def = 500, .min = 300, .max = 3000,
                                      .unit = "ms",
                                      .visible = whenAny<In::Function>(InputFunction::ShortLong,
                                                                       InputFunction::Dimming)}));

inline constexpr auto kParameters = parameterLayout(
    kDeviceBlock,               // once
    repeat<8>(kInputBlock),     // "Input 1" … "Input 8"
    repeat<8>(kOutputBlock));
```

- **One id enum per block.** The enum type identifies the block, so `In::Function` needs no
  channel prefix and cannot be used with the wrong block. Two blocks with the same enum type in one
  layout are a compile error.
- **Value types:** `bool`, `uint8_t`, `uint16_t`, `int16_t`, scoped enums over those, `Dpt9Float`
  and `float` (the last two need `KNX_DPT_FLOAT`). An enum-typed parameter reads back as the enum.
- **A product without channels** declares one block, as Habinari would. Nothing forces `repeat`.

### 3.3 Layout

- Blocks in declaration order; a repeated block is `count × stride` octets; parameters in
  declaration order inside a block; whole octets; 16-bit values big-endian. This is what the
  exporter emits today, so ETS sees no change in kind.
- `consteval` code produces, from the *types* in the layout: each parameter's offset in its block,
  each block's base and stride, and the image size. From the *values* it produces the default image
  and a 32-bit **layout fingerprint** (a hash over ids, value types and offsets).
- The image size replaces `kApplicationCodeSize`. A profile sets only a ceiling
  (`KNX_APPLICATION_SEGMENT_BYTES`), checked with a `static_assert` that names the symbol.
- **Planned extensions, not in the first version:** bit-packed fields (`bits<3>`), and ETS
  `<Union>` overlays for parameters that are never visible together (the function-specific
  parameters of an input). Both change only the layout function and the exporter; product code that
  uses the view does not change. Both need a new application version.

### 3.4 Run time

**One buffer.** The product runtime owns the image, an array of exactly the layout's size, and
binds it into the stack before start (`objects::ParameterImageBinding`: the image, the default
image, the fingerprint, the bounds check). ETS memory writes land in it,
`ApplicationProgramObject` exposes it as its program data, the view reads it. The `std::vector`
copy and the typed copy in `ParameterState` go away. *As built in P1; the first draft put the
buffer in `DeviceModel`, which is not a template and could only have sized it by a ceiling.*

**Storage.** The image is stored as one record: the fingerprint, then the image. While the
application program is not loaded the record is a single zero octet, so a restart cannot turn
defaults, or half of a download, into a loaded application.

**Start-up.**

| What is stored | Image at start | Application program |
|---|---|---|
| Nothing (a device never downloaded) | Defaults | Loaded, as the tables are |
| A record of this layout, every value inside its bounds | The stored image | Loaded |
| A record of another layout (new firmware), a value outside its bounds, or the zero octet | Defaults | **Unloaded** |

The third row replaces Habinari's hand-made version parameter: ETS and the product
(`parametersLoaded()`) both see that a download is needed. Today's special case "a segment nobody
wrote reads as zeros" is gone, because a download that writes nothing leaves the image as it was.

**Reading.**

```cpp
auto params = app.parameters();               // view over the image
auto dev    = params.block<Dev>();
auto in     = params.block<In>(channel);      // channel is a run-time index, 0-based

InputFunction f = in.get<In::Function>();     // one byte load at a constant offset
uint16_t ms     = in.get<In::LongPressMs>();  // two byte loads
```

A block view is one pointer. `get` has no table and no search: the offset is a compile-time
constant, and the channel logic that calls it is compiled once for all channels.

**Change notification.** One callback, `onParametersChanged(view)`, after a completed download
that passed the bounds check and after a run-time `setParameter`. It gets the view, so it needs no
handle to the product. Products reload what they keep decoded. It is not called at start: the
product reads `parameters()` once `startCommissionedProduct()` has returned.

**During a download.** ETS writes into the live image, so between `StartLoading` and
`LoadCompleted` the image is half old and half new. `parametersLoaded()` is false for that time,
and after a download that was refused or never completed. A product that reads parameters
continuously, not only in `onParametersChanged`, checks it. The lifecycle state does not change:
it reports programming mode and the individual address only. If a download never completes, the
restart brings back the last completed one, because the image is stored only at `LoadCompleted`.

**Writing from the device** (today's `applyCommissionedParameter`):
`product.setParameter<Id>(value)`, or `setParameter<Id>(instance, value)` for a repeated block. It
checks the value against the parameter's bounds, encodes it into the image, has the image stored
with the next coalesced save and fires the callback. The coalesced save needs the product's clock
(`setTimeSource`); without one the image is stored only when ETS restarts the device.

**Validation at load.** ETS enforces ranges, but a device should not trust its memory blindly. A
compact bounds table (offset, width, minimum, maximum; about 6 B per *declared* parameter, not per
instance) is checked once when an image is taken into use: at `LoadCompleted` and at restore. An
image that fails is rejected as a whole: default image, load state *Error* (which ETS reads back
as a failed download), unloaded after the next restart. After that, `get` trusts the buffer.
Making the table a profile option (`KNX_PARAMETER_VALIDATION`) is left for P3; in P1 it is always
linked.

### 3.5 What the firmware links

| Data | In the image | Size |
|---|---|---|
| Default image | Yes | Image size (H8i8o: about 560 B, estimated) |
| Bounds table | If validation is on | About 6 B per declared parameter |
| Fingerprint, image size, strides | Yes | A few words |
| Keys, names, units, options, sections, conditions | **No** | — |

The runtime never takes a reference to the layout object, only to constants derived from it (the
way `kPortTable` is used since S1). The size gate's `forbid_strings` check gets one parameter name
per block, so a change that pulls the descriptors back in fails CI.

A target that wants the descriptors at run time, for example a web page that lists parameters on
an ESP32, can reference the export table from its own code. It is the same constant data; it is
then linked for that product only.

### 3.6 Conditions

One condition type, used for parameters, sections, and (§3.7) communication objects:

| Form | ETS output |
|---|---|
| `when<Id>(value)` | `<choose><when test="value">` |
| `whenAny<Id>(a, b, …)` | `<when test="a b …">` |
| `whenNot<Id>(value)`, `whenRange<Id>(lo, hi)` | `<when test="!=value">`, `<when test="&gt;=lo">` nested with `&lt;=hi` |

- A condition names a parameter of the same block instance, or of an unrepeated block. A condition
  on another channel's parameter is not supported.
- A parameter condition and its section's condition combine (logical and), as today.
- **Conditions are export data.** The device does not evaluate them. A hidden parameter keeps
  whatever value it last had, so firmware logic must branch on the controlling parameter itself,
  never assume that "hidden" means "default".

### 3.7 Channels: ports repeat with their block (K9)

A channel is a parameter block plus the ports that belong to it, repeated together:

```cpp
repeat<8>(channel("Input {n}", kInputBlock,
    port<InPort::A>(/* … */),
    port<InPort::B>(/* … */, visibleWhen = whenAny<In::Function>(/* … */))))
```

- **Object numbers:** channel base + instance × ports per channel, so a function change never
  renumbers (h8i8o 02 §1).
- **Bindings and publishes take the instance:** `publish<InPort::A>(channel, value)`,
  `onCommand<OutPort::Switch>([](unsigned channel, bool on) { … })`. One binding entry serves all
  instances, which also removes binding RAM (12 B per receive port today).
- **Exporter:** one ETS `<Channel>` per instance, `ComObjectRefRef` wrapped in `<choose>` for
  conditional objects, and several `ComObject` definitions on one number for alternatives
  (K9 items 1 to 4).
- **Alternatives on one object number** (object A of an input is DPT 1.001, 5.010 or 18.001 by
  function): the port lists its alternatives with a condition each; storage is sized for the
  largest; the product publishes with the alternative it selected.

This part changes the endpoint definition and the port table, and is a second phase (§8). It is
less settled than §3.2 to §3.6: the binding API and the alternatives need their own short design
once the parameter part is measured.

### 3.8 Exporter and JSON

- The JSON gains `parameterBlocks` (key, name text, repeat count, base, stride, parameters with
  their offsets) and `optionTypes` (shared lists). **Offsets are written by the C++ side**; the
  Python exporter stops deriving them from declaration order.
- The Python exporter expands repeated blocks into concrete `Parameter`, `ParameterRef` and
  `ParameterBlock` elements. ETS Modules would make the file smaller but are a larger step and not
  needed for import.
- `knxprod2profile.py` already reads a product back; a test compares the offsets in the generated
  file with the C++ layout for every example product.

### 3.9 Removed

`StaticParameterSchema`, `ParameterDescriptor` in its present form, `ParameterState`,
`ParameterCallbacks`, per-parameter `onParameterChanged`, the seven `wireParameterDataCallback`
lambdas, `ApplicationProgramObject::_parameterData`, `kApplicationCodeSize`,
`kMaxParameterOptions`, and `endpoint::TransmitPolicyBinder`: it collected values that arrived one
callback at a time, and a product now builds the whole policy from the view.

## 4. Evaluation

### 4.1 Developer experience

| | Today | Proposed |
|---|---|---|
| Eight channels | Eight copies, or a macro | `repeat<8>` |
| Read a value | `parameters().get<Id>()`, typed | The same, plus the channel index; enums read as enums |
| React to a download | One callback per parameter; Habinari built a table to get "reload all" | One callback; reload |
| Stale stored block after a firmware change | Product's own version parameter | Fingerprint, automatic |
| Unwritten segment | Special case in `DeviceModel` | Cannot occur: the buffer starts as the default image |
| Wrong id, wrong block | Compile error | Compile error |
| Error messages | Deep template traces | Deep template traces; **not better** |

Worse than today in two places: a product that wants to know *which* parameter changed must
compare values itself, and ids need one enum per block.

### 4.2 Maintainability

- **Less code and one path.** One layout function, one buffer, one notification. The download
  path loses its special cases.
- **The metaprogramming is concentrated.** The layout function, the default image and the
  fingerprint are `consteval` code over type lists, the hardest kind of code in the stack to read
  and debug. It is small (a few hundred lines, estimated) and fully host-testable with
  `static_assert`s, but it needs a maintainer comfortable with it.
- **The C++/Python contract grows.** Blocks, shared option types and conditions are new JSON, and
  the Python exporter gains the expansion. The offset comparison test (§3.8) is the guard.
- **Every ETS feature still needs an import test on a real ETS6**, which is manual. Channels and
  conditional objects are new ground for the exporter.

### 4.3 Scalability

| Dimension | Behaviour |
|---|---|
| Flash per parameter instance | Its default octets. The code is per *read site*, not per parameter |
| RAM | The image, once. H8i8o: about 560 B at whole octets (estimated from 02), roughly half with bit packing |
| Compile time | Follows the number of *declared* parameters (H8i8o: about 55, not 200). Habinari's 81 in one block are the largest single list. **Not measured** |
| Large products | Bounded by the 16-bit memory address space of the download and by ETS download time: at APDU 15, 12 octets per write, so 560 B is about 47 writes plus verification |
| `.knxprod` size | Grows with instances, because the exporter expands them |

### 4.4 Performance

- A read is one or two byte loads. No decoding at download, no callbacks per parameter.
- Start-up copies the image once and, with validation on, walks the bounds table once.
- `Dpt9Float` and `float` parameters decode on every `get` (software floating point on a
  Cortex-M3). Products that read them in a loop should keep the decoded value, as Habinari does.

### 4.5 Estimated effect on the H8i8o

| | Today's model (measured) | Proposed (estimated) |
|---|---:|---:|
| Flash for 200 parameters | 117,680 B | 1–2 KB: default image 560 B, bounds table about 350 B, view code at the read sites |
| RAM for 200 parameters | Not measured (did not link) | About 560 B |

## 5. Downsides and risks

1. **It breaks every product.** Habinari (81 parameters and a table of setters), the thermostat
   example, three test suites and two guides must move. Habinari's setter table maps almost
   directly onto "reload on `onParametersChanged`", but that migration is not sized yet.
2. **No per-parameter change event.**
3. **The whole image is in RAM.** Irrelevant on an ESP32; on the H8i8o it is a visible share of
   the roughly 3 KB the application has. Bit packing and unions are the answer, and they are not in
   the first version. Reading the image straight from flash would remove it entirely but ties the
   model to memory-mapped storage, so it is left out.
4. **Position is identity.** Inserting or reordering a parameter moves the ones after it, which
   needs a new application version and a new download. That is true today as well; the fingerprint
   now enforces it. There is no carrying of old values into a new layout.
5. **Hidden parameters keep stale values** (§3.6). A firmware bug of the kind "acted on a parameter
   ETS was hiding" is possible and not caught by the model.
6. **Conditions are limited** to what nests cleanly in `<choose>/<when>`: one controlling parameter
   per condition, no conditions across channels, no general boolean expressions.
7. **Validation is by range only.** An enum with gaps in its values is checked against its minimum
   and maximum, not its option list.
8. **Run-time introspection is opt-in** and costs the descriptors' flash where used.
9. **The numbers are estimates.** In particular the compile time and whether the linker really
   drops every descriptor under LTO; S1 showed that one stray reference pulls in kilobytes.
10. **Phase 2 (§3.7) is a larger change than the parameters**, touching the port table and the
    binding API that S1 just settled.

## 6. Alternatives considered

| Alternative | Why not |
|---|---|
| **Compact path beside today's model** | Two models to document, export and test; products must choose. Only compatibility would justify it, and there is none to keep |
| **Today's API, table-driven** (as S1 did for ports) | Removes the per-parameter templates, but each instance still has a descriptor entry and RAM for its value and callback; estimated 8–15 KB on the H8i8o; no repetition |
| **Schema file (YAML or XML) and a code generator** producing a C header and the `.knxprod`, as OpenKNX does | The real competitor. Plain generated code, no metaprogramming, and every ETS feature is directly reachable. But it adds a second source language and a build step, splits a product between a file for parameters and C++ for ports, and gives up the property this stack is built on: one `constexpr` definition, checked by the compiler |
| **Plain structs with reflection** | The natural form (`struct InputParams { … }`), but it needs C++26 reflection, which the toolchains here do not have. The view API is chosen so that a struct-based front end could be added later without changing the layout or the exporter |

## 7. Verdict

For the long run this is the right shape: it applies to parameters the rule S1 and S2 already
applied to ports (constant tables, nothing per item at run time, names outside the image), it
removes more code than it adds, and it gives products the two things they have been building by
hand, repetition and "reload everything". Its real costs are the migration, the concentrated
metaprogramming and the loss of per-parameter events. None of them grows with the number of
products, whereas the cost of today's model grows with every parameter.

The part to be careful with is phase 2. The parameter model stands on its own and should be
measured and settled first.

## 8. Work plan

| Step | Content | Exit |
|---|---|---|
| P0 | Prototype in a branch: `parameterBlock`, `repeat`, layout, default image, view. The H8i8o probe again with 68 objects and 200 parameters | **Decision point:** flash for the parameters ≤ 3 KB, RAM = image size, compile time acceptable, no descriptor string in the ELF. Otherwise stop and review |
| P1 | Single buffer sized from the product; fingerprint; validation; `onParametersChanged`; `set` | Host tests, including a download through the stack simulator |
| P2 | Exporter: blocks, shared option types, conditions, offsets from C++; offset comparison test | XSD validation; ETS6 import of the thermostat example |
| P3 | Migrate Habinari, examples, tests and guides; remove the old model | All suites green; size gate budgets lowered |
| P4 | Channels (§3.7): own short design first, then ports, bindings, `<Channel>`, conditional objects, alternatives | ETS6 import and download on the H8i8o |

### P0 result (17 November 2026)

Built: `include/knx/product/parameter_layout.hpp` (namespace `knx::product::params`, beside the old
model) with blocks, `repeat`, the layout, the default image, the fingerprint, the bounds table and
its check, the views, and a walk over the declared parameters for the exporter. Host test
`test_parameter_layout` (8 cases, and the layout checked with `static_assert`s); the host suite is
162 of 162.

Measured on the H8i8o firmware with 68 objects (`tp1-small`, Cortex-M3, LTO), the same probe shape
as §1: a device block of 8 parameters, 8 inputs and 8 outputs of 12 each, 200 parameters from 32
declarations, every one with a name, a third with an option list, and 5 of the 12 parameters of
each channel block with a condition. The probe copies
the default image to RAM, checks it against the bounds and reads every parameter once per channel.

| | Old model | New model | Decision point |
|---|---:|---:|---:|
| Flash for the 200 parameters | 117,680 B | **1,108 B** | ≤ 3 KB |
| of which the default image | | 250 B | |
| of which the bounds table | | 286 B (32 entries of 8 B, 3 blocks of 10 B) | |
| of which code: the copy, the check and 32 read sites | | 536 B | |
| Static RAM for the parameters | did not link | 250 B, the image | = image size |
| Whole firmware | 213,296 B | 96,724 B | |
| Parameter names or option labels in the image | all | none (searched the binary) | none |
| Clean build of the firmware, wall clock | 35 s | 17 s | acceptable |

On the board the probe ran and returned the expected checksum of all 200 defaults, so the offsets
the Cortex-M3 build reads at are the ones the layout declares.

What P0 did not cover:

- **The runtime.** The image was a buffer in the probe. The single buffer in `DeviceModel`, the
  fingerprint check at start-up, `onParametersChanged` and `set` are P1.
- **The exporter.** The walk (`forEachParameter`) exists and is tested on the host, but no JSON or
  `.knxprod` comes from it yet (P2).
- **The H8i8o's real parameter list.** The probe's image is 250 B because its blocks are 12
  parameters of mostly one octet. The real list (h8i8o 02) is longer, with 16-bit times: the 560 B
  estimate of §4.3 stands.
- **Compile time of a large flat block.** The probe's largest block has 12 parameters. Habinari's
  81 in one block are not measured.

One thing the prototype settled about the API: firmware must name the layout only as a template
argument (`ViewFor<kParameters>`, `kDefaultImageOf<kParameters>`, `kBoundsOf<kParameters>`), never
as a run-time reference. The header says so, and the size gate's string check (§3.5) is what will
enforce it.

### P1 result (17 November 2026)

Built:

- **Stack.** `ApplicationProgramObject` takes an `objects::ParameterImageBinding`; with one bound,
  its program data is the image. `DeviceModel` registers the image as the application code segment,
  so ETS writes straight into it, and checks it at `LoadCompleted`. `InterfaceObjectManager` stores
  and restores the record with the fingerprint. A product without a binding keeps the old path.
- **Product API.** `makeCommissionedProduct<kParameters>(endpoints)`, `parameters()` returning the
  view, `onParametersChanged(view)`, `setParameter<Id>()`, `parametersLoaded()`. The product
  definition carries the layout as a type only (`params::LayoutParameters<kParameters>`).
- **A start overload for a caller-assembled stack port**, `startCommissionedProduct(platform,
  product, bindings, stackPort, options)`. The test needed it to run a product on a simulated bus;
  it is also the way to a medium the other overloads do not cover.
- **Test** `test_parameter_image_download` (integration, 7 cases): a product on the real stack over
  the mock TP1 physical layer, with the test playing ETS on a transport connection (StartLoading,
  `A_Memory_Write`, `A_Memory_Read`, LoadCompleted, `A_Restart`). It covers the defaults, the
  download, the restart, a refused image, an interrupted download, an image of another layout and
  `setParameter`. The host suite is 163 of 163.

Measured on the H8i8o firmware with 68 objects and the 200 parameters of the P0 probe, now through
`makeCommissionedProduct<kParameters>()`:

| | P0 (image in a probe buffer) | P1 (through the runtime) |
|---|---:|---:|
| Whole firmware | 96,724 B | 98,480 B |
| Flash against the same firmware without parameters (95,616 B) | 1,108 B | 2,864 B |
| Heap high water on the board (12,072 B without parameters) | not measured | 12,220 B |
| Parameter names or option labels in the image | none | none |

On the board the firmware started, read all 200 defaults through `parameters()` with the expected
checksum, and reported the application program as loaded.

Of the 2,864 B, about 1,070 B are not for the parameters: the stack now links the old path and the
new one side by side. The same firmware *without* parameters grew by 1,064 B (94,916 to 95,980 B).

**The size gate was over budget until P3 (see "P3 result").** The `tp1-small` Cortex-M3 reference device
is at 88,514 B against a budget of 88,064 B (it was 87,446 B). The budget is not changed: step P3
removes the old path (the program data vector, the separate 256-octet segment, the typed copy), and
the figure is to be back under the budget then. The H8i8o device check of the gate still shows
10,915 B of flash headroom. This branch should not be merged before P3 for that reason.

Not covered by P1: an ETS6 download of a product with the new model, which needs the exporter (P2).

### P2 result (17 November 2026)

Built:

- **C++.** `parameterLayoutToJson()` (`parameter_layout_export.hpp`) writes `parameterImage`,
  `optionTypes` and `parameterBlocks`; an option list is one entry however many parameters use it,
  recognised by the constant it is declared as. `exportCommissionedProductToJson()`
  (`commissioned_product_export.hpp`) is the one call for either parameter model, and
  `knx_commissioned_product()` uses it, so a product's CMake does not change.
- **Checks at compile time.** `params::checkLayout()` runs when a product names its layout. It
  refuses a condition on an unknown parameter, on a parameter of another repeated block, on the
  parameter itself, a block condition from inside the block, and a default outside `min..max` or
  outside the option list. The last one matters on the device: the default image would fail its
  own bounds check.
- **Python.** `exporter.py` expands each block into one set of `Parameter` and `ParameterRef`
  elements and one `ParameterBlock` per instance, with offsets `base + instance × stride + offset`.
  Shared option lists and equal numeric ranges become one `ParameterType` each. Conditions become
  `<choose>/<when>` with the tests `a b c`, `!=v`, and `>=lo` around `<=hi`. A `bool` without
  options is a check box. The code segment and the download are the size of the image.
- **Fixture and tests.** Pilot C in `tools/product_exporter` is a whole product with a repeated
  block, a shared option list and every condition form. `test_exporter_parameter_blocks.py`
  (10 cases), the offset comparison between the JSON and the product file in
  `test_validate_knxprod_xsd.py`, and the JSON and `checkLayout` cases in `test_parameter_layout`.

Checked: Pilot C validates against the KNX project schema 23 with `xmlschema` (the test image does
not have that package and falls back to the structural check, as before), and OpenKNXproducer
4.3.12 signs it into a `.knxprod` with the ETS6 libraries.

Not done in P2:

- **ETS6 import and download on a device.** No product on a board uses the new model yet; the
  H8i8o application (M6) will be the first. The pages are `ParameterBlock`s directly under the
  `ChannelIndependentBlock`, the form the old exporter already used and ETS6 imported.
- **ETS Modules.** Every instance is written out. A file with 8 × 12 parameters has 96 `Parameter`
  elements; that is fine for import but grows with the instance count.
- **`knxprod2profile.py`** was not taught the blocks. The offset comparison is done on the JSON
  and the product file instead.

### P3 result (17 November 2026)

**Sections.** Habinari has 81 parameters under one id enum, on 12 ETS pages, with conditions
across the pages. One block per page would have meant 12 id enums. So a block can now put
parameters on pages of their own: `params::section({.name, .visible}, param..., param...)` inside
`parameterBlock`. A section changes the pages, never the image, and its condition is declared
once (the old model repeated it on every parameter of a group). `checkLayout` refuses a section
whose controlling parameter is inside it.

**Removed from KNstaX:**

- The parameter schema: `StaticParameterSchema`, `ParameterDescriptor`, `parameterOptions`,
  `parameter()`, `makeParameterSchema`, the typed `ParameterState` with its per-parameter decode,
  `CommissionedParametersView`, per-parameter `onParameterChanged`, `applyCommissionedParameter`,
  `expert::applyDemoParameterValue` and `expert::applyParameterDataBytes` (now
  `expert::loadParameterImage`).
- In the stack: the program data vector and the unused parameter data of
  `ApplicationProgramObject`, the 256-octet segment of `DeviceModel`, `kApplicationCodeSize`, and
  `memoryRegionWritten` with its "written" flag, which only existed to tell an unwritten segment
  from zeros.
- In the export: `ExportParameterDescriptor`, the flat `parameters` list in the JSON and in
  `exporter.py`, and the parameter descriptors of the `knxprod2profile` generator.
- `endpoint::TransmitPolicyBinder`.

A product without parameters is `makeCommissionedProduct(endpoints)`: no image on the device, no
code segment in the product file.

**Migrated:** the thermostat example and the tests that used the schema. The flat-list exporter
tests are replaced by `test_exporter_parameter_blocks.py` (17 cases, with sections and the load
procedure). Habinari's firmware moved in its own repository.

**Measured.** Host suite 163 of 163 (two tests of the old model went, one came). The size gate
for `tp1-small` on Cortex-M3 passes again: 87,686 B against the budget of 88,064 B. It was
87,446 B before the parameter work and 88,514 B while both models were linked, so the parameter
image costs the reference device, which has no parameters, 240 B.

Not done: the `KNX_PARAMETER_VALIDATION` profile option (the bounds check is always linked), and
the size gate's `forbid_strings` entry for a parameter name (§3.5), because the gate's reference
device declares no parameters.

## 9. Open questions

1. **Tables inside a block.** The H8i8o output has "up to 8 scene entries" per channel. Either 16
   flat parameters, or one level of `repeat` inside a block. Decide in P0.
2. ~~**Lifecycle during a download.**~~ Settled in P1 (§3.4 "During a download"): the lifecycle
   does not change, `parametersLoaded()` reports it. Still to see with ETS6 on the board (P2):
   whether a partial download writes only the changed octets. The single image handles that case;
   the old separate segment did not.
3. **Fingerprint and the application version.** Should a changed fingerprint with an unchanged
   `applicationVersion` be a build error against a recorded value, so that a layout change cannot
   ship under an old version by accident?
4. **Name text for instances.** "Input {n}" is fixed text. ETS can show a parameter's value in a
   channel name ("Input 3: Kitchen"); that needs a text parameter type, which the model does not
   have.
5. **A device that was never downloaded reports "loaded".** It does so for the tables too, and P1
   keeps the parameters consistent with them. Should a new device report its application program
   as unloaded instead?
6. **A default clock for the coalesced save.** Without `setTimeSource` the stack stores state only
   at an ETS restart. Should the product runtime take the platform's millisecond clock by default?
