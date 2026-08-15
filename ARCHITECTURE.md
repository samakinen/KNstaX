# KNstaX Architecture

## Overview

KNstaX is a C++23 KNX System-7 stack with an embedded-first runtime model.

Current production-focused media scope:
- TP1
- KNXnet/IP tunneling
- KNXnet/IP routing

Current product/runtime authoring model:
- `DeviceDefinition` is the canonical declarative product source
- `DeviceRuntime` is the canonical live runtime boundary
- `RuntimeServices` is the injected environment and protocol-engine service boundary
- exporter metadata is derived from the same definition source rather than duplicated in handwritten runtime code

RF is not part of the current repository architecture.

The codebase is structured so protocol logic remains portable while backend availability is selected by platform and build configuration. Host builds exist primarily for fast regression, interoperability, and architecture validation.

## Design goals

1. One implementation per responsibility.
2. Clear separation between protocol logic, TP1 medium behavior, and platform glue.
3. Embedded-first behavior with bounded memory and deterministic service flow.
4. Strong host testability without preserving host-only shortcuts.

## Architecture invariants

- There is one TP1 architecture only.
- `Tp1MacController` is the only TP1 MAC/policy engine.
- `Tp1MediumBackend` is the only TP1 backend boundary.
- `Tp1PhysicalLayer` remains a frame-oriented facade for higher layers.
- Platform code must not leak scheduler- or driver-specific semantics into TP1 protocol behavior.
- Embedded builds prefer compile-time backend selection over runtime family switching.
- Public architecture documents must describe shipped code, not retired designs.

## System stack

### End-to-end protocol layering

```text
Application / User code
    ↓
DeviceDefinition + DeviceRuntime
    ↓
ApplicationLayer
    ↓
TransportLayer
    ↓
NetworkLayer
    ↓
Tp1DataLinkLayer or IP adapters
    ↓
Physical / backend-specific transport
```

### Canonical TP1 stack shape

```text
Tp1DataLinkLayer
    ↓
Tp1PhysicalLayer
    ↓
Tp1MacPhysical
    ├─ Tp1MacController
    └─ Tp1MediumBackend
             ├─ BitBangMediumBackendAdapter
             └─ TpuartMediumBackendAdapter
```

This is the only supported TP1 architecture.

## TP1 responsibilities

### `Tp1DataLinkLayer`

Responsibilities:
- frame encode/decode through the TP1 frame codec
- address filtering
- statistics
- RX worker lifecycle through platform abstractions

Non-responsibilities:
- backend-family-specific MAC policy
- direct FreeRTOS API ownership
- backend diagnostics interpretation

### `Tp1PhysicalLayer`

`Tp1PhysicalLayer` is intentionally narrow. It exposes frame-oriented send/receive and bus-monitor control upward to the data-link layer. It does not expose backend-family-specific byte-stream policy.

### `Tp1MacPhysical`

`Tp1MacPhysical` is the canonical TP1 physical front-end. It owns one `Tp1MediumBackend` and one `Tp1MacController`, then adapts backend events into frame-oriented behavior for `Tp1DataLinkLayer`.

### `Tp1MacController`

`Tp1MacController` is the single TP1 MAC/policy implementation.

Responsibilities:
- ACK policy evaluation
- backend event interpretation
- diagnostics snapshot flow
- transport-facing TP1 service progression

Backend-specific MAC policy must not be duplicated elsewhere.

### `Tp1MediumBackend`

`Tp1MediumBackend` is the only backend boundary used by TP1 MAC logic.

Mandatory backend responsibilities:
- initialize and close backend state
- send TP1 frames
- expose medium state and capability profile
- emit TP1 events
- advance backend service logic

Optional behavior is exposed through narrow extension interfaces such as ACK control and diagnostics.

### Link health

Medium health is medium-neutral vocabulary, defined in `knx/physical/link_state.hpp` and shared by every physical family:

| Medium | Source of the indication |
| --- | --- |
| TP1 bitbang | transceiver bus-health output (STKNX `KNX_OK`) |
| TP1 TPUART | `SAVE` pin and `U_State.indication` warning bits (not yet wired) |
| KNXnet/IP tunneling | interface up plus a live tunnel connection (not yet wired) |
| KNXnet/IP routing | interface up plus multicast group joined (not yet wired) |

A backend reports it through `getLinkState()`, the `supportsLinkStateIndication` capability, and `Tp1RxEventType::LinkStateChanged`. All three default to "no indication" (`LinkState::Unknown`), and `Unknown` never changes stack behaviour — backends that cannot tell need no changes.

Above the backend, `Tp1MacPhysical` caches the debounced state, exposes it to the application through `setLinkStateCallback()`, and refuses transmission while the link is known `Down`. Without the indication a dead bus is indistinguishable from a quiet one, so every frame would otherwise spend its full CSMA and retransmission budget to discover the same thing.

`LinkMonitor` (`knx/physical/link_monitor.hpp`) holds the debounce logic, shared so the semantics do not diverge per backend. It samples levels rather than latching edges: an interrupt only wakes the consumer, so a missed or bouncing edge cannot desynchronise the state.

