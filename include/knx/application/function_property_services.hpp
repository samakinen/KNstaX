// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file function_property_services.hpp
 * @brief KNX Function Property services (A_FunctionPropertyCommand,
 *        A_FunctionPropertyState_Read / _Response).
 *
 * Per KNX 03/03/07 §3.4.5.  A Function Property is an interface-object
 * property whose datatype is PDT_FUNCTION: instead of holding a value it
 * invokes device behaviour and answers with a return code plus function
 * specific output.
 *
 * This matters well beyond being a completeness item: it is how ETS drives
 * KNX Data Secure commissioning.  PID_SECURITY_MODE on the Security Interface
 * Object is a Function Property, so a device that only implements
 * A_PropertyValue_Write cannot be switched into secure mode by ETS.
 */

#pragma once

#include "knx/application/apci_services.hpp"
#include "knx/application/property.hpp"
#include "knx/types.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace application {

/// Maximum function input/output payload carried in one APDU.
inline constexpr size_t kMaxFunctionPropertyDataBytes = 32u;

using FunctionPropertyDataBuffer =
    util::FixedVector<uint8_t, kMaxFunctionPropertyDataBytes>;

/**
 * @brief Standard return codes (03/03/07 §3.4.5.3).
 *
 * 0x00 is the single success value; every other code is function specific.
 * The named entries below are the ones this stack produces itself.
 */
enum class FunctionPropertyReturnCode : uint8_t {
    Success = 0x00,
    /// Addressed object or property does not exist, or is not PDT_FUNCTION.
    InvalidProperty = 0x01,
    /// Input data did not match what the function expects.
    InvalidCommand = 0x02,
    /// The caller lacks the required access level.
    AccessDenied = 0x03,

    // ── PID_GO_DIAGNOSTICS (03/05/01 §4.8.1) ────────────────────────────────
    // Group Object Diagnostics is specified directly in the unified return
    // code space of 03/03/07 §3.4.8.3 rather than in the small function
    // specific codes above, and it uses the same values on the classic and the
    // extended service.  They are named here so a handler can express them;
    // translateFunctionPropertyReturnCode() passes them through unchanged.
    GdConfig = 0x20,            ///< E_GD_CONFIG (positive; carries the GO config)
    GdGoStatusValue = 0x21,     ///< E_GD_GO_STATUS_VALUE (positive; carries GO status + value)
    GdGoVoid = 0xA1,            ///< E_GD_GO_VOID (no such Group Object Number)
    GdConfigFlags = 0xA2,       ///< E_GD_CONFIG_FLAGS (the GO's config flags refuse this)
    GdGoSizeMismatch = 0xA3,    ///< E_GD_GO_SIZE_MISMATCH (value too short or too long)
    CommandInvalid = 0xF2,      ///< E_COMMAND_INVALID (unknown Read-/WriteServiceID)
    CommandImpossible = 0xF3,   ///< E_COMMAND_IMPOSSIBLE (wrong Operation Mode)
    DataVoid = 0xF8,            ///< E_DATA_VOID (ServiceInfo carries invalid data)
    Error = 0xFF,               ///< E_ERROR (none of the above applies)
};

/**
 * @brief Which of the two request services invoked a function.
 *
 * The distinction is semantic, not cosmetic: Command may change device state,
 * State_Read must not.  A handler that ignores this would let a read-only
 * query mutate the device.
 */
enum class FunctionPropertyInvocation : uint8_t {
    Command,    ///< A_FunctionPropertyCommand — may change state.
    StateRead,  ///< A_FunctionPropertyState_Read — must be side-effect free.
};

/**
 * @brief A single decoded Function Property request.
 */
struct FunctionPropertyRequest {
    InterfaceObjectIndex objectIndex{0};
    PropertyID propertyId{};
    FunctionPropertyInvocation invocation{FunctionPropertyInvocation::Command};
    FunctionPropertyDataBuffer data{};
    /// How the request was secured. PID_SECURITY_MODE is a Function Property
    /// and 03/05/01 §6.3.5 lets only a tool-secured client command it, so the
    /// handler cannot decide without this.
    RequestSecurity security{};
};

/**
 * @brief Result of invoking a function property.
 */
struct FunctionPropertyResult {
    FunctionPropertyReturnCode returnCode{FunctionPropertyReturnCode::Success};
    FunctionPropertyDataBuffer data{};
};

/**
 * @brief Handler installed by an interface object for its function properties.
 *
 * Returning an error means "this object/property is not a function property",
 * which the service turns into the spec's degenerate response (no return code,
 * no data).  A function that ran but failed must return ok() with a non-zero
 * return code instead — the difference is observable to ETS.
 */
using FunctionPropertyHandler = util::InplaceFunction<
    util::Result<FunctionPropertyResult>(const IndividualAddress& source,
                                         const FunctionPropertyRequest& request),
    64>;

/**
 * @brief Emits A_FunctionPropertyState_Response back to the requester.
 *
 * `hasReturnCode` is false only for the error case of §3.4.5.3, where the
 * response must carry neither a return code nor data.
 */
using FunctionPropertyResponseCallback = util::InplaceFunction<
    void(const IndividualAddress& destination,
         InterfaceObjectIndex objectIndex,
         PropertyID propertyId,
         bool hasReturnCode,
         FunctionPropertyReturnCode returnCode,
         std::span<const uint8_t> data),
    64>;

/**
 * @brief Function Property service handler.
 *
 * @thread_safety Owner-context only, like the rest of the application layer.
 */
class FunctionPropertyServices {
public:
    /// Octets before the function payload: object_index + property_id.
    static constexpr size_t kRequestHeaderBytes = 2u;

    void setHandler(FunctionPropertyHandler handler) { _handler = std::move(handler); }
    void setResponseCallback(FunctionPropertyResponseCallback callback) {
        _responseCallback = std::move(callback);
    }

    /**
     * @brief Handle a decoded request and emit the response.
     */
    util::Result<void> handleRequest(const IndividualAddress& source,
                                     const FunctionPropertyRequest& request);

    /**
     * @brief Decode an A_FunctionPropertyCommand / State_Read APDU.
     *
     * @param apdu Full APDU including the 2-octet TPCI/APCI header.
     */
    static util::Result<FunctionPropertyRequest> decodeRequest(std::span<const uint8_t> apdu);

    /**
     * @brief Encode an A_FunctionPropertyState_Response APDU.
     *
     * @return Number of octets written to `out`.
     */
    static util::Result<size_t> encodeResponse(InterfaceObjectIndex objectIndex,
                                               PropertyID propertyId,
                                               bool hasReturnCode,
                                               FunctionPropertyReturnCode returnCode,
                                               std::span<const uint8_t> data,
                                               std::span<uint8_t> out);

private:
    FunctionPropertyHandler _handler{};
    FunctionPropertyResponseCallback _responseCallback{};
};

} // namespace application
} // namespace knx
