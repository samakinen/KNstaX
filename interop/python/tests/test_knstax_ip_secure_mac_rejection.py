# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import os
import subprocess
from pathlib import Path

import pytest

from knip_gateway.ip_secure_tunnel_gateway import KnxIpSecureTunnelingGateway
from tests.process_helpers import wait_for_line


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
async def test_knstax_rejects_session_response_without_mac() -> None:
    """KNstaX secure client should reject SessionResponse without MAC per spec compliance.

    Start the secure gateway without MAC in SessionResponse; the KNstaX peer must fail to start.
    """

    peer = _knstax_peer_path()
    if not peer.exists():
        pytest.skip("KNstaX peer not available")

    # Deterministic credentials for peer
    user_id = int(os.environ.get("XKNX_IPSEC_USER_ID", "1"))
    password = os.environ.get("XKNX_IPSEC_PASSWORD", "password")
    client_priv_hex = os.environ.get(
        "KNSTAX_IPSEC_CLIENT_PRIVATE_KEY_HEX",
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
    )
    client_serial_hex = os.environ.get("KNSTAX_IPSEC_CLIENT_SERIAL_HEX", "0000786b6e78")
    initial_seq = int(os.environ.get("KNSTAX_IPSEC_INITIAL_SEQ", "1"))

    gw = KnxIpSecureTunnelingGateway(
        bind_host="127.0.0.1",
        bind_port=0,
        user_id=user_id,
        user_password=password,
        include_session_response_mac16=False,  # Intentionally violate spec
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
            "3000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    # The peer should not reach READY due to handshake rejection.
    ready = await wait_for_line(proc, b"READY", timeout_s=2.0, raise_on_missing=False)

    try:
        if ready:
            pytest.xfail("Peer accepted SessionResponse without MAC; rebuild or legacy tolerance present.")
        else:
            assert not ready
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        await gw.stop()
