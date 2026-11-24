# Design: portable AES-128 and in-place KNX CCM

> A design record: the status line is current, the body is the history of the work. See [the index](README.md) for where the current reference is.

| | |
|---|---|
| Status | **Partly done**, 22 November 2026: the AES of §3.1 is in the tree as one backend of three, and the build split of §3.5 is done for Data Secure. The in-place CCM (§3.2, §3.3) is not. See §7. |
| Goal | AES-128 and the Data Secure CCM on every platform without mbedTLS or a hardware block, small enough for `tp1-small`. |
| Replaces | Work item D1 of [data-secure-small-target.md](data-secure-small-target.md) and K6 of [cortex-m-tpuart2-support.md](cortex-m-tpuart2-support.md) §8. Also covers D7 (stack). |

## 1. Decision

- **AES:** a 32-bit word-oriented, encrypt-only AES-128 with one 256-octet S-box and the round key
  computed on the fly. 794 B of flash, a 16-octet context, about 1 500 instructions per block on a
  Cortex-M3. It is the only AES implementation on every platform, including ESP32 and hosts.
- **CCM:** a KNX-specific CCM that encrypts and decrypts **in the TPDU buffer**, with no copies:
  538 B of flash and about 200 B of peak stack, against 890 B and about 860 B for the code it
  replaces.
- **No error paths in the crypto.** Software AES cannot fail, so the block and CCM functions
  return `void` (or `bool` for "MAC verified"), not `util::Result`.

## 2. Measurements

Arm GNU Toolchain 13.2.Rel1, `-mcpu=cortex-m3 -mthumb -Os`, no LTO. Instructions counted on QEMU
`mps2-an385` with `-icount shift=0` (SysTick on virtual time, calibrated with a loop of known
length: 40 instructions per tick). A Cortex-M3 at 48 MHz with one flash wait state runs at about
1.3–1.5 cycles per instruction on this kind of code (estimate).

### 2.1 AES-128 candidates

All four pass FIPS-197 Appendix C.1 and agree with each other over 500 chained keys.

| Candidate | Flash (code + tables) | Context | Encrypt, instructions per block | Key set-up |
|---|---:|---:|---:|---:|
| V1 byte-oriented, 176-octet expanded key | 700 B | 176 B | 4 906 | 1 018 |
| V2 byte-oriented, round key on the fly | 726 B | 16 B | 6 349 | 23 |
| **V3 32-bit columns, round key on the fly** | **794 B** | **16 B** | **1 503** | **41** |
| V4 one 1 KiB T-table, expanded key | 1 932 B | 176 B | 653 | 556 |

Stack of the encrypt function: V1 72 B, V2 96 B, V3 64 B, V4 48 B. `-O2` on V3 changes nothing
(804 B, same speed).

V3 costs 94 B more flash than the smallest and is three to four times faster than the byte-oriented
versions. V4 is 2.3 times faster than V3 per block but costs 1.1 KiB more flash and 160 B more
stack, and a telegram (about 4–8 blocks) is already well under a millisecond with V3.

### 2.2 CCM

| | Today (`aes128_cbc_mac.cpp`, `aes128_ctr.cpp`, `ccmEncrypt`/`ccmDecrypt`) | In-place prototype |
|---|---:|---:|
| Flash, CBC-MAC and CTR | 616 + 274 = 890 B | 538 B (both directions, MAC check included) |
| Stack, `ccmEncrypt` / `ccmDecrypt` frame | 688 B each | 80 B each |
| Peak stack in the crypto (CCM + MAC + AES) | about 860 B | about 200 B |
| Buffers in the BAU's secure transforms | 268 B (TX) and 255 B (RX) | none |

The prototype matches KNstaX's CBC-MAC and CTR, composed as `ccmEncrypt` composes them, on 122
cases: payloads of 0–60 octets, associated data of 1 and 7 octets (S-A_Data and S-A_Sync), the
round trip, and a flipped MAC bit refused.

Time for one secured telegram, encrypt, V3:

