# KNstaX

A KNX System-7 protocol stack in C++23 for building ETS-commissionable devices
on ESP32 and on bare-metal Cortex-M, with a host build for development and
testing.

You declare your device once — its communication objects, their datapoint
types, its ETS parameters — as a `constexpr` definition. That single
declaration is both the runtime model the firmware executes and the source the
ETS product file is generated from, so the device and its ETS entry cannot
drift apart.

Target profile: **mask version 07B0 (System B, TP1)**; mask 57B0 for a KNX IP
end device.

## Status

KNstaX runs real devices on a live KNX bus, commissioned with ETS6. It is **not
certified** and has never been run through the KNX Conformance Test Tool (see
[Certification](#certification) below).

What has been proven on hardware against ETS6:

| Hardware | Bus interface | Proven with ETS6 |
|---|---|---|
| ESP32-C6 ([Habinari](https://github.com/samakinen/habinari-firmware) room controller) | Software bit-bang TP1 (STKNX transceiver) | Discovery, individual address, application download, group communication; **KNX Data Secure**: secure commissioning, secured download, secured group communication |
| GD32F103 (STM32F103-compatible Cortex-M3, 128 KiB flash / 20 KiB RAM; [H8i8o](https://github.com/samakinen/h8i8o) 8-in/8-out module) | Siemens TP-UART2 | Product import, individual address, application and parameter download, group communication, configuration kept in flash across restarts and power cuts; **KNX Data Secure**: secure commissioning with the device certificate, secured download, secured group communication in both directions |
| ESP32-C6 (Habinari, KNX IP variant) | KNXnet/IP routing over Wi-Fi | Discovery and commissioning as a KNX IP device on an unsecured backbone |

KNXnet/IP tunnelling, KNX IP Secure tunnelling and Data Secure over IP are
tested on the wire against [`xknx`](interop/README.md) in CI, but not against
ETS or a commercial gateway. KNX Secure Routing and the coupler routing actions
rest on host tests only. Expect to debug those on first contact. The
[support matrix](#support-matrix) splits it row by row, and the
[conformance status](docs/reference/knx_conformance_status.md) does so service
by service.

The bit-bang TP1 backend works but is timing-sensitive: commission a new board
with the [bring-up guide](docs/reference/board_bringup_guide.md) and a scope.

RF and Powerline are not implemented and not planned — there is no hardware to
develop or validate them against.

## Features

- **ETS-driven commissioning.** Individual address, group addresses and
  parameters are owned by ETS and restored from persistent storage (NVS on
  ESP-IDF, a flash log on bare metal) at boot. Firmware never hardcodes a group
  address.
- **Communication flags that are behaviour, not documentation.** All six KNX
  flags (C/R/W/T/U/I) and per-object priority are enforced by the inbound
  dispatch path before a value is touched or a read is answered, and they are
  exported to the product file, so what ETS shows is what the device does.
- **Datapoint types with the right sub-type.** Main types 1–20, 232, 242, 243
  and 244, across 84 catalogue entries. The named helpers pick the
  KNX-standard sub-type per signal — a contact reported as 1.001 instead of
  1.019 works on the wire but shows up in ETS as a switch.
- **Two TP1 physical layers** behind a medium-neutral MAC engine: a
  timer-ISR software bit-bang backend, and a **TP-UART2** backend over a small
  C UART HAL that a board implements in a few hundred lines. A host-side
  TP-UART2 simulator runs that backend's tests without a chip.
- **Small targets.** Kconfig profiles select features and size every table
  and queue. A 60-object TP1 device fits a 128 KiB / 20 KiB Cortex-M3 with
  room for the application, and a size gate in CI keeps it there.
- **KNX Data Secure** through the Security Interface Object, with
  fixed-capacity key tables (nothing allocates during a secure download),
  persistent sequence numbers, and helpers for a product to generate its own
  Factory Tool Key (FDSK) and print the device certificate ETS asks for. The
  AES block is a configuration choice: a portable software AES-128 (no crypto
  library; the bare-metal default), mbedTLS, or the ESP32's AES peripheral.
- **KNXnet/IP** tunnelling and routing, Secure Tunnelling with ECDH, and
  opt-in Secure Routing — compiled out entirely on a TP1-only build. CI fails
  if a gated-out medium so much as compiles.
- **Couplers**: the routing algorithm of 03/03/03 §2.4.2.4 in full, built
  through the same call as an end device.

## Quick start

**ESP32 with a bit-bang TP1 transceiver** — the switch example drives a real
transceiver:

```bash
cd examples/esp_idf_switch
idf.py set-target esp32c6      # or esp32, esp32s3, esp32c3 …
idf.py menuconfig              # KNX Stack Configuration → pins, timing, backend
idf.py build flash monitor
```

> **Read the [board bring-up guide](docs/reference/board_bringup_guide.md) before
> connecting to a live installation.** An inverted TX polarity holds the bus
> dominant and takes down the whole line, not just your device.

**Bare-metal Cortex-M with a TP-UART2** — start from the GD32F1 reference
port, [ports/gd32f1](ports/gd32f1/README.md): start-up code, linker script,
UART and flash HALs, a one-channel example firmware, and a guide to porting it
to another MCU:

```bash
cmake -S ports/gd32f1 -B build/gd32f1 -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake
cmake --build build/gd32f1
```

**No hardware** — build and run the test suite on the host:

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
// createTp1Physical() builds the bit-bang TP1 physical layer (see the
// esp_idf_switch example).
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
dependencies: the same header is compiled by the firmware *and* by the
host-side exporter, which is what makes the runtime and the ETS entry incapable
of disagreeing. `knx_ets_product()` (in `cmake/knx_ets_product.cmake`) builds
a firmware's product file with the firmware and from its configuration, checks
it against a committed product lock (a changed product needs a new application
version, or ETS refuses it) and packs it where ETS is installed; the host build
generates the switch example's:

```bash
cmake --build build --target tp1_switch_ets_knxprod
```

Full walkthrough: [product authoring guide](docs/reference/product_authoring_guide.md).
Parameters, enumerations, bounds and the load procedure:
[ETS export guide](docs/reference/ets_export_guide.md).

0x00FA is the example manufacturer ID used throughout this repository; it
belongs to the KNX Association. A product you distribute needs its own.

## Examples

| Example | What it is for |
|---|---|
| [`esp_idf_switch/`](examples/esp_idf_switch/) | **Start here.** Relay out, state feedback, lifecycle LED, programming button, backend selection from Kconfig |
| [`esp_idf_temperature_sensor/`](examples/esp_idf_temperature_sensor/) | The smallest useful sensor |
| [`esp_idf_low_power_sensor/`](examples/esp_idf_low_power_sensor/) | Work-available / sleep-hint API driving light sleep |
| [`tp1_thermostat/`](examples/tp1_thermostat/) | The richest product definition — HVAC modes, setpoints, ETS parameters |
| [`tp1_line_coupler/`](examples/tp1_line_coupler/) | A coupler, through the same authoring path as an end device |
| [`ip_device/`](examples/ip_device/), [`ip_routing_device/`](examples/ip_routing_device/), [`tp1_ip_interface/`](examples/tp1_ip_interface/) | KNXnet/IP tunnelling, routing and interface |
| [`ports/gd32f1/example/`](ports/gd32f1/example/) | Bare-metal GD32F103 / STM32F103 with a TP-UART2: the start-up order every Cortex-M product needs |

See [examples/README.md](examples/README.md) for what each one deliberately
does *not* do, and why.

### Products built on KNstaX

Two complete, open-source firmwares show the stack in a real product, including
Data Secure, persistence and power-fail handling:

- **[H8i8o](https://github.com/samakinen/h8i8o)** — replacement firmware for
  the H8i8o 8-input/8-output module: GD32F103, TP-UART2, bare metal, Data
  Secure with a self-generated FDSK. The reference for a Cortex-M product
  (board support, flash layout, RTT diagnostics, product file build).
- **[Habinari](https://github.com/samakinen/habinari-firmware)** — a room
  climate sensor and HVAC controller on the ESP32-C6: bit-bang TP1 or KNXnet/IP
  routing over Wi-Fi from one product definition, Data Secure, and Modbus/MQTT
  alongside KNX. The reference for an ESP-IDF product.

## Support matrix

"Implemented" means the runtime path exists and host tests cover it.
"Validated" means it has worked against real ETS on real hardware — a much
stronger claim, kept in its own column on purpose.

| Area | Implemented | Validated against hardware / ETS6 |
|---|---|---|
| TP1, bit-bang (ESP32) | Yes | **Yes** — download and group communication on a live bus |
| TP1, TP-UART2 (bare-metal Cortex-M) | Yes | **Yes** — download and group communication on a live bus (GD32F103) |
| TP1, NCN5120 / NCN5130 | No — the TP-UART backend is structured for the family; the differences are not coded | — |
| Bus power-fail warning (TP-UART2 `SAVE`, bit-bang `KNX_OK`) | Yes, debounced; the product calls `requestPowerFailSave()` to save the Data Secure sequence state | **Partly** — the warning pin was read and timed on a GD32F103 (about 120 ms of supply after it); the save itself is host-tested |
| Flash persistence (`FlashLogPersistence`) | Yes | **Yes** — power cut at random moments on a GD32F103, every record whole |
| KNX Data Secure | Yes — fixed-capacity key tables, persistent sequence numbers, FDSK/device-certificate helpers | **Yes** on both TP1 backends (ESP32-C6 bit-bang, GD32F103 TP-UART2) — secure commissioning, secured download, secured group communication |
| KNXnet/IP routing (multicast), KNX IP medium | Yes | **Yes** — commissioned from ETS6 on an ESP32-C6 over Wi-Fi (unsecured backbone); also tested against `xknx` in CI |
| KNXnet/IP tunnelling (unicast) | Yes, client and interface server | **No** — tested on the wire against `xknx` in CI |
| KNXnet/IP Secure Tunnelling | Yes, ECDH + `.knxkeys` import | **No** — tested on the wire against `xknx` in CI |
| KNXnet/IP Device Management | **No** — a secured IP backbone cannot be commissioned without it | — |
| KNX Secure Routing | Yes, opt-in; needs a provisioned backbone key | **No** |
| Couplers / Router Object | Yes, opt-in via `configureRouterRole()` | **No** — the routing actions have not run on a real two-line installation |
| Serial-number commissioning | Yes | **No** |
| RF, Powerline | No, and not planned | — |

| Platform | Status |
|---|---|
| ESP-IDF 5.3+ (ESP32 family) | Validated on an ESP32-C6 with the bit-bang backend |
| Bare-metal Cortex-M, single-threaded (`CooperativePlatform`) | Validated on a GD32F103 (Cortex-M3) with a TP-UART2; Cortex-M0+ and M4 are built and size-checked in CI |
| Linux / macOS / Windows host | Development and tests; not a product target |

### Data Secure in a product

ETS distributes the keys; the device needs a **Factory Default Setup Key
(FDSK)** unique to it and a **device certificate** (serial number + FDSK) that
the installer enters in ETS. The FDSK is per device, so it is never in the
firmware image: the product supplies it. On a part without a hardware random
number generator, `security/seeded_random.hpp` seeds an AES-CTR generator from
whatever noise the board has, and `security/device_certificate.hpp` formats
the certificate. The H8i8o firmware shows the whole flow — key generation at
first start, storage that survives a factory reset, and the certificate in the
boot log and on a label.

Budget for it: on the Cortex-M3 reference device Data Secure adds about 20 KB
of flash and 2.3 KB of RAM (see [Footprint](#footprint-and-capacity)), and a
secured telegram needs about 1.6 KiB of stack in the CCM path, so plan a stack
of about 5 KiB on bare metal. Set `CONFIG_KNX_MAX_APDU_LENGTH=55` on a
TP-UART2 board, the largest APDU a 64-octet frame carries and what ETS needs to
write a key table entry in one frame.

The [conformance status](docs/reference/knx_conformance_status.md#knx-secure)
describes the access policies, sequence-number persistence and what remains
open.

## Footprint and capacity

The size gate (`test/baremetal/size_gate/`) builds four reference devices in
CI, each a 60-object TP1 end device linked with LTO, runs it under QEMU through
an ETS download and group traffic with the heap locked after start-up, and
fails if flash or RAM exceeds its budget in `budgets/`:

| Reference device | KNstaX flash | KNstaX RAM (static + heap) |
|---|---:|---:|
| `tp1-small`, Cortex-M3 | 86.7 KB | 7.8 KB |
| `tp1-small`, Cortex-M0+ | 89.6 KB | 0.3 KB static (linked, not run) |
| `tp1-standard`, Cortex-M4 | 104.8 KB | 12.4 KB |
| `tp1-small` + Data Secure (software AES, 55-octet APDU), Cortex-M3 | 107.2 KB | 10.1 KB |

Budgets only move down unless an increase is justified; a pull request shows
the change against its base. The plain Cortex-M3 budget also checks that the
whole device — KNstaX plus reservations for application, board support,
persistence and stack — fits a 128 KiB / 20 KiB part. For a real Data Secure
product on that part, see the H8i8o: its complete image is about 118 KB of a
120 KiB image region.

- A group object costs **12 bytes** plus its value in a shared pool, about
  16 bytes in all; its datapoint type is read from the product's port table in
  flash. Callbacks bound to ports are stored inline with a capture of
  `KNX_CALLBACK_CAPTURE_BYTES`.
- The object table is sized at compile time by the number of ports in the
  product definition, so overflowing it is impossible by construction.
- The ISR core and the TP1 wire path are allocation-free.
- `scripts/ci/knstax_size_budget.py` checks an ESP-IDF linker map against a
  flash budget and fails if a gated-out KNXnet/IP symbol reaches the link. Run
  it by hand: CI has no ESP-IDF build.

Table capacities are Kconfig options, set per profile:

| Table | `tp1-small` | `tp1-standard` | `full` and host |
|---|---:|---:|---:|
| Group address (address table) | 128 | 255 | 256 |
| Association | 160 | 400 | 512 |

These are this stack's defaults, **not** KNX protocol ceilings. A product
overrides them in its own defconfig fragment; the constraint to check is the
device's memory, not the specification.
[Small-target profile](docs/design/small-target-profile.md) has the details.

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
they are for advanced use; new device code should not start there. The BAU is
grouped into facets (`bau.transmission()`, `bau.management()`, `bau.link()`);
see the [product authoring guide](docs/reference/product_authoring_guide.md).

## Building and testing

```bash
./build.sh test                                           # host build + full test suite
ctest --test-dir build -L tp1 --output-on-failure         # TP1 only
ctest --test-dir build -L tp1-virtual --output-on-failure # virtual TP1 bus
./build.sh test-all                                       # adds the xknx interop suite
```

The host suite covers protocol layers, interface objects, DPT codecs, product
runtime and commissioning flows. On top of it:

- a **virtual TP1 bus** reproduces timing scenarios that are hard to stage
  physically — collisions, missing ACKs, truncated frames;
- a **TP-UART2 chip simulator** on the virtual bus runs the TP-UART backend's
  tests, including the acknowledge deadline, without a chip;
- a **wire-level interop harness** drives the stack with real KNXnet/IP frames
  using `xknx` as an independent peer ([interop/README.md](interop/README.md)).

**CI** (`.github/workflows/ci.yml`) runs the host suite with KNX Secure on and
off and in the `host-small` and `host-minimal` profiles, again under
AddressSanitizer and UndefinedBehaviorSanitizer; builds the library alone in
the TP1-only, KNXnet/IP-only and knxip shapes with warnings as errors; runs
the interop suite and the size gate; and builds the GD32F1 reference port.
**CI does not build for ESP-IDF.** After touching the bit-bang backend, build
an ESP-IDF example by hand for both an xtensa and a RISC-V target: the two SoC
families declare its GPIO registers differently.

Host and virtual-bus results do not replace electrical and timing validation
on real transceivers.

To repeat a CI job locally, including the size gate with its QEMU run, use the
image with the CI runner's packages:

```bash
docker build -t knstax-ci -f tools/docker/Dockerfile.ci tools/docker
docker run --rm -v "$PWD":/src -w /src knstax-ci \
    sh test/baremetal/size_gate/run_gate.sh /tmp/gate tp1-small cortex-m3
```

### Choosing what is built

What a standalone CMake build contains is a **profile**: a defconfig fragment
under `configs/profiles/` over the `Kconfig` schema. A host build uses
`host-test` (every medium and tool on); `-DKNX_PLATFORM=baremetal` uses
`tp1-small`. A product picks its own before it adds KNstaX, and may override
single symbols in a fragment of its own:

```cmake
include(${KNSTAX_DIR}/cmake/knx_config.cmake)
knx_configure(PROFILE tp1-small
              OVERRIDES ${CMAKE_CURRENT_SOURCE_DIR}/knx.defconfig)   # e.g. CONFIG_KNX_LOG_LEVEL=1
add_subdirectory(${KNSTAX_DIR} knstax)
```

Profiles are `host-test`, `host-small`, `host-minimal`, `full`, `tp1-small` and
`tp1-standard`; `PROFILE` also takes the path of a fragment. A fragment is
`CONFIG_KNX_X=value` lines; an unknown symbol or a value Kconfig rejects stops
the configure step. The resolved configuration is written to
`<build>/knx-config/knx.config`, and `cmake --build <build> --target
knx_menuconfig` edits it. Python 3 is needed at configure time.

Optional code is switched by Kconfig symbols: `KNX_ENABLE_COUPLER` (the router
role), `KNX_ENABLE_SECURITY` (Data Secure and the Security Interface Object;
it selects the extended property and memory services, function properties and
group object diagnostics, which ETS needs with a secure device),
`KNX_ENABLE_GO_DIAGNOSTICS`, `KNX_ENABLE_FUNCTION_PROPERTIES`,
`KNX_ENABLE_PROPERTY_EXT`, `KNX_ENABLE_MEMORY_EXT`,
`KNX_ENABLE_NETWORK_PARAMETER`, the datapoint families `KNX_DPT_FLOAT`,
`_TIME`, `_STRING`, `_COLOUR`, `_ACCESS`, and `KNX_ENABLE_DPT_CATALOGUE`. All
default to on except the coupler and Security. A feature that is off is not
compiled and not exposed to ETS, and a product that uses it fails to build.
`-DKNX_HOST_PROFILE=host-minimal` runs the host suite with the `tp1-small`
feature set.

The older CMake options (`-DKNX_FEATURE_NETIP=OFF`, `-DKNX_SECURE_ENABLED=OFF`
and the like) still work as overrides over the profile, but are deprecated.

### Requirements

- GCC 13+, Clang 17+, AppleClang 16+ or MSVC 19.38+ (C++23)
- CMake 3.21+, Python 3
- ESP-IDF 5.3+ for ESP32 targets
- Bare-metal Cortex-M: `-DKNX_PLATFORM=baremetal` with
  `cmake/toolchains/arm-none-eabi.cmake` and GCC 13+ for `arm-none-eabi`. The
  board provides three C interfaces: a UART (`include/knx/physical/uart_hal.h`),
  flash for persistence, and a millisecond tick.
  [ports/gd32f1](ports/gd32f1/README.md) implements them and explains how to
  port them; [Cortex-M and TP-UART2](docs/design/cortex-m-tpuart2-support.md)
  has the background.

### Using it as an ESP-IDF component

Add it under your project's `components/`, preferably as a git submodule so
the stack version is pinned with your firmware:

```bash
git submodule add https://github.com/samakinen/KNstaX.git components/KNstaX
idf.py menuconfig   # → KNX Stack Configuration
```

## ETS compatibility

The exporter emits KNX project **schema v23 (ETS 6)**, XSD-validated in CI. The
generated file is a `.knxprod.xml`, not a signed `.knxprod`: sign and pack it
with [OpenKNXproducer](https://github.com/OpenKNX/OpenKNXproducer) or
[Kaenx-Creator](https://github.com/OpenKNX/Kaenx-Creator) before importing it
into ETS.

## Certification

No device built on this stack has been certified, and the KNX Conformance
Test Tool has never been run against it. Behaviour is written against the
specification texts and cited by clause in the source; the Data Secure CCM
framing is checked against the worked examples in 03/03/07 Annex C; KNXnet/IP
is exercised on the wire against `xknx`; and the paths in the
[status table](#status) have worked with ETS6. That is evidence of being
faithful to the specification, not of conformance — only the KNX Association
can certify a product. The
[conformance status](docs/reference/knx_conformance_status.md#known-gaps-for-certification)
lists what stands between the stack and a certifiable device.

## Documentation

| Document | For |
|---|---|
| [Product authoring guide](docs/reference/product_authoring_guide.md) | Defining a device: ports, parameters, persistence, bindings |
| [ETS export guide](docs/reference/ets_export_guide.md) | Product definition → ETS product file, parameter UI, the load procedure |
| [Board bring-up guide](docs/reference/board_bringup_guide.md) | Getting a new bit-bang TP1 board onto the bus without taking the line down |
| [GD32F1 reference port](ports/gd32f1/README.md) | Bare-metal Cortex-M with a TP-UART2, and porting it to another MCU |
| [Troubleshooting](docs/reference/troubleshooting.md) | ETS commissioning and bus-level failures, by symptom |
| [Conformance status](docs/reference/knx_conformance_status.md) | Service-by-service inventory; what is missing and why |
| [Architecture](ARCHITECTURE.md) | Internal structure and invariants |
| [Quick reference](QUICK_REFERENCE.md) | Commands and APIs on one page |
| [Development guidelines](DEVELOPMENT_GUIDELINES.md) | Contributing |

[Design records](docs/design/README.md) explain why the bare-metal platform,
the small-target profile, the parameter model, the software AES and Data Secure
on small parts are built the way they are, with the measurements taken at the
time. They are history; the documents above describe the current state.

## Contributing

Branch, add tests, keep `./build.sh test` green, and read
[DEVELOPMENT_GUIDELINES.md](DEVELOPMENT_GUIDELINES.md) first — it carries
architectural invariants that a pull request will be measured against.

## Licence

Copyright (C) 2025-2026 Sami Mäkinen <sami.makinen@flou.io>.

KNstaX is free software: you may redistribute it and/or modify it under the
terms of the **GNU General Public License, version 3 or (at your option) any
later version**. It is distributed in the hope that it will be useful, but
**without any warranty**. See [LICENSE](LICENSE) for the full text.

### KNX specifications are not included

The KNX specification documents are **not distributed with this repository**.
They are KNX Association copyright and licensed per user. Source comments cite
them by volume, clause and version so that any claim can be checked against
your own copy, obtained from [my.knx.org](https://my.knx.org).

KNX® is a registered trademark of the KNX Association cvba. This project is not
affiliated with, endorsed by, or certified by the KNX Association.

## Acknowledgements

Inspired by the [OpenKNX](https://github.com/OpenKNX) project, and by the KNX
Association's specifications.
