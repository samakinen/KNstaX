# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import errno
import ipaddress
import socket
import struct
from dataclasses import dataclass
from typing import Optional, Tuple

from . import knxip_codec


@dataclass
class RxRouting:
    cemi: bytes
    src: Tuple[str, int]


class KnxIpRoutingClient(asyncio.DatagramProtocol):
    """Minimal KNXnet/IP Routing (multicast) client.

    Receives/sends ROUTING_INDICATION (0x0530) frames.
    """

    def __init__(self) -> None:
        self.transport: Optional[asyncio.DatagramTransport] = None
        self.group: str = "224.0.23.12"
        self.port: int = 3671
        self.interface_ip: str = "0.0.0.0"
        self._rx_queue: asyncio.Queue[RxRouting] = asyncio.Queue()

    async def start(self, *, group: str, port: int, interface_ip: str = "0.0.0.0") -> None:
        self.group = group
        self.port = port
        self.interface_ip = interface_ip

        loop = asyncio.get_running_loop()

        # For multicast routing, multiple processes typically bind the same UDP port.
        # Create the socket ourselves so we can set reuse options *before* bind.
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except OSError:
                # Not supported/enabled on all kernels.
                pass
            last_exc: OSError | None = None
            for attempt in range(3):
                try:
                    sock.bind(("0.0.0.0", port))
                    last_exc = None
                    break
                except OSError as exc:
                    last_exc = exc
                    if exc.errno == errno.EADDRINUSE and attempt + 1 < 3:
                        await asyncio.sleep(0.05)
                        continue
                    raise
            else:
                if last_exc is not None:
                    raise last_exc

        transport, _proto = await loop.create_datagram_endpoint(lambda: self, sock=sock)
        self.transport = transport

        # Join multicast group.
        mcast = ipaddress.IPv4Address(group).packed
        iface = ipaddress.IPv4Address(interface_ip).packed
        mreq = mcast + iface
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

        # Configure egress interface and loopback so local sender receives on same host.
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, iface)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)

    async def stop(self) -> None:
        if self.transport:
            self.transport.close()
            self.transport = None

    def datagram_received(self, data: bytes, addr: Tuple[str, int]) -> None:
        try:
            hdr = knxip_codec.parse_header(data)
        except Exception:
            return
        if hdr.service_type != knxip_codec.ST_ROUTING_INDICATION:
            return
        try:
            cemi = knxip_codec.parse_routing_indication(data)
        except Exception:
            return
        self._rx_queue.put_nowait(RxRouting(cemi=cemi, src=addr))

    def send_cemi(self, cemi: bytes) -> None:
        if not self.transport:
            raise RuntimeError("client not started")
        pkt = knxip_codec.build_routing_indication(cemi)
        self.transport.sendto(pkt, (self.group, self.port))

    async def recv_routing(self, timeout_s: float = 3.0) -> RxRouting:
        return await asyncio.wait_for(self._rx_queue.get(), timeout=timeout_s)
