// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_descriptor_service.cpp
 * @brief Device Descriptor Service implementation
 */

#include "knx/application/device_descriptor_service.hpp"
#include "knx/util/log.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/util/result.hpp"
#include <algorithm>
#include <array>

static const char* TAG = "KNX.App.DescService";

namespace knx {
namespace application {

DeviceDescriptorService::DeviceDescriptorService(const DeviceDescriptor& descriptor)
    : _descriptor(descriptor)
    , _responseCallback(nullptr)
{
}

void DeviceDescriptorService::setResponseCallback(DescriptorResponseCallback callback) {
    _responseCallback = callback;
}

util::Result<void> DeviceDescriptorService::handleReadRequest(
    const IndividualAddress& source,
    uint8_t descriptorType
) {
    if (!_responseCallback) {
        KNX_LOGW(TAG, "No response callback set");
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }

    if (!isDescriptorTypeSupported(descriptorType)) {
        KNX_LOGW(TAG, "Unsupported descriptor type requested: %d (supported: 0, 2)", descriptorType);
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    // Encode the requested descriptor type
    switch (descriptorType) {
        case 0: {
            // Type 0 descriptor - 2 bytes
            auto encoded = _descriptor.type0.encode();
            KNX_LOGD(TAG, "Responding with Type 0 descriptor: mask=0x%02X%02X",
                     _descriptor.type0.maskHigh,
                     _descriptor.type0.maskLow);
            _responseCallback(source, descriptorType, encoded);
            break;
        }
        
        case 2: {
            // Type 2 descriptor - 14 bytes
            auto encoded = _descriptor.type2.encode();
            KNX_LOGD(TAG, "Responding with Type 2 descriptor: device=0x%04X, app=0x%04X",
                     _descriptor.type2.deviceType.value(),
                     _descriptor.type2.applicationVersion);
            _responseCallback(source, descriptorType, encoded);
            break;
        }
        
        default:
            // Should not be reached due to isDescriptorTypeSupported check above.
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    return util::Result<void>::ok();
}

util::Result<size_t> DeviceDescriptorService::encodeReadRequest(uint8_t descriptorType, std::span<uint8_t, 2> out) {
    // A_DeviceDescriptor_Read encodes descriptorType in the 6-bit APCI data field.
    const auto apci = APCIField::create(APCIService::DeviceDescriptorRead, descriptorType);
    return knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        apci,
        std::span<const uint8_t>(),
        out
    );
}

uint8_t DeviceDescriptorService::decodeReadRequest(std::span<const uint8_t> apciData) {
    // Accept TPDU (2-byte header) and extract the 6-bit APCI data.
    if (apciData.size() >= 2) {
        const auto hdr = knx::protocol::unpackTpduHeader(apciData[0], apciData[1]);
        if (hdr.apci.service() != APCIService::DeviceDescriptorRead) {
            return 0xFF;
        }
        return hdr.apci.data6();
    }

    KNX_LOGW(TAG, "Invalid device descriptor read request size: %zu", apciData.size());
    return 0xFF;
}

util::Result<size_t> DeviceDescriptorService::encodeResponse(
    uint8_t descriptorType,
    std::span<const uint8_t> descriptorData,
    std::span<uint8_t> out
) {
    // A_DeviceDescriptor_Response encodes descriptorType in the 6-bit APCI data field.
    const auto apci = APCIField::create(APCIService::DeviceDescriptorResponse, descriptorType);
    return knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        apci,
        descriptorData,
        out
    );
}

util::Result<size_t> DeviceDescriptorService::decodeResponse(
    std::span<const uint8_t> apciData,
    uint8_t& descriptorType,
    std::span<uint8_t> out
) {
    // Prefer TPDU: 2-byte header, payload is descriptor data.
    if (apciData.size() >= 2) {
        const auto hdr = knx::protocol::unpackTpduHeader(apciData[0], apciData[1]);
        if (hdr.apci.service() != APCIService::DeviceDescriptorResponse) {
            descriptorType = 0xFF;
            return util::ErrorCode::InvalidParameter;
        }
        descriptorType = hdr.apci.data6();
        if (apciData.size() > 2) {
            const size_t payloadSize = apciData.size() - 2;
            if (out.size() < payloadSize) {
                return util::ErrorCode::BufferTooSmall;
            }
            std::copy_n(apciData.begin() + 2, payloadSize, out.begin());
            return payloadSize;
        }
        return 0;
    }

    KNX_LOGW(TAG, "Invalid response data size: %zu", apciData.size());
    descriptorType = 0xFF;
    return util::ErrorCode::InvalidParameter;
}

void DeviceDescriptorService::setDescriptor(const DeviceDescriptor& descriptor) {
    _descriptor = descriptor;
    KNX_LOGI(TAG, "Device descriptor updated");
}

} // namespace application
} // namespace knx
