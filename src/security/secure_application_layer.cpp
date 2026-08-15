// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/secure_application_layer.hpp"

#include "knx/security/aes128_cbc_mac.hpp"
#include "knx/security/aes128_ctr.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <cstring>

static const char* TAG = "KNX.SecureAL";

namespace knx {
namespace security {

namespace {

constexpr uint8_t kScfToolAccess = 0x80;
constexpr uint8_t kScfAlgorithmShift = 4;
constexpr uint8_t kScfAlgorithmMask = 0x07;
constexpr uint8_t kScfSystemBroadcast = 0x08;
constexpr uint8_t kScfServiceMask = 0x07;

constexpr size_t kSecureApciSize = 2;   // 03 F1
constexpr size_t kScfSize = 1;
constexpr size_t kSerialNumberSize = 6;

constexpr size_t kMaxPlain = DataSecureSession::kMaxPlainApduSize;

// The first TPDU octet holds the six TPCI bits above the two high APCI bits, so
// the Secure APCI has to be matched with the TPCI masked off: on a transport
// connection the same APDU arrives as 43 F1 rather than 03 F1.
bool isSecureApdu(std::span<const uint8_t> tpdu) {
    return tpdu.size() >= kSecureApciSize
        && (tpdu[0] & 0x03) == DataSecureSession::APCI_SEC_HIGH
        && tpdu[1] == DataSecureSession::APCI_SEC_LOW;
}

} // namespace

uint8_t SecurityControlField::encode() const noexcept {
    uint8_t raw = static_cast<uint8_t>(static_cast<uint8_t>(service) & kScfServiceMask);
    raw |= static_cast<uint8_t>((algorithm & kScfAlgorithmMask) << kScfAlgorithmShift);
    if (toolAccess) raw |= kScfToolAccess;
    if (systemBroadcast) raw |= kScfSystemBroadcast;
    return raw;
}

std::optional<SecurityControlField> SecurityControlField::decode(uint8_t raw) noexcept {
    SecurityControlField scf;
    scf.toolAccess = (raw & kScfToolAccess) != 0;
    scf.algorithm = static_cast<uint8_t>((raw >> kScfAlgorithmShift) & kScfAlgorithmMask);
    scf.systemBroadcast = (raw & kScfSystemBroadcast) != 0;

    switch (raw & kScfServiceMask) {
        case 0: scf.service = SecureService::Data; break;
        case 2: scf.service = SecureService::SyncRequest; break;
        case 3: scf.service = SecureService::SyncResponse; break;
        default:
            return std::nullopt;  // reserved service
    }

    // Only CCM with authentication and confidentiality is used by the services
    // implemented here; a reserved SAI must be ignored (§5.1.4).
    if (scf.algorithm != kAlgorithmAuthConf) {
        return std::nullopt;
    }
    return scf;
}

SecureApplicationLayer::SecureApplicationLayer(objects::SecurityInterfaceObject& securityObject,
                                               Clock clock,
                                               RandomSource randomSource)
    : _securityObject(securityObject)
    , _clock(std::move(clock))
    , _randomSource(std::move(randomSource))
{
}

SecureBlockContext SecureApplicationLayer::blockContext(const SecureFrameInfo& info) const {
    SecureBlockContext ctx;
    ctx.srcIa = info.source;
    ctx.dstRaw = info.destination;
    // AT octet A000EEEEb: EFF is 0 for standard and plain extended frames.
    ctx.atOctet = info.destinationType == AddressType::Group ? 0x80 : 0x00;
    ctx.tpci6 = info.tpci6;
    return ctx;
}

util::Result<SecureApplicationLayer::Key> SecureApplicationLayer::selectKey(
    const SecureFrameInfo& info,
    const IndividualAddress& peer,
    bool toolAccess) const
{
    Key key{};
    if (toolAccess) {
        if (!_securityObject.hasToolKey()) {
            return util::ErrorCode::OperationNotReady;
        }
        key = _securityObject.getToolKey();
        return key;
    }

    if (info.destinationType == AddressType::Group && info.destination != 0) {
        auto result = _securityObject.getGroupKey(GroupAddress(info.destination), key);
        if (result.isError()) {
            return result.error();
        }
        return key;
    }

    auto result = _securityObject.getDeviceKey(peer, key);
    if (result.isError()) {
        return result.error();
    }
    return key;
}

util::Result<void> SecureApplicationLayer::ccmDecrypt(const Key& key,
                                                      const SecureBlockContext& ctx,
                                                      std::span<const uint8_t, 6> nonce,
                                                      uint8_t scf,
                                                      std::span<const uint8_t> aTail,
                                                      std::span<const uint8_t> cipher,
                                                      std::span<const uint8_t, kMacSize> mac,
                                                      std::span<uint8_t> plainOut)
{
    if (cipher.size() > kMaxPlain || plainOut.size() < cipher.size()) {
        return util::ErrorCode::BufferTooSmall;
    }

    std::array<uint8_t, 16> counter0{};
    auto counterResult = buildSecureCounter0(ctx, nonce, counter0);
    if (counterResult.isError()) return counterResult.error();

    // The MAC occupies the first four octets of the S0 keystream and the
    // payload continues in the same block (03/03/07 Annex C: C = (P XOR
    // MSB(S)) | MAC with S = LSB96(S0) | S1 | ...), so both are decrypted as
    // one run starting at Ctr0.
    std::array<uint8_t, kMacSize + kMaxPlain> combinedIn{};
    std::array<uint8_t, kMacSize + kMaxPlain> combinedOut{};
    std::memcpy(combinedIn.data(), mac.data(), kMacSize);
    if (!cipher.empty()) {
        std::memcpy(combinedIn.data() + kMacSize, cipher.data(), cipher.size());
    }

    const size_t combinedLen = kMacSize + cipher.size();
    auto decResult = Aes128Ctr::crypt(key,
                                      counter0,
                                      std::span<const uint8_t>(combinedIn).first(combinedLen),
                                      std::span<uint8_t>(combinedOut).first(combinedLen));
    if (decResult.isError()) return decResult.error();

    const auto plainView = std::span<const uint8_t>(combinedOut).subspan(kMacSize, cipher.size());

    std::array<uint8_t, 16> block0{};
    auto blockResult = buildSecureBlock0(ctx, nonce, plainView.size(), block0);
    if (blockResult.isError()) return blockResult.error();

    std::array<uint8_t, 1 + kSerialNumberSize> associated{};
    associated[0] = scf;
    if (!aTail.empty()) {
        if (aTail.size() > kSerialNumberSize) return util::ErrorCode::InvalidParameter;
        std::memcpy(associated.data() + 1, aTail.data(), aTail.size());
    }

    Aes128CbcMac::Block computed{};
    auto macResult = Aes128CbcMac::compute(key,
                                           block0,
                                           std::span<const uint8_t>(associated).first(1 + aTail.size()),
                                           plainView,
                                           computed);
    if (macResult.isError()) return macResult.error();

    if (std::memcmp(computed.data(), combinedOut.data(), kMacSize) != 0) {
        return util::ErrorCode::DecodeFailed;
    }

    if (!plainView.empty()) {
        std::memcpy(plainOut.data(), plainView.data(), plainView.size());
    }
    return util::Result<void>::ok();
}

util::Result<void> SecureApplicationLayer::ccmEncrypt(const Key& key,
                                                      const SecureBlockContext& ctx,
                                                      std::span<const uint8_t, 6> nonce,
                                                      uint8_t scf,
                                                      std::span<const uint8_t> aTail,
                                                      std::span<const uint8_t> plain,
                                                      std::span<uint8_t> cipherOut)
{
    if (plain.size() > kMaxPlain || cipherOut.size() < plain.size() + kMacSize) {
        return util::ErrorCode::BufferTooSmall;
    }

    std::array<uint8_t, 16> block0{};
    auto blockResult = buildSecureBlock0(ctx, nonce, plain.size(), block0);
    if (blockResult.isError()) return blockResult.error();

    std::array<uint8_t, 1 + kSerialNumberSize> associated{};
    associated[0] = scf;
    if (!aTail.empty()) {
        if (aTail.size() > kSerialNumberSize) return util::ErrorCode::InvalidParameter;
        std::memcpy(associated.data() + 1, aTail.data(), aTail.size());
    }

    Aes128CbcMac::Block mac{};
    auto macResult = Aes128CbcMac::compute(key,
                                           block0,
                                           std::span<const uint8_t>(associated).first(1 + aTail.size()),
                                           plain,
                                           mac);
    if (macResult.isError()) return macResult.error();

    std::array<uint8_t, 16> counter0{};
    auto counterResult = buildSecureCounter0(ctx, nonce, counter0);
    if (counterResult.isError()) return counterResult.error();

    std::array<uint8_t, kMacSize + kMaxPlain> combinedIn{};
    std::array<uint8_t, kMacSize + kMaxPlain> combinedOut{};
    std::memcpy(combinedIn.data(), mac.data(), kMacSize);
    if (!plain.empty()) {
        std::memcpy(combinedIn.data() + kMacSize, plain.data(), plain.size());
    }

    const size_t combinedLen = kMacSize + plain.size();
    auto encResult = Aes128Ctr::crypt(key,
                                      counter0,
                                      std::span<const uint8_t>(combinedIn).first(combinedLen),
                                      std::span<uint8_t>(combinedOut).first(combinedLen));
    if (encResult.isError()) return encResult.error();

    // On the wire the encrypted payload comes first and the encrypted MAC last.
    if (!plain.empty()) {
        std::memcpy(cipherOut.data(), combinedOut.data() + kMacSize, plain.size());
    }
    std::memcpy(cipherOut.data() + plain.size(), combinedOut.data(), kMacSize);
    return util::Result<void>::ok();
}

SecureRxResult SecureApplicationLayer::processIncoming(const SecureFrameInfo& info,
                                                       std::span<const uint8_t> tpdu,
                                                       std::span<uint8_t> plainOut)
{
    _inbound = InboundContext{true, false, false, info.source.raw};

    if (!isSecureApdu(tpdu)) {
        // A plain telegram towards a secured group address must not be
        // accepted: the key's existence is what makes security mandatory there.
        if (_securityObject.isSecurityEnabled()
                && info.destinationType == AddressType::Group
                && info.destination != 0) {
            Key key{};
            if (_securityObject.getGroupKey(GroupAddress(info.destination), key).isOk()) {
                _securityObject.logSecurityFailure(objects::SecurityFailure::AuthenticationFailed,
                                                   info.source.raw);
                return {SecureRxDisposition::Rejected, 0};
            }
        }
        return {SecureRxDisposition::Plain, 0};
    }

    if (!_securityObject.isSecurityEnabled()) {
        KNX_LOGW(TAG, "Secure APDU from 0x%04X but security is disabled", info.source.raw);
        return {SecureRxDisposition::Rejected, 0};
    }

    if (tpdu.size() < kSecureApciSize + kScfSize) {
        return {SecureRxDisposition::Rejected, 0};
    }

    const auto scf = SecurityControlField::decode(tpdu[kSecureApciSize]);
    if (!scf.has_value()) {
        // Reserved SCF field: ignore without logging a security failure (§5.1.3.5.2).
        return {SecureRxDisposition::Rejected, 0};
    }

    _inbound.secured = true;
    _inbound.toolAccess = scf->toolAccess;

    switch (scf->service) {
        case SecureService::Data: {
            SecureRxResult result = handleData(info, *scf, tpdu, plainOut);
            if (result.disposition == SecureRxDisposition::Unwrapped) {
                // The one point where "this APDU was verified, and with what"
                // is known. Everything above only sees the plain APDU, so the
                // Access Policies have to travel with it from here.
                result.security.secured = true;
                result.security.toolAccess = scf->toolAccess;
                result.security.confidentiality =
                    scf->algorithm == SecurityControlField::kAlgorithmAuthConf;
            }
            return result;
        }
        case SecureService::SyncRequest:
            return handleSyncRequest(info, *scf, tpdu);
        case SecureService::SyncResponse:
            // This device never initiates an S-A_Sync, so a response is not
            // addressed to any outstanding request of ours.
            return {SecureRxDisposition::Consumed, 0};
    }
    return {SecureRxDisposition::Rejected, 0};
}

SecureRxResult SecureApplicationLayer::handleData(const SecureFrameInfo& info,
                                                   const SecurityControlField& scf,
                                                   std::span<const uint8_t> tpdu,
                                                   std::span<uint8_t> plainOut)
{
    // Group-addressed and point-to-point S-A_Data are the same check: the Last
    // Valid SeqNr is kept per *sender* in the Security Individual Address Table
    // (03/05/01 §6.3.8), not per destination. selectKey() below picks the group
    // key when the destination is a group address.
    constexpr size_t kHeader = kSecureApciSize + kScfSize + kSequenceSize;
    if (tpdu.size() < kHeader + kMacSize) {
        return {SecureRxDisposition::Rejected, 0};
    }

    const uint64_t seq = readSecureU48BE(
        std::span<const uint8_t, 6>(tpdu.data() + kSecureApciSize + kScfSize, 6));
    if (seq == 0) {
        return {SecureRxDisposition::Rejected, 0};
    }

    const uint64_t lastValid = scf.toolAccess ? _securityObject.getToolAccessSequence()
                                              : _securityObject.getPeerSequence(info.source);
    if (seq <= lastValid) {
        KNX_LOGW(TAG, "Secure data from 0x%04X rejected: SeqNr %llu not newer than %llu",
                 info.source.raw,
                 static_cast<unsigned long long>(seq),
                 static_cast<unsigned long long>(lastValid));
        _securityObject.logSecurityFailure(objects::SecurityFailure::SequenceNumberInvalid,
                                           info.source.raw);
        return {SecureRxDisposition::Rejected, 0};
    }

    auto keyResult = selectKey(info, info.source, scf.toolAccess);
    if (keyResult.isError()) {
        _securityObject.logSecurityFailure(objects::SecurityFailure::UnknownKey, info.source.raw);
        return {SecureRxDisposition::Rejected, 0};
    }

    const size_t cipherLen = tpdu.size() - kHeader - kMacSize;
    std::array<uint8_t, 6> nonce{};
    writeSecureU48BE(std::span<uint8_t, 6>(nonce), seq);

    auto decResult = ccmDecrypt(keyResult.value(),
                                blockContext(info),
                                std::span<const uint8_t, 6>(nonce),
                                scf.encode(),
                                {},
                                tpdu.subspan(kHeader, cipherLen),
                                std::span<const uint8_t, kMacSize>(tpdu.data() + kHeader + cipherLen, kMacSize),
                                plainOut);
    if (decResult.isError()) {
        _securityObject.logSecurityFailure(objects::SecurityFailure::DecryptionFailed, info.source.raw);
        return {SecureRxDisposition::Rejected, 0};
    }

    if (scf.toolAccess) {
        _securityObject.setToolAccessSequence(seq);
    } else {
        _securityObject.setPeerSequence(info.source, seq);
    }
    // A peer context only steers how we protect a frame we address *back* to
    // that peer, so it is recorded for point-to-point traffic only. A multicast
    // sender is not a correspondent.
    if (info.destinationType != AddressType::Group || info.destination == 0) {
        rememberPeer(info.source, scf.toolAccess);
    }

    return {SecureRxDisposition::Unwrapped, cipherLen};
}

SecureRxResult SecureApplicationLayer::handleSyncRequest(const SecureFrameInfo& info,
                                                          const SecurityControlField& scf,
                                                          std::span<const uint8_t> tpdu)
{
    if (tpdu.size() != kSyncRequestSize) {
        return {SecureRxDisposition::Rejected, 0};
    }

    // §5.3.2 step 1: at most one response per second.
    const uint32_t now = _clock ? _clock() : 0u;
    if (_syncResponded && static_cast<uint32_t>(now - _lastSyncResponseMs) < 1000u) {
        return {SecureRxDisposition::Consumed, 0};
    }

    const uint8_t* cursor = tpdu.data() + kSecureApciSize + kScfSize;
    const uint64_t seqLocal = readSecureU48BE(std::span<const uint8_t, 6>(cursor, 6));
    cursor += kSequenceSize;
    const std::span<const uint8_t> serial(cursor, kSerialNumberSize);
    cursor += kSerialNumberSize;

    // §5.3.2 step 2: the serial number decides whether we are addressed, before
    // any crypto is done. On (system) broadcast it must be present and ours.
    const bool serialIsZero = std::all_of(serial.begin(), serial.end(),
                                          [](uint8_t b) { return b == 0; });
    if (info.isBroadcast()) {
        if (serialIsZero || std::memcmp(serial.data(), _serialNumber.data(), kSerialNumberSize) != 0) {
            return {SecureRxDisposition::Consumed, 0};
        }
    } else if (!serialIsZero
               && std::memcmp(serial.data(), _serialNumber.data(), kSerialNumberSize) != 0) {
        return {SecureRxDisposition::Consumed, 0};
    }

    if (seqLocal == 0) {
        return {SecureRxDisposition::Consumed, 0};
    }

    auto keyResult = selectKey(info, info.source, scf.toolAccess);
    if (keyResult.isError()) {
        KNX_LOGW(TAG, "S-A_Sync_Request from 0x%04X: no %s key",
                 info.source.raw, scf.toolAccess ? "tool" : "P2P");
        return {SecureRxDisposition::Consumed, 0};
    }
    const Key key = keyResult.value();

    std::array<uint8_t, 6> nonce{};
    writeSecureU48BE(std::span<uint8_t, 6>(nonce), seqLocal);

    std::array<uint8_t, kChallengeSize> challenge{};
    auto decResult = ccmDecrypt(key,
                                blockContext(info),
                                std::span<const uint8_t, 6>(nonce),
                                scf.encode(),
                                serial,
                                std::span<const uint8_t>(cursor, kChallengeSize),
                                std::span<const uint8_t, kMacSize>(cursor + kChallengeSize, kMacSize),
                                challenge);
    if (decResult.isError()) {
        // §5.3.2 step 3: any verification error means no answer at all.
        KNX_LOGW(TAG, "S-A_Sync_Request from 0x%04X failed verification", info.source.raw);
        _securityObject.logSecurityFailure(objects::SecurityFailure::DecryptionFailed, info.source.raw);
        return {SecureRxDisposition::Consumed, 0};
    }

    // §5.3.2 step 4: SeqNrlocal is the *next* number the peer will use, while we
    // store the last one we accepted — hence the -1 / +1 around the comparison.
    const uint64_t storedLocal = scf.toolAccess ? _securityObject.getToolAccessSequence()
                                                : _securityObject.getPeerSequence(info.source);
    const uint64_t candidate = seqLocal - 1u;
    const uint64_t newStoredLocal = std::max(candidate, storedLocal);
    if (scf.toolAccess) {
        _securityObject.setToolAccessSequence(newStoredLocal);
    } else {
        _securityObject.setPeerSequence(info.source, newStoredLocal);
    }

    // SeqNrremote is the number we will use ourselves next. Sending a sync
    // response must not consume it (NOTE 44).
    const uint64_t seqRemote = _securityObject.getSendingSequenceThreadSafe() + 1u;

    std::array<uint8_t, kChallengeSize> random{};
    if (_randomSource) {
        _randomSource(random);
    }

    SecurityControlField responseScf = scf;
    responseScf.service = SecureService::SyncResponse;

    SecureFrameInfo outgoing;
    outgoing.source = _ownAddress;
    outgoing.connected = info.connected;
    outgoing.tpci6 = 0;
    if (info.isBroadcast()) {
        outgoing.destination = 0;
        outgoing.destinationType = AddressType::Group;
    } else {
        outgoing.destination = info.source.raw;
        outgoing.destinationType = AddressType::Individual;
    }
    if (outgoing.connected && _tpciResolver) {
        // The response travels with our own send sequence number, so the TPCI
        // the MAC covers has to come from the transport, not from the request.
        outgoing.tpci6 = _tpciResolver(outgoing);
    }

    std::array<uint8_t, 2 * kSequenceSize> payload{};
    writeSecureU48BE(std::span<uint8_t, 6>(payload.data(), 6), seqRemote);
    writeSecureU48BE(std::span<uint8_t, 6>(payload.data() + 6, 6), newStoredLocal + 1u);

    std::array<uint8_t, kSyncResponseSize> response{};
    response[0] = DataSecureSession::APCI_SEC_HIGH;
    response[1] = DataSecureSession::APCI_SEC_LOW;
    response[2] = responseScf.encode();
    for (size_t i = 0; i < kChallengeSize; ++i) {
        response[3 + i] = static_cast<uint8_t>(challenge[i] ^ random[i]);
    }

    auto encResult = ccmEncrypt(key,
                                blockContext(outgoing),
                                std::span<const uint8_t, 6>(random),
                                responseScf.encode(),
                                {},
                                payload,
                                std::span<uint8_t>(response).subspan(3 + kChallengeSize));
    if (encResult.isError()) {
        KNX_LOGE(TAG, "Failed to build S-A_Sync_Response: %s",
                 util::errorCodeToString(encResult.error()));
        return {SecureRxDisposition::Consumed, 0};
    }

    _lastSyncResponseMs = now;
    _syncResponded = true;
    rememberPeer(info.source, scf.toolAccess);

    if (_sink) {
        auto sendResult = _sink(outgoing, response);
        if (sendResult.isError()) {
            KNX_LOGW(TAG, "Failed to send S-A_Sync_Response to 0x%04X: %s",
                     info.source.raw, util::errorCodeToString(sendResult.error()));
        } else {
            KNX_LOGI(TAG, "S-A_Sync_Response to 0x%04X (SeqNr own=%llu, expected from peer=%llu)",
                     info.source.raw,
                     static_cast<unsigned long long>(seqRemote),
                     static_cast<unsigned long long>(newStoredLocal + 1u));
        }
    }

    return {SecureRxDisposition::Consumed, 0};
}

util::Result<size_t> SecureApplicationLayer::processOutgoing(const SecureFrameInfo& info,
                                                             std::span<const uint8_t> plainTpdu,
                                                             std::span<uint8_t> secureOut)
{
    if (!_securityObject.isSecurityEnabled() || plainTpdu.size() < 2) {
        return static_cast<size_t>(0);
    }
    // Already secured (an S-A_Sync_Response the S-AL built itself).
    if (isSecureApdu(plainTpdu)) {
        return static_cast<size_t>(0);
    }

    const bool group = info.destinationType == AddressType::Group && info.destination != 0;
    const bool broadcast = info.isBroadcast();
    const IndividualAddress peer(
        info.destinationType == AddressType::Individual ? info.destination
                                                        : _inbound.peer);

    bool toolAccess = false;
    if (!group) {
        // Answering the request we are currently handling: mirror its
        // protection exactly. This is the only signal available for a broadcast
        // answer, whose destination names no peer.
        const bool answeringInbound = _inbound.valid
            && (broadcast || _inbound.peer == info.destination);
        if (answeringInbound) {
            if (!_inbound.secured) {
                return static_cast<size_t>(0);
            }
            toolAccess = _inbound.toolAccess;
        } else if (broadcast) {
            // Not an answer: a spontaneous broadcast is never secured.
            return static_cast<size_t>(0);
        } else {
            const PeerContext* context = findPeer(peer);
            if (context == nullptr) {
                return static_cast<size_t>(0);
            }
            toolAccess = context->toolAccess;
        }
    }

    auto keyResult = selectKey(info, peer, toolAccess);
    if (keyResult.isError()) {
        // No key for this destination: plain is the correct behaviour.
        return static_cast<size_t>(0);
    }

    const size_t secureLen = DataSecureSession::protectedTpduSize(plainTpdu.size());
    if (secureOut.size() < secureLen) {
        return util::ErrorCode::BufferTooSmall;
    }

    SecurityControlField scf;
    scf.toolAccess = toolAccess;
    scf.service = SecureService::Data;

    const uint64_t seq = _securityObject.incrementSendingSequence();
    std::array<uint8_t, 6> nonce{};
    writeSecureU48BE(std::span<uint8_t, 6>(nonce), seq);

    secureOut[0] = DataSecureSession::APCI_SEC_HIGH;
    secureOut[1] = DataSecureSession::APCI_SEC_LOW;
    secureOut[2] = scf.encode();
    std::memcpy(secureOut.data() + 3, nonce.data(), nonce.size());

    auto encResult = ccmEncrypt(keyResult.value(),
                                blockContext(info),
                                std::span<const uint8_t, 6>(nonce),
                                scf.encode(),
                                {},
                                plainTpdu,
                                secureOut.subspan(3 + kSequenceSize));
    if (encResult.isError()) {
        return encResult.error();
    }
    return secureLen;
}

void SecureApplicationLayer::rememberPeer(const IndividualAddress& peer, bool toolAccess) {
    for (auto& entry : _peers) {
        if (entry.valid && entry.address == peer.raw) {
            entry.toolAccess = toolAccess;
            return;
        }
    }
    auto& slot = _peers[_nextPeerSlot];
    slot.address = peer.raw;
    slot.toolAccess = toolAccess;
    slot.valid = true;
    _nextPeerSlot = (_nextPeerSlot + 1) % kPeerContexts;
}

const SecureApplicationLayer::PeerContext* SecureApplicationLayer::findPeer(
    const IndividualAddress& peer) const
{
    for (const auto& entry : _peers) {
        if (entry.valid && entry.address == peer.raw) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace security
} // namespace knx
