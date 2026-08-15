#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
import asyncio
import os
import sys
from xknx import XKNX
from xknx.io import ConnectionConfig, ConnectionType, SecureConfig
from xknx.tools import group_value_write, group_value_read


async def main():
    if len(sys.argv) < 3:
        print("Usage: run_xknx_client.py <host> <port> [group]")
        return 2
    host = sys.argv[1]
    port = int(sys.argv[2])
    group = sys.argv[3] if len(sys.argv) > 3 else "0/0/1"

    user_id = int(os.environ.get("XKNX_IPSEC_USER_ID", "1"))
    user_password = os.environ.get("XKNX_IPSEC_PASSWORD", "password")
    secure_config = SecureConfig(user_id=user_id, user_password=user_password)

    cfg = ConnectionConfig(connection_type=ConnectionType.TUNNELING_TCP_SECURE, gateway_ip=host, gateway_port=port)
    cfg.secure_config = secure_config

    xknx = XKNX(connection_config=cfg)
    await xknx.start()
    try:
        print("xknx: sending GroupValueWrite True", flush=True)
        group_value_write(xknx, group, True)
        await asyncio.sleep(0.5)
        print("xknx: sending GroupValueRead", flush=True)
        group_value_read(xknx, group)
        await asyncio.sleep(1.0)
    finally:
        await xknx.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
