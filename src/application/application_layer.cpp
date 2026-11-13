// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file application_layer.cpp
 * @brief KNX Application Layer - receive, filter and route
 */

#include "knx/application/application_layer.hpp"
#include "knx/application/communication_mode.hpp"
#include "knx/application/security_access_policy.hpp"
#include "knx/config.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/hex.hpp"
#include "knx/util/log.hpp"

static const char* TAG = "KNX.App.Layer";

namespace knx {
namespace application {

namespace {

/// The communication modes a received frame may have travelled on.
///
/// A set rather than one mode because T_Data_Broadcast and
/// T_Data_SystemBroadcast both arrive addressed to 0000h, and what tells them
/// apart (the TP1 control field, or ROUTING_SYSTEM_BROADCAST on IP) is
/// consumed below the transport layer. Accepting either keeps every
/// broadcast-mode service working whichever of the two a tool chose; what
/// matters for P3 is that point-to-point and group frames are told apart from
/// broadcasts, and that is preserved.
CommunicationModes receivedCommunicationModes(const transport::TDataFrame& frame)
{
    switch (frame.service) {
        case transport::TDataService::Connected:
            return modes(CommunicationMode::Connected);
        case transport::TDataService::Individual:
            return modes(CommunicationMode::Connectionless);
        case transport::TDataService::Group:
            break;
    }
    if (frame.destination.raw == 0u) {
        return CommunicationMode::Broadcast | CommunicationMode::SystemBroadcast;
    }
    return modes(CommunicationMode::Multicast);
}

} // namespace

ApplicationLayer::ApplicationLayer(transport::TransportLayer& transport)
    : _transport(transport)
    , _send(transport)
    , _management(_send)
{
}

ApplicationLayer::~ApplicationLayer() {
    close();
}

util::Result<void> ApplicationLayer::init(const IndividualAddress& ownAddress) {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }

    _send.open(ownAddress);
    _management.init();

    _transport.setReceiveCallback([this](const transport::TDataFrame& frame) {
        handleTransportRx(frame);
    });

    // 03/03/07 §3.5.7: "A current access level shall be valid until the
    // connection is released". Without this a partner that authorised once
    // kept its level on every later connection from the same address.
    _transport.setConnectionClosedCallback([this](const IndividualAddress& partner) {
        _management.connectionClosed(partner);
    });

    _initialized = true;
    KNX_LOGD(TAG, "Application layer initialized");

    return util::Result<void>::ok();
}

void ApplicationLayer::close() {
    if (_initialized) {
        // Prevent transport from calling back into a destroyed ApplicationLayer.
        _transport.setReceiveCallback(nullptr);
        _transport.setConnectionClosedCallback(nullptr);
        _send.close();
        _initialized = false;
        KNX_LOGI(TAG, "Application layer closed");
    }
}

void ApplicationLayer::forwardToUser(const ADataFrame& frame) {
    // Already logged once by the "Application RX service=..." line.
    if (_rxCallback) {
        _rxCallback(frame.source,
                    frame.destination,
                    frame.service,
                    frame.data,
                    frame.destinationType);
        if (_receiveWorkAvailableCallback) {
            _receiveWorkAvailableCallback();
        }
    }
}

void ApplicationLayer::handleTransportRx(const transport::TDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGW(TAG, "Application layer not initialized, dropping transport frame src=0x%04X dst=0x%04X",
                 frame.source.raw,
                 frame.destination.raw);
        return;
    }

    if (frame.tpdu.size() < 2) {
        KNX_LOGW(TAG,
                 "Received TPDU too short for APCI header from src=0x%04X dst=0x%04X",
                 frame.source.raw,
                 frame.destination.raw);
        return;
    }

    const auto header = knx::protocol::unpackTpduHeader(frame.tpdu[0], frame.tpdu[1]);
    application::APCIField apciValue = header.apci;
    APCIService service = apciValue.service();

    // 03/03/07 Table 1: a service that arrives on a communication mode it is
    // not defined for is ignored, before any handler sees it. Without this an
    // A_Memory_Write or A_PropertyValue_Write sent to a group address, or as a
    // broadcast, reached the same handlers as one sent on a connection.
    //
    // For a KNX Data Secure frame this checks the inner, decrypted service
    // against the mode the outer S-A_Data frame arrived on: the RX transform
    // replaces the TPDU in place and leaves the transport service and the
    // destination alone, which is the mapping §5 prescribes — the S-AL sits
    // inside the Application Layer and does not change the communication mode.
    const CommunicationModes receivedModes = receivedCommunicationModes(frame);
    if (!isCommunicationModeAllowed(service, receivedModes)) {
        KNX_LOGW(TAG,
                 "Service 0x%03X from 0x%04X to 0x%04X ignored: not defined on this communication "
                 "mode (03/03/07 Table 1)",
                 static_cast<uint16_t>(service),
                 frame.source.raw,
                 frame.destination.raw);
        return;
    }