| Payload | Blocks | Instructions | At 48 MHz (est.) |
|---|---:|---:|---:|
| 2 octets (1-bit group write) | 3 | 4 951 | 0.10–0.15 ms |
| 42 octets (APDU of 55) | 7 | 11 971 | 0.3–0.4 ms |

None of this is on the TP-UART2's acknowledge path: the chip is told to acknowledge from the
destination address, before the APDU is decrypted. A TP1 frame takes 20–40 ms on the wire.

### 2.3 Secure application layer

`secure_application_layer.cpp` is 3 270 B and `data_secure.cpp` 244 B on the same flags. Part of
the secure application layer's size is the `Result` plumbing around crypto calls that cannot fail,
and the copies into the combined buffers. Expect a few hundred bytes back from §3.3 (not measured).

## 3. Design

### 3.1 AES-128

```cpp
// include/knx/security/aes128.hpp
namespace knx::security {

/// AES-128, encryption only (CCM, CBC-MAC and CTR never decrypt a block).
/// The round keys are derived on the fly, so the object is the 16-octet key.
class Aes128 {
public:
    using Key = std::array<uint8_t, 16>;
    using Block = std::array<uint8_t, 16>;

    explicit Aes128(const Key& key) noexcept;
    ~Aes128();                        // secureZero of the key words
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;

    /// @p in and @p out may be the same block.
    void encrypt(const uint8_t* in, uint8_t* out) const noexcept;

private:
    uint32_t _key[4];
};

}
```

- Implementation in `src/security/aes128.cpp`: V3 of §2.1. State as four 32-bit columns (octet 0 =
  row 0), SubBytes and ShiftRows in one pass of 16 S-box lookups, MixColumns as
  `mul2(t ^ ror8(t)) ^ ror8(t) ^ ror16(t ^ ror8(t))` with a packed `mul2`, and the next round key
  computed from the previous one in the same loop.
- The S-box is a `constexpr` table generated at compile time from the field arithmetic (a
  `consteval` function), not 256 literals typed in: the source shows how it is made, and a typo
  cannot survive.
- Loads and stores are by octet, so no alignment assumption, and no endianness dependence.
- `Aes128BlockCipher`, the mbedTLS and ESP backends and the `#if __has_include` selection in
  `aes_block.hpp` go. Every platform runs the same code that the host tests run.

**Why not keep the hardware backends.** On the ESP32, `esp_aes` takes a lock and powers the
peripheral for each call; for single blocks it saves little or nothing (to be measured on an ESP32
before it is removed, as the one condition on this decision). mbedTLS 4 removed the block API that
`aes_block.hpp` depends on. One implementation means one set of tests and no build-time detection.

**Side channels.** The S-box lookups are indexed by secret data. On the Cortex-M0+/M3/M4 parts this
profile targets there is no data cache, and a table read from flash or SRAM takes the same time
whatever the index, so the lookups do not leak time. Power analysis needs the device on a bench,
and the keys are readable over SWD there anyway unless read protection is set. A part with a data
cache (Cortex-M7, some M33) would need a bitsliced variant; that is not planned.

### 3.2 KNX CCM, in place

```cpp
// include/knx/security/knx_ccm.hpp
namespace knx::security::ccm {

/// 03/03/07 §5.1.3: CBC-MAC over B0 | len(A) | A | P (A and P not padded apart),
/// then CTR from Ctr0, the MAC taking S0[0..3] and the payload continuing at S0[4].
/// Encrypts @p payload in place and writes the 4-octet MAC.
void seal(const Aes128& aes, const std::array<uint8_t, 16>& block0,
          const std::array<uint8_t, 16>& counter0, std::span<const uint8_t> associated,
          std::span<uint8_t> payload, std::span<uint8_t, 4> macOut) noexcept;

/// Decrypts @p payload in place and checks the MAC in constant time. On a
/// mismatch the payload is wiped and false is returned.
[[nodiscard]] bool open(const Aes128& aes, const std::array<uint8_t, 16>& block0,
                        const std::array<uint8_t, 16>& counter0, std::span<const uint8_t> associated,
                        std::span<uint8_t> payload, std::span<const uint8_t, 4> mac) noexcept;

}
```

