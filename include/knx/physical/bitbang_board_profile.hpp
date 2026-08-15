// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bitbang_board_profile.hpp
 * @brief Embedded board-profile defaults for TP1 bitbang backends
 */

#pragma once

#include "knx/physical/bitbang_driver_interface.hpp"

namespace knx {
namespace physical {

struct BitBangBoardProfile {
    uint8_t txPin{0xFF};
    uint8_t rxPin{0xFF};
    bool enablePullup{true};
    bool txDominantHigh{true};
    bool rxDominantHigh{true};
    uint32_t serialBitTimeUs{104};
    uint32_t zeroActiveTimeUs{35};
    uint32_t zeroEqualizationTimeUs{69};
    // Mid-bit-cell: bits are decoded from an edge-set sticky flag, so this only
    // has to sit between this cell's edge and the next one. 52 µs of a 104 µs
    // cell splits the late/early alarm tolerance evenly. See Kconfig.
    uint32_t bitSamplingOffsetUs{52};
    uint32_t startBitValidationTimeUs{15};
    LinkSignalConfig linkSignal{};
};

inline BitBangBoardProfile defaultBitBangBoardProfile() {
    BitBangBoardProfile profile{};

#if defined(ESP_PLATFORM)
#if defined(CONFIG_KNX_TP1_BITBANG_TX_PIN)
    profile.txPin = static_cast<uint8_t>(CONFIG_KNX_TP1_BITBANG_TX_PIN);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_RX_PIN)
    profile.rxPin = static_cast<uint8_t>(CONFIG_KNX_TP1_BITBANG_RX_PIN);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_RX_PULLUP)
    profile.enablePullup = CONFIG_KNX_TP1_BITBANG_RX_PULLUP;
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_TX_DOMINANT_HIGH)
    profile.txDominantHigh = CONFIG_KNX_TP1_BITBANG_TX_DOMINANT_HIGH;
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_RX_DOMINANT_HIGH)
    profile.rxDominantHigh = CONFIG_KNX_TP1_BITBANG_RX_DOMINANT_HIGH;
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_SERIAL_BIT_TIME_US)
    profile.serialBitTimeUs = static_cast<uint32_t>(CONFIG_KNX_TP1_BITBANG_SERIAL_BIT_TIME_US);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_ZERO_ACTIVE_US)
    profile.zeroActiveTimeUs = static_cast<uint32_t>(CONFIG_KNX_TP1_BITBANG_ZERO_ACTIVE_US);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_ZERO_EQUALIZATION_US)
    profile.zeroEqualizationTimeUs = static_cast<uint32_t>(CONFIG_KNX_TP1_BITBANG_ZERO_EQUALIZATION_US);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_SAMPLE_OFFSET_US)
    profile.bitSamplingOffsetUs = static_cast<uint32_t>(CONFIG_KNX_TP1_BITBANG_SAMPLE_OFFSET_US);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_START_BIT_VALIDATION_US)
    profile.startBitValidationTimeUs = static_cast<uint32_t>(CONFIG_KNX_TP1_BITBANG_START_BIT_VALIDATION_US);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_LINK_PIN)
    profile.linkSignal.pin = static_cast<uint8_t>(CONFIG_KNX_TP1_BITBANG_LINK_PIN);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_LINK_ACTIVE_HIGH)
    profile.linkSignal.activeHigh = CONFIG_KNX_TP1_BITBANG_LINK_ACTIVE_HIGH;
#else
    profile.linkSignal.activeHigh = false;
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_LINK_PULLUP)
    profile.linkSignal.enablePullup = CONFIG_KNX_TP1_BITBANG_LINK_PULLUP;
#else
    profile.linkSignal.enablePullup = false;
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_LINK_DOWN_DEBOUNCE_US)
    profile.linkSignal.downDebounceUs = static_cast<uint32_t>(CONFIG_KNX_TP1_BITBANG_LINK_DOWN_DEBOUNCE_US);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_LINK_UP_DEBOUNCE_US)
    profile.linkSignal.upDebounceUs = static_cast<uint32_t>(CONFIG_KNX_TP1_BITBANG_LINK_UP_DEBOUNCE_US);
#endif
#if defined(CONFIG_KNX_TP1_BITBANG_LINK_POWERFAIL_ISR)
    profile.linkSignal.notifyPowerFailFromIsr = CONFIG_KNX_TP1_BITBANG_LINK_POWERFAIL_ISR;
#else
    profile.linkSignal.notifyPowerFailFromIsr = false;
#endif
#endif

    return profile;
}

inline void applyBitBangBoardProfile(const BitBangBoardProfile& profile, BitBangConfig& config) {
    if (config.txPin == 0xFF) {
        config.txPin = profile.txPin;
    }
    if (config.rxPin == 0xFF) {
        config.rxPin = profile.rxPin;
    }

    config.enablePullup = profile.enablePullup;
    config.txDominantHigh = profile.txDominantHigh;
    config.rxDominantHigh = profile.rxDominantHigh;
    config.serialBitTimeUs = profile.serialBitTimeUs;
    config.zeroActiveTimeUs = profile.zeroActiveTimeUs;
    config.zeroEqualizationTimeUs = profile.zeroEqualizationTimeUs;
    config.bitSamplingOffsetUs = profile.bitSamplingOffsetUs;
    config.startBitValidationTimeUs = profile.startBitValidationTimeUs;

    if (!config.linkSignal.isConfigured()) {
        config.linkSignal = profile.linkSignal;
    }
}

inline void applyDefaultBitBangBoardProfile(BitBangConfig& config) {
    applyBitBangBoardProfile(defaultBitBangBoardProfile(), config);
}

} // namespace physical
} // namespace knx