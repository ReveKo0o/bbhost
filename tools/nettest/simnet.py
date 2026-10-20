"""The network under a simulated client: a NAT, the P2P wire, STUN.

Every simulated client has its own "public" address on the loopback network
(127.0.1.N) and sits behind a userspace NAT of a chosen kind. The server sees
each mapping as its own source address, exactly as it would see a player
behind that kind of router:

  none   no NAT: one socket on a fixed port, no filtering, no expiry
  full   full cone: one mapping for every destination, anyone may send in
  addr   address-restricted cone: in only from an IP the client sent to
  port   port-restricted cone: in only from an ip:port the client sent to
  sym    symmetric: a new mapping per destination, in only from it

Mappings expire after `timeout` seconds without an outbound datagram (a real
router's UDP timeout), and anything sent to an expired mapping is lost.
"""
from __future__ import annotations

import os
import socket
import struct
import threading
import time

NAT_KINDS = ("none", "full", "addr", "port", "sym")


class _Mapping:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.last_out = time.time()
        self.sent_to: set[tuple[str, int]] = set()
        self.sent_ip: set[str] = set()
        self.closed = False

    @property
    def port(self) -> int:
        return self.sock.getsockname()[1]


class Nat:
    """A NAT in front of one client. sendto() picks (or makes) the mapping;
    a reader per mapping hands what passes the filter to on_packet(data, src)."""

    def __init__(self, kind: str, public_ip: str, on_packet, timeout: float = 120.0,
                 fixed_port: int = 0):
        if kind not in NAT_KINDS:
            raise ValueError(f"unknown NAT kind {kind}")
        self.kind = kind
        self.ip = public_ip
        self.on_packet = on_packet
        self.timeout = timeout
        self.fixed_port = fixed_port
        self.lock = threading.Lock()
        self.maps: dict[object, _Mapping] = {}
        self.dropped_filtered = 0
        self.dropped_expired = 0
        self.mappings_made = 0
        self.running = True

    def _expired(self, m: _Mapping, now: float) -> bool:
        return self.kind != "none" and now - m.last_out > self.timeout

    def _close_locked(self, key, m: _Mapping) -> None:
        m.closed = True
        try:
            m.sock.close()
        except OSError:
            pass
        if self.maps.get(key) is m:
            del self.maps[key]

    def _mapping_for(self, dest: tuple[str, int]) -> _Mapping:
        key = dest if self.kind == "sym" else None
        now = time.time()
        with self.lock:
            m = self.maps.get(key)
            if m is not None and self._expired(m, now):
                self._close_locked(key, m)
                m = None
            if m is None:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((self.ip, self.fixed_port if self.kind == "none" else 0))
                s.settimeout(0.2)
                m = _Mapping(s)
                self.maps[key] = m
                self.mappings_made += 1
                threading.Thread(target=self._reader, args=(key, m), daemon=True,
                                 name=f"nat-{self.ip}-{m.port}").start()
            m.last_out = now
            m.sent_to.add(dest)
            m.sent_ip.add(dest[0])
            return m

    def sendto(self, data: bytes, dest: tuple[str, int]) -> int:
        if not self.running:
            return 0
        m = self._mapping_for(dest)
        try:
            return m.sock.sendto(data, dest)
        except OSError:
            return 0

    def mapping_ports(self) -> list[int]:
        with self.lock:
            return [m.port for m in self.maps.values() if not m.closed]

    def _reader(self, key, m: _Mapping) -> None:
        while self.running and not m.closed:
            try:
                data, src = m.sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            now = time.time()
            with self.lock:
                if self._expired(m, now):
                    self.dropped_expired += 1
                    self._close_locked(key, m)
                    return
                ok = True
                if self.kind == "addr":
                    ok = src[0] in m.sent_ip
                elif self.kind in ("port", "sym"):
                    ok = src in m.sent_to
                if not ok:
                    self.dropped_filtered += 1
                    continue
            try:
                self.on_packet(data, src)
            except Exception as e:  # a handler bug must not kill the mapping
                print(f"[nat {self.ip}] handler error: {e!r}", flush=True)

    def close(self) -> None:
        self.running = False
        with self.lock:
            for key, m in list(self.maps.items()):
                self._close_locked(key, m)


# ---- the P2P wire ----------------------------------------------------------
# [0xff][0x80 valid | 0x40 byte vports | type 3][src vport][dst vport] payload,
# as bbhost's hle/net.cpp writes it.

