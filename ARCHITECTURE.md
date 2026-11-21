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

The commissioned product layer is table-driven. An endpoint definition compiles to one `constexpr`
port table (`kPortTable`, in `knx/product/impl/model/port_table.hpp`): a 14-byte descriptor per port, slot
equal to index, no strings. The port keys and display names are a second table that only the exporter and
tests read. `EndpointRuntime<Definition, Capacity>` is a thin shell over `ProductPortCore`
(`src/product/product_port_core.cpp`), which works on ports by slot: group-object registration, address
binding, transmit policy, bus-write dispatch, state providers, publish and the pending-action queue. The
typed API (`publish<Id>(v)`, `onCommand<Id>(fn)`) is inline wrappers that pass the port's slot, and bindings
are type-erased entries kept only for the ports that can use them. See
`docs/design/small-target-profile.md` (6.1 and the S1 status).

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

#### Who owns the group address list

There is one copy of a device's group address list: the `AddressTableObject`'s own storage (inline in the object, so its address never changes). A device does not keep further lists in the stack port or the data link layer. `BusAccessUnit` attaches the table to the stack port (`attachAddressTable`); the stack port gives each data link layer a view of it (`Tp1DataLinkLayer::setGroupAddressView`: a span and whether it is sorted), and the layer's receive filter (a binary search when the table is in ascending order, as an ETS download leaves it; a linear scan when firmware appended entries out of order) and its acknowledge list read that storage. A layer used on its own (unit tests, the KNXnet/IP physicals' tests) still has `addGroupAddress()`, which keeps a private list made on first use.

Publication point. The view is published by `BusAccessStackPort::publishGroupAddresses()`, which `DeviceModel::resubscribeGroupAddresses()` and `GroupRuntime` call after every change of the table: each write to the address table object (Unload and StartLoading empty it, the segment is applied at LoadCompleted, a property write changes entries), a master reset that clears links, a group object bound to an address, and the restore at start-up. A view is a pointer and a count that the layer takes as one value, so it never reads past what was published, and an in-place change between two publications is seen as it is, word by word.

The decision to acknowledge a telegram is made by the physical layer's own matcher, in its timing domain, in one of two ways:

- **A copy.** The bit-bang ISR decides from its own copy of the group addresses. The data link layer builds it from the view into a transient array and hands it over through `setAckGroupAddresses`; the core replaces its copy under the ISR lock. No interrupt reads the table's storage.
- **The storage itself.** The TP-UART backend keeps no copy (`setAckGroupAddressView` returns true). Its receive interrupt scans the table's storage between two loads of a generation that `AddressTableDomain` bumps around every change (`util::SharedGroupAddressState`; the data link layer does the same for a list it owns). A read during a change, or one the generation shows was overlapped by one, is discarded: that frame is not acknowledged and its sender repeats it. This does not depend on the view being republished, so a change that is published late, or not at all, cannot make the interrupt act on half-written entries.

On the cooperative platform the receive filter, the stack's loop and the publication run in one context, so the swap of pointer and count is a plain store at a point where no frame is being filtered. On a platform with a separate receive task the filter has the relaxed guarantee the earlier list had: a telegram that arrives while a download changes the table is filtered by the old list, the new one or a mixture.

Behaviour change: the layer filters and acknowledges on the table, so an address that is no longer in the table is no longer accepted. The earlier lists only grew: after an ETS Unload, or a download with fewer addresses, the device kept accepting and acknowledging the old ones until the next restart.

#### The group tables and their load segments

ETS loads the three group tables with the SystemB procedure: Unload, StartLoading, the table into a memory-mapped segment by A_Memory_Write (`table_segments.hpp`; the 12-octet chunks of a 15-octet APDU do not align with entries), a verification A_Memory_Read, LoadCompleted. Each table exists once:

