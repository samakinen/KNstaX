// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/tunneling_server_endpoint.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace knx {
namespace network {

/**
 * @brief The cEMI server of a KNXnet/IP interface: connects the tunnelling
 *        connections of a TunnelingServerEndpoint to a TP1 line.
 *
 * Each connection is a device of its own on the line, with the Individual
 * Address the server assigned to it (03_08_04 §2.2.2):
 *
 * - init() publishes every tunnel address to the line, which acknowledges
 *   frames addressed to them on the medium and accepts them in reception.
 * - An L_Data.req whose Source Address is 0000h goes onto TP1 with the
 *   connection's tunnel address; any other source is sent unchanged
 *   (03_08_04 §2.2.2, 03_06_03 §4.1.5.3.3).
 * - Once the TP1 transmission ends the client gets an L_Data.con: Confirm
 *   flag 0 when the data link layer reports success, 1 when the frame was
 *   NAKed, refused BUSY or left unacknowledged after the repetitions
 *   (03_06_03 §4.1.5.3.4). Requests are sent one at a time in arrival order
 *   (03_06_03 §4.1.5.1, FIFO).
 * - A frame received from TP1 becomes an L_Data.ind: group-addressed frames
 *   to every connection, point-to-point frames only to the connection whose
 *   tunnel address is the destination (03_08_04 §2.2.2). A frame one
 *   connection sent successfully is indicated to the other connections by
 *   the same rule, as the line would show it to another device.
 *
 * Threading: the tunnelling side (the server's poll() and this poll()) runs
 * on one task. TP1 indications may arrive on the data link's RX task; they
 * are queued and handed to the server in poll().
 */
class Tp1IpInterfaceBridge {
public:
    struct Statistics {
        uint64_t tp1ToIpForwarded{0};
        uint64_t ipToTp1Forwarded{0};
        uint64_t tp1ToIpDropped{0};
        uint64_t ipToTp1Dropped{0};
        uint64_t positiveConfirmations{0};
        uint64_t negativeConfirmations{0};
    };

    /// L_Data.req frames buffered while one is on the line.
    static constexpr size_t kRequestQueueDepth = 8;
    /// TP1 frames buffered between the RX task and poll().
    static constexpr size_t kIndicationQueueDepth = 16;

    Tp1IpInterfaceBridge(datalink::Tp1DataLinkLayer& tp1Line,
                         netip::TunnelingServerEndpoint& tunnelingServer);

    Tp1IpInterfaceBridge(const Tp1IpInterfaceBridge&) = delete;
    Tp1IpInterfaceBridge& operator=(const Tp1IpInterfaceBridge&) = delete;

    /// Open the tunnelling server first: its tunnel addresses are published
    /// to the TP1 line here.
    util::Result<void> init();
    void close();

    /// Drive the bridge: hand queued TP1 frames to the clients, run the TP1
    /// transmission of the pending L_Data.req and confirm it. Call it on the
    /// task that polls the tunnelling server, after each server poll().
    void poll();

    Statistics statistics() const;

private:
    struct PendingRequest {
        ChannelId channelId{ChannelId::invalid()};
        datalink::LDataFrame frame{};
    };

    void onTp1Frame(const datalink::LDataFrame& frame);
    void onTunnelingCemi(ChannelId channelId, std::span<const uint8_t> cemi);

    void drainIndications();
    void pumpTransmit();
    void finishRequest(bool success);
    void indicate(const datalink::LDataFrame& frame, ChannelId exceptChannel);
    void sendConfirmation(const PendingRequest& request, bool success);

    datalink::Tp1DataLinkLayer& tp1Line_;
    netip::TunnelingServerEndpoint& tunnelingServer_;

    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> tp1ToIpForwarded_{0};
    std::atomic<uint64_t> ipToTp1Forwarded_{0};
    std::atomic<uint64_t> tp1ToIpDropped_{0};
    std::atomic<uint64_t> ipToTp1Dropped_{0};
    std::atomic<uint64_t> positiveConfirmations_{0};
    std::atomic<uint64_t> negativeConfirmations_{0};

    // L_Data.req FIFO; tunnelling task only.
    std::array<PendingRequest, kRequestQueueDepth> requests_{};
    size_t requestHead_{0};
    size_t requestCount_{0};
    bool transmitting_{false};

    // TP1 -> IP single-producer (RX task) / single-consumer (poll) ring.
    std::array<datalink::LDataFrame, kIndicationQueueDepth> indications_{};
    std::atomic<size_t> indicationHead_{0};
    std::atomic<size_t> indicationTail_{0};
};

} // namespace network
} // namespace knx
