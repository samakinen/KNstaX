# Low-Power TP1 Contact Sensor (ESP-IDF)

Demonstrates the owner-context low-power API, which only means anything on
target hardware.

One communication object:

| Object | DPT | Direction |
|---|---|---|
| Contact State | 1.019 Window/Door | contact GPIO → KNX |

DPT 1.019, not the generic 1.001 Switch: the sub-type is what tells a
visualisation that `1` means *open* rather than *on*.

## The API this example exists for

```cpp
app.setWorkAvailableCallback([]{ /* set a flag; never block */ });
const auto hint = app.ownerWorkHint();
if (!hint.hasImmediateWork()) {
    platform.delay(hint.maxSleepMs.value_or(1000u));   // idle task light-sleeps
}
```

`setWorkAvailableCallback()` may fire from the data-link RX context, so it must
stay non-blocking — set a flag and nothing more.

`ownerWorkHint().maxSleepMs` accounts for pending cyclic sends and deferred
transmissions, so honouring it keeps the heartbeat on time while still allowing
long idle periods. Ignoring it and sleeping a fixed interval works, but the
heartbeat drifts.

## Why light sleep and not deep sleep

This device **receives** as well as transmits, and the TP1 receive path is a GPIO
edge feeding a timer ISR. Deep sleep powers the timer down, so an incoming
telegram would be *missed* rather than merely delayed.

A deep-sleep design needs either:

- a transceiver that buffers frames while the host sleeps (a TPUART has its own
  MCU), or
- a device that only ever transmits and never has to answer a read — which means
  clearing the Read flag on every object, and accepting that ETS diagnostics
  cannot poll it.

Light sleep keeps the peripherals clocked and still removes the bulk of the idle
draw.

## Build

```bash
idf.py set-target esp32c6      # or esp32, esp32s3, esp32c3 …
idf.py build flash monitor
```

`sdkconfig.defaults` enables `CONFIG_PM_ENABLE` and tickless idle. Without
those the sleep-hint API still runs but nothing actually sleeps — the example
logs a warning saying so rather than pretending.

## Wiring

| Kconfig / constant | Default | Purpose |
|---|---|---|
| `CONFIG_KNX_TP1_BITBANG_TX_PIN` | 4 | transceiver TX |
| `CONFIG_KNX_TP1_BITBANG_RX_PIN` | 5 | transceiver RX |
| `kContactGpio` in `main.cpp` | 6 | dry contact, pulled up, any-edge interrupt |

The contact pin is registered as a light-sleep wake source, so a state change
wakes the device immediately instead of waiting out the sleep interval.

> See [`../../docs/reference/board_bringup_guide.md`](../../docs/reference/board_bringup_guide.md)
> before connecting to a live bus.

## Transmit policy

```cpp
policy.onChangeEnabled = true;         // suppress repeats of the same state
policy.cyclicIntervalMs = 3'600'000u;  // but prove liveness once an hour
```

Send-on-change plus a slow heartbeat is the right shape for a contact: a door
that has not moved in a week should not be generating traffic, but a receiver
still needs to distinguish "closed" from "dead".

Cyclic sends are time-based and inert without a clock, which is why the example
calls `app.setTimeSource(...)`.
