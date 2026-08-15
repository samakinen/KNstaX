// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file enum_string.hpp
 * @brief Template-based enum to string conversion utilities
 * 
 * Provides type-safe string conversion for all KNX enumeration types.
 * This eliminates code duplication and provides consistent string
 * representations across the stack.
 */

#pragma once

#include <string_view>
#include "knx/util/result.hpp"

namespace knx {
namespace util {

/**
 * @brief Generic enum to string converter
 * 
 * Template specializations provide string conversion for each enum type.
 * Using string_view avoids allocations.
 * 
 * @tparam E Enum type
 * @param value Enum value to convert
 * @return String representation of the enum value
 */
template<typename E>
constexpr std::string_view enumToString(E value);

// ============================================================================
// ErrorCode Specialization
// ============================================================================

template<>
constexpr std::string_view enumToString<ErrorCode>(ErrorCode value) {
    switch (value) {
        case ErrorCode::Success: return "Success";
        case ErrorCode::InvalidParameter: return "InvalidParameter";
        case ErrorCode::InvalidAddress: return "InvalidAddress";
        case ErrorCode::InvalidFrameSize: return "InvalidFrameSize";
        case ErrorCode::BufferTooSmall: return "BufferTooSmall";
        case ErrorCode::ChecksumError: return "ChecksumError";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::QueueFull: return "QueueFull";
        case ErrorCode::NotInitialized: return "NotInitialized";
        case ErrorCode::AlreadyInitialized: return "AlreadyInitialized";
        case ErrorCode::TransmissionFailed: return "TransmissionFailed";
        case ErrorCode::DecodeFailed: return "DecodeFailed";
        case ErrorCode::EncodeFailed: return "EncodeFailed";
        case ErrorCode::ResourceUnavailable: return "ResourceUnavailable";
        case ErrorCode::OperationNotSupported: return "OperationNotSupported";
        case ErrorCode::UnknownError: return "UnknownError";
        default: return "Unknown";
    }
}

// ============================================================================
// PersistenceResult Specialization
// ============================================================================

template<>
constexpr std::string_view enumToString<PersistenceResult>(PersistenceResult value) {
    switch (value) {
        case PersistenceResult::Success: return "Success";
        case PersistenceResult::NotInitialized: return "NotInitialized";
        case PersistenceResult::ReadError: return "ReadError";
        case PersistenceResult::WriteError: return "WriteError";
        case PersistenceResult::InvalidData: return "InvalidData";
        case PersistenceResult::StorageFull: return "StorageFull";
        default: return "Unknown";
    }
}

// ============================================================================
// PropertyAccessResult Specialization
// ============================================================================

template<>
constexpr std::string_view enumToString<PropertyAccessResult>(PropertyAccessResult value) {
    switch (value) {
        case PropertyAccessResult::Success: return "Success";
        case PropertyAccessResult::InvalidObjectIndex: return "InvalidObjectIndex";
        case PropertyAccessResult::InvalidPropertyId: return "InvalidPropertyId";
        case PropertyAccessResult::InvalidIndex: return "InvalidIndex";
        case PropertyAccessResult::ReadOnly: return "ReadOnly";
        case PropertyAccessResult::TypeMismatch: return "TypeMismatch";
        case PropertyAccessResult::OutOfRange: return "OutOfRange";
        default: return "Unknown";
    }
}

// ============================================================================
// DeviceError Specialization
// ============================================================================

template<>
constexpr std::string_view enumToString<DeviceError>(DeviceError value) {
    switch (value) {
        case DeviceError::None: return "None";
        case DeviceError::InvalidState: return "InvalidState";
        case DeviceError::MemoryError: return "MemoryError";
        case DeviceError::ConfigurationError: return "ConfigurationError";
        case DeviceError::HardwareError: return "HardwareError";
        default: return "Unknown";
    }
}

// ============================================================================
// AuthorizationResult Specialization
// ============================================================================

template<>
constexpr std::string_view enumToString<AuthorizationResult>(AuthorizationResult value) {
    switch (value) {
        case AuthorizationResult::Success: return "Success";
        case AuthorizationResult::Unauthorized: return "Unauthorized";
        case AuthorizationResult::InvalidKey: return "InvalidKey";
        case AuthorizationResult::InsufficientLevel: return "InsufficientLevel";
        default: return "Unknown";
    }
}

// ============================================================================
// MemoryAccessResult Specialization
// ============================================================================

template<>
constexpr std::string_view enumToString<MemoryAccessResult>(MemoryAccessResult value) {
    switch (value) {
        case MemoryAccessResult::Success: return "Success";
        case MemoryAccessResult::InvalidAddress: return "InvalidAddress";
        case MemoryAccessResult::InvalidSize: return "InvalidSize";
        case MemoryAccessResult::ReadOnly: return "ReadOnly";
        case MemoryAccessResult::OutOfBounds: return "OutOfBounds";
        default: return "Unknown";
    }
}

// ============================================================================
// RestartResult Specialization
// ============================================================================

template<>
constexpr std::string_view enumToString<RestartResult>(RestartResult value) {
    switch (value) {
        case RestartResult::Success: return "Success";
        case RestartResult::NotSupported: return "NotSupported";
        case RestartResult::InvalidMode: return "InvalidMode";
        default: return "Unknown";
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Get string representation with fallback
 * 
 * Returns the enum string or a custom default if unknown.
 * 
 * @tparam E Enum type
 * @param value Enum value
 * @param defaultValue Default string if unknown
 * @return String representation
 */
template<typename E>
constexpr std::string_view enumToStringOr(E value, std::string_view defaultValue) {
    auto str = enumToString(value);
    return (str == "Unknown") ? defaultValue : str;
}

/**
 * @brief Check if enum value is valid (has known string)
 * 
 * @tparam E Enum type
 * @param value Enum value
 * @return true if value has known string representation
 */
template<typename E>
constexpr bool isValidEnum(E value) {
    return enumToString(value) != "Unknown";
}

} // namespace util
} // namespace knx
