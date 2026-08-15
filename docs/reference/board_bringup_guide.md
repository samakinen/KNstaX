# Board bring-up guide (TP1)

Getting a new TP1 board to talk to the bus. The bitbang backend has about a
dozen Kconfig knobs and getting them wrong produces the same symptom — silence —
regardless of which one is wrong, so this guide is ordered to eliminate causes
one at a time rather than by topic.

If you are using a TPUART transceiver (NCN5120/NCN5121), most of this does not
apply: select `KNX_TP1_TPUART`, set the UART pins, and skip to
[Step 5](#step-5-first-telegram).

## Step 0: what you need

- A TP1 transceiver (STKNX, NCN5120, or equivalent) wired to the ESP32.
- A KNX power supply and at least one other device or a bus monitor.
- A logic analyser or scope on TX and RX. **You will not get through bring-up
  without one.** The failure modes below are all timing or polarity problems
  that are invisible from firmware logs.

## Step 1: pins and polarity

```
CONFIG_KNX_TP1_BITBANG_TX_PIN
CONFIG_KNX_TP1_BITBANG_RX_PIN
CONFIG_KNX_TP1_BITBANG_TX_DOMINANT_HIGH
CONFIG_KNX_TP1_BITBANG_RX_DOMINANT_HIGH
CONFIG_KNX_TP1_BITBANG_RX_PULLUP
```

TP1 is a dominant/recessive bus: a logical `0` is *dominant* (actively driven),
`1` is recessive (idle). What your GPIO must do to assert dominant depends on
the transceiver and on whether there is an inverting buffer in between.

**Determine polarity empirically, not from the datasheet block diagram.** Put a
scope on RX with the bus idle:

- Idle line reads **high** → `RX_DOMINANT_HIGH=n` (a dominant bit pulls it low).
- Idle line reads **low** → `RX_DOMINANT_HIGH=y`.

Then have another device send something. The start bit is dominant; confirm the
first edge moves in the direction you just configured as dominant.

Get TX polarity from the transceiver's TX input description. Getting it
backwards means you hold the bus dominant permanently the moment the driver
initialises, which takes the whole line down — check TX idles recessive before
connecting to a live installation.

`RX_PULLUP` is only needed when the transceiver's RX output is open-drain.

## Step 2: bit timing

```
CONFIG_KNX_TP1_BITBANG_SERIAL_BIT_TIME_US      default 104
CONFIG_KNX_TP1_BITBANG_ZERO_ACTIVE_US          default 35
CONFIG_KNX_TP1_BITBANG_ZERO_EQUALIZATION_US    default 69
CONFIG_KNX_TP1_BITBANG_SAMPLE_OFFSET_US        default 52
CONFIG_KNX_TP1_BITBANG_START_BIT_VALIDATION_US default 15
```

TP1-256 runs at 9600 bit/s → 104 µs per bit. The defaults are the KNX standard
values and **you should not need to change the first three.** They describe the
bus, not your board.

The two worth attention:

- **`SAMPLE_OFFSET_US`** — where in the 104 µs bit cell the receiver samples.

  The thing to understand first: a bit is decoded from a **sticky flag set by
  the RX edge interrupt**, not by reading the bus level. So the sample point
  does *not* have to land inside the 35 µs dominant pulse. It only has to fall
  after this cell's edge and before the next one.

  That makes the margin two-sided: tolerance against a *late* sample is
  (bit time − offset), and against an *early* one it is the offset itself.
  Mid-cell — 52 µs of 104 — splits them evenly, which is why it is the default.
  Pushing the sample later does not "bias toward the settled half of the bit";
  it spends late margin, and an earlier default of 78 µs left only 26 µs, which
  measured interrupt latency on a busy ESP32-C6 can exceed. The failure is
  silent: bits shift into the wrong position rather than erroring.

  If your transceiver's RX path is slow (opto-isolated designs often are),
  measure the bus-edge-to-RX-pin delay and shift this to re-centre — but keep
  both margins in view rather than only adding delay. Symptom of getting it
  wrong: frames received with occasional bit errors that look random.

- **`START_BIT_VALIDATION_US`** — how long the line must stay dominant after the
  first falling edge before the driver accepts a real start bit. The validation
  timer fires at this offset and reads the bus level; if the line has already
  gone recessive, the edge is discarded as a glitch. 15–20 µs is the
  recommended band. It **must stay below both the zero-active time (35 µs) and
  the sample offset** — raising it past either breaks decoding rather than
  hardening it. Raise it within that band on electrically noisy installations;
  lower it only if you are missing the start of frames.

## Step 3: link health signal (optional but recommended)

```
CONFIG_KNX_TP1_BITBANG_LINK_PIN
CONFIG_KNX_TP1_BITBANG_LINK_ACTIVE_HIGH
CONFIG_KNX_TP1_BITBANG_LINK_PULLUP
CONFIG_KNX_TP1_BITBANG_LINK_DOWN_DEBOUNCE_US   default 20000
CONFIG_KNX_TP1_BITBANG_LINK_UP_DEBOUNCE_US     default 200000
CONFIG_KNX_TP1_BITBANG_LINK_POWERFAIL_ISR
```

STKNX exposes a `KNX_OK` output that goes inactive when bus power fails. Wiring
it lets the stack stop transmitting into a dead bus instead of spending the full
CSMA and retransmission budget on every frame to discover the same thing.

**The down-debounce window must be shorter than your board's hold-up time** —
how long your local supply survives after the bus dies. Measure it: cut bus
power and scope your 3V3 rail. If hold-up is shorter than 20 ms, either shorten
the window or enable `LINK_POWERFAIL_ISR`, which delivers a pre-debounce
notification straight from the edge interrupt so you can save state before the
rail collapses.

Polarity here is a board property. If you get it inverted, the device believes
the bus is down whenever it is up, and refuses to transmit — silently.

## Step 4: verify RX before TX

Bring the board up with the bus connected but do not send anything. Enable debug
logging for the datalink layer and watch for received frames from other devices.

**RX working, TX untested** is the correct state to be in before you transmit
onto a live installation. If you cannot receive, do not start transmitting:
a stuck-dominant TX pin takes down the entire line, not just your device.

If nothing arrives:

1. Scope RX. Is there activity at all? No → wiring/transceiver, not firmware.
2. Is the idle level what you configured as recessive? No → polarity (Step 1).
3. Activity present, correct idle, still nothing decoded → sample offset
   (Step 2), or the frame is being rejected by the checksum. Check the driver's
   receive-integrity diagnostics counters.

## Step 5: first telegram

Put the device in programming mode and use ETS's "Device Info" read, or send a
group value from a bus monitor and confirm the firmware callback fires.

Watch the TP1 timing statistics (`bitbang_timing_stats.hpp`) for:

- **collisions** — some collisions are normal on a busy bus; a rate near 100 %
  means your TX is not arbitrating correctly, usually a TX polarity problem.
- **ACK window misses** — the device is not answering the DL-ACK inside t_ack.
  Almost always a sample-offset or interrupt-latency problem.
- **deadline misses** — the timer ISR is not being serviced in time. Check for
  other high-priority work or long critical sections on the same core.

## Step 6: validate against the virtual bus

Once hardware works, the virtual TP1 simulator reproduces timing scenarios that
are hard to stage physically (collisions, missing ACKs, truncated frames):

```bash
ctest --test-dir build -L tp1-virtual --output-on-failure
```

Virtual results do **not** replace HIL electrical/compliance validation with a
real transceiver.

## Common failure modes

| Symptom | Most likely cause |
|---|---|
| Nothing received, RX pin shows activity | RX polarity inverted (Step 1) |
| Whole bus goes down when the board powers up | TX polarity inverted; TX idles dominant |
| Frames received with sporadic bit errors | `SAMPLE_OFFSET_US` too early for your RX path delay |
| Two frames concatenated into one buffer | Inter-byte timeout starved — check for long ISR-disabled sections |
| Device never transmits, no error logged | Link-health polarity inverted; stack believes the bus is down |
| ETS finds the device but download fails midway | Not a bring-up issue — see [troubleshooting.md](troubleshooting.md) |
