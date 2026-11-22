# Design: KNX Data Secure on `tp1-small`

| | |
|---|---|
| Status | **In progress**, 22 November 2026. D1 (the AES part), D2, D3–D5 and D6 are done and the size gate measures the device (§9); D7 and D9 are open, and the gate does not yet script a secure download (§7). Figures marked "est." are estimates from the source and `sizeof` reasoning, not a size gate run. |
| Goal | Data Secure (03/03/07 §5, the Security Interface Object) on a TP1 end device with 128 KiB of flash and 20 KiB of RAM, with the heap locked after start-up. |
| Motivating product | H8i8o (GD32F103CB + TP-UART2). Its board-side plan is in the h8i8o repository, `docs/data-secure.md`. |
| Builds on | [cortex-m-tpuart2-support.md](cortex-m-tpuart2-support.md) §8 (K6, software AES) and [small-target-profile.md](small-target-profile.md) (profiles, capacities, `init_only`, the size gate). |

## 1. Summary

Data Secure is implemented and has run against ETS6 on ESP32. Getting it onto `tp1-small` is not
mainly a crypto problem: AES-128 encrypt-only is about 1–1.5 KiB of flash. Four things stop it today,
and two of them are about memory:

1. **No AES backend on bare metal.** `aes_block.hpp` knows only mbedTLS and the ESP AES driver.
2. **The standalone build requires mbedTLS** (AES, MD, PBKDF2 and both libraries) and compiles the
   IP Secure crypto (SHA-256, X25519, PBKDF2) whenever Secure is on. The ESP-IDF branch already
   keeps those apart.
3. **The Security Interface Object allocates after start-up.** Its tables are `std::vector`s
   that grow as ETS writes them, the key lookups are `std::map`s rebuilt after every download, and
   the per-peer sequence map gains a node the first time a new sender's telegram is accepted. With
   `KNX_HEAP_POLICY=init_only` and a locked heap, the first secure download aborts the device.
4. **A 15-octet APDU cannot carry one key-table entry once it is secured** (§3.4).

Without changes, Data Secure on the reference device would cost about 4.5–5 KiB of RAM (est., §4),
against about 1.5 KiB of headroom on the H8i8o. With the work items in §5 it would cost about
2–2.5 KiB (est.), most of it the longer APDU. Flash is not the constraint: about 13–19 KiB (est.)
against about 26 KiB free on the H8i8o.

## 2. What exists

- `SecureApplicationLayer` (S-A_Data, S-A_Sync), `SecurityInterfaceObject` (object type 17),
  Access Policies, secure persistence and sequence checkpoints, the product's
  `SecureCommissioningOptions` (factory Tool Key) and the exporter's security section.
- The crypto is behind one class, `Aes128BlockCipher` (`aes_block.hpp`): "encrypt one 16-octet
  block". CBC-MAC and CTR are built on it in `aes128_cbc_mac.cpp` and `aes128_ctr.cpp`. Only the
  AES block itself is backend-specific.
- Data Secure sources are already a separate list, `KNX_SOURCES_SECURE_DATA`; the IP Secure crypto
  is `KNX_SOURCES_SECURE_IP_CRYPTO`.

## 3. What stops it today

### 3.1 AES backend

`aes_block.hpp` ends in `#error "KNX Secure requires an AES backend"` when neither
`<mbedtls/aes.h>` nor `<aes/esp_aes.h>` is found, which is the case with `arm-none-eabi` and newlib.

### 3.2 Standalone build requirements

`CMakeLists.txt` (standalone branch) fails the configure step without `mbedtls/aes.h`,
`mbedtls/md.h`, `mbedtls/pkcs5.h` and both libraries, and appends `KNX_SOURCES_SECURE`, which
includes `KNX_SOURCES_SECURE_IP_CRYPTO`, whenever Secure is on. A TP1-only device needs neither.

### 3.3 Allocation after start-up

`init_only` refuses an allocation once the product has called `heap::lock()`: a debug build traps
and a release build aborts in `operator new`. These secure paths allocate after start-up:

