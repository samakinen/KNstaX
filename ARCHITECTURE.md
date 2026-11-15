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
- `Tp1MacPhysical` is the only physical layer for the TP1 medium.
- `Tp1MediumBackend` is the only TP1 backend boundary.
- Physical layers are frame-oriented. Their contract is the
  `datalink::Tp1PhysicalLike` concept; there is no abstract physical base class.
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

The BAU reaches everything from `NetworkLayer` down through one concrete
class, `bau::BusAccessStackPort` (`knx/bau/stack_port.hpp`). It owns the
medium (physical + data link), the network, transport and application layers,
the Data Secure application layer when built in, and for a coupler the second
medium and the `TwoPortCoupler` between them. It has one implementation and no
test double, so it is not an interface.

`ApplicationLayer` is three parts. It decodes each received APDU, drops a
service that arrived on a communication mode it is not defined for (03/03/07
Table 1) or that the Access Policy refuses, and routes the rest: management
services to `ManagementServer`, everything else to the group runtime.
`ManagementServer` owns the nine management service objects, decodes their
requests and encodes their responses; it is handed a `RequestContext` saying
whether the request came on the transport connection and how it was secured,
and its answers leave on that same path. `ASendPipeline` encodes APDUs and
owns retries and the send-outcome queue for both.

### Canonical TP1 stack shape

