# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import ipaddress
from dataclasses import dataclass
from typing import Optional, Tuple


# Minimal KNXnet/IP framing helpers for the interop harness.
# Implements only what the harness needs: tunneling over UDP.

KNXNETIP_HEADER_LEN = 0x06
KNXNETIP_VERSION = 0x10

ST_CONNECTION_REQUEST = 0x0205
ST_CONNECTION_RESPONSE = 0x0206
ST_CONNECTIONSTATE_REQUEST = 0x0207
ST_CONNECTIONSTATE_RESPONSE = 0x0208
ST_DISCONNECT_REQUEST = 0x0209
ST_DISCONNECT_RESPONSE = 0x020A
ST_TUNNELING_REQUEST = 0x0420
ST_TUNNELING_ACK = 0x0421
ST_ROUTING_INDICATION = 0x0530

HPAI_LENGTH = 0x08
HPAI_PROTO_UDP = 0x01
HPAI_PROTO_TCP = 0x02

CRI_LENGTH = 0x04
CRI_CONN_TUNNEL = 0x04
CRI_LAYER_TP1 = 0x02

TUNNELING_HEADER_LENGTH = 0x04

E_NO_ERROR = 0x00


@dataclass(frozen=True)
class Hpai:
    ip: str
    port: int


@dataclass(frozen=True)
class KnxIpHeader:
    service_type: int
    total_length: int


def _u16be(b: bytes, off: int) -> int:
    return (b[off] << 8) | b[off + 1]


def _p16be(v: int) -> bytes:
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


def parse_header(pkt: bytes) -> KnxIpHeader:
    if len(pkt) < 6:
        raise ValueError("packet too short")
    if pkt[0] != KNXNETIP_HEADER_LEN or pkt[1] != KNXNETIP_VERSION:
        raise ValueError("not a KNXnet/IP frame")
    st = _u16be(pkt, 2)
    total = _u16be(pkt, 4)
    if total != len(pkt):
        # Be tolerant: some stacks might pad; accept as long as header claims <= actual.
        if total > len(pkt):
            raise ValueError("invalid total length")
    return KnxIpHeader(service_type=st, total_length=total)


def build_header(service_type: int, body_len: int) -> bytearray:
    total = KNXNETIP_HEADER_LEN + body_len
    hdr = bytearray(6)
    hdr[0] = KNXNETIP_HEADER_LEN
    hdr[1] = KNXNETIP_VERSION
    hdr[2:4] = _p16be(service_type)
    hdr[4:6] = _p16be(total)
    return hdr


def _parse_hpai(pkt: bytes, off: int) -> Optional[Hpai]:
    if len(pkt) < off + 8:
        return None
    if pkt[off] != HPAI_LENGTH:
        return None
    # Some KNXnet/IP interactions use UDP (0x01) while KNX/IP Secure tunneling uses TCP (0x02).
    if pkt[off + 1] not in (HPAI_PROTO_UDP, HPAI_PROTO_TCP):
        return None
    ip = str(ipaddress.IPv4Address(pkt[off + 2 : off + 6]))
    port = _u16be(pkt, off + 6)
    return Hpai(ip=ip, port=port)


def parse_connect_request(pkt: bytes) -> Tuple[Optional[Hpai], Optional[Hpai]]:
    """Return (ctrl_hpai, data_hpai) if present."""
    # Body: HPAI ctrl (8) + HPAI data (8) + CRI (4)
    ctrl = _parse_hpai(pkt, 6)
    data = _parse_hpai(pkt, 14)
    return ctrl, data


def build_connect_response(channel_id: int, status: int, data_endpoint: Hpai, *, hpai_proto: int = HPAI_PROTO_UDP) -> bytes:
    # We include: [channel(1)][status(1)][HPAI data endpoint (8)][CRD (4)]
    body = bytearray()
    body.append(channel_id & 0xFF)
    body.append(status & 0xFF)

    ip = ipaddress.IPv4Address(data_endpoint.ip)
    body.extend(
        bytes(
            [
                HPAI_LENGTH,
                hpai_proto & 0xFF,
                *ip.packed,
                (data_endpoint.port >> 8) & 0xFF,
                data_endpoint.port & 0xFF,
            ]
        )
    )

    # CRD: same format as CRI in our client code.
    body.extend(bytes([CRI_LENGTH, CRI_CONN_TUNNEL, CRI_LAYER_TP1, 0x00]))

    pkt = build_header(ST_CONNECTION_RESPONSE, len(body))
    pkt.extend(body)
    return bytes(pkt)


def parse_connectionstate_request(pkt: bytes) -> Tuple[Optional[int], Optional[Hpai]]:
    # Body: [channel(1)][reserved(1)][HPAI ctrl (8)]
    if len(pkt) < 8:
        return None, None
    ch = pkt[6]
    ctrl = _parse_hpai(pkt, 8)
    return ch, ctrl


def build_connectionstate_response(channel_id: int, status: int) -> bytes:
    body = bytes([channel_id & 0xFF, status & 0xFF])
    pkt = build_header(ST_CONNECTIONSTATE_RESPONSE, len(body))
    pkt.extend(body)
    return bytes(pkt)


def parse_disconnect_request(pkt: bytes) -> Tuple[Optional[int], Optional[Hpai]]:
    # Body: [channel(1)][reserved(1)][HPAI ctrl (8)]
    if len(pkt) < 8:
        return None, None
    ch = pkt[6]
    ctrl = _parse_hpai(pkt, 8)
    return ch, ctrl


def build_disconnect_response(channel_id: int, status: int) -> bytes:
    body = bytes([channel_id & 0xFF, status & 0xFF])
    pkt = build_header(ST_DISCONNECT_RESPONSE, len(body))
    pkt.extend(body)
    return bytes(pkt)


@dataclass(frozen=True)
class TunnelingRequest:
    channel_id: int
    seq: int
    status: int
    cemi: bytes


def parse_tunneling_request(pkt: bytes) -> Optional[TunnelingRequest]:
    if len(pkt) < 10:
        return None
    hdr_len = pkt[6]
    if hdr_len != TUNNELING_HEADER_LENGTH:
        return None
    ch = pkt[7]
    seq = pkt[8]
    status = pkt[9]
    cemi = pkt[10:]
    return TunnelingRequest(channel_id=ch, seq=seq, status=status, cemi=cemi)


def build_tunneling_ack(channel_id: int, seq: int, status: int = E_NO_ERROR) -> bytes:
    body = bytes([TUNNELING_HEADER_LENGTH, channel_id & 0xFF, seq & 0xFF, status & 0xFF])
    pkt = build_header(ST_TUNNELING_ACK, len(body))
    pkt.extend(body)
    return bytes(pkt)


def build_tunneling_request(channel_id: int, seq: int, cemi: bytes, status: int = 0x00) -> bytes:
    body = bytearray([TUNNELING_HEADER_LENGTH, channel_id & 0xFF, seq & 0xFF, status & 0xFF])
    body.extend(cemi)
    pkt = build_header(ST_TUNNELING_REQUEST, len(body))
    pkt.extend(body)
    return bytes(pkt)


def parse_routing_indication(pkt: bytes) -> bytes:
    hdr = parse_header(pkt)
    if hdr.service_type != ST_ROUTING_INDICATION:
        raise ValueError("not a routing indication")
    # Body is the raw cEMI frame.
    if len(pkt) < 7:
        raise ValueError("routing indication too short")
    return pkt[6:hdr.total_length]


def build_routing_indication(cemi: bytes) -> bytes:
    pkt = build_header(ST_ROUTING_INDICATION, len(cemi))
    pkt.extend(cemi)
    return bytes(pkt)
