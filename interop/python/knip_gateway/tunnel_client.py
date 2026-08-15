# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import socket
from dataclasses import dataclass
from typing import Optional, Tuple

from . import knxip_codec


@dataclass
class RxTunneling:
    cemi: bytes
    src: Tuple[str, int]
    channel_id: int
    seq: int


class KnxIpTunnelingClient(asyncio.DatagramProtocol):
    """Minimal KNXnet/IP UDP tunneling client.

    Designed for deterministic interop tests and to avoid depending on any 3rd-party stack.
    Implements: CONNECT, CONNECTIONSTATE (optional), DISCONNECT, send/receive TUNNELING_REQUEST/ACK.
    """

    def __init__(self) -> None:
        self.transport: Optional[asyncio.DatagramTransport] = None
        self.local_addr: Optional[Tuple[str, int]] = None
        self.remote: Optional[Tuple[str, int]] = None
        self.channel_id: int = 0
        self._connect_fut: Optional[asyncio.Future[None]] = None
        self._disconnect_fut: Optional[asyncio.Future[None]] = None
        self._rx_queue: asyncio.Queue[RxTunneling] = asyncio.Queue()
        self._seq_out: int = 0

    async def start(self, gateway: Tuple[str, int], local_ip: str = "127.0.0.1") -> None:
        self.remote = gateway
        loop = asyncio.get_running_loop()
        self._connect_fut = loop.create_future()
        transport, _proto = await loop.create_datagram_endpoint(
            lambda: self,
            local_addr=(local_ip, 0),
        )
        self.transport = transport
        assert self.transport is not None
        sock: socket.socket = self.transport.get_extra_info("socket")
        self.local_addr = sock.getsockname()

        # Send CONNECT_REQUEST matching KNstaX tunneling.cpp format.
        pkt = self._build_connect_request()
        self.transport.sendto(pkt, gateway)

        await asyncio.wait_for(self._connect_fut, timeout=3.0)

    async def stop(self) -> None:
        if not self.transport or not self.remote or self.channel_id == 0:
            if self.transport:
                self.transport.close()
            return

        loop = asyncio.get_running_loop()
        self._disconnect_fut = loop.create_future()
        pkt = self._build_disconnect_request()
        self.transport.sendto(pkt, self.remote)

        try:
            await asyncio.wait_for(self._disconnect_fut, timeout=1.0)
        except TimeoutError:
            pass
        finally:
            self.transport.close()

    def datagram_received(self, data: bytes, addr: Tuple[str, int]) -> None:
        try:
            hdr = knxip_codec.parse_header(data)
        except Exception:
            return

        # Minimal diagnostics for debugging handshake issues.
        try:
            print(f"CLIENT: rx st=0x{hdr.service_type:04x} from {addr} len={len(data)}")
        except Exception:
            pass

        if hdr.service_type == knxip_codec.ST_CONNECTION_RESPONSE:
            # Minimal parse used by KNstaX: channel at byte 6, status at byte 7.
            if len(data) >= 8:
                ch = data[6]
                status = data[7]
                if status == knxip_codec.E_NO_ERROR and ch != 0:
                    self.channel_id = ch
                    if self._connect_fut and not self._connect_fut.done():
                        self._connect_fut.set_result(None)
            return

        if hdr.service_type == knxip_codec.ST_DISCONNECT_RESPONSE:
            if self._disconnect_fut and not self._disconnect_fut.done():
                self._disconnect_fut.set_result(None)
            return

        if hdr.service_type == knxip_codec.ST_TUNNELING_ACK:
            # We currently do not track outgoing acks in the client.
            return

        if hdr.service_type == knxip_codec.ST_TUNNELING_REQUEST:
            req = knxip_codec.parse_tunneling_request(data)
            if not req:
                return
            # Ack back (best-effort).
            if self.transport and self.remote:
                self.transport.sendto(knxip_codec.build_tunneling_ack(req.channel_id, req.seq), self.remote)

            self._rx_queue.put_nowait(RxTunneling(cemi=req.cemi, src=addr, channel_id=req.channel_id, seq=req.seq))
            return

    async def recv_tunneling(self, timeout_s: float = 3.0) -> RxTunneling:
        return await asyncio.wait_for(self._rx_queue.get(), timeout=timeout_s)

    def send_cemi(self, cemi: bytes) -> None:
        if not self.transport or not self.remote:
            raise RuntimeError("client not started")
        if self.channel_id == 0:
            raise RuntimeError("not connected")
        pkt = knxip_codec.build_tunneling_request(self.channel_id, self._seq_out, cemi)
        self._seq_out = (self._seq_out + 1) & 0xFF
        self.transport.sendto(pkt, self.remote)

    def _build_connect_request(self) -> bytes:
        assert self.local_addr is not None
        ip, port = self.local_addr[0], self.local_addr[1]
        ctrl = knxip_codec.Hpai(ip=ip, port=port)
        data = knxip_codec.Hpai(ip=ip, port=port)

        body = bytearray()

        def append_hpai(h: knxip_codec.Hpai) -> None:
            ipb = bytes(map(int, h.ip.split(".")))
            body.extend(
                bytes(
                    [
                        knxip_codec.HPAI_LENGTH,
                        knxip_codec.HPAI_PROTO_UDP,
                        *ipb,
                        (h.port >> 8) & 0xFF,
                        h.port & 0xFF,
                    ]
                )
            )

        append_hpai(ctrl)
        append_hpai(data)
        body.extend(bytes([knxip_codec.CRI_LENGTH, knxip_codec.CRI_CONN_TUNNEL, knxip_codec.CRI_LAYER_TP1, 0x00]))

        pkt = knxip_codec.build_header(knxip_codec.ST_CONNECTION_REQUEST, len(body))
        pkt.extend(body)
        return bytes(pkt)

    def _build_disconnect_request(self) -> bytes:
        assert self.local_addr is not None
        ip, port = self.local_addr[0], self.local_addr[1]

        body = bytearray([self.channel_id & 0xFF, 0x00])
        ipb = bytes(map(int, ip.split(".")))
        body.extend(
            bytes(
                [
                    knxip_codec.HPAI_LENGTH,
                    knxip_codec.HPAI_PROTO_UDP,
                    *ipb,
                    (port >> 8) & 0xFF,
                    port & 0xFF,
                ]
            )
        )

        pkt = knxip_codec.build_header(knxip_codec.ST_DISCONNECT_REQUEST, len(body))
        pkt.extend(body)
        return bytes(pkt)
