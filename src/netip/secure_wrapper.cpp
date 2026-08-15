// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/header_codec.hpp"

#include <cstring>
#include <algorithm>
#include <ranges>

namespace knx {
namespace netip {
namespace ip_secure {

using knx::security::Aes128CbcMac;
using knx::security::Aes128Ctr;

namespace {

constexpr size_t kSessionIdOffset = SecureWrapper::kHeaderLen;
constexpr size_t kSessionIdLen = 2;
constexpr size_t kSeqOffset = kSessionIdOffset + kSessionIdLen;
constexpr size_t kSerialOffset = kSeqOffset + SecureWrapper::kSeqLen;
constexpr size_t kTagOffset = kSerialOffset + SecureWrapper::kSerialLen;
constexpr size_t kWrapperBodyOffset = SecureWrapper::kHeaderLen + kSessionIdLen + SecureWrapper::kSeqLen + SecureWrapper::kSerialLen + SecureWrapper::kTagLen;

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
    auto macResult = Aes128CbcMac::compute(key, block0, additionalData, inner, macCbc);
    if (macResult.isError()) return macResult.error();

    auto ctr0 = makeCounter(seq, serial, counterSuffix);

    std::array<uint8_t, kMacLen> encMac{};
    auto encMacResult = Aes128Ctr::crypt(key, ctr0, macCbc, encMac);
    if (encMacResult.isError()) return encMacResult.error();

    if (!inner.empty()) {
        auto payloadCtr = ctr0;
        incrementCounter(payloadCtr);
        auto encPayloadResult = Aes128Ctr::crypt(key,
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
    const SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(wrapper[6]) << 8) | wrapper[7]));
    if (sid != expectedSessionId) return util::ErrorCode::InvalidParameter;

    std::array<uint8_t, kSeqLen> seq{};
    std::array<uint8_t, kSerialLen> serial{};
    std::array<uint8_t, kTagLen> tag{};

    std::ranges::copy(wrapper.subspan(8, kSeqLen), seq.begin());
    std::ranges::copy(wrapper.subspan(14, kSerialLen), serial.begin());
    std::ranges::copy(wrapper.subspan(20, kTagLen), tag.begin());

    std::array<uint8_t, 4> suffix{};
    if (tag[0] == 0x00 && tag[1] == 0x00 && sid.isValid()) {
        suffix = {0x00, 0x00, 0xFF, 0x00};
    } else {
        suffix = {tag[0], tag[1], 0xFF, 0x00};
    }

    const auto ctr0 = makeCounter(seq, serial, suffix);

    Aes128CbcMac::Block macTr{};
    auto decMacResult = Aes128Ctr::crypt(key,
                                         ctr0,
                                         wrapper.subspan(kWrapperBodyOffset + innerLen, kMacLen),
                                         macTr);
    if (decMacResult.isError()) return decMacResult.error();

    if (innerLen > 0) {
        auto payloadCtr = ctr0;
        incrementCounter(payloadCtr);
        auto decPayloadResult = Aes128Ctr::crypt(key,
                             payloadCtr,
                             wrapper.subspan(kWrapperBodyOffset, innerLen),
                             outPlaintext.subspan(0, innerLen));
        if (decPayloadResult.isError()) return decPayloadResult.error();
    }
    const std::span<const uint8_t> innerPlain(outPlaintext.data(), innerLen);

    const auto additionalData = makeAdditionalData(std::span<const uint8_t, kHeaderLen>(wrapper.data(), kHeaderLen), sid);
    const auto block0 = makeBlock0(seq, serial, tag, innerLen);

    Aes128CbcMac::Block macCbc{};
    auto macResult = Aes128CbcMac::compute(key, block0, additionalData, innerPlain, macCbc);
    if (macResult.isError()) return macResult.error();
    if (macCbc != macTr) return util::ErrorCode::DecodeFailed;
    return innerLen;
}

} // namespace ip_secure
} // namespace netip
} // namespace knx
