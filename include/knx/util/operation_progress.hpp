// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/util/result.hpp"

#include <cstdint>

namespace knx {
namespace util {

enum class OperationProgressState : uint8_t {
    Pending = 0,
    Success,
    Busy,
    TransmissionFailed,
    Timeout,
    Complete = Success,
};

constexpr bool isPending(OperationProgressState state) noexcept
{
    return state == OperationProgressState::Pending;
}

constexpr bool isComplete(OperationProgressState state) noexcept
{
    return state == OperationProgressState::Success || state == OperationProgressState::Complete;
}

constexpr bool isTerminal(OperationProgressState state) noexcept
{
    return state != OperationProgressState::Pending;
}

constexpr OperationProgressState progressStateFromError(ErrorCode error) noexcept
{
    switch (error) {
        case ErrorCode::Busy:
            return OperationProgressState::Busy;
        case ErrorCode::Timeout:
            return OperationProgressState::Timeout;
        default:
            return OperationProgressState::TransmissionFailed;
    }
}

} // namespace util
} // namespace knx