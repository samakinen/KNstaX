# Design: KNX Data Secure on `tp1-small`

| | |
|---|---|
| Status | **Proposed**, 21 November 2026. Analysis only: nothing here is built. Every figure marked "est." is an estimate from the source and `sizeof` reasoning, not a size gate run. |
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
| Persistence record scratch, `+1024` when Secure is on (`InterfaceObjectManager::kRecordScratchBytes`) | 1 024 |
| `KNX_MAX_APDU_LENGTH` 15 → 55 (small-target-profile.md §11) | about 1 000 |
| **Heap and static** | **about 4 850** |
| Stack, at peak: the TX and RX transforms' buffers (`kMaxSecureTpduSize` 268 B, `kMaxPlainApduSize` 255 B), CCM blocks, the AES context (mbedTLS: 280 B) | about 1 300 (measured: 688 B CCM frame + 523 B transform buffers + MAC and AES) |

The keys are held twice: once in the downloaded table, once in the resolved map. Most of the map
cost is node overhead, not key material.

## 5. Work items

Each item is generic; none is specific to the H8i8o.

### D1. Portable AES-128 (K6)

Designed and measured in [software-aes.md](software-aes.md): one portable, encrypt-only AES-128
(32-bit columns, round key on the fly) on every platform, and an in-place KNX CCM. Measured on a
Cortex-M3: 794 B + 538 B of flash, a 16-octet context, no static RAM, about 1 500 instructions per
block and 5 000–12 000 per secured telegram (0.1–0.4 ms at 48 MHz, est.), off the acknowledge path.

### D2. Split the build requirements

- Standalone CMake: Data Secure needs no library; mbedTLS is required only when
  `KNX_SOURCES_SECURE_IP_CRYPTO` is compiled (Secure Tunnelling, `.knxkeys` import). Append
  `KNX_SOURCES_SECURE_DATA` and the IP crypto separately, as the ESP-IDF branch already does.
- With one AES implementation (software-aes.md §3.1) there is no backend choice to configure.

### D3. Fixed-capacity Security Interface Object storage

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

### D6. Persistence without the +1 KiB scratch

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
