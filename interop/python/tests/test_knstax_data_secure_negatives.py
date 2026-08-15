# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import os
import secrets
import subprocess
from pathlib import Path

import pytest

from tests.process_helpers import wait_for_line

from knip_gateway.tunnel_gateway import KnxIpTunnelingGateway


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



@pytest.mark.asyncio
async def test_data_secure_replay_dropped_or_flagged(tmp_path: Path) -> None:
    """Sending the same Data Secure sequence twice should be dropped or flagged by xknx.

    We send two identical secure writes from the same IA with the same sequence number.
    Expectation: xknx either drops the second (no app-level telegram) or reports a key issue.
    """

    try:
        from xknx import XKNX
        from xknx.io import ConnectionConfig, ConnectionType, SecureConfig
        from xknx.secure.keyring import sync_load_keyring
        from xknx.telegram import GroupAddress
        from xknx.telegram.apci import GroupValueWrite
    except Exception as exc:  # pragma: no cover
        pytest.skip(f"xknx not available: {exc}")

    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group_address = os.environ.get("XKNX_GA") or "0/0/1"
    xknx_ia = os.environ.get("XKNX_IA") or "1.1.2"
    knstax_own = os.environ.get("KNSTAX_OWN") or "1.1.1"

    # Generate a keyring authorizing KNstaX as sender.
    knxkeys_password = secrets.token_urlsafe(18)
    group_key = secrets.token_bytes(16)
    # Place generated .knxkeys in pytest's tmp_path fixture directory
    knxkeys_path = tmp_path / "knxkeys"
    knxkeys_path.mkdir(exist_ok=True)
    knxkeys_file_path = knxkeys_path / f"generated_{secrets.token_hex(8)}.knxkeys"

    # Minimal keyring compatible with xknx's loader.
    from tests.test_knstax_data_secure_xknx import _generate_knxkeys_file  # reuse helper
    _generate_knxkeys_file(
        out_path=knxkeys_file_path,
        password=knxkeys_password,
        group_address=group_address,
        receiver_ia=xknx_ia,
        senders_ia=[knstax_own, xknx_ia],
        group_key=group_key,
    )

    keyring = sync_load_keyring(str(knxkeys_file_path), knxkeys_password)
    ga_obj = GroupAddress(group_address)
    ga_keys = keyring.get_data_secure_group_keys()
    assert ga_obj in ga_keys
    group_key_bytes = ga_keys[ga_obj]

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    await asyncio.sleep(0.05)
    host, port = gw.address

    # Setup xknx with secure config
    secure_config = SecureConfig(knxkeys_file_path=str(knxkeys_file_path), knxkeys_password=knxkeys_password)
    connection_config = ConnectionConfig(
        connection_type=ConnectionType.TUNNELING,
        gateway_ip=host,
        gateway_port=port,
        local_ip="127.0.0.1",
        route_back=True,
        individual_address=xknx_ia,
        secure_config=secure_config,
    )

    xknx = XKNX(connection_config=connection_config)

    got_count = 0
    got_issue = asyncio.Event()

    def _on_rx(telegram: object) -> None:
        nonlocal got_count
        if str(getattr(telegram, "destination_address", "")) != group_address:
            return
        if isinstance(getattr(telegram, "payload", None), GroupValueWrite):
            got_count += 1

    def _on_secure_issue(telegram: object) -> None:
        if not got_issue.is_set():
            got_issue.set()

    await xknx.start()
    try:
        xknx.telegram_queue.register_telegram_received_cb(_on_rx, group_addresses=[ga_obj])
        xknx.telegram_queue.register_data_secure_group_key_issue_cb(_on_secure_issue)

        # First send with sequence N
        seq_n = 12345
        env = dict(os.environ)
        args = [
            str(peer),
            "--gw-host", host,
            "--gw-port", str(port),
            "--own", knstax_own,
            "--ga", group_address,
            "--dpt-type", "1",
            "--send", "01",
            "--no-expect-rx",
            "--stay-alive-ms", "800",
            "--timeout-ms", "5000",
            "--data-secure",
            "--data-secure-key-hex", bytes(group_key_bytes).hex(),
            "--data-secure-send-seq", str(seq_n),
        ]
        proc1 = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        ready = await wait_for_line(proc1, b"READY", timeout_s=3.0, raise_on_missing=False)
        if not ready:
            pytest.skip("KNstaX peer failed to start (no READY)")
        # Wait until the first telegram is received
        for _ in range(30):
            if got_count >= 1:
                break
            await asyncio.sleep(0.1)

        # Second send with the same sequence N (replay)
        proc2 = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        ready2 = await wait_for_line(proc2, b"READY", timeout_s=3.0, raise_on_missing=False)
        if not ready2:
            pytest.skip("KNstaX peer (replay) failed to start (no READY)")

        # Give xknx time to either drop or flag
        await asyncio.sleep(0.8)

        # Validate: either issue flagged or no increment in app-level deliveries
        assert got_count >= 1, "Did not receive the first secure telegram"
        assert got_issue.is_set() or got_count == 1, "Replay was accepted as a fresh telegram"
    finally:
        try:
            await xknx.stop()
        except Exception:
            pass
        gw.stop()


