// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file linux_platform.cpp
 * @brief Linux/POSIX platform implementation
 */

#include "knx/platform/linux_platform.hpp"

#include <random>
#include "knx/platform/memory_interface.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/uart_interface.hpp"
#include "knx/platform/spi_interface.hpp"
#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <string>
#include <vector>
#include <ctime>
#include <limits>
#include <unistd.h>
#include <sys/utsname.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <array>
#include <bit>

// This file provides host-only stubs and should not be compiled for embedded targets.
#ifdef ESP_PLATFORM
#error "linux_platform.cpp is host-only and must not be compiled under ESP_PLATFORM."
#endif

#include "knx/util/log.hpp"

namespace knx {
namespace platform {

namespace {

struct SelectedInterface {
    std::string name;
    in_addr ipv4{};
};

static bool selectPrimaryIpv4Interface(SelectedInterface& selected) {
    struct ifaddrs* ifaddr = nullptr;
    if (::getifaddrs(&ifaddr) != 0) {
        KNX_LOGE("KNX.Platform", "getifaddrs failed: %s", strerror(errno));
        return false;
    }

    bool found = false;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        const unsigned int flags = ifa->ifa_flags;
        if ((flags & IFF_UP) == 0) {
            continue;
        }
        if ((flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (!sa) {
            continue;
        }

        selected.name = ifa->ifa_name;
        selected.ipv4 = sa->sin_addr;
        found = true;
        break;
    }

    if (!found) {
        // Fall back to loopback if nothing else is available.
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_name) {
                continue;
            }
            if (ifa->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            const unsigned int flags = ifa->ifa_flags;
            if ((flags & IFF_UP) == 0) {
                continue;
            }
            if ((flags & IFF_LOOPBACK) == 0) {
                continue;
            }
            auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            selected.name = ifa->ifa_name;
            selected.ipv4 = sa->sin_addr;
            found = true;
            break;
        }
    }

    ::freeifaddrs(ifaddr);
    return found;
}

static bool tryGetInterfaceMac(const std::string& ifName, uint8_t mac[6]) {
    if (ifName.empty()) {
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        KNX_LOGE("KNX.Platform", "socket(AF_INET,SOCK_DGRAM) failed: %s", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifName.c_str());

    const int rc = ::ioctl(fd, SIOCGIFHWADDR, &ifr);
    ::close(fd);

    if (rc != 0) {
        KNX_LOGW("KNX.Platform", "SIOCGIFHWADDR failed for %s: %s", ifName.c_str(), strerror(errno));
        return false;
    }

    std::memcpy(mac, reinterpret_cast<uint8_t*>(ifr.ifr_hwaddr.sa_data), 6);
    return true;
}

static uint32_t fnv1a32(std::span<const std::byte> data) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < data.size(); ++i) {
        hash ^= static_cast<uint32_t>(std::to_integer<uint8_t>(data[i]));
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t computeHostSerial() {
    std::ifstream file("/etc/machine-id");
    if (file.is_open()) {
        std::string machineId;
        std::getline(file, machineId);
        if (!machineId.empty()) {
            auto charSpan = std::span<const char>(machineId.data(), machineId.size());
            return fnv1a32(std::as_bytes(charSpan));
        }
    }

    const uint64_t mix = (static_cast<uint64_t>(::getpid()) << 32) ^ static_cast<uint64_t>(::time(nullptr));
    const auto mixBytes = std::bit_cast<std::array<std::byte, sizeof(mix)>>(mix);
    return fnv1a32(std::span<const std::byte>(mixBytes.data(), mixBytes.size()));
}

} // namespace

// ============================================================================
// Linux Memory Interface (File-backed persistent EEPROM)
// ============================================================================

class LinuxMemory : public MemoryInterface {
public:
    LinuxMemory() : _buffer(4096, 0), _dirty(false) {}
    
    ~LinuxMemory() {
        if (_dirty) {
            _saveToFile();
        }
    }

    MemoryType type() const override {
        return MemoryType::EEPROM;
    }

    size_t size() const override {
        return _buffer.size();
    }

    size_t pageSize() const override {
        return 1;
    }

    size_t eraseBlockSize() const override {
        return 1;
    }

    util::Result<void> init() override {
        return _loadFromFile() ? util::Result<void>::ok() : util::Result<void>::err(util::ErrorCode::OperationFailed);
    }
    
    uint32_t read(uint32_t address, std::span<uint8_t> buffer) override {
        if (buffer.size() == 0) {
            return 0;
        }
        if (address >= _buffer.size()) {
            return 0;
        }
        const size_t maxLen = _buffer.size() - address;
        const size_t toCopy = (buffer.size() > maxLen) ? maxLen : buffer.size();
        std::memcpy(buffer.data(), _buffer.data() + address, toCopy);
        return static_cast<uint32_t>(toCopy);
    }
    
    uint32_t write(uint32_t address, std::span<const uint8_t> buffer) override {
        if (buffer.size() == 0) {
            return 0;
        }
        if (address >= _buffer.size()) {
            return 0;
        }
        const size_t maxLen = _buffer.size() - address;
        const size_t toCopy = (buffer.size() > maxLen) ? maxLen : buffer.size();
        std::memcpy(_buffer.data() + address, buffer.data(), toCopy);
        _dirty = true;
        return static_cast<uint32_t>(toCopy);
    }

    uint32_t write(uint32_t address, uint8_t value, size_t repeat) override {
        if (repeat == 0) {
            return 0;
        }
        if (address >= _buffer.size()) {
            return 0;
        }
        const size_t maxLen = _buffer.size() - address;
        const size_t toWrite = (repeat > maxLen) ? maxLen : repeat;
        std::fill(_buffer.begin() + address, _buffer.begin() + address + toWrite, value);
        _dirty = true;
        return static_cast<uint32_t>(toWrite);
    }
    
    void commit() override {
        (void)_saveToFile();
    }

    util::Result<void> erase(uint32_t address, size_t length) override {
        if (length == 0) {
            return util::ErrorCode::InvalidParameter;
        }
        if (address >= _buffer.size()) {
            return util::ErrorCode::InvalidAddress;
        }
        const size_t maxLen = _buffer.size() - address;
        const size_t toErase = (length > maxLen) ? maxLen : length;
        std::fill(_buffer.begin() + address, _buffer.begin() + address + toErase, 0);
        _dirty = true;
        return util::Result<void>::ok();
    }

