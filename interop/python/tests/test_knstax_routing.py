# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import os
import subprocess
from pathlib import Path

import pytest

from tests.process_helpers import drain_process_output, wait_for_line

from knip_gateway import cemi_codec
from knip_gateway.routing_client import KnxIpRoutingClient


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _knstax_peer_path() -> Path:
    v = os.environ.get("KNSTAX_ROUTING_PEER_BIN")
    if not v:
        raise RuntimeError("KNSTAX_ROUTING_PEER_BIN environment variable or pytest --knstax-routing-peer-bin must be set to run routing interop tests")
    p = Path(v)
    if not p.exists():
        raise RuntimeError(f"KNSTAX_ROUTING_PEER_BIN is set but binary not found: {v}")
    return p


def _pick_port() -> int:
    pid = os.getpid()
    return 35000 + (pid % 20000)


def _pick_port_salt(salt: int) -> int:
    pid = os.getpid() + int(salt)
    return 35000 + (pid % 20000)


def _pick_group() -> str:
    pid = os.getpid()
    octet = 1 + (pid % 250)
    return f"239.255.4.{octet}"



def _ia(s: str) -> int:
    area_s, line_s, dev_s = s.split(".")
    area, line, dev = int(area_s), int(line_s), int(dev_s)
    return ((area & 0x0F) << 12) | ((line & 0x0F) << 8) | (dev & 0xFF)


def _ga_3level(s: str) -> int:
    main_s, mid_s, sub_s = s.split("/")
    main, mid, sub = int(main_s), int(mid_s), int(sub_s)
    return ((main & 0x1F) << 11) | ((mid & 0x07) << 8) | (sub & 0xFF)


def _make_l_data_ind_group_write_bool(*, src_ia: str, dst_ga: str, value: int) -> bytes:
    # DPT1 short APDU: value stored in APCI data6.
    tpdu = cemi_codec.pack_tpdu(0x00, cemi_codec.apci(cemi_codec.APCI_GROUP_VALUE_WRITE, value & 0x3F), b"")
    frame = cemi_codec.LDataFrame(src=_ia(src_ia), dst=_ga_3level(dst_ga), is_group=True, tpdu=tpdu)
    return cemi_codec.encode_l_data(frame, message_code=cemi_codec.CEMI_MC_L_DATA_IND)


def _make_l_data_ind_group_write_payload(*, src_ia: str, dst_ga: str, payload: bytes) -> bytes:
    # Long APDU: payload bytes follow TPCI/APCI header.
    tpdu = cemi_codec.pack_tpdu(0x00, cemi_codec.apci(cemi_codec.APCI_GROUP_VALUE_WRITE, 0), payload)
    frame = cemi_codec.LDataFrame(src=_ia(src_ia), dst=_ga_3level(dst_ga), is_group=True, tpdu=tpdu)
    return cemi_codec.encode_l_data(frame, message_code=cemi_codec.CEMI_MC_L_DATA_IND)


def _dpt9_encode(value: float) -> bytes:
    # KNX 2-byte float (DPT9): value = 0.01 * M * 2^E, M is signed 11-bit.
    scaled = int(round(value * 100.0))
    exp = 0
    mant = scaled
    while mant < -2048 or mant > 2047:
        mant = int(round(mant / 2.0))
        exp += 1
        if exp > 15:
            raise ValueError("DPT9 out of range")

    sign = 0
    if mant < 0:
        sign = 1
        mant = (1 << 11) + mant
    raw = ((sign & 0x01) << 15) | ((exp & 0x0F) << 11) | (mant & 0x07FF)
    return bytes([(raw >> 8) & 0xFF, raw & 0xFF])


def _dpt9_decode(payload: bytes) -> float:
    if len(payload) != 2:
        raise ValueError("expected 2 bytes")
    raw = (payload[0] << 8) | payload[1]
    sign = (raw >> 15) & 0x01
    exp = (raw >> 11) & 0x0F
    mant = raw & 0x07FF
    if sign:
        mant = mant - (1 << 11)
    return 0.01 * float(mant) * float(2**exp)