| Where | When | What |
|---|---|---|
| `writeBlobProperty`, `resizeBlobProperty` (`security_interface_object.cpp`) | ETS writes PID 52, 53, 54, 60, 61 | `std::vector` chunk, `upsertExtraProperty` map node, `resize` of the table |
| `writeSecurityFailuresLog`, `readBlobProperty` (table absent) | ETS writes or reads | temporary `std::vector` |
| `DeviceModel` key resolution (`device_model.cpp`, after a download or restore) | every table change | two `KeyMap`s (`std::map<uint16_t, std::array<uint8_t,16>>`) and a `std::vector<IndividualAddress>` |
| `applyDownloadedGroupKeys`, `applyDownloadedDeviceKeys` | the same | the maps move in; the old ones are freed |
| `SecurityInterfaceObject::setPeerSequence` | the first accepted telegram from a new sender | `_peerSequences[address]` inserts a node |
| `syncSequencesFromAddressTable` | after a download or restore | a node per table entry |

The capacities a product declares (`SecurityDefinition::groupKeyTableEntries`,
`p2pKeyTableEntries`, `individualAddressEntries`) go only into the `.knxprod`. Nothing bounds the
tables at run time, so the heap needed depends on what ETS downloads, which §3.3 rule 2 of
small-target-profile.md does not allow.

The size gate does not see any of this, because no reference device enables Secure
(small-target-profile.md, "Follow-up", step 4).

### 3.4 APDU length

A secured APDU carries 13 octets more than the plain one: TPCI/APCI 2, SCF 1, sequence number 6,
MAC 4 (`DataSecureSession::kSecureOverhead`). With `KNX_MAX_APDU_LENGTH=15`:

- a secured 1-bit group write fits a standard frame exactly (2 + 13 = 15);
- one entry of PID_GRP_KEY_TABLE (GA_Index 2 + key 16 = 18 octets) written with
  A_PropertyExtValueWrite is a plain APDU of about 28 octets and a secured one of about 41 (est.).
  An entry cannot be split across writes, so ETS cannot download the key tables at all.

The TP-UART2 carries frames of up to 64 octets (`TpUartMediumBackend::CHIP_MAX_FRAME`), so an
extended frame can hold an APDU of up to 55. Small-target-profile.md §11 already names 55 as the
fall-back and estimates it at about +1 KiB of RAM: every frame object (`MAX_TPDU_LENGTH`), the TP1
receive queue, the transport queue, the connection's pending data and the property service buffers
grow with it.

## 4. RAM cost as built (est.)

Reference: 32 secured group addresses, 4 entries in the Security Individual Address Table, one P2P
key, 24 group objects, Cortex-M3, newlib `malloc` (8-octet header and alignment per block).

| Item | Bytes (est.) |
|---|---:|
| `SecurityInterfaceObject` itself (five `std::map` heads, keys, sequence numbers, failure log) | 250 |
| Raw tables in `_extraProperties` (32 × 18 + 4 × 8 + 20 + 24, plus a map node and a vector per table) | 850 |
| Resolved group keys, `std::map` (about 44 B per node) | 1 400 |
| Resolved device keys and peer sequence map | 180 |
| `SecureApplicationLayer` (four callbacks, peer contexts) | 150 |
| Persistence record scratch, `+1024` when Secure is on (`InterfaceObjectManager::kRecordScratchBytes`): gone, the key tables are saved from their own storage (memory-reductions.md, P3) | ~~1 024~~ 0 |
| `KNX_MAX_APDU_LENGTH` 15 → 55 (small-target-profile.md §11) | about 1 000 |
| **Heap and static** | **about 3 850** (4 850 before P3) |
| Stack, at peak: the TX and RX transforms' buffers (`kMaxSecureTpduSize` 268 B, `kMaxPlainApduSize` 255 B), CCM blocks, the AES context (mbedTLS: 280 B) | about 1 300 (measured: 688 B CCM frame + 523 B transform buffers + MAC and AES) |

The keys are held twice: once in the downloaded table, once in the resolved map. Most of the map
cost is node overhead, not key material.

## 5. Work items

Each item is generic; none is specific to the H8i8o.

### D1. Portable AES-128 (K6)

**AES done** (the software backend; software-aes.md §7). The in-place CCM is part of D7.

Designed and measured in [software-aes.md](software-aes.md): one portable, encrypt-only AES-128
(32-bit columns, round key on the fly) on every platform, and an in-place KNX CCM. Measured on a
Cortex-M3: 794 B + 538 B of flash, a 16-octet context, no static RAM, about 1 500 instructions per
block and 5 000–12 000 per secured telegram (0.1–0.4 ms at 48 MHz, est.), off the acknowledge path.