    std::span<uint8_t> getBuffer(uint32_t address, size_t length) override {
        if (address >= _buffer.size() || length == 0) {
            return {};
        }
        const size_t maxLen = _buffer.size() - address;
        const size_t toReturn = (length > maxLen) ? maxLen : length;
        return std::span<uint8_t>(_buffer.data() + address, toReturn);
    }
    
private:
    std::vector<uint8_t> _buffer;
    bool _dirty;
    static constexpr const char* EEPROM_FILE = "/tmp/knx_eeprom.bin";
    
    bool _loadFromFile() {
        std::ifstream file(EEPROM_FILE, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            // File doesn't exist yet - initialize with zeros
            KNX_LOGD("KNX.Platform", "EEPROM file not found, initializing empty");
            return true;
        }
        
        std::streamsize size = file.tellg();
        if (size > static_cast<std::streamsize>(_buffer.size())) {
            KNX_LOGW("KNX.Platform", "EEPROM file larger than buffer, truncating");
            size = _buffer.size();
        }
        
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(_buffer.data()), size);
        
        KNX_LOGD("KNX.Platform", "Loaded %zu bytes from EEPROM file", size);
        _dirty = false;
        return true;
    }
    
    bool _saveToFile() {
        std::ofstream file(EEPROM_FILE, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            KNX_LOGE("KNX.Platform", "Failed to open EEPROM file for writing");
            return false;
        }
        
        file.write(reinterpret_cast<const char*>(_buffer.data()), _buffer.size());
        if (!file.good()) {
            KNX_LOGE("KNX.Platform", "Failed to write EEPROM file");
            return false;
        }
        
        KNX_LOGD("KNX.Platform", "Saved %zu bytes to EEPROM file", _buffer.size());
        _dirty = false;
        return true;
    }
};

// ============================================================================
// Linux Network Interface (Real POSIX socket implementation)
// ============================================================================

class LinuxNetwork : public NetworkInterface {
public:
    class LinuxTcpSocket : public TcpSocket {
    public:
        LinuxTcpSocket() = default;
        ~LinuxTcpSocket() override { close(); }

        knx::util::Result<void> connect(IpAddress destAddr, uint16_t destPort) override {
            if (_fd >= 0) {
                return knx::util::Result<void>::ok();
            }

            _fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (_fd < 0) {
                return knx::util::ErrorCode::ResourceUnavailable;
            }

            struct sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = destAddr.raw;
            dest.sin_port = htons(destPort);

            if (::connect(_fd, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) != 0) {
                close();
                return knx::util::ErrorCode::OperationFailed;
            }

            struct sockaddr_in local{};
            socklen_t llen = sizeof(local);
            if (::getsockname(_fd, reinterpret_cast<struct sockaddr*>(&local), &llen) == 0) {
                _localAddr = IpAddress(local.sin_addr.s_addr);
                _localPort = ntohs(local.sin_port);
            }

            return knx::util::Result<void>::ok();
        }

        void close() override {
            if (_fd >= 0) {
                ::close(_fd);
                _fd = -1;
            }
            _localAddr = IpAddress(0);
            _localPort = 0;
        }

        bool isOpen() const override { return _fd >= 0; }

#include <span>

        int send(std::span<const uint8_t> data) override {
            if (_fd < 0 || data.data() == nullptr || data.size() == 0) {
                return -1;
            }
            const ssize_t rc = ::send(_fd, data.data(), data.size(),
#ifdef MSG_NOSIGNAL
                                      MSG_NOSIGNAL
#else
                                      0
#endif
            );
            return (rc < 0) ? -1 : static_cast<int>(rc);
        }

        int receive(std::span<uint8_t> buffer) override {
            if (_fd < 0 || buffer.data() == nullptr || buffer.size() == 0) {
                return -1;
            }
            const ssize_t rc = ::recv(_fd, buffer.data(), buffer.size(), 0);
            if (rc < 0) {
                return -1;
            }
            return static_cast<int>(rc);
        }

        size_t available() const override {
            if (_fd < 0) {
                return 0;
            }
            int bytes = 0;
            if (::ioctl(_fd, FIONREAD, &bytes) != 0 || bytes < 0) {
                return 0;
            }
            return static_cast<size_t>(bytes);
        }

        uint16_t localPort() const override { return _localPort; }
        IpAddress localAddress() const override { return _localAddr; }

    private:
        int _fd{-1};
        IpAddress _localAddr{IpAddress(0)};
        uint16_t _localPort{0};
    };

    class LinuxUdpSocket : public UdpSocket {
    public:
        LinuxUdpSocket() = default;
        ~LinuxUdpSocket() override { close(); }

