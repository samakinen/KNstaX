# Interop Test Goals & Scope

This file documents the intent of the `interop/` test harness so future changes (including by AI agents) keep the same purpose and do not accidentally “optimize away” important interoperability coverage.

## Primary goal

Use `xknx` (a widely used KNX stack) as a **black-box, spec-following peer** to validate that the KNstaX KNXnet/IP implementation behaves in an interoperable way on the wire.

In practice this means:

- Drive KNstaX using **real KNXnet/IP frames** (UDP tunneling, routing; and TCP secure tunneling where enabled).
- Validate KNstaX behavior at protocol boundaries: connect/state lifecycle, tunneling request/ack flows, cEMI correctness, and expected confirmations.
- Prefer deterministic and locally reproducible tests that can run in CI.

## What “passing” means (and does not mean)

Passing the interop tests is strong evidence that KNstaX is compatible with other spec-compliant implementations **for the features covered by the tests**.

However:

- `xknx` is not an official KNX Association conformance test suite; interop success is not the same as formal certification.
- A deterministic local simulator is a pragmatic test tool, but it can never perfectly replicate every real gateway’s timing/behavior.

## Scope (what we currently care about)

The harness focuses on KNXnet/IP interoperability, especially:

- KNXnet/IP tunneling (UDP) end-to-end behavior
- KNXnet/IP routing (multicast) where applicable
- KNX/IP Secure tunneling (TCP) session + secure-wrapper mechanics
- GroupValueWrite/Read interactions and required confirmations

## Non-goals (avoid scope creep)

- Full KNX stack certification coverage (device management, property services, all management procedures)
- Testing discovery/autodetection: tests must use **explicit** peer binary paths/inputs for reproducibility
- Hardware-in-the-loop requirements for CI

## Design principles for maintainers

- Keep tests **wire-level**: verify what a real peer would see (frames, sequencing, service types).
- Prefer **explicit configuration** over discovery (peer binaries, ports, credentials) to keep CI stable.
- When adding “fixes” for a specific peer, confirm they are still reasonable under the KNX specs and are not just “xknx quirks”.
- When debugging failures, capture logs from:
  - the gateway simulator,
  - the KNstaX peer process,
  - and the `xknx` client.

## Extending confidence beyond xknx

To strengthen the claim “likely interoperable with other spec-compliant stacks”, periodically run the same high-level scenarios against at least one additional independent implementation (e.g., a real gateway or another stack), while keeping these tests as the fast/CI baseline.
