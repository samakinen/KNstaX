# KNstaX

A KNX System-7 protocol stack in C++23 for building ETS-commissionable devices
on ESP32, with a host build for development and testing.

You declare your device once — its communication objects, their datapoint
types, its ETS parameters — as a `constexpr` definition. That single
declaration is both the runtime model the firmware executes and the source the
`.knxprod` catalogue entry is generated from, so the device and its ETS entry
cannot drift apart.

Target profile: **mask version 07B0 (System B, TP1)**.

## Project status

Read this before you plan around it.

**This stack is incomplete and only partly validated.** What has been proven on
real hardware is TP1 over the **bit-bang** backend: ETS6 discovery,
individual-address assignment, application download, group communication — and,
with KNX Data Secure enabled, a secure download and secure group communication.

Everything else is implemented and covered by host tests but has never been run
against real ETS, a real gateway, or real transceiver hardware — TPUART,
KNXnet/IP tunnelling and routing, the coupler routing actions, and the
IP-side secure paths (Secure Tunnelling and Secure Routing). Assume anything
outside the proven path needs debugging on first contact; the support matrix
splits it row by row.

**Not certified, and not verified against the official test suite.** No run of
the KNX Conformance Test Tool has ever been performed, and no device built on
this stack has been submitted for certification. What testing exists is of a
different kind: behaviour is written against the specification texts and cited
by clause, the Data Secure CCM framing is checked against the worked examples in
03/03/07 Annex C, and KNXnet/IP behaviour is exercised on the wire against
`xknx` as an independent peer. That is evidence of being spec-faithful. It is
not evidence of conformance, the two are not interchangeable, and only the KNX
Association can certify a product. This project is not affiliated with or
endorsed by them.

**Data Secure works; the IP-side secure paths are unproven.** A full ETS secure
download and secure group communication have both been exercised on hardware.
KNXnet/IP Secure Tunnelling and Secure Routing have wired runtime paths
validated against the specification's own test vectors, but have never run
against a real peer.

There is no FDSK: key provisioning is manual by design, which makes secure
commissioning a self-service flow rather than the certificate-based one an
integrator expects from a catalogue product.

**The bit-bang TP1 backend is beta.** TPUART is the conservative choice.
Bit-bang works, but it needs the
[bring-up guide](docs/reference/board_bringup_guide.md) and a scope to
commission on a new board.

**RF and Powerline are not implemented and are not planned.** This is not a
matter of unwritten code: there is no RF or PL hardware available to develop
against, and none to test against, so an implementation could not be validated
even if it were written. Treat the media list in the support matrix as closed.

[docs/reference/knx_conformance_status.md](docs/reference/knx_conformance_status.md)
is the service-by-service version of this section, and is deliberately blunt
about every gap between the current state and a certifiable device.

## What you get

- **ETS-driven commissioning.** Individual address, group addresses and
  parameters are owned by ETS and restored from NVS at boot. Firmware never
  hardcodes a group address.
- **Communication flags that are behaviour, not documentation.** All six KNX
  flags (C/R/W/T/U/I) and per-object priority are enforced by the inbound
  dispatch path before a value is touched or a read is answered, and they are
  exported to the `.knxprod`, so what ETS shows is what the device does.
- **Datapoint types with the right sub-type.** Main types 1–20, 232, 242, 243
  and 244, across 84 catalogue entries. The named helpers pick the KNX-standard
  sub-type per signal — a contact reported as 1.001 instead of 1.019 works on
  the wire but shows up in ETS as a switch.
- **Two TP1 backends behind one MAC engine**: a timer-ISR software bit-bang
  (the tested path) and TPUART for NCN5120/NCN5121 (implemented, never run
  against the chip). Selected from Kconfig; nothing above the backend boundary
  changes between them.
- **KNXnet/IP** tunnelling and routing, compiled out entirely on a TP1-only
  build — CI proves the gate by failing if a gated-out medium so much as
  compiles.
- **KNX Secure**: Data Secure through the Security Interface Object with
  `PID_SECURITY_MODE` driven by `A_FunctionPropertyCommand` (the way ETS does
  it), KNXnet/IP Secure Tunnelling with ECDH, and opt-in Secure Routing. Crypto
  is mbedTLS.
- **Couplers**: the routing algorithm of 03/03/03 §2.4.2.4 in full, built
  through the same call as an end device.

## Quick start

The fastest honest path to a device on a bus is the switch example — it drives a
real transceiver rather than a null backend:

```bash
cd examples/esp_idf_switch
idf.py set-target esp32c6      # or esp32, esp32s3, esp32c3 …
idf.py menuconfig              # KNX Stack Configuration → pins, timing, backend
idf.py build flash monitor
```

> **Read the [board bring-up guide](docs/reference/board_bringup_guide.md) before
> connecting to a live installation.** An inverted TX polarity holds the bus
> dominant and takes down the whole line, not just your device.

To build and test on a host with no hardware at all:

```bash
./build.sh test
```

## What a product looks like

