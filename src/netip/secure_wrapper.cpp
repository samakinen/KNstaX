// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/security/aes_block.hpp"
#include "knx/security/secure_memory.hpp"

#include <cstring>
#include <algorithm>
#include <ranges>

namespace knx {
namespace netip {
namespace ip_secure {

using knx::security::Aes128BlockCipher;
using knx::security::Aes128CbcMac;
using knx::security::Aes128Ctr;

namespace {

constexpr size_t kSessionIdOffset = SecureWrapper::kHeaderLen;
constexpr size_t kSessionIdLen = 2;
constexpr size_t kSeqOffset = kSessionIdOffset + kSessionIdLen;
constexpr size_t kSerialOffset = kSeqOffset + SecureWrapper::kSeqLen;
constexpr size_t kTagOffset = kSerialOffset + SecureWrapper::kSerialLen;
constexpr size_t kWrapperBodyOffset = SecureWrapper::kHeaderLen + kSessionIdLen + SecureWrapper::kSeqLen + SecureWrapper::kSerialLen + SecureWrapper::kTagLen;

// TIMER_NOTIFY has no session id: the security information follows the header.
constexpr size_t kNotifyTimerOffset = SecureWrapper::kHeaderLen;
constexpr size_t kNotifySerialOffset = kNotifyTimerOffset + SecureWrapper::kSeqLen;
constexpr size_t kNotifyTagOffset = kNotifySerialOffset + SecureWrapper::kSerialLen;
constexpr size_t kNotifyMacOffset = kNotifyTagOffset + SecureWrapper::kTagLen;
static_assert(kNotifyMacOffset + SecureWrapper::kMacLen == SecureWrapper::kTimerNotifyLen);

static void writeHeader(std::span<uint8_t, SecureWrapper::kHeaderLen> header, uint16_t totalLen)
{
    const size_t payloadLen = totalLen - SecureWrapper::kHeaderLen;
    (void)KnxNetIpCodec::encodeHeader(SecureWrapper::kServiceTypeSecureWrapper, payloadLen, header);
}

static void writeSessionId(std::span<uint8_t, kSessionIdLen> out, SessionId sessionId)
{
    out[0] = static_cast<uint8_t>((sessionId.value() >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(sessionId.value() & 0xFF);
}

static std::array<uint8_t, SecureWrapper::kHeaderLen + kSessionIdLen> makeAdditionalData(std::span<const uint8_t, SecureWrapper::kHeaderLen> header,
                                                                              SessionId sessionId)
{
    std::array<uint8_t, SecureWrapper::kHeaderLen + kSessionIdLen> additionalData{};
    std::ranges::copy(header, additionalData.begin());
    writeSessionId(std::span<uint8_t, kSessionIdLen>(additionalData.data() + SecureWrapper::kHeaderLen, kSessionIdLen), sessionId);
    return additionalData;
}

static Aes128CbcMac::Block makeBlock0(std::span<const uint8_t, SecureWrapper::kSeqLen> seq,
                                      std::span<const uint8_t, SecureWrapper::kSerialLen> serial,
                                      std::span<const uint8_t, SecureWrapper::kTagLen> tag,
                                      size_t payloadLen)
{
    Aes128CbcMac::Block block0{};
    std::ranges::copy(seq, block0.begin());
    std::ranges::copy(serial, block0.begin() + SecureWrapper::kSeqLen);
    block0[12] = tag[0];
    block0[13] = tag[1];
    block0[14] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    block0[15] = static_cast<uint8_t>(payloadLen & 0xFF);
    return block0;
}

static void incrementCounter(Aes128Ctr::Counter& counter)
{
    for (size_t i = counter.size(); i > 0; --i) {
        ++counter[i - 1];
        if (counter[i - 1] != 0) {
            return;
        }
    }
}

static Aes128Ctr::Counter makeCounter(std::span<const uint8_t, SecureWrapper::kSeqLen> seq,
                                      std::span<const uint8_t, SecureWrapper::kSerialLen> serial,
                                      std::span<const uint8_t, 4> suffix)
{
    Aes128Ctr::Counter counter{};
    std::ranges::copy(seq, counter.begin());
    std::ranges::copy(serial, counter.begin() + SecureWrapper::kSeqLen);
    std::ranges::copy(suffix, counter.begin() + SecureWrapper::kSeqLen + SecureWrapper::kSerialLen);
    return counter;
}

} // namespace

util::Result<void> SecureWrapper::decodeHeader_(std::span<const uint8_t> in, NetIpServiceType& serviceType, uint16_t& totalLen)
{
    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(in, header);
    if (headerResult.isError()) return headerResult.error();
    if (header.totalLength > in.size()) return util::ErrorCode::InvalidFrameSize;
    serviceType = header.serviceType;
    totalLen = header.totalLength;
    return util::Result<void>::ok();
}

util::Result<size_t> SecureWrapper::wrap(const Key& key,
                                         SessionId sessionId,
                                         const std::array<uint8_t, kSeqLen>& seq,
                                         const std::array<uint8_t, kSerialLen>& serial,
                                         const std::array<uint8_t, kTagLen>& tag,
                                         const std::array<uint8_t, 4>& counterSuffix,
                                         std::span<const uint8_t> inner,
                                         std::span<uint8_t> outWrapper)
{
    Aes128BlockCipher cipher;
    auto keyResult = cipher.setKey(key);
    if (keyResult.isError()) return keyResult.error();
    return wrap(cipher, sessionId, seq, serial, tag, counterSuffix, inner, outWrapper);
}

util::Result<size_t> SecureWrapper::wrap(Aes128BlockCipher& cipher,
                                         SessionId sessionId,
                                         const std::array<uint8_t, kSeqLen>& seq,
                                         const std::array<uint8_t, kSerialLen>& serial,
                                         const std::array<uint8_t, kTagLen>& tag,
                                         const std::array<uint8_t, 4>& counterSuffix,
                                         std::span<const uint8_t> inner,
                                         std::span<uint8_t> outWrapper)
{
    // The total length is a 16-bit header field. Checked before narrowing: an
    // inner datagram near 64 KiB would otherwise wrap round to a small total,
    // pass the buffer check below, and be written past the end of the output.
    if (inner.size() > kMaxTotalLen - kOverhead) return util::ErrorCode::InvalidFrameSize;
    const uint16_t totalLen = static_cast<uint16_t>(kOverhead + inner.size());
    if (outWrapper.size() < totalLen) return util::ErrorCode::BufferTooSmall;

    writeHeader(std::span<uint8_t, kHeaderLen>(outWrapper.data(), kHeaderLen), totalLen);
    writeSessionId(std::span<uint8_t, kSessionIdLen>(outWrapper.data() + kSessionIdOffset, kSessionIdLen), sessionId);
    std::ranges::copy(seq, outWrapper.subspan(kSeqOffset, seq.size()).begin());
    std::ranges::copy(serial, outWrapper.subspan(kSerialOffset, serial.size()).begin());
    std::ranges::copy(tag, outWrapper.subspan(kTagOffset, tag.size()).begin());

    const auto additionalData = makeAdditionalData(std::span<const uint8_t, kHeaderLen>(outWrapper.data(), kHeaderLen), sessionId);
    const auto block0 = makeBlock0(seq, serial, tag, inner.size());

    Aes128CbcMac::Block macCbc{};
    auto macResult = Aes128CbcMac::compute(cipher, block0, additionalData, inner, macCbc);
    if (macResult.isError()) return macResult.error();

    auto ctr0 = makeCounter(seq, serial, counterSuffix);

    std::array<uint8_t, kMacLen> encMac{};
    auto encMacResult = Aes128Ctr::crypt(cipher, ctr0, macCbc, encMac);
    if (encMacResult.isError()) return encMacResult.error();

    if (!inner.empty()) {
        auto payloadCtr = ctr0;
        incrementCounter(payloadCtr);
        auto encPayloadResult = Aes128Ctr::crypt(cipher,
                                                 payloadCtr,
                                                 inner,
                                                 outWrapper.subspan(kWrapperBodyOffset, inner.size()));
        if (encPayloadResult.isError()) return encPayloadResult.error();
    }

    std::ranges::copy(
        encMac,
        outWrapper.subspan(kWrapperBodyOffset + inner.size(), encMac.size()).begin());

    return totalLen;
}

util::Result<size_t> SecureWrapper::unwrapAndVerify(const Key& key,
                                                    SessionId expectedSessionId,
                                                    std::span<const uint8_t> wrapper,
                                                    std::span<uint8_t> outPlaintext)
{
    Aes128BlockCipher cipher;
    auto keyResult = cipher.setKey(key);
    if (keyResult.isError()) return keyResult.error();
    return unwrapAndVerify(cipher, expectedSessionId, wrapper, outPlaintext, nullptr);
}

util::Result<size_t> SecureWrapper::unwrapAndVerify(Aes128BlockCipher& cipher,
                                                    SessionId expectedSessionId,
                                                    std::span<const uint8_t> wrapper,
                                                    std::span<uint8_t> outPlaintext,
                                                    Parsed* parsedOut)
{
    NetIpServiceType serviceType;
    uint16_t totalLen = 0;
    auto headerResult = decodeHeader_(wrapper, serviceType, totalLen);
    if (headerResult.isError()) return headerResult.error();
    if (serviceType != kServiceTypeSecureWrapper) return util::ErrorCode::DecodeFailed;
    if (totalLen < kOverhead) return util::ErrorCode::DecodeFailed;

    const size_t minLen = kOverhead;
    if (wrapper.size() < minLen) return util::ErrorCode::InvalidFrameSize;

    const size_t encLen = totalLen - kWrapperBodyOffset;
    if (encLen < kMacLen) return util::ErrorCode::DecodeFailed;

    const size_t innerLen = encLen - kMacLen;
    if (outPlaintext.size() < innerLen) return util::ErrorCode::BufferTooSmall;

    // Parse wrapper fields
    const SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(wrapper[kSessionIdOffset]) << 8) |
                                              wrapper[kSessionIdOffset + 1]));
    if (sid != expectedSessionId) return util::ErrorCode::InvalidParameter;