        knx::util::Result<void> open(uint16_t port = 0) override {
            if (_fd >= 0) {
                return knx::util::Result<void>::ok();
            }

            _fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (_fd < 0) {
                return knx::util::ErrorCode::ResourceUnavailable;
            }

            int reuse = 1;
            ::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

#ifdef SO_REUSEPORT
            ::setsockopt(_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(port);

            if (::bind(_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
                close();
                return knx::util::ErrorCode::OperationFailed;
            }

            struct sockaddr_in bound{};
            socklen_t boundLen = sizeof(bound);
            if (::getsockname(_fd, reinterpret_cast<struct sockaddr*>(&bound), &boundLen) == 0) {
                _localPort = ntohs(bound.sin_port);
            }

            return knx::util::Result<void>::ok();
        }

        void close() override {
            if (_fd >= 0) {
                ::close(_fd);
                _fd = -1;
                _localPort = 0;
            }
        }

        bool isOpen() const override { return _fd >= 0; }

        knx::util::Result<void> joinMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) override {
            if (_fd < 0) {
                return knx::util::ErrorCode::NotInitialized;
            }
            struct ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = multicastAddr.raw;
            mreq.imr_interface.s_addr = interfaceAddr.isZero() ? htonl(INADDR_ANY) : interfaceAddr.raw;
            return (::setsockopt(_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0)
                ? knx::util::Result<void>::ok()
                : knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
        }

        void leaveMulticast(IpAddress multicastAddr, IpAddress interfaceAddr) override {
            if (_fd < 0) {
                return;
            }
            struct ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = multicastAddr.raw;
            mreq.imr_interface.s_addr = interfaceAddr.isZero() ? htonl(INADDR_ANY) : interfaceAddr.raw;
            (void)::setsockopt(_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        }

        knx::util::Result<void> setMulticastInterface(IpAddress interfaceAddr) override {
            if (_fd < 0) return knx::util::ErrorCode::NotInitialized;
            struct in_addr ifaddr{};
            ifaddr.s_addr = interfaceAddr.isZero() ? htonl(INADDR_ANY) : interfaceAddr.raw;
            return (::setsockopt(_fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) == 0)
                ? knx::util::Result<void>::ok()
                : knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
        }

        knx::util::Result<void> setMulticastLoopback(MulticastLoopbackMode mode) override {
            if (_fd < 0) return knx::util::ErrorCode::NotInitialized;
            const bool enable = (mode == MulticastLoopbackMode::Enable);
            const unsigned char loop = enable ? 1 : 0;
            return (::setsockopt(_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) == 0)
                ? knx::util::Result<void>::ok()
                : knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
        }

        knx::util::Result<void> setMulticastTtl(uint8_t ttl) override {
            if (_fd < 0) return knx::util::ErrorCode::NotInitialized;
            const unsigned char v = ttl;
            return (::setsockopt(_fd, IPPROTO_IP, IP_MULTICAST_TTL, &v, sizeof(v)) == 0)
                ? knx::util::Result<void>::ok()
                : knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
        }

        int send(IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) override {
            if (_fd < 0 || data.data() == nullptr || data.size() == 0) {
                return -1;
            }
            struct sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = destAddr.raw;
            dest.sin_port = htons(destPort);
            const ssize_t rc = ::sendto(_fd, data.data(), data.size(), 0,
                                        reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
            return (rc < 0) ? -1 : static_cast<int>(rc);
        }
        int receive(std::span<uint8_t> buffer) override {
            IpAddress srcAddr(0);
            uint16_t srcPort = 0;
            return receive(buffer, srcAddr, srcPort);
        }

        int receive(std::span<uint8_t> buffer, IpAddress& srcAddr, uint16_t& srcPort) override {
            if (_fd < 0 || buffer.data() == nullptr || buffer.size() == 0) {
                return -1;
            }

            struct sockaddr_in src{};
            socklen_t srcLen = sizeof(src);
            const ssize_t rc = ::recvfrom(_fd, buffer.data(), buffer.size(), 0,
                                          reinterpret_cast<struct sockaddr*>(&src), &srcLen);
            if (rc < 0) {
                return -1;
            }
            srcAddr = IpAddress(src.sin_addr.s_addr);
            srcPort = ntohs(src.sin_port);
            return static_cast<int>(rc);
        }

        size_t available() const override {
            if (_fd < 0) {
                return 0;
            }
            int bytes = 0;
            if (::ioctl(_fd, FIONREAD, &bytes) != 0 || bytes < 0) {
                return 0;
            }
            return static_cast<size_t>(bytes);
        }

        uint16_t localPort() const override { return _localPort; }

    private:
        int _fd{-1};
        uint16_t _localPort{0};
    };

    knx::util::Result<void> init() override {
        KNX_LOGD("KNX.Platform", "LinuxNetwork initialized (POSIX UDP sockets)");
        return knx::util::Result<void>::ok();
    }

    bool isConnected() const override {
        return !ipAddress().isZero();
    }

    IpAddress ipAddress() const override {
        SelectedInterface selected;
        if (!selectPrimaryIpv4Interface(selected)) {
            return IpAddress(0);
        }
        return IpAddress(selected.ipv4.s_addr);
    }

    IpAddress subnetMask() const override {
        struct ifaddrs* ifaddr = nullptr;
        if (::getifaddrs(&ifaddr) != 0) {
            return IpAddress(0);
        }

        SelectedInterface selected;
        if (!selectPrimaryIpv4Interface(selected)) {
            ::freeifaddrs(ifaddr);
            return IpAddress(0);
        }

        uint32_t mask = 0;
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_netmask || !ifa->ifa_name) {
                continue;
            }
            if (ifa->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if (selected.name != ifa->ifa_name) {
                continue;
            }

            auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_netmask);
            mask = sa->sin_addr.s_addr;
            break;
        }

        ::freeifaddrs(ifaddr);
        return IpAddress(mask);
    }

    IpAddress gateway() const override {
        SelectedInterface selected;
        if (!selectPrimaryIpv4Interface(selected)) {
            return IpAddress(0);
        }

        std::ifstream route("/proc/net/route");
        if (!route.is_open()) {
            return IpAddress(0);
        }

        std::string line;
        // Skip header
        std::getline(route, line);

        while (std::getline(route, line)) {
            char iface[64] = {0};
            unsigned long dest = 0;
            unsigned long gw = 0;
            unsigned int flags = 0;

            // Format: Iface Destination Gateway Flags ...
            if (std::sscanf(line.c_str(), "%63s %lx %lx %x", iface, &dest, &gw, &flags) < 4) {
                continue;
            }

            // Default route has Destination == 0
            if (dest != 0) {
                continue;
            }

            if (selected.name != iface) {
                continue;
            }

            const uint32_t gwLe = static_cast<uint32_t>(gw);
            return IpAddress(htonl(gwLe));
        }

        return IpAddress(0);
    }

    void macAddress(std::span<uint8_t> mac) const override {
        if (mac.data() == nullptr || mac.size() < 6) {
            return;
        }
        std::memset(mac.data(), 0, 6);

        SelectedInterface selected;
        if (!selectPrimaryIpv4Interface(selected)) {
            return;
        }

        (void)tryGetInterfaceMac(selected.name, mac.data());
    }

    std::unique_ptr<UdpSocket> createUdpSocket() override {
        return std::make_unique<LinuxUdpSocket>();
    }

    std::unique_ptr<TcpSocket> createTcpSocket() override {
        return std::make_unique<LinuxTcpSocket>();
    }
};

// ============================================================================
// Linux UART Interface (Real termios implementation)
// ============================================================================

class LinuxUart : public UartInterface {
public:
    LinuxUart() : _fd(-1), _rxCallback(nullptr), _rxCallbackContext(nullptr), _overflowFlag(false) {}
    
    ~LinuxUart() {
        close();
    }
    