@pytest.mark.asyncio
async def test_data_secure_unauthorized_sender_rejected(tmp_path: Path) -> None:
    """A Data Secure telegram from an unauthorized IA must be rejected by xknx.

    We generate a keyring that does not authorize KNstaX IA as a sender for the GA.
    Expectation: xknx does not deliver a decrypted telegram; it may flag a key issue.
    """

    try:
        from xknx import XKNX
        from xknx.io import ConnectionConfig, ConnectionType, SecureConfig
        from xknx.secure.keyring import sync_load_keyring
        from xknx.telegram import GroupAddress
        from xknx.telegram.apci import GroupValueWrite
    except Exception as exc:  # pragma: no cover
        pytest.skip(f"xknx not available: {exc}")

    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group_address = os.environ.get("XKNX_GA") or "0/0/1"
    xknx_ia = os.environ.get("XKNX_IA") or "1.1.2"
    knstax_own = os.environ.get("KNSTAX_OWN") or "1.1.1"

    # Generate a keyring that does NOT authorize KNstaX as sender
    knxkeys_password = secrets.token_urlsafe(18)
    group_key = secrets.token_bytes(16)
    knxkeys_path = tmp_path / "knxkeys"
    knxkeys_path.mkdir(exist_ok=True)
    knxkeys_file_path = knxkeys_path / f"generated_{secrets.token_hex(8)}.knxkeys"

    from tests.test_knstax_data_secure_xknx import _generate_knxkeys_file  # reuse helper
    _generate_knxkeys_file(
        out_path=knxkeys_file_path,
        password=knxkeys_password,
        group_address=group_address,
        receiver_ia=xknx_ia,
        senders_ia=[xknx_ia],  # KNstaX IA not authorized
        group_key=group_key,
    )

    keyring = sync_load_keyring(str(knxkeys_file_path), knxkeys_password)
    ga_obj = GroupAddress(group_address)
    ga_keys = keyring.get_data_secure_group_keys()
    assert ga_obj in ga_keys
    group_key_bytes = ga_keys[ga_obj]

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    await asyncio.sleep(0.05)
    host, port = gw.address

    secure_config = SecureConfig(knxkeys_file_path=str(knxkeys_file_path), knxkeys_password=knxkeys_password)
    connection_config = ConnectionConfig(
        connection_type=ConnectionType.TUNNELING,
        gateway_ip=host,
        gateway_port=port,
        local_ip="127.0.0.1",
        route_back=True,
        individual_address=xknx_ia,
        secure_config=secure_config,
    )

    xknx = XKNX(connection_config=connection_config)

    got_telegram = asyncio.Event()
    got_issue = asyncio.Event()

    def _on_rx(telegram: object) -> None:
        if str(getattr(telegram, "destination_address", "")) != group_address:
            return
        if isinstance(getattr(telegram, "payload", None), GroupValueWrite):
            got_telegram.set()

    def _on_secure_issue(telegram: object) -> None:
        if not got_issue.is_set():
            got_issue.set()

    await xknx.start()
    try:
        xknx.telegram_queue.register_telegram_received_cb(_on_rx, group_addresses=[ga_obj])
        xknx.telegram_queue.register_data_secure_group_key_issue_cb(_on_secure_issue)

        env = dict(os.environ)
        args = [
            str(peer),
            "--gw-host", host,
            "--gw-port", str(port),
            "--own", knstax_own,
            "--ga", group_address,
            "--dpt-type", "1",
            "--send", "01",
            "--no-expect-rx",
            "--stay-alive-ms", "800",
            "--timeout-ms", "5000",
            "--data-secure",
            "--data-secure-key-hex", bytes(group_key_bytes).hex(),
        ]
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        ready = await wait_for_line(proc, b"READY", timeout_s=3.0, raise_on_missing=False)
        if not ready:
            pytest.skip("KNstaX peer failed to start (no READY)")

        await asyncio.sleep(0.8)
        # Validate: no app-level delivery; issue may be flagged
        assert not got_telegram.is_set(), "Unauthorized Data Secure sender was accepted"
        # xknx may flag a key issue depending on internal validation stage
        # Accept either explicit issue or silent drop.
    finally:
        try:
            await xknx.stop()
        except Exception:
            pass
        gw.stop()
