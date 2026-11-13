// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file a_send_pipeline.hpp
 * @brief Outbound half of the Application Layer: APDU encoding, the transport
 *        frame, retries and the send-outcome feed.
 *
 * Shared by the group services (ApplicationLayer) and the management server
 * (ManagementServer). It holds no request state: whether a point-to-point
 * frame goes out on the transport connection is the caller's argument, not
 * something the pipeline remembers.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace application {

struct ASendOutcome {
    GroupAddress destination;       ///< Destination address (group or individual)
    APCIService service;            ///< APCI service code
    AddressType destinationType;    ///< Destination address type
    util::ErrorCode result;         ///< Lower-layer send result
    uint8_t attempts{0};            ///< Number of send attempts performed
};

struct SendOptions {
    uint8_t maxAttempts{1};
    bool retryOnBusy{true};
    bool retryOnTimeout{true};
    bool retryOnTransmissionFailed{false};
    Priority priority{Priority::Low};
};

enum class SendPolicyPreset : uint8_t {
    SingleAttempt = 0,
    RetryTransientOnce,
    RetryTransientTwice,
};

/// How a frame to an individual address travels (03/03/04 §2): on the
/// transport connection (T_Data_Connected) or connectionless
/// (T_Data_Individual). Ignored for group and broadcast destinations.
enum class PointToPointMode : uint8_t {
    Connectionless,
    Connected,
};

/// Services whose first payload octet is carried in the APCI's 6-bit data
/// field instead of in the APDU body.
///
/// Shared by the receive and the transmit path deliberately: they are the two
/// halves of one encoding, and when they disagree a service is answered with a
/// payload shifted by one octet.
bool usesApciData6(APCIService service);

/**
 * @brief Builds application frames and hands them to the transport layer.
 *
 * Blocking (sendData) and stepped (beginSendData / pollSendData) sends share
 * the encoding and the outcome queue. Not thread-safe: called from the thread
 * that runs the transport layer.
 */
class ASendPipeline {
public:
    using SendProgressState = util::OperationProgressState;

    explicit ASendPipeline(transport::TransportLayer& transport);

    ASendPipeline(const ASendPipeline&) = delete;
    ASendPipeline& operator=(const ASendPipeline&) = delete;
    ASendPipeline(ASendPipeline&&) = delete;
    ASendPipeline& operator=(ASendPipeline&&) = delete;

    /// Start accepting sends, with @p ownAddress as the source address.
    void open(const IndividualAddress& ownAddress);
    /// Stop accepting sends; drops the outcome queue and a stepped send.
    void close();
    bool isOpen() const { return _open; }

    void setOwnAddress(const IndividualAddress& ownAddress) { _ownAddress = ownAddress; }
    const IndividualAddress& ownAddress() const { return _ownAddress; }

    util::Result<void> sendData(const GroupAddress& destination, APCIService service,
                                std::span<const uint8_t> data, AddressType destinationType,
                                const SendOptions& options,
                                PointToPointMode mode = PointToPointMode::Connectionless);
    util::Result<void> beginSendData(const GroupAddress& destination, APCIService service,
                                     std::span<const uint8_t> data, AddressType destinationType,
                                     const SendOptions& options);
    util::Result<SendProgressState> pollSendData();

    bool popSendOutcome(ASendOutcome& outcome);
    size_t queuedSendOutcomeCount() const { return _txOutcomeQueueCount; }
    size_t droppedSendOutcomeCount() const { return _droppedSendOutcomes; }
    bool transmissionInProgress() const { return _sendOperation.active; }

    static SendOptions optionsForPreset(SendPolicyPreset preset);

private:
    struct SendOperationState {
        bool active{false};
        transport::TDataFrame frame{};
        ASendOutcome outcome{};
        SendOptions options{};
        uint8_t attemptsStarted{0};
    };

    util::Result<void> buildTransportTxFrame(const GroupAddress& destination,
                                             APCIService service,
                                             std::span<const uint8_t> data,
                                             AddressType destinationType,
                                             PointToPointMode mode,
                                             transport::TDataFrame& tFrame,
                                             ASendOutcome& outcome) const;
    void enqueueSendOutcome(const ASendOutcome& outcome);
    void finishSendOperation();
    util::Result<void> startSendAttempt();

    transport::TransportLayer& _transport;
    IndividualAddress _ownAddress{0};
    bool _open{false};

    static constexpr size_t TX_OUTCOME_QUEUE_CAPACITY = 8u;
    std::array<ASendOutcome, TX_OUTCOME_QUEUE_CAPACITY> _txOutcomeQueue{};
    size_t _txOutcomeQueueHead{0};
    size_t _txOutcomeQueueCount{0};
    size_t _droppedSendOutcomes{0};

    SendOperationState _sendOperation{};
};

} // namespace application
} // namespace knx