### D2. Split the build requirements

**Done.** Standalone CMake as below. The backend is a Kconfig choice after all
(`KNX_AES_BACKEND`), because the mbedTLS and ESP backends were kept; bare metal defaults to the
software one.

- Standalone CMake: Data Secure needs no library; mbedTLS is required only when
  `KNX_SOURCES_SECURE_IP_CRYPTO` is compiled (Secure Tunnelling, `.knxkeys` import). Append
  `KNX_SOURCES_SECURE_DATA` and the IP crypto separately, as the ESP-IDF branch already does.
- With one AES implementation (software-aes.md §3.1) there is no backend choice to configure.

### D3. Fixed-capacity Security Interface Object storage

**Done.** As below, with two more capacities: `KNX_SECURITY_ZONE_KEYS` (PID_ZONE_KEY_TABLE, stored
for ETS and not used; 0 on a TP1 end device) and `KNX_SECURITY_UNLISTED_SENDERS` (D5). The
capacities are in the Kconfig menu "Security Configuration", next to the other Secure settings,
since they exist only with Secure on; the defaults are the `full` column. Each array property
reports its capacity as its maximum number of elements, so ETS reads it from the property
description. PID_GO_SECURITY_FLAGS is sized once by `setGroupObjectCount()`, which the BAU calls
before the restore. The check of a product's declared sizes is `securityPolicyFitsConfiguration()`:
a compile error for a `constexpr` product, InvalidParameter from `start()` otherwise. The
host test `test_SecurityInterfaceObject_SecureDownloadAllocatesNothing` replaces `operator new`
and runs two downloads of every table, the lookups and the sequence updates: no allocation.

New capacities (Kconfig, menu "Capacities"), each a promise checked on write as §3.3 rule 2 asks:

| Symbol | `tp1-small` | `tp1-standard` | `full` |
|---|---:|---:|---:|
| `KNX_SECURITY_GROUP_KEYS` | 32 | 128 | 255 |
| `KNX_SECURITY_P2P_KEYS` | 2 | 8 | 32 |
| `KNX_SECURITY_IA_ENTRIES` | 8 | 32 | 128 |
| `KNX_SECURITY_FIRMWARE_KEYS` | 0 | 4 | 16 |

- Each table becomes a `std::array` in its wire format plus a count. PID_GO_SECURITY_FLAGS is
  sized by the group object count. The small properties (security report, report control,
  failures log) become plain members. `_extraProperties` goes.
- A write beyond capacity gets a negative property response (zero elements written) and a log
  line, the same as the address and association tables.
- The product's declared `groupKeyTableEntries` and the other two are checked against the
  capacities at compile time, as `maxApduLength` already is against `KNX_MAX_APDU_LENGTH`. The
  `.knxprod` then cannot promise ETS more than the device stores.

### D4. Look keys up in the downloaded tables

**Done.** The group key lookup runs the other way round from the plan: each entry's GA_Index is
turned into its group address through the address table (`getAddress`, an array access) and
compared, rather than the address searched in the address table first. It is the same 32 steps
and needs only the index-to-address direction, which the address table has in O(1). The resolver
is a function pointer the BAU installs (`setGroupAddressResolver`). The last entry for an address
wins, as the map did. `DeviceModel::applySecurityKeyTables()` only logs what is in force now.

Remove the resolved `KeyMap`s.

- Group key: group address → index in the address table (the table is sorted, the lookup already
  exists for the acknowledge path) → linear scan of the group key table for that GA_Index. With
  32 entries that is about 32 two-octet compares per telegram.
- P2P key: individual address → index in the Security Individual Address Table → scan the P2P key
  table for that IA_Index.
- Firmware keys (`setGroupKey`, `setDeviceKey`) keep their precedence rule, but in a small fixed
  array (`KNX_SECURITY_FIRMWARE_KEYS`, 0 on `tp1-small`).
- The "unresolved after download" log moves to where the tables are loaded.

Saves about 1.4–1.6 KiB (est.) at the reference size, and the rb-tree code.

### D5. Last Valid SeqNr in the table only

Remove `_peerSequences`. The Security Individual Address Table already holds each partner's Last
Valid SeqNr (§6.3.8.4), and `setPeerSequence` already writes through to it. Reading and writing
the table directly removes the map and its allocations.

