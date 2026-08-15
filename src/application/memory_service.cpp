// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file memory_service.cpp
 * @brief KNX Memory Service implementation
 */

#include "knx/application/memory_service.hpp"
#include "knx/util/log.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/util/result.hpp"

#include <algorithm>

namespace knx {
namespace application {

static const char* TAG = "KNX.App.Memory";

MemoryService::MemoryService(AddressSpace& addressSpace)
    : _addressSpace(addressSpace) {
}

static util::ErrorCode toErrorCode(MemoryAccessResult result) {
    switch (result) {
        case MemoryAccessResult::Success:
            return util::ErrorCode::Success;
        case MemoryAccessResult::InvalidAddress:
        case MemoryAccessResult::RegionNotFound:
            return util::ErrorCode::InvalidAddress;
        case MemoryAccessResult::InvalidLength:
            return util::ErrorCode::InvalidParameter;
        case MemoryAccessResult::AccessDenied:
            return util::ErrorCode::OperationNotSupported;
        default:
            return util::ErrorCode::OperationFailed;
    }
}

util::Result<void> MemoryService::handleReadRequest(const IndividualAddress& source, 
                                      uint8_t count, MemoryAddress address) {
    // Validate access
        auto result = validateAccess(address, count, AccessType::Read);
    if (result != MemoryAccessResult::Success) {
        KNX_LOGW(TAG, "Read denied at 0x%04X (%d bytes): result=%d", 
             address.raw, count, static_cast<int>(result));
        return util::Result<void>::err(toErrorCode(result));
    }
    
    // Execute read callback
    if (!_readCallback) {
        KNX_LOGE(TAG, "No read callback registered");
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }
    
    MemoryResponse::DataBuffer data;
    data.resize(count);
    auto readRes = _readCallback(address, count, data.span());
    if (readRes.isError()) {
        KNX_LOGE(TAG, "Read callback failed at 0x%04X", address.raw);
        return readRes.error();
    }
    
    // Send response
    MemoryResponse response;
    response.count = count;
    response.address = address;
    response.data = data;
    sendResponse(source, response);
    
    KNX_LOGD(TAG, "Read %d bytes from 0x%04X", count, address.raw);
    return util::Result<void>::ok();
}

util::Result<void> MemoryService::handleWriteRequest(const IndividualAddress& source, 
                                       uint8_t count, MemoryAddress address, 
                                       std::span<const uint8_t> data) {
    // Validate access
        auto result = validateAccess(address, count, AccessType::Write);
    if (result != MemoryAccessResult::Success) {
            KNX_LOGW(TAG, "Write denied at 0x%04X (%d bytes): result=%d", 
                     address.raw, count, static_cast<int>(result));
        return util::Result<void>::err(toErrorCode(result));
    }
    
    // Validate data size
    if (data.size() != count) {
        KNX_LOGE(TAG, "Data size mismatch: expected %d, got %zu", count, data.size());
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    // Execute write callback
    if (!_writeCallback) {
        KNX_LOGE(TAG, "No write callback registered");
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }
    
    auto writeRes = _writeCallback(address, data);
    if (writeRes.isError()) {
        KNX_LOGE(TAG, "Write callback failed at 0x%04X", address.raw);
        return writeRes.error();
    }

    // Verify response (KNX 03.03.07): A_Memory_Response after a write carries
    // the memory content read back — the count is encoded in the APCI, so a
    // response without the data octets is a malformed frame that ETS rejects.
    MemoryResponse response;
    response.count = count;
    response.address = address;
    response.data.resize(count);
    if (_readCallback) {
        auto verifyRes = _readCallback(address, count, response.data.span());
        if (verifyRes.isError()) {
            KNX_LOGE(TAG, "Verify read-back failed at 0x%04X", address.raw);
            return verifyRes.error();
        }
    } else {
        // No independent read path: echo the data that was just written.
        std::copy(data.begin(), data.end(), response.data.begin());
    }
    sendResponse(source, response);
    
    KNX_LOGD(TAG, "Wrote %d bytes to 0x%04X", count, address.raw);
    return util::Result<void>::ok();
}

util::Result<void> MemoryService::encodeReadRequest(uint8_t count,
                                                    MemoryAddress address,
                                                    std::span<uint8_t, kEncodedReadRequestLength> out) {
    if (count == 0 || count > MAX_MEMORY_BYTES) {
        return util::ErrorCode::InvalidParameter;
    }

    const auto apci = APCIField::create(APCIService::MemoryRead, count);
    const std::array<uint8_t, 2> payload = {
        static_cast<uint8_t>((address.raw >> 8) & 0xFF),
        static_cast<uint8_t>(address.raw & 0xFF)
    };

    auto result = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        apci,
        payload,
        out
    );
    if (result.isError()) return result.error();
    return util::Result<void>::ok();
}

util::Result<size_t> MemoryService::encodeWriteRequest(uint8_t count,
                                                       MemoryAddress address,
                                                       std::span<const uint8_t> data,
                                                       std::span<uint8_t> out) {
    if (count == 0 || count > MAX_MEMORY_BYTES) {
        return util::ErrorCode::InvalidParameter;
    }
    
    if (data.size() != count) {
        return util::ErrorCode::InvalidParameter;
    }
    
    if (out.size() < 4 + data.size()) {
        return util::ErrorCode::BufferTooSmall;
    }

    const auto apci = APCIField::create(APCIService::MemoryWrite, count);
    out[2] = static_cast<uint8_t>((address.raw >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(address.raw & 0xFF);
    for (size_t i = 0; i < data.size(); ++i) {
        out[4 + i] = data[i];
    }

    auto headerResult = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        apci,
        out.subspan(2, 2 + data.size()),
        out
    );
    if (headerResult.isError()) return headerResult.error();
    return headerResult.value();
}

util::Result<size_t> MemoryService::encodeResponse(uint8_t count,
                                                   MemoryAddress address,
                                                   std::span<const uint8_t> data,
                                                   std::span<uint8_t> out) {
    if (count == 0 || count > MAX_MEMORY_BYTES) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // Data size must match count for read responses (0 for write responses)
    if (data.size() != count && !data.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    
    if (out.size() < 4 + data.size()) {
        return util::ErrorCode::BufferTooSmall;
    }

    const auto apci = APCIField::create(APCIService::MemoryResponse, count);
    out[2] = static_cast<uint8_t>((address.raw >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(address.raw & 0xFF);
    for (size_t i = 0; i < data.size(); ++i) {
        out[4 + i] = data[i];
    }

    auto headerResult = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        apci,
        out.subspan(2, 2 + data.size()),
        out
    );
    if (headerResult.isError()) return headerResult.error();
    return headerResult.value();
}

util::Result<void> MemoryService::decodeRequest(std::span<const uint8_t> data, uint8_t& count, MemoryAddress& address) {
    if (data.size() < 4) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    const auto hdr = knx::protocol::unpackTpduHeader(data[0], data[1]);
    const auto svc = hdr.apci.service();
    if (svc != APCIService::MemoryRead && svc != APCIService::MemoryWrite && svc != APCIService::MemoryResponse) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    count = hdr.apci.data6();
    
    if (count == 0 || count > MAX_MEMORY_BYTES) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    // Extract 16-bit address (TPDU payload starts at offset 2)
    address = MemoryAddress(static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | static_cast<uint16_t>(data[3])));
    
    return util::Result<void>::ok();
}

util::Result<void> MemoryService::decodeWriteRequest(std::span<const uint8_t> data, uint8_t& count, 
                                       MemoryAddress& address, std::span<uint8_t> writeData) {
    auto decodeRes = decodeRequest(data, count, address);
    if (decodeRes.isError()) {
        return decodeRes;
    }
    
    // Extract write data
    const size_t expectedSize = 4u + static_cast<size_t>(count);
    if (data.size() < expectedSize) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    if (writeData.size() < count) {
        return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
    }

    std::copy(data.begin() + 4, data.begin() + 4 + count, writeData.begin());
    return util::Result<void>::ok();
}

MemoryAccessResult MemoryService::validateAccess(MemoryAddress address,
                                                 uint8_t length,
                                                 AccessType accessType,
                                                 size_t maxLength) const {
    // Check length.  The cap is the caller's, not a property of the address
    // space: A_Memory_* encodes its count in 6 APCI bits and so tops out at 63
    // octets, while A_MemoryExtended_* carries a full octet and is capped at
    // 250 (§3.4.9).  Hard-coding the classic limit here made every extended
    // write above 63 octets answer E_LENGTH_EXCEEDS_MAX_APDU_LENGTH — which is
    // exactly the table-segment size ETS downloads, so no real project could
    // ever be loaded.
    if (length == 0 || length > maxLength) {
        return MemoryAccessResult::InvalidLength;
    }
    
    // Check if region exists
    const MemoryRegion* region = _addressSpace.findRegion(address);
    if (!region) {
        return MemoryAccessResult::RegionNotFound;
    }
    
    // Check access permissions
    auto accessRes = (accessType == AccessType::Write)
        ? _addressSpace.canWrite(address, length)
        : _addressSpace.canRead(address, length);
    if (accessRes.isError()) {
        return MemoryAccessResult::AccessDenied;
    }
    
    return MemoryAccessResult::Success;
}

void MemoryService::sendResponse(const IndividualAddress& dest, const MemoryResponse& response) {
    if (_responseCallback) {
        _responseCallback(dest, response);
    }
}

void MemoryService::sendExtendedResponse(const IndividualAddress& dest,
                                         const MemoryExtendedResponse& response) {
    if (_extendedResponseCallback) {
        _extendedResponseCallback(dest, response);
    }
}

MemoryExtendedReturnCode MemoryService::checkExtendedAccess(ExtendedMemoryAddress address,
                                                            uint8_t count,
                                                            AccessType accessType) const {
    if (count == 0 || count > kMaxExtendedMemoryBytes) {
        return MemoryExtendedReturnCode::ExceedsMaxApduLength;
    }

    // The device's memory regions are described in the 16-bit address space, so
    // anything above 0xFFFF cannot resolve to a region no matter what it is.
    if (!address.fitsInMemoryAddress()) {
        return MemoryExtendedReturnCode::AddressVoid;
    }

    // A range that starts inside the 16-bit space but runs past its end is not
    // representable either; catching it here keeps validateAccess from being
    // handed a wrapped length.
    if (static_cast<uint32_t>(address.raw) + count > 0x10000u) {
        return MemoryExtendedReturnCode::AddressVoid;
    }

    switch (validateAccess(address.narrow(), count, accessType, kMaxExtendedMemoryBytes)) {
        case MemoryAccessResult::Success:
            return MemoryExtendedReturnCode::Success;
        case MemoryAccessResult::AccessDenied: {
            // The spec separates "this location refuses that direction" from
            // "you are not authorised": Table 3 offers only E_ACCESS_WRITE_ONLY
            // and Table 4 only E_ACCESS_READ_ONLY, while E_ACCESS_DENIED is
            // reserved for A_Authorize / KNX Security refusals.
            const auto mode = _addressSpace.getAccessMode(address.narrow(), count);
            if (accessType == AccessType::Read && mode == MemoryAccessMode::WriteOnly) {
                return MemoryExtendedReturnCode::AccessWriteOnly;
            }
            if (accessType == AccessType::Write && mode == MemoryAccessMode::ReadOnly) {
                return MemoryExtendedReturnCode::AccessReadOnly;
            }
            return MemoryExtendedReturnCode::AccessDenied;
        }
        case MemoryAccessResult::InvalidLength:
            return MemoryExtendedReturnCode::ExceedsMaxApduLength;
        case MemoryAccessResult::InvalidAddress:
        case MemoryAccessResult::RegionNotFound:
            return MemoryExtendedReturnCode::AddressVoid;
    }

    return MemoryExtendedReturnCode::GenericError;
}

util::Result<void> MemoryService::handleExtendedReadRequest(const IndividualAddress& source,
                                                            uint8_t count,
                                                            ExtendedMemoryAddress address) {
    MemoryExtendedResponse response;
    response.kind = MemoryExtendedResponseKind::Read;
    response.address = address;

    response.returnCode = checkExtendedAccess(address, count, AccessType::Read);
    if (response.returnCode != MemoryExtendedReturnCode::Success) {
        KNX_LOGW(TAG, "Extended read denied at 0x%06X (%d bytes): rc=0x%02X",
                 static_cast<unsigned>(address.raw), count,
                 static_cast<unsigned>(response.returnCode));
        sendExtendedResponse(source, response);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    if (!_readCallback) {
        KNX_LOGE(TAG, "No read callback registered");
        response.returnCode = MemoryExtendedReturnCode::GenericError;
        sendExtendedResponse(source, response);
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }

    MemoryExtendedResponse::DataBuffer data;
    data.resize(count);
    auto readRes = _readCallback(address.narrow(), count, data.span());
    if (readRes.isError()) {
        KNX_LOGE(TAG, "Extended read callback failed at 0x%06X", static_cast<unsigned>(address.raw));
        // The region resolved but the storage could not be read.
        response.returnCode = MemoryExtendedReturnCode::MemoryError;
        sendExtendedResponse(source, response);
        return readRes.error();
    }

    response.data = data;
    sendExtendedResponse(source, response);

    KNX_LOGD(TAG, "Extended read %d bytes from 0x%06X", count, static_cast<unsigned>(address.raw));
    return util::Result<void>::ok();
}

util::Result<void> MemoryService::handleExtendedWriteRequest(const IndividualAddress& source,
                                                             uint8_t count,
                                                             ExtendedMemoryAddress address,
                                                             std::span<const uint8_t> data) {
    MemoryExtendedResponse response;
    response.kind = MemoryExtendedResponseKind::Write;
    response.address = address;

    response.returnCode = checkExtendedAccess(address, count, AccessType::Write);
    if (response.returnCode == MemoryExtendedReturnCode::Success && data.size() < count) {
        // Truncated APDU: the declared count outruns the octets actually sent.
        response.returnCode = MemoryExtendedReturnCode::ExceedsMaxApduLength;
    }

    if (response.returnCode != MemoryExtendedReturnCode::Success) {
        KNX_LOGW(TAG, "Extended write denied at 0x%06X (%d bytes): rc=0x%02X",
                 static_cast<unsigned>(address.raw), count,
                 static_cast<unsigned>(response.returnCode));
        sendExtendedResponse(source, response);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    if (!_writeCallback) {
        KNX_LOGE(TAG, "No write callback registered");
        response.returnCode = MemoryExtendedReturnCode::GenericError;
        sendExtendedResponse(source, response);
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }

    auto writeRes = _writeCallback(address.narrow(), data.subspan(0, count));
    if (writeRes.isError()) {
        KNX_LOGE(TAG, "Extended write callback failed at 0x%06X", static_cast<unsigned>(address.raw));
        // The region resolved but the storage could not be written.
        response.returnCode = MemoryExtendedReturnCode::MemoryError;
        sendExtendedResponse(source, response);
        return writeRes.error();
    }

    sendExtendedResponse(source, response);

    KNX_LOGD(TAG, "Extended wrote %d bytes to 0x%06X", count, static_cast<unsigned>(address.raw));
    return util::Result<void>::ok();
}

} // namespace application
} // namespace knx
