# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import asyncio
import traceback
import dataclasses
import ipaddress
from typing import Dict, Optional, Tuple

from . import knxip_codec

# We intentionally reuse xknx's KNX/IP Secure crypto helpers to avoid
# re-implementing AES-CBC-MAC/CTR details in the interop harness.
from xknx.io.ip_secure import (  # type: ignore
    COUNTER_0_HANDSHAKE,
    calculate_message_authentication_code_cbc,
    decrypt_ctr,
    derive_user_password,
    encrypt_data_ctr,
)

from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)


KNXNETIP_HEADER_LEN = 0x06
KNXNETIP_VERSION = 0x10

# KNX/IP Secure session services
ST_SESSION_REQUEST = 0x0951
ST_SESSION_RESPONSE = 0x0952
ST_SECURE_WRAPPER = 0x0950
ST_SESSION_AUTHENTICATE = 0x0953
ST_SESSION_STATUS = 0x0954


def _u16be(b: bytes, off: int) -> int:
    return (b[off] << 8) | b[off + 1]


def _p16be(v: int) -> bytes:
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


async def _read_knxip_frame(reader: asyncio.StreamReader, *, max_len: int = 64 * 1024) -> bytes:
    hdr = await reader.readexactly(6)
    if hdr[0] != KNXNETIP_HEADER_LEN or hdr[1] != KNXNETIP_VERSION:
        raise ValueError("not a KNXnet/IP frame")
    total = _u16be(hdr, 4)
    if total < 6 or total > max_len:
        raise ValueError(f"invalid frame length: {total}")
    rest = await reader.readexactly(total - 6)
    return hdr + rest


def _build_knxip_frame(service_type: int, body: bytes) -> bytes:
    return bytes([KNXNETIP_HEADER_LEN, KNXNETIP_VERSION]) + _p16be(service_type) + _p16be(6 + len(body)) + body


def _parse_session_request(frame: bytes) -> bytes:
    # Body: HPAI (8) + client pub (32)
    if len(frame) != 6 + 8 + 32:
        raise ValueError("invalid SessionRequest length")
    if _u16be(frame, 2) != ST_SESSION_REQUEST:
        raise ValueError("not SessionRequest")
    client_pub = frame[6 + 8 :]
    if len(client_pub) != 32:
        raise ValueError("invalid client pubkey")
    return client_pub


def _build_session_response(
    *,
    session_id: int,
    server_pub: bytes,
    include_mac16: bool,
) -> bytes:
    # xknx expects 2 + 32 + 16 bytes body (MAC may be all zeros if not used)
    body = bytearray()
    body.extend(session_id.to_bytes(2, "big"))
    body.extend(server_pub)
    if include_mac16:
        body.extend(bytes(16))
    return _build_knxip_frame(ST_SESSION_RESPONSE, bytes(body))


def _wrap_secure(
    *,
    session_key: bytes,
    session_id: int,
    sequence_information: bytes,
    serial_number: bytes,
    message_tag: bytes,
    inner_frame: bytes,
) -> bytes:
    payload_length = len(inner_frame)
    total_length = 38 + payload_length
    wrapper_header = bytes.fromhex("06 10 09 50") + total_length.to_bytes(2, "big")

    mac_cbc = calculate_message_authentication_code_cbc(
        key=session_key,
        additional_data=wrapper_header + session_id.to_bytes(2, "big"),
        payload=inner_frame,
        block_0=(sequence_information + serial_number + message_tag + payload_length.to_bytes(2, "big")),
    )

    encrypted_data, mac_tr = encrypt_data_ctr(
        key=session_key,
        counter_0=(sequence_information + serial_number + message_tag + b"\xff\x00"),
        mac_cbc=mac_cbc,
        payload=inner_frame,
    )

    body = (
        session_id.to_bytes(2, "big")
        + sequence_information
        + serial_number
        + message_tag
        + encrypted_data
        + mac_tr
    )
    return _build_knxip_frame(ST_SECURE_WRAPPER, body)


