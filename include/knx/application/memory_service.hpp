// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file memory_service.hpp
 * @brief KNX Memory Service (A_Memory_Read/Write)
 * 
 * Implements memory access services per KNX spec 3/5/1 section 2.3.
 * Provides read and write access to device memory with validation.
 * 
 * Note: Uses structures from apci_services.hpp
 */

#pragma once

#include "knx/application/address_space.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/config.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <functional>
#include <span>

namespace knx {
namespace application {

/**
 * @brief Memory Service Handler
 * 
 * Handles A_Memory_Read and A_Memory_Write services with address
 * space validation and callback-based memory access.
 */
class MemoryService {
public:
    /// Maximum bytes per memory operation
   static constexpr uint8_t MAX_MEMORY_BYTES = static_cast<uint8_t>(kMaxMemoryServiceDataBytes);
    
    /**
     * @brief Memory read callback
     * @param address Starting address
     * @param length Number of bytes to read
     * @param data Output buffer
     * @return true if read successful
     */
   using ReadCallback = std::function<util::Result<void>(MemoryAddress address, uint8_t length, std::span<uint8_t> data)>;
    
    /**
     * @brief Memory write callback
     * @param address Starting address
     * @param data Data to write
     * @return true if write successful
     */
   using WriteCallback = std::function<util::Result<void>(MemoryAddress address, std::span<const uint8_t> data)>;
    
    /**
     * @brief Memory response callback
     * @param dest Destination address
     * @param response Response data
     */
    using ResponseCallback = std::function<void(const IndividualAddress& dest, const MemoryResponse& response)>;

    /**
     * @brief Extended memory response callback
     *
     * Separate from ResponseCallback because the extended services answer with
     * a return code and a 24-bit address, and must answer even on failure.
     */
    using ExtendedResponseCallback =
        std::function<void(const IndividualAddress& dest, const MemoryExtendedResponse& response)>;

    /**
     * @brief Whether Verify Mode is on (PID_DEVICE_CONTROL bit 2, 03/05/01 §4.2.14.7)
     *
     * Decides whether A_Memory_Write is answered at all (03/03/07 §3.5.4).
     * Left unset, Verify Mode is off, which is its default in the spec.
     */
    using VerifyModeProvider = std::function<bool()>;

    /**
     * @brief Whether @p source holds the access level memory writes require
     *
     * Asked for A_Memory_Write and A_MemoryExtended_Write alike. Left unset,
     * every write is permitted.
     */
    using WritePermission = std::function<bool(const IndividualAddress& source)>;

    /// Largest payload an extended memory operation may carry.
    ///
    /// 03/03/07 §3.4.9.1 and §3.4.9.2 both cap the services at 250 octets.  The
    /// response also has to fit the APDU budget once the two APCI octets, the
    /// return code and the 3-octet address are accounted for, so take whichever
    /// bound bites first.
    static constexpr size_t kMaxExtendedMemoryBytes =
        (config::MAX_APDU_LENGTH - 6u) < 250u ? (config::MAX_APDU_LENGTH - 6u) : 250u;


    /**
     * @brief Initialize memory service with address space
     * @param addressSpace Address space manager for validation
     */
    explicit MemoryService(AddressSpace& addressSpace);
    
    /**
     * @brief Set memory read callback
     */
    void setReadCallback(ReadCallback callback) { _readCallback = callback; }
    
    /**
     * @brief Set memory write callback
     */
    void setWriteCallback(WriteCallback callback) { _writeCallback = callback; }
    
    /**
     * @brief Set response callback
     */
    void setResponseCallback(ResponseCallback callback) { _responseCallback = callback; }

    /**
     * @brief Set extended memory response callback
     */
    void setExtendedResponseCallback(ExtendedResponseCallback callback) {
        _extendedResponseCallback = std::move(callback);
    }

    void setVerifyModeProvider(VerifyModeProvider provider) { _verifyModeProvider = std::move(provider); }
    void setWritePermission(WritePermission permission) { _writePermission = std::move(permission); }