    util::Result<void> init(const UartConfig& config) override {
        if (_fd >= 0) {
            KNX_LOGW("KNX.Platform", "UART already initialized");
            return util::ErrorCode::AlreadyInitialized;
        }
        
        // Try common serial device paths
        const char* devices[] = {
            "/dev/ttyUSB0",
            "/dev/ttyACM0",
            "/dev/ttyS0",
            "/dev/serial0",
            nullptr
        };
        
        for (int i = 0; devices[i] != nullptr; i++) {
            _fd = ::open(devices[i], O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (_fd >= 0) {
                KNX_LOGD("KNX.Platform", "Opened UART device: %s", devices[i]);
                break;
            }
        }
        
        if (_fd < 0) {
            KNX_LOGE("KNX.Platform", "No UART device found");
            return util::ErrorCode::ResourceUnavailable;
        }
        
        // Configure termios
        struct termios tty;
        if (tcgetattr(_fd, &tty) != 0) {
            KNX_LOGE("KNX.Platform", "tcgetattr failed: %s", strerror(errno));
            ::close(_fd);
            _fd = -1;
            return util::ErrorCode::OperationFailed;
        }
        
        // Set baud rate
        speed_t speed = _baudToSpeed(config.baudRate);
        cfsetospeed(&tty, speed);
        cfsetispeed(&tty, speed);
        
        // Clear existing settings
        tty.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
        
        // Set data bits
        switch (config.dataBits) {
            case 5: tty.c_cflag |= CS5; break;
            case 6: tty.c_cflag |= CS6; break;
            case 7: tty.c_cflag |= CS7; break;
            case 8: default: tty.c_cflag |= CS8; break;
        }
        
        // Set parity
        switch (config.parity) {
            case UartConfig::Parity::None:
                break;  // Already cleared above
            case UartConfig::Parity::Odd:
                tty.c_cflag |= PARENB | PARODD;
                break;
            case UartConfig::Parity::Even:
                tty.c_cflag |= PARENB;
                break;
        }
        
        // Set stop bits
        if (config.stopBits == 2) {
            tty.c_cflag |= CSTOPB;
        }
        
        // Enable receiver, ignore modem control lines
        tty.c_cflag |= CREAD | CLOCAL;
        
        // Disable canonical mode, echo, signals
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        
        // Disable software flow control
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        
        // Raw output
        tty.c_oflag &= ~OPOST;
        
        // Set read timeout (non-blocking)
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;
        
        // Apply settings
        if (tcsetattr(_fd, TCSANOW, &tty) != 0) {
            KNX_LOGE("KNX.Platform", "tcsetattr failed: %s", strerror(errno));
            ::close(_fd);
            _fd = -1;
            return util::ErrorCode::OperationFailed;
        }
        
        // Flush buffers
        tcflush(_fd, TCIOFLUSH);
        
        const char* parityStr = config.parity == UartConfig::Parity::None ? "N" :
                                (config.parity == UartConfig::Parity::Even ? "E" : "O");
        KNX_LOGD("KNX.Platform", "UART configured: %u baud, %u%s%u", 
                 config.baudRate, config.dataBits, parityStr, config.stopBits);
        return util::Result<void>::ok();
    }
    
    void close() override {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
            KNX_LOGD("KNX.Platform", "UART closed");
        }
    }
    
    bool isOpen() const override {
        return _fd >= 0;
    }
    
    size_t available() const override {
        if (_fd < 0) return 0;
        
        int bytes = 0;
        if (::ioctl(_fd, FIONREAD, &bytes) < 0) {
            return 0;
        }
        
        return static_cast<size_t>(bytes);
    }
    
    int read() override {
        if (_fd < 0) return -1;
        
        uint8_t byte;
        ssize_t result = ::read(_fd, &byte, 1);
        if (result == 1) {
            return byte;
        }
        return -1;
    }
    
    size_t read(std::span<uint8_t> buffer) override {
        if (_fd < 0 || buffer.size() == 0) return 0;

        ssize_t bytesRead = ::read(_fd, buffer.data(), buffer.size());
        if (bytesRead < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  // No data available
            }
            KNX_LOGE("KNX.Platform", "UART read failed: %s", strerror(errno));
            return 0;
        }

        return static_cast<size_t>(bytesRead);
    }
    
    size_t write(uint8_t byte) override {
        if (_fd < 0) return 0;
        
        ssize_t written = ::write(_fd, &byte, 1);
        if (written < 0) {
            KNX_LOGE("KNX.Platform", "UART write failed: %s", strerror(errno));
            return 0;
        }
        
        return static_cast<size_t>(written);
    }
    
    size_t write(std::span<const uint8_t> data) override {
        if (_fd < 0 || data.size() == 0) return 0;

        ssize_t written = ::write(_fd, data.data(), data.size());
        if (written < 0) {
            KNX_LOGE("KNX.Platform", "UART write failed: %s", strerror(errno));
            return 0;
        }

        return static_cast<size_t>(written);
    }
    
    void flush() override {
        if (_fd >= 0) {
            tcdrain(_fd);  // Wait for all output to be transmitted
        }
    }
    
    void clear() override {
        if (_fd >= 0) {
            tcflush(_fd, TCIFLUSH);  // Flush input buffer
            _overflowFlag = false;
        }
    }
    
    bool overflow() const override {
        return _overflowFlag;
    }
    
    void setRxCallback(RxCallback callback, void* context) override {
        _rxCallback = callback;
        _rxCallbackContext = context;
    }
    
private:
    int _fd;
    RxCallback _rxCallback;
    void* _rxCallbackContext;
    bool _overflowFlag;
    
    speed_t _baudToSpeed(uint32_t baudrate) const {
        switch (baudrate) {
            case 9600: return B9600;
            case 19200: return B19200;
            case 38400: return B38400;
            case 57600: return B57600;
            case 115200: return B115200;
            case 230400: return B230400;
            case 460800: return B460800;
            case 500000: return B500000;
            case 576000: return B576000;
            case 921600: return B921600;
            case 1000000: return B1000000;
            case 1152000: return B1152000;
            case 1500000: return B1500000;
            case 2000000: return B2000000;
            case 2500000: return B2500000;
            case 3000000: return B3000000;
            case 3500000: return B3500000;
            case 4000000: return B4000000;
            default:
                KNX_LOGW("KNX.Platform", "Unsupported baud rate %u, using 115200", baudrate);
                return B115200;
        }
    }
};

// ============================================================================
// Linux SPI Interface (Real spidev implementation)
// ============================================================================

class LinuxSpi : public SpiInterface {
public:
    LinuxSpi() : _fd(-1), _csPin(-1), _csActiveHigh(false) {}
    
    ~LinuxSpi() {
        close();
    }
    
