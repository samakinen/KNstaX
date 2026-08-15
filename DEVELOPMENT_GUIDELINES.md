# KNstaX Development Guidelines

Policy for changing this stack. [ARCHITECTURE.md](ARCHITECTURE.md) describes
what the system *is*; this document describes what a change to it must satisfy.

## Project posture

KNstaX is an embedded-first KNX System-7 stack in C++23.

Engineering priorities, in order:

1. correctness
2. architectural clarity
3. deterministic embedded behaviour
4. maintainability
5. host testability

Backwards compatibility is not a priority when it conflicts with the target
architecture. Remove obsolete code rather than keeping unused compatibility
wrappers.

---

## Architecture policy

### Non-negotiable invariants

- One implementation per responsibility.
- No parallel TP1 architectures for the same behaviour.
- `Tp1MacController` is the only TP1 MAC/policy engine.
- `Tp1MediumBackend` is the only TP1 MAC/backend boundary.
- `Tp1PhysicalLayer` stays frame-oriented and backend-family-neutral.
- Public docs must match `main`.

### TP1 rules

- Do not reintroduce the legacy standalone `BitbangPhysical` architecture.
- Do not add backend-specific MAC policy outside `Tp1MacController`.
- Prefer compile-time backend selection for embedded product builds.
- Avoid RTTI in TP1 hot paths, event emission paths, steady-state capability
  checks, and service loops. Limited construction-time RTTI in temporary
  migration code may be tolerated; it must not become the pattern.
- Keep ISR and ISR-adjacent code allocation-free.
- Keep backend service logic bounded and deterministic.

### Layer separation

Each KNX layer must be independently testable, with a clear interface to its
neighbours and dependencies injected by reference:

```
Application Layer (services, DPTs, group objects)
    ↓ ADUs
Secure Application Layer (encryption/decryption)
    ↓ secure APDUs
Application Layer (APDU processing)
    ↓ TPDUs
Transport Layer (connections, sequencing)
    ↓ NPDUs
Network Layer (routing, hop count)
    ↓ L_Data frames
Data Link Layer (frame codec, addressing)
    ↓ raw bytes
Physical Layer (medium-specific transmission)
```

### Concurrency

- Every transport, data-link, and physical API must document its concurrency
  model.
- Protocol-layer code must not call FreeRTOS APIs directly where a platform
  abstraction exists. FreeRTOS belongs in the platform layer and thin wrappers,
  nowhere else.
- Host tests must not assume synchronous completion where production code is
  asynchronous. If a callback is async by design, wait for observable
  completion rather than asserting immediately after injection.

```cpp
// Not allowed in a protocol layer:
QueueHandle_t queue = xQueueCreate(...);
vTaskDelay(pdMS_TO_TICKS(100));

// Correct — through the narrowest platform interface that suffices:
Result<void> NetworkLayer::init(platform::TimingPlatform& timing);
```

Depend on the smallest platform subset you need — `TimingPlatform`,
`QueuePlatform`, `MutexPlatform`, `TaskingPlatform` — rather than the broad
`Platform`. New platform hooks need a narrow justification; `Platform` must not
become a grab-bag.

### Allocation

- ISR core: no dynamic allocation.
- Backend service and MAC service: fixed-capacity structures preferred.
- Data-link and above: controlled dynamic helpers are allowed off the
  realtime-critical path.
- A new heap-backed queue in a TP1 hot or near-hot path requires explicit
  justification in the change.

### Feature claims

- Do not advertise unsupported media or runtime targets.
- Do not claim RF support unless real RF code is present and tested.
- Do not claim full KNX Secure coverage unless the runtime path is wired and
  validated.
- Do not describe experimental or deleted architectures as current behaviour.

Feature claims live in [README.md](README.md) and
[docs/reference/knx_conformance_status.md](docs/reference/knx_conformance_status.md).
A feature listed as implemented is one that has runtime behaviour and a test —
not one that has an enum value.

---

## Error handling

`Result<T>` is the canonical error type for every KNstaX API. It wraps
`std::expected<T, ErrorCode>` internally and is the stable named type; do not
introduce raw `std::expected<>` at call sites.

**Never use exceptions** for error handling. Embedded targets need predictable
control flow and bounded cost.

```cpp
#include "knx/util/result.hpp"

using knx::util::Result;
using knx::util::ErrorCode;

// Returning a value
Result<size_t> encodeFrame(const LDataFrame& frame, std::span<uint8_t> buffer) {
    if (buffer.size() < MIN_FRAME_SIZE) return ErrorCode::BufferTooSmall;
    // …
    return encodedLength;
}

// Returning nothing
Result<void> init(const Config& config) {
    if (!config.isValid()) return ErrorCode::InvalidParameter;
    if (_initialized)      return ErrorCode::AlreadyInitialized;
    // …
    return Result<void>::ok();
}

// Consuming
auto result = encodeFrame(frame, buffer);
if (!result.isOk()) {
    KNX_LOGE(TAG, "Encode failed: %s", errorCodeToString(result.error()));
    return;
}
size_t length = result.value();
```

