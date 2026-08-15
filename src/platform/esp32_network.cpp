// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_network.cpp
 * @brief ESP32 network implementation
 */

#include "esp32_network.hpp"
#include "knx/util/log.hpp"
#include "esp_log.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <cstring>

static const char* TAG = "KNX.Network";

namespace knx {
namespace platform {

// ===========================================================================
// Esp32UdpSocket
// ===========================================================================

Esp32UdpSocket::Esp32UdpSocket()
    : _socket(-1)
    , _port(0)
{
}

Esp32UdpSocket::~Esp32UdpSocket() {
    close();
}

knx::util::Result<void> Esp32UdpSocket::open(uint16_t port) {
    if (_socket >= 0) {
        KNX_LOGW(TAG, "Socket already open");
        return knx::util::Result<void>::ok();
    }
    
    _socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_socket < 0) {
        KNX_LOGE(TAG, "Failed to create socket: %d", errno);
        return knx::util::ErrorCode::ResourceUnavailable;
    }
    
    // Set socket options
    int reuse = 1;
    if (setsockopt(_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        KNX_LOGW(TAG, "Failed to set SO_REUSEADDR: %d", errno);
    }
    
    // Bind to port
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        KNX_LOGE(TAG, "Failed to bind socket to port %u: %d", port, errno);
        ::close(_socket);
        _socket = -1;
        return knx::util::ErrorCode::OperationFailed;
    }
    
    _port = port;
    KNX_LOGI(TAG, "UDP socket opened on port %u", _port);
    
    return knx::util::Result<void>::ok();
}

void Esp32UdpSocket::close() {
    if (_socket >= 0) {
        // Leave all multicast groups
        for (const auto& addr : _multicastAddresses) {
            leaveMulticast(addr, IpAddress(0));
        }
        
        ::close(_socket);
        _socket = -1;
        _port = 0;
        KNX_LOGI(TAG, "UDP socket closed");
    }
}

bool Esp32UdpSocket::isOpen() const {
    return _socket >= 0;
}

knx::util::Result<void> Esp32UdpSocket::joinMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) {
    if (_socket < 0) {
        return knx::util::ErrorCode::NotInitialized;
    }
    
    struct ip_mreq mreq;
    std::memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = multicastAddr.raw;
    mreq.imr_interface.s_addr = interfaceAddr.isZero() ? htonl(INADDR_ANY) : interfaceAddr.raw;
    
    if (setsockopt(_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        KNX_LOGE(TAG, "Failed to join multicast group: %d", errno);
        return knx::util::ErrorCode::OperationFailed;
    }
    
    _multicastAddresses.push_back(multicastAddr);
    
    uint8_t a, b, c, d;
    multicastAddr.toOctets(a, b, c, d);
    KNX_LOGI(TAG, "Joined multicast group %u.%u.%u.%u", a, b, c, d);
    
    return knx::util::Result<void>::ok();
}

void Esp32UdpSocket::leaveMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) {
    if (_socket < 0) {
        return;
    }
    
    struct ip_mreq mreq;
    std::memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = multicastAddr.raw;
    mreq.imr_interface.s_addr = interfaceAddr.isZero() ? htonl(INADDR_ANY) : interfaceAddr.raw;
    
    setsockopt(_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    
    auto it = std::find(_multicastAddresses.begin(), _multicastAddresses.end(), multicastAddr);
    if (it != _multicastAddresses.end()) {
        _multicastAddresses.erase(it);
    }
}

knx::util::Result<void> Esp32UdpSocket::setMulticastInterface(IpAddress interfaceAddr) {
    if (_socket < 0) return knx::util::ErrorCode::NotInitialized;
    struct in_addr ifaddr;
    std::memset(&ifaddr, 0, sizeof(ifaddr));
    ifaddr.s_addr = interfaceAddr.isZero() ? htonl(INADDR_ANY) : interfaceAddr.raw;
    return setsockopt(_socket, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) == 0
        ? knx::util::Result<void>::ok()
        : knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
}

knx::util::Result<void> Esp32UdpSocket::setMulticastLoopback(MulticastLoopbackMode mode) {
    if (_socket < 0) return knx::util::ErrorCode::NotInitialized;
    const bool enable = (mode == MulticastLoopbackMode::Enable);
    const unsigned char loop = enable ? 1 : 0;
    return setsockopt(_socket, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) == 0
        ? knx::util::Result<void>::ok()
        : knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
}

knx::util::Result<void> Esp32UdpSocket::setMulticastTtl(uint8_t ttl) {
    if (_socket < 0) return knx::util::ErrorCode::NotInitialized;
    const unsigned char v = ttl;
    return setsockopt(_socket, IPPROTO_IP, IP_MULTICAST_TTL, &v, sizeof(v)) == 0
        ? knx::util::Result<void>::ok()
        : knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
}

int Esp32UdpSocket::send(IpAddress destAddr, uint16_t destPort,
                         std::span<const uint8_t> buffer) {
    if (_socket < 0 || buffer.size() == 0) {
        return -1;
    }
    
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(destPort);
    addr.sin_addr.s_addr = destAddr.raw;
    
    int sent = sendto(_socket, buffer.data(), buffer.size(), 0, (struct sockaddr*)&addr, sizeof(addr));
    if (sent < 0) {
        KNX_LOGE(TAG, "Failed to send UDP packet: %d", errno);
    }
    
    return sent;
}

int Esp32UdpSocket::receive(std::span<uint8_t> buffer) {
    IpAddress srcAddr;
    uint16_t srcPort;
    return receive(buffer, srcAddr, srcPort);
}

int Esp32UdpSocket::receive(std::span<uint8_t> buffer,
                            IpAddress& srcAddr, uint16_t& srcPort) {
    if (_socket < 0 || buffer.size() == 0) {
        return -1;
    }

    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);

    int received = recvfrom(_socket, buffer.data(), buffer.size(), MSG_DONTWAIT,
                           (struct sockaddr*)&addr, &addrLen);

    if (received > 0) {
        srcAddr = IpAddress(addr.sin_addr.s_addr);
        srcPort = ntohs(addr.sin_port);
    }

    return received;
}