Behaviour for a sender that is not in the table: today it gets a map node and is tracked. To
check against 03/03/07 before building: whether such a sender's telegram must be rejected, or
tracked in a small fixed set of transient slots.

**Done, with the transient slots.** A listed partner's number is read and written in its table
entry. A sender without an entry takes one of `KNX_SECURITY_UNLISTED_SENDERS` slots (the same
8-octet format, not persisted, as the map was not); when all are taken, a secured telegram from
one more such sender is rejected before decryption and logged as InvalidSender, and nothing is
evicted, because an evicted sender could replay. Rejecting every unlisted sender, which a strict
reading of §6.3.8.4 suggests, is left out: a device on firmware keys has no table at all, and
whether ETS lists every group sender has not been checked on a bus.

The slots also do what the live map did across a download: when ETS empties the table
(StartLoading, or a resize to one element) or overwrites an entry, each number above 0 moves to a
slot, and an entry written again for that sender takes the higher of the two numbers. A download
therefore never rolls a partner's replay window back. That needs as many slots as there are
partners with a number; with fewer, the rest are dropped with a log line.

Only a change to a table entry marks the checkpoint dirty; an unlisted slot has nothing to save.

### D6. Persistence without the +1 KiB scratch

**Done:** the scratch went with memory-reductions.md P3, and the tables are now saved from their
own storage (`tableData()`), with no copy.

With D3 the tables are fixed arrays already in their wire format. Persist them from that storage
(a record header and a span; `FlashLog` writes in pieces already) rather than copying them into
`_recordScratch`, and drop the `+1024`. Saves 1 KiB of permanent heap.

### D7. No buffers in the secure path

Measured on a Cortex-M3: `ccmEncrypt` and `ccmDecrypt` take 688 B of stack each (two 259-octet
arrays), and `bau_stack_port.cpp` adds a 268 B (TX) or 255 B (RX) buffer around them: about 1.3 KiB
of the 2.5 KiB reserve for one secured telegram. Encrypting and decrypting in the frame's own TPDU
(software-aes.md §3.3) removes all four; the measured peak in the crypto is then about 200 B.

### D8. Sequence numbers and flash wear

**Done on `main` (3a13d29),** see [data-secure-sequence-persistence.md](data-secure-sequence-persistence.md):
the sending number is reserved `KNX_SECURE_SEQ_RESERVE` ahead, partner and Tool Access numbers are
written only when changed and at most once per `KNX_SECURE_REPLAY_CHECKPOINT_S`, and
`requestPowerFailSave()` lets a board write them at its power-fail warning (the H8i8o: `SAVE`).

### D9. Randomness

The S-A_Sync_Response Random must not repeat for a key. `CooperativePlatform` falls back to
`Platform::randomBytes()`, which mixes `micros()`, the serial number and a call counter. After a
reset, `micros()` at the first sync is close to the same value every time. Document that a
bare-metal product must supply a seeded source (noise at start-up plus a persisted counter), and
give the cooperative platform a hook for it if it has none.

### Already proposed elsewhere, still open

From small-target-profile.md §10, they also pay for Data Secure: a shared staging buffer for the
load segments (about 1 KiB), the memory segment bindings (392 B), the association table's
by-address index (320 B), `CommissionedProductRuntime` (2 224 B). The data link layer's frame pool
(576 B) is gone (memory-reductions.md R6).

## 6. Projected cost after D1–D8 (est.)

Same reference as §4.

| Item | Bytes (est.) |
|---|---:|
| `SecurityInterfaceObject` with fixed tables (32 × 18 + 2 × 20 + 8 × 8 + 24 + members) | 850 |
| `SecureApplicationLayer` | 150 |
| `KNX_MAX_APDU_LENGTH` 55 | about 1 000 |
| **Heap and static** | **about 2 000** |
| Peak stack in the crypto (in place, D7; measured, software-aes.md) | about 200 |

**Flash (est.):** Security Interface Object 5–7 KiB, secure application layer and CCM 4–6 KiB,
extended property services 1–2 KiB, AES and CCM 1.3 KiB (measured), secure persistence and key lookup 1–2 KiB:
**13–19 KiB**. The 1.1 measurement in cortex-m-tpuart2-support.md (+6.3 KB of code for Data Secure
on top of an image that already held the object, before LTO) is consistent with that.

## 7. Measuring it

