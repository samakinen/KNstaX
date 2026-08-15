#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
import asyncio
import os
from knip_gateway.ip_secure_tunnel_gateway import KnxIpSecureTunnelingGateway


async def main():
    user_id = int(os.environ.get("XKNX_IPSEC_USER_ID", "1"))
    user_password = os.environ.get("XKNX_IPSEC_PASSWORD", "password")
    gw = KnxIpSecureTunnelingGateway(bind_host="127.0.0.1", bind_port=0, user_id=user_id, user_password=user_password)
    await gw.start()
    host, port = gw.address
    print(f"GW_STARTED {host} {port}", flush=True)
    # keep running until killed
    try:
        await asyncio.Event().wait()
    finally:
        await gw.stop()


if __name__ == "__main__":
    asyncio.run(main())
