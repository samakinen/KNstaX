// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_ext_services.hpp
 * @brief KNX extended property services (A_PropertyExt* / A_FunctionPropertyExt*)
 *
 * Implements 03/03/07 Application Layer v02.01.01 §3.4.3.2, §3.4.5 and §3.4.8.
 * Profiles v02.01.01 §9.1.2.3 makes these mandatory for the KNX Data Security
 * profile, which is why a Data Secure product needs them even on mask 07B0.
 *
 * These are not a wider-integer restatement of PropertyServices.  They address
 * an interface object by (object type, object instance) rather than by object
 * index, carry 12-bit property IDs, use a 16-bit start index, and answer every
 * request — including failures — with a return code from the shared KNX Error
 * Code Set.  That last point is the one that matters at commissioning time: a
 * silent refusal leaves the tool waiting.
 */

#pragma once

#include "knx/application/apci_services.hpp"
#include "knx/application/function_property_services.hpp"
#include "knx/application/property_services.hpp"
#include "knx/application/property_store.hpp"
#include "knx/types.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace knx {
namespace application {

/**
 * @brief Extended property services
 *
 * Server (device) side: decodes the requests a management client sends and
 * produces the matching responses.
 */
class PropertyExtServices {
public:
    using ValueResponseCallback = std::function<void(
        const IndividualAddress& destination,
        APCIService service,
        const PropertyExtValueResponse& response)>;

    using DescriptionResponseCallback = std::function<void(
        const IndividualAddress& destination,
        const PropertyExtDescriptionResponse& response)>;

    using FunctionResponseCallback = std::function<void(
        const IndividualAddress& destination,
        const FunctionPropertyExtResponse& response)>;

    /**
     * @brief Optional backend for function property invocation
     *
     * Mirrors the classic FunctionPropertyServices hook.  Returning nullopt
     * means "no such function property", which becomes E_DATA_TYPE_CONFLICT.
     */
    using FunctionProvider = util::InplaceFunction<std::optional<KnxReturnCode>(
        const IndividualAddress& source,
        const PropertyExtHeader& header,
        FunctionPropertyInvocation invocation,
        const RequestSecurity& security,
        std::span<const uint8_t> input,
        FunctionPropertyExtResponse::DataBuffer& output), 48>;

    /**
     * @brief Access Policy gate (03/4/1 §6.2), installed by the application layer.
     *
     * The extended services address an object by *type*, so unlike the classic
     * ones no index resolution is needed to know what is being touched.
     * Returning false denies the access, which is answered with E_ACCESS_DENIED.
     */
    using AccessCheck = util::InplaceFunction<bool(
        uint16_t objectType,
        uint16_t propertyId,
        bool write), 32>;

    explicit PropertyExtServices(PropertyStoreManager& storeManager);

    void setValueResponseCallback(ValueResponseCallback callback) {
        _valueResponseCallback = std::move(callback);
    }
    void setDescriptionResponseCallback(DescriptionResponseCallback callback) {
        _descriptionResponseCallback = std::move(callback);
    }
    void setFunctionResponseCallback(FunctionResponseCallback callback) {
        _functionResponseCallback = std::move(callback);
    }
    /// Route value access to the live interface objects instead of the local
    /// property store. Without these, an extended property write lands in a
    /// store blob that nothing reads: ETS installs its own Tool Key through
    /// A_PropertyExtValue_WriteCon, so a device that only stores it keeps
    /// answering with the old key and ETS reports "no key to decrypt".
    void setReadProvider(PropertyReadProvider provider) {
        _readProvider = std::move(provider);
    }

    void setWriteProvider(PropertyWriteProvider provider) {
        _writeProvider = std::move(provider);
    }

    void setFunctionProvider(FunctionProvider provider) {
        _functionProvider = std::move(provider);
    }

    void setAccessCheck(AccessCheck check) {
        _accessCheck = std::move(check);
    }

    /// A_PropertyExtValue_Read → A_PropertyExtValue_Response
    util::Result<void> handleValueRead(const IndividualAddress& source,
                                       const PropertyExtValueRequest& request);

    /// A_PropertyExtValue_WriteCon → A_PropertyExtValue_WriteConRes.
    /// @param confirmed false for _WriteUnCon, which must not answer.
    util::Result<void> handleValueWrite(const IndividualAddress& source,
                                        const PropertyExtValueRequest& request,
                                        bool confirmed);

    /// A_PropertyExtDescription_Read → A_PropertyExtDescription_Response
    util::Result<void> handleDescriptionRead(const IndividualAddress& source,
                                             const PropertyExtDescriptionRequest& request);

    /// A_FunctionPropertyExtCommand / _State_Read → _State_Response
    util::Result<void> handleFunctionProperty(const IndividualAddress& source,
                                              const FunctionPropertyExtRequest& request,
                                              FunctionPropertyInvocation invocation);

    /**
     * @brief Resolve (object type, object instance) to an object index.
     *
     * Object instance is 1-based and counts objects of that type in index
     * order, per 03/05/01.  Exposed for tests and for callers that need the
     * same mapping.
     */
    std::optional<InterfaceObjectIndex> resolveObject(uint16_t objectType,
                                                      uint16_t objectInstance) const;

private:
    PropertyStoreManager& _storeManager;
    PropertyReadProvider _readProvider;
    PropertyWriteProvider _writeProvider;
    ValueResponseCallback _valueResponseCallback;
    DescriptionResponseCallback _descriptionResponseCallback;
    FunctionResponseCallback _functionResponseCallback;
    FunctionProvider _functionProvider;
    AccessCheck _accessCheck;

    void sendValueFailure(const IndividualAddress& destination,
                          APCIService service,
                          const PropertyExtValueRequest& request,
                          KnxReturnCode returnCode);
};

} // namespace application
} // namespace knx
