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
 * @brief Backend provider for property reads
 *
 * The interface objects own the property values; the BAU answers from them.
 * Returning nullopt means the provider cannot answer (no such property, or
 * the element range does not exist): both service families then send their
 * negative response.
 */
using PropertyReadProvider = util::InplaceFunction<std::optional<util::Result<size_t>>(
    const IndividualAddress& source,
    const PropertyValueReadRequest& request,
    std::span<uint8_t> out
), 32>;

/**
 * @brief Backend provider for property writes
 *
 * Performs the write on the interface objects. Without one the classic
 * services refuse every write.
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
 * @brief The current access level of a requester, in wire numbering
 *
 * 0 is maximum access. A property write is refused when the requester's level
 * is numerically above the write level in the property's description
 * (03/03/07 §3.5.7: the current access level decides which writes a partner
 * may make). Left unset, no level is enforced.
 */
using AccessLevelQuery = util::InplaceFunction<uint8_t(const IndividualAddress& source), 32>;

/**
 * @brief Property description info
 */
struct PropertyDescriptionInfo {
    InterfaceObjectIndex objectIndex{};
    PropertyID propertyId{};
    PropertyIndex propertyIndex{};
    PropertyWriteAccess writeAccess{PropertyWriteAccess::Denied};
    /// False for a write-only property. Not part of the classic description
    /// PDU, but the extended value services answer E_ACCESS_WRITE_ONLY with it.
    bool readable{true};
    PropertyDataType type{PropertyDataType::GenericData};
    uint16_t maxElements{1};
    uint8_t readLevel{0};
    uint8_t writeLevel{0};
};

/**
 * @brief Backend provider for property descriptions
 *
 * The descriptions of the interface objects, addressed as 03/03/07 §3.4.3
 * addresses them: by @p propertyId when it is not zero, otherwise by the
 * 0-based @p propertyIndex. The returned info carries the property's PID and
 * its index either way. Besides answering A_PropertyDescription_Read and
 * A_PropertyExtDescription_Read, it supplies the existence, access and write
 * level both value services check against. Returning nullopt means "no such
 * object or property".
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
    /**
     * @brief Set property value response callback
     */
    void setValueResponseCallback(PropertyValueResponseCallback callback);
    
    /**
     * @brief Set property description response callback
     */
    void setDescriptionResponseCallback(PropertyDescriptionResponseCallback callback);

    /**
     * @brief Set the backend read provider
     */
    void setReadProvider(PropertyReadProvider provider);

    /**
     * @brief Set the backend write provider
     */
    void setWriteProvider(PropertyWriteProvider provider);

    /**
     * @brief Set the backend description provider
     */
    void setDescriptionProvider(PropertyDescriptionProvider provider);

    /**
     * @brief Set the Access Policy gate applied before every property access
     */
    void setAccessCheck(PropertyAccessCheck check);

    /**
     * @brief Set the source of the requester's access level for writes
     */
    void setAccessLevelQuery(AccessLevelQuery query) { _accessLevel = std::move(query); }

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
    
private:
    PropertyValueResponseCallback _valueResponseCallback;
    PropertyDescriptionResponseCallback _descriptionResponseCallback;

    PropertyReadProvider _readProvider;
    PropertyWriteProvider _writeProvider;
    PropertyDescriptionProvider _descriptionProvider;
    PropertyAccessCheck _accessCheck;
    AccessLevelQuery _accessLevel;
};

} // namespace application
} // namespace knx
