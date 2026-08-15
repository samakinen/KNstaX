// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_ack_utils.hpp
 * @brief Shared helpers for raw TP1 ACK byte classification.
 */

#pragma once

#include "knx/physical/tp1_medium_backend.hpp"

namespace knx {
namespace physical {

inline constexpr Tp1AckClass tp1AckClassFromByte(uint8_t value) noexcept {
    switch (value) {
        case 0xCC:
            return Tp1AckClass::Ack;
        case 0x0C:
            return Tp1AckClass::Nack;
        case 0xC0:
            return Tp1AckClass::Busy;
        case 0x00:
            return Tp1AckClass::NackBusy;
        default:
            return Tp1AckClass::None;
    }
}

inline constexpr bool isTp1AckByte(uint8_t value) noexcept {
    return tp1AckClassFromByte(value) != Tp1AckClass::None;
}

inline constexpr uint8_t tp1AckByteFromClass(Tp1AckClass ackClass) noexcept {
    switch (ackClass) {
        case Tp1AckClass::Ack:
            return 0xCC;
        case Tp1AckClass::Nack:
            return 0x0C;
        case Tp1AckClass::Busy:
            return 0xC0;
        case Tp1AckClass::NackBusy:
            return 0x00;
        case Tp1AckClass::None:
        default:
            return 0;
    }
}

} // namespace physical
} // namespace knx