    util::Result<void> init(const SpiConfig& config) override {
        if (_fd >= 0) {
            KNX_LOGW("KNX.Platform", "SPI already initialized");
            return util::ErrorCode::AlreadyInitialized;
        }
        
        // Save configuration
        _config = config;
        _csPin = config.csPin;
        _csActiveHigh = config.csActiveHigh;

        if (_csPin >= 0) {
            if (!_csGpio.init(_csPin)) {
                KNX_LOGE("KNX.Platform", "SPI manual CS requested (GPIO %d) but sysfs GPIO is unavailable", _csPin);
                return util::ErrorCode::ResourceUnavailable;
            }
            // Deassert CS by default
            setCs(ChipSelectLevel::Deassert);
        }
        
        // Try common SPI device paths
        const char* devices[] = {
            "/dev/spidev0.0",
            "/dev/spidev0.1",
            "/dev/spidev1.0",
            "/dev/spidev1.1",
            nullptr
        };
        
        for (int i = 0; devices[i] != nullptr; i++) {
            _fd = ::open(devices[i], O_RDWR);
            if (_fd >= 0) {
                KNX_LOGD("KNX.Platform", "Opened SPI device: %s", devices[i]);
                break;
            }
        }
        
        if (_fd < 0) {
            KNX_LOGE("KNX.Platform", "No SPI device found");
            return util::ErrorCode::ResourceUnavailable;
        }
        
        // Set SPI mode
        uint8_t spiMode = static_cast<uint8_t>(config.mode);
        if (::ioctl(_fd, SPI_IOC_WR_MODE, &spiMode) < 0) {
            KNX_LOGE("KNX.Platform", "Failed to set SPI mode: %s", strerror(errno));
            ::close(_fd);
            _fd = -1;
            return util::ErrorCode::OperationFailed;
        }
        
        // Set bits per word (8 bits)
        uint8_t bits = 8;
        if (::ioctl(_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
            KNX_LOGE("KNX.Platform", "Failed to set SPI bits per word: %s", strerror(errno));
            ::close(_fd);
            _fd = -1;
            return util::ErrorCode::OperationFailed;
        }
        
        // Set max speed
        uint32_t speed = config.clockHz;
        if (::ioctl(_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
            KNX_LOGE("KNX.Platform", "Failed to set SPI speed: %s", strerror(errno));
            ::close(_fd);
            _fd = -1;
            return util::ErrorCode::OperationFailed;
        }
        
        // Set bit order (LSB first if needed)
        uint8_t lsb = (config.bitOrder == SpiBitOrder::LsbFirst) ? 1 : 0;
        if (::ioctl(_fd, SPI_IOC_WR_LSB_FIRST, &lsb) < 0) {
            KNX_LOGW("KNX.Platform", "Failed to set SPI bit order (may not be supported)");
            // Not critical, continue
        }
        
        KNX_LOGD("KNX.Platform", "SPI configured: %u Hz, mode %u, %s first", 
                 config.clockHz, spiMode,
                 config.bitOrder == SpiBitOrder::MsbFirst ? "MSB" : "LSB");
        return util::Result<void>::ok();
    }
    
    void close() override {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
            KNX_LOGD("KNX.Platform", "SPI closed");
        }

        _csGpio.close();
    }
    
    bool isOpen() const override {
        return _fd >= 0;
    }
    
    void beginTransaction() override {
        if (_csPin >= 0) {
            setCs(ChipSelectLevel::Assert);
        }
    }
    
    void endTransaction() override {
        if (_csPin >= 0) {
            setCs(ChipSelectLevel::Deassert);
        }
    }
    
    uint8_t transfer(uint8_t data) override {
        if (_fd < 0) return 0;
        
        uint8_t rxData = 0;
        struct spi_ioc_transfer spi_transfer;
        std::memset(&spi_transfer, 0, sizeof(spi_transfer));
        
        spi_transfer.tx_buf = reinterpret_cast<__u64>(&data);
        spi_transfer.rx_buf = reinterpret_cast<__u64>(&rxData);
        spi_transfer.len = 1;
        spi_transfer.speed_hz = _config.clockHz;
        spi_transfer.bits_per_word = 8;
        
        if (::ioctl(_fd, SPI_IOC_MESSAGE(1), &spi_transfer) < 0) {
            KNX_LOGE("KNX.Platform", "SPI transfer failed: %s", strerror(errno));
            return 0;
        }
        
        return rxData;
    }
    
    size_t transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) override {
        if (_fd < 0) return 0;
        const size_t length = tx.size() ? tx.size() : rx.size();
        if (length == 0) return 0;

        // Prepare buffers
        std::vector<uint8_t> txTemp, rxTemp;
        const uint8_t* txPtr = tx.data();
        uint8_t* rxPtr = rx.data();

        if (tx.empty()) {
            txTemp.resize(length, 0x00);
            txPtr = txTemp.data();
        }
        if (rx.empty()) {
            rxTemp.resize(length);
            rxPtr = rxTemp.data();
        }

        struct spi_ioc_transfer spi_transfer;
        std::memset(&spi_transfer, 0, sizeof(spi_transfer));

        spi_transfer.tx_buf = reinterpret_cast<__u64>(txPtr);
        spi_transfer.rx_buf = reinterpret_cast<__u64>(rxPtr);
        if (length > std::numeric_limits<__u32>::max()) {
            KNX_LOGE("KNX.Platform", "SPI transfer length too large: %zu", length);
            return 0;
        }
        spi_transfer.len = static_cast<__u32>(length);
        spi_transfer.speed_hz = _config.clockHz;
        spi_transfer.bits_per_word = 8;

        int ret = ::ioctl(_fd, SPI_IOC_MESSAGE(1), &spi_transfer);
        if (ret < 0) {
            KNX_LOGE("KNX.Platform", "SPI transfer failed: %s", strerror(errno));
            return 0;
        }

        return length;
    }
    
    size_t transfer(std::span<uint8_t> buffer) override {
        if (buffer.size() == 0) return 0;
        return transfer(buffer, buffer);
    }
    
    void setCs(ChipSelectLevel level) override {
        if (_csPin < 0) {
            return;
        }

        const bool asserted = (level == ChipSelectLevel::Assert);
        const bool assertLevel = _csActiveHigh ? asserted : !asserted;
        if (!_csGpio.set(assertLevel)) {
            KNX_LOGE("KNX.Platform", "Failed to set sysfs GPIO CS%d", _csPin);
        }
    }
    
private:
    struct SysfsGpioOut {
        int pin{-1};
        int valueFd{-1};