- **Association table.** The segment is the table's storage (`AssociationTableDomain`: a big-endian count, then TSAP and ASAP per entry). What is in force is the first `size()` entries: Unload and StartLoading set it to 0, the octets ETS writes after them land in the same storage, and LoadCompleted (`applyLoadSegment()`) takes the count ETS wrote, dropping invalid entries and repeats as adding them one by one did. Lookups decode the entries in place by a linear scan, O(n) per group telegram (a few microseconds for the 80 entries of a 60-object device); there is no parsed copy and no by-address index. Persistence saves the entries straight from the storage. An append by the firmware (`addEntry`, `loadTable`) while the table is Loading is refused with `Busy`, because it would land on what ETS has written.
- **Group address table.** The entries stay a native array, because the data link layers and the TP-UART interrupt read them (above). The segment (`AddressTableObject::loadSegment()`) is staging for a download, parsed into the entries at LoadCompleted, and the buffer a save encodes the table into (`persistenceRecord()`), so the persistence layer needs no copy of its own. While the table is Loading the segment is not touched and the record is empty, as the table is.
- **Group object table.** Accepted and not applied (the group objects are the product's), and kept only so that ETS reads back what it wrote: `DeviceModel` holds one descriptor per group object the device registered before `init()` (256 when it registered none yet).

A segment reads back what ETS wrote, before and after LoadCompleted. After a restore or a change by the firmware the association and address table segments read as the table in force (the address table's after the next save), where they used to read zeros or the last download.

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
| TP1 TP-UART | transceiver `SAVE` output, plus the chip's own health (`U_State`, resets) |
| KNXnet/IP tunneling | interface up plus a live tunnel connection (not yet wired) |
| KNXnet/IP routing | interface up plus multicast group joined (not yet wired) |

A backend reports it through `getLinkState()`, the `supportsLinkStateIndication` capability, and `Tp1RxEventType::LinkStateChanged`. All three default to "no indication" (`LinkState::Unknown`), and `Unknown` never changes stack behaviour — backends that cannot tell need no changes.

Above the backend, `Tp1MacPhysical` caches the debounced state, exposes it to the application through `setLinkStateCallback()`, and refuses transmission while the link is known `Down`. Without the indication a dead bus is indistinguishable from a quiet one, so every frame would otherwise spend its full CSMA and retransmission budget to discover the same thing.

`LinkMonitor` (`knx/physical/link_monitor.hpp`) holds the debounce logic, shared so the semantics do not diverge per backend. It samples levels rather than latching edges: an interrupt only wakes the consumer, so a missed or bouncing edge cannot desynchronise the state. STKNX `KNX_OK` and TP-UART `SAVE` report the same thing and go through it alike, with one `LinkSignalPolicy` (debounce windows, early warning). Where the stack owns the GPIO (the bitbang driver), `LinkSignalConfig` adds pin, polarity and pull-up; where the board's HAL reads the signal (`knx_uart_hal_t::read_save`), the HAL applies them.

Debounce windows and polarity are board properties, not stack constants. The down window must stay inside the board's hold-up time — how long the local supply survives the bus. Boards whose hold-up is too short to wait out that window can additionally take a pre-debounce `LinkEventKind::PowerFailImminent` notification from interrupt context, which is what makes a save-before-power-loss path possible on hardware that has no margin for the debounced path. The handler is registered through `Tp1MediumBackend::setPowerFailIsrHandler()` (or `Tp1MacPhysical`, which forwards it), whichever backend is underneath.

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

`TpUartMediumBackend` (`src/physical/tpuart_medium_backend.cpp`) is a second `Tp1MediumBackend`, for a transceiver with its own MAC on a host UART (the Siemens TP-UART2; an NCN5120 differs in a few requests and is not coded yet). It talks to the board through the C interface `knx_uart_hal_t` (`include/knx/physical/uart_hal.h`).

- The chip owns bit timing, bus access, sending the L2 acknowledge, observing the acknowledge of our own frame and repetition. The backend therefore reports `Tp1CapabilityProfile::transceiverRepeats`, and `Tp1DataLinkLayer` makes exactly one attempt per frame and takes the chip's `L_Data.con` as the final outcome. A negative confirmation does not say whether the peer answered NACK, BUSY or nothing, so for a group frame it is treated as "nobody listening" (sent), as the bit-bang path treats an unanswered group frame after its repetitions.
- The acknowledge decision for group addresses (and assumed individual addresses) is made in the UART receive interrupt, which writes `U_AckInformation` as soon as the destination is known; the chip acknowledges the own individual address itself. The interrupt reads the address table's storage itself, under the table's generation (see the group address view above): a frame that arrives while the list changes is not acknowledged, and its sender repeats it.
- The chip takes `U_AckInformation` only within 1.7 ms of the address-type octet. An octet written from the interrupt queues behind whatever the main context has queued, so the main context writes nothing (no `U_L_Data` pair, no request) while a received frame waits for its acknowledge decision. What it queued before the frame started has left long before the decision, 6.8 ms later.
- The receive path runs from RAM on bare metal (`KNX_ISR_RAM`, `include/knx/platform/isr_ram.hpp`) and never calls out of it; the size gate fails if it does.

- `VirtualTpUartChip` (`include/knx/physical/virtual_tpuart_chip.hpp`, host only) simulates the chip on a character-level TP1 line with scripted partners and fault injection; `-L tpuart` runs the backend and the stack over it.

Design, protocol values and status: `docs/design/cortex-m-tpuart2-support.md` §3.

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
- No default `ObjectPersistence`. The product registers one with `objects::setPersistenceFactory()`, normally `objects::FlashLogPersistence` over a `FlashLog` on its internal flash (`knx/objects/flash_log.hpp`; the board implements the C `knx_flash_hal_t`). Saves only program; the main loop calls `FlashLog::service()`, which erases one page or compacts per call.
- No system-clock fallback in `util::nowMs()` / `util::delayMs()`: time comes from the platform, which `BusAccessStackPort` now also hands to `Tp1MacPhysical`.

The bit-bang backend is not built in this mode; the product passes a `Tp1MediumBackend`, normally `TpUartMediumBackend` over its board's UART. `test/baremetal/size_gate` links a reference device in this configuration and checks it against flash and RAM budgets (`test/baremetal/size_gate/run_gate.sh`). Status of the bare-metal work and the measured sizes: `docs/design/cortex-m-tpuart2-support.md`.

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
`tp1-standard`, `host-minimal`), plus the product's own override fragments. The result is the generated
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
`KNX_MEMORY_REGIONS` (the regions `A_Memory_*` reaches, each with the RAM behind it), `KNX_AUTHORIZED_PARTNERS`
(the `A_Authorize` levels held at once),
`KNX_SEND_PIPELINE_DEPTH`, `KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES`, `KNX_PENDING_ACTION_CAPACITY` (the product
runtime's queue of publishes made before start; 0 means twice the port count) and `KNX_CALLBACK_CAPTURE_BYTES`
(a cap on the inline capture of the stack's `util::InplaceFunction` callbacks; 0 keeps their 32 or 64 bytes). Their defaults are the values the
stack had before they were options (254, 32, 8, 256, 512, 16, 8, 16, 8, 16, 0, 0), so ESP-IDF and the `full` and
`host-test` profiles are unchanged; `tp1-small` and `tp1-standard` set smaller ones. A value below what the
stack itself needs (4 memory regions, 5 with the coupler; one authorised partner per transport connection)
fails to compile. Buffers derive from
them: the longest TP1 frame is the APDU length plus 8 octets for standard frames only (23 for 15) or
plus 9 with extended frames (263 for 254), see `config::MAX_TP1_FRAME_LENGTH`. A full table or queue
returns an error (and the KNX negative response where one exists); the `.knxprod` exporter reports the
same table ceilings and the product's APDU length. A product's `maxApduLength` may not exceed
`KNX_MAX_APDU_LENGTH`. The host suite runs against the small values with the `host-small` profile
(`-DKNX_HOST_PROFILE=host-small`, excluding the `needs-full-capacity` label).

**Heap policy** (`KNX_HEAP_POLICY`, Kconfig menu "Memory"): `free` (the default) leaves `operator new` to the
toolchain. `init_only` (`tp1-small`, `tp1-standard`) replaces it: the stack allocates while it starts, the
product calls `knx::heap::lock()` when start-up is over (`knx/util/heap.hpp`), and a later allocation traps in
a debug build and fails in a release build as an exhausted heap does. The size gate runs its traffic sequence
with the heap locked, which is the check that the steady state allocates nothing. The `arena` policy of the
design is not implemented. See the product authoring guide for what a product has to do.

Embedded TP1 backend selection is compile-time driven through Kconfig. `KNX_TP1_BITBANG` is the only choice today; the factory builds the canonical TP1 composition over it.

Bitbang builds also consume board/transceiver traits from Kconfig, including:
- GPIO pin selection
- RX pull-up policy
- dominant polarity
- software timing traits for the timer-ISR backend

Optional features are Kconfig symbols as well (menu "Optional Features", plus `KNX_ENABLE_COUPLER` and
`KNX_ENABLE_SECURITY`): the coupler role, the Security Interface Object, group object diagnostics, function,
extended property, extended memory and network parameter services, and the DPT families and catalogue.
`config.hpp` turns each into a `KNX_FEATURE_*` macro. A feature that is off is absent: `knx_optional_sources()`
(`cmake/knx_sources.cmake`) leaves its sources out of both the ESP-IDF and the standalone build, the headers
remove its declarations (so using it is a compile error, and a datapoint family that is off stops the build
with a message naming the symbol), and the size gate checks the linked image for its symbols. `host-minimal`
is the host profile with the `tp1-small` features; a test that needs an absent feature is not registered
(`knx_add_test(... NEEDS <feature>)`). See `docs/design/small-target-profile.md` (C3 status).

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
- Data Secure sequence numbers survive resets with few writes: the sending number through a stored ceiling a window ahead (`KNX_SECURE_SEQ_RESERVE`), the partners' numbers through a checkpoint written only when changed (`KNX_SECURE_REPLAY_CHECKPOINT_S`) and on `requestPowerFailSave()`. See [docs/design/data-secure-sequence-persistence.md](docs/design/data-secure-sequence-persistence.md).
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
