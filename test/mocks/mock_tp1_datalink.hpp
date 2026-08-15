// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file mock_tp1_datalink.hpp
 * @brief Mock TP1 data link layer for testing
 */

#pragma once

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/platform/linux_platform.hpp"
#include "mock_physical_layer.hpp"
#include <vector>

namespace knx {
namespace mocks {

class MockTp1DataLinkLayer : public datalink::Tp1DataLinkLayer {
public:
    MockTp1DataLinkLayer()
        : datalink::Tp1DataLinkLayer(testPlatform(), _physical)
        , _open(true)
        , _rxCallback(nullptr)
    {}

    ~MockTp1DataLinkLayer() override = default;

    util::Result<void> init(const IndividualAddress& ownAddress) override {
        (void)ownAddress;
        _open = true;
        return util::Result<void>::ok();
    }

    void close() override {
        _open = false;
    }

    bool isOpen() const override {
        return _open;
    }

    util::Result<void> sendFrame(const datalink::LDataFrame& frame) override {
        _sentFrames.push_back(frame);
        return util::Result<void>::ok();
    }

    util::Result<void> beginTransmit(const datalink::LDataFrame& frame) override {
        if (!_open) {
            return util::ErrorCode::NotInitialized;
        }
        if (_txActive) {
            return util::ErrorCode::Busy;
        }
        _pendingFrame = frame;
        _txActive = true;
        _txStarted = false;
        return util::Result<void>::ok();
    }

    util::Result<datalink::Tp1DataLinkLayer::TxProgressState> pollTransmit() override {
        if (!_open) {
            return util::ErrorCode::NotInitialized;
        }
        if (!_txActive) {
            return util::ErrorCode::OperationNotReady;
        }

        if (!_txStarted) {
            _sentFrames.push_back(_pendingFrame);
            _txStarted = true;
            _txActive = false;
            return datalink::Tp1DataLinkLayer::TxProgressState::Success;
        }

        _txActive = false;
        return datalink::Tp1DataLinkLayer::TxProgressState::Success;
    }

    void setReceiveCallback(datalink::LDataCallback callback) override {
        _rxCallback = std::move(callback);
    }

    // Test helpers
    size_t getSentFrameCount() const {
        return _sentFrames.size();
    }

    datalink::LDataFrame getLastSentFrame() const {
        if (_sentFrames.empty()) {
            return {};
        }
        return _sentFrames.back();
    }

    void clearSentFrames() {
        _sentFrames.clear();
    }

    void injectRxFrame(const datalink::LDataFrame& frame) {
        if (_rxCallback) {
            _rxCallback(frame);
        }
    }

private:
    static knx::platform::LinuxPlatform& testPlatform() {
        static knx::platform::LinuxPlatform platform;
        return platform;
    }

    knx::test::MockPhysicalLayer _physical;
    bool _open;
    datalink::LDataCallback _rxCallback;
    std::vector<datalink::LDataFrame> _sentFrames;
    datalink::LDataFrame _pendingFrame;
    bool _txActive{false};
    bool _txStarted{false};
};

} // namespace mocks
} // namespace knx
