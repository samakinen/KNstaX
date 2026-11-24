# Design records

These documents record why parts of KNstaX are built the way they are: the
problem, the options weighed, the decision, and the measurements taken while
the work was done. They are kept because the reasoning and the numbers are
still useful when a decision has to be revisited.

They are **records, not reference documentation.** Each was written as the
work went along, so its body contains dated progress notes, work-item labels
(K1, S3, D7, …) and figures that later work has superseded. Only the status
line at the top of each is kept current. For how the stack behaves today, read
the reference documents linked in the table.

| Record | What it decided | Current state is in |
|---|---|---|
| [cortex-m-tpuart2-support.md](cortex-m-tpuart2-support.md) | Bare-metal Cortex-M builds: `CooperativePlatform`, the UART HAL, the TP-UART2 backend and its simulator, `FlashLogPersistence`, the bus power-fail warning | [GD32F1 port](../../ports/gd32f1/README.md), [conformance status](../reference/knx_conformance_status.md) |
| [small-target-profile.md](small-target-profile.md) | Kconfig for standalone builds, configuration profiles, capacities, the table-driven runtime, `init_only` allocation and the size gate | [README: footprint and building](../../README.md#footprint-and-capacity), [quick reference](../../QUICK_REFERENCE.md) |
| [memory-reductions.md](memory-reductions.md) | The RAM and flash reductions that made room for Data Secure on a 20 KiB part | [README: footprint](../../README.md#footprint-and-capacity) |
| [parameter-model.md](parameter-model.md) | ETS parameters as a `constexpr` layout read in place from one image | [ETS export guide](../reference/ets_export_guide.md), [product authoring guide](../reference/product_authoring_guide.md) |
| [software-aes.md](software-aes.md) | The portable AES-128 backend and the choice between AES backends | `KNX_AES_BACKEND` in [Kconfig](../../Kconfig) |
| [data-secure-small-target.md](data-secure-small-target.md) | Data Secure on `tp1-small`: fixed-capacity key tables, the feature selections, the FDSK left to the product | [README: Data Secure in a product](../../README.md#data-secure-in-a-product) |
| [data-secure-sequence-persistence.md](data-secure-sequence-persistence.md) | Sequence numbers that are never reused after a reset or power loss, at a bounded flash write rate | [Conformance status](../reference/knx_conformance_status.md), [troubleshooting](../reference/troubleshooting.md) |

## Writing a new one

Write a design record when a change has real alternatives, touches several
layers, or has a cost (flash, RAM, a public API) that someone will later want
to know the reason for. Start with a status table (status, goal, what it builds
on) and a section that states the problem. When the work is done, update the
status line, and move what users and product developers need into the
reference documents and the README. Do not leave the record as the only place
it is written down.
