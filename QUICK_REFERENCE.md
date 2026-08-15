# Quick Reference

A lookup sheet. Everything here is explained properly somewhere else — the
pointers say where.

## Where to look

| I want to… | Read |
|---|---|
| Understand what this is and whether it fits | [README.md](README.md) |
| Define a device — ports, DPTs, parameters, persistence | [docs/reference/product_authoring_guide.md](docs/reference/product_authoring_guide.md) |
| Generate a `.knxprod` for ETS | [docs/reference/ets_export_guide.md](docs/reference/ets_export_guide.md) |
| Get a new TP1 board onto the bus | [docs/reference/board_bringup_guide.md](docs/reference/board_bringup_guide.md) |
| Diagnose a device that misbehaves | [docs/reference/troubleshooting.md](docs/reference/troubleshooting.md) |
| Know exactly which KNX services exist | [docs/reference/knx_conformance_status.md](docs/reference/knx_conformance_status.md) |
| Change the stack itself | [ARCHITECTURE.md](ARCHITECTURE.md), [DEVELOPMENT_GUIDELINES.md](DEVELOPMENT_GUIDELINES.md) |
| See it done | [examples/README.md](examples/README.md) |

Key headers:

| Header | Role |
|---|---|
| [include/knx/product/commissioned_product.hpp](include/knx/product/commissioned_product.hpp) | The device authoring surface — start here |
| [include/knx/product/endpoint_semantics.hpp](include/knx/product/endpoint_semantics.hpp) | Named signal helpers and port modifiers |
| [include/knx/application/dpt_catalog.inc](include/knx/application/dpt_catalog.inc) | The DPT catalogue, one line per sub-type |
| [include/knx/physical/tp1_medium_backend.hpp](include/knx/physical/tp1_medium_backend.hpp) | TP1 backend boundary — implement this for a new transceiver |
| [include/knx/platform/platform.hpp](include/knx/platform/platform.hpp) | Platform abstraction — implement this for a new OS |

## Commands

```bash
# Host build and full test suite
./build.sh test

# …plus the xknx wire-level interop suite
./build.sh test-all

# Subsets
ctest --test-dir build --output-on-failure
ctest --test-dir build -L tp1 --output-on-failure
ctest --test-dir build -L tp1-virtual --output-on-failure

# TP1 regression and benchmark gates
./scripts/ci/tp1_ctest_regression.sh --build-dir build
./scripts/ci/tp1_benchmark_gate.sh --build-dir build

# Generate the ETS product file from a product.hpp
cmake --build build --target tp1_switch_ets_knxprod

# ESP-IDF (from an example directory)
idf.py set-target esp32c6
idf.py menuconfig
idf.py build flash monitor
```

Running one test:

```bash
ctest --test-dir build -N                                   # list what exists
ctest --test-dir build -R '^test_product_runtime$' -V       # run one, verbosely
```

CTest exit code `8` means no test matched the filter, or the build directory was
never configured — not that a test failed.

## Address and priority types

```cpp
IndividualAddress addr(1, 1, 1);   // area.line.device
GroupAddress      ga(1, 2, 3);     // main/middle/sub  (3-level)
GroupAddress      ga(1, 256);      // main/sub         (2-level)

enum class Priority : uint8_t { System = 0, Normal = 1, Urgent = 2, Low = 3 };
```

Never pass a raw `uint16_t` where an address is meant — the typed forms are
what make the wire encoding and the validation rules non-optional.

`Priority::Low` is the correct default for routine sensor traffic. Raising it is
a decision about a shared bus, not a local one.

## Kconfig

Medium and TP1 backend are compile-time choices:

```
KNX_MEDIUM_TP1 / KNX_MEDIUM_IP        # medium (mutually exclusive)
KNX_TP1_TPUART / KNX_TP1_BITBANG      # TP1 backend family
```

Most-adjusted values, with defaults:

| Option | Default | Notes |
|---|---|---|
| `KNX_MAX_GROUP_OBJECT_PAYLOAD_BYTES` | 16 | Costs ~2× per object; 16 covers every catalogued DPT |
| `KNX_TASK_STACK_SIZE` / `_PRIORITY` | 4096 / 5 | Main processing task |
| `KNX_RX_TASK_STACK_SIZE` / `_PRIORITY` | 3072 / 10 | Frame reception; keep it high |
| `KNX_ENABLE_SECURITY` | n | KNX Secure components |
| `KNX_USE_NVS` | y | ESP32 NVS instead of raw flash |
| `KNX_DEBUG_ENABLED` + `KNX_LOG_LEVEL` | n / 3 | 0=None … 5=Verbose |

`KNX_MAX_GROUP_OBJECTS`, `KNX_MAX_ASSOCIATIONS` and
`KNX_MAX_ADDRESS_TABLE_ENTRIES` appear in menuconfig but **are not read by any
code** — they reach `config.hpp` and stop there. Real table capacity is the
segment map in `include/knx/objects/table_segments.hpp` (256 group addresses,
512 associations, 256 group objects), and a product's object count is fixed by
its definition at compile time.

The bit-bang timing and link-health options (`KNX_TP1_BITBANG_*`) are board
properties. Do not guess them — the
[bring-up guide](docs/reference/board_bringup_guide.md) derives each one
empirically, and getting polarity wrong takes down the whole line.

Log arguments are guarded by the level check, so debug logging costs nothing
when disabled. The TP1 ISR path deliberately never logs; use the timing
statistics counters there instead.

## Symptom → cause, first guesses

Full runbook in [troubleshooting.md](docs/reference/troubleshooting.md).

| Symptom | Look at first |
|---|---|
| ETS cannot find the device | Is exactly one device in programming mode? |
| Download dies partway | DL-ACK timing (`SAMPLE_OFFSET_US`), or dropped repeated frames |
| Device ignores group writes | The object's C and W flags — they are enforced |
| Device never answers reads | The R flag; objects without it stay silent by design |
| Device sends nothing | T flag, `Operational` state, or the transmit policy suppressing it |
| Value arrives as nonsense | DPT mismatch between sender and receiver |
| Whole bus drops when the board boots | TX polarity inverted — TX idles dominant |
| Device silently refuses to transmit | Link-health pin polarity inverted |
