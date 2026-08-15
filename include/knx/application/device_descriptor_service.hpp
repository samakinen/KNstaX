// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_descriptor_service.hpp
 * @brief Device Descriptor Service handler
 * 
 * Handles A_DeviceDescriptor_Read requests and generates responses.
 * Per KNX spec 03/05/01 (Resources).
 */

#pragma once

#include "knx/application/device_descriptor.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace knx {
namespace application {

/**
 * @brief Response callback for device descriptor service
 * 
 * Called when a descriptor response needs to be sent.
 * 
 * @param destination Destination address
 * @param descriptorType Descriptor type (0 or 2)
 * @param data Encoded descriptor data
 */
using DescriptorResponseCallback = std::function<void(
    const IndividualAddress& destination,
    uint8_t descriptorType,
    std::span<const uint8_t> data
)>;

struct DescriptorSupportEntry {
    uint8_t descriptorType;
    bool supported;
    size_t encodedSize;
};

/**
 * @brief Device Descriptor Service
 * 
 * Handles device descriptor read requests and generates appropriate responses.
 */
class DeviceDescriptorService {
public:
    static constexpr std::array<DescriptorSupportEntry, 4> kDescriptorSupportMatrix{{
        DescriptorSupportEntry{0u, true, 2u},
        DescriptorSupportEntry{1u, false, 0u},
        DescriptorSupportEntry{2u, true, 14u},
        DescriptorSupportEntry{3u, false, 0u},
    }};

    /**
     * @brief Constructor
     * @param descriptor Device descriptor data
     */
    explicit DeviceDescriptorService(const DeviceDescriptor& descriptor);
    
    /**
     * @brief Set response callback
     * @param callback Function to call when sending response
     */
    void setResponseCallback(DescriptorResponseCallback callback);
    
    /**
     * @brief Handle device descriptor read request
     * 
     * Processes A_DeviceDescriptor_Read and triggers response via callback.
     * 
     * @param source Source address of requester
     * @param descriptorType Requested descriptor type (0 or 2)
    * @return Result<void> indicating success or error
     */
    util::Result<void> handleReadRequest(const IndividualAddress& source, uint8_t descriptorType);
    
    /**
     * @brief Encode descriptor read request
     * 
     * Creates APCI data for A_DeviceDescriptor_Read service.
     * 
     * @param descriptorType Descriptor type to request
        * @param out Caller-managed TPDU buffer
        * @return Number of bytes written, or an error code
     */
        static util::Result<size_t> encodeReadRequest(uint8_t descriptorType, std::span<uint8_t, 2> out);
    
    /**
     * @brief Decode descriptor read request
     * 
     * Extracts descriptor type from A_DeviceDescriptor_Read APCI data.
     * 
     * @param apciData APCI data bytes
     * @return Descriptor type
     */
    static uint8_t decodeReadRequest(std::span<const uint8_t> apciData);
    
    /**
     * @brief Encode descriptor response
     * 
     * Creates APCI data for A_DeviceDescriptor_Response service.
     * 
     * @param descriptorType Descriptor type
     * @param descriptorData Encoded descriptor
     * @param out Caller-managed TPDU buffer
     * @return Number of bytes written, or an error code
     */
    static util::Result<size_t> encodeResponse(
        uint8_t descriptorType,
        std::span<const uint8_t> descriptorData,
        std::span<uint8_t> out
    );
    
    /**
     * @brief Decode descriptor response
     * 
     * Extracts descriptor data from A_DeviceDescriptor_Response APCI data.
     * 
     * @param apciData APCI data bytes
     * @param descriptorType Output: descriptor type
     * @param out Caller-managed descriptor buffer
     * @return Number of bytes written, or an error code
     */
    static util::Result<size_t> decodeResponse(
        std::span<const uint8_t> apciData,
        uint8_t& descriptorType,
        std::span<uint8_t> out
    );
    
    /**
     * @brief Update device descriptor
     * @param descriptor New descriptor data
     */
    void setDescriptor(const DeviceDescriptor& descriptor);
    
    /**
     * @brief Get current device descriptor
     * @return Device descriptor
     */
    const DeviceDescriptor& getDescriptor() const { return _descriptor; }

    static constexpr bool isDescriptorTypeSupported(uint8_t descriptorType)
    {
        for (const auto& entry : kDescriptorSupportMatrix) {
            if (entry.descriptorType == descriptorType) {
                return entry.supported;
            }
        }
        return false;
    }

    static constexpr size_t encodedDescriptorSize(uint8_t descriptorType)
    {
        for (const auto& entry : kDescriptorSupportMatrix) {
            if (entry.descriptorType == descriptorType) {
                return entry.encodedSize;
            }
        }
        return 0u;
    }

private:
    DeviceDescriptor _descriptor;
    DescriptorResponseCallback _responseCallback;
};

} // namespace application
} // namespace knx