size_t Esp32UdpSocket::available() const {
    if (_socket < 0) {
        return 0;
    }
    
    int bytes;
    if (ioctl(_socket, FIONREAD, &bytes) < 0) {
        return 0;
    }
    
    return bytes;
}

uint16_t Esp32UdpSocket::localPort() const {
    return _port;
}

// ===========================================================================
// Esp32Network
// ===========================================================================

Esp32Network::Esp32Network()
    : _netif(nullptr)
    , _initialized(false)
{
}

Esp32Network::~Esp32Network() {
    // Network cleanup handled by ESP-IDF
}

knx::util::Result<void> Esp32Network::init() {
    if (_initialized) {
        return knx::util::Result<void>::ok();
    }
    
    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Get default WiFi station netif
    _netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!_netif) {
        KNX_LOGW(TAG, "WiFi station netif not found, will retry when WiFi is ready");
    }
    
    _initialized = true;
    KNX_LOGI(TAG, "Network interface initialized");
    
    return knx::util::Result<void>::ok();
}

bool Esp32Network::isConnected() const {
    if (!_netif) {
        return false;
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(_netif, &ip_info) == ESP_OK) {
        return ip_info.ip.addr != 0;
    }
    
    return false;
}

IpAddress Esp32Network::ipAddress() const {
    if (!_netif) {
        return IpAddress(0);
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(_netif, &ip_info) == ESP_OK) {
        return IpAddress(ip_info.ip.addr);
    }
    
    return IpAddress(0);
}

IpAddress Esp32Network::subnetMask() const {
    if (!_netif) {
        return IpAddress(0);
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(_netif, &ip_info) == ESP_OK) {
        return IpAddress(ip_info.netmask.addr);
    }
    
    return IpAddress(0);
}

IpAddress Esp32Network::gateway() const {
    if (!_netif) {
        return IpAddress(0);
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(_netif, &ip_info) == ESP_OK) {
        return IpAddress(ip_info.gw.addr);
    }
    
    return IpAddress(0);
}

void Esp32Network::macAddress(std::span<uint8_t> mac) const {
    if (_netif && mac.size() >= 6) {
        esp_netif_get_mac(_netif, mac.data());
    }
}

std::unique_ptr<UdpSocket> Esp32Network::createUdpSocket() {
    return std::make_unique<Esp32UdpSocket>();
}

} // namespace platform
} // namespace knx