- The CBC-MAC XORs the input straight into the chaining block (no second block buffer), and the
  CTR keystream block is the only other 16 octets. Both wipe what they leave on the stack.
- `buildSecureBlock0` and `buildSecureCounter0` (`data_secure.cpp`) stay, made `void`.
- One `Aes128` per telegram: `seal` and `open` take the keyed object, so the key is set up once
  per telegram instead of twice. With on-the-fly round keys that costs 41 instructions anyway.

### 3.3 The secure application layer and the BAU transforms

- `SecureApplicationLayer::processIncoming` and `processOutgoing` work on the frame's own TPDU
  (`TDataFrame::tpdu`, a fixed vector of `config::MAX_TPDU_LENGTH`):
  - **Receive:** decrypt the payload in place, then move it to the start of the APDU and shorten
    the vector. The 255-octet `plainTpdu` in `bau_stack_port.cpp` goes.
  - **Send:** move the plain APDU up by the 9 octets of SCF and sequence number, write the header,
    seal in place and append the MAC. If the secured form does not fit the vector, refuse the
    send, as today. The 268-octet `secureTpdu` goes.
- `ccmEncrypt` and `ccmDecrypt` and their two 259-octet arrays go.
- The `Result` returns on the crypto calls go, which takes their error branches with them.

### 3.4 IP Secure

`secure_wrapper.cpp`, `secure_session.cpp` and `secure_routing_security.cpp` use CBC-MAC and CTR
with the KNXnet/IP Secure framing. They move to `Aes128` too. `Aes128CbcMac` and `Aes128Ctr` stay
for them, rewritten on `Aes128` and `void`, and are compiled only with KNXnet/IP Secure. SHA-256,
X25519 and PBKDF2 stay on mbedTLS for now (D2 of the Data Secure design: only Secure Tunnelling
needs them).

### 3.5 Build

- `KNX_SOURCES_SECURE_DATA`: `aes128.cpp`, `knx_ccm.cpp`, `data_secure.cpp`,
  `secure_application_layer.cpp`. No library dependency.
- `KNX_SOURCES_SECURE_IP_AEAD` (new): `aes128_cbc_mac.cpp`, `aes128_ctr.cpp`, with KNXnet/IP Secure.
- The standalone CMake requires mbedTLS only for `KNX_SOURCES_SECURE_IP_CRYPTO`.

## 4. Tests

- **Host unit tests** (`test_aes128.cpp`): FIPS-197 Appendix C.1, the NIST SP 800-38A F.5.1
  CTR-AES128 vectors (block function only), and in-place operation.
- **CCM:** the existing 03/03/07 Annex C vectors of `test_secure_application_layer.cpp` run on the
  new path unchanged. Add the round trip, a flipped bit in each of MAC, payload, associated data,
  B0 and Ctr0, and payload lengths 0 to the maximum.
- **Differential test**, while the old code exists: the new path against the mbedTLS one on host,
  random keys and lengths. It goes with the old code.
- **Bare metal** (`test/baremetal/crypto/`): the same vectors on QEMU `mps2-an385` and `mps2-an386`
  with semihosting, plus the instruction counts of §2 as an upper bound, so a change that makes
  the block function much slower fails CI.

## 5. Cost on `tp1-small`, summary

| | Flash | RAM (static, heap) | Peak stack |
|---|---:|---:|---:|
| AES-128 (V3) | 794 B | 0 | 64 B |
| KNX CCM | 538 B | 0 | about 140 B on top of AES |
| **Crypto total** | **about 1.3 KiB** | **0** | **about 200 B** |
| Removed from the secure path compared with today | 890 B (CBC-MAC, CTR) and the copies | | about 1.2 KiB (688 B CCM frame, 523 B transform buffers) |

The 1–1.5 KiB flash estimate of the Data Secure design (D1) holds, and the crypto adds no RAM
outside the stack.

