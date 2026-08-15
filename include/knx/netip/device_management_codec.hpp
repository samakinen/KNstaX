// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/application/property.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {
namespace device_management {

inline constexpr uint8_t kConnectionHeaderLength = 0x04;
inline constexpr uint8_t kPropertyReadRequestCode = 0xFC;
inline constexpr uint8_t kPropertyReadConfirmationCode = 0xFB;
inline constexpr uint8_t kPropertyWriteRequestCode = 0xF6;
inline constexpr uint8_t kPropertyWriteConfirmationCode = 0xF5;
inline constexpr size_t kPropertyHeaderSize = 8;

struct ConnectionHeader {
    ChannelId channelId{ChannelId::invalid()};
    uint8_t sequenceCounter{0};
};

struct PropertyAccessTarget {
    InterfaceObjectType objectType{};
    InterfaceObjectInstance objectInstance{1};
    application::PropertyID propertyId{application::PropertyID::ObjectType};
    uint8_t elementCount{1};
    uint16_t startIndex{1};
};

struct PropertyReadRequest {
    ConnectionHeader connection{};
    PropertyAccessTarget target{};
};

struct PropertyWriteRequest {
    ConnectionHeader connection{};
    PropertyAccessTarget target{};
    std::span<const uint8_t> data{};
};

struct PropertyReadConfirmationView {
    ConnectionHeader connection{};
    PropertyAccessTarget target{};
    std::span<const uint8_t> data{};
};

struct PropertyWriteConfirmation {
    ConnectionHeader connection{};
    PropertyAccessTarget target{};
};

util::Result<size_t> encodePropertyReadRequest(const PropertyReadRequest& request, std::span<uint8_t> out);
util::Result<size_t> encodePropertyWriteRequest(const PropertyWriteRequest& request, std::span<uint8_t> out);
util::Result<PropertyReadConfirmationView> decodePropertyReadConfirmation(std::span<const uint8_t> payload);
util::Result<PropertyWriteConfirmation> decodePropertyWriteConfirmation(std::span<const uint8_t> payload);

} // namespace device_management
} // namespace netip
} // namespace knx