        static bool writeFile(const std::string& path, const std::string& content) {
            const int fd = ::open(path.c_str(), O_WRONLY);
            if (fd < 0) {
                return false;
            }
            const ssize_t rc = ::write(fd, content.c_str(), content.size());
            ::close(fd);
            return rc == static_cast<ssize_t>(content.size());
        }

        bool init(int gpioPin) {
            close();
            pin = gpioPin;

            // sysfs GPIO may not exist on all kernels/distros, but it's a reasonable
            // dependency-free option when available.
            if (::access("/sys/class/gpio", F_OK) != 0) {
                return false;
            }

            const std::string gpioDir = "/sys/class/gpio/gpio" + std::to_string(pin);
            if (::access(gpioDir.c_str(), F_OK) != 0) {
                // Export
                if (!writeFile("/sys/class/gpio/export", std::to_string(pin))) {
                    // If export fails because it's already exported, continue.
                    if (errno != EBUSY) {
                        return false;
                    }
                }

                // Wait briefly for sysfs to create the directory
                for (int i = 0; i < 50; i++) {
                    if (::access(gpioDir.c_str(), F_OK) == 0) {
                        break;
                    }
                    ::usleep(1000);
                }
            }

            if (::access(gpioDir.c_str(), F_OK) != 0) {
                return false;
            }

            if (!writeFile(gpioDir + "/direction", "out")) {
                return false;
            }

            valueFd = ::open((gpioDir + "/value").c_str(), O_WRONLY);
            if (valueFd < 0) {
                return false;
            }

            return true;
        }

        bool set(bool high) {
            if (valueFd < 0) {
                return false;
            }
            const char c = high ? '1' : '0';
            if (::lseek(valueFd, 0, SEEK_SET) < 0) {
                return false;
            }
            const ssize_t rc = ::write(valueFd, &c, 1);
            return rc == 1;
        }

        void close() {
            if (valueFd >= 0) {
                ::close(valueFd);
                valueFd = -1;
            }
            pin = -1;
        }
    };

    int _fd;
    SpiConfig _config;
    int _csPin;
    bool _csActiveHigh;
    SysfsGpioOut _csGpio;
};

// ============================================================================
// Linux Platform Implementation
// ============================================================================

static thread_local TaskHandle g_currentTaskHandle = nullptr;

LinuxPlatform::LinuxPlatform() {
    _startTime = std::chrono::steady_clock::now();
    _memory = std::make_unique<LinuxMemory>();
    _network = std::make_unique<LinuxNetwork>();
    initSystemInfo();
    _uart = std::make_unique<LinuxUart>();
    _spi = std::make_unique<LinuxSpi>();
}

LinuxPlatform::~LinuxPlatform() {
    std::lock_guard<std::mutex> lock(_tasksMutex);
    for (auto& kv : _tasks) {
        if (kv.second && kv.second->thread.joinable()) {
            kv.second->thread.join();
        }
    }
    _tasks.clear();
}

void LinuxPlatform::initSystemInfo() {
    _serialNumber = computeHostSerial();

    _macAddress.fill(0);
    if (_network) {
        _network->macAddress(_macAddress);
        bool allZero = true;
        for (size_t i = 0; i < _macAddress.size(); i++) {
            if (_macAddress[i] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            // Fall back to a locally-administered MAC derived from serial.
            _macAddress[0] = 0x02;
            _macAddress[1] = static_cast<uint8_t>((_serialNumber >> 24) & 0xFF);
            _macAddress[2] = static_cast<uint8_t>((_serialNumber >> 16) & 0xFF);
            _macAddress[3] = static_cast<uint8_t>((_serialNumber >> 8) & 0xFF);
            _macAddress[4] = static_cast<uint8_t>(_serialNumber & 0xFF);
            _macAddress[5] = 0x01;
        }
    }
}

void LinuxPlatform::restart() {
    std::cout << "RESTART: System restart requested" << std::endl;
    std::exit(0);
}

void LinuxPlatform::fatalError() {
    std::cerr << "FATAL ERROR: Halting system" << std::endl;
    std::exit(1);
}

uint32_t LinuxPlatform::millis() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _startTime);
    return static_cast<uint32_t>(elapsed.count());
}

uint64_t LinuxPlatform::micros() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - _startTime);
    return elapsed.count();
}

void LinuxPlatform::delay(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void LinuxPlatform::delayMicroseconds(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

uint32_t LinuxPlatform::uniqueSerialNumber() const {
    return _serialNumber;
}

void LinuxPlatform::randomBytes(std::span<uint8_t> out) {
    static thread_local std::random_device device;
    for (auto& byte : out) {
        byte = static_cast<uint8_t>(device() & 0xFFu);
    }
}

void LinuxPlatform::macAddress(std::span<uint8_t, 6> mac) const {
    std::copy(_macAddress.begin(), _macAddress.end(), mac.begin());
}

// ========================================================================
// Threading
// ========================================================================

TaskHandle LinuxPlatform::createTask(const TaskConfig& config) {
    const TaskHandle handle = generateTaskHandle();

    auto wrapper = std::make_unique<TaskWrapper>(config.function);
    TaskWrapper* wrapperPtr = wrapper.get();

    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        _tasks[handle] = std::move(wrapper);
    }

    wrapperPtr->thread = std::thread([handle, wrapperPtr]() {
        g_currentTaskHandle = handle;
        if (wrapperPtr->function) {
            wrapperPtr->function();
        }
        g_currentTaskHandle = nullptr;
    });

    return handle;
}

void LinuxPlatform::deleteTask(TaskHandle task) {
    // The joined thread needs _tasksMutex itself on every taskNotifyTake(), so
    // joining while holding the lock deadlocks whenever the worker loops back
    // into a take() after we enter the join. Unregister under the lock, then
    // release it and join outside.
    //
    // Dropping the map entry first is what lets the worker finish: its next
    // taskNotifyTake() no longer finds the handle and returns 0 immediately,
    // so the task function falls out of its poll loop instead of waiting for
    // the full poll timeout. The wrapper itself is kept alive in this local
    // until after the join, because the thread body dereferences it.
    std::unique_ptr<TaskWrapper> wrapper;
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        auto it = _tasks.find(task);
        if (it == _tasks.end()) {
            return;
        }
        wrapper = std::move(it->second);
        _tasks.erase(it);
    }

    if (wrapper->thread.joinable()) {
        wrapper->thread.join();
    }
}

TaskHandle LinuxPlatform::currentTask() {
    return g_currentTaskHandle;
}

