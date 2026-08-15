// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

struct KnxNetIpHeader {
    NetIpServiceType serviceType{};
    uint16_t totalLength = 0;
};

class KnxNetIpCodec {
public:
    static constexpr uint8_t kHeaderLen = 0x06;
    static constexpr uint8_t kVersion = 0x10;

    static util::Result<void> decodeHeader(std::span<const uint8_t> data, KnxNetIpHeader& header);

    static util::Result<size_t> encodeHeader(NetIpServiceType serviceType,
                                             size_t payloadLength,
                                             std::span<uint8_t, kHeaderLen> outHeader);

    static util::Result<std::span<const uint8_t>> payloadSpan(std::span<const uint8_t> data,
                                                              NetIpServiceType expectedServiceType);
};

} // namespace netip
} // namespace knx