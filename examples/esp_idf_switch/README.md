# TP1 Switch Actuator (ESP-IDF)

The canonical onboarding example: the smallest complete ETS-commissionable KNX
device that drives real hardware.

Two communication objects:

| Object | DPT | Direction |
|---|---|---|
| Relay Command | 1.001 Switch | KNX write → relay GPIO |
| Relay State | 1.001 Switch | relay GPIO → KNX, and answers reads |

## Build

```bash
idf.py set-target esp32c6      # or esp32, esp32s3, esp32c3 …
idf.py menuconfig              # KNX Stack Configuration → pins, timing, backend
idf.py build flash monitor
```

`sdkconfig.defaults` selects the **software bit-bang** TP1 backend. Switch to
`CONFIG_KNX_TP1_TPUART` for an NCN5120-class transceiver — `main.cpp` handles
both and nothing above `createTp1Physical()` changes.

## Wiring

Defaults in `sdkconfig.defaults`; override in menuconfig:

| Kconfig | Default | Purpose |
|---|---|---|
| `CONFIG_KNX_TP1_BITBANG_TX_PIN` | 4 | transceiver TX |
| `CONFIG_KNX_TP1_BITBANG_RX_PIN` | 5 | transceiver RX |

The bus-health input is **off** in this example. `CONFIG_KNX_TP1_BITBANG_LINK_PIN`
defaults to 255, which disables the feature and makes the driver report
`LinkState::Unknown`. Set it to a real GPIO if your transceiver exposes a
health output (STKNX `KNX_OK`) — see
[Step 3 of the bring-up guide](../../docs/reference/board_bringup_guide.md),
and check the polarity, because an inverted one makes the device refuse to
transmit silently.

The application GPIOs are constants at the top of `main.cpp`: relay on 10,
status LED on 8, programming button on 9 (active low).

> **Read [`../../docs/reference/board_bringup_guide.md`](../../docs/reference/board_bringup_guide.md)
> before connecting to a live installation.** An inverted TX polarity holds the
> bus dominant and takes down the whole line, not just this device. Bring RX up
> and verify it before you transmit anything.

## Commissioning

1. Build the product file: `cmake --build <host-build> --target tp1_switch_ets_knxprod`
   — it is generated from `main/product.hpp`, the same header the firmware
   compiles.
2. Import it into ETS (via Kaenx-Creator).
3. Press the programming button; the status LED goes on and the lifecycle
   callback reports `Commissioning`.
4. Assign an individual address and group addresses in ETS and download.

The firmware never hardcodes a group address. Everything ETS owns is restored
from NVS on the next boot.

## What to look at in the source

| Concern | Where |
|---|---|
| Product definition (ports, DPTs, ETS identity) | `main/product.hpp` — no hardware, no OS |
| Backend selection from Kconfig | `createTp1Physical()` |
| KNX write → hardware | `.onCommand<Port::RelayCommand>` |
| Hardware → KNX read response | `.provideState<Port::RelayState>` |
| Commissioning state → LED | `.onLifecycleChanged` |
| Publishing feedback | `app.publish<Port::RelayState>()`, gated on `Operational` |

## Bumping the persistence schema

`PersistencePolicy::schemaVersion` in `product.hpp` is the layout version of the
persisted commissioned state. **Increment it whenever you add, remove or reorder
a port or a parameter.** The stored layout is positional, so state written by a
different version is discarded on boot rather than reinterpreted — a device that
silently comes up mis-parameterised is far worse than one that asks ETS to
download again.
