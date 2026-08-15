// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau_go_diagnostics.cpp
 * @brief PID_GO_DIAGNOSTICS of the Group Object Table Object (03/05/01 §4.8.1).
 *
 * Group Object Diagnostics is how a management client reads or writes a group
 * value *through* the device instead of putting the telegram on the bus itself.
 * ETS needs it for every Data Secure group address: it has no Sequence Number
 * Sending of its own for the group key, so "read/write the group address" in the
 * ETS group monitor is compiled into a Function Property command on this object
 * and the device is the one that transmits.
 *
 * Split out of bau_management.cpp because it is the only management service that
 * reaches into the group-object runtime.
 */

#include "knx/bau/bau.hpp"

#include "knx/application/function_property_services.hpp"
#include "knx/objects/security_interface_object.hpp"
#include "knx/util/log.hpp"

#include <array>

namespace knx {
namespace bau {

namespace {

constexpr const char* TAG = "KNX.BAU.GoDiag";

using application::FunctionPropertyInvocation;
using application::FunctionPropertyRequest;
using application::FunctionPropertyResult;
using application::FunctionPropertyReturnCode;

/// PID_GO_DIAGNOSTICS on the Group Object Table Object.
constexpr uint8_t kPidGoDiagnostics = 66;

/// WriteServiceIDs, Table 35.
enum : uint8_t {
    kWriteSetLocalValue = 0x00,      ///< Set local GO value
    kWriteSendGroupValueWrite = 0x01,///< Send A_GroupValue_Write
    kWriteSendLocalValueOnBus = 0x02,///< Send local GO value on bus
    kWriteSendGroupValueRead = 0x03, ///< Send A_GroupValue_Read
    kWriteLimitServiceSenders = 0x04,///< Limit GO service senders (Diagnostic Mode only)
};

/// ReadServiceIDs, Table 41.
enum : uint8_t {
    kReadGoConfig = 0x00,   ///< Get GO Config
    kReadLocalValue = 0x01, ///< Get local GO value
};

// Security bits of the Flags field, "the same meaning as the GO Security flags"
// of PID_GO_SECURITY_FLAGS (§6.3.15, Figure 81): b0 authentication, b1
// confidentiality, everything above reserved and required to be 0.
constexpr uint8_t kFlagAuthentication = 0x01;
constexpr uint8_t kFlagConfidentiality = 0x02;
constexpr uint8_t kFlagSecurityMask = kFlagAuthentication | kFlagConfidentiality;
/// WriteServiceID 01h only: "Selector", whether a 1-octet value below 64 may
/// ride in the six data bits of the APCI.  This stack always uses the compact
/// form for such values, which is what the flag's 0-value asks for.
constexpr uint8_t kFlagSelector = 0x80;

/// Transmission Status of the GO Status octet, Table 33.
enum : uint8_t {
    kTransmissionIdleOk = 0x00,
    kTransmissionIdleError = 0x01,
};

uint16_t readU16(std::span<const uint8_t> data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

/// Payload width the object's datapoint type calls for, in whole octets.
/// Sub-octet datapoints occupy one octet on this path — the six-bit form is a
/// property of the APCI encoding, not of the value handed to the GO server.
size_t expectedValueOctets(const application::GroupObject& object)
{
    const uint16_t bits = application::dptPayloadBits(object.dptId());
    if (bits == 0) {
        // Unknown datapoint: accept whatever the object is already carrying.
        return object.getRawValue().size();
    }
    return bits < 8u ? 1u : static_cast<size_t>(bits) / 8u;
}

} // namespace

util::Result<FunctionPropertyResult> BusAccessUnit::handleGroupObjectDiagnostics(
    const FunctionPropertyRequest& request)
{
    if (request.propertyId != static_cast<application::PropertyID>(kPidGoDiagnostics)) {
        // Not a function property of this object; the caller turns this into
        // the §3.4.5.3 / E_DATA_TYPE_CONFLICT answer.
        return util::ErrorCode::OperationNotSupported;
    }

    const auto data = request.data.span();
    FunctionPropertyResult result{};

    // Octet 10 is Reserved, octet 11 the Read-/WriteServiceID.  §4.8.1.1.7 is
    // explicit that the first octet after Reserved is always the command and
    // that nothing beyond it may be interpreted for an unknown one.
    if (data.size() < 2u) {
        result.returnCode = FunctionPropertyReturnCode::CommandInvalid;
        return result;
    }
    const uint8_t serviceId = data[1];
    const auto serviceInfo = data.subspan(2);

    // Every response, positive or negative, repeats the ServiceID (Figures 22
    // to 29), so start each answer from that and append.
    const auto answer = [&](FunctionPropertyReturnCode code)
        -> util::Result<FunctionPropertyResult> {
        result.returnCode = code;
        result.data.clear();
        (void)result.data.push_back(serviceId);
        return result;
    };

    // Resolve the Group Object Number, which §4.8.1 defines as the ASAP — the
    // same number ETS downloads in the Association Table, and therefore the
    // index this stack stores group objects under.
    const auto resolveObject = [&](application::GroupObject** out) -> bool {
        if (serviceInfo.size() < 2u) {
            return false;
        }
        *out = _groupObjectTable.getGroupObject(GroupObjectIndex(readU16(serviceInfo)));
        return *out != nullptr;
    };

    // E_GD_GO_STATUS_VALUE: ServiceID, GO Number, GO Status, then the value.
    // The Update and Data Request bits of the GO Status are optional per
    // §4.8.1.1.2 and this stack keeps no such communication flags, so only the
    // Transmission Status is reported.
    const auto answerStatusValue = [&](uint8_t transmissionStatus,
                                       const application::GroupObject& object)
        -> util::Result<FunctionPropertyResult> {
        result.returnCode = FunctionPropertyReturnCode::GdGoStatusValue;
        result.data.clear();
        (void)result.data.push_back(serviceId);
        (void)result.data.push_back(serviceInfo[0]);
        (void)result.data.push_back(serviceInfo[1]);
        (void)result.data.push_back(transmissionStatus);
        for (const uint8_t octet : object.getRawValue()) {
            if (!result.data.push_back(octet)) {
                // The value does not fit the APDU this service can answer in.
                return answer(FunctionPropertyReturnCode::Error);
            }
        }
        return result;
    };

    if (request.invocation == FunctionPropertyInvocation::StateRead) {
        application::GroupObject* object = nullptr;

        switch (serviceId) {
            case kReadGoConfig: {
                if (!resolveObject(&object)) {
                    return answer(FunctionPropertyReturnCode::GdGoVoid);
                }

                // Figure 22/23: GO Number, GO Config, Size, DPT_ID.  Bits 0..7
                // of GO Config are the higher octet of the System B Group
                // Object Descriptor (U/T/I/W/R/C + priority), bit 8 auth, bit 9
                // conf, bit 10 "at least one GA is linked to this GO".
                const uint16_t goNumber = readU16(serviceInfo);
                uint16_t config = static_cast<uint16_t>(object->descriptor() >> 8);

                const auto* goFlags = _securityObject.findExtraProperty(
                    objects::SecurityProperty::GoSecurityFlags);
                if (goFlags != nullptr && goNumber < goFlags->size()) {
                    const uint8_t security = (*goFlags)[goNumber];
                    if ((security & kFlagAuthentication) != 0u) config |= 0x0100u;
                    if ((security & kFlagConfidentiality) != 0u) config |= 0x0200u;
                }
                if (isGroupObjectLinked(GroupObjectIndex(goNumber))) {
                    config |= 0x0400u;
                }

                const auto dpt = object->dptId();
                result.returnCode = FunctionPropertyReturnCode::GdConfig;
                result.data.clear();
                (void)result.data.push_back(serviceId);
                (void)result.data.push_back(serviceInfo[0]);
                (void)result.data.push_back(serviceInfo[1]);
                (void)result.data.push_back(static_cast<uint8_t>(config >> 8));
                (void)result.data.push_back(static_cast<uint8_t>(config & 0xFFu));
                (void)result.data.push_back(object->valueFieldType());
                (void)result.data.push_back(static_cast<uint8_t>(dpt.mainNumber() >> 8));
                (void)result.data.push_back(static_cast<uint8_t>(dpt.mainNumber() & 0xFFu));
                (void)result.data.push_back(static_cast<uint8_t>(dpt.sub >> 8));
                (void)result.data.push_back(static_cast<uint8_t>(dpt.sub & 0xFFu));
                return result;
            }

            case kReadLocalValue:
                if (!resolveObject(&object)) {
                    return answer(FunctionPropertyReturnCode::GdGoVoid);
                }
                // §4.8.1.4.3: reading the local value is subject to no config
                // flag and no GO security flag, and touches no communication
                // flag.  Nothing goes on the bus except the answer itself.
                return answerStatusValue(kTransmissionIdleOk, *object);

            default:
                return answer(FunctionPropertyReturnCode::CommandInvalid);
        }
    }

    application::GroupObject* object = nullptr;

    switch (serviceId) {
        case kWriteSetLocalValue: {
            if (!resolveObject(&object)) {
                return answer(FunctionPropertyReturnCode::GdGoVoid);
            }
            const auto value = serviceInfo.subspan(2);
            // §4.8.1.3.2: a missing Data field is a size mismatch, and so is a
            // value at least one octet too short or too long.
            if (value.empty() || value.size() != expectedValueOctets(*object)) {
                return answer(FunctionPropertyReturnCode::GdGoSizeMismatch);
            }
            // "This command shall be handled as an accepted A_GroupValue_Write
            // for the GO", so Roles and Permissions no longer apply but the
            // Config flags still do — which is exactly what receiveValue()
            // enforces.
            if (object->receiveValue(value, application::GroupObject::ValueSource::Write)
                    .isError()) {
                return answer(FunctionPropertyReturnCode::GdConfigFlags);
            }
            return answerStatusValue(kTransmissionIdleOk, *object);
        }

        case kWriteSendGroupValueWrite: {
            // Figure 35: Flags, Group Address, Data.
            if (serviceInfo.size() < 4u) {
                return answer(FunctionPropertyReturnCode::DataVoid);
            }
            const uint8_t flags = serviceInfo[0];
            const GroupAddress destination(readU16(serviceInfo.subspan(1)));
            const auto value = serviceInfo.subspan(3);
            if (value.empty()) {
                return answer(FunctionPropertyReturnCode::DataVoid);
            }
            const auto flagCheck = validateGroupDiagnosticsFlags(flags, destination, true);
            if (flagCheck != FunctionPropertyReturnCode::Success) {
                return answer(flagCheck);
            }

            // Step 1: put the telegram on the bus.  The return code is
            // explicitly independent of whether the transmission succeeds
            // (NOTE 42), so only a refusal to even build it is reported.
            const auto sendResult = sendGroupValue(destination, value);
            if (sendResult.isError()) {
                KNX_LOGW(TAG, "GO diagnostics write on %d/%d/%d failed: %s",
                         destination.main(), destination.middle(), destination.sub(),
                         util::errorCodeToString(sendResult.error()));
                return answer(FunctionPropertyReturnCode::Error);
            }

            // Step 2: hand the same service to our own Application Layer, so
            // the local objects linked to that group address are updated just
            // as if the telegram had come from the bus.
            handleApplicationFrame(_deviceObject.getIndividualAddress(), destination,
                                   MessageKind::GroupValueWrite, value, AddressType::Group);
            return answer(FunctionPropertyReturnCode::Success);
        }

        case kWriteSendLocalValueOnBus: {
            if (!resolveObject(&object)) {
                return answer(FunctionPropertyReturnCode::GdGoVoid);
            }
            if (!object->transmit()) {
                return answer(FunctionPropertyReturnCode::GdConfigFlags);
            }
            const GroupObjectIndex index(readU16(serviceInfo));
            // The raw emit path, not sendGroupValueForObject(): a diagnostic
            // request must not be swallowed by the send-on-change filter or
            // deferred by the min-interval floor.
            const auto sendResult = emitGroupValueForObject(index, object->getRawValue());
            return answerStatusValue(sendResult.isOk() ? kTransmissionIdleOk
                                                       : kTransmissionIdleError,
                                     *object);
        }

        case kWriteSendGroupValueRead: {
            // Figure 39: Flags, Group Address.
            if (serviceInfo.size() < 3u) {
                return answer(FunctionPropertyReturnCode::DataVoid);
            }
            const uint8_t flags = serviceInfo[0];
            const GroupAddress destination(readU16(serviceInfo.subspan(1)));
            const auto flagCheck = validateGroupDiagnosticsFlags(flags, destination, false);
            if (flagCheck != FunctionPropertyReturnCode::Success) {
                return answer(flagCheck);
            }

            const auto sendResult = requestGroupValue(destination);
            if (sendResult.isError()) {
                KNX_LOGW(TAG, "GO diagnostics read on %d/%d/%d failed: %s",
                         destination.main(), destination.middle(), destination.sub(),
                         util::errorCodeToString(sendResult.error()));
                return answer(FunctionPropertyReturnCode::Error);
            }

            // §4.8.1.3.5 step 2: the read indication also goes to our own
            // Application Layer.  Without it the requesting client would never
            // see this device's own value — a locally originated telegram is
            // not looped back through the receive path.
            handleApplicationFrame(_deviceObject.getIndividualAddress(), destination,
                                   MessageKind::GroupValueRead, {}, AddressType::Group);
            return answer(FunctionPropertyReturnCode::Success);
        }

        case kWriteLimitServiceSenders:
            // Table 34 makes this one available exclusively in Diagnostic
            // Mode, which this device does not implement; §4.8.1.1.5 names
            // E_COMMAND_IMPOSSIBLE as the answer for exactly that.
            return answer(FunctionPropertyReturnCode::CommandImpossible);

        default:
            return answer(FunctionPropertyReturnCode::CommandInvalid);
    }
}

FunctionPropertyReturnCode BusAccessUnit::validateGroupDiagnosticsFlags(
    uint8_t flags, const GroupAddress& destination, bool allowSelector) const
{
    // §4.8.1.3.3 / §4.8.1.3.5, Table 37 and Table 39: E_DATA_VOID covers a
    // reserved bit that is not 0, an unknown group address, and security
    // requested for a group address that has no key.
    const uint8_t allowed =
        static_cast<uint8_t>(kFlagSecurityMask | (allowSelector ? kFlagSelector : 0u));
    if ((flags & static_cast<uint8_t>(~allowed)) != 0u) {
        return FunctionPropertyReturnCode::DataVoid;
    }
    if (destination.raw == 0u || !_addressTable.findIndex(destination).isValid()) {
        return FunctionPropertyReturnCode::DataVoid;
    }
    if ((flags & kFlagSecurityMask) != 0u) {
        std::array<uint8_t, 16> key{};
        if (_securityObject.getGroupKey(destination, key).isError()) {
            return FunctionPropertyReturnCode::DataVoid;
        }
    }
    return FunctionPropertyReturnCode::Success;
}

} // namespace bau
} // namespace knx