void LinuxPlatform::taskDelay(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void LinuxPlatform::taskYield() {
    std::this_thread::yield();
}

// ========================================================================
// Synchronization
// ========================================================================

MutexHandle LinuxPlatform::createMutex() {
    auto handle = generateMutexHandle();
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        _mutexes[handle] = std::make_unique<PosixMutex>();
    }
    return handle;
}

void LinuxPlatform::deleteMutex(MutexHandle mutex) {
    std::lock_guard<std::mutex> lock(_syncMutex);
    _mutexes.erase(mutex);
}

util::Result<void> LinuxPlatform::lockMutex(MutexHandle mutex, uint32_t timeout_ms) {
    PosixMutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _mutexes.find(mutex);
        if (it == _mutexes.end()) {
            return util::ErrorCode::ResourceUnavailable;
        }
        m = it->second.get();
    }

    if (timeout_ms == 0) {
        return m->mutex.try_lock() ? util::Result<void>::ok()
                                   : util::Result<void>::err(util::ErrorCode::Timeout);
    }
    if (timeout_ms == UINT32_MAX) {
        m->mutex.lock();
        return util::Result<void>::ok();
    }

    return m->mutex.try_lock_for(std::chrono::milliseconds(timeout_ms))
        ? util::Result<void>::ok()
        : util::Result<void>::err(util::ErrorCode::Timeout);
}

void LinuxPlatform::unlockMutex(MutexHandle mutex) {
    PosixMutex* m = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _mutexes.find(mutex);
        if (it == _mutexes.end()) {
            return;
        }
        m = it->second.get();
    }

    m->mutex.unlock();
}

QueueHandle LinuxPlatform::createQueue(size_t itemSize, size_t itemCount) {
    auto handle = generateQueueHandle();
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        _queues[handle] = std::make_unique<PosixQueue>(itemSize, itemCount);
    }
    return handle;
}

void LinuxPlatform::deleteQueue(QueueHandle queue) {
    std::lock_guard<std::mutex> lock(_syncMutex);
    _queues.erase(queue);
}

util::Result<void> LinuxPlatform::queueSend(QueueHandle queue, const void* item, uint32_t timeoutMs) {
    if (!item) {
        return util::ErrorCode::InvalidParameter;
    }

    PosixQueue* q = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _queues.find(queue);
        if (it == _queues.end()) {
            return util::ErrorCode::ResourceUnavailable;
        }
        q = it->second.get();
    }

    std::unique_lock<std::mutex> qlock(q->mutex);

    auto hasSpace = [&q]() { return q->items.size() < q->itemCount; };
    if (timeoutMs == 0) {
        if (!hasSpace()) {
            return util::ErrorCode::Timeout;
        }
    } else if (timeoutMs == UINT32_MAX) {
        q->notFull.wait(qlock, hasSpace);
    } else {
        if (!q->notFull.wait_for(qlock, std::chrono::milliseconds(timeoutMs), hasSpace)) {
            return util::ErrorCode::Timeout;
        }
    }

    std::vector<uint8_t> data(static_cast<const uint8_t*>(item), 
                              static_cast<const uint8_t*>(item) + q->itemSize);
    q->items.push(data);
    q->notEmpty.notify_one();
    
    return util::Result<void>::ok();
}

util::Result<void> LinuxPlatform::queueReceive(QueueHandle queue, void* item, uint32_t timeoutMs) {
    if (!item) {
        return util::ErrorCode::InvalidParameter;
    }

    PosixQueue* q = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _queues.find(queue);
        if (it == _queues.end()) {
            return util::ErrorCode::ResourceUnavailable;
        }
        q = it->second.get();
    }

    std::unique_lock<std::mutex> qlock(q->mutex);

    auto hasItem = [&q]() { return !q->items.empty(); };
    if (timeoutMs == 0) {
        if (!hasItem()) {
            return util::ErrorCode::Timeout;
        }
    } else if (timeoutMs == UINT32_MAX) {
        q->notEmpty.wait(qlock, hasItem);
    } else {
        if (!q->notEmpty.wait_for(qlock, std::chrono::milliseconds(timeoutMs), hasItem)) {
            return util::ErrorCode::Timeout;
        }
    }

    if (q->items.empty()) {
        return util::ErrorCode::OperationFailed;
    }
    
    auto& data = q->items.front();
    std::memcpy(item, data.data(), data.size());
    q->items.pop();
    q->notFull.notify_one();
    
    return util::Result<void>::ok();
}

size_t LinuxPlatform::queueCount(QueueHandle queue) {
    PosixQueue* q = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _queues.find(queue);
        if (it == _queues.end()) {
            return 0;
        }
        q = it->second.get();
    }

    std::lock_guard<std::mutex> qlock(q->mutex);
    return q->items.size();
}

SemaphoreHandle LinuxPlatform::createBinarySemaphore() {
    auto handle = generateSemaphoreHandle();
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        _semaphores[handle] = std::make_unique<PosixSemaphore>(0);
    }
    return handle;
}

void LinuxPlatform::deleteSemaphore(SemaphoreHandle semaphore) {
    std::lock_guard<std::mutex> lock(_syncMutex);
    _semaphores.erase(semaphore);
}

util::Result<void> LinuxPlatform::semaphoreGive(SemaphoreHandle semaphore) {
    PosixSemaphore* sem = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _semaphores.find(semaphore);
        if (it == _semaphores.end()) {
            return util::ErrorCode::ResourceUnavailable;
        }
        sem = it->second.get();
    }

    std::unique_lock<std::mutex> mlock(sem->mutex);
    // Binary semaphore: saturate at 1
    sem->count = 1;
    sem->cv.notify_one();
    return util::Result<void>::ok();
}

util::Result<void> LinuxPlatform::semaphoreTake(SemaphoreHandle semaphore, uint32_t timeoutMs) {
    PosixSemaphore* sem = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _semaphores.find(semaphore);
        if (it == _semaphores.end()) {
            return util::ErrorCode::ResourceUnavailable;
        }
        sem = it->second.get();
    }

    std::unique_lock<std::mutex> mlock(sem->mutex);
    auto ready = [&sem]() { return sem->count > 0; };

    if (timeoutMs == 0) {
        if (!ready()) {
            return util::ErrorCode::Timeout;
        }
    } else if (timeoutMs == UINT32_MAX) {
        sem->cv.wait(mlock, ready);
    } else {
        if (!sem->cv.wait_for(mlock, std::chrono::milliseconds(timeoutMs), ready)) {
            return util::ErrorCode::Timeout;
        }
    }

    sem->count = 0;
    return util::Result<void>::ok();
}

