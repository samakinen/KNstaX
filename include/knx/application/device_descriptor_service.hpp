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
#include "knx/util/inplace_function.hpp"
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
using DescriptorResponseCallback = util::InplaceFunction<void(
    const IndividualAddress& destination,
    uint8_t descriptorType,
    std::span<const uint8_t> data
), 32>;

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

    /// The descriptor_type of the error answer to an unsupported type
    /// (03/03/07 §3.4.2.1), sent without data.
    static constexpr uint8_t kUnsupportedDescriptorType = 0x3Fu;

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
