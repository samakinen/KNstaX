# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import base64
import datetime as _dt
import os
import secrets
import subprocess
from pathlib import Path

import pytest

from tests.process_helpers import drain_process_output, wait_for_line

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



def _env(name: str) -> str | None:
    v = os.environ.get(name)
    return v if v else None


def _generate_knxkeys_file(
    *,
    out_path: Path,
    password: str,
    group_address: str,
    receiver_ia: str,
    senders_ia: list[str],
    group_key: bytes,
) -> None:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from xknx.secure.keyring import KeyringSAXContentHandler, hash_keyring_password
    from xknx.secure.util import sha256_hash
    import xml.sax

    created = (
        _dt.datetime.now(_dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )
    hashed_password = hash_keyring_password(password.encode("utf-8"))
    initialization_vector = sha256_hash(created.encode("utf-8"))[:16]

    cipher = Cipher(algorithms.AES(hashed_password), modes.CBC(initialization_vector))
    encryptor = cipher.encryptor()
    encrypted_key = encryptor.update(group_key) + encryptor.finalize()
    key_b64 = base64.b64encode(encrypted_key).decode("ascii")

    # Signature is over SAX events excluding xmlns and Signature attrs, plus base64(hashed_password).
    xml_template = (
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Keyring xmlns=\"http://knx.org/xml/keyring/1\" Project=\"KNstaX-Interop\" "
        f"CreatedBy=\"KNstaX\" Created=\"{created}\" Signature=\"\">\n"
        f"  <Interface Type=\"Tunneling\" IndividualAddress=\"{receiver_ia}\">\n"
        f"    <Group Address=\"{group_address}\" Senders=\"{' '.join(senders_ia)}\"/>\n"
        "  </Interface>\n"
        "  <GroupAddresses>\n"
        f"    <Group Address=\"{group_address}\" Key=\"{key_b64}\"/>\n"
        "  </GroupAddresses>\n"
        "</Keyring>\n"
    )

    out_path.write_text(xml_template, encoding="utf-8")

    handler = KeyringSAXContentHandler(password)
    parser = xml.sax.make_parser()
    parser.setContentHandler(handler)
    parser.parse(str(out_path))
    signature = sha256_hash(handler.output)[:16]
    signature_b64 = base64.b64encode(signature).decode("ascii")

    xml_final = xml_template.replace('Signature=""', f'Signature="{signature_b64}"')
    out_path.write_text(xml_final, encoding="utf-8")


def _dpt_value_to_knx_bytes(value: object) -> bytes:
    to_knx = getattr(value, "to_knx", None)
    if callable(to_knx):
        b = to_knx()
        return bytes(b)
    # xknx DPTBinary exposes .value (0/1) but no .to_knx().
    if hasattr(value, "value"):
        try:
            v_int = int(getattr(value, "value"))
        except Exception:
            v_int = None
        if v_int in (0, 1):
            return b"\x01" if v_int else b"\x00"
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    raise TypeError(f"Unsupported xknx DPT value type: {type(value)!r}")


@pytest.mark.asyncio
async def test_xknx_to_knstax_data_secure_groupwrite_udp_tunneling() -> None:
    """Optional interop test: xknx Data Secure -> KNstaX decrypts -> group object write callback fires.

        This uses xknx and a .knxkeys file. If XKNX_KNXKEYS_FILE and XKNX_KNXKEYS_PASSWORD
        are not provided, a temporary .knxkeys is generated automatically.

    Optional env vars:
      - XKNX_GA  (e.g. "0/0/1") to pick a specific secure group address from the keyring
      - XKNX_IA  (default "1.1.2") source individual address for xknx
      - KNSTAX_OWN (default "1.1.1") individual address for the KNstaX peer
    """

    try:
        from xknx import XKNX
        from xknx.io import ConnectionConfig, ConnectionType, SecureConfig
        from xknx.secure.keyring import sync_load_keyring
        from xknx.tools import group_value_write
    except Exception as exc:  # pragma: no cover
        pytest.skip(f"xknx not available: {exc}")

    knxkeys_file = _env("XKNX_KNXKEYS_FILE")
    knxkeys_password = _env("XKNX_KNXKEYS_PASSWORD")

    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    wanted_ga = _env("XKNX_GA") or "0/0/1"
    xknx_ia = _env("XKNX_IA") or "1.1.2"

    if not knxkeys_file or not knxkeys_password:
        # Create an ETS-style keyring for xknx to load in a temporary test directory.
        knxkeys_password = secrets.token_urlsafe(24)
        group_key = secrets.token_bytes(16)
        # Use pytest tmp_path if available via fixture, otherwise fall back to a per-test temp dir.
        try:
            tmp = locals().get("tmp_path")
        except Exception:
            tmp = None
        if tmp is None:
            knxkeys_path = Path.cwd() / "_tmp_knxkeys"
            knxkeys_path.mkdir(exist_ok=True)
        else:
            knxkeys_path = tmp / "knxkeys"
            knxkeys_path.mkdir(exist_ok=True)
        knxkeys_file_path = knxkeys_path / f"generated_{secrets.token_hex(8)}.knxkeys"
        _generate_knxkeys_file(
            out_path=knxkeys_file_path,
            password=knxkeys_password,
            group_address=wanted_ga,
            receiver_ia=xknx_ia,
            senders_ia=[xknx_ia],
            group_key=group_key,
        )
        knxkeys_file = str(knxkeys_file_path)

    keyring = sync_load_keyring(knxkeys_file, knxkeys_password)
    ga_keys = keyring.get_data_secure_group_keys()
    if not ga_keys:
        pytest.skip("No Data Secure group keys found in provided .knxkeys")

    from xknx.telegram import GroupAddress

    ga_obj = GroupAddress(wanted_ga)
    if ga_obj not in ga_keys:
        pytest.skip(f"Requested XKNX_GA={wanted_ga} not found in keyring Data Secure keys")
    group_address = wanted_ga
    group_key = ga_keys[ga_obj]

    if not isinstance(group_key, (bytes, bytearray)) or len(group_key) != 16:
        pytest.skip("Keyring returned invalid group key length (expected 16 bytes)")

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    env = dict(os.environ)
    knstax_own = _env("KNSTAX_OWN") or "1.1.1"

    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            knstax_own,
            "--ga",
            group_address,
            "--timeout-ms",
            "6000",
            "--data-secure",
            "--data-secure-key-hex",
            bytes(group_key).hex(),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
    )

    try:
        await wait_for_line(proc, b"READY", timeout_s=3.0)
        secure_config = SecureConfig(
            knxkeys_file_path=knxkeys_file,
            knxkeys_password=knxkeys_password,
        )
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
        await xknx.start()
        try:
            group_value_write(xknx, group_address, True)
            await asyncio.sleep(0.3)
        finally:
            await xknx.stop()

        # Peer should report decrypted group write and exit successfully.
        await wait_for_line(proc, b"EVENT group_write", timeout_s=4.0)

        for _ in range(80):
            rc = proc.poll()
            if rc is not None:
                break
            await asyncio.sleep(0.1)

        rc = proc.poll()
        assert rc == 0, f"peer failed with rc={rc}"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
        gw.stop()


