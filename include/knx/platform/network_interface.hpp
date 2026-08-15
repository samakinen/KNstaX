// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file network_interface.hpp
 * @brief Network abstraction interface
 * 
 * Provides abstraction for IP networking (UDP/TCP sockets)
 */

#pragma once

#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <memory>

#include "knx/netip/netip_config.hpp"

namespace knx {
namespace platform {

/**
 * @brief Multicast loopback configuration
 */
enum class MulticastLoopbackMode : uint8_t {
    Disable = 0,
    Enable = 1
};

/**
 * @brief UDP socket interface
 */
class UdpSocket {
public:
    virtual ~UdpSocket() = default;
    
    /**
     * @brief Open/bind socket (Result-based)
     * @param port Local port to bind (0 for any)
     */
    virtual knx::util::Result<void> open(uint16_t port = 0) = 0;
    
    /**
     * @brief Close socket
     */
    virtual void close() = 0;
    
    /**
     * @brief Check if socket is open
     */
    virtual bool isOpen() const = 0;
    
    /**
     * @brief Join multicast group
     * @param multicastAddr Multicast address (IPv4, network byte order)
     * @param interfaceAddr Interface address to use (IPv4, network byte order). Use 0 for default.
     * @return true on success
     */
    virtual knx::util::Result<void> joinMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) = 0;

    /**
     * @brief Convenience overload: join on default interface.
     */
    knx::util::Result<void> joinMulticast(IpAddress multicastAddr) {
        return joinMulticast(multicastAddr, IpAddress(0));
    }
    
    /**
     * @brief Leave multicast group
     * @param multicastAddr Multicast address (IPv4, network byte order)
     * @param interfaceAddr Interface address used for membership (IPv4, network byte order). Use 0 for default.
     */
    virtual void leaveMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) = 0;

    // Convenience overload: leave on default interface.
    void leaveMulticast(IpAddress multicastAddr) { leaveMulticast(multicastAddr, IpAddress(0)); }

    /**
     * @brief Configure multicast egress interface
     * @param interfaceAddr Interface address (IPv4, network byte order). Use 0 for default.
     */
    virtual knx::util::Result<void> setMulticastInterface(IpAddress interfaceAddr) = 0;

    /**
     * @brief Configure multicast loopback (whether sender receives its own packets)
     */
    virtual knx::util::Result<void> setMulticastLoopback(MulticastLoopbackMode mode) = 0;

    /**
     * @brief Configure multicast TTL
     */
    virtual knx::util::Result<void> setMulticastTtl(uint8_t ttl) = 0;
    
    /**
     * @brief Send data to address
     * @param destAddr Destination IP address
     * @param destPort Destination port
     * @param data Data to send
     * @return Number of bytes sent, or -1 on error
     */
    virtual int send(IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) = 0;
    
    /**
     * @brief Receive data
     * @param buffer Destination buffer span
     * @return Number of bytes received, or -1 on error
     */
    virtual int receive(std::span<uint8_t> buffer) = 0;
    
    /**
     * @brief Receive data with source information
     * @param buffer Destination buffer span
     * @param srcAddr Source IP address (output)
     * @param srcPort Source port (output)
     * @return Number of bytes received, or -1 on error
     */
    virtual int receive(std::span<uint8_t> buffer, IpAddress& srcAddr, uint16_t& srcPort) = 0;
    
    /**
     * @brief Get number of bytes available to read
     */
    virtual size_t available() const = 0;
    
    /**
     * @brief Get local port
     */
    virtual uint16_t localPort() const = 0;
};

/**
 * @brief TCP socket interface (stream)
 */
class TcpSocket {
public:
    virtual ~TcpSocket() = default;

    /**
     * @brief Connect to a remote endpoint
     * @param destAddr Destination IP address (IPv4, network byte order)
     * @param destPort Destination port
     * @return true on success
     */
    virtual knx::util::Result<void> connect(IpAddress destAddr, uint16_t destPort) = 0;

    /**
     * @brief Close socket
     */
    virtual void close() = 0;

    /**
     * @brief Check if socket is connected/open
     */
    virtual bool isOpen() const = 0;

    /**
     * @brief Send bytes on the stream
        * @param data Data to send
        * @return Number of bytes sent, or -1 on error
        */
        virtual int send(std::span<const uint8_t> data) = 0;

    /**
     * @brief Receive bytes from the stream
     * @param buffer Destination buffer span
     * @return Number of bytes received, 0 on orderly shutdown, or -1 on error
     */
    virtual int receive(std::span<uint8_t> buffer) = 0;

    /**
     * @brief Get number of bytes available to read
     */
    virtual size_t available() const = 0;

    /**
     * @brief Get local port (host byte order). Returns 0 if unknown.
     */
    virtual uint16_t localPort() const { return 0; }

    /**
     * @brief Get local IPv4 address (network byte order). Returns 0 if unknown.
     */
    virtual IpAddress localAddress() const { return IpAddress(0); }
};

/**
 * @brief Network interface
 */
class NetworkInterface {
public:
    virtual ~NetworkInterface() = default;
    
    /**
     * @brief Initialize network (Result-based)
     */
    virtual knx::util::Result<void> init() = 0;
    
    /**
     * @brief Check if network is connected
     */
    virtual bool isConnected() const = 0;
    
    /**
     * @brief Get current IP address
     */
    virtual IpAddress ipAddress() const = 0;
    
    /**
     * @brief Get subnet mask
     */
    virtual IpAddress subnetMask() const = 0;
    
    /**
     * @brief Get default gateway
     */
    virtual IpAddress gateway() const = 0;
    
    /**
     * @brief Get MAC address
     * @note Expects a span of at least 6 bytes to be provided.
     */
    virtual void macAddress(std::span<uint8_t> mac) const = 0;
    
    /**
     * @brief Create UDP socket
     * @return UDP socket instance or nullptr on failure
     */
    virtual std::unique_ptr<UdpSocket> createUdpSocket() = 0;

    /**
     * @brief Create TCP socket
     * @return TCP socket instance or nullptr if not supported on this platform
     */
    virtual std::unique_ptr<TcpSocket> createTcpSocket() { return nullptr; }
    
protected:
    NetworkInterface() = default;
};

// ============================================================================
// Helper Functions
// ============================================================================

// Common multicast addresses
static const IpAddress KNX_MULTICAST_ADDRESS = IpAddress::fromOctets(224, 0, 23, 12);
constexpr uint16_t KNX_IP_PORT = knx::netip::config::kDefaultPort;

} // namespace platform
} // namespace knx
