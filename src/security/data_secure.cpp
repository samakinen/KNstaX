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

DataSecureSession::DataSecureSession(const Key& key) : key_(key) {}

void DataSecureSession::setSendSeq(uint64_t seq48) {
    sendSeq48_ = mask48(seq48);
}

constexpr void DataSecureSession::writeU16BE(std::span<uint8_t, 2> out, uint16_t v) noexcept {
    out[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(v & 0xFF);
}

constexpr void DataSecureSession::writeU48BE(std::span<uint8_t, 6> out, uint64_t v) noexcept {
    const uint64_t x = mask48(v);
    out[0] = static_cast<uint8_t>((x >> 40) & 0xFF);
    out[1] = static_cast<uint8_t>((x >> 32) & 0xFF);
    out[2] = static_cast<uint8_t>((x >> 24) & 0xFF);
    out[3] = static_cast<uint8_t>((x >> 16) & 0xFF);
    out[4] = static_cast<uint8_t>((x >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(x & 0xFF);
}

SecureBlockContext DataSecureSession::blockContext(const DataSecureContext& ctx) {
    SecureBlockContext blockCtx;
    blockCtx.srcIa = ctx.srcIa;
    blockCtx.dstRaw = ctx.dstAddr.raw;
    blockCtx.atOctet = static_cast<uint8_t>(ctx.cemiCtrl2 & CTRL2_RELEVANT_MASK);
    blockCtx.tpci6 = 0;  // group data is always T_Data_Group, TPCI = 0
    return blockCtx;
}

util::Result<void> DataSecureSession::buildBlock0(const DataSecureContext& ctx,
                                                 size_t plainApduLen,
                                                 std::array<uint8_t, 16>& block0) {
    if (!ctx.dstAddr.isValid()) return util::ErrorCode::InvalidAddress;

    std::array<uint8_t, 6> nonce{};
    writeSecureU48BE(std::span<uint8_t, 6>(nonce), ctx.seq48);
    return buildSecureBlock0(blockContext(ctx), std::span<const uint8_t, 6>(nonce), plainApduLen, block0);
}

util::Result<void> DataSecureSession::buildCounter0(const DataSecureContext& ctx,
                                                   std::array<uint8_t, 16>& counter0) {
    if (!ctx.dstAddr.isValid()) return util::ErrorCode::InvalidAddress;

    std::array<uint8_t, 6> nonce{};
    writeSecureU48BE(std::span<uint8_t, 6>(nonce), ctx.seq48);
    return buildSecureCounter0(blockContext(ctx), std::span<const uint8_t, 6>(nonce), counter0);
}

util::Result<size_t> DataSecureSession::protect(const DataSecureContext& frameCtx,
                                                std::span<const uint8_t> plainApdu,
                                                std::span<uint8_t> secureTpdu) {
    if (plainApdu.size() < 2) return util::ErrorCode::InvalidFrameSize;
    if (plainApdu.size() > kMaxPlainApduSize) return util::ErrorCode::InvalidParameter;

    const size_t secureLen = protectedTpduSize(plainApdu.size());
    if (secureTpdu.size() < secureLen) return util::ErrorCode::BufferTooSmall;

    DataSecureContext ctx = frameCtx;

    uint64_t seq48 = mask48(ctx.seq48);
    if (seq48 == 0) {
        seq48 = mask48(sendSeq48_ + 1ULL);
        if (seq48 == 0) return util::ErrorCode::OperationFailed;
    }

    if (sendSeq48_ != 0 && seq48 <= mask48(sendSeq48_)) {
        return util::ErrorCode::InvalidParameter;
    }
    ctx.seq48 = seq48;

    std::array<uint8_t, 16> block0{};
    auto blockResult = buildBlock0(ctx, plainApdu.size(), block0);
    if (blockResult.isError()) return blockResult.error();

    std::array<uint8_t, 16> macFull{};
    {
        Aes128CbcMac::Block mac;
        const std::array<uint8_t, 1> additionalData{SCF_ENCRYPTION_S_A_DATA};
        auto macResult = Aes128CbcMac::compute(key_, block0, additionalData, plainApdu, mac);
        if (macResult.isError()) {
            return macResult.error();
        }
        macFull = mac;
    }

    std::array<uint8_t, 16> counter0{};
    auto counterResult = buildCounter0(ctx, counter0);
    if (counterResult.isError()) return counterResult.error();

    std::array<uint8_t, kMacSize + kMaxPlainApduSize> combined{};
    std::array<uint8_t, kMacSize + kMaxPlainApduSize> combinedEnc{};
    std::memcpy(combined.data(), macFull.data(), kMacSize);
    if (!plainApdu.empty()) {
        std::memcpy(combined.data() + kMacSize, plainApdu.data(), plainApdu.size());
    }

    auto encResult = Aes128Ctr::crypt(key_,
                                      counter0,
                                      std::span<const uint8_t>(combined).first(kMacSize + plainApdu.size()),
                                      std::span<uint8_t>(combinedEnc).first(kMacSize + plainApdu.size()));
    if (encResult.isError()) {
        return encResult.error();
    }

    secureTpdu[0] = APCI_SEC_HIGH;
    secureTpdu[1] = APCI_SEC_LOW;
    secureTpdu[2] = SCF_ENCRYPTION_S_A_DATA;
    writeU48BE(std::span<uint8_t, 6>(secureTpdu.data() + 3, 6), ctx.seq48);
    if (!plainApdu.empty()) {
        std::memcpy(secureTpdu.data() + kSecureHeaderSize, combinedEnc.data() + kMacSize, plainApdu.size());
    }
    std::memcpy(secureTpdu.data() + kSecureHeaderSize + plainApdu.size(), combinedEnc.data(), kMacSize);

    sendSeq48_ = seq48;
    return secureLen;
}

util::Result<size_t> DataSecureSession::unprotect(const DataSecureContext& frameCtx,
                                                  std::span<const uint8_t> secureTpdu,
                                                  std::span<uint8_t> plainApdu) {
    if (secureTpdu.size() < kSecureOverhead) return util::ErrorCode::InvalidFrameSize;
    if (secureTpdu[0] != APCI_SEC_HIGH || secureTpdu[1] != APCI_SEC_LOW) return util::ErrorCode::DecodeFailed;

    const uint8_t scf = secureTpdu[2];
    if (scf != SCF_ENCRYPTION_S_A_DATA) return util::ErrorCode::DecodeFailed;

    uint64_t seq48 = 0;
    for (int i = 0; i < 6; ++i) {
        seq48 = (seq48 << 8) | secureTpdu[3 + i];
    }
    seq48 = mask48(seq48);
    if (seq48 == 0) return util::ErrorCode::DecodeFailed;

    if (!replay_.wouldAccept(seq48)) return util::ErrorCode::OperationNotReady;

    const size_t payloadStart = 3 + 6;
    if (secureTpdu.size() < payloadStart + kMacSize) return util::ErrorCode::InvalidFrameSize;

    const size_t encMacStart = secureTpdu.size() - kMacSize;
    const size_t encPayloadLen = encMacStart - payloadStart;
    if (plainApdu.size() < encPayloadLen) return util::ErrorCode::BufferTooSmall;

    DataSecureContext ctx = frameCtx;
    ctx.seq48 = seq48;

    std::array<uint8_t, 16> counter0{};
    auto counterResult = buildCounter0(ctx, counter0);
    if (counterResult.isError()) return counterResult.error();

    std::array<uint8_t, kMacSize + kMaxPlainApduSize> combinedEnc{};
    std::array<uint8_t, kMacSize + kMaxPlainApduSize> combinedPlain{};
    std::memcpy(combinedEnc.data(), secureTpdu.data() + encMacStart, kMacSize);
    if (encPayloadLen != 0) {
        std::memcpy(combinedEnc.data() + kMacSize, secureTpdu.data() + payloadStart, encPayloadLen);
    }

    auto decResult = Aes128Ctr::crypt(key_,
                                      counter0,
                                      std::span<const uint8_t>(combinedEnc).first(kMacSize + encPayloadLen),
                                      std::span<uint8_t>(combinedPlain).first(kMacSize + encPayloadLen));
    if (decResult.isError()) {
        return decResult.error();
    }

    std::array<uint8_t, kMacSize> macTr{};
    std::memcpy(macTr.data(), combinedPlain.data(), kMacSize);
    const auto plainView = std::span<const uint8_t>(combinedPlain).subspan(kMacSize, encPayloadLen);

    std::array<uint8_t, 16> block0{};
    auto blockResult = buildBlock0(ctx, plainView.size(), block0);
    if (blockResult.isError()) return blockResult.error();

    Aes128CbcMac::Block macFull{};
    const std::array<uint8_t, 1> additionalData{scf};
    auto macResult = Aes128CbcMac::compute(key_, block0, additionalData, plainView, macFull);
    if (macResult.isError()) {
        return macResult.error();
    }

    if (std::memcmp(macFull.data(), macTr.data(), kMacSize) != 0) {
        return util::ErrorCode::DecodeFailed;
    }

    if (encPayloadLen != 0) {
        std::memcpy(plainApdu.data(), plainView.data(), encPayloadLen);
    }

    replay_.accept(seq48);
    recvSeq48_ = seq48;
    return encPayloadLen;
}

} // namespace knx::security