    std::array<uint8_t, kSeqLen> seq{};
    std::array<uint8_t, kSerialLen> serial{};
    std::array<uint8_t, kTagLen> tag{};

    std::ranges::copy(wrapper.subspan(kSeqOffset, kSeqLen), seq.begin());
    std::ranges::copy(wrapper.subspan(kSerialOffset, kSerialLen), serial.begin());
    std::ranges::copy(wrapper.subspan(kTagOffset, kTagLen), tag.begin());

    std::array<uint8_t, 4> suffix{};
    if (tag[0] == 0x00 && tag[1] == 0x00 && sid.isValid()) {
        suffix = {0x00, 0x00, 0xFF, 0x00};
    } else {
        suffix = {tag[0], tag[1], 0xFF, 0x00};
    }

    const auto ctr0 = makeCounter(seq, serial, suffix);

    Aes128CbcMac::Block macTr{};
    auto decMacResult = Aes128Ctr::crypt(cipher,
                                         ctr0,
                                         wrapper.subspan(kWrapperBodyOffset + innerLen, kMacLen),
                                         macTr);
    if (decMacResult.isError()) return decMacResult.error();

    // Whatever lands in the caller's buffer is unauthenticated until the MAC
    // has matched, so every failure from here on wipes it: a caller that
    // ignores the error must not find a decryption of a forged frame there.
    const auto plainOut = outPlaintext.first(innerLen);
    if (innerLen > 0) {
        auto payloadCtr = ctr0;
        incrementCounter(payloadCtr);
        auto decPayloadResult = Aes128Ctr::crypt(cipher,
                                                 payloadCtr,
                                                 wrapper.subspan(kWrapperBodyOffset, innerLen),
                                                 plainOut);
        if (decPayloadResult.isError()) {
            knx::security::secureZero(plainOut);
            return decPayloadResult.error();
        }
    }

