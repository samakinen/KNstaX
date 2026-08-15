# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import os
import subprocess
from pathlib import Path

import pytest

from tests.process_helpers import drain_process_output, wait_for_line

from knip_gateway.ip_secure_tunnel_gateway import KnxIpSecureTunnelingGateway


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _knstax_peer_path() -> Path:
    v = os.environ.get("KNSTAX_TUNNEL_PEER_BIN")
    if not v:
        raise RuntimeError("KNSTAX_TUNNEL_PEER_BIN environment variable or pytest --knstax-tunnel-peer-bin must be set to run interop tests")
    p = Path(v)
    if not p.exists():
        raise RuntimeError(f"KNSTAX_TUNNEL_PEER_BIN is set but binary not found: {v}")
    return p



def _ipsec_defaults() -> tuple[int, str, str, str, int]:
    # user_id, password, client_private_key_hex (32 bytes), client_serial_hex (6 bytes), initial_seq
    user_id = int(os.environ.get("XKNX_IPSEC_USER_ID", "1"))
    password = os.environ.get("XKNX_IPSEC_PASSWORD", "password")

    # Deterministic test values (must be non-zero, and shared with peer).
    client_priv_hex = os.environ.get(
        "KNSTAX_IPSEC_CLIENT_PRIVATE_KEY_HEX",
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
    )
    client_serial_hex = os.environ.get("KNSTAX_IPSEC_CLIENT_SERIAL_HEX", "0000786b6e78")  # 0000"xknx"

    initial_seq = int(os.environ.get("KNSTAX_IPSEC_INITIAL_SEQ", "1"))
    return user_id, password, client_priv_hex, client_serial_hex, initial_seq


def _dpt_value_to_knx_bytes(value: object) -> bytes:
    # xknx uses DPT wrappers (e.g. DPTBinary) for payload values.
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    to_knx = getattr(value, "to_knx", None)
    if callable(to_knx):
        return bytes(to_knx())
    inner = getattr(value, "value", value)
    if isinstance(inner, bool):
        return b"\x01" if inner else b"\x00"
    if isinstance(inner, int):
        return bytes([inner & 0xFF])
    raise TypeError(f"Unsupported xknx DPT value type: {type(value)!r}")


