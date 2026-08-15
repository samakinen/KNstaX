// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <span>

#pragma once

#include "knx/datalink/frame_codec.hpp"
#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#include "knx/physical/virtual_tp1_bus_peer.hpp"
#include "knx/physical/virtual_tp1_test_runtime.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <vector>

namespace knx {
namespace testsupport {

class VirtualTp1SentFrameSource {
public:
    virtual ~VirtualTp1SentFrameSource() = default;

    virtual size_t sentFrameCount() const = 0;
    virtual std::vector<uint8_t> sentFrame(size_t index) const = 0;
    virtual const physical::BitBangConfig& sentFrameConfig() const = 0;
};

enum class VirtualTp1PeerResponse : uint8_t {
    Ack,
    Nack,
    Busy,
    NackBusy,
    Silent,
};

class VirtualTp1TestScheduler {
public:
    VirtualTp1TestScheduler(physical::VirtualTp1TestRuntime& runtime,
                            physical::VirtualTp1BusPeer& busPeer,
                            const VirtualTp1SentFrameSource& sentFrameSource)
        : _runtime(runtime)
        , _busPeer(busPeer)
        , _sentFrameSource(sentFrameSource) {}

    bool waitUntilSentFrameCount(size_t expectedCount,
                                 uint32_t maxAdvanceMs = 2000u,
                                 uint32_t stepUs = 200u) const {
        if (_sentFrameSource.sentFrameCount() >= expectedCount) {
            return true;
        }

        const uint64_t maxAdvanceUs = static_cast<uint64_t>(maxAdvanceMs) * 1000u;
        const uint64_t effectiveStepUs = (stepUs == 0u) ? 1u : static_cast<uint64_t>(stepUs);
        const uint32_t maxSteps = static_cast<uint32_t>((maxAdvanceUs + effectiveStepUs - 1u) / effectiveStepUs);

        for (uint32_t step = 0u; step < maxSteps; ++step) {
            _runtime.advanceTimeUs(effectiveStepUs);
            if (_sentFrameSource.sentFrameCount() >= expectedCount) {
                return true;
            }
        }

        return _sentFrameSource.sentFrameCount() >= expectedCount;
    }

    bool injectRawFrame(std::span<const uint8_t> raw, uint64_t startUs = 20u) {
        _busPeer.clearScript();

        const uint64_t baseUs = _runtime.nowUs() + startUs;
        const uint64_t byteSpacingUs = static_cast<uint64_t>(_sentFrameSource.sentFrameConfig().serialBitTimeUs) * 13u;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (!_busPeer.addByteWaveformAtUs(baseUs + (byteSpacingUs * i),
                                              raw[i],
                                              _sentFrameSource.sentFrameConfig(),
                                              false)) {
                return false;
            }
        }
        if (!_busPeer.injectScript()) {
            return false;
        }

        _runtime.advanceTimeUs(startUs + (byteSpacingUs * raw.size())
                               + (static_cast<uint64_t>(_sentFrameSource.sentFrameConfig().serialBitTimeUs) * 16u));
        return true;
    }

    bool injectLDataFrame(const datalink::LDataFrame& frame, uint64_t startUs = 20u) {
        uint8_t buffer[23]{};
        auto encoded = datalink::FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
        if (encoded.isError()) {
            return false;
        }

        return injectRawFrame(std::vector<uint8_t>(buffer, buffer + encoded.value()), startUs);
    }

    bool advanceUntilSentFrameCount(size_t expectedCount, uint32_t maxAdvanceMs = 100u) {
        for (uint32_t i = 0u; i < maxAdvanceMs && _sentFrameSource.sentFrameCount() < expectedCount; ++i) {
            _runtime.advanceTimeMs(1u);
        }
        return _sentFrameSource.sentFrameCount() >= expectedCount;
    }

    bool advanceUntilLocalTxQuiescent(uint32_t maxAdvanceMs = 100u,
                                      uint32_t stepUs = 200u,
                                      uint32_t stableStepsRequired = 10u) {
        const size_t initialTransitionCount = _runtime.bus().capturedTxTransitions().size();
        size_t lastTransitionCount = initialTransitionCount;
        // A queued frame starts transmitting only after the KNX t_idle window
        // (50/53 bit times of bus idle), so initial silence is NOT quiescence:
        // require the TX to have produced edges before counting stability.
        bool txObserved = false;
        uint32_t stableSteps = 0u;
        const uint32_t maxSteps = (maxAdvanceMs * 1000u + stepUs - 1u) / stepUs;

        for (uint32_t step = 0u; step < maxSteps; ++step) {
            _runtime.advanceTimeUs(stepUs);
            const size_t currentTransitionCount = _runtime.bus().capturedTxTransitions().size();
            if (currentTransitionCount != lastTransitionCount) {
                lastTransitionCount = currentTransitionCount;
                txObserved = true;
                stableSteps = 0u;
                continue;
            }

            if (!txObserved) {
                continue;
            }

            ++stableSteps;
            if (stableSteps >= stableStepsRequired) {
                return true;
            }
        }

        return false;
    }

    bool driveTxOutcomeForSentFrame(size_t frameIndex,
                                    VirtualTp1PeerResponse response,
                                    uint32_t silenceAdvanceMs = 25u) {
        if (response == VirtualTp1PeerResponse::Silent) {
            _runtime.advanceTimeMs(silenceAdvanceMs);
            return true;
        }

        const auto ackByte = toAckByte(response);
        if (!ackByte.has_value()) {
            return false;
        }

        _busPeer.clearScript();
        (void)_sentFrameSource.sentFrame(frameIndex);
        if (!advanceUntilLocalTxQuiescent()) {
            return false;
        }

        const uint64_t ackStartUs = _runtime.nowUs();
        if (!_busPeer.addByteWaveformAtUs(ackStartUs,
                                          ackByte.value(),
                                          _sentFrameSource.sentFrameConfig(),
                                          false)) {
            return false;
        }
        if (!_busPeer.injectScript()) {
            return false;
        }

        _runtime.advanceTimeUs(static_cast<uint64_t>(_sentFrameSource.sentFrameConfig().serialBitTimeUs) * 16u);
        return true;
    }

    static std::optional<uint8_t> toAckByte(VirtualTp1PeerResponse response) {
        switch (response) {
            case VirtualTp1PeerResponse::Ack:
                return physical::BitBangDriverTimerIsr::ACK_BYTE_ACK;
            case VirtualTp1PeerResponse::Nack:
                return physical::BitBangDriverTimerIsr::ACK_BYTE_NACK;
            case VirtualTp1PeerResponse::Busy:
                return physical::BitBangDriverTimerIsr::ACK_BYTE_BUSY;
            case VirtualTp1PeerResponse::NackBusy:
                return physical::BitBangDriverTimerIsr::ACK_BYTE_NACK_BUSY;
            case VirtualTp1PeerResponse::Silent:
            default:
                return std::nullopt;
        }
    }

private:
    physical::VirtualTp1TestRuntime& _runtime;
    physical::VirtualTp1BusPeer& _busPeer;
    const VirtualTp1SentFrameSource& _sentFrameSource;
};

} // namespace testsupport
} // namespace knx
