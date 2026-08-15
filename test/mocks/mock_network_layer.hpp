// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file mock_network_layer.hpp
 * @brief Mock network layer for testing
 */

#pragma once

#include "knx/network/network_layer.hpp"
#include "mock_tp1_datalink.hpp"
#include <vector>

namespace knx {
namespace mocks {

/**
 * @brief Mock network layer for testing
 * 
 * Captures sent frames and allows simulating received frames
 */
class MockNetworkLayer : public network::NetworkLayer {
public:
    MockNetworkLayer() 
        : network::NetworkLayer(_mockDatalink)
        , _mockDatalink()
    {}
    
    virtual ~MockNetworkLayer() {}
    
    // Test helpers
    size_t getSentFrameCount() const {
        return _mockDatalink.getSentFrameCount();
    }
    
    network::NDataFrame getLastSentFrame() const {
        // Convert from datalink frame to network frame
        network::NDataFrame nFrame;
        if (_mockDatalink.getSentFrameCount() > 0) {
            nFrame.dlFrame = _mockDatalink.getLastSentFrame();
        }
        return nFrame;
    }
    
    void clearSentFrames() {
        _mockDatalink.clearSentFrames();
    }

private:
    MockTp1DataLinkLayer _mockDatalink;
};

} // namespace mocks
} // namespace knx
