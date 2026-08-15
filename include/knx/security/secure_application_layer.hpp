// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file secure_application_layer.hpp
 * @brief KNX Data Secure application layer (S-AL), 03/03/07 §5.
 *
 * Sits between the transport layer and the application layer and implements the
 * two secure services a device has to support:
 *
 *  - **S-A_Data** (§5.2): unwraps inbound secured APDUs and wraps outbound ones,
 *    on group addresses (group key), on point-to-point frames (Tool Key or the
 *    peer's P2P key) and on broadcast.
 *  - **S-A_Sync** (§5.3.2): answers the S-A_Sync_Request that a Configuration
 *    Client (ETS) sends when it does not know which sequence numbers this device
 *    expects and will use. Without an answer to it, secure commissioning cannot
 *    start at all — ETS just retries the request and reports the device as not
 *    responding.
 *
 * The layer is deliberately free of transport types: callers describe the
 * carrying frame with SecureFrameInfo, and outgoing frames (the sync response)
 * leave through a sink the caller installs.
 */

#include "knx/objects/security_interface_object.hpp"
#include "knx/security/data_secure.hpp"
#include "knx/types.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace knx {
namespace security {

/// S-AL service, low three bits of the SCF (03/03/07 Figure 106).
enum class SecureService : uint8_t {
    Data = 0,
    SyncRequest = 2,
    SyncResponse = 3,
};

/// Security Control Field (03/03/07 §5.2.1.2, Figure 106).
struct SecurityControlField {
    /// CCM with authentication only.
    static constexpr uint8_t kAlgorithmAuthOnly = 0;
    /// CCM with authentication and confidentiality — the only mode KNX uses
    /// for the services implemented here.
    static constexpr uint8_t kAlgorithmAuthConf = 1;

    bool toolAccess{false};
    uint8_t algorithm{kAlgorithmAuthConf};
    bool systemBroadcast{false};
    SecureService service{SecureService::Data};

    [[nodiscard]] uint8_t encode() const noexcept;

    /// Decode @p raw, or nullopt when it names a reserved algorithm or service.
    [[nodiscard]] static std::optional<SecurityControlField> decode(uint8_t raw) noexcept;
};

/// The carrying frame's identity, as bound into the CCM blocks.
struct SecureFrameInfo {
    IndividualAddress source{};
    uint16_t destination{0};
    AddressType destinationType{AddressType::Group};
    /// TPCI bits of the carrying TPDU: 0 when connectionless, 0x10 | SeqNo on
    /// a transport connection.
    uint8_t tpci6{0};
    /// True when the frame travels on an open T_Connect connection.
    bool connected{false};

    [[nodiscard]] bool isBroadcast() const noexcept {
        return destinationType == AddressType::Group && destination == 0;
    }
};

enum class SecureRxDisposition : uint8_t {
    Plain,      ///< Not a secure APDU — deliver unchanged.
    Unwrapped,  ///< Verified and decrypted — deliver the plain APDU.
    Consumed,   ///< Handled by the S-AL itself (sync) — do not deliver.
    Rejected,   ///< Failed a security check — drop.
};

struct SecureRxResult {
    SecureRxDisposition disposition{SecureRxDisposition::Plain};
    size_t plainLength{0};
    /// How the APDU was protected, for the Access Policies enforced further up
    /// (03/4/1 §6.2). Only meaningful for Unwrapped; a Plain frame keeps the
    /// all-false default, which is what "no permissions beyond Unlisted" means.
    RequestSecurity security{};
};

class SecureApplicationLayer {
public:
    using Key = std::array<uint8_t, 16>;
    using SerialNumber = std::array<uint8_t, 6>;

    /// Emits a ready-made TPDU. The S-AL uses it for the S-A_Sync_Response.
    using FrameSink = util::InplaceFunction<
        util::Result<void>(const SecureFrameInfo&, std::span<const uint8_t>), 64>;
    /// Milliseconds since boot; only used for the sync response rate limit.
    using Clock = util::InplaceFunction<uint32_t(), 32>;
    /// Fills the buffer with unpredictable octets (the S-A_Sync_Response Random).
    using RandomSource = util::InplaceFunction<void(std::span<uint8_t>), 32>;
    /// Answers "which TPCI will this outgoing frame carry?". Needed because the
    /// CCM blocks bind the TPCI, and a response sent on a transport connection
    /// carries *our* sequence number, not the request's.
    using TpciResolver = util::InplaceFunction<uint8_t(const SecureFrameInfo&), 32>;

    static constexpr size_t kChallengeSize = 6;
    static constexpr size_t kSequenceSize = 6;
    static constexpr size_t kMacSize = 4;
    static constexpr size_t kSyncRequestSize = 2 + 1 + kSequenceSize + 6 + kChallengeSize + kMacSize;
    static constexpr size_t kSyncResponseSize = 2 + 1 + kChallengeSize + 2 * kSequenceSize + kMacSize;

    SecureApplicationLayer(objects::SecurityInterfaceObject& securityObject,
                           Clock clock,
                           RandomSource randomSource);

    void setOwnAddress(const IndividualAddress& address) { _ownAddress = address; }
    void setSerialNumber(const SerialNumber& serialNumber) { _serialNumber = serialNumber; }
    void setFrameSink(FrameSink sink) { _sink = std::move(sink); }
    void setTpciResolver(TpciResolver resolver) { _tpciResolver = std::move(resolver); }

    /**
     * @brief Handle an inbound TPDU.
     *
     * @param plainOut receives the plain APDU when the result is Unwrapped.
     */
    SecureRxResult processIncoming(const SecureFrameInfo& info,
                                   std::span<const uint8_t> tpdu,
                                   std::span<uint8_t> plainOut);

    /**
     * @brief Secure an outbound TPDU if the destination calls for it.
     *
     * @return the secured length, or 0 when the frame must go out in plain.
     */
    util::Result<size_t> processOutgoing(const SecureFrameInfo& info,
                                         std::span<const uint8_t> plainTpdu,
                                         std::span<uint8_t> secureOut);

private:
    /// What secured the last request from a peer, so the answer can match it.
    struct PeerContext {
        uint16_t address{0};
        bool valid{false};
        bool toolAccess{false};
    };
    static constexpr size_t kPeerContexts = 4;

    [[nodiscard]] SecureBlockContext blockContext(const SecureFrameInfo& info) const;

    [[nodiscard]] util::Result<Key> selectKey(const SecureFrameInfo& info,
                                              const IndividualAddress& peer,
                                              bool toolAccess) const;

    /// CCM decrypt-and-verify. @p aTail follows the SCF in the associated data.
    util::Result<void> ccmDecrypt(const Key& key,
                                  const SecureBlockContext& ctx,
                                  std::span<const uint8_t, 6> nonce,
                                  uint8_t scf,
                                  std::span<const uint8_t> aTail,
                                  std::span<const uint8_t> cipher,
                                  std::span<const uint8_t, kMacSize> mac,
                                  std::span<uint8_t> plainOut);

    /// CCM encrypt-and-authenticate, appending the MAC to @p cipherOut.
    util::Result<void> ccmEncrypt(const Key& key,
                                  const SecureBlockContext& ctx,
                                  std::span<const uint8_t, 6> nonce,
                                  uint8_t scf,
                                  std::span<const uint8_t> aTail,
                                  std::span<const uint8_t> plain,
                                  std::span<uint8_t> cipherOut);

    SecureRxResult handleData(const SecureFrameInfo& info,
                              const SecurityControlField& scf,
                              std::span<const uint8_t> tpdu,
                              std::span<uint8_t> plainOut);

    SecureRxResult handleSyncRequest(const SecureFrameInfo& info,
                                     const SecurityControlField& scf,
                                     std::span<const uint8_t> tpdu);

    void rememberPeer(const IndividualAddress& peer, bool toolAccess);
    [[nodiscard]] const PeerContext* findPeer(const IndividualAddress& peer) const;

    /// The request currently being answered. A device answers on the same
    /// protection it received (§5.5), and for a broadcast answer this is the
    /// only thing that identifies which peer — and therefore which key — the
    /// exchange belongs to.
    struct InboundContext {
        bool valid{false};
        bool secured{false};
        bool toolAccess{false};
        uint16_t peer{0};
    };
    InboundContext _inbound{};

    objects::SecurityInterfaceObject& _securityObject;
    Clock _clock;
    RandomSource _randomSource;

    FrameSink _sink{};
    TpciResolver _tpciResolver{};
    IndividualAddress _ownAddress{};
    SerialNumber _serialNumber{};

    std::array<PeerContext, kPeerContexts> _peers{};
    size_t _nextPeerSlot{0};

    /// millis() of the last S-A_Sync_Response; §5.3.2 requires at most one per
    /// second so a flood of requests cannot be used to spin the device.
    uint32_t _lastSyncResponseMs{0};
    bool _syncResponded{false};
};

} // namespace security
} // namespace knx
