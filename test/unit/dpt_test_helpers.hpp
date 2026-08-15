// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "unity.h"
#include "knx/config.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

template <size_t Capacity = knx::config::MAX_APDU_LENGTH, typename EncoderFn>
std::vector<uint8_t> encodePayload(EncoderFn&& encoder)
{
    std::array<uint8_t, Capacity> buffer{};
    auto result = encoder(std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(result.isOk());
    return std::vector<uint8_t>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(result.value()));
}

template <size_t Capacity = knx::config::MAX_APDU_LENGTH, typename EncoderFn>
auto encodeResult(EncoderFn&& encoder)
{
    std::array<uint8_t, Capacity> buffer{};
    return encoder(std::span<uint8_t>(buffer));
}
