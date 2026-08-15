// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file mock_tp1_physical.hpp
 * @brief Simple mock TP1 physical layer for host examples and tests.
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/util/result.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace knx::testing {

class MockTp1Physical final {
public:
    using ProgressState = util::OperationProgressState;

    MockTp1Physical()
        : _open(false)
        , _txCount(0)
        , _state(physical::PhysicalLayerState::Idle)
        , _callback(nullptr)
        , _context(nullptr)
        , _asyncSendEnabled(false)
        , _nextAsyncSequence(1u)
        , _initError()
    {}

    util::Result<void> init()
    {
        if (_initError.has_value()) {
            _open = false;
            _state = physical::PhysicalLayerState::Error;
            return _initError.value();
        }
        _open = true;
        _state = physical::PhysicalLayerState::Idle;
        return util::Result<void>::ok();
    }

    void close()
    {
        _open = false;
        _state = physical::PhysicalLayerState::Idle;
    }

    bool isOpen() const
    {
        return _open;
    }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame)
    {
        if (!_open) {
            return util::ErrorCode::NotInitialized;
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_queuedSendResults.empty()) {
                const auto result = _queuedSendResults.front();
                _queuedSendResults.pop();
                if (result != util::ErrorCode::Success) {
                    return result;
                }
            }

            _sentFrames.emplace(frame.begin(), frame.end());
            ++_txCount;
        }

        _sentFrameCv.notify_one();
        return frame.size();
    }

    util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame)
    {
        auto sendResult = sendFrame(frame);
        if (sendResult.isError()) {
            return sendResult.error();
        }

        const uint32_t sequence = _nextAsyncSequence++;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_queuedAsyncOutcomes.empty()) {
                _queuedAsyncOutcomes.push(physical::TxOutcomeState::Success);
            }
            _activeAsyncOutcomeBySequence[sequence] = _queuedAsyncOutcomes.front();
            _queuedAsyncOutcomes.pop();
            _pendingAsyncPollCountBySequence[sequence] = _asyncSendEnabled ? 1u : 0u;
        }
        return sequence;
    }

    util::Result<physical::TxOutcomeState> pollTransmit(uint32_t sequence)
    {
        if (!_open) {
            return util::ErrorCode::NotInitialized;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        auto outcomeIt = _activeAsyncOutcomeBySequence.find(sequence);
        if (outcomeIt == _activeAsyncOutcomeBySequence.end()) {
            return util::ErrorCode::InvalidParameter;
        }

        auto pendingIt = _pendingAsyncPollCountBySequence.find(sequence);
        if (pendingIt != _pendingAsyncPollCountBySequence.end() && pendingIt->second > 0u) {
            --pendingIt->second;
            return physical::TxOutcomeState::Pending;
        }

        const auto outcome = outcomeIt->second;
        _activeAsyncOutcomeBySequence.erase(outcomeIt);
        _pendingAsyncPollCountBySequence.erase(sequence);
        return outcome;
    }

    util::Result<std::vector<uint8_t>> receiveFrame(uint32_t timeoutMs)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        if (_rxFrames.empty() && timeoutMs > 0) {
            _rxFrameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
                return !_rxFrames.empty();
            });
        }

        if (_rxFrames.empty()) {
            return util::ErrorCode::Timeout;
        }

        auto frame = std::move(_rxFrames.front());
        _rxFrames.pop();
        return frame;
    }

    util::Result<void> beginReceive(uint32_t timeoutMs)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _rxActive = true;
        _rxDeadlineMs = timeoutMs;
        return util::Result<void>::ok();
    }

    util::Result<ProgressState> pollReceive()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_rxActive) {
            return util::ErrorCode::OperationNotReady;
        }
        if (_rxFrames.empty()) {
            return util::ErrorCode::Timeout;
        }

        _lastReceivedFrame = std::move(_rxFrames.front());
        _rxFrames.pop();
        _rxActive = false;
        return ProgressState::Success;
    }

    util::Result<std::span<const uint8_t>> receivedFrameView()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_lastReceivedFrame.empty()) {
            return util::ErrorCode::OperationNotReady;
        }
        return std::span<const uint8_t>(_lastReceivedFrame);
    }

    void setReceiveCallback(physical::ReceiveCallback callback, void* context)
    {
        _callback = std::move(callback);
        _context = context;
    }

    physical::PhysicalLayerState getState() const
    {
        return _state;
    }

    util::Result<void> setBusMonitorMode(Toggle /*mode*/)
    {
        return util::Result<void>::ok();
    }

    size_t transmittedFrameCount() const
    {
        return _txCount;
    }

    void injectFrame(std::span<const uint8_t> frame)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _rxFrames.push(std::vector<uint8_t>(frame.begin(), frame.end()));
        }

        _rxFrameCv.notify_one();

        if (_callback) {
            _callback(_context);
        }
    }

    void injectRxFrame(std::span<const uint8_t> frame)
    {
        injectFrame(frame);
    }

    bool getSentFrame(std::vector<uint8_t>& frame, uint32_t timeoutMs = 100)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        if (_sentFrames.empty() && timeoutMs > 0) {
            _sentFrameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
                return !_sentFrames.empty();
            });
        }

        if (_sentFrames.empty()) {
            return false;
        }

        frame = std::move(_sentFrames.front());
        _sentFrames.pop();
        return true;
    }

    bool tryGetSentFrame(std::vector<uint8_t>& frame)
    {
        return getSentFrame(frame, 0);
    }

    size_t sentFrameCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _sentFrames.size();
    }

    bool popTxFrame(std::vector<uint8_t>& frame)
    {
        return tryGetSentFrame(frame);
    }

    void queueSendResult(util::ErrorCode result)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queuedSendResults.push(result);
    }

    void setInitError(util::ErrorCode error)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _initError = error;
    }

    void clearInitError()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _initError.reset();
    }

    void setAsyncSendEnabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _asyncSendEnabled = enabled;
    }

    void queueAsyncOutcome(physical::TxOutcomeState outcome)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _queuedAsyncOutcomes.push(outcome);
    }

private:
    bool _open{false};
    size_t _txCount{0};
    physical::PhysicalLayerState _state{physical::PhysicalLayerState::Idle};
    physical::ReceiveCallback _callback{};
    void* _context{nullptr};
    mutable std::mutex _mutex;
    std::condition_variable _sentFrameCv;
    std::condition_variable _rxFrameCv;
    std::queue<std::vector<uint8_t>> _sentFrames;
    std::queue<std::vector<uint8_t>> _rxFrames;
    std::vector<uint8_t> _lastReceivedFrame;
    std::queue<util::ErrorCode> _queuedSendResults;
    bool _asyncSendEnabled{false};
    uint32_t _nextAsyncSequence{1u};
    bool _rxActive{false};
    uint32_t _rxDeadlineMs{0};
    std::optional<util::ErrorCode> _initError;
    std::queue<physical::TxOutcomeState> _queuedAsyncOutcomes;
    std::unordered_map<uint32_t, physical::TxOutcomeState> _activeAsyncOutcomeBySequence;
    std::unordered_map<uint32_t, uint8_t> _pendingAsyncPollCountBySequence;
};

} // namespace knx::testing