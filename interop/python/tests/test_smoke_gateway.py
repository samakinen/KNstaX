# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from knip_gateway.tunnel_gateway import KnxIpTunnelingGateway


def test_gateway_starts_and_binds():
    gw = KnxIpTunnelingGateway()
    gw.start()
    host, port = gw.address
    assert host in ("127.0.0.1", "0.0.0.0")
    assert isinstance(port, int)
    assert port > 0
    gw.stop()
