// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file espidf_gpio_register.hpp
 * @brief Direct GPIO register access that works across ESP32 SoC variants.
 *
 * The TP1 bitbang backend drives and samples GPIO from a timer ISR, where the
 * driver API is far too slow — a `gpio_set_level()` call costs more than the
 * timing margin of a 104 µs bit cell. So it writes `GPIO.out_w1ts` / `out_w1tc`
 * and reads `GPIO.in` directly.
 *
 * Those registers are not declared the same way on every SoC. The original
 * ESP32 (xtensa) declares them as plain `uint32_t`; the newer RISC-V parts
 * (C3, C6, H2, and S3) declare them as a union with a `.val` member. Code
 * written against one layout does not compile for the other, which is how this
 * backend ended up building only for C6-class targets.
 *
 * The accessors below pick the right form at compile time, so the ISR code
 * reads identically on every variant and stays a single register store.
 */

#pragma once

#include <cstdint>
#include <type_traits>

namespace knx {
namespace physical {
namespace espidf {

/// True when the SoC's register struct wraps the value in a `.val` member.
template <typename RegisterT>
concept HasValMember = requires(RegisterT reg) { reg.val; };

/// Store `value` into a GPIO register, whichever layout this SoC uses.
template <typename RegisterT>
[[gnu::always_inline]] inline void writeGpioRegister(RegisterT& reg, uint32_t value) noexcept
{
    if constexpr (HasValMember<std::remove_cvref_t<RegisterT>>) {
        reg.val = value;
    } else {
        reg = value;
    }
}

/// Load a GPIO register, whichever layout this SoC uses.
template <typename RegisterT>
[[gnu::always_inline]] inline uint32_t readGpioRegister(const RegisterT& reg) noexcept
{
    if constexpr (HasValMember<std::remove_cvref_t<RegisterT>>) {
        return static_cast<uint32_t>(reg.val);
    } else {
        return static_cast<uint32_t>(reg);
    }
}

} // namespace espidf
} // namespace physical
} // namespace knx
