"""A simulated bbhost client for the online test bench.

It talks to the private server exactly as bbhost does - the game's own HTTP
(login, summon signs), the NP surfaces the HLE calls (context_start, rooms,
heartbeats, events, signaling resolve) and the P2P socket (STUN, probes, the
relay) - and plays the game's part of a summon in miniature:

  host:  finds a sign, makes its room, asks the server to summon, resolves
         the guest and sends it an invite datagram over P2P until it joins
         (the game's type-1 item, sub_1090b00, in the real thing)
  guest: puts its sign up; an invite datagram that reaches it makes it join
  all:   once in a room, ping every other member over P2P every second

`quirks` replays bbhost's behaviour before the 2026-10-06 fixes, so a run
can show what players saw:
  "auto_leave"   the host leaves the server's room when one or fewer peers
                 are left after a member leaves (np_matching2.cpp)
  "invite_seed"  the guest takes the invite's HostAddr as the host's
                 address and punches it (np_matching2.cpp guest_invite)
  "no_keepalive" no STUN refresh between room calls (net/session.cpp)
"""
from __future__ import annotations

import base64
import json
import os
import struct
import threading
import time
import urllib.error
import urllib.request

from simnet import (Nat, PROBE, p2p_pack, p2p_unpack, relay_unwrap, relay_wrap,
                    new_txid, stun_parse, stun_request)

VPORT = 30  # the session's P2P vport (40 is the boot socket)


class HttpError(Exception):
    pass


class Api:
    """The server's HTTP surfaces, with bbhost's headers."""

    def __init__(self, base: str, timeout: float = 5.0):
        self.base = base.rstrip("/")
        self.timeout = timeout
        self.headers = {"Content-Type": "application/json", "X-BBHost-Ruleset": "vanilla"}

    def post(self, path: str, body: dict, query: str = "") -> dict:
        url = self.base + path + (("?" + query) if query else "")
        req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=self.headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            try:
                return json.loads(e.read() or b"{}") | {"_status": e.code}
            except Exception:
                return {"_status": e.code}
        except (urllib.error.URLError, OSError) as e:
            raise HttpError(f"{path}: {e}") from e


def summon_blob(sign_type: int, user_id: int) -> str:
    """A 0xE0-byte SummonData the server can parse: sign type at 0x76, the
    user id at 0xD0 (parsers/summon_data.py)."""
    b = bytearray(0xE0)
    struct.pack_into("<BBBB", b, 0x76, sign_type, 0, 0, 0)
    struct.pack_into("<i", b, 0xD0, user_id)
    return base64.b64encode(bytes(b)).decode()