P2P_MAGIC = 0xFF
P2P_FLAGS_BYTE_VPORTS = 0x80 | 0x40 | 0x03


def p2p_pack(src_vport: int, dst_vport: int, payload: bytes) -> bytes:
    return bytes([P2P_MAGIC, P2P_FLAGS_BYTE_VPORTS, src_vport & 0xFF, dst_vport & 0xFF]) + payload


def p2p_unpack(data: bytes):
    """(src vport, dst vport, payload) or None."""
    if len(data) < 4 or data[0] != P2P_MAGIC:
        return None
    fl = data[1]
    at = 2 + (4 if fl & 0x20 else 0)
    if fl & 0x40:
        if len(data) < at + 2:
            return None
        return data[at], data[at + 1], data[at + 2:]
    if len(data) < at + 4:
        return None
    return (data[at] << 8) | data[at + 1], (data[at + 2] << 8) | data[at + 3], data[at + 4:]


# A bbhost hole-punch probe (hle/net.cpp kProbe): no P2P header.
PROBE = bytes([0xFE]) + b"bbhp" + b"\0\0\0"


# ---- STUN --------------------------------------------------------------------
# The request bbhost sends (net/stun.cpp): a 20-byte header, 16-byte
# transaction id (the RFC 3489 shape the server parses), optional attributes.

STUN_BINDING_REQUEST = 0x0001
STUN_BINDING_RESPONSE = 0x0101
ATTR_MAPPED_ADDRESS = 0x0001
ATTR_XOR_MAPPED_ADDRESS = 0x8020
# bbhost's relay attributes (server stun_server.py, client net/stun.cpp):
# request: BBHOST-HELLO = b"bbr1" + token(8, zero when none); response:
# BBHOST-RELAY = b"bbr1" + token(8) + vport(u16) + observed ip(4) + port(u16).
ATTR_BBHOST_HELLO = 0x8100
ATTR_BBHOST_RELAY = 0x8101


def stun_request(txid: bytes, token: bytes | None = None, hello: bool = False) -> bytes:
    attrs = b""
    if hello:
        val = b"bbr1" + (token or b"\0" * 8)
        attrs += struct.pack("!HH", ATTR_BBHOST_HELLO, len(val)) + val
    return struct.pack("!HH", STUN_BINDING_REQUEST, len(attrs)) + txid + attrs


def stun_parse(data: bytes):
    """(txid, mapped (ip, port) or None, relay dict or None) for a binding
    response, else None."""
    if len(data) < 20:
        return None
    mtype, mlen = struct.unpack_from("!HH", data, 0)
    if mtype != STUN_BINDING_RESPONSE or len(data) < 20 + mlen:
        return None
    txid = data[4:20]
    mapped = None
    relay = None
    off = 20
    end = 20 + mlen
    while off + 4 <= end:
        at, al = struct.unpack_from("!HH", data, off)
        off += 4
        val = data[off:off + al]
        off += (al + 3) & ~3
        if at == ATTR_MAPPED_ADDRESS and len(val) >= 8:
            port = struct.unpack_from("!H", val, 2)[0]
            mapped = (socket.inet_ntoa(val[4:8]), port)
        elif at == ATTR_BBHOST_RELAY and len(val) >= 20 and val[:4] == b"bbr1":
            relay = {
                "token": val[4:12],
                "vport": struct.unpack_from("!H", val, 12)[0],
                "observed": (socket.inet_ntoa(val[14:18]), struct.unpack_from("!H", val, 18)[0]),
            }
    return txid, mapped, relay


def new_txid() -> bytes:
    return os.urandom(16)


# ---- bbhost's relay frames (server stun_server.py "bbrelay") -------------------
# client -> server, to the STUN port:  [0xfb]['R'][token 8][dst vport u16] P2P datagram
# server -> client, from the STUN port: [0xfb]['r'][src vport u16] P2P datagram
# client keepalive is the STUN binding with BBHOST-HELLO and the token.

RELAY_MAGIC = 0xFB


def relay_wrap(token: bytes, dst_vport: int, datagram: bytes) -> bytes:
    return bytes([RELAY_MAGIC, ord("R")]) + token + struct.pack("!H", dst_vport) + datagram


def relay_unwrap(data: bytes):
    """(src vport, datagram) for a server -> client relay frame, else None."""
    if len(data) < 4 or data[0] != RELAY_MAGIC or data[1] != ord("r"):
        return None
    return struct.unpack_from("!H", data, 2)[0], data[4:]