Use `std::optional<T>` for a value that may legitimately be absent, where
absence is not an error:

```cpp
std::optional<GroupAddress> parseGroupAddress(std::string_view str);
```

### Choosing an error code

| Code | Use for |
|---|---|
| `InvalidParameter` | Null pointers, out-of-range values |
| `BufferTooSmall` | Insufficient buffer space |
| `InvalidFrameSize` | Frame size violations |
| `ChecksumError` | CRC/checksum validation failed |
| `NotInitialized` | Operation on an uninitialised object |
| `AlreadyInitialized` | Double initialisation |
| `DecodeFailed` / `EncodeFailed` | Frame or data conversion failed |
| `TransmissionFailed` | Physical transmission error |
| `Timeout` | Operation timed out |
| `ResourceUnavailable` | Connection or memory unavailable |

Some older code still returns `bool`. Convert it when you touch it; do not add
new `bool`-returning APIs.

---

## C++ style

### Idioms

- Use modern C++23 where it improves the architecture without harming
  deterministic embedded behaviour.
- Prefer concepts and `requires` over `enable_if` or repetitive `static_assert`.
- Prefer `std::unique_ptr` / `std::shared_ptr` over raw owning pointers.
- Use RAII for every resource.
- Prefer `const` correctness and appropriate `std::move`.
- Avoid `goto`, explicit `new`/`delete`, C-style casts, and naked pointers in
  user-facing APIs.
- Avoid RTTI in TP1 runtime-critical or steady-state production paths.
- Avoid modules, coroutines, and heavy formatting in the embedded runtime unless
  a concrete subsystem-level benefit justifies them.
- Use ranges selectively in parsing and configuration code. Do not replace an
  explicit hot-path loop without a measured win.

### Containers own, spans view

- Prefer standard containers for ownership: `std::array` for fixed-size owned
  storage (over C arrays, unless C ABI or layout demands otherwise),
  `std::vector` / `std::string` for dynamic ownership.
- Represent non-owning contiguous memory as `std::span`, not a
  `(pointer, size)` pair. It expresses intent and costs nothing — but note it
  neither manages lifetime nor bounds-checks.
- Construct spans implicitly from containers rather than passing `data()` and
  size by hand. Use a fixed-extent `std::span<T, N>` only when the extent is
  guaranteed at compile time.
- Subrange with `first()`, `last()`, `subspan()` — not pointer arithmetic.
- Keep `memcpy` / `memmove` / `memset` where raw byte or overlap semantics are
  genuinely required. Do not obscure performance-critical semantics behind
  modern syntax.

### Naming

```cpp
class NetworkLayer { };           // types: PascalCase
struct IndividualAddress { };
enum class FrameType { };

bool _initialized;                // private members: leading underscore
std::unique_ptr<Tp1DataLinkLayer> _dataLink;

constexpr size_t MAX_FRAME_SIZE = 255;      // constants: SCREAMING_SNAKE
constexpr uint32_t DEFAULT_TIMEOUT_MS = 5000;

Result<void> init();              // functions: camelCase
void sendFrame();
uint8_t hopCount() const;
```

### Organisation

- One public class per header (small related types and enums may share).
- Private implementation classes in `.cpp` where practical.
- Group related functions in namespaces.
- No circular dependencies between modules.

### KNX addresses

Always use the typed forms. Never a raw `uint16_t` in a public API.

```cpp
IndividualAddress addr(1, 2, 3);   // area 1, line 2, device 3
IndividualAddress raw(0x1203);     // raw 16-bit
```

### Frames

Frames carry full KNX header information, checksum validation is mandatory, and
encode/decode must round-trip. Document the structure:

```cpp
/**
 * @brief L_Data frame (data link layer)
 *
 * KNX TP1 standard frame format. The checksum is maintained by the frame codec.
 */
struct LDataFrame {
    IndividualAddress source;
    GroupAddress destination;
    AddressType destinationType;
    Priority priority;
    bool ackRequested;
    bool confirmation;
    uint8_t hopCount;                   ///< 0–6, decremented at each router
    uint8_t tpci;
    uint8_t apci;
    std::vector<uint8_t> data;

    bool isValid() const;
};
```

---

## Documentation in code

Every source and header file gets a file header. Public APIs get Doxygen
comments that state the contract, not the obvious:

```cpp
/**
 * @brief Send a T_Data frame
 *
 * Encapsulates the ASDU in a T_Data PDU and forwards it to the network layer.
 *
 * @param frame The frame to send
 * @return Ok on successful queueing; InvalidParameter for a malformed
 *         destination; NotInitialized if no connection is established
 *
 * @pre  frame.destination is a valid individual or group address
 * @pre  For connected mode, T_Connect has been called
 * @post The frame is queued, not necessarily transmitted
 *
 * @thread_safety Thread-safe — internally synchronised
 * @note Sending is asynchronous; use callbacks for completion
 */
Result<void> sendFrame(const TDataFrame& frame);
```