The whole device model is one `constexpr`:

```cpp
#include "knx/product/commissioned_product.hpp"

enum class Port : uint16_t {
    RelayCommand = 0,
    RelayState   = 1,
};

inline constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port,
        semantics::SwitchCommand<Port::RelayCommand, "relay_command", "Relay Command">,
        semantics::SwitchState  <Port::RelayState,   "relay_state",   "Relay State">>(
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

Firmware binds callbacks to those ports and starts the runtime:

```cpp
// createTp1Physical() picks the bit-bang or TPUART backend from Kconfig;
// nothing above this line changes between the two.
auto physical = createTp1Physical(platform);

auto result = startCommissionedProduct(
    platform, kProduct,
    makeCommissionedBindings(kProduct)
        .onCommand<Port::RelayCommand>([&](bool on) { relayOn = on; })
        .provideState<Port::RelayState>([&] { return relayOn; })
        .onLifecycleChanged([](DeviceLifecycleState s) { /* drive the LED */ }),
    std::move(physical.value()));

if (result.isError()) return 1;
auto app = std::move(result.value());

for (;;) {
    app->loop();
    platform.delay(5);
}
```

Keep the product definition in its own header with no hardware or OS
dependencies: the same header is compiled by the firmware *and* by the host-side
exporter, which is what makes the runtime and the catalogue entry incapable of
disagreeing.

```bash
cmake --build build --target tp1_switch_ets_knxprod
```

Full walkthrough: [product authoring guide](docs/reference/product_authoring_guide.md).
Parameters, enumerations, bounds and the load procedure:
[ETS export guide](docs/reference/ets_export_guide.md).

## Examples

| Example | What it is for |
|---|---|
| [`esp_idf_switch/`](examples/esp_idf_switch/) | **Start here.** Relay out, state feedback, lifecycle LED, programming button, backend selection from Kconfig |
| [`esp_idf_temperature_sensor/`](examples/esp_idf_temperature_sensor/) | The smallest useful sensor |
| [`esp_idf_low_power_sensor/`](examples/esp_idf_low_power_sensor/) | Work-available / sleep-hint API driving light sleep |
| [`tp1_thermostat/`](examples/tp1_thermostat/) | The richest product definition — HVAC modes, setpoints, ETS parameters |
| [`tp1_line_coupler/`](examples/tp1_line_coupler/) | A coupler, through the same authoring path as an end device |
| [`ip_device/`](examples/ip_device/), [`ip_routing_device/`](examples/ip_routing_device/), [`tp1_ip_interface/`](examples/tp1_ip_interface/) | KNXnet/IP tunnelling, routing and interface |

See [examples/README.md](examples/README.md) for what each one deliberately
does *not* do, and why.

## Support matrix

"Implemented" below means the runtime path exists and host tests cover it.
**"Validated" is a separate column on purpose** — most of this stack has never
been exercised against real ETS or real hardware, and the difference matters far
more than the feature list.

| Area | Implemented | Validated against hardware / ETS |
|---|---|---|
| TP1 (timer-ISR bit-bang) | Yes, beta quality | **Yes** — ETS6 download and group communication on a real bus |
| TP1 (TPUART) | Yes | **No** — never run against an NCN5120-class transceiver |
| KNXnet/IP tunnelling (unicast) | Yes | **No** — host interop against `xknx` only, never a real ETS connection or gateway |
| KNXnet/IP routing (multicast) | Yes | **No** — same |
| KNX Data Secure | Yes; manual key provisioning | **Yes** — ETS secure download and secure group communication exercised on hardware |
| KNXnet/IP Secure Tunnelling | Yes, ECDH + `.knxkeys` import | **No** |
| KNX Secure Routing | Yes, opt-in; needs a pre-provisioned backbone key | **No** |
| Couplers / Router Object | Yes, opt-in via `configureRouterRole()` | **No** — the routing actions are untested; layer-2 ACK is computed but not enforced on TP1 |
| Serial-number commissioning | Yes | **No** |
| RF, Powerline | No, and not planned | — |

Two rows have hardware evidence behind them: bit-bang TP1 and Data Secure over
it. Every other row should be read as "written against the specification and
covered by host tests" — a real claim, but a much weaker one.

Key provisioning is manual across every secure path. That is inherent to the
KNX commissioning model — ETS distributes keys to devices; the stack provides
the runtime path.

For the service-by-service inventory, including what is missing and why, see
[docs/reference/knx_conformance_status.md](docs/reference/knx_conformance_status.md).

## Footprint and capacity

- CI enforces a **300 KB flash budget** on the shipping ESP-IDF configuration
  (TP1-only switch example), and fails if a gated-out KNXnet/IP symbol reaches
  the link at all.
- Each communication object costs about **288 bytes**, dominated by two payload
  buffers of `MAX_GROUP_OBJECT_PAYLOAD_BYTES` (16 by default — current value,
  and last transmitted value for send-on-change). RAM is what actually bounds a
  device on an MCU.
- The CO table is sized at compile time by the number of ports in the product
  definition, so overflowing it is impossible by construction. That number is
  the practical limit for any one product.

Table capacity comes from the memory segment map in
`include/knx/objects/table_segments.hpp`:

| Table | Entries |
|---|---|
| Group address (address table) | 256 |
| Association | 512 |
| Group object | 256 |

These are this stack's segment sizes, **not** KNX protocol ceilings — the
standard's own limits are higher, and the segment map is a handful of
`constexpr` values. Raising them is a local edit plus a RAM budget, so if a
product needs more, the constraint to check is the device's memory, not the
specification.

The ISR core and the TP1 wire path are allocation-free.

## Authoring surface

For a normal ETS-commissioned device, one header is the contract:

```cpp
#include "knx/product/commissioned_product.hpp"   // definition, bindings, runtime start
```

Plus `knx/knx.hpp`, `knx/types.hpp` and `knx/application/dpt.hpp` as support
headers. `knx/knx.hpp` intentionally exposes only the commissioned-product
surface.

Lower-level surfaces exist — `knx/product/endpoint.hpp` for custom runtime
assembly, and `bau::BusAccessUnit` for direct standards-facing control — but
they are advanced-only. New device code should not start there. The BAU is
grouped into facets (`bau.transmission()`, `bau.management()`, `bau.link()`);
see the [product authoring guide](docs/reference/product_authoring_guide.md).

## Building and testing

```bash
./build.sh test                                          # host build + full test suite
ctest --test-dir build -L tp1 --output-on-failure        # TP1 only
ctest --test-dir build -L tp1-virtual --output-on-failure # virtual TP1 bus
./build.sh test-all                                      # adds the xknx interop suite
```

The host suite covers protocol layers, interface objects, DPT codecs, product
runtime and commissioning flows. On top of it:

- a **virtual TP1 bus** reproduces timing scenarios that are hard to stage
  physically — collisions, missing ACKs, truncated frames;
- a **wire-level interop harness** drives the stack with real KNXnet/IP frames
  using `xknx` as an independent spec-following peer
  ([interop/README.md](interop/README.md));
- **CI builds the shipping ESP-IDF configuration** for both an xtensa and a
  RISC-V target, because the two SoC families declare the GPIO registers the
  bit-bang backend writes differently.

Virtual and host results do not substitute for HIL electrical and timing
validation on real transceivers.

### Requirements

- GCC 13+, Clang 17+, AppleClang 16+ or MSVC 19.38+ (C++23)
- CMake 3.21+
- ESP-IDF 5.3+ for ESP32 targets

### Installing as an ESP-IDF component

```bash
cd components
git clone <repository-url> KNstaX
idf.py menuconfig   # → KNX Stack Configuration
```

## ETS compatibility

The exporter emits KNX project **schema v23 (ETS 6)**, XSD-validated in CI. The
generated file is a `.knxprod.xml`, not a zipped `.knxprod` — pack it with
[Kaenx-Creator](https://github.com/OpenKNX/Kaenx-Creator) or a comparable tool
before importing.

## Documentation

| Document | For |
|---|---|
| [Product authoring guide](docs/reference/product_authoring_guide.md) | Defining a device: ports, parameters, persistence, bindings |
| [ETS export guide](docs/reference/ets_export_guide.md) | `product.hpp` → `.knxprod`, parameter UI, the load procedure |
| [Board bring-up guide](docs/reference/board_bringup_guide.md) | Getting a new TP1 board onto the bus without taking the line down |
| [Troubleshooting](docs/reference/troubleshooting.md) | ETS commissioning and bus-level failures, by symptom |
| [Conformance status](docs/reference/knx_conformance_status.md) | Service-by-service inventory; what is missing and why |
| [Architecture](ARCHITECTURE.md) | Internal structure and invariants |
| [Development guidelines](DEVELOPMENT_GUIDELINES.md) | Contributing |

## Contributing

Branch, add tests, keep `./build.sh test` green, and read
[DEVELOPMENT_GUIDELINES.md](DEVELOPMENT_GUIDELINES.md) first — it carries
architectural invariants that a PR will be measured against.

## License

Copyright (C) 2025-2026 Sami Mäkinen <sami.makinen@flou.io>.

KNstaX is free software: you may redistribute it and/or modify it under the
terms of the **GNU General Public License, version 3 or (at your option) any
later version**. It is distributed in the hope that it will be useful, but
**without any warranty**. See [LICENSE](LICENSE) for the full text.

### KNX specifications are not included

The KNX specification documents are **not distributed with this repository**.
They are KNX Association copyright and licensed per user. Source comments cite
them by volume, clause and version so that any claim can be checked against your
own copy, obtained from [my.knx.org](https://my.knx.org).

KNX® is a registered trademark of the KNX Association cvba. This project is not
affiliated with, endorsed by, or certified by the KNX Association.

## Acknowledgments

Inspired by the [OpenKNX](https://github.com/OpenKNX) project, and by the KNX
Association's specifications.