@pytest.mark.asyncio
async def test_xknx_to_knstax_ip_secure_groupwrite_tcp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    user_id, password, client_priv_hex, client_serial_hex, initial_seq = _ipsec_defaults()

    gw = KnxIpSecureTunnelingGateway(
        bind_host="127.0.0.1",
        bind_port=0,
        user_id=user_id,
        user_password=password,
        include_session_response_mac16=True,
    )
    await gw.start()
    host, port = gw.address

    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--ip-secure",
            "--ip-secure-user-id",
            str(user_id),
            "--ip-secure-password",
            password,
            "--ip-secure-client-private-key-hex",
            client_priv_hex,
            "--ip-secure-client-serial-hex",
            client_serial_hex,
            "--ip-secure-initial-seq",
            str(initial_seq),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--timeout-ms",
            "5000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)

        from xknx import XKNX
        from xknx.io import ConnectionConfig, ConnectionType
        from xknx.io import SecureConfig
        from xknx.tools import group_value_write

        cfg = ConnectionConfig(
            connection_type=ConnectionType.TUNNELING_TCP_SECURE,
            gateway_ip=host,
            gateway_port=port,
        )
        cfg.secure_config = SecureConfig(user_id=user_id, user_password=password)
        xknx = XKNX(connection_config=cfg)

        try:
            await xknx.start()
            # Plain GroupValueWrite over secure tunnel.
            group_value_write(xknx, "1/0/0", True)
            await asyncio.sleep(0.3)
            assert gw.rx_tunnel_req > 0
            assert gw.tx_forward > 0
        finally:
            await xknx.stop()

        # Peer should exit after receiving a telegram.
        for _ in range(60):
            rc = proc.poll()
            if rc is not None:
                break
            await asyncio.sleep(0.1)

        rc = proc.poll()
        if rc != 0:
            out = drain_process_output(proc)
            raise AssertionError(
                f"peer exit={rc}, output:\n{out}\n\n"
                f"gateway stats rx_secure_wrapper={gw.rx_secure_wrapper} rx_tunnel_req={gw.rx_tunnel_req} tx_forward={gw.tx_forward}\n"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        await gw.stop()


@pytest.mark.asyncio
async def test_knstax_to_xknx_ip_secure_groupwrite_tcp_tunneling():
    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    user_id, password, client_priv_hex, client_serial_hex, initial_seq = _ipsec_defaults()

    gw = KnxIpSecureTunnelingGateway(
        bind_host="127.0.0.1",
        bind_port=0,
        user_id=user_id,
        user_password=password,
        include_session_response_mac16=True,
    )
    await gw.start()
    host, port = gw.address

    from xknx import XKNX
    from xknx.io import ConnectionConfig, ConnectionType
    from xknx.io import SecureConfig
    from xknx.telegram import GroupAddress

    cfg = ConnectionConfig(
        connection_type=ConnectionType.TUNNELING_TCP_SECURE,
        gateway_ip=host,
        gateway_port=port,
    )
    cfg.secure_config = SecureConfig(user_id=user_id, user_password=password)

    xknx = XKNX(connection_config=cfg)

    got = asyncio.Event()

    def cb(telegram):
        try:
            if str(telegram.destination_address) == str(GroupAddress("1/0/0")):
                got.set()
        except Exception:
            pass

    xknx.telegram_queue.register_telegram_received_cb(cb)

    try:
        await xknx.start()
        try:
            env = dict(os.environ)
            proc = subprocess.Popen(
                [
                    str(peer),
                    "--gw-host",
                    host,
                    "--gw-port",
                    str(port),
                    "--ip-secure",
                    "--ip-secure-user-id",
                    str(user_id),
                    "--ip-secure-password",
                    password,
                    "--ip-secure-client-private-key-hex",
                    client_priv_hex,
                    "--ip-secure-client-serial-hex",
                    client_serial_hex,
                    "--ip-secure-initial-seq",
                    str(initial_seq),
                    "--own",
                    "1.1.1",
                    "--ga",
                    "1/0/0",
                    "--send",
                    "01",
                    "--no-expect-rx",
                    "--timeout-ms",
                    "4000",
                    "--stay-alive-ms",
                    "1500",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env=env,
            )

            await wait_for_line(proc, b"READY", timeout_s=3.0)
            await asyncio.wait_for(got.wait(), timeout=4.0)
        finally:
            await xknx.stop()

        assert gw.rx_tunnel_req > 0
        assert gw.tx_forward > 0
    finally:
        if "proc" in locals() and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        await gw.stop()


@pytest.mark.asyncio
async def test_xknx_reads_from_knstax_ip_secure_groupread_response_tcp_tunneling():
    """Interop test: xknx GroupValueRead -> KNstaX responds -> xknx receives GroupValueResponse (over KNX/IP Secure)."""

    try:
        from xknx import XKNX
        from xknx.io import ConnectionConfig, ConnectionType, SecureConfig
        from xknx.telegram import GroupAddress
        from xknx.telegram.apci import GroupValueResponse
        from xknx.tools import group_value_read
    except Exception as exc:  # pragma: no cover
        pytest.skip(f"xknx not available: {exc}")

    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    user_id, password, client_priv_hex, client_serial_hex, initial_seq = _ipsec_defaults()

    gw = KnxIpSecureTunnelingGateway(
        bind_host="127.0.0.1",
        bind_port=0,
        user_id=user_id,
        user_password=password,
        include_session_response_mac16=True,
    )
    await gw.start()
    host, port = gw.address

    # Start peer: it will answer reads with its initialized value.
    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--ip-secure",
            "--ip-secure-user-id",
            str(user_id),
            "--ip-secure-password",
            password,
            "--ip-secure-client-private-key-hex",
            client_priv_hex,
            "--ip-secure-client-serial-hex",
            client_serial_hex,
            "--ip-secure-initial-seq",
            str(initial_seq),
            "--own",
            "1.1.1",
            "--ga",
            "1/0/0",
            "--dpt-type",
            "1",
            "--init",
            "01",
            "--no-expect-rx",
            "--stay-alive-ms",
            "1500",
            "--timeout-ms",
            "5000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    got_response: asyncio.Future[object] = asyncio.get_event_loop().create_future()
    ga_str = "1/0/0"
    ga_obj = GroupAddress(ga_str)

    cfg = ConnectionConfig(
        connection_type=ConnectionType.TUNNELING_TCP_SECURE,
        gateway_ip=host,
        gateway_port=port,
    )
    cfg.secure_config = SecureConfig(user_id=user_id, user_password=password)
    xknx = XKNX(connection_config=cfg)

    await xknx.start()
    try:
        def _on_rx(telegram: object) -> None:
            if got_response.done():
                return
            if str(getattr(telegram, "destination_address", "")) != ga_str:
                return
            payload = getattr(telegram, "payload", None)
            if isinstance(payload, GroupValueResponse):
                got_response.set_result(telegram)

        xknx.telegram_queue.register_telegram_received_cb(_on_rx, group_addresses=[ga_obj])

        await wait_for_line(proc, b"READY", timeout_s=3.0)

        group_value_read(xknx, ga_str)
        done, _ = await asyncio.wait([got_response], timeout=5.0, return_when=asyncio.FIRST_COMPLETED)
        assert done, "Timed out waiting for GroupValueResponse"

        telegram = got_response.result()
        payload = getattr(telegram, "payload")
        assert isinstance(payload, GroupValueResponse)
        # Peer init=01 for DPT1.
        assert _dpt_value_to_knx_bytes(payload.value) == b"\x01"
    finally:
        await xknx.stop()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        await gw.stop()
