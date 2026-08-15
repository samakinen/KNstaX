// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_services.hpp
 * @brief Property service handlers
 * 
 * Handles property value read/write and property description services.
 * Per KNX spec 03/03/07 (Application Layer).
 */

#pragma once

#include "knx/application/property.hpp"
#include "knx/application/property_store.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>

#include "knx/util/inplace_function.hpp"
#include <optional>
#include <span>

namespace knx {
namespace application {

/**
 * @brief Property value response callback
 */
using PropertyValueResponseCallback = std::function<void(
    const IndividualAddress& destination,
    const PropertyValueResponse& response
)>;

/**
 * @brief Property description response callback
 */
using PropertyDescriptionResponseCallback = std::function<void(
    const IndividualAddress& destination,
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    PropertyIndex propertyIndex,
    PropertyWriteAccess writeAccess,
    PropertyDataType type,
    uint16_t maxElements,
    uint8_t readLevel,
    uint8_t writeLevel
)>;

/**
 * @brief Optional backend provider for property reads
 *
 * If set and it returns a value, it overrides the store read.
 * Returning nullopt defers to the property store.
 */
using PropertyReadProvider = util::InplaceFunction<std::optional<util::Result<size_t>>(
    const IndividualAddress& source,
    const PropertyValueReadRequest& request,
    std::span<uint8_t> out
), 32>;

/**
 * @brief Optional backend consumer for property writes
 *
 * If set, it is invoked before writing to the store; returning an error rejects the write.
 */
using PropertyWriteConsumer = util::InplaceFunction<util::Result<void>(
    const IndividualAddress& source,
    const PropertyValueWriteRequest& request
), 32>;

/**
 * @brief Optional backend provider for property writes
 *
 * If set, it fully handles the write and bypasses the store.
 */
using PropertyWriteProvider = util::InplaceFunction<util::Result<void>(
    const IndividualAddress& source,
    const PropertyValueWriteRequest& request
), 32>;

/**
 * @brief Access Policy gate for property access (03/4/1 §6.2)
 *
 * Installed by the application layer, which is the only place that knows how
 * the request was secured. Returning false denies the access; the service then
 * answers with the same "NumberOfElements = 0" response it uses for a property
 * that does not exist — a client without permission must not be able to tell
 * the two apart, and ETS must not be left waiting for a reply that never comes.
 *
 * Left unset, every access is allowed.
 */
using PropertyAccessCheck = util::InplaceFunction<bool(
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    bool write
), 32>;

/**
 * @brief Property description info
 */
struct PropertyDescriptionInfo {
    InterfaceObjectIndex objectIndex{};
    PropertyID propertyId{};
    PropertyIndex propertyIndex{};
    PropertyWriteAccess writeAccess{PropertyWriteAccess::Denied};
    PropertyDataType type{PropertyDataType::GenericData};
    uint16_t maxElements{1};
    uint8_t readLevel{0};
    uint8_t writeLevel{0};
};

/**
 * @brief Optional backend provider for property descriptions
 *
 * If set and it returns a value, it bypasses the store-based descriptor lookup.
 */
using PropertyDescriptionProvider = util::InplaceFunction<std::optional<PropertyDescriptionInfo>(
    const IndividualAddress& source,
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    PropertyIndex propertyIndex
), 32>;

/**
 * @brief Property Services
 * 
 * Handles A_PropertyValue_Read/Write and A_PropertyDescription_Read.
 */
class PropertyServices {
public:
    static constexpr size_t kEncodedValueReadRequestLength = 6;
    static constexpr size_t kEncodedValueResponseHeaderLength = 6;
    static constexpr size_t kEncodedValueWriteRequestHeaderLength = 6;
    static constexpr size_t kEncodedDescriptionReadRequestLength = 5;
    static constexpr size_t kEncodedDescriptionResponseLength = 9;

    /**
     * @brief Constructor
     * @param storeManager Property store manager
     */
    explicit PropertyServices(PropertyStoreManager& storeManager);
    
    /**
     * @brief Set property value response callback
     */
    void setValueResponseCallback(PropertyValueResponseCallback callback);
    
