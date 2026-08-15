// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file ip_physical_layer.cpp
 * @brief KNXnet/IP physical layer implementation
 */

#include "knx/physical/ip_physical_layer.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include "knx/util/timing_utils.hpp"
#include <cstring>
#include <vector>
#include <span>

#include "knx/netip/netip_config.hpp"

static const char* TAG = "KNX.IP";

namespace knx {
namespace physical {

IpPhysical::IpPhysical()
    : _bindPort(knx::netip::config::kDefaultPort)
    , _bindAddress(IpAddress::fromOctets(0, 0, 0, 0))
    , _mcastInterface(IpAddress::fromOctets(0, 0, 0, 0))
{}

IpPhysical::~IpPhysical() {
    close();
}

void IpPhysical::setNetworkInterface(platform::NetworkInterface* network) {
    if (_initialized) return;
    _network = network;
}

void IpPhysical::setTimingPlatform(platform::TimingPlatform* timingPlatform) {
    if (_initialized) return;
    _timingPlatform = timingPlatform;
}

util::Result<void> IpPhysical::init() {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }

    if (!_network) {
        KNX_LOGE(TAG, "NetworkInterface not set");
        _state = IpPhysicalLayerState::Error;
        return util::ErrorCode::NotInitialized;
    }

    _sock = _network->createUdpSocket();
    if (!_sock) {
        KNX_LOGE(TAG, "Failed to create UDP socket");
        _state = IpPhysicalLayerState::Error;
        return util::ErrorCode::ResourceUnavailable;
    }

    auto openRes = _sock->open(_bindPort);
    if (openRes.isError()) {
        KNX_LOGE(TAG, "UDP open/bind failed: %s", util::errorCodeToString(openRes.error()));
        _sock.reset();
        _state = IpPhysicalLayerState::Error;
        return openRes.error();
    }

    // Join KNX multicast group (optional for routing)
    const IpAddress group = IpAddress::fromOctets(224, 0, 23, 12);
    const IpAddress iface = _mcastInterface;
    if (!group.isZero()) {
        (void)_sock->joinMulticast(group, iface);
    }

    _initialized = true;
    char bindBuf[16];
    _bindAddress.toString(bindBuf);
    KNX_LOGI(TAG, "IP physical layer initialized (bind %s:%u)", bindBuf, _bindPort);
    return util::Result<void>::ok();
}

void IpPhysical::close() {
    if (_initialized) {
        if (_sock) {
            _sock->close();
            _sock.reset();
        }
        _initialized = false;
    }
}

bool IpPhysical::isOpen() const {
    return _initialized;
}

util::Result<size_t> IpPhysical::sendFrame(std::span<const uint8_t> frame,
                                           IpAddress remoteIp,
                                           uint16_t remotePort) {
    auto beginResult = beginTransmit(frame, remoteIp, remotePort);
    if (beginResult.isError()) return beginResult.error();

    auto progress = pollTransmit();
    if (progress.isError()) return progress.error();
    if (progress.value() != ProgressState::Success) {
        if (progress.value() == ProgressState::Busy) return util::ErrorCode::Busy;
        if (progress.value() == ProgressState::Timeout) return util::ErrorCode::Timeout;
        return util::ErrorCode::TransmissionFailed;
    }

    char destBuf[16];
    remoteIp.toString(destBuf);
    KNX_LOGD(TAG, "Frame sent to %s:%u (%zu bytes)", destBuf, remotePort, frame.size());
    return frame.size();
}

util::Result<void> IpPhysical::beginTransmit(std::span<const uint8_t> frame,
                                             IpAddress remoteIp,
                                             uint16_t remotePort) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (frame.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    if (remoteIp.isZero()) {
        KNX_LOGE(TAG, "Invalid remote IP");
        return util::ErrorCode::InvalidAddress;
    }
    if (_txActive) {
        return util::ErrorCode::Busy;
    }

    _state = IpPhysicalLayerState::Transmitting;
    _txFrame.assign(frame.begin(), frame.end());
    _txRemoteIp = remoteIp;
    _txRemotePort = remotePort;
    _txActive = true;
    _txStarted = false;
    return util::Result<void>::ok();
}

util::Result<IpPhysical::ProgressState> IpPhysical::pollTransmit() {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_txActive) {
        return util::ErrorCode::OperationNotReady;
    }

    if (!_txStarted) {
        const int sent = _sock->send(_txRemoteIp, _txRemotePort, _txFrame);
        _txStarted = true;
        _txActive = false;
        _state = IpPhysicalLayerState::Idle;
        if (sent < 0 || static_cast<size_t>(sent) != _txFrame.size()) {
            KNX_LOGE(TAG, "UDP send failed");
            _state = IpPhysicalLayerState::Error;
            return ProgressState::TransmissionFailed;
        }
        return ProgressState::Success;
    }

    _txActive = false;
    _state = IpPhysicalLayerState::Idle;
    return ProgressState::Success;
}

util::Result<void> IpPhysical::beginReceive(uint32_t timeoutMs) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (_rxActive) {
        return util::ErrorCode::Busy;
    }

    _rxActive = true;
    _rxFrameLength = 0;
    _state = IpPhysicalLayerState::Receiving;
    _rxDeadlineMs = static_cast<uint64_t>(util::nowMs(_timingPlatform)) + timeoutMs;
    return util::Result<void>::ok();
}

util::Result<IpPhysical::ProgressState> IpPhysical::pollReceive() {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_rxActive) {
        return util::ErrorCode::OperationNotReady;
    }

    if (_sock->available() == 0) {
        const auto nowMs = static_cast<uint64_t>(util::nowMs(_timingPlatform));
        if (nowMs >= _rxDeadlineMs) {
            _rxActive = false;
            _state = IpPhysicalLayerState::Idle;
            return ProgressState::Timeout;
        }
        return ProgressState::Pending;
    }

    const int recvd = _sock->receive(_rxFrame);
    if (recvd <= 0) {
        _rxActive = false;
        _state = IpPhysicalLayerState::Idle;
        return ProgressState::TransmissionFailed;
    }

    _rxFrameLength = static_cast<size_t>(recvd);
    _rxActive = false;
    _state = IpPhysicalLayerState::Idle;
    if (_rxCallback) {
        _rxCallback(_rxCallbackContext);
    }
    return ProgressState::Complete;
}

util::Result<std::span<const uint8_t>> IpPhysical::receivedFrameView() {
    if (_rxFrameLength == 0) {
        return util::ErrorCode::OperationNotReady;
    }
    return std::span<const uint8_t>(_rxFrame.data(), _rxFrameLength);
}

void IpPhysical::setReceiveCallback(IpReceiveCallback callback, void* context) {
    _rxCallback = std::move(callback);
    _rxCallbackContext = context;
}

IpPhysicalLayerState IpPhysical::getState() const {
    return _state;
}

void IpPhysical::setBindAddressPort(IpAddress address, uint16_t port) {
    if (_initialized) return; // ignore after init
    _bindAddress = address;
    _bindPort = port ? port : _bindPort;
}

void IpPhysical::setMulticastInterface(IpAddress interfaceAddress) {
    if (_initialized) return; // ignore after init
    _mcastInterface = interfaceAddress;
}

} // namespace physical
} // namespace knx
