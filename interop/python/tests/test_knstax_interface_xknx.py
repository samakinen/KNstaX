# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

"""xknx tunnelling through a KNstaX KNXnet/IP interface (tunnelling server +
TP1 bridge) on a scripted TP1 line.

xknx sends each L_Data.req and waits for its L_Data.con (03_06_03 4.1.5.1;
xknx raises ConfirmationError after 3 s without one), so a group write that
returns at all shows the interface confirms. The tests also check the
Confirm flag, the tunnel address on the line and an L_Data.ind from TP1.
"""

from __future__ import annotations

import asyncio
import os
import subprocess
from pathlib import Path

import pytest

from tests.process_helpers import wait_for_line


def _interface_peer_path() -> Path:
    v = os.environ.get("KNSTAX_INTERFACE_PEER_BIN")
    if not v:
        routing = os.environ.get("KNSTAX_ROUTING_PEER_BIN")
        if not routing:
            raise RuntimeError("KNSTAX_INTERFACE_PEER_BIN (or KNSTAX_ROUTING_PEER_BIN) must be set")
        v = str(Path(routing).with_name("knstax_interface_peer"))
    p = Path(v)
    if not p.exists():
        raise RuntimeError(f"knstax_interface_peer not found: {v}")
    return p


def _pick_port(salt: int) -> int:
    return 30000 + ((os.getpid() + salt) % 5000)


async def _run_interface_case(tx_outcome: str, salt: int) -> tuple[list[bytes], list[str], bytes]:
    from xknx import XKNX
    from xknx.cemi.cemi_handler import CEMIHandler
    from xknx.dpt import DPTBinary
    from xknx.io import ConnectionConfig, ConnectionType
    from xknx.telegram import GroupAddress, Telegram
    from xknx.telegram.apci import GroupValueWrite

    port = _pick_port(salt)
    proc = subprocess.Popen(
        [
            str(_interface_peer_path()),
            "--port", str(port),
            "--tunnel", "1.1.10",
            "--tunnel", "1.1.11",
            "--tx-outcome", tx_outcome,
            "--inject-ga", "1/2/3",
            "--inject-after-ms", "300",
            "--timeout-ms", "6000",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    raw_cemis: list[bytes] = []
    original = CEMIHandler.handle_raw_cemi

    def recording(self: CEMIHandler, raw_cemi: bytes) -> None:
        raw_cemis.append(bytes(raw_cemi))
        original(self, raw_cemi)

    received: list[str] = []
    CEMIHandler.handle_raw_cemi = recording  # type: ignore[method-assign]
    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)
        xknx = XKNX(
            connection_config=ConnectionConfig(
                connection_type=ConnectionType.TUNNELING,
                gateway_ip="127.0.0.1",
                gateway_port=port,
                local_ip="127.0.0.1",
            )
        )
        xknx.telegram_queue.register_telegram_received_cb(
            lambda telegram: received.append(str(telegram.destination_address)),
        )
        await xknx.start()
        try:
            assert str(xknx.current_address) == "1.1.10"
            # Returns only once the L_Data.con has arrived.
            await xknx.cemi_handler.send_telegram(
                Telegram(destination_address=GroupAddress("1/2/4"), payload=GroupValueWrite(DPTBinary(1)))
            )
            for _ in range(50):
                if "1/2/3" in received:
                    break
                await asyncio.sleep(0.05)
        finally:
            await xknx.stop()

        await wait_for_line(proc, b"TP1_TX src=1.1.10 dst=1/2/4 group=1", timeout_s=2.0)
    finally:
        CEMIHandler.handle_raw_cemi = original  # type: ignore[method-assign]
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
    output = proc.stdout.read() if proc.stdout else b""
    return raw_cemis, received, output


def _confirmations(raw_cemis: list[bytes]) -> list[bytes]:
    return [raw for raw in raw_cemis if raw and raw[0] == 0x2E]


def _confirm_flag(raw: bytes) -> int:
    ctrl1 = raw[2 + raw[1]]
    return ctrl1 & 0x01


@pytest.mark.asyncio
async def test_xknx_group_write_through_knstax_interface_is_confirmed() -> None:
    raw_cemis, received, _ = await _run_interface_case("success", salt=11)
    confirmations = _confirmations(raw_cemis)
    assert len(confirmations) == 1, raw_cemis
    assert _confirm_flag(confirmations[0]) == 0
    # The group write the line received reached xknx as an L_Data.ind.
    assert "1/2/3" in received


@pytest.mark.asyncio
async def test_xknx_group_write_through_knstax_interface_negative_confirmation() -> None:
    raw_cemis, _, _ = await _run_interface_case("nak", salt=12)
    confirmations = _confirmations(raw_cemis)
    assert len(confirmations) == 1, raw_cemis
    assert _confirm_flag(confirmations[0]) == 1