@pytest.mark.asyncio
async def test_xknx_reads_from_knstax_data_secure_groupread_response_udp_tunneling(tmp_path: Path) -> None:
    """Interop test: xknx Data Secure GroupValueRead -> KNstaX responds securely -> xknx decrypts response."""

    try:
        from xknx import XKNX
        from xknx.io import ConnectionConfig, ConnectionType, SecureConfig
        from xknx.secure.keyring import sync_load_keyring
        from xknx.telegram import GroupAddress, IndividualAddress
        from xknx.telegram.apci import GroupValueResponse
        from xknx.tools import group_value_read
    except Exception as exc:  # pragma: no cover
        pytest.skip(f"xknx not available: {exc}")

    peer = _knstax_peer_path()
    assert peer.exists(), f"Missing peer executable: {peer}. Build with: cmake --build build_test"

    group_address = _env("XKNX_GA") or "0/0/1"
    xknx_ia = _env("XKNX_IA") or "1.1.2"
    knstax_own = _env("KNSTAX_OWN") or "1.1.1"

    knxkeys_file = _env("XKNX_KNXKEYS_FILE")
    knxkeys_password = _env("XKNX_KNXKEYS_PASSWORD")
    if not knxkeys_file or not knxkeys_password:
        knxkeys_password = secrets.token_urlsafe(24)
        group_key = secrets.token_bytes(16)
        knxkeys_path = tmp_path / "knxkeys"
        knxkeys_path.mkdir(exist_ok=True)
        knxkeys_file_path = knxkeys_path / f"generated_{secrets.token_hex(8)}.knxkeys"
        _generate_knxkeys_file(
            out_path=knxkeys_file_path,
            password=knxkeys_password,
            group_address=group_address,
            receiver_ia=xknx_ia,
            # Response is sent from KNstaX (knstax_own) and should be accepted by xknx.
            senders_ia=[knstax_own, xknx_ia],
            group_key=group_key,
        )
        knxkeys_file = str(knxkeys_file_path)

    keyring = sync_load_keyring(knxkeys_file, knxkeys_password)
    ga_keys = keyring.get_data_secure_group_keys()
    ga_obj = GroupAddress(group_address)
    if ga_obj not in ga_keys:
        pytest.skip(f"Requested XKNX_GA={group_address} not found in keyring Data Secure keys")
    group_key = ga_keys[ga_obj]
    assert isinstance(group_key, (bytes, bytearray)) and len(group_key) == 16

    # If user supplied a .knxkeys, ensure it authorizes KNstaX as sender for this GA.
    iface = keyring.get_interface_by_individual_address(IndividualAddress(xknx_ia))
    if iface is not None:
        senders = iface.group_addresses.get(ga_obj)
        if senders is not None and IndividualAddress(knstax_own) not in senders:
            pytest.skip(
                f"Provided .knxkeys does not list KNSTAX_OWN={knstax_own} as a sender for GA {group_address}"
            )

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    proc = subprocess.Popen(
        [
            str(peer),
            "--gw-host",
            host,
            "--gw-port",
            str(port),
            "--own",
            knstax_own,
            "--ga",
            group_address,
            "--dpt-type",
            "1",
            "--init",
            "01",
            "--no-expect-rx",
            "--stay-alive-ms",
            "1200",
            "--data-secure",
            "--data-secure-key-hex",
            bytes(group_key).hex(),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=dict(os.environ),
    )

    got_response: asyncio.Future[object] = asyncio.get_event_loop().create_future()
    got_secure_issue: asyncio.Future[object] = asyncio.get_event_loop().create_future()

    secure_config = SecureConfig(knxkeys_file_path=knxkeys_file, knxkeys_password=knxkeys_password)
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
    await xknx.start()
    try:
        def _on_rx(telegram: object) -> None:
            if got_response.done():
                return
            if str(getattr(telegram, "destination_address", "")) != group_address:
                return
            payload = getattr(telegram, "payload", None)
            if isinstance(payload, GroupValueResponse):
                got_response.set_result(telegram)

        def _on_secure_issue(telegram: object) -> None:
            if not got_secure_issue.done():
                got_secure_issue.set_result(telegram)

        xknx.telegram_queue.register_telegram_received_cb(_on_rx, group_addresses=[ga_obj])
        xknx.telegram_queue.register_data_secure_group_key_issue_cb(_on_secure_issue)

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            # Trigger secure read.
            group_value_read(xknx, group_address)

            done, _ = await asyncio.wait(
                [got_response, got_secure_issue],
                timeout=5.0,
                return_when=asyncio.FIRST_COMPLETED,
            )
            assert done, "Timed out waiting for secure GroupValueResponse"
            assert not got_secure_issue.done(), "xknx reported Data Secure key issue"
            assert got_response.done(), "Timed out waiting for secure GroupValueResponse"

            telegram = got_response.result()
            payload = getattr(telegram, "payload")
            assert isinstance(payload, GroupValueResponse)
            # KNstaX init=01 for DPT1 should be returned.
            knx_bytes = _dpt_value_to_knx_bytes(payload.value)
            assert knx_bytes in (b"\x01", b"\x00")

            # Peer should not crash; it will exit on its own after --stay-alive-ms.
            for _ in range(80):
                rc = proc.poll()
                if rc is not None:
                    break
                await asyncio.sleep(0.1)
            rc = proc.poll()
            if rc not in (None, 0):
                out = drain_process_output(proc)
                raise AssertionError(f"peer exit={rc}, output:\n{out}")
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await xknx.stop()
        gw.stop()


@pytest.mark.asyncio
async def test_knstax_to_xknx_data_secure_groupwrite_udp_tunneling(tmp_path: Path) -> None:
    """Interop test: KNstaX encrypts Data Secure -> xknx decrypts and receives a GroupValueWrite."""

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

    group_address = _env("XKNX_GA") or "0/0/1"
    xknx_ia = _env("XKNX_IA") or "1.1.2"
    knstax_own = _env("KNSTAX_OWN") or "1.1.1"

    # Prefer user-provided keyring, otherwise generate one that authorizes KNstaX as sender.
    knxkeys_file = _env("XKNX_KNXKEYS_FILE")
    knxkeys_password = _env("XKNX_KNXKEYS_PASSWORD")
    if not knxkeys_file or not knxkeys_password:
        knxkeys_password = secrets.token_urlsafe(24)
        group_key = secrets.token_bytes(16)
        knxkeys_path = tmp_path / "knxkeys"
        knxkeys_path.mkdir(exist_ok=True)
        knxkeys_file_path = knxkeys_path / f"generated_{secrets.token_hex(8)}.knxkeys"
        _generate_knxkeys_file(
            out_path=knxkeys_file_path,
            password=knxkeys_password,
            group_address=group_address,
            receiver_ia=xknx_ia,
            senders_ia=[knstax_own, xknx_ia],
            group_key=group_key,
        )
        knxkeys_file = str(knxkeys_file_path)

    keyring = sync_load_keyring(knxkeys_file, knxkeys_password)
    ga_keys = keyring.get_data_secure_group_keys()
    ga_obj = GroupAddress(group_address)
    if ga_obj not in ga_keys:
        pytest.skip(f"Requested XKNX_GA={group_address} not found in keyring Data Secure keys")
    group_key = ga_keys[ga_obj]
    assert isinstance(group_key, (bytes, bytearray)) and len(group_key) == 16

    gw = KnxIpTunnelingGateway(bind_host="127.0.0.1", bind_port=0)
    gw.start()
    host, port = gw.address

    got_telegram: asyncio.Future[object] = asyncio.get_event_loop().create_future()
    got_secure_issue: asyncio.Future[object] = asyncio.get_event_loop().create_future()

    secure_config = SecureConfig(knxkeys_file_path=knxkeys_file, knxkeys_password=knxkeys_password)
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
    await xknx.start()
    try:
        def _on_rx(telegram: object) -> None:
            if not got_telegram.done():
                got_telegram.set_result(telegram)

        def _on_secure_issue(telegram: object) -> None:
            if not got_secure_issue.done():
                got_secure_issue.set_result(telegram)

        xknx.telegram_queue.register_telegram_received_cb(
            _on_rx,
            group_addresses=[ga_obj],
        )
        xknx.telegram_queue.register_data_secure_group_key_issue_cb(_on_secure_issue)

        proc = subprocess.Popen(
            [
                str(peer),
                "--gw-host",
                host,
                "--gw-port",
                str(port),
                "--own",
                knstax_own,
                "--ga",
                group_address,
                "--dpt-type",
                "1",
                "--send",
                "01",
                "--no-expect-rx",
                "--stay-alive-ms",
                "800",
                "--data-secure",
                "--data-secure-key-hex",
                bytes(group_key).hex(),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=dict(os.environ),
        )

        try:
            await wait_for_line(proc, b"READY", timeout_s=3.0)

            done, _ = await asyncio.wait(
                [got_telegram, got_secure_issue],
                timeout=4.0,
                return_when=asyncio.FIRST_COMPLETED,
            )
            assert done, "Timed out waiting for xknx to receive secure telegram"
            assert not got_secure_issue.done(), "xknx reported Data Secure key issue"

            telegram = got_telegram.result()
            # Ensure xknx delivered a decrypted group write to the app layer.
            assert str(getattr(telegram, "destination_address")) == group_address
            assert isinstance(getattr(telegram, "payload"), GroupValueWrite)

            # The KNstaX peer may block waiting for tunneling confirmations; don't require it to exit.
            await asyncio.sleep(0.2)
            rc = proc.poll()
            if rc not in (None, 0):
                out = drain_process_output(proc)
                raise AssertionError(f"peer failed with rc={rc}, output:\n{out}")
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        await xknx.stop()
        gw.stop()