Thread-safety is part of the contract and must be stated:

```cpp
/// @thread_safety Read-only — no synchronisation needed
PhysicalLayerState state() const;

/// @thread_safety NOT thread-safe — call during initialisation only
/// @warning Calling while receiving is undefined behaviour
void setReceiveCallback(ReceiveCallback callback);
```

Explain *why* for non-obvious logic, especially wire formats and spec-driven
behaviour. Cite the specification by volume, clause and version — the documents
themselves are not in this repository, so a citation is what makes a claim
checkable:

```cpp
// KNX TP1 frame format (03/02/02 §2.2.1):
//   [0]    control field
//   [1-2]  source address
//   [3-4]  destination address
//   [5]    length and hop count
//   [6]    TPCI
//   [7+]   APCI + data
//   [last] checksum — XOR of all preceding bytes, inverted
```

Do not write comments that restate the code.

---

## Incomplete code

Partial implementations must be marked, not left to look finished. A reader must
be able to tell a stub from a shipped feature without running it.

```cpp
/**
 * @brief [STUB] <what it will be>
 *
 * @warning Not functional. <What it does instead, exactly.>
 * @todo <Concrete remaining work, one item per line.>
 */
```

Rules:

- The runtime behaviour of a stub must be observable — log at warning level, or
  return a specific error. A stub that silently returns success is worse than
  no implementation at all.
- A stub must never be described as implemented in README.md or in the
  conformance status document. Those two files and the code must agree.
- `TODO:` comments carry a priority: `P0` blocks production, `P1` blocks the
  next release, `P2` is nice to have, `P3` is future work. Do not write
  calendar estimates into source comments; they are wrong within a month.

---

## Testing

- Every public component has unit tests; protocol flows have integration tests.
- Target ≥80 % coverage for core functionality.
- Unity is the framework in use. Mock external dependencies — physical layer,
  platform.
- Tests must validate the current architecture, not preserve deleted paths.
- Tests must follow the same async semantics as production code.

```cpp
#include "unity.h"
#include "knx/my_component.hpp"

void setUp(void) { }
void tearDown(void) { }

void test_component_rejects_invalid_input(void) {
    MyComponent comp;
    TEST_ASSERT_TRUE(comp.init().isOk());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidParameter, comp.process({}).error());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_component_rejects_invalid_input);
    return UNITY_END();
}
```

Beyond the host suite: a virtual TP1 bus (`-L tp1-virtual`) for timing scenarios
that are hard to stage physically, and a wire-level interop harness using `xknx`
as an independent peer. Neither substitutes for HIL validation on real
transceivers.

---

## Performance and memory safety

- Prefer stack allocation where practical; pre-allocate when the size is known.
- Use move semantics to avoid copies; avoid string concatenation in loops.
- `constexpr` for compile-time constants.
- Profile before optimising, and document performance-critical sections.
- Always bound-check. Prefer `std::span` or range-checked accessors.
- Initialise every variable; release every resource in a destructor.
- AddressSanitizer and MemorySanitizer run in CI — keep them clean.

---

## Review checklist

Before committing:

- Public functions documented, including thread-safety
- Naming conventions followed
- No compiler warnings under `-Wall -Wextra -Wpedantic`
- No magic numbers
- `Result<T>` used for error handling; no new `bool` returns
- No direct FreeRTOS calls in protocol layers
- Unit and integration tests pass
- Stubs marked `[STUB]` with a TODO, and absent from feature claims
- Specification compliance verified against a cited clause
- Docs updated alongside the code

For a TP1 change, additionally:

1. Does it preserve the single `Tp1MacController` architecture?
2. Does it keep backend differences behind `Tp1MediumBackend`?
3. Does it avoid RTTI and host-only shortcuts?
4. Does it keep the concurrency model explicit?
5. Does it preserve deterministic behaviour on embedded builds?

## Which documents to update

| Change | Update |
|---|---|
| A KNX service or interface object gains/loses behaviour | [docs/reference/knx_conformance_status.md](docs/reference/knx_conformance_status.md) |
| Architectural consequence | [ARCHITECTURE.md](ARCHITECTURE.md) |
| User-facing capability | [README.md](README.md) |
| Authoring surface or product API | [docs/reference/product_authoring_guide.md](docs/reference/product_authoring_guide.md) |
| Exporter output or ETS workflow | [docs/reference/ets_export_guide.md](docs/reference/ets_export_guide.md) |
| A new failure mode worth recognising | [docs/reference/troubleshooting.md](docs/reference/troubleshooting.md) |

When in doubt: check the KNX specification, read ARCHITECTURE.md for the design
decision, look at how a similar problem is already solved in the tree, and ask
in review.