def _unwrap_secure(
    *,
    session_key: bytes,
    expected_session_id: int,
    frame: bytes,
) -> Tuple[bytes, bytes, bytes, bytes]:
    # Returns (sequence_information, serial_number, message_tag, inner_frame)
    if len(frame) < 6 + 38:
        raise ValueError("SecureWrapper too short")
    if _u16be(frame, 2) != ST_SECURE_WRAPPER:
        raise ValueError("not SecureWrapper")

    sid = int.from_bytes(frame[6:8], "big")
    if sid != expected_session_id:
        raise ValueError("invalid session id")

    seq = frame[8:14]
    serial = frame[14:20]
    tag = frame[20:22]
    encrypted_data = frame[22:-16]
    mac = frame[-16:]

    wrapper_header = frame[:6]

    dec_frame, mac_tr = decrypt_ctr(
        key=session_key,
        counter_0=(seq + serial + tag + b"\xff\x00"),
        mac=mac,
        payload=encrypted_data,
    )

    mac_cbc = calculate_message_authentication_code_cbc(
        key=session_key,
        additional_data=wrapper_header + sid.to_bytes(2, "big"),
        payload=dec_frame,
        block_0=(seq + serial + tag + len(dec_frame).to_bytes(2, "big")),
    )

    if mac_cbc != mac_tr:
        raise ValueError("SecureWrapper MAC verification failed")

    return seq, serial, tag, dec_frame


def _verify_session_authenticate(
    *,
    user_password_key: bytes,
    user_id_expected: int,
    client_pub: bytes,
    server_pub: bytes,
    authenticate_inner: bytes,
) -> None:
    # Plain inner: header(6) + reserved(1) + userId(1) + mac(16)
    if len(authenticate_inner) != 0x18:
        raise ValueError("invalid SessionAuthenticate length")
    if authenticate_inner[0] != 0x06 or authenticate_inner[1] != 0x10:
        raise ValueError("invalid SessionAuthenticate header")
    if _u16be(authenticate_inner, 2) != ST_SESSION_AUTHENTICATE:
        raise ValueError("not SessionAuthenticate")

    user_id = authenticate_inner[7]
    if user_id != (user_id_expected & 0xFF):
        raise ValueError("unexpected user_id")

    pub_keys_xor = bytes(a ^ b for a, b in zip(client_pub, server_pub))
    authenticate_header_data = bytes.fromhex("06 10 09 53 00 18")

    authenticate_mac_cbc = calculate_message_authentication_code_cbc(
        key=user_password_key,
        additional_data=(authenticate_header_data + bytes(1) + bytes([user_id]) + pub_keys_xor),
        block_0=bytes(16),
    )
    _, authenticate_mac = encrypt_data_ctr(
        key=user_password_key,
        counter_0=COUNTER_0_HANDSHAKE,
        mac_cbc=authenticate_mac_cbc,
    )

    if authenticate_inner[8:] != authenticate_mac:
        raise ValueError("SessionAuthenticate MAC verification failed")


@dataclasses.dataclass
class _SecureClient:
    peer: Tuple[str, int]
    writer: asyncio.StreamWriter

    session_id: int
    session_key: bytes
    user_id: int

    # SecureWrapper fields
    outgoing_seq: int = 0
    last_incoming_seq: int = -1
    serial: bytes = b"\x00\x00PyGW"  # 6 bytes, deterministic

    channel_id: int = 0
    next_tunnel_seq_to_client: int = 0