```text
Tp1DataLinkLayer
    ↓   (Tp1PhysicalLike)
Tp1MacPhysical
    └─ Tp1MediumBackend
             └─ BitBangMediumBackendAdapter
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

### `Tp1PhysicalLike`

The concept `Tp1DataLinkLayer` requires of a physical layer (`Tp1MacPhysical`, the KNXnet/IP physicals and the test mock). It is intentionally narrow: it exposes frame-oriented send/receive and bus-monitor control upward to the data-link layer. It does not expose backend-family-specific byte-stream policy.

### `Tp1MacPhysical`

`Tp1MacPhysical` is the TP1 physical layer. It owns one `Tp1MediumBackend` and adapts its events into frame-oriented behavior for `Tp1DataLinkLayer`.

Responsibilities:
- latching TX outcomes from the backend's TX-ACK events (ACK, NACK, BUSY, deadline miss) into the result of a transmission
- the RX queue, and the BUSY back-pressure it asks the backend to signal when nearly full
- forwarding the ACK decision inputs (own address, subscribed group addresses, local busy) to the backend ahead of time
- driving the backend's service step from the transmit and receive polls

The per-telegram ACK decision itself is not here: it has to be made within t_ack of the frame end, which only the backend's timing domain (ISR, transceiver silicon) can meet.

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

The ISR core's state machine, invariants, timing schedule and its event
contract with the layers above are specified in
[docs/reference/tp1_bitbang_isr.md](docs/reference/tp1_bitbang_isr.md). Host
builds check the invariants and the contract at run time, so every TP1 test
enforces that document.

### Other TP1 backends

Bit-bang is the only TP1 backend. A transceiver with its own MAC (NCN5120 and similar) would be integrated as another `Tp1MediumBackend`, reusing the same TP1 MAC policy engine instead of defining a parallel TP1 architecture.

## Platform boundary

`platform::Platform` provides system, timing, memory, tasking, synchronization, and hardware service access.

Current state:
- `Tp1DataLinkLayer` RX tasking is routed through platform abstractions.
- Lower TP1 physical code uses platform abstractions instead of direct FreeRTOS APIs.
- Narrow service interfaces such as `TimingPlatform`, `QueuePlatform`, `MutexPlatform`, and `TaskingPlatform` are preferred over broad `Platform` dependencies.

Protocol and data-link code should depend only on the smallest required subset of platform behavior.

### Cooperative (single-threaded) mode

A target without an RTOS builds with `-DKNX_PLATFORM=baremetal` (standalone CMake, with a toolchain file such as `cmake/toolchains/arm-none-eabi.cmake`). `KNX_PLATFORM_BAREMETAL` then selects:

- `platform::LocalMutex` and `platform::LocalLock` (`knx/platform/mutex.hpp`) as no-ops where hosts use `std::mutex`. The arm-none-eabi libstdc++ has no `std::mutex`. Data shared with an interrupt must not rely on them; it uses critical sections.
- `platform::CooperativePlatform` (`knx/platform/cooperative_platform.hpp`): tasks are refused, locks never block, queues are fixed rings that fail instead of waiting, and a notification from an interrupt sets a flag the main loop reads.
- `Tp1DataLinkLayer` without an RX task (`config::RX_TASK_ENABLED_DEFAULT` is false). The main loop drives it through `processRxAvailable()`, which `BusAccessStackPort::processBackgroundWork()` already calls.
- No default `ObjectPersistence`. The product registers one with `objects::setPersistenceFactory()`.
- No system-clock fallback in `util::nowMs()` / `util::delayMs()`: time comes from the platform, which `BusAccessStackPort` now also hands to `Tp1MacPhysical`.

The bit-bang backend is not built in this mode; the product adds its own `Tp1MediumBackend`. `test/baremetal/size_gate` links a reference device in this configuration and checks it against flash and RAM budgets (`test/baremetal/size_gate/run_gate.sh`). Status of the bare-metal work and the measured sizes: `docs/design/cortex-m-tpuart2-support.md`.

## Execution model

TP1 receive processing uses a common async service model:
- physical/backend receive activity triggers wakeup
- the data-link RX worker drains available frames
- callbacks observe completed frames, not backend-local byte transitions

Host tests are expected to follow the same high-level semantics as embedded builds. Tests must not rely on deleted legacy behavior or assume immediate in-call completion after frame injection.

## Configuration model

`Kconfig` is the one configuration schema. ESP-IDF resolves it with its own tooling
(`sdkconfig.h`). A standalone CMake build (host tests, bare metal) resolves it at configure
time with the vendored Kconfiglib (`tools/kconfig/knx_genconfig.py`), from a **profile**, a
defconfig fragment under `configs/profiles/` (`host-test`, `host-small`, `full`, `tp1-small`,
`tp1-standard`), plus the product's own override fragments. The result is the generated
`knx/autoconf.h`, which carries the same `CONFIG_KNX_*` macros as `sdkconfig.h`; `config.hpp`
includes whichever exists. The resolved configuration is also written as `knx.config` in
`<build>/knx-config/`, and as `knx_config.cmake` for CMake to choose source groups from.

Symbols that exist only in standalone builds (platform, `KNX_BUILD_TP1`, `KNX_BUILD_NETIP`,
the virtual bus, ETS tooling, test hooks, zlib) are in `Kconfig.standalone`, which ESP-IDF
never reads. A standalone build keeps `KNX_MEDIUM` at its default and selects media with
`KNX_BUILD_*`, because a host build compiles TP1 and KNXnet/IP together while an ESP-IDF image
is one medium.

A product calls `knx_configure(PROFILE <name> OVERRIDES <files>...)` before
`add_subdirectory(KNstaX)`; a build that does not gets `tp1-small` on bare metal and `host-test`
otherwise. The older `-DKNX_FEATURE_*` options still work as overrides. See
[docs/design/small-target-profile.md](docs/design/small-target-profile.md).

**Capacities** (Kconfig menu "Capacities", defined in every build, no fallback in `config.hpp`):
`KNX_MAX_APDU_LENGTH`, `KNX_TP1_RX_QUEUE_FRAMES`, `KNX_TRANSPORT_RX_QUEUE`,
`KNX_ADDRESS_TABLE_ENTRIES`, `KNX_ASSOCIATION_TABLE_ENTRIES`, `KNX_ADDITIONAL_INDIVIDUAL_ADDRESSES`,
`KNX_SEND_PIPELINE_DEPTH` and `KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES`. Their defaults are the values the
stack had before they were options (254, 32, 8, 256, 512, 16, 8, 16), so ESP-IDF and the `full` and
`host-test` profiles are unchanged; `tp1-small` and `tp1-standard` set smaller ones. Buffers derive from
them: the longest TP1 frame is the APDU length plus 8 octets for standard frames only (23 for 15) or
plus 9 with extended frames (263 for 254), see `config::MAX_TP1_FRAME_LENGTH`. A full table or queue
returns an error (and the KNX negative response where one exists); the `.knxprod` exporter reports the
same table ceilings and the product's APDU length. A product's `maxApduLength` may not exceed
`KNX_MAX_APDU_LENGTH`. The host suite runs against the small values with the `host-small` profile
(`-DKNX_HOST_PROFILE=host-small`, excluding the `needs-full-capacity` label).

Embedded TP1 backend selection is compile-time driven through Kconfig. `KNX_TP1_BITBANG` is the only choice today; the factory builds the canonical TP1 composition over it.

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
- IP Secure Tunneling is supported through a dedicated physical adapter with ECDH session management, gated by `KNX_SECURE_IP_TUNNELING` so a routing-only device does not compile the session machinery it cannot use.
- Secure Routing is wired in `IpRoutingPhysical` and applied from a `PID_BACKBONE_KEY` download at runtime, which is how ETS enables it; it is not the default routing configuration.

## KNXnet/IP scope

The IP medium is implemented as a KNX IP device (03/02/06) rather than only as a
transport for the TP1 stack:

- `RoutingEndpoint` owns the one socket the medium needs and classifies every
  datagram from the KNXnet/IP header before anything else touches it. That
  ordering is what lets Secure Routing coexist with the services specified to
  travel in the clear on a secured group.
- `RoutingFlowControl` holds the egress budget (50 datagrams/s, 5 ms floor) and
  the ROUTING_BUSY back-off as clock arithmetic, so both are testable at
  simulated time.
- `CoreServerResponder` answers discovery and self-description on that same
  socket. Core is mandatory for Device Class C; Device Management and Tunnelling
  servers are not implemented.
- `netip/system_broadcast.hpp` decides which broadcasts belong to the
  system-broadcast domain, which on IP is a service type rather than a control
  field bit.

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
