// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file generic_interface_object.hpp
 * @brief Generic Interface Object with raw property storage
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/result.hpp"
#include <map>
#include <mutex>
#include <vector>

namespace knx {
namespace objects {

class GenericInterfaceObject : public InterfaceObject {
public:
    /// Construct from the reference-object manifest. Schema is pulled directly
    /// from referenceObjectPropertyManifestEntries(type) — no caller-supplied
    /// registration list needed. Only valid for types returned by isReferenceObjectType().
    explicit GenericInterfaceObject(InterfaceObjectType type);
    ~GenericInterfaceObject() override = default;

    GenericInterfaceObject(const GenericInterfaceObject&) = delete;
    GenericInterfaceObject& operator=(const GenericInterfaceObject&) = delete;
    GenericInterfaceObject(GenericInterfaceObject&&) = delete;
    GenericInterfaceObject& operator=(GenericInterfaceObject&&) = delete;

    InterfaceObjectType objectType() const override { return _type; }
    size_t getPropertyRegistrations(std::span<PropertyRegistrationInfo> out) const override;
    KernelBinding kernelBinding() const override;

    /**
     * @brief Seed a property with a device-supplied value.
     *
     * Reference objects otherwise answer every read with zeros, which for
     * something like the KNXnet/IP Parameter Object means ETS sees a device
     * with no IP address and no name.  The runtime uses this to publish the
     * values it actually knows (address, MAC, friendly name, ...).
     *
     * Fails when the property is not part of this object's manifest, or when
     * the value is larger than the manifest's declared capacity.
     */
    util::Result<void> setPropertyValue(application::PropertyID propertyId,
                                        std::span<const uint8_t> value);

    /// Current stored value, or an empty span when the property was never set.
    /// The copy is taken under the value mutex, so the caller owns the result.
    std::vector<uint8_t> propertyValue(application::PropertyID propertyId) const;

private:
    static util::Result<void> readGeneric(
        const PropertyContext& context,
        DomainIndex startIndex,
        uint16_t elementCount,
        util::ByteWriter& out,
        const void* userData);

    static util::Result<void> writeGeneric(
        const PropertyContext& context,
        DomainIndex startIndex,
        uint16_t elementCount,
        util::ByteReader& in,
        const void* userData);

    bool validateHandlersOnce_() const;
    void initFromRegistrations_(std::span<const PropertyRegistrationInfo> registrations);

    struct GenericPropertyInfo {
        application::PropertyID id{};
        uint16_t maxElements{1};
        uint8_t elementSize{1};
        application::PropertyDataType type{application::PropertyDataType::GenericData};
    };

    static constexpr size_t kMaxRegistrationCount = InterfaceObject::kMaxPropertyRegistrations;
    static constexpr size_t kMaxHandlerCount = kMaxRegistrationCount + 1u;

    InterfaceObjectType _type;
    util::FixedVector<PropertyRegistrationInfo, kMaxRegistrationCount> _registrations;
    mutable std::mutex _valuesMutex;
    std::map<application::PropertyID, std::vector<uint8_t>> _values;
    util::FixedVector<GenericPropertyInfo, kMaxRegistrationCount> _handlerInfo;
    util::FixedVector<PropertyHandler, kMaxHandlerCount> _handlers;
    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
    mutable bool _handlersValidated{false};
    mutable bool _handlersValid{true};
};

} // namespace objects
} // namespace knx
