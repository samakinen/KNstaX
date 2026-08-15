// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file mock_physical_layer.hpp
 * @brief Mock TP1 physical layer for testing
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/testing/mock_tp1_physical.hpp"

namespace knx {
namespace test {

using MockPhysicalLayer = testing::MockTp1Physical;

/// Returns a BAU-layer stack port for tests that construct bau::BusAccessUnit directly.
inline std::unique_ptr<bau::BusAccessStackPort> createTp1TestStackPort(
	platform::Platform& platform,
	std::unique_ptr<MockPhysicalLayer> physical)
{
	return bau::detail::createTp1MockTestStackPort(platform, std::move(physical));
}

} // namespace test
} // namespace knx
