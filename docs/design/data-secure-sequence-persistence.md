# Design: Data Secure sequence numbers in persistent storage

| | |
|---|---|
| Status | **Built**, 21 November 2026: steps S1 to S5 (§6). |
| Goal | A sequence number this device has sent is never sent again, after any restart or power loss, and the replay state it keeps for its partners survives restarts, while flash or NVS is written rarely and the rate can be tuned per product. |
| Scope | Generic: the Security Interface Object, `DeviceModel`, `InterfaceObjectManager`, the BAU and the product facade. Boards add only a power-fail signal, when they have one. |

## 1. What was wrong

Data Secure keeps three kinds of sequence state (03/03/07 §5.3, 03/05/01 §6.2 and §6.3.8):

| State | Moves when | Stored before this change |
|---|---|---|
| Sequence Number Sending (`PID_SEQUENCE_NUMBER_SENDING`) | every secured telegram this device sends | only with a full save of the commissioned state: after a download or a configuration change |
| Last Valid SeqNr per partner (in `PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE`) | every secured telegram accepted from a partner | checkpoint, at most once a minute, and on the restart path |
| Sequence Number for Tool Access (not a property) | every secured telegram accepted under the Tool Key | with the partner table, as a second record |

The sending number was the defect. After **any** restart (a planned one too, as the checkpoint did
not cover it) the device resumed from the value of its last full save. It then reused sequence
numbers it had already sent: partners rejected them as replays until the counter passed their
stored value, and a reused number under the same key repeats the CCM nonce.

The receive side was correct but wrote more than it needed: both records on every checkpoint, even
when only one had changed, at a fixed minute that no product could tune.

## 2. Requirements

1. **Never reuse a sending number**, after a crash, a watchdog reset or a power loss with no
   warning. This cannot depend on a power-fail signal.
2. **Few writes.** Every write costs endurance, and on a small MCU an erase stalls the CPU (on the
   GD32F103 37–39 ms with the TP-UART2 in busy mode). Writes are bounded by a rate that does not
   grow with traffic beyond a known factor.
3. **Tunable per product** through Kconfig, with defaults that suit both an ESP32 with NVS and a
   Cortex-M with a small flash log.
4. **Use a power-fail signal when the board has one** (STKNX `KNX_OK`, TP-UART `SAVE`, a supply
   voltage detector) to save the replay state at the last moment, so the timer can run rarely.
5. **No behaviour change without persistence.** A device built without persistence (host tests,
   products that opt out) sends as before.

## 3. Design

### 3.1 Sending number: a reserved window

The stored value is not the counter but an upper bound on every number used so far.

- A record holds `R`, the **reserved ceiling** (6 octets, big-endian, persistence key `0x11F1`,
  next to the Tool Access record `0x11F0`, outside every PID of object type 17). Numbers up to `R`
  may be used without another write.
- **Sending.** The next number `n` is used only if `n ≤ R`. If not, the device first stores
  `R' = n − 1 + N` (N is the window, §3.4) and then uses `n`. If that write fails, the telegram is
  **refused**, not sent plain and not sent with an unreserved number: the transport's TX transform
  already fails the send when securing fails.
- **Top-up in the background.** Once fewer than N/2 numbers remain, the BAU's `loop()` stores a new
  ceiling `counter + N` at the next pass, before the window runs out, so a send waits for a write
  only on the first send after a start-up or when the background write failed. A failed top-up is
  retried no faster than every 5 s, like a failed full save.
- **Start-up.** The counter becomes the higher of the restored property, the factory value and `R`.
  Nothing is reserved yet, so nothing is written at start-up: a device that sends nothing secured
  never writes this record, and a reset loop costs no writes.
- **Writes:** one per N/2 secured sends, plus one per start-up that is followed by a secured send.
  An unexpected reset skips at most N numbers. With 48-bit numbers and the default N = 8192 that is
  34 billion resets.

Nothing is written at a planned restart or at power fail: the ceiling is already stored before any
number below it is used.

When ETS writes `PID_SEQUENCE_NUMBER_SENDING`, the counter takes that value. A value above the
ceiling needs a reservation at the next send, which happens by itself. ETS does not write a lower
value. If it did, a later start-up would move the counter back up to the ceiling, which only skips
numbers.

### 3.2 Replay state: a checkpoint written only when it changed

- The partner table and the Tool Access number have **separate dirty flags**. A checkpoint writes
  only the records that changed. The Tool Access number moves only while ETS talks to the device,
  so outside commissioning one record is written instead of two.
- The checkpoint interval comes from Kconfig (§3.4). A change starts the clock: the first change
  after a quiet period is written one interval later at the earliest, and further changes in that
  interval ride along. So the timer writes at most once per interval, and not at all while nothing
  secured arrives.
- The restart path still writes the checkpoint (`flushPersistenceIfDirty()`), as before.
- What an unexpected reset can lose: partner numbers accepted since the last checkpoint. Those
  telegrams could be replayed once, until the partner's next telegram or an S-A_Sync moves the
  window on. A power-fail save (§3.3) closes that gap for a bus outage; only a crash leaves it open.

### 3.3 Power-fail save

