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
from knip_gateway.tunnel_client import KnxIpTunnelingClient
from knip_gateway.tunnel_gateway import KnxIpTunnelingGateway


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _knstax_peer_path() -> Path:
    v = os.environ.get("KNSTAX_TUNNEL_PEER_BIN")
    if not v:
        raise RuntimeError("KNSTAX_TUNNEL_PEER_BIN environment variable or pytest --knstax-tunnel-peer-bin must be set to run tunneling interop tests")
    p = Path(v)
    if not p.exists():
        raise RuntimeError(f"KNSTAX_TUNNEL_PEER_BIN is set but binary not found: {v}")
    return p



def _ia(s: str) -> int:
    # area.line.device -> 4/4/8 bits
    area_s, line_s, dev_s = s.split(".")
    area, line, dev = int(area_s), int(line_s), int(dev_s)
    return ((area & 0x0F) << 12) | ((line & 0x0F) << 8) | (dev & 0xFF)


def _ga_3level(s: str) -> int:
    # main/middle/sub -> 5/3/8 bits
    main_s, mid_s, sub_s = s.split("/")
    main, mid, sub = int(main_s), int(mid_s), int(sub_s)
    return ((main & 0x1F) << 11) | ((mid & 0x07) << 8) | (sub & 0xFF)


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


def _make_l_data_req(
    *,
    src_ia: str,
    dst_ga: str,
    apci_service: int,
    data6: int = 0,
    payload: bytes = b"",
) -> bytes:
    tpdu = cemi_codec.pack_tpdu(0x00, cemi_codec.apci(apci_service, data6), payload)
    frame = cemi_codec.LDataFrame(src=_ia(src_ia), dst=_ga_3level(dst_ga), is_group=True, tpdu=tpdu)
    return cemi_codec.encode_l_data(frame, message_code=cemi_codec.CEMI_MC_L_DATA_REQ)


