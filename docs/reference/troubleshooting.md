# Troubleshooting

A runbook for the failures that actually happen, ordered by where they occur in
a device's life. For a board that has never worked at all, start with
[board_bringup_guide.md](board_bringup_guide.md) instead.

## ETS cannot find the device

**"No device found in programming mode"**

1. Is programming mode actually on? `app.lifecycleState()` returns
   `Commissioning`, or `bau.programmingModeEnabled()` is true. The lifecycle
   callback firing with `Commissioning` is the definitive check — a blinking
   LED driven by your own code is not.
2. Is exactly *one* device in programming mode? `A_IndividualAddress_Read` is a
   broadcast answered by every device in programming mode, and two answers
   collide.
3. Can the device receive at all? If nothing from the bus reaches the firmware,
   this is a bring-up problem, not a commissioning problem.

**Alternative that avoids the button entirely:** this stack implements
`A_IndividualAddressSerialNumber_Read/_Write`, so a management client that knows
the device's 6-octet serial number can address it directly without programming
mode. The serial number is derived from the MAC and logged at boot.

## Download fails partway through

**Download starts, then "Device does not respond"**

The most common cause is a transport-layer retransmission problem rather than
anything application-level. Check, in order:

1. **DL-ACK timing.** If the device answers the data-link ACK outside the t_ack
   window, ETS sees an unacknowledged frame and gives up. Check the bitbang
   ACK-window miss counter. This is a `SAMPLE_OFFSET_US` / interrupt-latency
   issue, not a protocol one.
2. **Repeated frames.** A repeated connected request that the data link drops
   instead of passing up means the transport layer never retransmits, and ETS
   waits out its timeout. Look for a rising repeat count with no corresponding
   transport activity.
3. **APDU length.** The device advertises `maxApduLength` in its product
   definition. If the declared value exceeds what the transport actually
   handles, long property writes fail while short ones succeed.

**Download completes but the device behaves as before**

The load state machine did not reach `Loaded`. Read PID_LOAD_STATE_CONTROL on
the object concerned (Address Table = index 1, Association Table = 2, Group
Object Table = 3, Application Program = 4). A value stuck at `Loading` means the
LoadCompleted event never arrived or was rejected.

## Group communication does not work after a successful download

**Device does not react to group writes**

1. **Check the object's flags.** Since flags are enforced, an object without
   Write enable silently ignores writes, and one without Communication enable
   ignores everything. In ETS, open the communication object and confirm C and W
   are ticked. This is the single most common cause and it looks exactly like a
   firmware bug.
2. **Check the association.** The group address must be linked to the *right*
   communication object number. An address linked to the wrong object produces
   silence with no error anywhere.
3. Enable debug logging on `KNX.BAU` — a rejected telegram logs which flag
   rejected it.

**Device does not answer group reads**

Read enable (R) is what makes a device answer `A_GroupValue_Read`. Objects
without it stay silent by design; this is how several devices share a group
address with exactly one designated responder. If nothing answers, no device on
that address has R set.

**Device sends nothing on its own**

Transmit enable (T), plus: `publish()` only sends once the runtime is
`Operational`, and the transmit policy can legitimately suppress a send. A
send-on-change policy with a threshold suppresses sub-threshold updates, and a
min-interval policy defers them. Both are working as configured, not failing.

## Values are wrong rather than absent

**Value arrives but is nonsense**

Almost always a DPT mismatch: the sender's datapoint type does not match the
receiver's. A 2-byte float read as a 1-byte scaling value produces plausible-
looking garbage. Compare the DPT on both ends in ETS.

**Temperature reads ~0 or a huge number**

DPT 9.001 is a 2-byte half-float with an exponent; sending a raw IEEE-754 float
into a DPT 9 object, or a DPT 9 payload into a DPT 14 object, gives exactly this.

## Data Secure

**ETS cannot enable secure mode**

Secure mode is set through `A_FunctionPropertyCommand` on PID_SECURITY_MODE of
the Security Interface Object, not a plain property write. If the device answers
the function-property request with a response carrying *no return code*, the
stack did not recognise the property as a function property — check that the
Security Interface Object is registered.

**Secure telegrams rejected**

1. Sequence number out of the replay window. After a device restart without
   persisted sequence numbers, the peer's counter is ahead. The receiving
   sequence number must be restored from NVS across a reboot.
2. Wrong key. This stack provisions its tool key into NVS at first boot and logs
   it rather than using an FDSK, so the key ETS holds must be the logged one.

## Bus-level symptoms

**Device works alone but not on a populated bus**

Collision handling. Check the collision counter: a healthy device on a busy bus
loses some arbitrations and retries. Near-100 % collisions means TX arbitration
is broken — usually TX polarity.

**Whole installation degrades after a power cycle**

Read-on-init storm. If many devices have the I flag set, they all issue
`A_GroupValue_Read` at once when power returns. This stack spreads its own reads
across `loop()` calls and pushes them through the rate limiter, and 03/05/01
§4.12.5.2.4.1.3 explicitly warns against setting I by default — check how many
of your objects have it enabled.

**Device stops transmitting and logs nothing**

If a link-health pin is configured with the wrong polarity, the stack believes
the bus is down and refuses to transmit. `getLinkState()` reports what it thinks.

## Getting more detail

```bash
# Host-side reproduction with the virtual TP1 bus
ctest --test-dir build -L tp1-virtual --output-on-failure

# Wire-level interop against a spec-following peer (xknx)
./build.sh test-all
```

On target, raise `CONFIG_KNX_LOG_LEVEL`. Log arguments are guarded by the level
check, so debug logging costs nothing when disabled — but the TP1 ISR path
deliberately does not log at all; use the timing statistics counters there
instead.
