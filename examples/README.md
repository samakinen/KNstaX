# Examples

Every example uses the same authoring path — a `constexpr` product definition
plus `startCommissionedProduct(...)` — so what differs between them is the
hardware and the KNX signals, not the structure.

## Start here: real hardware (ESP-IDF)

These drive an actual TP1 transceiver. If your question is "how do I wire this
to my board", the answer is in one of these three.

| Example | What it shows |
|---|---|
| [`esp_idf_switch/`](esp_idf_switch/) | **The canonical onboarding example.** Relay output, state feedback, lifecycle-driven indicator LED, programming button, and TP1 backend selection from Kconfig. |
| [`esp_idf_temperature_sensor/`](esp_idf_temperature_sensor/) | The smallest useful sensor: one transmitting object with a cyclic publish. |
| [`esp_idf_low_power_sensor/`](esp_idf_low_power_sensor/) | `setWorkAvailableCallback()` + `ownerWorkHint()` driving automatic light sleep, plus send-on-change with a slow heartbeat. Only meaningful on target. |

Each is a self-contained ESP-IDF project:

```bash
cd examples/esp_idf_switch
idf.py set-target esp32c6      # or esp32, esp32s3, esp32c3 …
idf.py menuconfig              # KNX Stack Configuration → pins, timing, backend
idf.py build flash monitor
```

**Read [`../docs/reference/board_bringup_guide.md`](../docs/reference/board_bringup_guide.md)
before connecting to a live installation.** An inverted TX polarity holds the
bus dominant and takes down the whole line, not just your device.

CI builds all three for both an xtensa and a RISC-V target, because the two SoC
families declare the GPIO registers the bitbang backend writes differently.

## Host examples

Built by `knx_sdk_example_contracts` and gated by `knx_sdk_release_gate`.

| Example | Why it is a host example |
|---|---|
| [`tp1_thermostat/`](tp1_thermostat/) | The richest product definition in the tree — HVAC mode, setpoints, ETS parameters. Kept host-buildable so CI compiles the full product path on every push. |
| [`ip_device/`](ip_device/) | KNXnet/IP tunnelling. Genuinely host-oriented: the medium is a network socket. |
| [`tp1_line_coupler/`](tp1_line_coupler/) | A **coupler**: two TP1 ports with spec routing between them, built through the same `startCommissionedProduct` call as an end device. The one example where `CouplerOptions` replaces a single medium backend. |
| [`ip_routing_device/`](ip_routing_device/) | An **end device** reached over KNXnet/IP routing multicast via `IpRoutingOptions`. "Routing" is the transport mode here, not forwarding between subnetworks — for that, see `tp1_line_coupler/`. |
| [`tp1_ip_interface/`](tp1_ip_interface/) | KNXnet/IP interface via `IpTunnelingOptions`. |

```bash
cmake -S . -B build_examples -DKNX_BUILD_EXAMPLES=ON
cmake --build build_examples -j
```

`./build.sh test` enables the example contracts by default, so this lane stays
exercised during routine host validation.

## Advanced references

- [`knxnetip_routing_sniffer.cpp`](knxnetip_routing_sniffer.cpp) — passive
  routing observer. Uses the raw `bau.link()` facet, which bypasses address
  filtering and the group-object runtime entirely; that is what a sniffer wants
  and what device firmware must not do.
- [`provider_binding_example.cpp`](provider_binding_example.cpp) — advanced
  binding surface. In-tree for reference; not part of the release gate.

### Sniffing the integration tests

Some integration tests derive their multicast group and port from the PID to
reduce cross-test interference. To watch a specific test, pin them:

```bash
export KNX_TEST_MCAST_GROUP=239.255.0.42
export KNX_TEST_MCAST_PORT=3671   # knx::netip::config::kDefaultPort
export KNX_TEST_MCAST_IFACE=127.0.0.1

./build_examples/knxnetip_routing_sniffer \
    --group "$KNX_TEST_MCAST_GROUP" --port "$KNX_TEST_MCAST_PORT" \
    --iface "$KNX_TEST_MCAST_IFACE" --count 1 --timeout-ms 2000 &
ctest --test-dir build_examples -R '^test_netip_routing_multicast$'
```

`--count 0` runs forever.

## What every example deliberately does not do

- **No hardcoded group addresses.** ETS assigns them; firmware never guesses.
  Ports are referenced by their logical `Port::` enumerator.
- **No commissioning plumbing in firmware.** Individual address, group
  addresses and parameters are owned by ETS and restored from persistence by
  KNstaX at boot.
- **No hardware in `product.hpp`.** The product definition stays free of
  platform and OS dependencies, because the same header is compiled by the
  firmware *and* by the host-side `.knxprod` exporter. That is what makes the
  runtime and the ETS catalogue entry incapable of drifting apart.

## Generating the ETS product file

`knx_commissioned_product()` turns a `product.hpp` into a `.knxprod.xml` at
build time:

```bash
cmake --build build --target tp1_switch_ets_knxprod
```

See [`../docs/reference/ets_export_guide.md`](../docs/reference/ets_export_guide.md)
for parameter groups, enumerations, bounds and the download procedure.
