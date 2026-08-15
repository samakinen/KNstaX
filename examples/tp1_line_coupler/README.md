# TP1 Line Coupler

A KNX coupler built through the **same** `startCommissionedProduct` call as an
end device. The only difference is `CouplerOptions` in place of a single medium
backend:

```cpp
auto app = startCommissionedProduct(
    platform, kCouplerProduct, std::move(bindings),
    CouplerOptions{
        .primary   = createTp1Backend(mainLinePins),   // upstream
        .secondary = createTp1Backend(subLinePins),    // downstream subnetwork
    });
```

Everything else — endpoints, parameters, ETS export, persistence, programming
mode — behaves exactly as it does for a switch or a sensor.

## What the stack does for you

> **Untested.** The coupler path is implemented and covered by host tests, but
> has never been run on real hardware or across a real two-line installation.
> Everything below describes what the code does, not what has been observed on a
> bus.

| Concern | Handled by |
|---|---|
| Routing between the two ports | 03/03/03 §2.4.2.4, in full |
| Coupler role | Derived from the individual address ETS assigns |
| Router Object (type 6) | Published automatically |
| Filter table download | `PID_ROUTETABLE_CONTROL`, bound to the live table |
| Group objects of the coupler's own | The ordinary endpoint path, reachable from both sides |

## Role comes from the address, not from configuration

| Individual address | Role |
|---|---|
| `x.y.0` (y ≠ 0) | Line coupler for line `x.y` |
| `x.0.0` | Backbone coupler for area `x` |
| anything else | Repeater |

An uncommissioned coupler is a **repeater**: it forwards rather than filters.
That is deliberate. A coupler that blocked everything before it had an address
would cut the installation in half the moment it was plugged in, and you would
have to reach it *through* itself to fix that.

## The one thing firmware must do

ETS sends a coupler two different kinds of configuration, and they do not arrive
the same way:

- **The filter table** goes through `PID_ROUTETABLE_CONTROL`, a Function
  Property. It applies live. Nothing to do.
- **`PID_MAIN_LCCONFIG` and its siblings** are ordinary properties. They land in
  the Router Object and change nothing until you call:

```cpp
app.syncRouterRoutingConfig();
```

The example calls it on the transition back to `Operational`, which is the
cheapest correct trigger. Skip it and a downloaded PHYS_LOCK or BROADCAST_LOCK
will read back correctly in ETS while the coupler keeps routing — the kind of
bug that costs a site visit.

## Observability

```cpp
coupler->setFrameFilteredCallback(...);  // routing condition said no
coupler->setFrameDroppedCallback(...);   // hop count exhausted
```

These are not the same thing, and the distinction is worth keeping. *Filtered*
is the filter table doing its job. *Dropped* means a telegram is failing to
reach its destination because it ran out of hops — a topology problem, usually
too many couplers in a chain, and something to surface rather than swallow.

## Running the example

It builds against null backends so it starts anywhere:

```bash
cmake --build <build> --target tp1_line_coupler
./tp1_line_coupler
```

Nothing is forwarded, since neither port is attached to a bus — the point is to
show the assembly and the lifecycle. For real hardware, replace `makeBackend()`
with two genuine TP1 backends on **separate transceivers**, one per line.

> A coupler sits electrically between two lines. Read
> [`../../docs/reference/board_bringup_guide.md`](../../docs/reference/board_bringup_guide.md)
> first: a TX polarity fault here holds *both* lines dominant, not one.

## Layer-2 acknowledge

`PHYS_IACK` and `GROUP_IACK_ROUT` are computed and reported through
`setAckPolicyCallback()`, but **not enforced** on TP1 — the backends decide
`L_ACK` inside the receive ISR from a published address table and cannot consult
a per-frame decision made in the coupler. See
[`../../docs/reference/knx_conformance_status.md`](../../docs/reference/knx_conformance_status.md).
