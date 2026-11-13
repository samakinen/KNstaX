// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file a_send_pipeline.cpp
 * @brief APDU encoding, transport frame, retries and send outcomes.
 */

#include "knx/application/a_send_pipeline.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/log.hpp"

static const char* TAG = "KNX.App.Send";

namespace knx {
namespace application {

namespace {

uint8_t normalizeMaxAttempts(const SendOptions& options) {
    return options.maxAttempts == 0u ? 1u : options.maxAttempts;
}

bool shouldRetrySendError(util::ErrorCode error, const SendOptions& options) {
    switch (error) {
        case util::ErrorCode::Busy:
            return options.retryOnBusy;
        case util::ErrorCode::Timeout:
            return options.retryOnTimeout;
        case util::ErrorCode::TransmissionFailed:
            return options.retryOnTransmissionFailed;
        default:
            return false;
    }
}

ASendPipeline::SendProgressState mapSendErrorToProgressState(util::ErrorCode error) {
    switch (error) {
        case util::ErrorCode::Busy:
            return ASendPipeline::SendProgressState::Busy;
        case util::ErrorCode::Timeout:
            return ASendPipeline::SendProgressState::Timeout;
        case util::ErrorCode::TransmissionFailed:
        default:
            return ASendPipeline::SendProgressState::TransmissionFailed;
    }
}

} // namespace

bool usesApciData6(APCIService service) {
    switch (service) {
        case APCIService::MemoryRead:
        case APCIService::MemoryWrite:
        case APCIService::MemoryResponse:
        case APCIService::DeviceDescriptorRead:
        case APCIService::DeviceDescriptorResponse:
        // A_ADC_Read / A_ADC_Response carry channel_nr there
        // (03/03/07 §3.5.2, Figures 72 and 73).
        case APCIService::ADCRead:
        case APCIService::ADCResponse:
            return true;
        default:
            return false;
    }
}

ASendPipeline::ASendPipeline(transport::TransportLayer& transport)
    : _transport(transport)
{
}

SendOptions ASendPipeline::optionsForPreset(SendPolicyPreset preset) {
    SendOptions options;
    switch (preset) {
        case SendPolicyPreset::SingleAttempt:
            return options;
        case SendPolicyPreset::RetryTransientOnce:
            options.maxAttempts = 2u;
            options.retryOnBusy = true;
            options.retryOnTimeout = true;
            return options;
        case SendPolicyPreset::RetryTransientTwice:
            options.maxAttempts = 3u;
            options.retryOnBusy = true;
            options.retryOnTimeout = true;
            return options;
    }

    return options;
}

void ASendPipeline::open(const IndividualAddress& ownAddress) {
    _ownAddress = ownAddress;
    _open = true;
}

void ASendPipeline::close() {
    _txOutcomeQueueHead = 0u;
    _txOutcomeQueueCount = 0u;
    _droppedSendOutcomes = 0u;
    finishSendOperation();
    _open = false;
}

void ASendPipeline::finishSendOperation() {
    _sendOperation = SendOperationState{};
}

util::Result<void> ASendPipeline::startSendAttempt() {
    auto beginResult = _transport.beginTransmit(_sendOperation.frame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    ++_sendOperation.attemptsStarted;
    _sendOperation.outcome.attempts = _sendOperation.attemptsStarted;
    return util::Result<void>::ok();
}

void ASendPipeline::enqueueSendOutcome(const ASendOutcome& outcome) {
    // Send outcomes are a diagnostic feed. EndpointRuntime::loop() drains it,
    // but during an ETS download outcomes are produced faster than the owner
    // loop runs, so it overflows regardless. Discarding the *newest* outcome
    // there was backwards — it threw away the entries a consumer would next
    // want and logged a warning per frame. Overwrite the oldest instead: the
    // queue then always holds the most recent outcomes, and a queue that is
    // merely being drained slowly stays silent.
    if (_txOutcomeQueueCount >= TX_OUTCOME_QUEUE_CAPACITY) {
        ++_droppedSendOutcomes;
        _txOutcomeQueue[_txOutcomeQueueHead] = outcome;
        _txOutcomeQueueHead = (_txOutcomeQueueHead + 1u) % TX_OUTCOME_QUEUE_CAPACITY;
        return;
    }

    const size_t index = (_txOutcomeQueueHead + _txOutcomeQueueCount) % TX_OUTCOME_QUEUE_CAPACITY;
    _txOutcomeQueue[index] = outcome;
    ++_txOutcomeQueueCount;
}

bool ASendPipeline::popSendOutcome(ASendOutcome& outcome) {
    if (_txOutcomeQueueCount == 0u) {
        return false;
    }

    outcome = _txOutcomeQueue[_txOutcomeQueueHead];
    _txOutcomeQueueHead = (_txOutcomeQueueHead + 1u) % TX_OUTCOME_QUEUE_CAPACITY;
    --_txOutcomeQueueCount;
    return true;
}

util::Result<void> ASendPipeline::buildTransportTxFrame(const GroupAddress& destination,
                                                        APCIService service,
                                                        std::span<const uint8_t> data,
                                                        AddressType destinationType,
                                                        PointToPointMode mode,
                                                        transport::TDataFrame& tFrame,
                                                        ASendOutcome& outcome) const {
    outcome.destination = destination;
    outcome.service = service;
    outcome.destinationType = destinationType;
    outcome.result = util::ErrorCode::Success;
    outcome.attempts = 0u;

    if (!_open) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }

    // Create transport frame
    tFrame.source = _ownAddress;
    tFrame.destination = destination;
    tFrame.destinationType = destinationType;
    tFrame.ackRequested = false;
    tFrame.repeated = false;  // First transmission, not a repeat
    // Only an answer to a request that came in on the connection may go out on
    // it; the management server says which that is (RequestContext).
    if (destinationType == AddressType::Individual) {
        tFrame.service = mode == PointToPointMode::Connected ? transport::TDataService::Connected
                                                             : transport::TDataService::Individual;
    } else {
        tFrame.service = transport::TDataService::Group;
    }

    // GroupValue has two encodings:
    // - Short APDU: APCI low 6 bits contain the 1-byte value (0..63) and no payload.
    // - Long APDU: APCI low 6 bits are 0 and payload carries the data bytes.
    const auto isGroupValueService = [](APCIService s) {
        return s == APCIService::GroupValueRead || s == APCIService::GroupValueWrite || s == APCIService::GroupValueResponse;
    };

    if (isGroupValueService(service)) {
        if (service == APCIService::GroupValueRead) {
            auto buildResult = knx::protocol::buildTpduInPlace(
                knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
                APCIField::create(service),
                std::span<const uint8_t>{},
                tFrame.tpdu
            );
            if (buildResult.isError()) {
                return buildResult.error();
            }
        } else {
            if (data.empty()) {
                KNX_LOGE(TAG, "GroupValue %u requires data", static_cast<unsigned>(service));
                return util::ErrorCode::InvalidParameter;
            }

            if (data.size() == 1u && data[0] <= 0x3F) {
                const auto apci = APCIField::create(service, data[0]);
                auto buildResult = knx::protocol::buildTpduInPlace(
                    knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
                    apci,
                    std::span<const uint8_t>{},
                    tFrame.tpdu
                );
                if (buildResult.isError()) {
                    return buildResult.error();
                }
            } else {
                const auto apci = APCIField::create(service, 0);
                auto buildResult = knx::protocol::buildTpduInPlace(
                    knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
                    apci,
                    data,
                    tFrame.tpdu
                );
                if (buildResult.isError()) {
                    return buildResult.error();
                }
            }
        }
    } else if (usesApciData6(service)) {
        if (data.empty()) {
            KNX_LOGE(TAG, "APCI data6 required but missing for service %u", static_cast<unsigned>(service));
            return util::ErrorCode::InvalidParameter;
        }
        const uint8_t data6 = data[0];
        const auto apci = APCIField::create(service, data6);
        const std::span<const uint8_t> tail = data.subspan(1);
        auto buildResult = knx::protocol::buildTpduInPlace(
            knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
            apci,
            tail,
            tFrame.tpdu
        );
        if (buildResult.isError()) {
            return buildResult.error();
        }
    } else {
        auto buildResult = knx::protocol::buildTpduInPlace(
            knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
            APCIField::create(service),
            data,
            tFrame.tpdu
        );
        if (buildResult.isError()) {
            return buildResult.error();
        }
    }

    return util::Result<void>::ok();
}

util::Result<void> ASendPipeline::sendData(const GroupAddress& destination,
                                           APCIService service,
                                           std::span<const uint8_t> data,
                                           AddressType destinationType,
                                           const SendOptions& options,
                                           PointToPointMode mode) {
    transport::TDataFrame tFrame;
    ASendOutcome outcome;
    auto buildResult = buildTransportTxFrame(destination, service, data, destinationType, mode, tFrame, outcome);
    if (buildResult.isError()) {
        outcome.result = buildResult.error();
        enqueueSendOutcome(outcome);
        return buildResult.error();
    }
    tFrame.priority = options.priority;

    const uint8_t maxAttempts = normalizeMaxAttempts(options);
    util::Result<void> sendResult = util::ErrorCode::OperationFailed;
    for (uint8_t attempt = 1u; attempt <= maxAttempts; ++attempt) {
        outcome.attempts = attempt;
        sendResult = _transport.sendFrame(tFrame);
        if (sendResult.isOk()) {
            outcome.result = util::ErrorCode::Success;
            enqueueSendOutcome(outcome);
            return util::Result<void>::ok();
        }

        outcome.result = sendResult.error();
        if (attempt >= maxAttempts || !shouldRetrySendError(sendResult.error(), options)) {
            break;
        }
    }

    enqueueSendOutcome(outcome);
    return sendResult;
}

util::Result<void> ASendPipeline::beginSendData(const GroupAddress& destination,
                                                APCIService service,
                                                std::span<const uint8_t> data,
                                                AddressType destinationType,
                                                const SendOptions& options) {
    if (_sendOperation.active) {
        return util::ErrorCode::Busy;
    }

    transport::TDataFrame tFrame;
    ASendOutcome outcome;
    auto buildResult = buildTransportTxFrame(destination, service, data, destinationType,
                                             PointToPointMode::Connectionless, tFrame, outcome);
    if (buildResult.isError()) {
        outcome.result = buildResult.error();
        enqueueSendOutcome(outcome);
        return buildResult.error();
    }
    tFrame.priority = options.priority;

    finishSendOperation();
    _sendOperation.active = true;
    _sendOperation.frame = tFrame;
    _sendOperation.outcome = outcome;
    _sendOperation.options = options;

    auto beginResult = startSendAttempt();
    if (beginResult.isError()) {
        _sendOperation.outcome.result = beginResult.error();
        enqueueSendOutcome(_sendOperation.outcome);
        finishSendOperation();
        return beginResult.error();
    }

    return util::Result<void>::ok();
}

util::Result<ASendPipeline::SendProgressState> ASendPipeline::pollSendData() {
    if (!_open) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_sendOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    auto progress = _transport.pollTransmit();
    if (progress.isError()) {
        _sendOperation.outcome.result = progress.error();
        enqueueSendOutcome(_sendOperation.outcome);
        finishSendOperation();
        return progress.error();
    }

    switch (progress.value()) {
        case transport::TransportLayer::SendProgressState::Pending:
            return SendProgressState::Pending;
        case transport::TransportLayer::SendProgressState::Success:
            _sendOperation.outcome.result = util::ErrorCode::Success;
            enqueueSendOutcome(_sendOperation.outcome);
            finishSendOperation();
            return SendProgressState::Success;
        case transport::TransportLayer::SendProgressState::Busy:
        case transport::TransportLayer::SendProgressState::TransmissionFailed:
        case transport::TransportLayer::SendProgressState::Timeout:
        {
            util::ErrorCode error = util::ErrorCode::TransmissionFailed;
            if (progress.value() == transport::TransportLayer::SendProgressState::Busy) {
                error = util::ErrorCode::Busy;
            } else if (progress.value() == transport::TransportLayer::SendProgressState::Timeout) {
                error = util::ErrorCode::Timeout;
            }

            _sendOperation.outcome.result = error;

            if (_sendOperation.attemptsStarted < normalizeMaxAttempts(_sendOperation.options)
                && shouldRetrySendError(error, _sendOperation.options)) {
                auto retryBegin = startSendAttempt();
                if (retryBegin.isOk()) {
                    return SendProgressState::Pending;
                }
                _sendOperation.outcome.result = retryBegin.error();
            }

            const SendProgressState terminalState = mapSendErrorToProgressState(_sendOperation.outcome.result);
            enqueueSendOutcome(_sendOperation.outcome);
            finishSendOperation();
            return terminalState;
        }
    }

    _sendOperation.outcome.result = util::ErrorCode::OperationFailed;
    enqueueSendOutcome(_sendOperation.outcome);
    finishSendOperation();
    return util::ErrorCode::OperationFailed;
}

} // namespace application
} // namespace knx
