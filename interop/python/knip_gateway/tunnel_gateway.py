# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

from __future__ import annotations

import dataclasses
import socket
import threading
import time
from typing import Dict, Optional, Tuple

from . import knxip_codec


@dataclasses.dataclass
class TunnelClient:
    addr: Tuple[str, int]
    channel_id: int
    assigned_ia_raw: int
    send_sock: Optional[socket.socket] = None
    last_seq_from_client: Optional[int] = None
    next_seq_to_client: int = 0
    last_seen: float = dataclasses.field(default_factory=time.time)


class KnxIpTunnelingGateway:
    """Minimal KNXnet/IP tunnelling gateway simulator.

    Implements just enough of:
    - CONNECTION_REQUEST (0x0205) -> CONNECTION_RESPONSE (0x0206)
    - TUNNELING_REQUEST (0x0420) -> TUNNELING_ACK (0x0421)

    And forwards received TUNNELING_REQUEST payloads (cEMI) to all *other*
    connected clients (fan-out bus simulation).

    This is intentionally minimal and designed for deterministic interop tests.
    """

    def __init__(self, bind_host: str = "127.0.0.1", bind_port: int = 0):
        self.bind_host = bind_host
        self.bind_port = bind_port

        self._sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()

        # Clients can use different source ports for control/data.
        # Track by channel-id and learn addr aliases on first use.
        self._clients_by_channel: Dict[int, TunnelClient] = {}
        self._addr_to_channel: Dict[Tuple[str, int], int] = {}
        self._next_channel = 1
        self._lock = threading.Lock()

        # Stats for tests/debugging (no logging).
        self.rx_connect = 0
        self.rx_state = 0
        self.rx_disconnect = 0
        self.rx_tunnel_req = 0
        self.rx_tunnel_ack = 0
        self.tx_connect_resp = 0
        self.tx_state_resp = 0
        self.tx_disconnect_resp = 0
        self.tx_tunnel_ack = 0
        self.tx_forward = 0
        self.connect_send_errors = 0
        self.connect_send_last_error: Optional[str] = None

        # Minimal diagnostics (kept small; useful in tests on failure).
        self.last_tunnel_sender_channel: Optional[int] = None
        self.last_tunnel_forwarded_to_channels: Tuple[int, ...] = ()
        self.forward_send_errors: int = 0

        self.last_rx_service_type: Optional[int] = None
        self.last_rx_header6: Optional[bytes] = None
        self.last_rx_from: Optional[Tuple[str, int]] = None

        self.last_disconnect_channel: Optional[int] = None
        self.last_disconnect_from: Optional[Tuple[str, int]] = None

    def connected_channels(self) -> Tuple[int, ...]:
        with self._lock:
            return tuple(sorted(self._clients_by_channel.keys()))

    @property
    def address(self) -> Tuple[str, int]:
        if not self._sock:
            raise RuntimeError("gateway not started")
        return self._sock.getsockname()[0], self._sock.getsockname()[1]

    def start(self) -> None:
        if self._sock:
            return
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.bind_host, self.bind_port))
        sock.settimeout(0.2)
        self._sock = sock

        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="knxip-gw", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)
        self._thread = None
        if self._sock:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def _run(self) -> None:
        assert self._sock is not None
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break

            if len(data) >= 6:
                self.last_rx_service_type = (data[2] << 8) | data[3]
                self.last_rx_header6 = bytes(data[:6])
                self.last_rx_from = addr

            try:
                hdr = knxip_codec.parse_header(data)
            except Exception:
                continue

            st = hdr.service_type
            if st == knxip_codec.ST_CONNECTION_REQUEST:
                self._handle_connection_request(data, addr)
            elif st == knxip_codec.ST_CONNECTIONSTATE_REQUEST:
                self._handle_connectionstate_request(data, addr)
            elif st == knxip_codec.ST_DISCONNECT_REQUEST:
                self._handle_disconnect_request(data, addr)
            elif st == knxip_codec.ST_TUNNELING_REQUEST:
                self._handle_tunneling_request(data, addr)
            elif st == knxip_codec.ST_TUNNELING_ACK:
                self._handle_tunneling_ack(data, addr)


    def _handle_connection_request(self, pkt: bytes, addr: Tuple[str, int]) -> None:
        assert self._sock is not None
        self.rx_connect += 1

        # Minimal inline diagnostics to help debug handshake issues in tests.
        # Kept concise to avoid noisy output.
        # Format: GW: rx_connect from (ip,port)
        try:
            print(f"GW: rx_connect from {addr}")
        except Exception:
            pass

        ctrl_ep, data_ep = knxip_codec.parse_connect_request(pkt)
        with self._lock:
            existing_ch = self._addr_to_channel.get(addr)
            if existing_ch is not None and existing_ch in self._clients_by_channel:
                ch = existing_ch
            else:
                ch = self._next_channel & 0xFF
                self._next_channel += 1
                assigned_raw = 0x1100 + ch
                client = TunnelClient(addr=addr, channel_id=ch, assigned_ia_raw=assigned_raw)
                # Create a per-client connected sending socket to satisfy peers that filter by source port.
                try:
                    client.send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    # Prefer control endpoint if present, otherwise use source address.
                    dest = (ctrl_ep.ip, int(ctrl_ep.port)) if ctrl_ep is not None and int(ctrl_ep.port) != 0 else addr
                    client.send_sock.connect(dest)
                except OSError:
                    client.send_sock = None
                self._clients_by_channel[ch] = client
                self._addr_to_channel[addr] = ch

            # Learn data endpoint alias if provided (some clients use a different local port).
            if data_ep is not None and data_ep.port != 0:
                ep_tuple = (data_ep.ip, int(data_ep.port))
                self._addr_to_channel.setdefault(ep_tuple, ch)

        # Use the gateway bind address as data endpoint.
        gw_ip, gw_port = self.address
        resp = knxip_codec.build_connect_response(
            channel_id=ch,
            status=knxip_codec.E_NO_ERROR,
            data_endpoint=knxip_codec.Hpai(ip=gw_ip, port=gw_port),
        )
        # Prefer replying to the observed source address (addr) to ensure
        # loopback tests receive responses from 127.0.0.1. Use the control
        # HPAI as a fallback when present.
        dests = [addr]
        if ctrl_ep is not None and int(ctrl_ep.port) != 0 and ctrl_ep.ip != "0.0.0.0":
            dests.append((ctrl_ep.ip, int(ctrl_ep.port)))

        # Always reply from the gateway's bound socket so source port matches
        # the advertised control endpoint and client expectations.
        # Best-effort sendto paths with fallbacks.
        for dest in dests:
            try:
                # Use sendto from the gateway's bound socket so the source
                # address/port match the gateway's advertised data endpoint.
                self._sock.sendto(resp, dest)
                self.tx_connect_resp += 1
                try:
                    print(f"GW: sent CONN_RESP to {dest}")
                except Exception:
                    pass
                break
            except OSError as exc:
                # Capture error and try next destination.
                self.connect_send_errors += 1
                self.connect_send_last_error = str(exc)
                try:
                    print(f"GW: failed CONN_RESP to {dest}: {exc}")
                except Exception:
                    pass
                continue

    def _handle_connectionstate_request(self, pkt: bytes, addr: Tuple[str, int]) -> None:
        assert self._sock is not None
        self.rx_state += 1
        ch, ctrl_ep = knxip_codec.parse_connectionstate_request(pkt)
        if ch is None:
            return
        with self._lock:
            client = self._clients_by_channel.get(ch)
            if client:
                self._addr_to_channel[addr] = ch
        if not client:
            return
        resp = knxip_codec.build_connectionstate_response(client.channel_id, knxip_codec.E_NO_ERROR)
        # Prefer per-client connected socket for reply when available.
        if client.send_sock is not None:
            try:
                client.send_sock.send(resp)
                self.tx_state_resp += 1
                return
            except OSError:
                pass
        # Fallback to source address
        try:
            self._sock.sendto(resp, addr)
            self.tx_state_resp += 1
        except OSError:
            pass

    def _handle_disconnect_request(self, pkt: bytes, addr: Tuple[str, int]) -> None:
        assert self._sock is not None
        self.rx_disconnect += 1
        ch, ctrl_ep = knxip_codec.parse_disconnect_request(pkt)
        if ch is None:
            return
        self.last_disconnect_channel = ch
        self.last_disconnect_from = addr
        with self._lock:
            client = self._clients_by_channel.pop(ch, None)
            # Remove known aliases for this channel.
            if client:
                for a, c in list(self._addr_to_channel.items()):
                    if c == ch:
                        self._addr_to_channel.pop(a, None)
        if not client:
            return
        resp = knxip_codec.build_disconnect_response(client.channel_id, knxip_codec.E_NO_ERROR)
        # Prefer per-client connected socket for reply when available.
        if client.send_sock is not None:
            try:
                client.send_sock.send(resp)
                self.tx_disconnect_resp += 1
                return
            except OSError:
                pass
        # Fallback to source address
        try:
            self._sock.sendto(resp, addr)
            self.tx_disconnect_resp += 1
        except OSError:
            pass

    def _handle_tunneling_ack(self, pkt: bytes, addr: Tuple[str, int]) -> None:
        self.rx_tunnel_ack += 1
        # For now we don't track outgoing confirmation state.
        # Having the gateway accept ACKs avoids clients retrying too aggressively.
        req = knxip_codec.parse_tunneling_request(pkt)
        # ACK has the same tunneling header layout (len/ch/seq/status) but no cEMI.
        ch = pkt[7] if len(pkt) > 7 else None
        with self._lock:
            client = self._clients_by_channel.get(int(ch) if ch is not None else -1)
            if client:
                self._addr_to_channel[addr] = client.channel_id
                client.last_seen = time.time()

    def _handle_tunneling_request(self, pkt: bytes, addr: Tuple[str, int]) -> None:
        assert self._sock is not None
        self.rx_tunnel_req += 1

        req = knxip_codec.parse_tunneling_request(pkt)
        if not req:
            return
        with self._lock:
            client = self._clients_by_channel.get(req.channel_id)
            if not client:
                # If a client sends tunnelling frames without a CONNECT, ignore.
                return
            self._addr_to_channel[addr] = client.channel_id
            client.last_seq_from_client = req.seq
            client.last_seen = time.time()

        # ACK back to sender.
        ack = knxip_codec.build_tunneling_ack(req.channel_id, req.seq, knxip_codec.E_NO_ERROR)
        if client.send_sock is not None:
            try:
                client.send_sock.send(ack)
                self.tx_tunnel_ack += 1
            except OSError:
                pass
        else:
            self._sock.sendto(ack, addr)
        self.tx_tunnel_ack += 1

        # Forward cEMI to all other clients as L_Data.ind (0x29).
        cemi = req.cemi
        if len(cemi) >= 1:
            cemi = bytes((0x29,)) + cemi[1:]

        with self._lock:
            others = [c for c in self._clients_by_channel.values() if c.channel_id != client.channel_id]
            self.last_tunnel_sender_channel = client.channel_id
            self.last_tunnel_forwarded_to_channels = tuple(sorted(c.channel_id for c in others))

        for other in others:
            try:
                seq_out = other.next_seq_to_client & 0xFF
                other.last_seen = time.time()

                pkt = knxip_codec.build_tunneling_request(other.channel_id, seq_out, cemi)
                if other.send_sock is not None:
                    try:
                        other.send_sock.send(pkt)
                    except OSError:
                        raise
                else:
                    self._sock.sendto(pkt, other.addr)
                self.tx_forward += 1

                other.next_seq_to_client = (seq_out + 1) & 0xFF
            except OSError:
                self.forward_send_errors += 1
                continue