- `BusAccessUnit::requestPowerFailSave()` (and the same call on `CommissionedProduct`) sets an
  atomic flag. It is safe from an interrupt, another task or the link state callback.
- The next `loop()` handles it first: it writes the replay-state checkpoint at once, if it is
  dirty, and nothing else. It does **not** run a full save of the commissioned state: that is about
  23 records, far too long for a hold-up window, and a configuration change already has its own
  quiet-window flush.
- The caller wakes whatever runs `loop()`. KNstaX does not do it from the request, because the
  request may come from an interrupt.
- A marginal bus can make the signal flap. A save happens only when the state is dirty, so a flap
  costs a write only if secured telegrams arrived since the last one.
- Where the signal comes from is the board's business: the bit-bang driver's link-down event
  (STKNX `KNX_OK`, `CONFIG_KNX_TP1_BITBANG_LINK_PIN`), a TP-UART `SAVE` pin, or a supply voltage
  detector. Whether the save completes inside the hold-up time has to be measured on the board.

### 3.4 Kconfig options

Both are in the "Security Configuration" menu and exist only with `KNX_ENABLE_SECURITY`.

| Symbol | Default | Range | Effect |
|---|---:|---|---|
| `KNX_SECURE_SEQ_RESERVE` | 8192 | 64 … 16777216 | Window N of §3.1: one write per N/2 secured sends; an unexpected reset skips up to N numbers |
| `KNX_SECURE_REPLAY_CHECKPOINT_S` | 900 | 10 … 86400 | Interval of §3.2: at most one checkpoint per interval while secured telegrams arrive; a longer interval means a longer replay window after a crash or a power loss without warning |

The checkpoint default moves from 60 s to 900 s. Planned restarts flush as before, a product with a
power-fail signal saves at the outage, and what remains is a crash: 15 minutes of partner numbers
that could be replayed once, against 15 times fewer writes. A product that cannot signal power
failure and wants a shorter window sets a shorter interval.

### 3.5 Write budget with the defaults

Worst case, a device that both sends and receives a secured telegram every second, all day:

| Record | Writes per day |
|---|---:|
| Sending ceiling (one per 4096 sends) | 21 |
| Partner table (one per 900 s) | 96 |
| Tool Access number | 0 outside commissioning |

On an ESP32 with the 32 KB NVS partition this is far below the flash endurance. On a Cortex-M with
an 8 KiB flash log it is not negligible: a partner table of ten entries (80 octets plus the record
header) written 96 times a day is about 9 KB a day, so the log compacts about twice a day (four page
erases each, alternating between the two banks), and more often when the commissioned state fills
much of a bank. Such a product sets a longer interval and relies on a power-fail save, and a
partner record that holds only the changed entry would cut this by the table size (§6).

## 4. Start-up order

1. `restore()` loads the persisted properties, including `PID_SEQUENCE_NUMBER_SENDING`.
2. `settleRestoredSecurityState()` keeps the higher of that and the factory value.
3. `applySecurityKeyTables()` adopts the partner numbers from the restored table.
4. `loadSecuritySequenceState()` loads the Tool Access number and the sending ceiling `R`, each on
   its own (a missing record no longer hides the other), and raises the counter to `R`.

A master reset leaves all of this alone: 03/03/07 §5 says the Sequence Number Sending is not
influenced by it, and the ceiling record is not part of the banked state that a reset rewrites. A
persistence schema change erases the whole namespace and with it the ceiling; that is the existing
behaviour for every record and happens only with a firmware that changes the layout.

## 5. What was considered and not taken

- **Writing the sending number with every telegram.** Correct, but one write per telegram.
- **Saving the sending number only at power fail.** Breaks requirement 1: a crash or a reset with no
  warning reuses numbers.
- **Writing the ceiling at every start-up.** Simpler, but every reset (and every reset of a loop)
  writes. Reserving on the first secured send costs the same once the device sends, and nothing
  otherwise.

## 6. Plan

| Step | Content | Test |
|---|---|---|
| S1 | Kconfig symbols and `config.hpp` constants | builds with and without Secure |
| S2 | Security Interface Object: ceiling, reservation callback, top-up flag, separate dirty flags | unit tests: no writer means no limit; first send reserves; no write inside the window; top-up at half; a failed write refuses the number and leaves the counter; restoring a ceiling raises the counter |
| S3 | `InterfaceObjectManager`: ceiling record, records written only when dirty, loads independent of each other | covered by S5 |
| S4 | `DeviceModel` and BAU: install the writer when persistence is on, configurable checkpoint interval, top-up with retry, `requestPowerFailSave()`; pass-through on `EndpointRuntime` and `CommissionedProduct` | covered by S5 |
| S5 | Restart tests | secured sends, power cut without a flush, next number above every number sent; power-fail request saves the partner numbers without a flush; without it they are not saved before the interval |

Products: Habinari calls `requestPowerFailSave()` when `KNX_OK` reports the bus down. The H8i8o
builds `tp1-small` without Data Secure, so nothing changes there. Before it enables Secure, it needs
the `SAVE` pin question answered (or a supply voltage detector), and a partner record that holds
only the changed entry, so the checkpoint is not the whole table.