Debounce windows and polarity are board properties, not stack constants. The down window must stay inside the board's hold-up time — how long the local supply survives the bus. Boards whose hold-up is too short to wait out that window can additionally take a pre-debounce `LinkEventKind::PowerFailImminent` notification from interrupt context, which is what makes a save-before-power-loss path possible on hardware that has no margin for the debounced path.

## TP1 backend families

### Timer-ISR bitbang backend

The timer-ISR bitbang path is the canonical software TP1 backend.

Key pieces:
- `BitBangDriverTimerIsr`
- `BitBangDriverTimerIsrEspIdf`
- `BitBangMediumBackendAdapter`
- timer/GPIO HAL layers for ESP-IDF and host-style virtual testing

Design intent:
- allocation-free ISR core
- bounded event generation
- explicit TP1 byte/event interpretation before MAC policy flow
- observable diagnostics for collision, ACK-window, deadline, and receive-integrity status

### TPUART backend

The TPUART family is integrated through `TpuartMediumBackendAdapter`, so TPUART-class transports reuse the same TP1 MAC policy engine instead of defining a parallel TP1 architecture.

## Platform boundary

`platform::Platform` provides system, timing, memory, tasking, synchronization, and hardware service access.

Current state:
- `Tp1DataLinkLayer` RX tasking is routed through platform abstractions.
- Lower TP1 physical code uses platform abstractions instead of direct FreeRTOS APIs.
- Narrow service interfaces such as `TimingPlatform`, `QueuePlatform`, `MutexPlatform`, and `TaskingPlatform` are preferred over broad `Platform` dependencies.

Protocol and data-link code should depend only on the smallest required subset of platform behavior.

## Execution model

TP1 receive processing uses a common async service model:
- physical/backend receive activity triggers wakeup
- the data-link RX worker drains available frames
- callbacks observe completed frames, not backend-local byte transitions

Host tests are expected to follow the same high-level semantics as embedded builds. Tests must not rely on deleted legacy behavior or assume immediate in-call completion after frame injection.

## Configuration model

Embedded TP1 backend-family selection is compile-time driven through Kconfig.

Current production choices:
- `KNX_TP1_TPUART`
- `KNX_TP1_BITBANG`

Factories build the same canonical TP1 composition regardless of the selected backend family.

Bitbang builds also consume board/transceiver traits from Kconfig, including:
- GPIO pin selection
- RX pull-up policy
- dominant polarity
- software timing traits for the timer-ISR backend

## Standards and configuration plane

The current live standards/configuration path is split across four concrete layers:

- `objects::InterfaceObjectManager` owns authoritative standards-facing interface-object state, property dispatch, and persistence-backed save/load behavior.
- `application::PropertyServices` owns KNX property-value and property-description service handling at the application-layer boundary.
- `application::ApplicationLayer` is the semantic registration seam where higher layers register property objects and bind live property read/write/description providers.
- the internal runtime stack bridge owns the connection from lower stack engines into interface-object inventory and property-service registration.

Current configuration inputs also come from two distinct sources:

- product-declared static configuration in `include/knx/product/`, `include/knx/product/export_descriptor.hpp`, and the product exporter path
- ETS-derived binary/import configuration in `include/knx/ets/ets_config_loader.hpp` and the related importer/migration tests

This means the standards/configuration plane is no longer only implied by lower-layer helpers, but it is still not yet one unified commissioning model. The current maintained product-facing export path is described in [docs/reference/ets_export_guide.md](docs/reference/ets_export_guide.md).

## Security and IP scope

- Data Secure is fully wired through the Security Interface Object and runtime transport/application flow; key provisioning is manual (inherent to the KNX commissioning model).
- IP Secure Tunneling is supported through a dedicated physical adapter with ECDH session management.
- Secure Routing is wired in `IpRoutingPhysical` and activated opt-in via `secureRoutingEnabled_`; it is not the default routing configuration.

## Testing strategy

The repository uses:
- unit tests for protocol, backend, and object behavior
- integration tests for BAU, TP1/IP flows, product/runtime paths, and application behavior
- interop tests through the Python harness
- TP1-specific regression labeling and CI guardrails

Tests must validate the canonical architecture and current runtime behavior, not preserve deleted legacy paths.

## Reference documentation

- Product authoring: [docs/reference/product_authoring_guide.md](docs/reference/product_authoring_guide.md)
- ETS export workflow: [docs/reference/ets_export_guide.md](docs/reference/ets_export_guide.md)
- KNX conformance status: [docs/reference/knx_conformance_status.md](docs/reference/knx_conformance_status.md)
- Board bring-up: [docs/reference/board_bringup_guide.md](docs/reference/board_bringup_guide.md)
- Troubleshooting: [docs/reference/troubleshooting.md](docs/reference/troubleshooting.md)

## TP1 validation guardrails

Common validation entry points:
- `./build.sh test`
- `ctest --test-dir build --output-on-failure`
- `ctest --test-dir build -L tp1 --output-on-failure`
- `ctest --test-dir build -L tp1-virtual --output-on-failure`
- `./scripts/ci/tp1_ctest_regression.sh --build-dir build`
- `./scripts/ci/tp1_benchmark_gate.sh --build-dir build`

These checks are intended to keep the canonical TP1 architecture, diagnostics mapping, and TP1 benchmark coverage synchronized with future changes.

Virtual simulator results do not replace HIL electrical/compliance validation with real TP1 transceiver hardware.
