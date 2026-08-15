# TP1 Temperature Sensor (ESP-IDF)

The smallest useful sensor: one transmitting communication object, published on
a fixed interval once the device is operational.

| Object | DPT | Direction |
|---|---|---|
| Temperature | 9.001 Temperature (°C) | sensor → KNX, and answers reads |

If the switch example is where you learn the authoring path, this is where you
see how little is left once the hardware gets simple.

## Build

```bash
idf.py set-target esp32c6      # or esp32, esp32s3, esp32c3 …
idf.py menuconfig              # KNX Stack Configuration → pins, timing, backend
idf.py build flash monitor
```

`sdkconfig.defaults` selects the **TPUART** backend (NCN5120/NCN5121) on UART 1,
TX 17 / RX 16. Switch to `CONFIG_KNX_TP1_BITBANG` for a software backend —
`createTp1Physical()` handles both and nothing above it changes.

> **Read [`../../docs/reference/board_bringup_guide.md`](../../docs/reference/board_bringup_guide.md)
> before connecting to a live installation**, particularly if you switch to the
> bit-bang backend. An inverted TX polarity holds the bus dominant and takes
> down the whole line, not just this device.

The example builds from inside this repository checkout; its `CMakeLists.txt`
prefers the repository root at `../..`, and falls back to a
`components/KNstaX` layout if you lift the app into a separate ESP-IDF
workspace.

## What to look at in the source

| Concern | Where |
|---|---|
| Product definition (port, DPT, ETS identity) | `kTemperatureSensorProduct` in `main/main.cpp` |
| Backend selection from Kconfig | `createTp1Physical()` |
| Hardware → KNX read response | `.provideState<SensorPort::Temperature>` |
| Periodic publish, gated on `Operational` | the `for(;;)` loop |

Two details worth copying:

- **`provideState` and `publish` are both present, and both are needed.**
  `publish()` pushes a new value onto the bus; `provideState` is what answers an
  `A_GroupValue_Read` from ETS or another device in between publishes. A sensor
  with only the first is silent to anyone who asks.
- **Publishing is gated on `lifecycleState() == Operational`.** The gate is
  there because publishing early is *silent*, not because it errors: a port
  with no group address linked to it succeeds and emits nothing. Without the
  gate an uncommissioned device looks like it is working. `isPortLinked()`
  answers the same question per port, which is how you report the datapoints an
  ETS project left unused.

## Making it a real sensor

`readBoardTemperatureC()` returns a constant. Replace it with an I²C/SPI/ADC
read — that is the only change the hardware requires.

Two things to reconsider when you do:

- **The fixed 10-second interval is a placeholder.** A real temperature sensor
  usually wants send-on-change with a threshold plus a slow heartbeat, which
  costs a fraction of the bus traffic and still proves liveness. See the
  transmit policy in
  [`../esp_idf_low_power_sensor/`](../esp_idf_low_power_sensor/).
- **Decide what a failed read sends.** Publishing a stale or default value is
  indistinguishable on the bus from a working sensor reporting that value.

## Commissioning

The firmware hardcodes no group address. Assign the individual address and group
addresses in ETS and download; everything ETS owns is restored from NVS on the
next boot.

`PersistencePolicy::schemaVersion` is the layout version of that persisted
state — **increment it whenever you add, remove or reorder a port or a
parameter**, or the device will misread what the previous firmware wrote.