Add a `tp1-small` + Data Secure reference variant to the size gate
(`test/baremetal/size_gate`, budget `budgets/tp1-small-secure-m3.json`). Its traffic:
S-A_Sync_Request under the Tool Key, the secure load procedure of the key tables and the Security
Individual Address Table, secured group writes and reads from two partners, a secured
A_Restart. Heap locked after start-up. The gate's "zero allocations under the lock" check is
what proves §3.3 closed. Without it, a device with Secure on has never run with the heap locked.

## 8. Order of work

1. D1 and D2 (portable AES, build split): Secure compiles for `arm-none-eabi`. Host tests run
   against the portable backend.
2. The size gate variant (§7), as built, to replace the estimates in §4 with measurements.
3. D3–D5 (fixed tables, key lookup, sequence state): the gate's lock check passes.
4. D6 and D7: the permanent and stack savings.
5. D9 with the first product that uses it (H8i8o: a noise source), and the board calling
   `requestPowerFailSave()` from its power-fail warning.
6. Conformance status and the product authoring guide updated as each item lands
   (DEVELOPMENT_GUIDELINES.md, "Which documents to update").

## 9. Status and measurements

Steps 1 to 3 of §8 are done, step 2 with a narrower traffic script than §7 asks for.

**Size gate variant.** `test/baremetal/size_gate/variants/secure.defconfig` over `tp1-small`:
Secure on, `KNX_AES_BACKEND_SOFTWARE`, the extended property services, `KNX_MAX_APDU_LENGTH=55`.
Budget `budgets/tp1-small-secure-m3.json`, in CI as `tp1-small-secure-m3`. Its traffic is the
plain gate's: the ETS-order download of the three tables and the parameters, then group traffic.
No secured telegram, no key table download.

Measured on QEMU `mps2-an385`, LTO, Arm GNU Toolchain 13.2.Rel1, against plain `tp1-small` (and a
one-off run with only `KNX_MAX_APDU_LENGTH=55`, to separate the two):

| | Flash | RAM: static + heap |
|---|---:|---:|
| `tp1-small` | 86 651 B | 7 774 B (290 + 7 484) |
| `tp1-small`, APDU 55 only | 86 681 B (+30) | 8 654 B (+880) |
| `tp1-small` + Data Secure (variant `secure`) | 105 814 B (+19 163) | 9 374 B (290 + 9 084; +1 600) |
| of which Data Secure without the APDU change | +19 133 | +720 |
| of which the software AES | 780 | 0 |

- The flash estimate of §6 (13–19 KiB) holds at its top end. The APDU estimate of §4 (about
  1 000 B) measured 880 B.
- 15 values routed, the download applied, **no allocation after `heap::lock()`** during the plain
  download: with no security tables downloaded the key maps stay empty and nothing in §3.3 runs.
  The allocations of §3.3 need the secure traffic of §7 to show, which the gate does not script
  yet; D3–D5 are still needed.
- The H8i8o device check (the `tp1-small` M3 budget's `device` section) is not applied to this
  variant, because it fails: flash 133 510 of 131 072 B with the application (12 KiB), BSP (4 KiB)
  and persistence (8 KiB) reservations, **2 438 B over**. RAM fits: 15 422 of 20 480 B.

**After D3–D5.** The variant sets the capacities of §4's reference: 32 group keys, 2 P2P keys, 8
Security Individual Address Table entries, 8 unlisted senders, no firmware or zone keys.

| | Flash | RAM: static + heap |
|---|---:|---:|
| `tp1-small` + Data Secure, before D3–D5 | 105 814 B | 9 374 B (key tables empty) |
| `tp1-small` + Data Secure, after D3–D5 | 104 531 B (−1 283) | 10 074 B (+700) |

- Flash went down by the red-black tree code and the resolution pass.
- RAM went up because the tables are now reserved at their capacity (about 750 B at these sizes)
  instead of being empty until a download. Before, a device with these tables filled needed about
  2.4 KiB more (§4: the raw tables plus the resolved maps); now the figure is the same before and
  after a download, and the device cannot run out of heap in the middle of one.
- No allocation after `heap::lock()` in the gate's (plain) traffic, and none in the host test of
  D3 for the secure download itself.
- The H8i8o flash check would now fail by about 1.2 KiB (132 227 of 131 072 B); the H8i8o's own
  image is what counts there (its `docs/data-secure.md`).