    // Service data is APDU payload bytes (after 2-byte header).
    // For services where data6 belongs to the service payload, prepend it first
    // so handlers receive a contiguous byte sequence.
    util::FixedVector<uint8_t, config::MAX_APDU_LENGTH> serviceData;
    const bool needsData6 = usesApciData6(service);
    if (needsData6) {
        serviceData.push_back(apciValue.data6());
    }
    if (frame.tpdu.size() > 2) {
        for (size_t i = 2; i < frame.tpdu.size(); ++i) {
            (void)serviceData.push_back(frame.tpdu[i]);
        }
    }

    // GroupValue short APDU carries the 1-byte value in APCI data6 (even if 0).
    // If a payload exists, it is the long APDU form and the payload bytes are used.
    if (service == APCIService::GroupValueWrite || service == APCIService::GroupValueResponse) {
        if (serviceData.empty()) {
            serviceData.push_back(apciValue.data6());
        }
    }

    // Create application frame
    ADataFrame aFrame;
    aFrame.source = frame.source;
    aFrame.destination = frame.destination;
    aFrame.service = service;
    aFrame.data = serviceData;
    aFrame.destinationType = frame.destinationType;
    aFrame.apciData = apciValue.data6();
    aFrame.security = frame.security;

    KNX_LOGD(TAG, "Application RX service=0x%03X src=0x%04X dst=0x%04X data=%s",
             static_cast<uint16_t>(aFrame.service),
             aFrame.source.raw,
             aFrame.destination.raw,
             util::toHex(aFrame.data).c_str());

    if (!isRequestPermitted(aFrame)) {
        return;
    }

    // Answers raised while serving the request leave on the transport service
    // the request arrived on, with the request's security deciding the Access
    // Policy questions on the way. The context lives on this stack frame, so
    // a nested request gets its own.
    const RequestContext context{
        .source = aFrame.source,
        .connected = (frame.service == transport::TDataService::Connected),
        .security = aFrame.security,
    };
    if (!_management.handle(aFrame, context)) {
        forwardToUser(aFrame);
    }
}

bool ApplicationLayer::isRequestPermitted(const ADataFrame& aFrame) const {
    // Group communication is the common case and is gated by the Group Object
    // Security Flags rather than by this policy, so it never reaches the
    // provider call.
    if (!isManagementWriteService(aFrame.service)) {
        return true;
    }

    const bool securityModeEnabled = _securityModeProvider && _securityModeProvider();
    if (managementWritePermitted(aFrame.service, securityModeEnabled, aFrame.security)) {
        return true;
    }

    // Dropped without an answer. 03/4/1 §6.2.2 defines a Permission as whether
    // the indication "shall be accepted", and an error response would both
    // confirm the device is there and tell an unauthorised sender which
    // services it implements.
    KNX_LOGW(TAG,
             "Service 0x%03X from 0x%04X refused: security mode is on and the request is %s",
             static_cast<uint16_t>(aFrame.service),
             aFrame.source.raw,
             aFrame.security.secured ? "not tool-secured" : "not secured");
    return false;
}

util::Result<void> ApplicationLayer::sendGroupValueWrite(const GroupAddress& destination,
                                                         std::span<const uint8_t> data) {
    return _send.sendData(destination, APCIService::GroupValueWrite, data, AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::beginSendGroupValueWrite(const GroupAddress& destination,
                                                              std::span<const uint8_t> data) {
    return _send.beginSendData(destination, APCIService::GroupValueWrite, data, AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::sendGroupValueRead(const GroupAddress& destination) {
    return _send.sendData(destination, APCIService::GroupValueRead, std::span<const uint8_t>{}, AddressType::Group,
                          _defaultSendOptions);
}

util::Result<void> ApplicationLayer::beginSendGroupValueRead(const GroupAddress& destination) {
    return _send.beginSendData(destination, APCIService::GroupValueRead, std::span<const uint8_t>{},
                               AddressType::Group, _defaultSendOptions);
}

util::Result<void> ApplicationLayer::sendGroupValueResponse(const GroupAddress& destination,
                                                            std::span<const uint8_t> data) {
    return _send.sendData(destination, APCIService::GroupValueResponse, data, AddressType::Group,
                          _defaultSendOptions);
}

util::Result<void> ApplicationLayer::beginSendGroupValueResponse(const GroupAddress& destination,
                                                                 std::span<const uint8_t> data) {
    return _send.beginSendData(destination, APCIService::GroupValueResponse, data, AddressType::Group,
                               _defaultSendOptions);
}

} // namespace application
} // namespace knx