## 6. Order of work

1. `aes128.cpp` with its unit tests and the bare-metal crypto test.
2. `knx_ccm.cpp`; the secure application layer and the BAU transforms in place (§3.3); Annex C and
   differential tests green.
3. The IP Secure users on `Aes128` (§3.4); remove `aes_block.hpp` and the backends.
4. CMake split (§3.5); the standalone build with Secure on and no mbedTLS for a TP1-only profile.
5. ESP32 check of §3.1 (time per telegram, hardware block against V3) before the ESP backend is
   gone for good.

## 7. Status

Done (branch `software-aes`):

- **AES (§3.1), with one change of plan.** V3 is `Aes128Soft` in `aes128_soft.hpp` /
  `src/security/aes128_soft.cpp`, the S-box generated by a `consteval` function and checked with a
  `static_assert`. The mbedTLS and ESP backends were **kept**, at the owner's request: the backend is
  the Kconfig choice `KNX_AES_BACKEND` (`SOFTWARE`, `MBEDTLS`, `ESP_HW`), not `__has_include`
  probing. Defaults: on ESP-IDF, `MBEDTLS` while it has the block API (mbedTLS 2/3, ESP-IDF 5:
  what the build used before, itself on the AES block), `ESP_HW` with mbedTLS 4 on a chip with the
  AES block, `SOFTWARE` everywhere else (an ESP32-C2 on ESP-IDF 6, which had no backend at all); the `host-test` and `full` profiles select `MBEDTLS`, `host-small` `SOFTWARE`.
  `Aes128BlockCipher` (`aes_block.hpp`) stays as the façade the CCM code uses, so its `Result`
  returns stay too; the software path has no error branch.
- **Build (§3.5, standalone).** `aes128_soft.cpp` is in `KNX_SOURCES_SECURE_DATA`. mbedTLS is
  required only for the mbedTLS backend or for `KNX_SOURCES_SECURE_IP_CRYPTO`, which the standalone
  build compiles with KNXnet/IP (as before for a KNXnet/IP build; the ESP-IDF branch keeps it to
  tunnelling). `KNX_SOURCES_SECURE_IP_AEAD` was not introduced: CBC-MAC and CTR still serve Data
  Secure.
- **Tests (§4, host part).** `test_aes128_soft`: FIPS-197 Appendix B and C.1, SP 800-38A F.5.1
  CTR, in place, key change, and 500 chained keys against the configured backend (the differential
  test, on a host with mbedTLS). The Annex C vectors of `test_secure_application_layer` run on the
  software backend in the `host-small` profile.

Measured (Arm GNU Toolchain 13.2.Rel1, Cortex-M3):

| | Figure |
|---|---:|
| `aes128_soft.cpp` object, `-Os`, no LTO (encrypt, setKey, destructor, S-box) | 848 B |
| In the LTO image of the size gate: `encrypt` 500 + S-box 256 + `setKey` 24 | 780 B |
| Stack of `encrypt` | 64 B |

Not done:

- **In-place CCM (§3.2) and the transforms (§3.3), so D7.** They change the secure application
  layer's and the BAU's TX/RX paths, which only the host tests and an ESP32 run cover today; not a
  low-risk change next to the backend work. The existing CCM (`ccmEncrypt`/`ccmDecrypt`, CBC-MAC
  and CTR on `Aes128BlockCipher`) runs on the software AES unchanged.
- **The bare-metal crypto test** (`test/baremetal/crypto/`). FIPS-197 C.1, Appendix B and one
  SP 800-38A block were run once on QEMU `mps2-an385` and `mps2-an386` with a throwaway harness
  (pass); nothing in CI runs AES on a Cortex-M, and no instruction-count bound.
- **ESP32 measurement (step 5).** ESP-IDF builds were not run here. The defaults keep what an
  ESP-IDF build chose before (mbedTLS's AES on ESP-IDF 5, `esp_aes` on ESP-IDF 6); whether
  `ESP_HW` or `SOFTWARE` is faster for single blocks is still to be measured on an ESP32.