    /**
     * @brief Set property description response callback
     */
    void setDescriptionResponseCallback(PropertyDescriptionResponseCallback callback);

    /**
     * @brief Set optional backend read provider
     */
    void setReadProvider(PropertyReadProvider provider);

    /**
     * @brief Set optional backend write consumer
     */
    void setWriteConsumer(PropertyWriteConsumer consumer);

    /**
     * @brief Set optional backend write provider (bypasses store)
     */
    void setWriteProvider(PropertyWriteProvider provider);

    /**
     * @brief Set optional backend description provider (bypasses store)
     */
    void setDescriptionProvider(PropertyDescriptionProvider provider);

    /**
     * @brief Set the Access Policy gate applied before every property access
     */
    void setAccessCheck(PropertyAccessCheck check);
    
    /**
     * @brief Handle property value read request
     * 
     * @param source Source address
     * @param request Read request
     * @return Result<void> indicating success or error
     */
    util::Result<void> handleValueRead(
        const IndividualAddress& source,
        const PropertyValueReadRequest& request
    );
    
    /**
     * @brief Handle property value write request
     * 
     * @param source Source address
     * @param request Write request
     * @return Result<void> indicating success or error
     */
    util::Result<void> handleValueWrite(
        const IndividualAddress& source,
        const PropertyValueWriteRequest& request
    );
    
    /**
     * @brief Handle property description read request
     * 
     * @param source Source address
     * @param objectIndex Object index
     * @param propertyId Property ID or index
     * @param propertyIndex Property index (0 for ID query)
     * @return Result<void> indicating success or error
     */
    util::Result<void> handleDescriptionRead(
        const IndividualAddress& source,
        InterfaceObjectIndex objectIndex,
        PropertyID propertyId,
        PropertyIndex propertyIndex
    );
    
    // Encoding/decoding helpers
    
    /**
     * @brief Encode property value read request
     */
    static util::Result<void> encodeValueReadRequest(
        const PropertyValueReadRequest& request,
        std::span<uint8_t, kEncodedValueReadRequestLength> out
    );
    
    /**
     * @brief Decode property value read request
     */
    static PropertyValueReadRequest decodeValueReadRequest(
        std::span<const uint8_t> data
    );
    
    /**
     * @brief Encode property value response
     */
    static util::Result<size_t> encodeValueResponse(
        const PropertyValueResponse& response,
        std::span<uint8_t> out
    );
    
    /**
     * @brief Decode property value response
     */
    static PropertyValueResponse decodeValueResponse(
        std::span<const uint8_t> data
    );
    
    /**
     * @brief Encode property value write request
     */
    static util::Result<size_t> encodeValueWriteRequest(
        const PropertyValueWriteRequest& request,
        std::span<uint8_t> out
    );
    
    /**
     * @brief Decode property value write request
     */
    static PropertyValueWriteRequest decodeValueWriteRequest(
        std::span<const uint8_t> data
    );
    
    /**
     * @brief Encode property description read request
     */
    static util::Result<void> encodeDescriptionReadRequest(
        InterfaceObjectIndex objectIndex,
        PropertyID propertyId,
        PropertyIndex propertyIndex,
        std::span<uint8_t, kEncodedDescriptionReadRequestLength> out
    );
    
    /**
     * @brief Encode property description response
     */
    static util::Result<void> encodeDescriptionResponse(
        InterfaceObjectIndex objectIndex,
        PropertyID propertyId,
        PropertyIndex propertyIndex,
        PropertyWriteAccess writeAccess,
        PropertyDataType type,
        uint16_t maxElements,
        uint8_t readLevel,
        uint8_t writeLevel,
        std::span<uint8_t, kEncodedDescriptionResponseLength> out
    );

private:
    PropertyStoreManager& _storeManager;
    PropertyValueResponseCallback _valueResponseCallback;
    PropertyDescriptionResponseCallback _descriptionResponseCallback;

    PropertyReadProvider _readProvider;
    PropertyWriteConsumer _writeConsumer;
    PropertyWriteProvider _writeProvider;
    PropertyDescriptionProvider _descriptionProvider;
    PropertyAccessCheck _accessCheck;
};

} // namespace application
} // namespace knx