def _make_l_data_ind_group_read(*, src_ia: str, dst_ga: str) -> bytes:
    tpdu = cemi_codec.pack_tpdu(0x00, cemi_codec.apci(cemi_codec.APCI_GROUP_VALUE_READ, 0), b"")
    frame = cemi_codec.LDataFrame(src=_ia(src_ia), dst=_ga_3level(dst_ga), is_group=True, tpdu=tpdu)
    return cemi_codec.encode_l_data(frame, message_code=cemi_codec.CEMI_MC_L_DATA_IND)


async def _recv_until_apci(
    client: KnxIpRoutingClient,
    *,
    want_service: int,
    want_dst_ga: str,
    timeout_s: float = 3.0,
) -> bytes:
    deadline = asyncio.get_event_loop().time() + timeout_s
    want_dst = _ga_3level(want_dst_ga)
    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            raise AssertionError(f"timeout waiting for APCI service 0x{want_service:03x}")
        rx = await client.recv_routing(timeout_s=remaining)
        msg_code, frame = cemi_codec.decode_l_data(rx.cemi)
        if msg_code != cemi_codec.CEMI_MC_L_DATA_IND:
            continue
        if not frame.is_group:
            continue
        if frame.dst != want_dst:
            continue
        svc, payload = cemi_codec.decode_group_value(frame.tpdu)
        if svc != want_service:
            continue
        return payload