@pytest.mark.asyncio
async def test_client_to_knstax_groupwrite_udp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--timeout-ms",
            "4000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        client = KnxIpTunnelingClient()
        await client.start((host, port), local_ip="127.0.0.1")
        try:
            # DPT1 short APDU: value stored in APCI data6.
            cemi = _make_l_data_req(
                src_ia="1.1.2",
                dst_ga="1/0/0",
                apci_service=cemi_codec.APCI_GROUP_VALUE_WRITE,
                data6=1,
            )
            client.send_cemi(cemi)

            await asyncio.sleep(0.1)
            assert gw.rx_tunnel_req > 0, "gateway did not receive tunnelling request"
            assert gw.tx_forward > 0, "gateway did not forward tunnelling request to other clients"
        finally:
            await client.stop()

        # Peer should exit successfully after receiving the forwarded telegram.
        for _ in range(50):
            rc = proc.poll()
            if rc is not None:
                break
            await asyncio.sleep(0.1)

        rc = proc.poll()
        if rc != 0:
            out = drain_process_output(proc)
            raise AssertionError(
                f"peer exit={rc}, output:\n{out}\n\n"
                f"gateway stats rx_tunnel_req={gw.rx_tunnel_req} tx_forward={gw.tx_forward} tx_tunnel_ack={gw.tx_tunnel_ack}\n"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()


@pytest.mark.asyncio
async def test_knstax_to_client_groupwrite_udp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    client = KnxIpTunnelingClient()
    await client.start((host, port), local_ip="127.0.0.1")

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--send",
            "01",
            "--no-expect-rx",
            "--timeout-ms",
            "2000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)
        rx = await client.recv_tunneling(timeout_s=3.0)
        _mc, ldata = cemi_codec.decode_l_data(rx.cemi)
        svc, pl = cemi_codec.decode_group_value(ldata.tpdu)
        assert svc == cemi_codec.APCI_GROUP_VALUE_WRITE
        assert ldata.is_group
        assert ldata.dst == _ga_3level("1/0/0")
        assert pl in (b"\x01", b"\x00")

        assert gw.rx_tunnel_req > 0, "gateway did not receive tunnelling request"
        assert gw.tx_forward > 0, "gateway did not forward tunnelling request to other clients"
    finally:
        await client.stop()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()


@pytest.mark.asyncio
async def test_client_reads_from_knstax_groupread_response_udp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--init",
            "01",
            "--expect-read",
            "--no-expect-rx",
            "--timeout-ms",
            "5000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        client = KnxIpTunnelingClient()
        await client.start((host, port), local_ip="127.0.0.1")
        try:
            cemi = _make_l_data_req(src_ia="1.1.2", dst_ga="1/0/0", apci_service=cemi_codec.APCI_GROUP_VALUE_READ)
            client.send_cemi(cemi)

            rx = await client.recv_tunneling(timeout_s=3.0)
            _mc, ldata = cemi_codec.decode_l_data(rx.cemi)
            svc, pl = cemi_codec.decode_group_value(ldata.tpdu)
            assert svc == cemi_codec.APCI_GROUP_VALUE_RESPONSE
            assert pl in (b"\x01", b"\x00")

            await wait_for_line(proc, b"EVENT group_read", timeout_s=3.0)
        finally:
            await client.stop()

        rc = await asyncio.get_event_loop().run_in_executor(None, proc.wait)
        assert rc == 0, f"peer exit={rc}"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()


@pytest.mark.asyncio
async def test_client_write_then_read_state_roundtrip_udp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--expect-read",
            "--timeout-ms",
            "20000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        client = KnxIpTunnelingClient()
        await client.start((host, port), local_ip="127.0.0.1")
        try:
            write = _make_l_data_req(
                src_ia="1.1.2",
                dst_ga="1/0/0",
                apci_service=cemi_codec.APCI_GROUP_VALUE_WRITE,
                data6=1,
            )
            client.send_cemi(write)
            await asyncio.sleep(0.1)

            read = _make_l_data_req(src_ia="1.1.2", dst_ga="1/0/0", apci_service=cemi_codec.APCI_GROUP_VALUE_READ)
            client.send_cemi(read)

            rx = await client.recv_tunneling(timeout_s=3.0)
            _mc, ldata = cemi_codec.decode_l_data(rx.cemi)
            svc, pl = cemi_codec.decode_group_value(ldata.tpdu)
            assert svc == cemi_codec.APCI_GROUP_VALUE_RESPONSE
            assert pl in (b"\x01",)

            await wait_for_line(proc, b"EVENT group_read", timeout_s=3.0)
        finally:
            await client.stop()

        rc = await asyncio.get_event_loop().run_in_executor(None, proc.wait)
        assert rc == 0, f"peer exit={rc}"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()


@pytest.mark.asyncio
async def test_client_write_then_read_dpt5_u8_roundtrip_udp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--dpt-type",
            "5",
            "--expect-read",
            "--timeout-ms",
            "20000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        client = KnxIpTunnelingClient()
        await client.start((host, port), local_ip="127.0.0.1")
        try:
            write_payload = bytes([123])
            write = _make_l_data_req(
                src_ia="1.1.2",
                dst_ga="1/0/0",
                apci_service=cemi_codec.APCI_GROUP_VALUE_WRITE,
                payload=write_payload,
            )
            client.send_cemi(write)
            await asyncio.sleep(0.1)

            read = _make_l_data_req(src_ia="1.1.2", dst_ga="1/0/0", apci_service=cemi_codec.APCI_GROUP_VALUE_READ)
            client.send_cemi(read)

            rx = await client.recv_tunneling(timeout_s=3.0)
            _mc, ldata = cemi_codec.decode_l_data(rx.cemi)
            svc, pl = cemi_codec.decode_group_value(ldata.tpdu)
            assert svc == cemi_codec.APCI_GROUP_VALUE_RESPONSE
            assert pl == write_payload
            await wait_for_line(proc, b"EVENT group_read", timeout_s=3.0)
        finally:
            await client.stop()

        rc = await asyncio.get_event_loop().run_in_executor(None, proc.wait)
        assert rc == 0, f"peer exit={rc}"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()


@pytest.mark.asyncio
async def test_client_write_then_read_dpt9_float_roundtrip_udp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--dpt-type",
            "9",
            "--expect-read",
            "--timeout-ms",
            "20000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        client = KnxIpTunnelingClient()
        await client.start((host, port), local_ip="127.0.0.1")
        try:
            target = 21.37
            write_payload = _dpt9_encode(target)
            write = _make_l_data_req(
                src_ia="1.1.2",
                dst_ga="1/0/0",
                apci_service=cemi_codec.APCI_GROUP_VALUE_WRITE,
                payload=write_payload,
            )
            client.send_cemi(write)
            await asyncio.sleep(0.1)

            read = _make_l_data_req(src_ia="1.1.2", dst_ga="1/0/0", apci_service=cemi_codec.APCI_GROUP_VALUE_READ)
            client.send_cemi(read)

            rx = await client.recv_tunneling(timeout_s=3.0)
            _mc, ldata = cemi_codec.decode_l_data(rx.cemi)
            svc, pl = cemi_codec.decode_group_value(ldata.tpdu)
            assert svc == cemi_codec.APCI_GROUP_VALUE_RESPONSE
            got = _dpt9_decode(pl)
            assert abs(got - target) < 0.05
            await wait_for_line(proc, b"EVENT group_read", timeout_s=3.0)
        finally:
            await client.stop()

        rc = await asyncio.get_event_loop().run_in_executor(None, proc.wait)
        assert rc == 0, f"peer exit={rc}"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()


@pytest.mark.asyncio
async def test_client_device_descriptor_read_response_udp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--stay-alive-ms",
            "5000",
            "--no-expect-rx",
            "--timeout-ms",
            "7000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        client = KnxIpTunnelingClient()
        await client.start((host, port), local_ip="127.0.0.1")
        try:
            req = _make_l_data_req(
                src_ia="1.1.2",
                dst_ga="1/0/0",
                apci_service=cemi_codec.APCI_DEVICE_DESCRIPTOR_READ,
                data6=0,
            )
            client.send_cemi(req)

            rx = await client.recv_tunneling(timeout_s=3.0)
            _mc, ldata = cemi_codec.decode_l_data(rx.cemi)
            _tpci, apci_field = cemi_codec.unpack_tpdu(ldata.tpdu[0], ldata.tpdu[1])
            svc = cemi_codec.decode_apci_service(apci_field)
            assert svc == cemi_codec.APCI_DEVICE_DESCRIPTOR_RESPONSE
            assert len(ldata.tpdu) >= 4
        finally:
            await client.stop()

        rc = await asyncio.get_event_loop().run_in_executor(None, proc.wait)
        assert rc == 0, f"peer exit={rc}"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()
