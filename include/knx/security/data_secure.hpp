// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/security/aes128_cbc_mac.hpp"
#include "knx/security/aes128_ctr.hpp"
#include "knx/security/replay_window.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx::security {

/// Fields of the carrying KNX frame that the CCM blocks B0 / Ctr0 bind to
/// (03/03/07 §5.1.3.2, Figures 100 and 102).  Binding them is what stops a
/// secured APDU from being replayed towards a different address or on a
/// different transport service.
struct SecureBlockContext {
    IndividualAddress srcIa{};
    /// Destination address, raw: group address, individual address, or 0 for
    /// (system) broadcast.
    uint16_t dstRaw{0};
    /// AT octet, A000EEEEb: bit 7 is the Address Type of the frame, the low
    /// nibble the Extended Frame Format (0 for standard and plain extended
    /// frames).
    uint8_t atOctet{0x80};
    /// The six TPCI bits of the TPDU that carries the secure APDU: 0 for
    /// T_Data_Group / T_Data_Individual / T_Data_Broadcast, 0x10 | SeqNo for
    /// T_Data_Connected.
    uint8_t tpci6{0};
};

/// B0 = nonce(6) | SA(2) | DA(2) | 00 | AT | TPCI/APCISec(2) | 00 | q
util::Result<void> buildSecureBlock0(const SecureBlockContext& ctx,
                                     std::span<const uint8_t, 6> nonce,
                                     size_t payloadLen,
                                     std::array<uint8_t, 16>& block0);

/// Ctr0 = nonce(6) | SA(2) | DA(2) | 00 00 00 00 01 00
util::Result<void> buildSecureCounter0(const SecureBlockContext& ctx,
                                       std::span<const uint8_t, 6> nonce,
                                       std::array<uint8_t, 16>& counter0);

/// Write a 48-bit value big-endian into @p out.
void writeSecureU48BE(std::span<uint8_t, 6> out, uint64_t value) noexcept;

/// Read a big-endian 48-bit value.
uint64_t readSecureU48BE(std::span<const uint8_t, 6> in) noexcept;

struct DataSecureContext {
    IndividualAddress srcIa{};
    GroupAddress dstAddr{};
    uint8_t cemiCtrl2{0};

    // If non-zero, used as the sequence number embedded in the Secure APDU.
    // If zero, DataSecureSession will auto-assign/increment.
    uint64_t seq48{0};
};

class DataSecureSession {
public:
    using Key = std::array<uint8_t, 16>;

    static constexpr uint8_t SCF_ENCRYPTION_S_A_DATA = 0x10;
    static constexpr uint8_t APCI_SEC_HIGH = 0x03;
    static constexpr uint8_t APCI_SEC_LOW = 0xF1;
    static constexpr uint8_t CTRL2_RELEVANT_MASK = 0x8F;

    // 00 00 00 00 01 00
    static constexpr std::array<uint8_t, 6> DATA_SECURE_CTR_SUFFIX = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    static constexpr size_t kMacSize = 4;
    static constexpr size_t kSequenceSize = 6;
    static constexpr size_t kSecureHeaderSize = 3 + kSequenceSize;
    static constexpr size_t kSecureOverhead = kSecureHeaderSize + kMacSize;
    static constexpr size_t kMaxPlainApduSize = 0xFF;
    static constexpr size_t kMaxSecureTpduSize = kSecureOverhead + kMaxPlainApduSize;

    explicit DataSecureSession(const Key& key);

    [[nodiscard]] static constexpr size_t protectedTpduSize(size_t plainApduSize) noexcept
    {
        return kSecureOverhead + plainApduSize;
    }

    [[nodiscard]] static constexpr size_t unprotectedApduSize(size_t secureTpduSize) noexcept
    {
        return secureTpduSize - kSecureOverhead;
    }

    // Protect a plain TPDU APDU payload (tpci+apci+data) into a Secure TPDU:
    // [APCI_SEC_HIGH, APCI_SEC_LOW, SCF, seq48(6), encPayload, encMac4]
    util::Result<size_t> protect(const DataSecureContext& ctx,
                                 std::span<const uint8_t> plainApdu,
                                 std::span<uint8_t> secureTpdu);

    // Unprotect a Secure TPDU created by protect().
    util::Result<size_t> unprotect(const DataSecureContext& ctx,
                                   std::span<const uint8_t> secureTpdu,
                                   std::span<uint8_t> plainApdu);

    uint64_t sendSeq() const { return sendSeq48_ & 0x0000FFFFFFFFFFFFULL; }
    uint64_t recvSeq() const { return recvSeq48_ & 0x0000FFFFFFFFFFFFULL; }

    void setSendSeq(uint64_t seq48);

private:
    Key key_{};
    uint64_t sendSeq48_{0};
    uint64_t recvSeq48_{0};
    ReplayWindow replay_;

    static constexpr void writeU16BE(std::span<uint8_t, 2> out, uint16_t v) noexcept;
    static constexpr void writeU48BE(std::span<uint8_t, 6> out, uint64_t v) noexcept;

    static SecureBlockContext blockContext(const DataSecureContext& ctx);

    static util::Result<void> buildBlock0(const DataSecureContext& ctx,
                                          size_t plainApduLen,
                                          std::array<uint8_t, 16>& block0);

    static util::Result<void> buildCounter0(const DataSecureContext& ctx,
                                            std::array<uint8_t, 16>& counter0);
};

} // namespace knx::security
