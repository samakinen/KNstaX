// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file application_layer.cpp
 * @brief KNX Application Layer - Service Dispatcher Implementation
 * 
 * Implements service dispatching, APCI parsing, and response routing.
 */

#include "knx/application/application_layer.hpp"
#include "knx/application/device_descriptor.hpp"
#include "knx/application/property_store.hpp"
#include "knx/application/address_space.hpp"
#include "knx/application/security_access_policy.hpp"
#include "knx/config.hpp"
#include "knx/util/log.hpp"
#include "knx/util/hex.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/application/apci_services.hpp"
#include <memory>
#include <span>

static const char* TAG = "KNX.App.Layer";

namespace knx {
namespace application {

ApplicationLayer::ApplicationLayer(transport::TransportLayer& transport)
    : _transport(transport)
    , _ownAddress(0)
    , _rxCallback(nullptr)
    , _initialized(false)
    , _programmingModeEnabled(false)
{
}

ApplicationLayer::~ApplicationLayer() {
    close();
}

namespace {

uint8_t normalizeMaxAttempts(const SendOptions& options) {
    return options.maxAttempts == 0u ? 1u : options.maxAttempts;
}

/// Services whose first payload octet is carried in the APCI's 6-bit data
/// field instead of in the APDU body.
///
/// Shared by the receive and the transmit path deliberately: they are the two
/// halves of one encoding, and when they disagree a service is answered with a
/// payload shifted by one octet.
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

using ResponsePayload = util::FixedVector<uint8_t, config::MAX_APDU_LENGTH>;

bool appendPayloadByte(ResponsePayload& payload, uint8_t value)
{
    return payload.push_back(value);
}

bool appendPayloadBytes(ResponsePayload& payload, std::span<const uint8_t> bytes)
{
    for (uint8_t byte : bytes) {
        if (!payload.push_back(byte)) {
            return false;
        }
    }
    return true;
}

void logOversizedServiceResponse(const char* serviceName, size_t payloadSize)
{
    KNX_LOGW(TAG,
             "%s response exceeds MAX_APDU_LENGTH: %zu > %zu",
             serviceName,
             payloadSize,
             static_cast<size_t>(config::MAX_APDU_LENGTH));
}

}

SendOptions ApplicationLayer::optionsForPreset(SendPolicyPreset preset) {
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

void ApplicationLayer::registerPropertyObject(InterfaceObjectType objectType,
                                              InterfaceObjectIndex objectIndex,
                                              PropertyRegistration registration)
{
    auto* store = _propertyStoreManager ? _propertyStoreManager->addObject(objectType, objectIndex) : nullptr;
    if (store) {
        registration(*store);
    }
}

util::Result<void> ApplicationLayer::registerMemoryRegion(const MemoryRegion& region,
                                                          std::span<uint8_t> storage)
{
    if (!_addressSpace || !_memoryService) {
        return util::Result<void>::err(util::ErrorCode::NotInitialized);
    }
    if (storage.size() < region.size) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    auto addRes = _addressSpace->addRegion(region);
    if (addRes.isError()) {
        return addRes;
    }
    if (!_memorySegments.push_back(MemorySegmentBinding{region, storage})) {
        return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
    }

    // (Re)wire the storage router. Idempotent: the callbacks iterate the
    // registered segment list at call time.
    _memoryService->setReadCallback(
        [this](MemoryAddress address, uint8_t length, std::span<uint8_t> out) -> util::Result<void> {
            for (const auto& segment : _memorySegments) {
                if (segment.region.containsRange(address, length)) {
                    const uint32_t offset = static_cast<uint32_t>(address.raw)
                                          - static_cast<uint32_t>(segment.region.startAddress.raw);
                    std::copy_n(segment.storage.data() + offset, length, out.data());
                    return util::Result<void>::ok();
                }
            }
            return util::Result<void>::err(util::ErrorCode::InvalidAddress);
        });
    _memoryService->setWriteCallback(
        [this](MemoryAddress address, std::span<const uint8_t> data) -> util::Result<void> {
            for (auto& segment : _memorySegments) {
                if (segment.region.containsRange(address, static_cast<uint8_t>(data.size()))) {
                    const uint32_t offset = static_cast<uint32_t>(address.raw)
                                          - static_cast<uint32_t>(segment.region.startAddress.raw);
                    std::copy(data.begin(), data.end(), segment.storage.data() + offset);
                    segment.written = true;
                    return util::Result<void>::ok();
                }
            }
            return util::Result<void>::err(util::ErrorCode::InvalidAddress);
        });

    return util::Result<void>::ok();
}

void ApplicationLayer::setPropertyReadProvider(PropertyReadProvider provider)
{
    // Both property service families get the same provider: A_PropertyValue_*
    // and A_PropertyExtValue_* address the same interface objects, and ETS
    // chooses between them per device profile — a Data Secure device is driven
    // through the extended ones.
    if (_propertyExtServices) {
        _propertyExtServices->setReadProvider(provider);
    }
    if (_propertyServices) {
        _propertyServices->setReadProvider(std::move(provider));
    }
}

void ApplicationLayer::setPropertyWriteConsumer(PropertyWriteConsumer consumer)
{
    if (_propertyServices) {
        _propertyServices->setWriteConsumer(std::move(consumer));
    }
}

void ApplicationLayer::setPropertyWriteProvider(PropertyWriteProvider provider)
{
    if (_propertyExtServices) {
        _propertyExtServices->setWriteProvider(provider);
    }
    if (_propertyServices) {
        _propertyServices->setWriteProvider(std::move(provider));
    }
}

void ApplicationLayer::setPropertyDescriptionProvider(PropertyDescriptionProvider provider)
{
    if (_propertyServices) {
        _propertyServices->setDescriptionProvider(std::move(provider));
    }
}

bool ApplicationLayer::memoryRegionWritten(MemoryAddress regionStart) const
{
    for (const auto& segment : _memorySegments) {
        if (segment.region.startAddress.raw == regionStart.raw) {
            return segment.written;
        }
    }
    return false;
}

void ApplicationLayer::finishSendOperation() {
    _sendOperation = SendOperationState{};
}

util::Result<void> ApplicationLayer::startSendAttempt() {
    auto beginResult = _transport.beginTransmit(_sendOperation.frame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    ++_sendOperation.attemptsStarted;
    _sendOperation.outcome.attempts = _sendOperation.attemptsStarted;
    return util::Result<void>::ok();
}

bool ApplicationLayer::shouldRetrySendError(util::ErrorCode error, const SendOptions& options) const {
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

ApplicationLayer::SendProgressState ApplicationLayer::mapSendErrorToProgressState(util::ErrorCode error) const {
    switch (error) {
        case util::ErrorCode::Busy:
            return SendProgressState::Busy;
        case util::ErrorCode::Timeout:
            return SendProgressState::Timeout;
        case util::ErrorCode::TransmissionFailed:
        default:
            return SendProgressState::TransmissionFailed;
    }
}

void ApplicationLayer::enqueueReceivedFrame(const ADataFrame& frame) {
    if (_rxQueueCount >= RX_QUEUE_CAPACITY) {
        ++_droppedReceiveFrames;
        KNX_LOGW(TAG, "Dropping application RX frame: queue full (%zu dropped)", _droppedReceiveFrames);
        return;
    }

    const size_t index = (_rxQueueHead + _rxQueueCount) % RX_QUEUE_CAPACITY;
    _rxQueue[index] = frame;
    ++_rxQueueCount;
}

void ApplicationLayer::enqueueSendOutcome(const ASendOutcome& outcome) {
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

bool ApplicationLayer::popReceivedFrame(ADataFrame& frame) {
    if (_rxQueueCount == 0u) {
        return false;
    }

    frame = _rxQueue[_rxQueueHead];
    _rxQueueHead = (_rxQueueHead + 1u) % RX_QUEUE_CAPACITY;
    --_rxQueueCount;
    return true;
}

bool ApplicationLayer::popSendOutcome(ASendOutcome& outcome) {
    if (_txOutcomeQueueCount == 0u) {
        return false;
    }

    outcome = _txOutcomeQueue[_txOutcomeQueueHead];
    _txOutcomeQueueHead = (_txOutcomeQueueHead + 1u) % TX_OUTCOME_QUEUE_CAPACITY;
    --_txOutcomeQueueCount;
    return true;
}

util::Result<void> ApplicationLayer::init(const IndividualAddress& ownAddress) {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }

    _ownAddress = ownAddress;

    // Initialize dependencies
    _deviceDescriptor = std::make_unique<DeviceDescriptor>(DeviceDescriptor::createDefault());
    _propertyStoreManager = std::make_unique<PropertyStoreManager>();
    _addressSpace = std::make_unique<AddressSpace>();

    // Initialize service handlers
    _deviceDescriptorService = std::make_unique<DeviceDescriptorService>(*_deviceDescriptor);
    _propertyServices = std::make_unique<PropertyServices>(*_propertyStoreManager);
    _propertyExtServices = std::make_unique<PropertyExtServices>(*_propertyStoreManager);
    _memoryService = std::make_unique<MemoryService>(*_addressSpace);
    _authorizationService = std::make_unique<AuthorizationService>();
    _restartService = std::make_unique<RestartService>();
    _functionPropertyServices = std::make_unique<FunctionPropertyServices>();
    _networkParameterService = std::make_unique<NetworkParameterService>();

    // Wire service handler response callbacks to transport
    wireServiceCallbacks();
    wireAccessPolicy();
    wireTransportReceiveCallback();

    // Management/commissioning service responses use system priority per KNX
    // spec. They also need real retry budget: the default SendOptions{} is
    // maxAttempts=1, so a single transient "TX busy" (e.g. racing the DL-ACK
    // just sent for the incoming request) silently drops the response for
    // good instead of retrying — sendData() breaks out of its retry loop as
    // soon as attempt>=maxAttempts, before retryOnBusy/retryOnTimeout are
    // even consulted.
    _serviceResponseSendOptions = optionsForPreset(SendPolicyPreset::RetryTransientTwice);
    _serviceResponseSendOptions.priority = Priority::System;

    _initialized = true;
    KNX_LOGD(TAG, "Application layer initialized");

    return util::Result<void>::ok();
}

void ApplicationLayer::close() {
    if (_initialized) {
        // Prevent transport from calling back into a destroyed ApplicationLayer.
        _transport.setReceiveCallback(nullptr);
        _rxQueueHead = 0u;
        _rxQueueCount = 0u;
        _droppedReceiveFrames = 0u;
        _txOutcomeQueueHead = 0u;
        _txOutcomeQueueCount = 0u;
        _droppedSendOutcomes = 0u;
        finishSendOperation();
        _initialized = false;
        KNX_LOGI(TAG, "Application layer closed");
    }
}

util::Result<void> ApplicationLayer::buildTransportTxFrame(const GroupAddress& destination,
                                                           APCIService service,
                                                           std::span<const uint8_t> data,
                                                           AddressType destinationType,
                                                           transport::TDataFrame& tFrame,
                                                           ASendOutcome& outcome) {
    outcome.destination = destination;
    outcome.service = service;
    outcome.destinationType = destinationType;
    outcome.result = util::ErrorCode::Success;
    outcome.attempts = 0u;

    if (!_initialized) {
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
    // it; see _handlingConnectedRequest.
    if (destinationType == AddressType::Individual) {
        tFrame.service = _handlingConnectedRequest ? transport::TDataService::Connected
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

void ApplicationLayer::wireServiceCallbacks() {
    _deviceDescriptorService->setResponseCallback(
        [this](const IndividualAddress& destination, uint8_t descriptorType, std::span<const uint8_t> data) {
            ResponsePayload payload;
            if (!appendPayloadByte(payload, descriptorType) || !appendPayloadBytes(payload, data)) {
                logOversizedServiceResponse("DeviceDescriptor", 1u + data.size());
                return;
            }
            (void)sendToIndividual(destination,
                                   APCIService::DeviceDescriptorResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });

    _memoryService->setResponseCallback(
        [this](const IndividualAddress& destination, const MemoryResponse& response) {
            ResponsePayload payload;
            if (!appendPayloadByte(payload, response.count)
                || !appendPayloadByte(payload, static_cast<uint8_t>((response.address.raw >> 8) & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>(response.address.raw & 0xFF))
                || !appendPayloadBytes(payload, response.data.span())) {
                logOversizedServiceResponse("Memory", 3u + response.data.size());
                return;
            }
            (void)sendToIndividual(destination,
                                   APCIService::MemoryResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });

    _memoryService->setExtendedResponseCallback(
        [this](const IndividualAddress& destination, const MemoryExtendedResponse& response) {
            // Layout differs from A_Memory_Response: return code first, then a
            // 3-octet address, then the data.  Nothing rides in the APCI.
            ResponsePayload payload;
            if (!appendPayloadByte(payload, static_cast<uint8_t>(response.returnCode))
                || !appendPayloadByte(payload, static_cast<uint8_t>((response.address.raw >> 16) & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>((response.address.raw >> 8) & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>(response.address.raw & 0xFF))
                || !appendPayloadBytes(payload, response.data.span())) {
                logOversizedServiceResponse("MemoryExtended", 4u + response.data.size());
                return;
            }
            const auto service = response.kind == MemoryExtendedResponseKind::Read
                                     ? APCIService::MemoryExtendedReadResponse
                                     : APCIService::MemoryExtendedWriteResponse;
            (void)sendToIndividual(destination, service, payload, _serviceResponseSendOptions);
        });

    _propertyExtServices->setValueResponseCallback(
        [this](const IndividualAddress& destination,
               APCIService service,
               const PropertyExtValueResponse& response) {
            ResponsePayload payload;
            uint8_t header[PropertyExtHeader::kEncodedLength]{};
            response.header.encode(header);
            const bool ok =
                appendPayloadBytes(payload, header)
                && appendPayloadByte(payload, response.elementCount)
                && appendPayloadByte(payload, static_cast<uint8_t>((response.startIndex >> 8) & 0xFF))
                && appendPayloadByte(payload, static_cast<uint8_t>(response.startIndex & 0xFF))
                // A_PropertyExtValue_Response trails the payload with data (or a
                // one-octet error), whereas _WriteConRes trails it with the
                // return code.  Distinct layouts, same header.
                && (service == APCIService::PropertyExtValueResponse
                        ? appendPayloadBytes(payload, response.data.span())
                        : appendPayloadByte(payload, static_cast<uint8_t>(response.returnCode)));
            if (!ok) {
                logOversizedServiceResponse("PropertyExtValue",
                                            PropertyExtHeader::kEncodedLength + 3u + response.data.size());
                return;
            }
            (void)sendToIndividual(destination, service, payload, _serviceResponseSendOptions);
        });

    _propertyExtServices->setDescriptionResponseCallback(
        [this](const IndividualAddress& destination, const PropertyExtDescriptionResponse& response) {
            ResponsePayload payload;
            uint8_t header[PropertyExtHeader::kEncodedLength]{};
            response.header.encode(header);
            const bool ok =
                appendPayloadBytes(payload, header)
                && appendPayloadByte(payload, static_cast<uint8_t>(((response.descriptionType & 0x0Fu) << 4)
                                                                   | ((response.propertyIndex >> 8) & 0x0Fu)))
                && appendPayloadByte(payload, static_cast<uint8_t>(response.propertyIndex & 0xFF))
                && appendPayloadByte(payload, static_cast<uint8_t>((response.dptMain >> 8) & 0xFF))
                && appendPayloadByte(payload, static_cast<uint8_t>(response.dptMain & 0xFF))
                && appendPayloadByte(payload, static_cast<uint8_t>((response.dptSub >> 8) & 0xFF))
                && appendPayloadByte(payload, static_cast<uint8_t>(response.dptSub & 0xFF))
                // bit 7 writeable, bit 6 reserved, bits 5..0 PDT.
                && appendPayloadByte(payload, static_cast<uint8_t>((response.writeEnabled ? 0x80u : 0x00u)
                                                                   | (response.propertyDataType & 0x3Fu)))
                && appendPayloadByte(payload, static_cast<uint8_t>((response.maxElements >> 8) & 0xFF))
                && appendPayloadByte(payload, static_cast<uint8_t>(response.maxElements & 0xFF))
                && appendPayloadByte(payload, static_cast<uint8_t>(((response.readLevel & 0x0Fu) << 4)
                                                                   | (response.writeLevel & 0x0Fu)));
            if (!ok) {
                logOversizedServiceResponse("PropertyExtDescription", 15u);
                return;
            }
            (void)sendToIndividual(destination,
                                   APCIService::PropertyExtDescriptionResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });

    _propertyExtServices->setFunctionResponseCallback(
        [this](const IndividualAddress& destination, const FunctionPropertyExtResponse& response) {
            ResponsePayload payload;
            uint8_t header[PropertyExtHeader::kEncodedLength]{};
            response.header.encode(header);
            // Here the return code precedes the data (Figure 64), the opposite
            // of the value services.
            if (!appendPayloadBytes(payload, header)
                || !appendPayloadByte(payload, static_cast<uint8_t>(response.returnCode))
                || !appendPayloadBytes(payload, response.data.span())) {
                logOversizedServiceResponse("FunctionPropertyExt",
                                            PropertyExtHeader::kEncodedLength + 1u + response.data.size());
                return;
            }
            (void)sendToIndividual(destination,
                                   APCIService::FunctionPropertyExtStateResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });

    _functionPropertyServices->setResponseCallback(
        [this](const IndividualAddress& destination,
               InterfaceObjectIndex objectIndex,
               PropertyID propertyId,
               bool hasReturnCode,
               FunctionPropertyReturnCode returnCode,
               std::span<const uint8_t> data) {
            ResponsePayload payload;
            // 03/03/07 §3.4.5.3: the "not a function property" answer carries
            // object_index and property_id only — no return code, no data.
            if (!appendPayloadByte(payload, static_cast<uint8_t>(objectIndex.value()))
                || !appendPayloadByte(payload, static_cast<uint8_t>(propertyId))) {
                logOversizedServiceResponse("FunctionProperty", 2u);
                return;
            }
            if (hasReturnCode) {
                if (!appendPayloadByte(payload, static_cast<uint8_t>(returnCode))
                    || !appendPayloadBytes(payload, data)) {
                    logOversizedServiceResponse("FunctionProperty", 3u + data.size());
                    return;
                }
            }
            (void)sendToIndividual(destination,
                                   APCIService::FunctionPropertyStateResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });

    _networkParameterService->setSerialNumberResponseCallback(
        [this](const KnxSerialNumber& serialNumber, uint16_t domainAddress) {
            // Answered on the broadcast address: the requester reached us
            // without knowing our individual address, so it cannot be replied
            // to point-to-point.
            ResponsePayload payload;
            if (!appendPayloadBytes(payload, serialNumber)
                || !appendPayloadByte(payload, static_cast<uint8_t>((domainAddress >> 8) & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>(domainAddress & 0xFF))
                || !appendPayloadByte(payload, 0u)
                || !appendPayloadByte(payload, 0u)) {
                logOversizedServiceResponse("SerialNumberResponse", serialNumber.size() + 4u);
                return;
            }
            (void)sendBroadcast(APCIService::IndividualAddressSerialNumberResponse,
                                payload,
                                _serviceResponseSendOptions);
        });

    _networkParameterService->setNetworkParameterResponseCallback(
        [this](InterfaceObjectType objectType,
               PropertyID propertyId,
               std::span<const uint8_t> value) {
            ResponsePayload payload;
            if (!appendPayloadByte(payload, static_cast<uint8_t>((objectType.value() >> 8) & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>(objectType.value() & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>(propertyId))
                || !appendPayloadBytes(payload, value)) {
                logOversizedServiceResponse("NetworkParameterResponse", 3u + value.size());
                return;
            }
            (void)sendBroadcast(APCIService::NetworkParameterResponse,
                                payload,
                                _serviceResponseSendOptions);
        });

    _networkParameterService->setAddressWriteCallback(
        [this](const IndividualAddress& newAddress) -> util::Result<void> {
            // Same effect as A_IndividualAddress_Write, but reached without
            // programming mode because the serial number already identified us.
            setOwnAddress(newAddress);
            if (_individualAddressUpdateCallback) {
                _individualAddressUpdateCallback(newAddress);
            }
            return util::Result<void>::ok();
        });

    _authorizationService->setResponseCallback(
        [this](const IndividualAddress& destination, AuthorizationLevel level) {
            // KNX access levels are inverted relative to privilege on the
            // wire: 0 = maximum access … 15 = no access.
            uint8_t wireLevel = 15u;
            switch (level) {
                case AuthorizationLevel::Maximum:       wireLevel = 0u;  break;
                case AuthorizationLevel::Configuration: wireLevel = 1u;  break;
                case AuthorizationLevel::Management:    wireLevel = 2u;  break;
                case AuthorizationLevel::None:          wireLevel = 15u; break;
            }
            ResponsePayload payload;
            if (!appendPayloadByte(payload, wireLevel)) {
                logOversizedServiceResponse("Authorize", 1u);
                return;
            }
            (void)sendToIndividual(destination,
                                   APCIService::AuthorizeResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });

    _propertyServices->setValueResponseCallback(
        [this](const IndividualAddress& destination, const PropertyValueResponse& response) {
            ResponsePayload payload;
            if (!appendPayloadByte(payload, response.objectIndex.value())
                || !appendPayloadByte(payload, static_cast<uint8_t>(response.propertyId))
                || !appendPayloadByte(payload,
                                      static_cast<uint8_t>(((response.elementCount & 0x0F) << 4)
                                                           | ((response.startIndex >> 8) & 0x0F)))
                || !appendPayloadByte(payload, static_cast<uint8_t>(response.startIndex & 0xFF))
                || !appendPayloadBytes(payload, response.data.span())) {
                logOversizedServiceResponse("PropertyValue", 4u + response.data.size());
                return;
            }
            (void)sendToIndividual(destination,
                                   APCIService::PropertyValueResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });

    _propertyServices->setDescriptionResponseCallback(
        [this](const IndividualAddress& destination,
               InterfaceObjectIndex objectIndex,
               PropertyID propertyId,
               PropertyIndex propertyIndex,
               PropertyWriteAccess writeAccess,
               PropertyDataType type,
               uint16_t maxElements,
               uint8_t readLevel,
               uint8_t writeLevel) {
            ResponsePayload payload;
            if (!appendPayloadByte(payload, objectIndex.value())
                || !appendPayloadByte(payload, static_cast<uint8_t>(propertyId))
                || !appendPayloadByte(payload, propertyIndex.value())
                || !appendPayloadByte(payload,
                                      static_cast<uint8_t>(static_cast<uint8_t>(type)
                                                           | (isWriteAllowed(writeAccess) ? 0x80 : 0x00)))
                || !appendPayloadByte(payload, static_cast<uint8_t>((maxElements >> 8) & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>(maxElements & 0xFF))
                || !appendPayloadByte(payload, static_cast<uint8_t>((readLevel << 4) | (writeLevel & 0x0F)))) {
                logOversizedServiceResponse("PropertyDescription", 7u);
                return;
            }
            (void)sendToIndividual(destination,
                                   APCIService::PropertyDescriptionResponse,
                                   payload,
                                   _serviceResponseSendOptions);
        });
}

void ApplicationLayer::wireAccessPolicy() {
    // The property services know what is being addressed; this layer knows how
    // the request arrived. Neither can decide alone, so the check is installed
    // from here and evaluated inside them, next to the negative responses the
    // spec prescribes for a refusal.
    _propertyServices->setAccessCheck(
        [this](InterfaceObjectIndex objectIndex, PropertyID propertyId, bool write) {
            const auto objectType = objectTypeForIndex(objectIndex);
            if (!objectType.has_value()) {
                // Unknown object: leave the "does not exist" answer to the
                // service rather than turning it into an access refusal.
                return true;
            }
            return isPropertyAccessPermitted(*objectType,
                                             static_cast<uint8_t>(propertyId),
                                             write,
                                             _requestSecurity);
        });

    _propertyExtServices->setAccessCheck(
        [this](uint16_t objectType, uint16_t propertyId, bool write) {
            return isPropertyAccessPermitted(objectType, propertyId, write, _requestSecurity);
        });
}

void ApplicationLayer::wireTransportReceiveCallback() {
    _transport.setReceiveCallback([this](const transport::TDataFrame& frame) {
        handleTransportRx(frame);
    });
}

util::Result<void> ApplicationLayer::sendToIndividual(IndividualAddress destination,
                                                      APCIService service,
                                                      std::span<const uint8_t> data,
                                                      const SendOptions& options) {
    return sendData(GroupAddress(destination.raw), service, data, AddressType::Individual, options);
}

util::Result<void> ApplicationLayer::sendToGroup(GroupAddress destination,
                                                 APCIService service,
                                                 std::span<const uint8_t> data,
                                                 const SendOptions& options) {
    return sendData(destination, service, data, AddressType::Group, options);
}

util::Result<void> ApplicationLayer::sendBroadcast(APCIService service,
                                                   std::span<const uint8_t> data,
                                                   const SendOptions& options) {
    return sendData(GroupAddress(0), service, data, AddressType::Group, options);
}

void ApplicationLayer::handleFunctionProperty(const ADataFrame& frame) {
    // object_index + property_id, then the function's input data.
    if (frame.data.size() < FunctionPropertyServices::kRequestHeaderBytes) {
        return;
    }

    FunctionPropertyRequest request{};
    request.objectIndex = InterfaceObjectIndex(frame.data[0]);
    request.propertyId = static_cast<PropertyID>(frame.data[1]);
    request.invocation = (frame.service == APCIService::FunctionPropertyCommand)
                             ? FunctionPropertyInvocation::Command
                             : FunctionPropertyInvocation::StateRead;
    request.security = frame.security;

    const auto payload =
        std::span<const uint8_t>(frame.data).subspan(FunctionPropertyServices::kRequestHeaderBytes);
    if (payload.size() > kMaxFunctionPropertyDataBytes) {
        KNX_LOGW(TAG, "Function property input of %zu bytes exceeds capacity", payload.size());
        return;
    }
    (void)request.data.assign(payload);

    (void)_functionPropertyServices->handleRequest(frame.source, request);
}

void ApplicationLayer::handleIndividualAddressSerialNumberRead(const ADataFrame& frame) {
    if (frame.data.size() < kKnxSerialNumberBytes) {
        return;
    }
    KnxSerialNumber serial{};
    std::copy_n(frame.data.begin(), kKnxSerialNumberBytes, serial.begin());
    (void)_networkParameterService->handleSerialNumberRead(serial);
}

void ApplicationLayer::handleIndividualAddressSerialNumberWrite(const ADataFrame& frame) {
    // serial(6) + new address(2); the 4 reserved octets are optional on the wire.
    if (frame.data.size() < kKnxSerialNumberBytes + 2u) {
        return;
    }
    IndividualAddressSerialNumberWrite request{};
    std::copy_n(frame.data.begin(), kKnxSerialNumberBytes, request.serialNumber.begin());
    request.newAddress = IndividualAddress(static_cast<uint16_t>(
        (static_cast<uint16_t>(frame.data[kKnxSerialNumberBytes]) << 8)
        | frame.data[kKnxSerialNumberBytes + 1u]));
    (void)_networkParameterService->handleSerialNumberWrite(request);
}

namespace {

/// Shared decoder for the two network-parameter request services.
bool decodeNetworkParameterPayload(const ADataFrame& frame, NetworkParameterRequest& request)
{
    constexpr size_t kHeaderBytes = 3u;  // objectType(2) + propertyId(1)
    if (frame.data.size() < kHeaderBytes) {
        return false;
    }
    request.objectType = InterfaceObjectType(static_cast<uint16_t>(
        (static_cast<uint16_t>(frame.data[0]) << 8) | frame.data[1]));
    request.propertyId = static_cast<PropertyID>(frame.data[2]);

    const auto value = std::span<const uint8_t>(frame.data).subspan(kHeaderBytes);
    if (value.size() > kMaxNetworkParameterValueBytes) {
        return false;
    }
    return static_cast<bool>(request.value.assign(value));
}

}  // namespace

void ApplicationLayer::handleNetworkParameterRead(const ADataFrame& frame) {
    NetworkParameterRequest request{};
    if (!decodeNetworkParameterPayload(frame, request)) {
        return;
    }
    (void)_networkParameterService->handleNetworkParameterRead(request);
}

void ApplicationLayer::handleNetworkParameterWrite(const ADataFrame& frame) {
    NetworkParameterRequest request{};
    if (!decodeNetworkParameterPayload(frame, request)) {
        return;
    }
    (void)_networkParameterService->handleNetworkParameterWrite(request);
}

void ApplicationLayer::handleDeviceDescriptorRead(const ADataFrame& frame) {
    if (frame.data.size() < 1) {
        return;
    }
    uint8_t descriptorType = frame.data[0];
    (void)_deviceDescriptorService->handleReadRequest(frame.source, descriptorType);
}

void ApplicationLayer::handlePropertyValueRead(const ADataFrame& frame) {
    if (frame.data.size() < 4) {
        return;
    }
    PropertyValueReadRequest request;
    request.objectIndex = InterfaceObjectIndex(frame.data[0]);
    request.propertyId = static_cast<PropertyID>(frame.data[1]);
    request.elementCount = (frame.data[2] >> 4) & 0x0F;
    request.startIndex = (static_cast<uint16_t>(frame.data[2] & 0x0F) << 8) | frame.data[3];
    (void)_propertyServices->handleValueRead(frame.source, request);
}

void ApplicationLayer::handlePropertyValueWrite(const ADataFrame& frame) {
    if (frame.data.size() < 4) {
        return;
    }
    PropertyValueWriteRequest request;
    request.objectIndex = InterfaceObjectIndex(frame.data[0]);
    request.propertyId = static_cast<PropertyID>(frame.data[1]);
    request.elementCount = (frame.data[2] >> 4) & 0x0F;
    request.startIndex = (static_cast<uint16_t>(frame.data[2] & 0x0F) << 8) | frame.data[3];
    if (frame.data.size() > 4) {
        (void)request.data.assign(std::span<const uint8_t>(frame.data).subspan(4));
    }
    (void)_propertyServices->handleValueWrite(frame.source, request);
}

void ApplicationLayer::handlePropertyDescriptionRead(const ADataFrame& frame) {
    if (frame.data.size() < 3) {
        return;
    }
    InterfaceObjectIndex objectIndex(frame.data[0]);
    PropertyID propertyId = static_cast<PropertyID>(frame.data[1]);
    PropertyIndex propertyIndex(frame.data[2]);
    (void)_propertyServices->handleDescriptionRead(frame.source, objectIndex, propertyId, propertyIndex);
}

void ApplicationLayer::handleMemoryRead(const ADataFrame& frame) {
    if (frame.data.size() < 3) {
        return;
    }
    uint8_t count = frame.data[0];
    MemoryAddress address(static_cast<uint16_t>((static_cast<uint16_t>(frame.data[1]) << 8) | frame.data[2]));
    (void)_memoryService->handleReadRequest(frame.source, count, address);
}

void ApplicationLayer::handleMemoryWrite(const ADataFrame& frame) {
    if (frame.data.size() < 3) {
        return;
    }
    uint8_t count = frame.data[0];
    MemoryAddress address(static_cast<uint16_t>((static_cast<uint16_t>(frame.data[1]) << 8) | frame.data[2]));
    const auto writeData = std::span<const uint8_t>(frame.data.data() + 3, frame.data.size() - 3);
    (void)_memoryService->handleWriteRequest(frame.source, count, address, writeData);
}

void ApplicationLayer::handleMemoryExtendedRead(const ADataFrame& frame) {
    // count(1) + address(3); no data6 — the whole request is in the payload.
    if (frame.data.size() < 4) {
        return;
    }
    const uint8_t count = frame.data[0];
    const ExtendedMemoryAddress address(
        (static_cast<uint32_t>(frame.data[1]) << 16)
        | (static_cast<uint32_t>(frame.data[2]) << 8)
        | static_cast<uint32_t>(frame.data[3]));
    (void)_memoryService->handleExtendedReadRequest(frame.source, count, address);
}

void ApplicationLayer::handleMemoryExtendedWrite(const ADataFrame& frame) {
    if (frame.data.size() < 4) {
        return;
    }
    const uint8_t count = frame.data[0];
    const ExtendedMemoryAddress address(
        (static_cast<uint32_t>(frame.data[1]) << 16)
        | (static_cast<uint32_t>(frame.data[2]) << 8)
        | static_cast<uint32_t>(frame.data[3]));
    const auto writeData = std::span<const uint8_t>(frame.data.data() + 4, frame.data.size() - 4);
    (void)_memoryService->handleExtendedWriteRequest(frame.source, count, address, writeData);
}

namespace {
/// Decode the 5-octet addressing header shared by every extended property
/// service, plus a bounds check so a truncated APDU cannot index past the end.
bool decodePropertyExtHeader(const ADataFrame& frame, size_t minimumLength, PropertyExtHeader& header)
{
    if (frame.data.size() < minimumLength) {
        return false;
    }
    header = PropertyExtHeader::decode(frame.data.span());
    return true;
}
} // namespace

void ApplicationLayer::handlePropertyExtValueRead(const ADataFrame& frame) {
    // header(5) + nr_of_elem(1) + start_index(2)
    PropertyExtValueRequest request;
    if (!decodePropertyExtHeader(frame, 8, request.header)) {
        return;
    }
    request.elementCount = frame.data[5];
    request.startIndex = static_cast<uint16_t>((static_cast<uint16_t>(frame.data[6]) << 8) | frame.data[7]);
    (void)_propertyExtServices->handleValueRead(frame.source, request);
}

void ApplicationLayer::handlePropertyExtValueWrite(const ADataFrame& frame, bool confirmed) {
    PropertyExtValueRequest request;
    if (!decodePropertyExtHeader(frame, 8, request.header)) {
        return;
    }
    request.elementCount = frame.data[5];
    request.startIndex = static_cast<uint16_t>((static_cast<uint16_t>(frame.data[6]) << 8) | frame.data[7]);
    for (size_t i = 8; i < frame.data.size(); ++i) {
        (void)request.data.push_back(frame.data[i]);
    }
    (void)_propertyExtServices->handleValueWrite(frame.source, request, confirmed);
}

void ApplicationLayer::handlePropertyExtDescriptionRead(const ADataFrame& frame) {
    // header(5) + description_type/property_index high nibble(1) + index low(1)
    PropertyExtDescriptionRequest request;
    if (!decodePropertyExtHeader(frame, 7, request.header)) {
        return;
    }
    request.descriptionType = static_cast<uint8_t>((frame.data[5] >> 4) & 0x0Fu);
    request.propertyIndex =
        static_cast<uint16_t>((static_cast<uint16_t>(frame.data[5] & 0x0Fu) << 8) | frame.data[6]);
    (void)_propertyExtServices->handleDescriptionRead(frame.source, request);
}

void ApplicationLayer::handleFunctionPropertyExt(const ADataFrame& frame,
                                                 FunctionPropertyInvocation invocation) {
    FunctionPropertyExtRequest request;
    if (!decodePropertyExtHeader(frame, PropertyExtHeader::kEncodedLength, request.header)) {
        return;
    }
    for (size_t i = PropertyExtHeader::kEncodedLength; i < frame.data.size(); ++i) {
        (void)request.data.push_back(frame.data[i]);
    }
    request.security = frame.security;
    (void)_propertyExtServices->handleFunctionProperty(frame.source, request, invocation);
}

void ApplicationLayer::handleAuthorizeRequest(const ADataFrame& frame) {
    if (frame.data.size() < 4) {
        return;
    }
    AuthorizationKey key;
    std::copy(frame.data.begin(), frame.data.begin() + 4, key.begin());
    (void)_authorizationService->handleRequest(frame.source, key);
}

void ApplicationLayer::handleRestart(const ADataFrame& frame) {
    // 03/03/07 §3.4.2.2: restart_type is bit 0 of the APCI data field, NOT the
    // first payload octet.  A basic restart carries no payload at all, so the
    // old "data[0] is the type" reading ignored every basic restart outright and
    // mistook a master reset's erase code for its type.
    //
    // Bit 5 distinguishes the response (0x3A1) from a request within the same
    // service group; a device must not act on somebody else's response.
    constexpr uint8_t kRestartResponseBit = 0x20u;
    if ((frame.apciData & kRestartResponseBit) != 0u) {
        return;
    }

    const bool masterReset = (frame.apciData & 0x01u) != 0u;
    const RestartType type = masterReset ? RestartType::MasterReset : RestartType::Basic;

    if (masterReset && frame.data.size() < 2) {
        // Erase code + channel number are mandatory for a master reset.
        KNX_LOGW(TAG, "Master reset from 0x%04X without erase code/channel — ignored",
                 frame.source.raw);
        return;
    }

    const auto result = _restartService->handleRequest(frame.source, type);

    if (masterReset) {
        // A master reset MUST be answered before the device goes down
        // (§3.4.2.2), otherwise the management client only sees a timeout —
        // ETS reports exactly "The device does not respond".
        //
        // Process time is a 2-octet second count (DPT_TimePeriodSec); 03/05/02
        // §3.7.1.2.2 allows leaving it at 0 when the restart takes under 5 s,
        // and requires 0 when the confirmation is negative.
        const uint8_t errorCode = result.isOk() ? 0x00u : 0xFFu;
        ResponsePayload payload;
        if (appendPayloadByte(payload, errorCode)
            && appendPayloadByte(payload, 0x00u)
            && appendPayloadByte(payload, 0x00u)) {
            const auto sendResult = sendToIndividual(IndividualAddress(frame.source.raw),
                                                     APCIService::RestartResponse,
                                                     payload,
                                                     _serviceResponseSendOptions);
            KNX_LOGD(TAG,
                     "Master reset from 0x%04X (erase=0x%02X channel=%u) -> response err=0x%02X (%s)",
                     frame.source.raw, frame.data[0], frame.data[1], errorCode,
                     sendResult.isOk() ? "ok" : util::errorCodeToString(sendResult.error()));
        } else {
            logOversizedServiceResponse("RestartResponse", 3u);
        }
    }

    if (result.isOk()) {
        _restartService->executePendingRestart();
    }
}

void ApplicationLayer::handleAdcRead(const ADataFrame& frame) {
    // 03/03/07 §3.5.2 Figure 72: channel_nr is the APCI's 6-bit data field and
    // read_count the octet after it. The answer (Figure 73) mirrors both and
    // appends the 2-octet sum.
    // The receive path prepends the APCI data field to the payload for every
    // service that encodes an octet there, so data[0] is the channel echo and
    // data[1] the read count.
    const uint8_t channel = frame.apciData & 0x3Fu;
    const uint8_t readCount = frame.data.size() >= 2u ? frame.data[1] : uint8_t{0};

    // "If the remote application process has a problem, e.g., overflow when
    // computing the summation, or wrong channel number, then the read_count of
    // the A_ADC_Response-PDU shall be zero." A device with no AD converter is
    // permanently in that case — but it still has to say so. Staying silent
    // costs the client its full connection timeout (observed: ETS spending
    // 10 s on this one request mid-commissioning).
    std::optional<uint16_t> sum;
    if (_adcReadProvider && readCount != 0u) {
        sum = _adcReadProvider(channel, readCount);
    }

    ResponsePayload payload;
    if (!appendPayloadByte(payload, channel)
        || !appendPayloadByte(payload, sum.has_value() ? readCount : uint8_t{0})
        || !appendPayloadByte(payload, static_cast<uint8_t>((sum.value_or(0) >> 8) & 0xFFu))
        || !appendPayloadByte(payload, static_cast<uint8_t>(sum.value_or(0) & 0xFFu))) {
        logOversizedServiceResponse("ADCResponse", 4u);
        return;
    }

    const auto sendResult = sendToIndividual(IndividualAddress(frame.source.raw),
                                             APCIService::ADCResponse,
                                             payload,
                                             _serviceResponseSendOptions);
    KNX_LOGD(TAG,
             "ADCRead from 0x%04X (channel=%u count=%u) -> sum=%u%s (%s)",
             frame.source.raw, channel, readCount,
             static_cast<unsigned>(sum.value_or(0)),
             sum.has_value() ? "" : " [channel not available]",
             sendResult.isOk() ? "ok" : util::errorCodeToString(sendResult.error()));
}

void ApplicationLayer::handleIndividualAddressRead(const ADataFrame& frame) {
    if (!_programmingModeEnabled) {
        return;
    }

    const uint16_t responseAddress = _ownAddress.raw;
    const uint8_t responsePayload[] = {
        static_cast<uint8_t>((responseAddress >> 8) & 0xFF),
        static_cast<uint8_t>(responseAddress & 0xFF)
    };
    const auto sendResult = sendBroadcast(
        APCIService::IndividualAddressResponse,
        std::span<const uint8_t>(responsePayload, sizeof(responsePayload)),
        _serviceResponseSendOptions);
    KNX_LOGD(TAG,
             "IndividualAddressRead from 0x%04X -> responding 0x%04X (%s)",
             frame.source.raw,
             responseAddress,
             sendResult.isOk() ? "ok" : util::errorCodeToString(sendResult.error()));
}

void ApplicationLayer::handleSystemNetworkParameterRead(const ADataFrame& frame) {
    // 03/03/07 §3.3.8 Figure 31: object_type(2) + PID(12 bits) + reserved(4)
    // + test_info(n).
    constexpr size_t kParameterTypeBytes = 4u;
    if (frame.data.size() < kParameterTypeBytes) {
        return;
    }

    const uint16_t objectType = static_cast<uint16_t>(
        (static_cast<uint16_t>(frame.data[0]) << 8) | frame.data[1]);
    const uint16_t propertyId = static_cast<uint16_t>(
        (static_cast<uint16_t>(frame.data[2]) << 4) | ((frame.data[3] >> 4) & 0x0Fu));
    const auto testInfo = std::span<const uint8_t>(frame.data).subspan(kParameterTypeBytes);

    // 03/05/02 §2.17.1.4 NM_Read_SerialNumber_By_ProgrammingMode: Device Object,
    // PID_SERIAL_NUMBER, single operand octet 01h.  This is how ETS scans for
    // devices in programming mode.
    constexpr uint16_t kDeviceObjectType = 0u;
    constexpr uint16_t kPidSerialNumber = 11u;
    constexpr uint8_t kProgrammingModeOperand = 0x01u;

    const bool isProgrammingModeScan =
        objectType == kDeviceObjectType
        && propertyId == kPidSerialNumber
        && testInfo.size() == 1u
        && testInfo[0] == kProgrammingModeOperand;

    if (!isProgrammingModeScan) {
        // 03/05/02 §2.17.1: an unsupported object_type/PID combination means
        // stay silent.  The A_SystemNetworkParameter_Response with FFFFh/FFh
        // of 03/03/07 would put one broadcast per device on the bus for every
        // scan, which is precisely what a system broadcast must not do.
        KNX_LOGD(TAG,
                 "SystemNetworkParameterRead objectType=%u PID=%u unsupported — ignored",
                 static_cast<unsigned>(objectType),
                 static_cast<unsigned>(propertyId));
        return;
    }

    if (!_programmingModeEnabled) {
        // The whole point of the procedure: only devices in programming mode answer.
        return;
    }

    // Response echoes parameter_type + test_info and appends test_result, here
    // the device's own KNX serial number (03/05/02 Figure 8).
    ResponsePayload payload;
    const auto& serial = _networkParameterService->serialNumber();
    if (!appendPayloadByte(payload, frame.data[0])
        || !appendPayloadByte(payload, frame.data[1])
        || !appendPayloadByte(payload, frame.data[2])
        || !appendPayloadByte(payload, frame.data[3])
        || !appendPayloadByte(payload, kProgrammingModeOperand)
        || !appendPayloadBytes(payload, std::span<const uint8_t>(serial.data(), serial.size()))) {
        logOversizedServiceResponse("SystemNetworkParameterResponse",
                                    kParameterTypeBytes + 1u + serial.size());
        return;
    }

    // NOTE: 03/05/02 §2.17.1.4 asks for a random 0..1 s wait before answering,
    // to spread the responses when several devices are in programming mode at
    // once.  This sends immediately — same as handleIndividualAddressRead — so
    // simultaneous responders rely on TP1 CSMA arbitration alone.
    const auto sendResult = sendBroadcast(
        APCIService::SystemNetworkParameterResponse,
        payload,
        _serviceResponseSendOptions);
    KNX_LOGD(TAG,
             "NM_Read_SerialNumber_By_ProgrammingMode from 0x%04X -> responding (%s)",
             frame.source.raw,
             sendResult.isOk() ? "ok" : util::errorCodeToString(sendResult.error()));
}

void ApplicationLayer::handleIndividualAddressWrite(const ADataFrame& frame) {
    if (!_programmingModeEnabled) {
        return;
    }

    if (frame.data.size() < 2) {
        KNX_LOGW(TAG,
                 "Invalid IndividualAddressWrite payload size %zu from src=0x%04X",
                 frame.data.size(),
                 frame.source.raw);
        return;
    }

    const uint16_t newAddressValue = (static_cast<uint16_t>(frame.data[0]) << 8) | frame.data[1];
    const IndividualAddress newAddress(newAddressValue);
    _ownAddress = newAddress;
    if (_individualAddressUpdateCallback) {
        _individualAddressUpdateCallback(newAddress);
    }
    KNX_LOGD(TAG,
             "IndividualAddressWrite from 0x%04X -> own address set to 0x%04X",
             frame.source.raw,
             newAddressValue);
}

void ApplicationLayer::forwardToUser(const ADataFrame& frame) {
    // Already logged once by the "Application RX service=..." line.
    if (_rxCallback) {
        _rxCallback(frame.source,
                    frame.destination,
                    frame.service,
                    frame.data,
                    frame.destinationType);
        if (_receiveWorkAvailableCallback) {
            _receiveWorkAvailableCallback();
        }
    } else {
        enqueueReceivedFrame(frame);
    }
}

util::Result<void> ApplicationLayer::sendData(const GroupAddress& destination,
                                              APCIService service,
                                              std::span<const uint8_t> data,
                                              AddressType destinationType,
                                              const SendOptions& options) {
    transport::TDataFrame tFrame;
    ASendOutcome outcome;
    auto buildResult = buildTransportTxFrame(destination, service, data, destinationType, tFrame, outcome);
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

util::Result<void> ApplicationLayer::beginSendData(const GroupAddress& destination,
                                                   APCIService service,
                                                   std::span<const uint8_t> data,
                                                   AddressType destinationType,
                                                   const SendOptions& options) {
    if (_sendOperation.active) {
        return util::ErrorCode::Busy;
    }

    transport::TDataFrame tFrame;
    ASendOutcome outcome;
    auto buildResult = buildTransportTxFrame(destination, service, data, destinationType, tFrame, outcome);
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

util::Result<ApplicationLayer::SendProgressState> ApplicationLayer::pollSendData() {
    if (!_initialized) {
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

void ApplicationLayer::processBackgroundWork() {
    _transport.processBackgroundWork();
}

void ApplicationLayer::setReceiveCallback(DataCallback callback) {
    _rxCallback = callback;
}

void ApplicationLayer::setReceiveWorkAvailableCallback(WorkAvailableCallback callback) {
    _receiveWorkAvailableCallback = std::move(callback);
}

void ApplicationLayer::setOwnAddress(const IndividualAddress& ownAddress) {
    _ownAddress = ownAddress;
}

void ApplicationLayer::setIndividualAddressUpdateCallback(IndividualAddressUpdateCallback callback) {
    _individualAddressUpdateCallback = std::move(callback);
}

void ApplicationLayer::handleTransportRx(const transport::TDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGW(TAG, "Application layer not initialized, dropping transport frame src=0x%04X dst=0x%04X",
                 frame.source.raw,
                 frame.destination.raw);
        return;
    }

    if (frame.tpdu.size() < 2) {
        KNX_LOGW(TAG,
                 "Received TPDU too short for APCI header from src=0x%04X dst=0x%04X",
                 frame.source.raw,
                 frame.destination.raw);
        return;
    }

    const auto header = knx::protocol::unpackTpduHeader(frame.tpdu[0], frame.tpdu[1]);
    application::APCIField apciValue = header.apci;
    APCIService service = apciValue.service();




    // Service data is APDU payload bytes (after 2-byte header).
    // For services where data6 belongs to the service payload, prepend it first
    // so handlers receive a contiguous byte sequence.
    util::FixedVector<uint8_t, config::MAX_APDU_LENGTH> serviceData;
    const bool needsData6 = usesApciData6(service);
    if (needsData6) {
        serviceData.push_back(apciValue.data6());
    }
    if (frame.tpdu.size() > 2) {
        for (size_t i = 2; i < frame.tpdu.size(); ++i) {
            (void)serviceData.push_back(frame.tpdu[i]);
        }
    }

    // GroupValue short APDU carries the 1-byte value in APCI data6 (even if 0).
    // If a payload exists, it is the long APDU form and the payload bytes are used.
    if (service == APCIService::GroupValueWrite || service == APCIService::GroupValueResponse) {
        if (serviceData.empty()) {
            serviceData.push_back(apciValue.data6());
        }
    }

    // Create application frame
    ADataFrame aFrame;
    aFrame.source = frame.source;
    aFrame.destination = frame.destination;
    aFrame.service = service;
    aFrame.data = serviceData;
    aFrame.destinationType = frame.destinationType;
    aFrame.apciData = apciValue.data6();
    aFrame.security = frame.security;

    KNX_LOGD(TAG, "Application RX service=0x%03X src=0x%04X dst=0x%04X data=%s",
             static_cast<uint16_t>(aFrame.service),
             aFrame.source.raw,
             aFrame.destination.raw,
             util::toHex(aFrame.data).c_str());

    // Responses raised by the handlers must use the same transport service the
    // request arrived on. Saved and restored rather than just cleared: a handler
    // may re-enter through the user callback.
    const bool previousConnectedRequest = _handlingConnectedRequest;
    _handlingConnectedRequest = (frame.service == transport::TDataService::Connected);

    // Saved and restored for the same reason as the connected flag: a handler
    // can re-enter through the user callback, and the nested request must not
    // inherit this one's permissions.
    const RequestSecurity previousRequestSecurity = _requestSecurity;
    _requestSecurity = aFrame.security;

    if (isRequestPermitted(aFrame)) {
        // Dispatch to appropriate handler
        dispatchService(aFrame);
    }

    _requestSecurity = previousRequestSecurity;
    _handlingConnectedRequest = previousConnectedRequest;
}

bool ApplicationLayer::isRequestPermitted(const ADataFrame& aFrame) const {
    // Group communication is the common case and is gated by the Group Object
    // Security Flags rather than by this policy, so it never reaches the
    // provider call.
    if (!isManagementWriteService(aFrame.service)) {
        return true;
    }

    const bool securityModeEnabled = _securityModeProvider && _securityModeProvider();
    if (managementWritePermitted(aFrame.service, securityModeEnabled, aFrame.security)) {
        return true;
    }

    // Dropped without an answer. 03/4/1 §6.2.2 defines a Permission as whether
    // the indication "shall be accepted", and an error response would both
    // confirm the device is there and tell an unauthorised sender which
    // services it implements.
    KNX_LOGW(TAG,
             "Service 0x%03X from 0x%04X refused: security mode is on and the request is %s",
             static_cast<uint16_t>(aFrame.service),
             aFrame.source.raw,
             aFrame.security.secured ? "not tool-secured" : "not secured");
    return false;
}

std::optional<uint16_t> ApplicationLayer::objectTypeForIndex(InterfaceObjectIndex objectIndex) const {
    if (!_propertyStoreManager) {
        return std::nullopt;
    }
    const auto* store = _propertyStoreManager->getObject(objectIndex);
    if (store == nullptr) {
        return std::nullopt;
    }
    return store->getObjectType().value();
}

bool ApplicationLayer::isPropertyAccessPermitted(uint16_t objectType,
                                                 uint16_t propertyId,
                                                 bool write,
                                                 const RequestSecurity& security) const {
    if (objectType == kSecurityInterfaceObjectType) {
        // Independent of the Security Mode: 03/05/01 §6.3.5 binds the write
        // side of PID_SECURITY_MODE to secure communication "regardless of its
        // value", and key material is never readable in plain either.
        return securityObjectAccessPermitted(propertyId, write, security);
    }
    return true;
}

void ApplicationLayer::dispatchService(const ADataFrame& aFrame) {
    switch (aFrame.service) {
        case APCIService::DeviceDescriptorRead:
            handleDeviceDescriptorRead(aFrame);
            return;

        case APCIService::PropertyValueRead:
            handlePropertyValueRead(aFrame);
            return;

        case APCIService::PropertyValueWrite:
            handlePropertyValueWrite(aFrame);
            return;

        case APCIService::PropertyDescriptionRead:
            handlePropertyDescriptionRead(aFrame);
            return;

        case APCIService::PropertyExtValueRead:
            handlePropertyExtValueRead(aFrame);
            break;

        case APCIService::PropertyExtValueWriteCon:
            handlePropertyExtValueWrite(aFrame, true);
            break;

        case APCIService::PropertyExtValueWriteUnCon:
            handlePropertyExtValueWrite(aFrame, false);
            break;

        case APCIService::PropertyExtDescriptionRead:
            handlePropertyExtDescriptionRead(aFrame);
            break;

        case APCIService::FunctionPropertyExtCommand:
            handleFunctionPropertyExt(aFrame, FunctionPropertyInvocation::Command);
            break;

        case APCIService::FunctionPropertyExtStateRead:
            handleFunctionPropertyExt(aFrame, FunctionPropertyInvocation::StateRead);
            break;

        case APCIService::MemoryExtendedRead:
            handleMemoryExtendedRead(aFrame);
            break;

        case APCIService::MemoryExtendedWrite:
            handleMemoryExtendedWrite(aFrame);
            break;

        case APCIService::MemoryRead:
            handleMemoryRead(aFrame);
            return;

        case APCIService::MemoryWrite:
            handleMemoryWrite(aFrame);
            return;

        case APCIService::AuthorizeRequest:
            handleAuthorizeRequest(aFrame);
            return;

        case APCIService::Restart:
            handleRestart(aFrame);
            return;

        case APCIService::ADCRead:
            handleAdcRead(aFrame);
            return;


        case APCIService::IndividualAddressRead:
            handleIndividualAddressRead(aFrame);
            return;

        case APCIService::SystemNetworkParameterRead:
            handleSystemNetworkParameterRead(aFrame);
            return;

        case APCIService::IndividualAddressWrite:
            handleIndividualAddressWrite(aFrame);
            return;

        case APCIService::FunctionPropertyCommand:
        case APCIService::FunctionPropertyStateRead:
            handleFunctionProperty(aFrame);
            return;

        case APCIService::IndividualAddressSerialNumberRead:
            handleIndividualAddressSerialNumberRead(aFrame);
            return;

        case APCIService::IndividualAddressSerialNumberWrite:
            handleIndividualAddressSerialNumberWrite(aFrame);
            return;

        case APCIService::NetworkParameterRead:
            handleNetworkParameterRead(aFrame);
            return;

        case APCIService::NetworkParameterWrite:
            handleNetworkParameterWrite(aFrame);
            return;

        default:
            forwardToUser(aFrame);
            return;
    }
}

util::Result<void> ApplicationLayer::sendGroupValueWrite(const GroupAddress& destination,
                                                         std::span<const uint8_t> data) {
    return sendData(destination, APCIService::GroupValueWrite, data, AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::beginSendGroupValueWrite(const GroupAddress& destination,
                                                              std::span<const uint8_t> data) {
    return beginSendData(destination, APCIService::GroupValueWrite, data, AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::sendGroupValueRead(const GroupAddress& destination) {
    return sendData(destination, APCIService::GroupValueRead, std::span<const uint8_t>{}, AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::beginSendGroupValueRead(const GroupAddress& destination) {
    return beginSendData(destination, APCIService::GroupValueRead, std::span<const uint8_t>{}, AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::sendGroupValueResponse(const GroupAddress& destination,
                                                            std::span<const uint8_t> data) {
    return sendData(destination, APCIService::GroupValueResponse, data, AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::beginSendGroupValueResponse(const GroupAddress& destination,
                                                                 std::span<const uint8_t> data) {
    return beginSendData(destination, APCIService::GroupValueResponse, data, AddressType::Group, _defaultSendOptions);
}

} // namespace application
} // namespace knx