@pytest.mark.asyncio
async def test_client_to_knstax_groupwrite_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(1)

    proc = subprocess.Popen(
        [
            str(peer),
            "--group",
            group,
            "--port",
            str(port),
            "--iface-address",
            "127.0.0.1",
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--timeout-ms",
            "4000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=dict(os.environ),
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        client = KnxIpRoutingClient()
        await client.start(group=group, port=port, interface_ip="127.0.0.1")
        try:
            cemi = _make_l_data_ind_group_write_bool(src_ia="1.1.2", dst_ga="1/0/0", value=1)
            client.send_cemi(cemi)
            await asyncio.sleep(0.2)
        finally:
            await client.stop()

        for _ in range(50):
            rc = proc.poll()
            if rc is not None:
                break
            await asyncio.sleep(0.1)

        rc = proc.poll()
        if rc != 0:
            out = drain_process_output(proc)
            raise AssertionError(f"peer exit={rc}, output:\n{out}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


@pytest.mark.asyncio
async def test_knstax_to_client_groupwrite_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(2)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--no-expect-rx",
                "--send",
                "01",
                "--stay-alive-ms",
                "800",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            rx = await client.recv_routing(timeout_s=3.0)
            msg_code, frame = cemi_codec.decode_l_data(rx.cemi)
            assert msg_code == cemi_codec.CEMI_MC_L_DATA_IND
            assert frame.is_group
            assert frame.dst == _ga_3level("1/0/0")

            svc, payload = cemi_codec.decode_group_value(frame.tpdu)
            assert svc == cemi_codec.APCI_GROUP_VALUE_WRITE
            assert payload == b"\x01"
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_client_to_knstax_groupread_response_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(3)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--no-expect-rx",
                "--init",
                "01",
                "--stay-alive-ms",
                "1200",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            cemi = _make_l_data_ind_group_read(src_ia="1.1.2", dst_ga="1/0/0")
            client.send_cemi(cemi)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_RESPONSE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            assert payload == b"\x01"
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_client_write_then_read_roundtrip_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(4)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--no-expect-rx",
                "--init",
                "00",
                "--stay-alive-ms",
                "1500",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            # Write value=1 then read it back; expect response reflects new state.
            cemi_wr = _make_l_data_ind_group_write_bool(src_ia="1.1.2", dst_ga="1/0/0", value=1)
            client.send_cemi(cemi_wr)
            await asyncio.sleep(0.05)

            cemi_rd = _make_l_data_ind_group_read(src_ia="1.1.2", dst_ga="1/0/0")
            client.send_cemi(cemi_rd)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_RESPONSE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            assert payload == b"\x01"
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_client_to_knstax_dpt5_read_response_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(5)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--dpt-type",
                "5",
                "--no-expect-rx",
                "--init",
                "2a",
                "--stay-alive-ms",
                "1200",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            cemi_rd = _make_l_data_ind_group_read(src_ia="1.1.2", dst_ga="1/0/0")
            client.send_cemi(cemi_rd)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_RESPONSE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            assert payload == bytes([0x2A])
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_client_write_then_read_dpt5_roundtrip_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(6)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--dpt-type",
                "5",
                "--no-expect-rx",
                "--init",
                "00",
                "--stay-alive-ms",
                "1500",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            cemi_wr = _make_l_data_ind_group_write_payload(src_ia="1.1.2", dst_ga="1/0/0", payload=b"\x7f")
            client.send_cemi(cemi_wr)
            await asyncio.sleep(0.05)

            cemi_rd = _make_l_data_ind_group_read(src_ia="1.1.2", dst_ga="1/0/0")
            client.send_cemi(cemi_rd)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_RESPONSE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            assert payload == b"\x7f"
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_client_to_knstax_dpt9_read_response_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(7)

    init_v = 21.5

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--dpt-type",
                "9",
                "--no-expect-rx",
                "--init-float",
                str(init_v),
                "--stay-alive-ms",
                "1200",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            cemi_rd = _make_l_data_ind_group_read(src_ia="1.1.2", dst_ga="1/0/0")
            client.send_cemi(cemi_rd)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_RESPONSE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            got = _dpt9_decode(payload)
            assert abs(got - init_v) < 0.05
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_client_write_then_read_dpt9_roundtrip_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(8)

    write_v = 12.34
    write_payload = _dpt9_encode(write_v)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--dpt-type",
                "9",
                "--no-expect-rx",
                "--init-float",
                "0.0",
                "--stay-alive-ms",
                "1500",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            cemi_wr = _make_l_data_ind_group_write_payload(src_ia="1.1.2", dst_ga="1/0/0", payload=write_payload)
            client.send_cemi(cemi_wr)
            await asyncio.sleep(0.05)

            cemi_rd = _make_l_data_ind_group_read(src_ia="1.1.2", dst_ga="1/0/0")
            client.send_cemi(cemi_rd)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_RESPONSE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            got = _dpt9_decode(payload)
            assert abs(got - write_v) < 0.05
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_knstax_to_client_dpt5_groupwrite_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(9)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--dpt-type",
                "5",
                "--no-expect-rx",
                "--send",
                "7f",
                "--stay-alive-ms",
                "800",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_WRITE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            assert payload == b"\x7f"
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()


@pytest.mark.asyncio
async def test_knstax_to_client_dpt9_groupwrite_ip_routing():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group = _pick_group()
    port = _pick_port_salt(10)

    target = 12.34
    write_payload = _dpt9_encode(target)

    client = KnxIpRoutingClient()
    await client.start(group=group, port=port, interface_ip="127.0.0.1")
    try:
        proc = subprocess.Popen(
            [
                str(peer),
                "--group",
                group,
                "--port",
                str(port),
                "--iface-address",
                "127.0.0.1",
                "--own",
                "1.1.1",
                "--ga",
                "1/0/0",
                "--dpt-type",
                "9",
                "--no-expect-rx",
                "--send-hex",
                write_payload.hex(),
                "--stay-alive-ms",
                "800",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            payload = await _recv_until_apci(
                client,
                want_service=cemi_codec.APCI_GROUP_VALUE_WRITE,
                want_dst_ga="1/0/0",
                timeout_s=3.0,
            )
            got = _dpt9_decode(payload)
            assert abs(got - target) < 0.05
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await client.stop()
