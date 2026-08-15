# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import os
import pytest

from knip_gateway.tunnel_gateway import KnxIpTunnelingGateway


@pytest.mark.asyncio
async def test_udp_tunneling_handshake_connect_state_disconnect() -> None:
    """xknx should establish a UDP tunneling session to our gateway, send a state request, then disconnect cleanly."""

    try:
        from xknx import XKNX
        from xknx.io import ConnectionConfig, ConnectionType
    except Exception as exc:  # pragma: no cover
        pytest.skip(f"xknx not available: {exc}")

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    await asyncio.sleep(0.05)
    host, port = gw.address

    xknx_ia = os.environ.get("XKNX_IA") or "1.1.2"

    cfg = ConnectionConfig(
        connection_type=ConnectionType.TUNNELING,
        gateway_ip=host,
        gateway_port=port,
        local_ip="127.0.0.1",
        route_back=True,
        individual_address=xknx_ia,
    )
    xknx = XKNX(connection_config=cfg)

    await xknx.start()
    try:
        # Give xknx time to send initial ConnectionStateRequest and acknowledge.
        await asyncio.sleep(0.2)
        # Expect a connected channel allocated.
        assert len(gw.connected_channels()) >= 1
    finally:
        # Ensure disconnect is processed cleanly.
        await xknx.stop()
        await asyncio.sleep(0.1)
        gw.stop()

    # Verify gateway observed disconnect.
    assert gw.last_disconnect_channel is not None