class SimClient:
    SIGN = {"coop": (0, 7), "invade": (2, 8)}  # SummonType, blob sign type

    def __init__(self, bench, name: str, nat: str = "none", ip: str = "127.0.1.1", p2p_port: int = 9307,
                 quirks: tuple[str, ...] = (), relay: bool = True, nat_timeout: float = 120.0,
                 area: int = 0x18010000, region: int = 1):
        self.bench = bench
        self.name = name
        self.ip = ip
        self.api = Api(bench.http_base)
        self.quirks = set(quirks)
        self.use_relay = relay  # bbhost's relay frames when the server offers them
        self.area, self.region = area, region
        self.lock = threading.RLock()
        self.nat = Nat(nat, ip, self._on_packet, timeout=nat_timeout, fixed_port=p2p_port)
        self.p2p_port = p2p_port
        self.log_lines: list[str] = []
        # server identity
        self.session_id = ""   # the game's login session
        self.user_id = 0
        # STUN / relay
        self.stun = bench.stun_addr
        self._stun_wait: dict[bytes, threading.Event] = {}
        self._stun_result: dict[bytes, tuple] = {}
        self.mapped: tuple[str, int] | None = None
        self.relay_token: bytes | None = None
        self.relay_vport = 0
        # room
        self.room = 0
        self.room_sid = ""
        self.member_id = 0
        self.is_host = False
        self.max_slot = 0
        self.peers: dict[int, dict] = {}         # member id -> {online, addr}
        self.provisional: dict[str, tuple] = {}   # online id -> addr (resolve before join)
        self.heard: dict[str, float] = {}        # online id -> last PONG/PING time
        self.events: list[dict] = []
        self.joined_rooms: list[int] = []
        self.join_failures: list[tuple[int, str]] = []
        self.invites_seen: list[int] = []
        self._inviting: dict[str, int] = {}       # guest online id -> room (host side)
        self._joining = False
        self.running = True
        self.heartbeats = True                    # a "crash" stops them
        self.cursor = 0
        self.threads: list[threading.Thread] = []

    # ---- logging -------------------------------------------------------------
    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} [{self.name}] {msg}"
        self.log_lines.append(line)
        self.bench.log(line)

    # ---- the P2P socket --------------------------------------------------------
    def _addr_is_relay(self, addr: tuple[str, int]) -> bool:
        return (self.use_relay and self.relay_token is not None and addr[0] == self.bench.relay_ip
                and self.bench.relay_lo <= addr[1] <= self.bench.relay_hi)

    def send_dgram(self, addr: tuple[str, int], payload: bytes) -> None:
        dgram = p2p_pack(VPORT, VPORT, payload)
        if self._addr_is_relay(addr):
            self.nat.sendto(relay_wrap(self.relay_token, addr[1], dgram), self.stun)
        else:
            self.nat.sendto(dgram, addr)

    def _on_packet(self, data: bytes, src: tuple[str, int]) -> None:
        if src == self.stun:
            r = relay_unwrap(data)
            if r is not None:
                src_vport, dgram = r
                self._on_dgram(dgram, (self.bench.relay_ip, src_vport))
                return
            st = stun_parse(data)
            if st is not None:
                txid, mapped, relay = st
                ev = self._stun_wait.get(txid)
                if ev is not None:
                    self._stun_result[txid] = (mapped, relay)
                    ev.set()
                return
        if data[:5] == PROBE[:5]:
            return
        self._on_dgram(data, src)

    def _on_dgram(self, data: bytes, src: tuple[str, int]) -> None:
        p = p2p_unpack(data)
        if p is None:
            return
        _, _, payload = p
        try:
            kind, who, arg = payload.decode().split(" ", 2)
        except ValueError:
            return
        if kind == "INVITE":
            room = int(arg)
            with self.lock:
                self.invites_seen.append(room)
                busy = self.room or self._joining
                if not busy:
                    self._joining = True
            if not busy:
                self.log(f"invite datagram from {who} ({src[0]}:{src[1]}) for room {room}")
                threading.Thread(target=self._join_from_invite, args=(room,), daemon=True).start()
        elif kind == "PING":
            with self.lock:
                self.heard[who] = time.time()
            # The game answers the address the datagram came from.
            self.send_dgram(src, f"PONG {self.name} {arg}".encode())
        elif kind == "PONG":
            with self.lock:
                self.heard[who] = time.time()

    # ---- STUN ------------------------------------------------------------------
    def stun_refresh(self, timeout: float = 1.0) -> bool:
        txid = new_txid()
        ev = threading.Event()
        self._stun_wait[txid] = ev
        hello = self.use_relay
        for _ in range(3):
            self.nat.sendto(stun_request(txid, self.relay_token, hello=hello), self.stun)
            if ev.wait(timeout):
                break
        self._stun_wait.pop(txid, None)
        res = self._stun_result.pop(txid, None)
        if not res:
            self.log("stun: no answer")
            return False
        mapped, relay = res
        changed = mapped != self.mapped
        self.mapped = mapped
        if relay:
            if self.relay_token != relay["token"]:
                self.log(f"relay: vport {relay['vport']} (token new)")
            self.relay_token = relay["token"]
            self.relay_vport = relay["vport"]
        if changed:
            self.log(f"stun: mapped {mapped[0]}:{mapped[1]}")
            if self.room_sid or self.session_id:
                self._signaling_update()
        return True

    def _signaling_update(self) -> None:
        if not self.mapped:
            return
        self.api.post("/mp/matching2/signaling_update", {
            "SessionId": self.room_sid, "MemberId": self.member_id, "OnlineId": self.name,
            "MappedAddr": self.mapped[0], "MappedPort": self.mapped[1]})

    def _keepalive_loop(self) -> None:
        while self.running:
            time.sleep(self.bench.keepalive)
            if self.running and "no_keepalive" not in self.quirks and self.heartbeats:
                try:
                    self.stun_refresh()
                except HttpError:
                    pass

    # ---- the server: login and the NP context -----------------------------------
    def endpoint(self) -> dict:
        m = self.mapped or ("", 0)
        return {"OnlineId": self.name, "LocalAddr": "127.0.0.1", "LocalPort": self.p2p_port,
                "PublicAddr": "127.0.0.1", "PublicPort": self.p2p_port,
                "MappedAddr": m[0], "MappedPort": m[1]}

    def start(self) -> None:
        r = self.api.post("/basic_utils/login", {
            "MessageId": "LoginRequest", "AuthorizationCode": "DUMMY", "PlatformAccountId": self.name,
            "ApplicationVersion": 109, "IssuerId": 1, "LanguageId": 1, "NatType": 2, "RegionId": 1})
        self.session_id, self.user_id = r.get("SessionId", ""), r.get("UserId", 0)
        self.stun_refresh()
        b = {"OnlineId": self.name, "SignalingAddr": "127.0.0.1", "SignalingPort": self.p2p_port}
        if self.mapped:
            b |= {"MappedAddr": self.mapped[0], "MappedPort": self.mapped[1]}
        self.api.post("/mp/matching2/context_start", b)
        for fn in (self._poll_loop, self._heartbeat_loop, self._keepalive_loop, self._ping_loop):
            t = threading.Thread(target=fn, daemon=True, name=f"{self.name}-{fn.__name__}")
            t.start()
            self.threads.append(t)
        self.log(f"online: user {self.user_id}, nat {self.nat.kind}, mapped {self.mapped}")

    def stop(self) -> None:
        self.running = False
        self.nat.close()

    def crash(self) -> None:
        """The game dies: no leave, no heartbeat, no traffic."""
        self.heartbeats = False
        self.running = False
        self.nat.close()
        self.log("crashed")

    # ---- events -------------------------------------------------------------------
    def _poll_loop(self) -> None:
        while self.running:
            try:
                r = self.api.post("/np/events/poll", {"OnlineId": self.name, "Cursor": self.cursor,
                                                      "Categories": ["matching2"]})
            except HttpError:
                time.sleep(0.5)
                continue
            if r.get("HasEvent"):
                ev = dict(r.get("Payload") or {})
                ev["Name"] = r.get("Name")
                with self.lock:
                    self.events.append(ev | {"_t": time.time()})
                try:
                    self._on_event(ev)
                except Exception as e:
                    self.log(f"event handler error {e!r}")
                nxt = r.get("NextCursor", self.cursor)
                try:
                    self.api.post("/np/events/ack", {"OnlineId": self.name, "Cursor": nxt})
                except HttpError:
                    pass
                self.cursor = nxt
                continue
            self.cursor = max(self.cursor, r.get("NextCursor", self.cursor))
            time.sleep(0.25)

    def _peer_addr(self, rec: dict) -> tuple[str, int]:
        # net/session.cpp peers_from_members: a mapped address that differs
        # from the local one wins.
        a, p = rec.get("Addr", ""), int(rec.get("Port", 0) or 0)
        ma, mp = rec.get("MappedAddr", ""), int(rec.get("MappedPort", 0) or 0)
        if ma and mp and ma != rec.get("LocalAddr") and ma != a:
            a, p = ma, mp
        if not a:
            a, p = rec.get("LocalAddr", ""), int(rec.get("LocalPort", 0) or 0)
        return a, p

    def _on_event(self, ev: dict) -> None:
        name = ev.get("Name")
        if name == "guest_invite":
            host = ev.get("HostOnlineId", "")
            if "invite_seed" in self.quirks:
                addr = self._peer_addr({"Addr": ev.get("HostAddr"), "Port": ev.get("HostPort"),
                                        "LocalAddr": ev.get("HostLocalAddr"), "LocalPort": ev.get("HostLocalPort"),
                                        "MappedAddr": ev.get("HostMappedAddr", ""),
                                        "MappedPort": ev.get("HostMappedPort", 0)})
            else:
                addr = self._resolve(host)
            if addr and addr[0]:
                with self.lock:
                    self.provisional[host] = addr
                self.log(f"guest_invite: room {ev.get('RoomId')} from {host}, host at {addr[0]}:{addr[1]}")
                for _ in range(3):
                    self.nat.sendto(PROBE, addr) if not self._addr_is_relay(addr) else \
                        self.send_dgram(addr, f"HELLO {self.name} 0".encode())
        elif name == "room_member_joined":
            mid = int(ev.get("MemberId", 0))
            with self.lock:
                if mid and mid != self.member_id:
                    self.peers[mid] = {"online": ev.get("OnlineId", ""), "addr": self._peer_addr(ev)}
                    self._inviting.pop(ev.get("OnlineId", ""), None)
            self.log(f"member {mid} ({ev.get('OnlineId')}) joined room {ev.get('RoomId')}")
        elif name == "room_member_left":
            mid = int(ev.get("MemberId", 0))
            with self.lock:
                self.peers.pop(mid, None)
                remaining = len(self.peers)
            self.log(f"member {mid} ({ev.get('OnlineId')}) left ({ev.get('Reason')}), {remaining} peer(s) left")
            if "auto_leave" in self.quirks and self.is_host and remaining <= 1:
                self.log("quirk auto_leave: the host leaves the server's room")
                self._server_leave()  # behind the game's back: our room/state stay
        elif name in ("room_closed", "room_member_kicked"):
            self.log(f"{name}: room {ev.get('RoomId')} ({ev.get('Reason', '')})")
            with self.lock:
                if int(ev.get("RoomId", 0) or 0) == self.room:
                    self._clear_room()

    # ---- rooms (the HLE's calls) ---------------------------------------------------
    def _clear_room(self) -> None:
        self.room, self.room_sid, self.member_id, self.is_host = 0, "", 0, False
        self.peers.clear()

    def create_room(self, max_slot: int = 5) -> int:
        if "no_keepalive" in self.quirks:
            self.stun_refresh()
        r = self.api.post("/mp/matching2/create_room", self.endpoint() | {"MaxMembers": max_slot})
        if r.get("ResKind") != 0:
            self.log(f"create_room failed: {r}")
            return 0
        with self.lock:
            self.room, self.room_sid = int(r["RoomId"]), r["SessionId"]
            self.member_id, self.is_host, self.max_slot = int(r.get("MemberId", 1)), True, max_slot
            self.peers.clear()
        self.log(f"room {self.room} created (member {self.member_id})")
        return self.room

    def _join_from_invite(self, room: int) -> None:
        try:
            self.join_room(room)
        finally:
            with self.lock:
                self._joining = False

    def join_room(self, room: int) -> bool:
        if "no_keepalive" in self.quirks:
            self.stun_refresh()
        r = self.api.post("/mp/matching2/join_room", self.endpoint() | {"RoomId": room})
        if r.get("ResKind") != 0:
            self.join_failures.append((room, r.get("Error", str(r))))
            self.log(f"join_room {room} failed: {r.get('Error', r)}")
            return False
        with self.lock:
            self.room, self.room_sid = room, r["SessionId"]
            self.member_id, self.is_host = int(r["MemberId"]), False
            self.max_slot = int(r.get("MaxMembers", 0) or 0)
            self.peers.clear()
            for m in r.get("Members", []):
                mid = int(m.get("MemberId", 0))
                if mid and mid != self.member_id:
                    self.peers[mid] = {"online": m.get("OnlineId", ""), "addr": self._peer_addr(m)}
            self.joined_rooms.append(room)
        self.log(f"joined room {room} as member {self.member_id} with {len(self.peers)} peer(s)")
        return True

    def _server_leave(self) -> None:
        if self.room_sid:
            self.api.post("/mp/matching2/leave_room", {"SessionId": self.room_sid, "MemberId": self.member_id})

    def leave_room(self) -> None:
        """The game's LeaveRoom."""
        self._server_leave()
        with self.lock:
            self.log(f"left room {self.room}")
            self._clear_room()

    def kick(self, member_id: int, opt: bytes = b"\x19\0\0\xff") -> dict:
        return self.api.post("/mp/matching2/kick_member", {
            "SessionId": self.room_sid, "MemberId": member_id, "KickerMemberId": self.member_id,
            "OptData": base64.b64encode(opt).decode()})

    def _heartbeat_loop(self) -> None:
        while self.running:
            time.sleep(self.bench.heartbeat)
            with self.lock:
                sid, mid = self.room_sid, self.member_id
            if sid and self.heartbeats and self.running:
                try:
                    r = self.api.post("/mp/matching2/heartbeat", {"SessionId": sid, "MemberId": mid})
                except HttpError:
                    continue
                if r.get("ResKind") not in (0, None):
                    self.log(f"heartbeat refused: {r.get('Error', r)}")

    def _resolve(self, online: str):
        r = self.api.post("/np/signaling/resolve", {"OnlineId": online})
        if r.get("ResKind") != 0:
            return None
        return r.get("Addr", ""), int(r.get("Port", 0) or 0)

    # ---- signs and summons (the game's) ------------------------------------------------
    def put_sign(self, kind: str = "coop") -> None:
        st, bt = self.SIGN[kind]
        self.api.post("/summon_messenger/create", {
            "MessageId": "SummonDataCreateRequest", "SessionId": self.session_id, "UserId": self.user_id,
            "CharaId": 1, "AreaId": self.area, "AreaRegionId": self.region, "SummonType": st,
            "SummonData": summon_blob(bt, self.user_id), "SummonDataVersion": 3}, f"user_id={self.user_id}")
        self.log(f"{kind} sign up")

    def find_signs(self, kind: str = "coop") -> list[dict]:
        st, _ = self.SIGN[kind]
        r = self.api.post("/summon_messenger/get", {
            "MessageId": "SummonDataGetListRequest", "SessionId": self.session_id, "UserId": self.user_id,
            "AreaId": self.area, "AreaRegionId": self.region, "GetMaxCount": 20,
            "SummonTypeList": [{"SummonType": st, "GetLimitCount": 5}]}, f"user_id={self.user_id}")
        return r.get("SummonDataList", [])

    def summon(self, guest: "SimClient", kind: str = "coop", max_slot: int = 5) -> None:
        """The host's game: a sign it found -> its room (made once) -> the
        request -> resolve the guest -> invite datagrams until it joins."""
        signs = [s for s in self.find_signs(kind) if s.get("UserId") == guest.user_id]
        if not signs:
            self.log(f"no {kind} sign from {guest.name}")
            return
        if not self.room:
            self.create_room(max_slot)
        self.api.post("/summon_messenger/request", {
            "MessageId": "SummonDataSummonRequest", "SessionId": self.session_id, "UserId": self.user_id,
            "TargetUserId": guest.user_id}, f"user_id={self.user_id}")
        addr = self._resolve(guest.name)
        self.log(f"summon {guest.name}: resolved {addr}")
        if not addr or not addr[0]:
            return
        with self.lock:
            self.provisional[guest.name] = addr
            self._inviting[guest.name] = self.room
        threading.Thread(target=self._invite_loop, args=(guest.name, addr), daemon=True).start()

    def _invite_loop(self, guest: str, addr: tuple[str, int]) -> None:
        t0 = time.time()
        while self.running and time.time() - t0 < self.bench.invite_window:
            with self.lock:
                room = self._inviting.get(guest)
            if not room:
                return
            self.send_dgram(addr, f"INVITE {self.name} {room}".encode())
            time.sleep(0.3)
        with self.lock:
            if self._inviting.pop(guest, None):
                self.log(f"invite to {guest} never answered ({self.bench.invite_window:.0f} s)")

    # ---- in a room: everyone pings everyone -----------------------------------------
    def _ping_loop(self) -> None:
        n = 0
        while self.running:
            time.sleep(1.0)
            with self.lock:
                targets = [(p["online"], p["addr"]) for p in self.peers.values() if p["addr"][0]]
            n += 1
            for online, addr in targets:
                self.send_dgram(addr, f"PING {self.name} {n}".encode())

    def hears(self, online: str, within: float = 3.0) -> bool:
        with self.lock:
            t = self.heard.get(online, 0)
        return time.time() - t <= within

    def peer_names(self) -> set[str]:
        with self.lock:
            return {p["online"] for p in self.peers.values()}
