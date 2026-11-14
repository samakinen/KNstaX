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
#include "knx/util/inplace_function.hpp"
#include "knx/objects/reference_object_registry.hpp"
#include "knx/util/result.hpp"
#include <map>
#include <mutex>
#include <vector>

namespace knx {
namespace objects {

class GenericInterfaceObject : public InterfaceObject {
public:
    /// Construct from the reference-object declarations,
    /// referenceObjectProperties(type). Only valid for types returned by
    /// isReferenceObjectType().
    explicit GenericInterfaceObject(InterfaceObjectType type);
    ~GenericInterfaceObject() override = default;

    GenericInterfaceObject(const GenericInterfaceObject&) = delete;
    GenericInterfaceObject& operator=(const GenericInterfaceObject&) = delete;
    GenericInterfaceObject(GenericInterfaceObject&&) = delete;
    GenericInterfaceObject& operator=(GenericInterfaceObject&&) = delete;

    InterfaceObjectType objectType() const override { return _type; }
    KernelBinding kernelBinding() const override;

    /**
     * @brief Seed a property with a device-supplied value.
     *
     * Reference objects otherwise answer every read with zeros, which for
     * something like the KNXnet/IP Parameter Object means ETS sees a device
     * with no IP address and no name.  The runtime uses this to publish the
     * values it actually knows (address, MAC, friendly name, ...).
     *
     * Fails when the object does not declare the property, or when the value
     * is larger than its declared capacity.
     */
    util::Result<void> setPropertyValue(application::PropertyID propertyId,
                                        std::span<const uint8_t> value);

    /// Current stored value, or an empty span when the property was never set.
    /// The copy is taken under the value mutex, so the caller owns the result.
    std::vector<uint8_t> propertyValue(application::PropertyID propertyId) const;

    /**
     * @brief Called after a remote write lands in this object.
     *
     * Some properties are not inert storage. PID_BACKBONE_KEY is the reason
     * this exists: ETS downloads the group key for KNX/IP Secure Routing as an
     * ordinary property write, and a device that only files it away keeps
     * multicasting in the clear while its project believes it is secured.
     *
     * The observer runs on the stack task that handled the write, after the
     * value is stored and outside the value mutex, and is given copies of the
     * value before and after the write rather than references into storage —
     * which is what lets it tell a change from the same value written again
     * (03_08_09 §2.2.2.3.2.5: E11 against E12). Keep it short — it sits
     * inside the property service's response path.
     */
    using WriteObserver = util::InplaceFunction<void(application::PropertyID propertyId,
                                                     std::span<const uint8_t> previous,
                                                     std::span<const uint8_t> value), 64>;
    void setWriteObserver(WriteObserver observer);

    /**
     * @brief Called before a remote write is stored; an error refuses it.
     *
     * Given the whole value the write would leave behind. A refused write
     * changes nothing and is reported to the writer the way any other invalid
     * value is. Runs under the value mutex, so it must not reach back into
     * this object.
     */
    using WriteValidator = util::InplaceFunction<util::Result<void>(application::PropertyID propertyId,
                                                                    std::span<const uint8_t> value), 32>;
    void setWriteValidator(WriteValidator validator);

    /**
     * @brief Back a declared property with runtime state instead of storage.
     *
     * For properties that are behaviour, not data: the Router Object's
     * PID_LOAD_STATE_CONTROL (a read returns the load state, a write is a
     * load event), PID_TABLE_REFERENCE and PID_MCB_TABLE are owned by the
     * Filter Table's load state machine. @p read supplies the whole current
     * value; @p write receives the whole value the write leaves behind (for
     * PDT_CONTROL, the control block as written). Neither observer nor
     * validator runs for a bound property. Bind before the object is
     * reachable by the management services.
     */
    struct PropertyBinding {
        util::InplaceFunction<std::vector<uint8_t>(), 32> read{};
        util::InplaceFunction<util::Result<void>(std::span<const uint8_t> value), 32> write{};
    };
    util::Result<void> bindProperty(application::PropertyID propertyId, PropertyBinding binding);

private:
    const PropertyBinding* findBinding(application::PropertyID propertyId) const;

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
    void initFromDeclarations_(std::span<const PropertyDeclaration> declarations);

    struct GenericPropertyInfo {
        application::PropertyID id{};
        uint16_t maxElements{1};
        uint8_t elementSize{1};
        application::PropertyDataType type{application::PropertyDataType::GenericData};
    };

    static constexpr size_t kMaxRegistrationCount = 64u;
    static constexpr size_t kMaxHandlerCount = kMaxRegistrationCount + 1u;

    InterfaceObjectType _type;
    mutable std::mutex _valuesMutex;
    std::map<application::PropertyID, std::vector<uint8_t>> _values;
    util::FixedVector<GenericPropertyInfo, kMaxRegistrationCount> _handlerInfo;
    util::FixedVector<PropertyHandler, kMaxHandlerCount> _handlers;
    WriteObserver _writeObserver{};
    WriteValidator _writeValidator{};
    std::vector<std::pair<application::PropertyID, PropertyBinding>> _bindings{};
    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
    mutable bool _handlersValidated{false};
    mutable bool _handlersValid{true};
};

} // namespace objects
} // namespace knx