    /**
     * @brief Handle A_MemoryExtended_Read request
     *
     * Unlike handleReadRequest, this always answers: the extended services are
     * confirmed, and ETS blocks waiting for a response even when the access is
     * refused.  Failures come back as a non-zero return code, not as silence.
     *
     * @param source Source address
     * @param count Number of octets to read
     * @param address 24-bit memory address
     * @return Result<void> indicating whether the read itself succeeded
     */
    util::Result<void> handleExtendedReadRequest(const IndividualAddress& source,
                                                 uint8_t count,
                                                 ExtendedMemoryAddress address);

    /**
     * @brief Handle A_MemoryExtended_Write request
     *
     * Always answers, for the same reason as handleExtendedReadRequest.
     *
     * @param source Source address
     * @param count Number of octets to write
     * @param address 24-bit memory address
     * @param data Write data
     * @return Result<void> indicating whether the write itself succeeded
     */
    util::Result<void> handleExtendedWriteRequest(const IndividualAddress& source,
                                                  uint8_t count,
                                                  ExtendedMemoryAddress address,
                                                  std::span<const uint8_t> data);


    /**
     * @brief Handle A_Memory_Read request
     * @param source Source address
     * @param count Number of bytes to read (1-63)
     * @param address Memory address (16-bit in current API)
    * @return Result<void> indicating success or error
     */
   util::Result<void> handleReadRequest(const IndividualAddress& source, uint8_t count, MemoryAddress address);
    
    /**
     * @brief Handle A_Memory_Write request
     * @param source Source address
     * @param count Number of bytes to write
     * @param address Memory address (16-bit in current API)
     * @param data Write data
    * @return Result<void> indicating success or error
     */
   util::Result<void> handleWriteRequest(const IndividualAddress& source, uint8_t count, 
                     MemoryAddress address, std::span<const uint8_t> data);
    
    /**
     * @brief Validate memory access
     * @param address Starting address
     * @param length Number of bytes
    * @param accessType Requested access type
     * @return ok; InvalidFrameSize for a length of 0 or above @p maxLength;
     *         InvalidAddress when no region holds @p address; WriteOnly /
     *         ReadOnly when the region refuses that direction, AccessDenied
     *         when it refuses the range otherwise.
     */
   /// @param maxLength Largest payload the *calling service* may carry. The
   ///                  classic A_Memory_* services top out at 63 octets (6-bit
   ///                  APCI count); A_MemoryExtended_* carries a full octet.
   util::Result<void> validateAccess(MemoryAddress address,
                                     uint8_t length,
                                     AccessType accessType,
                                     size_t maxLength = MAX_MEMORY_BYTES) const;
    
private:
    AddressSpace& _addressSpace;
    ReadCallback _readCallback;
    WriteCallback _writeCallback;
    ResponseCallback _responseCallback;
    ExtendedResponseCallback _extendedResponseCallback;
    VerifyModeProvider _verifyModeProvider;
    WritePermission _writePermission;

    bool verifyModeActive() const { return _verifyModeProvider && _verifyModeProvider(); }
    bool writePermitted(const IndividualAddress& source) const {
        return !_writePermission || _writePermission(source);
    }
    /// A_Memory_Response with number = 0 and no data: the "there was a
    /// problem" answer of 03/03/07 §3.5.3 and §3.5.4.
    void sendNegativeResponse(const IndividualAddress& dest, MemoryAddress address);

    void sendResponse(const IndividualAddress& dest, const MemoryResponse& response);
    void sendExtendedResponse(const IndividualAddress& dest, const MemoryExtendedResponse& response);

    /// Shared precondition check for both extended services.  Returns Success
    /// when the access may proceed against the narrowed 16-bit address.
    MemoryExtendedReturnCode checkExtendedAccess(ExtendedMemoryAddress address,
                                                 uint8_t count,
                                                 AccessType accessType) const;
};

} // namespace application
} // namespace knx