EventGroupHandle LinuxPlatform::createEventGroup() {
    auto handle = generateEventGroupHandle();
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        _eventGroups[handle] = std::make_unique<PosixEventGroup>();
    }
    return handle;
}

void LinuxPlatform::deleteEventGroup(EventGroupHandle eventGroup) {
    std::lock_guard<std::mutex> lock(_syncMutex);
    _eventGroups.erase(eventGroup);
}

void LinuxPlatform::eventGroupSetBits(EventGroupHandle eventGroup, uint32_t bits) {
    PosixEventGroup* eg = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _eventGroups.find(eventGroup);
        if (it == _eventGroups.end()) {
            return;
        }
        eg = it->second.get();
    }

    std::unique_lock<std::mutex> mlock(eg->mutex);
    eg->bits |= bits;
    eg->cv.notify_all();
}

void LinuxPlatform::eventGroupClearBits(EventGroupHandle eventGroup, uint32_t bits) {
    PosixEventGroup* eg = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _eventGroups.find(eventGroup);
        if (it == _eventGroups.end()) {
            return;
        }
        eg = it->second.get();
    }

    std::unique_lock<std::mutex> mlock(eg->mutex);
    eg->bits &= ~bits;
}

uint32_t LinuxPlatform::eventGroupWaitBits(EventGroupHandle eventGroup, uint32_t bits,
                                            EventGroupClearMode clearOnExit, EventGroupWaitMode waitAll, uint32_t timeoutMs) {
    PosixEventGroup* eg = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _eventGroups.find(eventGroup);
        if (it == _eventGroups.end()) {
            return 0;
        }
        eg = it->second.get();
    }

    std::unique_lock<std::mutex> mlock(eg->mutex);
    
    auto condition = [&eg, bits, waitAll]() {
        if (waitAll == EventGroupWaitMode::All) {
            return (eg->bits & bits) == bits;
        } else {
            return (eg->bits & bits) != 0;
        }
    };
    
    if (timeoutMs == 0) {
        if (!condition()) {
            return 0;
        }
    } else if (timeoutMs == UINT32_MAX) {
        eg->cv.wait(mlock, condition);
    } else {
        if (!eg->cv.wait_for(mlock, std::chrono::milliseconds(timeoutMs), condition)) {
            return 0;
        }
    }
    
    uint32_t result = eg->bits;
    if (clearOnExit == EventGroupClearMode::Clear) {
        eg->bits &= ~bits;
    }
    
    return result;
}

util::Result<void> LinuxPlatform::taskNotifyGive(TaskHandle task) {
    if (task == nullptr) {
        task = currentTask();
    }
    if (task == nullptr) {
        return util::ErrorCode::OperationFailed;
    }

    TaskWrapper* wrapper = nullptr;
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        auto it = _tasks.find(task);
        if (it == _tasks.end()) {
            return util::ErrorCode::ResourceUnavailable;
        }
        wrapper = it->second.get();
    }

    std::unique_lock<std::mutex> nlock(wrapper->notification.mutex);
    wrapper->notification.value++;
    wrapper->notification.cv.notify_one();
    return util::Result<void>::ok();
}

util::Result<void> LinuxPlatform::taskNotifyGiveFromISR(TaskHandle task) {
    return taskNotifyGive(task);
}

uint32_t LinuxPlatform::taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms) {
    const TaskHandle task = currentTask();
    if (task == nullptr) {
        return 0;
    }

    TaskWrapper* wrapper = nullptr;
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        auto it = _tasks.find(task);
        if (it == _tasks.end()) {
            return 0;
        }
        wrapper = it->second.get();
    }

    std::unique_lock<std::mutex> nlock(wrapper->notification.mutex);
    auto ready = [&wrapper]() { return wrapper->notification.value > 0; };

    if (timeout_ms == 0) {
        if (!ready()) {
            return 0;
        }
    } else if (timeout_ms == UINT32_MAX) {
        wrapper->notification.cv.wait(nlock, ready);
    } else {
        if (!wrapper->notification.cv.wait_for(nlock, std::chrono::milliseconds(timeout_ms), ready)) {
            return 0;
        }
    }

    const uint32_t value = wrapper->notification.value;
    if (clearMode == TaskNotifyClearMode::Clear) {
        wrapper->notification.value = 0;
        return value;
    }

    wrapper->notification.value--;
    return 1;
}

// ========================================================================
// Hardware Interfaces
// ========================================================================

MemoryInterface& LinuxPlatform::memory() {
    return *_memory;
}

NetworkInterface* LinuxPlatform::network() {
    return _network.get();
}

UartInterface* LinuxPlatform::uart() {
    return _uart.get();
}

SpiInterface* LinuxPlatform::spi() {
    return _spi.get();
}

// ========================================================================
// Logging
// ========================================================================

void LinuxPlatform::log(const char* level, const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    std::printf("[%s] %s: ", level, tag);
    std::vprintf(format, args);
    std::printf("\n");
    
    va_end(args);
}

// ========================================================================
// Helper Methods
// ========================================================================

TaskHandle LinuxPlatform::generateTaskHandle() {
    std::lock_guard<std::mutex> lock(_syncMutex);
    return reinterpret_cast<TaskHandle>(static_cast<uintptr_t>(_nextTaskId++));
}

MutexHandle LinuxPlatform::generateMutexHandle() {
    std::lock_guard<std::mutex> lock(_syncMutex);
    return reinterpret_cast<MutexHandle>(static_cast<uintptr_t>(_nextHandleId++));
}

QueueHandle LinuxPlatform::generateQueueHandle() {
    std::lock_guard<std::mutex> lock(_syncMutex);
    return reinterpret_cast<QueueHandle>(static_cast<uintptr_t>(_nextHandleId++));
}

SemaphoreHandle LinuxPlatform::generateSemaphoreHandle() {
    std::lock_guard<std::mutex> lock(_syncMutex);
    return reinterpret_cast<SemaphoreHandle>(static_cast<uintptr_t>(_nextHandleId++));
}

EventGroupHandle LinuxPlatform::generateEventGroupHandle() {
    std::lock_guard<std::mutex> lock(_syncMutex);
    return reinterpret_cast<EventGroupHandle>(static_cast<uintptr_t>(_nextHandleId++));
}

} // namespace platform
} // namespace knx