    const auto additionalData = makeAdditionalData(std::span<const uint8_t, kHeaderLen>(wrapper.data(), kHeaderLen), sid);
    const auto block0 = makeBlock0(seq, serial, tag, innerLen);

    Aes128CbcMac::Block macCbc{};
    auto macResult = Aes128CbcMac::compute(cipher, block0, additionalData, plainOut, macCbc);
    if (macResult.isError()) {
        knx::security::secureZero(plainOut);
        return macResult.error();
    }
    if (!knx::security::constantTimeEqual(macCbc, macTr)) {
        knx::security::secureZero(plainOut);
        return util::ErrorCode::DecodeFailed;
    }

    if (parsedOut != nullptr) {
        parsedOut->sessionId = sid;
        parsedOut->seq = seq;
        parsedOut->serial = serial;
        parsedOut->tag = tag;
    }
    return innerLen;
}

// TIMER_NOTIFY (03_08_09 §2.2.2.4) is the CCM of the SECURE_WRAPPER with the
// payload left empty: authenticated, not encrypted. A is the 6-octet header on
// its own, there being no session id (§2.2.2.4.3); B0 is timer | serial | tag |
// Q = 0000h (Figure 12) and Ctr0 is timer | serial | tag | FFh 00h (Figure 13).
util::Result<size_t> SecureWrapper::encodeTimerNotify(Aes128BlockCipher& cipher,
                                                      const TimerNotify& notify,
                                                      std::span<uint8_t> out)
{
    if (out.size() < kTimerNotifyLen) return util::ErrorCode::BufferTooSmall;

    auto headerResult = KnxNetIpCodec::encodeHeader(kServiceTypeTimerNotify,
                                                    kTimerNotifyLen - kHeaderLen,
                                                    out.first<kHeaderLen>());
    if (headerResult.isError()) return headerResult.error();
    std::ranges::copy(notify.timer, out.begin() + kNotifyTimerOffset);
    std::ranges::copy(notify.serial, out.begin() + kNotifySerialOffset);
    std::ranges::copy(notify.tag, out.begin() + kNotifyTagOffset);

    const auto block0 = makeBlock0(notify.timer, notify.serial, notify.tag, 0);
    Aes128CbcMac::Block macCbc{};
    auto macResult = Aes128CbcMac::compute(cipher,
                                           block0,
                                           std::span<const uint8_t>(out.first(kHeaderLen)),
                                           {},
                                           macCbc);
    if (macResult.isError()) return macResult.error();

    const std::array<uint8_t, 4> suffix{notify.tag[0], notify.tag[1], 0xFF, 0x00};
    const auto ctr0 = makeCounter(notify.timer, notify.serial, suffix);
    auto encResult = Aes128Ctr::crypt(cipher, ctr0, macCbc, out.subspan(kNotifyMacOffset, kMacLen));
    if (encResult.isError()) return encResult.error();

    return kTimerNotifyLen;
}

util::Result<SecureWrapper::TimerNotify> SecureWrapper::decodeTimerNotify(Aes128BlockCipher& cipher,
                                                                           std::span<const uint8_t> datagram)
{
    NetIpServiceType serviceType;
    uint16_t totalLen = 0;
    auto headerResult = decodeHeader_(datagram, serviceType, totalLen);
    if (headerResult.isError()) return headerResult.error();
    if (serviceType != kServiceTypeTimerNotify) return util::ErrorCode::DecodeFailed;
    // "Received KNXnet/IP Timer Notify frames larger or smaller than 36 octets
    // shall be discarded" (§2.2.2.4.4).
    if (totalLen != kTimerNotifyLen) return util::ErrorCode::InvalidFrameSize;

    TimerNotify notify{};
    std::ranges::copy(datagram.subspan(kNotifyTimerOffset, kSeqLen), notify.timer.begin());
    std::ranges::copy(datagram.subspan(kNotifySerialOffset, kSerialLen), notify.serial.begin());
    std::ranges::copy(datagram.subspan(kNotifyTagOffset, kTagLen), notify.tag.begin());

    const std::array<uint8_t, 4> suffix{notify.tag[0], notify.tag[1], 0xFF, 0x00};
    const auto ctr0 = makeCounter(notify.timer, notify.serial, suffix);
    Aes128CbcMac::Block macTr{};
    auto decResult = Aes128Ctr::crypt(cipher, ctr0, datagram.subspan(kNotifyMacOffset, kMacLen), macTr);
    if (decResult.isError()) return decResult.error();

    // The MAC is checked before the timer value means anything: an outdated
    // value still drives the synchronisation, so only an authentic one may
    // (§2.2.2.4.4, "Sequence Information").
    const auto block0 = makeBlock0(notify.timer, notify.serial, notify.tag, 0);
    Aes128CbcMac::Block macCbc{};
    auto macResult = Aes128CbcMac::compute(cipher, block0, datagram.first(kHeaderLen), {}, macCbc);
    if (macResult.isError()) return macResult.error();
    if (!knx::security::constantTimeEqual(macCbc, macTr)) return util::ErrorCode::DecodeFailed;

    return notify;
}

} // namespace ip_secure
} // namespace netip
} // namespace knx