class KnxIpSecureTunnelingGateway:
    """Minimal KNX/IP Secure tunnelling gateway simulator (TCP).

    Implements just enough of:
    - SECURE_SESSION_REQUEST/RESPONSE/AUTHENTICATE/STATUS handshake
    - then SecureWrapper-wrapped KNXnet/IP tunneling:
      CONNECTION_REQUEST/RESPONSE, CONNECTIONSTATE, DISCONNECT,
      TUNNELING_REQUEST/ACK and fan-out forwarding.

    Designed for deterministic interop tests (2 clients is the typical use).
    """

    def __init__(
        self,
        *,
        bind_host: str = "127.0.0.1",
        bind_port: int = 0,
        user_id: int = 1,
        user_password: str = "password",
        server_private_key_bytes: Optional[bytes] = None,
        include_session_response_mac16: bool = True,
    ):
        self.bind_host = bind_host
        self.bind_port = bind_port

        self.user_id = user_id
        # xknx derives user_password key from a string. Reuse it.
        self._user_password_key: bytes = derive_user_password(user_password)

        self._include_session_response_mac16 = include_session_response_mac16

        if server_private_key_bytes is None:
            # Deterministic, non-zero scalar. (Test-only)
            server_private_key_bytes = bytes(range(1, 33))
        if len(server_private_key_bytes) != 32:
            raise ValueError("server_private_key_bytes must be 32 bytes")
        self._server_private_key = X25519PrivateKey.from_private_bytes(server_private_key_bytes)

        self._server: Optional[asyncio.base_events.Server] = None
        self._bound: Optional[Tuple[str, int]] = None

        self._clients: Dict[int, _SecureClient] = {}
        self._next_session_id = 1
        self._next_channel_id = 1
        self._lock = asyncio.Lock()

        # Minimal diagnostics for tests
        self.rx_secure_wrapper = 0
        self.rx_tunnel_req = 0
        self.tx_forward = 0

    @property
    def address(self) -> Tuple[str, int]:
        if not self._bound:
            raise RuntimeError("gateway not started")
        return self._bound

    async def start(self) -> None:
        if self._server is not None:
            return

        self._server = await asyncio.start_server(self._handle_client, host=self.bind_host, port=self.bind_port)
        sock = self._server.sockets[0]
        host, port = sock.getsockname()[0], sock.getsockname()[1]
        self._bound = (host, port)

    async def stop(self) -> None:
        if self._server is None:
            return
        self._server.close()
        await self._server.wait_closed()
        self._server = None
        self._bound = None
        async with self._lock:
            self._clients.clear()

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        peer_t = (str(peer[0]), int(peer[1])) if peer else ("?", 0)

        try:
            # 1) SessionRequest
            req = await _read_knxip_frame(reader)
            client_pub = _parse_session_request(req)
            try:
                print(f"GW: session_request from {peer_t}")
            except Exception:
                pass

            # 2) SessionResponse
            session_id = self._next_session_id
            self._next_session_id += 1

            server_pub_bytes = self._server_private_key.public_key().public_bytes_raw()
            resp = _build_session_response(
                session_id=session_id,
                server_pub=server_pub_bytes,
                include_mac16=self._include_session_response_mac16,
            )
            writer.write(resp)
            await writer.drain()
            try:
                print(f"GW: session_response sid={session_id}")
            except Exception:
                pass

            # Derive session key
            peer_pub = X25519PublicKey.from_public_bytes(client_pub)
            secret = self._server_private_key.exchange(peer_pub)
            # sha256(secret)[:16] like xknx/KNstaX
            from hashlib import sha256

            session_key = sha256(secret).digest()[:16]

            # 3) SessionAuthenticate in SecureWrapper
            wrapped = await _read_knxip_frame(reader)
            seq, serial, tag, inner = _unwrap_secure(
                session_key=session_key,
                expected_session_id=session_id,
                frame=wrapped,
            )
            try:
                print(f"GW: session_auth received sid={session_id} tag={tag.hex()} serial={serial.hex()}")
            except Exception:
                pass
            if tag != b"\x00\x00":
                raise ValueError("expected tunneling tag 0x0000")

            _verify_session_authenticate(
                user_password_key=self._user_password_key,
                user_id_expected=self.user_id,
                client_pub=client_pub,
                server_pub=server_pub_bytes,
                authenticate_inner=inner,
            )

            # 4) SessionStatus success (wrapped)
            status_inner = _build_knxip_frame(ST_SESSION_STATUS, bytes([0x00, 0x00]))
            status_wrapped = _wrap_secure(
                session_key=session_key,
                session_id=session_id,
                sequence_information=(0).to_bytes(6, "big"),
                serial_number=b"\x00\x00PyGW",
                message_tag=b"\x00\x00",
                inner_frame=status_inner,
            )
            writer.write(status_wrapped)
            await writer.drain()
            try:
                print(f"GW: session_status sent sid={session_id}")
            except Exception:
                pass

            client = _SecureClient(
                peer=peer_t,
                writer=writer,
                session_id=session_id,
                session_key=session_key,
                user_id=self.user_id,
            )

            # We already used outgoing sequence number 0 for SessionStatus.
            client.outgoing_seq = 1

            async with self._lock:
                self._clients[session_id] = client
            try:
                print(f"GW: register session sid={session_id} peer={client.peer}")
            except Exception:
                pass

            # Main secured tunneling loop
            while True:
                frame = await _read_knxip_frame(reader)
                if _u16be(frame, 2) != ST_SECURE_WRAPPER:
                    continue

                self.rx_secure_wrapper += 1
                try:
                    # Print a short hex snippet of the wrapper for correlation (trim to 128 bytes).
                    hex_snip = frame.hex()[:256]
                    print(f"GW: secure_wrapper sid={session_id} len={len(frame)} hex={hex_snip}")
                except Exception:
                    pass
                seq, serial, tag, inner = _unwrap_secure(
                    session_key=session_key,
                    expected_session_id=session_id,
                    frame=frame,
                )
                seq_i = int.from_bytes(seq, "big")
                if seq_i <= client.last_incoming_seq:
                    # Drop replay/out-of-order
                    continue
                client.last_incoming_seq = seq_i

                try:
                    hdr = knxip_codec.parse_header(inner)
                except Exception:
                    continue

                try:
                    print(f"GW: inner sid={session_id} svc=0x{hdr.service_type:04x} chan={getattr(hdr, 'channel_id', None)}")
                except Exception:
                    pass

                if hdr.service_type == knxip_codec.ST_CONNECTION_REQUEST:
                    await self._handle_connect(inner, client)
                elif hdr.service_type == knxip_codec.ST_CONNECTIONSTATE_REQUEST:
                    await self._handle_state(inner, client)
                elif hdr.service_type == knxip_codec.ST_DISCONNECT_REQUEST:
                    await self._handle_disconnect(inner, client)
                    break
                elif hdr.service_type == knxip_codec.ST_TUNNELING_REQUEST:
                    await self._handle_tunneling_request(inner, client)
                elif hdr.service_type == knxip_codec.ST_TUNNELING_ACK:
                    # Accept silently
                    continue

        except (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError):
            pass
        except Exception as exc:
            try:
                print(f"GW: exception in client handler peer={peer_t}: {type(exc).__name__}: {exc}")
                traceback.print_exc()
            except Exception:
                pass
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            finally:
                async with self._lock:
                    # Remove by session_id if present
                    for sid, c in list(self._clients.items()):
                        if c.writer is writer:
                            self._clients.pop(sid, None)

    async def _send_secure(self, client: _SecureClient, inner_frame: bytes) -> None:
        seq_bytes = client.outgoing_seq.to_bytes(6, "big")
        client.outgoing_seq += 1

        wrapped = _wrap_secure(
            session_key=client.session_key,
            session_id=client.session_id,
            sequence_information=seq_bytes,
            serial_number=client.serial,
            message_tag=b"\x00\x00",
            inner_frame=inner_frame,
        )
        try:
            hex_snip = wrapped.hex()[:256]
            print(f"GW: send_secure sid={client.session_id} len={len(wrapped)} hex={hex_snip}")
        except Exception:
            pass
        client.writer.write(wrapped)
        await client.writer.drain()

    async def _handle_connect(self, inner: bytes, client: _SecureClient) -> None:
        async with self._lock:
            if client.channel_id == 0:
                client.channel_id = self._next_channel_id & 0xFF
                self._next_channel_id += 1

        gw_ip, gw_port = self.address
        resp = knxip_codec.build_connect_response(
            channel_id=client.channel_id,
            status=knxip_codec.E_NO_ERROR,
            data_endpoint=knxip_codec.Hpai(ip=gw_ip, port=gw_port),
            hpai_proto=knxip_codec.HPAI_PROTO_TCP,
        )
        await self._send_secure(client, resp)

    async def _handle_state(self, inner: bytes, client: _SecureClient) -> None:
        ch, _ctrl_ep = knxip_codec.parse_connectionstate_request(inner)
        if ch is None or int(ch) != int(client.channel_id):
            return
        resp = knxip_codec.build_connectionstate_response(client.channel_id, knxip_codec.E_NO_ERROR)
        await self._send_secure(client, resp)

    async def _handle_disconnect(self, inner: bytes, client: _SecureClient) -> None:
        ch, _ctrl_ep = knxip_codec.parse_disconnect_request(inner)
        if ch is None or int(ch) != int(client.channel_id):
            return
        resp = knxip_codec.build_disconnect_response(client.channel_id, knxip_codec.E_NO_ERROR)
        await self._send_secure(client, resp)

    async def _handle_tunneling_request(self, inner: bytes, client: _SecureClient) -> None:
        req = knxip_codec.parse_tunneling_request(inner)
        if not req:
            return
        if req.channel_id != client.channel_id:
            return

        self.rx_tunnel_req += 1
        try:
            cemi_hex = req.cemi.hex()[:256]
            print(f"GW: tunneling_request from sid={client.session_id} ch={req.channel_id} seq={req.seq} cemi_len={len(req.cemi)} cemi={cemi_hex}")
        except Exception:
            pass

        # ACK back to sender
        ack = knxip_codec.build_tunneling_ack(req.channel_id, req.seq, knxip_codec.E_NO_ERROR)
        try:
            print(f"GW: send_ack to sid={client.session_id} ch={req.channel_id} seq={req.seq} ack_len={len(ack)}")
        except Exception:
            pass
        await self._send_secure(client, ack)

        # Also synthesize a TP1-style confirmation for the sender.
        # xknx expects to receive an L_DATA_CON (0x2E) for its L_DATA_REQ.
        # In a real gateway this comes from the bus side; here we generate it deterministically.
        cemi_con = req.cemi
        if len(cemi_con) >= 1:
            cemi_con = bytes((0x2E,)) + cemi_con[1:]
        pkt_con = knxip_codec.build_tunneling_request(
            client.channel_id,
            client.next_tunnel_seq_to_client & 0xFF,
            cemi_con,
        )
        client.next_tunnel_seq_to_client = (client.next_tunnel_seq_to_client + 1) & 0xFF
        try:
            print(
                f"GW: synth_con to sid={client.session_id} ch={client.channel_id} seq={req.seq} "
                f"pkt_len={len(pkt_con)}"
            )
        except Exception:
            pass
        await self._send_secure(client, pkt_con)

        # Forward cEMI to all other clients as L_Data.ind (0x29).
        cemi = req.cemi
        if len(cemi) >= 1:
            cemi = bytes((0x29,)) + cemi[1:]

        async with self._lock:
            others = [c for c in self._clients.values() if c.session_id != client.session_id and c.channel_id != 0]

        for other in others:
            pkt = knxip_codec.build_tunneling_request(other.channel_id, other.next_tunnel_seq_to_client & 0xFF, cemi)
            other.next_tunnel_seq_to_client = (other.next_tunnel_seq_to_client + 1) & 0xFF
            try:
                print(f"GW: forward to sid={other.session_id} ch={other.channel_id} pkt_len={len(pkt)} pkt_hex={pkt.hex()[:256]}")
            except Exception:
                pass
            await self._send_secure(other, pkt)
            self.tx_forward += 1
