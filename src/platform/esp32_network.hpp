// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_network.hpp
 * @brief ESP32 network implementation
 */

#pragma once

#include "knx/platform/network_interface.hpp"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include <memory>
#include <vector>

namespace knx {
namespace platform {

class Esp32UdpSocket : public UdpSocket {
public:
    Esp32UdpSocket();
    virtual ~Esp32UdpSocket();
    
    knx::util::Result<void> open(uint16_t port = 0) override;
    void close() override;
    bool isOpen() const override;
    
    knx::util::Result<void> joinMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) override;
    void leaveMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) override;

    knx::util::Result<void> setMulticastInterface(IpAddress interfaceAddr) override;
    knx::util::Result<void> setMulticastLoopback(MulticastLoopbackMode mode) override;
    knx::util::Result<void> setMulticastTtl(uint8_t ttl) override;
    
    int send(IpAddress destAddr, uint16_t destPort, 
             std::span<const uint8_t> buffer) override;
    int receive(std::span<uint8_t> buffer) override;
    int receive(std::span<uint8_t> buffer,
               IpAddress& srcAddr, uint16_t& srcPort) override;
    
    size_t available() const override;
    uint16_t localPort() const override;
    
private:
    int _socket;
    uint16_t _port;
    std::vector<IpAddress> _multicastAddresses;
};

class Esp32Network : public NetworkInterface {
public:
    Esp32Network();
    virtual ~Esp32Network();
    
    knx::util::Result<void> init() override;
    bool isConnected() const override;
    
    IpAddress ipAddress() const override;
    IpAddress subnetMask() const override;
    IpAddress gateway() const override;
    void macAddress(std::span<uint8_t> mac) const override;
    
    std::unique_ptr<UdpSocket> createUdpSocket() override;
    
private:
    esp_netif_t* _netif;
    bool _initialized;
    
    static void wifiEventHandler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data);
};

} // namespace platform
} // namespace knx
