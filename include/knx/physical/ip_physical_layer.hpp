// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file ip_physical_layer.hpp
 * @brief KNXnet/IP physical layer interface
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <span>

#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"

namespace knx {
namespace physical {

/**
 * @brief IP physical layer state
 */
enum class IpPhysicalLayerState {
    Idle,
    Transmitting,
    Receiving,
    Error,
};

/**
 * @brief IP physical layer callback
 */
using IpReceiveCallback = util::InplaceFunction<void(void*), 32>;

/**
 * @brief IP (KNXnet/IP) physical layer interface
 *
 * Implements KNX over UDP/IP using KNXnet/IP protocol.
 * Typically used on systems with WiFi or Ethernet connectivity.
 *
 * Frame structure: Standard KNXnet/IP UDP packet containing L-Data telegram.
 */
class IpPhysical {
public:
    using ProgressState = util::OperationProgressState;

    IpPhysical();
    ~IpPhysical();

    IpPhysical(const IpPhysical&) = delete;
    IpPhysical& operator=(const IpPhysical&) = delete;
    IpPhysical(IpPhysical&&) = delete;
    IpPhysical& operator=(IpPhysical&&) = delete;

    // Must be called before init(). Required for cross-platform builds.
    void setNetworkInterface(platform::NetworkInterface* network);
    void setTimingPlatform(platform::TimingPlatform* timingPlatform);

    /**
     * @brief Initialize the IP physical layer
     * @return success or error code
     */
    util::Result<void> init();

    /**
     * @brief Close the layer and release resources
     */
    void close();

    /**
     * @brief Check if layer is open and ready
     */
    bool isOpen() const;

    /**
     * @brief Send frame via UDP
     * @param frame Frame data (KNXnet/IP format)
     * @param remoteIp Target IP address
     * @param remotePort Target UDP port (typically 3671)
     * @return Bytes transmitted or error code
     */
    util::Result<size_t> sendFrame(std::span<const uint8_t> frame,
                                   IpAddress remoteIp,
                                   uint16_t remotePort);

    util::Result<void> beginTransmit(std::span<const uint8_t> frame,
                                     IpAddress remoteIp,
                                     uint16_t remotePort);

    util::Result<ProgressState> pollTransmit();

    /**
     * @brief Receive frame from UDP
     * @param timeoutMs Receive timeout in milliseconds
     * @return Result containing the received frame bytes or an error
     */
    util::Result<void> beginReceive(uint32_t timeoutMs);

    util::Result<ProgressState> pollReceive();

    util::Result<std::span<const uint8_t>> receivedFrameView();

    /**
     * @brief Set RX callback (called when frame arrives)
     * @param callback Function to invoke
     * @param context User context passed to callback
     */
    void setReceiveCallback(IpReceiveCallback callback, void* context);

    /**
     * @brief Get current state
     */
    IpPhysicalLayerState getState() const;

    /**
    * @brief Configure bind address and port for UDP socket.
    * Must be called before init(). Defaults: address 0.0.0.0, port 3671.
     */
    void setBindAddressPort(IpAddress address, uint16_t port);

    /**
    * @brief Configure multicast interface for KNX group (224.0.23.12).
     * Optional; interface IPv4 address used for IP_ADD_MEMBERSHIP.
     * Must be set before init() to take effect.
     */
    void setMulticastInterface(IpAddress interfaceAddress);

private:
    static constexpr size_t MAX_RX_FRAME_SIZE = 2048;

    platform::NetworkInterface* _network{nullptr};
    std::unique_ptr<platform::UdpSocket> _sock;
    bool _initialized{false};
    IpPhysicalLayerState _state{IpPhysicalLayerState::Idle};
    IpReceiveCallback _rxCallback{};
    void* _rxCallbackContext{nullptr};
    uint16_t _bindPort{0};
    IpAddress _bindAddress{IpAddress(0)};
    IpAddress _mcastInterface{IpAddress(0)};
    bool _txActive{false};
    bool _txStarted{false};
    std::vector<uint8_t> _txFrame{};
    IpAddress _txRemoteIp{IpAddress(0)};
    uint16_t _txRemotePort{0};
    bool _rxActive{false};
    uint64_t _rxDeadlineMs{0};
    platform::TimingPlatform* _timingPlatform{nullptr};
    std::array<uint8_t, MAX_RX_FRAME_SIZE> _rxFrame{};
    size_t _rxFrameLength{0};
};

} // namespace physical
} // namespace knx
