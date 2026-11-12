// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/data_secure.hpp"

#include <cstring>

namespace knx::security {

static inline constexpr uint64_t mask48(uint64_t v) noexcept {
    return v & 0x0000FFFFFFFFFFFFULL;
}

void writeSecureU48BE(std::span<uint8_t, 6> out, uint64_t value) noexcept {
    const uint64_t x = mask48(value);
    out[0] = static_cast<uint8_t>((x >> 40) & 0xFF);
    out[1] = static_cast<uint8_t>((x >> 32) & 0xFF);
    out[2] = static_cast<uint8_t>((x >> 24) & 0xFF);
    out[3] = static_cast<uint8_t>((x >> 16) & 0xFF);
    out[4] = static_cast<uint8_t>((x >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(x & 0xFF);
}

uint64_t readSecureU48BE(std::span<const uint8_t, 6> in) noexcept {
    uint64_t value = 0;
    for (size_t i = 0; i < 6; ++i) {
        value = (value << 8) | in[i];
    }
    return value;
}

namespace {

void writeU16BE(std::span<uint8_t, 2> out, uint16_t v) noexcept {
    out[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(v & 0xFF);
}

} // namespace

namespace {

// A secured frame may legitimately come from — and be sent by — a device that
// has no individual address yet: 15.15.255 is the initial address every device
// transmits from until ETS assigns it one, and secure commissioning starts
// exactly there (the S-A_Sync_Response to ETS's broadcast S-A_Sync_Request is
// the device's first secured frame). IndividualAddress::isValid() rejects
// 15.15.255 because it is not an operational destination, which is the wrong
// question here: only 0.0.0 is never a source address.
bool isUsableSecureSource(const IndividualAddress& address) noexcept {
    return !isIndividualBroadcastAddress(address);
}

} // namespace

util::Result<void> buildSecureBlock0(const SecureBlockContext& ctx,
                                     std::span<const uint8_t, 6> nonce,
                                     size_t payloadLen,
                                     std::array<uint8_t, 16>& block0) {
    if (!isUsableSecureSource(ctx.srcIa)) return util::ErrorCode::InvalidAddress;
    if (payloadLen > DataSecureSession::kMaxPlainApduSize) return util::ErrorCode::InvalidParameter;

    std::memcpy(block0.data(), nonce.data(), nonce.size());
    writeU16BE(std::span<uint8_t, 2>(block0.data() + 6, 2), ctx.srcIa.raw);
    writeU16BE(std::span<uint8_t, 2>(block0.data() + 8, 2), ctx.dstRaw);
    block0[10] = 0x00;
    block0[11] = ctx.atOctet;
    // TPCI/APCISec: six TPCI bits followed by the ten-bit Secure APCI 0x3F1.
    block0[12] = static_cast<uint8_t>(((ctx.tpci6 & 0x3F) << 2) | DataSecureSession::APCI_SEC_HIGH);
    block0[13] = DataSecureSession::APCI_SEC_LOW;
    block0[14] = 0x00;
    block0[15] = static_cast<uint8_t>(payloadLen);
    return util::Result<void>::ok();
}

util::Result<void> buildSecureCounter0(const SecureBlockContext& ctx,
                                       std::span<const uint8_t, 6> nonce,
                                       std::array<uint8_t, 16>& counter0) {
    if (!isUsableSecureSource(ctx.srcIa)) return util::ErrorCode::InvalidAddress;

    std::memcpy(counter0.data(), nonce.data(), nonce.size());
    writeU16BE(std::span<uint8_t, 2>(counter0.data() + 6, 2), ctx.srcIa.raw);
    writeU16BE(std::span<uint8_t, 2>(counter0.data() + 8, 2), ctx.dstRaw);
    std::memcpy(counter0.data() + 10,
                DataSecureSession::DATA_SECURE_CTR_SUFFIX.data(),
                DataSecureSession::DATA_SECURE_CTR_SUFFIX.size());
    return util::Result<void>::ok();
}

} // namespace knx::security
