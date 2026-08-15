# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple


# Minimal cEMI L_Data encoder/decoder aligned with src/netip/cemi.cpp.

CEMI_MC_L_DATA_REQ = 0x11
CEMI_MC_L_DATA_IND = 0x29
CEMI_MC_L_DATA_CON = 0x2E

CF1_FRAMEFMT_STD = 0x80
CF1_REPEAT = 0x20
CF1_PRIORITY_MASK = 0x0C
CF1_ACK_REQ = 0x02
CF1_CONFIRM = 0x01

CF2_DEST_GROUP = 0x80
CF2_HOPCOUNT_SHIFT = 4


@dataclass(frozen=True)
class LDataFrame:
    src: int
    dst: int
    is_group: bool
    tpdu: bytes
    hop_count: int = 6
    priority: int = 3  # 0..3 (System/Urgent/Normal/Low)
    standard: bool = True
    repeated: bool = True
    ack_requested: bool = False
    confirmation: bool = False


def pack_tpdu(tpci: int, apci: int, payload: bytes) -> bytes:
    # Mirrors knx::protocol::packTpduHeader.
    tpci_type = tpci & 0xC0
    if tpci_type in (0x80, 0xC0):
        return bytes([tpci & 0xFF, 0x00]) + payload

    tpci6 = tpci & 0xFC
    apci10 = apci & 0x03FF
    tpdu0 = (tpci6 | ((apci10 >> 8) & 0x03)) & 0xFF
    tpdu1 = apci10 & 0xFF
    return bytes([tpdu0, tpdu1]) + payload


def unpack_tpdu(tpdu0: int, tpdu1: int) -> Tuple[int, int]:
    tpci_type = tpdu0 & 0xC0
    if tpci_type in (0x80, 0xC0):
        return tpdu0 & 0xFF, 0
    tpci = tpdu0 & 0xFC
    apci = (((tpdu0 & 0x03) << 8) | tpdu1) & 0x03FF
    return tpci, apci


def encode_l_data(frame: LDataFrame, message_code: int = CEMI_MC_L_DATA_REQ) -> bytes:
    if len(frame.tpdu) < 2 or len(frame.tpdu) > 256:
        raise ValueError("invalid TPDU length")

    cf1 = 0
    if frame.standard:
        cf1 |= CF1_FRAMEFMT_STD
    if frame.repeated:
        cf1 |= CF1_REPEAT
    cf1 |= ((frame.priority & 0x03) << 2) & CF1_PRIORITY_MASK
    if frame.ack_requested:
        cf1 |= CF1_ACK_REQ
    if frame.confirmation:
        cf1 |= CF1_CONFIRM

    cf2 = ((frame.hop_count & 0x07) << CF2_HOPCOUNT_SHIFT) & 0x70
    if frame.is_group:
        cf2 |= CF2_DEST_GROUP

    src_hi, src_lo = (frame.src >> 8) & 0xFF, frame.src & 0xFF
    dst_hi, dst_lo = (frame.dst >> 8) & 0xFF, frame.dst & 0xFF

    npdu_len = (len(frame.tpdu) - 1) & 0xFF

    out = bytearray()
    out.append(message_code & 0xFF)
    out.append(0x00)  # AddInfoLen
    out.append(cf1)
    out.append(cf2)
    out.extend([src_hi, src_lo, dst_hi, dst_lo, npdu_len])
    out.extend(frame.tpdu)
    return bytes(out)


def decode_l_data(cemi: bytes) -> Tuple[int, LDataFrame]:
    if len(cemi) < 11:
        raise ValueError("cEMI too short")
    idx = 0
    message_code = cemi[idx]
    idx += 1
    add_len = cemi[idx]
    idx += 1
    idx += add_len
    if len(cemi) < idx + 9:
        raise ValueError("cEMI too short")

    cf1 = cemi[idx]
    cf2 = cemi[idx + 1]
    idx += 2

    standard = (cf1 & CF1_FRAMEFMT_STD) != 0
    repeated = (cf1 & CF1_REPEAT) != 0
    priority = (cf1 & CF1_PRIORITY_MASK) >> 2
    ack_req = (cf1 & CF1_ACK_REQ) != 0
    confirm = (cf1 & CF1_CONFIRM) != 0

    is_group = (cf2 & CF2_DEST_GROUP) != 0
    hop_count = (cf2 & 0x70) >> CF2_HOPCOUNT_SHIFT
    if standard and (cf2 & 0x0F) != 0:
        raise ValueError("invalid CF2")

    src = (cemi[idx] << 8) | cemi[idx + 1]
    dst = (cemi[idx + 2] << 8) | cemi[idx + 3]
    idx += 4

    npdu_len = cemi[idx]
    idx += 1
    tpdu_len = npdu_len + 1
    if tpdu_len < 2 or len(cemi) < idx + tpdu_len:
        raise ValueError("invalid NPDU length")

    tpdu = cemi[idx : idx + tpdu_len]
    idx += tpdu_len
    if idx != len(cemi):
        raise ValueError("trailing bytes")

    return message_code, LDataFrame(
        src=src,
        dst=dst,
        is_group=is_group,
        tpdu=tpdu,
        hop_count=hop_count,
        priority=priority,
        standard=standard,
        repeated=repeated,
        ack_requested=ack_req,
        confirmation=confirm,
    )


# APCI service codes (from include/knx/application/apci_services.hpp)
APCI_GROUP_VALUE_READ = 0x000
APCI_GROUP_VALUE_RESPONSE = 0x040
APCI_GROUP_VALUE_WRITE = 0x080

APCI_DEVICE_DESCRIPTOR_READ = 0x300
APCI_DEVICE_DESCRIPTOR_RESPONSE = 0x340


def apci(service: int, data6: int = 0) -> int:
    return (service & 0x03FF) | (data6 & 0x3F)


def decode_apci_service(apci_field: int) -> int:
    service_group = apci_field & 0x03C0
    if service_group == 0x03C0:
        return apci_field & 0x03FF
    return service_group


def decode_group_value(tpdu: bytes) -> Tuple[int, bytes]:
    if len(tpdu) < 2:
        raise ValueError("tpdu too short")
    _tpci, apci_field = unpack_tpdu(tpdu[0], tpdu[1])
    svc = decode_apci_service(apci_field)

    payload = tpdu[2:]

    if svc in (APCI_GROUP_VALUE_WRITE, APCI_GROUP_VALUE_RESPONSE):
        if len(payload) == 0:
            # short APDU: 6-bit value stored in apci data6
            return svc, bytes([apci_field & 0x3F])
    return svc, payload
