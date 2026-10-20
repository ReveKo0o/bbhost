#!/usr/bin/env python3
"""The online test bench: simulated bbhost clients against a private server.

    tools/nettest/bench.py --server-tree <server package dir> [--quirks old] [--only NAME ...]

The server tree is copied (code only) into a scratch directory and started
there with STUN and the relay on, on ports of its own (HTTP 28671, STUN
23478, relay 24000-24199), so a run never touches a server you have running
or its data. Each scenario builds a session of simulated clients
(tools/nettest/simclient.py), drives it, and checks what the server and the
P2P traffic did. --quirks old replays bbhost's room handling from before
2026-10-06 (auto_leave, invite_seed, no_keepalive); without it the clients
behave as bbhost does now.
"""
from __future__ import annotations

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simclient import SimClient  # noqa: E402

OLD_QUIRKS = ("auto_leave", "invite_seed", "no_keepalive")


class Bench:
    def __init__(self, server_tree: str, python: str, work: str, quirks=(), verbose=False,
                 http_port=28671, stun_port=23478, relay=(24000, 24199), heartbeat_timeout=15.0,
                 vport_ttl=None, env=None, ssinfo_port=None):
        self.tree = os.path.abspath(server_tree)
        self.python = python
        self.work = work
        self.quirks = tuple(quirks)
        self.verbose = verbose
        self.http_port = http_port
        self.ssinfo_port = ssinfo_port or http_port + 1
        self.http_base = f"http://127.0.0.1:{http_port}"
        # The relay must not share the address bbhost reports for itself
        # (127.0.0.1 without online.p2p_addr): the server would take it for
        # a LAN peer and hand out the local address instead.
        self.relay_ip = "127.0.0.2"
        self.stun_addr = (self.relay_ip, stun_port)
        self.relay_lo, self.relay_hi = relay
        self.heartbeat_timeout = heartbeat_timeout
        self.vport_ttl = vport_ttl
        self.extra_env = env or {}
        self.keepalive = 15.0     # bbhost's STUN keepalive (s)
        self.heartbeat = 5.0      # bbhost's room heartbeat (s)
        self.invite_window = 20.0  # how long the host's game keeps inviting
        self.proc = None
        self.lines: list[str] = []
        self.clients: list[SimClient] = []
        self._next_ip = 1
        self._lock = threading.Lock()

    def log(self, line: str) -> None:
        with self._lock:
            self.lines.append(line)
        if self.verbose:
            print("   " + line, flush=True)

    # ---- the server -----------------------------------------------------------
    def start_server(self) -> None:
        root = os.path.join(self.work, "srv")
        if os.path.exists(root):
            shutil.rmtree(root)
        os.makedirs(os.path.join(root, "data"))
        shutil.copytree(self.tree, os.path.join(root, "server"),
                        ignore=shutil.ignore_patterns("*.db", "*.db-*", "__pycache__", "*.pyc", "playlogs"))
        env = os.environ.copy()
        env.update({"BB_MP_HEARTBEAT_TIMEOUT": str(self.heartbeat_timeout), "BB_REQUIRE_ACCOUNT": "0",
                    "PYTHONUNBUFFERED": "1"})
        if self.vport_ttl:
            env["BB_RELAY_VPORT_IDLE_TTL"] = str(self.vport_ttl)
        env.update(self.extra_env)
        self.server_log = os.path.join(self.work, "server.log")
        logf = open(self.server_log, "w")
        self.proc = subprocess.Popen(
            [self.python, "-m", "server.main", "--host", "127.0.0.1", "--port", str(self.http_port),
             "--ssinfo-port", str(self.ssinfo_port), "--no-ssl", "--stun", "--stun-port", str(self.stun_addr[1]),
             "--stun-bind", self.relay_ip, "--stun-external-addr", self.relay_ip, "--relay",
             "--relay-port-start", str(self.relay_lo), "--relay-port-end", str(self.relay_hi)],
            cwd=root, env=env, stdout=logf, stderr=subprocess.STDOUT, start_new_session=True)
        for _ in range(150):
            try:
                urllib.request.urlopen(self.http_base + "/openapi.json", timeout=1).read()
                return
            except Exception:
                if self.proc.poll() is not None:
                    break
                time.sleep(0.2)
        raise RuntimeError(f"server did not start (see {self.server_log})")

    def stop_server(self) -> None:
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
        self.proc = None

    def server_lines(self, needle: str) -> list[str]:
        with open(self.server_log, errors="replace") as f:
            return [l.rstrip() for l in f if needle in l]

    # ---- clients ----------------------------------------------------------------
    def client(self, name: str, nat: str = "none", quirks=None, **kw) -> SimClient:
        ip = f"127.0.1.{self._next_ip}"
        port = 9300 + self._next_ip
        self._next_ip += 1
        c = SimClient(self, name, nat=nat, ip=ip, p2p_port=port,
                      quirks=self.quirks if quirks is None else quirks, **kw)
        c.start()
        self.clients.append(c)
        return c

    def stop_clients(self) -> None:
        for c in self.clients:
            c.stop()
        self.clients.clear()
        self._next_ip = 1


# ---- helpers for scenarios --------------------------------------------------------

def wait(cond, timeout: float, step: float = 0.2) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        if cond():
            return True
        time.sleep(step)
    return cond()


class Check:
    def __init__(self, name: str):
        self.name = name
        self.fails: list[str] = []
        self.notes: list[str] = []

    def ok(self, cond: bool, what: str) -> bool:
        (self.notes if cond else self.fails).append(("ok   " if cond else "FAIL ") + what)
        return cond

    def note(self, what: str) -> None:
        self.notes.append("     " + what)

    @property
    def passed(self) -> bool:
        return not self.fails


def summon_into(b: Bench, host: SimClient, guest: SimClient, chk: Check, kind="coop", timeout=25.0) -> bool:
    guest.put_sign(kind)
    host.summon(guest, kind)
    joined = wait(lambda: guest.room and guest.room == host.room, timeout)
    return chk.ok(joined, f"{guest.name} ({guest.nat.kind}) joins {host.name}'s room by {kind} summon")


def mesh_ok(chk: Check, members: list[SimClient], timeout=8.0) -> bool:
    def full():
        return all(a.hears(z.name) for a in members for z in members if a is not z)
    good = wait(full, timeout)
    if not good:
        missing = [f"{a.name}->{z.name}" for a in members for z in members if a is not z and not a.hears(z.name)]
        chk.ok(False, f"P2P mesh among {len(members)}: silent pairs {', '.join(missing)}")
    else:
        chk.ok(True, f"P2P mesh among {len(members)} members, every pair hears the other")
    return good


def room_view(chk: Check, host: SimClient, guests: list[SimClient]) -> None:
    hs = host.peer_names()
    chk.ok(hs == {g.name for g in guests}, f"host's members {sorted(hs)} == {sorted(g.name for g in guests)}")


# ---- scenarios -------------------------------------------------------------------

def sc_coop2(b: Bench, nat_host="none", nat_guest="none") -> Check:
    chk = Check(f"coop2 host={nat_host} guest={nat_guest}")
    h = b.client("Host", nat_host)
    g = b.client("Guest1", nat_guest)
    if summon_into(b, h, g, chk):
        mesh_ok(chk, [h, g])
    return chk


def sc_coop3(b: Bench) -> Check:
    chk = Check("coop3: host + two cooperators")
    h = b.client("Host")
    g1 = b.client("Guest1")
    g2 = b.client("Guest2")
    summon_into(b, h, g1, chk)
    summon_into(b, h, g2, chk)
    mesh_ok(chk, [h, g1, g2])
    room_view(chk, h, [g1, g2])
    return chk


def sc_leave_rejoin(b: Bench) -> Check:
    """Three in a room; one cooperator goes home (LeaveRoom); the other must
    stay; the one who left is summoned again and the mesh comes back."""
    chk = Check("coop3 leave + rejoin")
    h = b.client("Host")
    g1 = b.client("Guest1")
    g2 = b.client("Guest2")
    summon_into(b, h, g1, chk)
    summon_into(b, h, g2, chk)
    room = h.room
    g1.leave_room()
    wait(lambda: "Guest1" not in h.peer_names(), 5)
    time.sleep(2)
    chk.ok(g2.room == room, f"Guest2 still in room {room} after Guest1 left (now {g2.room})")
    chk.ok(not b.server_lines(f"MP session closed (host left): id={h.room_sid}"),
           "the server kept the host's room")
    mesh_ok(chk, [h, g2])
    summon_into(b, h, g1, chk)
    mesh_ok(chk, [h, g1, g2])
    ids = sorted(m for m in h.peers)
    chk.note(f"member ids after the rejoin: {ids}")
    chk.ok(max(ids or [0]) <= 5, "member ids stay within the room's 5 slots")
    return chk


def sc_churn(b: Bench, rounds=4) -> Check:
    """A cooperator leaves and comes back again and again while another stays."""
    chk = Check(f"coop3 churn x{rounds}")
    h = b.client("Host")
    g1 = b.client("Guest1")
    g2 = b.client("Guest2")
    summon_into(b, h, g1, chk)
    summon_into(b, h, g2, chk)
    for i in range(rounds):
        g1.leave_room()
        wait(lambda: "Guest1" not in h.peer_names(), 5)
        if not summon_into(b, h, g1, chk):
            break
    mesh_ok(chk, [h, g1, g2])
    chk.note(f"member ids at the end: {sorted(h.peers)}")
    chk.ok(max(list(h.peers) or [0]) <= 5, "member ids stay within the room's 5 slots")
    return chk


def sc_five(b: Bench) -> Check:
    """Host, two cooperators, two invaders: the five a chalice dungeon allows."""
    chk = Check("five: host + 2 cooperators + 2 invaders")
    h = b.client("Host")
    g = [b.client("Coop1"), b.client("Coop2")]
    inv = [b.client("Red1"), b.client("Red2")]
    for x in g:
        summon_into(b, h, x, chk)
    # Both invaders wait with their signs up and ask for invader signs
    # themselves: the Sinister bell's SpEffect 9025 brings a maiden to the
    # ringer's own world, so a waiting invader can be invaded too, and the
    # server answers it as it answers any host.
    for x in inv:
        x.put_sign("invade")
    seen = [s.get("UserId") for s in inv[1].find_signs("invade")]
    chk.ok(inv[0].user_id in seen, f"a waiting invader is offered another invader's sign ({seen})")
    for x in inv:
        summon_into(b, h, x, chk, kind="invade")
    mesh_ok(chk, [h] + g + inv, timeout=10)
    room_view(chk, h, g + inv)
    sixth = b.client("Coop3")
    sixth.put_sign("coop")
    h.summon(sixth)
    time.sleep(6)
    chk.ok(not sixth.room, "a sixth player is refused (the room holds 5)")
    return chk


def sc_guest_crash(b: Bench) -> Check:
    chk = Check("coop3 guest crash (heartbeat timeout)")
    h = b.client("Host")
    g1 = b.client("Guest1")
    g2 = b.client("Guest2")
    summon_into(b, h, g1, chk)
    summon_into(b, h, g2, chk)
    room = h.room
    g1.crash()
    gone = wait(lambda: "Guest1" not in h.peer_names(), b.heartbeat_timeout + 25)
    chk.ok(gone, "the host is told Guest1 left after the heartbeat timeout")
    time.sleep(3)
    chk.ok(g2.room == room and h.room == room, "Guest2 and the host keep the room")
    mesh_ok(chk, [h, g2])
    return chk


def sc_host_leaves(b: Bench) -> Check:
    chk = Check("host leaves: guests are told")
    h = b.client("Host")
    g1 = b.client("Guest1")
    g2 = b.client("Guest2")
    summon_into(b, h, g1, chk)
    summon_into(b, h, g2, chk)
    h.leave_room()
    chk.ok(wait(lambda: not g1.room and not g2.room, 5), "both guests hear room_closed")
    return chk


def sc_kick(b: Bench) -> Check:
    chk = Check("host kicks a cooperator")
    h = b.client("Host")
    g1 = b.client("Guest1")
    g2 = b.client("Guest2")
    summon_into(b, h, g1, chk)
    summon_into(b, h, g2, chk)
    mid = next((m for m, p in h.peers.items() if p["online"] == "Guest1"), 0)
    h.kick(mid)
    chk.ok(wait(lambda: not g1.room, 5), "Guest1 hears it was kicked")
    chk.ok(wait(lambda: "Guest1" not in g2.peer_names(), 5), "Guest2 hears Guest1 left")
    ev = [e for e in g1.events if e.get("Name") == "room_member_kicked"]
    chk.ok(bool(ev) and ev[0].get("OptData"), "the kick carries the host's 4-byte reason (OptData)")
    return chk


def sc_lost_leave(b: Bench) -> Check:
    """Guest1's game dies and comes straight back (its leave never reached
    the server) and is summoned again within the heartbeat timeout."""
    chk = Check("rejoin before the old membership timed out")
    h = b.client("Host")
    g1 = b.client("Guest1")
    g2 = b.client("Guest2")
    summon_into(b, h, g1, chk)
    summon_into(b, h, g2, chk)
    g1.crash()
    g1b = b.client("Guest1")  # the same player, a new run
    if summon_into(b, h, g1b, chk):
        mesh_ok(chk, [h, g1b, g2])
        joins = [e for e in h.events if e.get("Name") == "room_member_joined" and e.get("OnlineId") == "Guest1"]
        chk.ok(len(joins) >= 2, f"the host was told Guest1 joined again ({len(joins)} joins)")
    return chk


def sc_idle_vport(b: Bench) -> Check:
    """A player waits longer than the relay's idle release with a sign up,
    then is summoned: the address the host resolves must still reach it."""
    chk = Check(f"summon after {b.vport_ttl or 600:.0f}s idle (relay idle release)")
    h = b.client("Host")
    g = b.client("Guest1")
    time.sleep((b.vport_ttl or 600) + 40)
    r = h._resolve("Guest1")
    chk.ok(bool(r) and not r[0].startswith("127.0.0.1:") and r[0] != "" and r != ("127.0.0.1", g.p2p_port),
           f"resolve answers a reachable address ({r})")
    summon_into(b, h, g, chk)
    return chk


SCENARIOS = {
    "coop2": sc_coop2,
    "coop3": sc_coop3,
    "leave_rejoin": sc_leave_rejoin,
    "churn": sc_churn,
    "five": sc_five,
    "guest_crash": sc_guest_crash,
    "host_leaves": sc_host_leaves,
    "kick": sc_kick,
    "lost_leave": sc_lost_leave,
}
NAT_MATRIX = [(h, g) for h in ("none", "port") for g in ("full", "addr", "port", "sym")]


def run_one(b: Bench, name: str, fn, *args) -> Check:
    print(f"== {name} {' '.join(map(str, args))}", flush=True)
    b.start_server()
    try:
        chk = fn(b, *args)
    except Exception:
        chk = Check(name)
        chk.ok(False, "scenario raised:\n" + traceback.format_exc())
    finally:
        b.stop_clients()
        b.stop_server()
        tag = "-".join([name] + [str(x) for x in args])
        with open(os.path.join(b.work, f"{tag}.log"), "w") as f:
            f.write("\n".join(b.lines) + "\n")
        if os.path.exists(b.server_log):
            shutil.copy(b.server_log, os.path.join(b.work, f"{tag}.server.log"))
        b.lines = []
    for line in chk.notes + chk.fails:
        print("   " + line, flush=True)
    print(f"   -> {'PASS' if chk.passed else 'FAIL'}", flush=True)
    return chk


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--server-tree", required=True, help="the private server's package directory")
    ap.add_argument("--python", default=None, help="the server's python (default: the venv beside its package)")
    ap.add_argument("--quirks", choices=("new", "old"), default="new")
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--nat-matrix", action="store_true", help="coop2 across host/guest NAT kinds")
    ap.add_argument("--idle", type=float, default=0, help="also run the idle-vport scenario with this TTL")
    ap.add_argument("--work", default=None)
    ap.add_argument("--serve", action="store_true",
                    help="only run the server (for tools/nettest/game_trio.sh) until interrupted")
    ap.add_argument("--http-port", type=int, default=28671)
    ap.add_argument("--ssinfo-port", type=int, default=0)
    ap.add_argument("--stun-port", type=int, default=23478)
    ap.add_argument("--relay-ports", default="24000-24199")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    py = a.python or os.path.join(os.path.dirname(os.path.abspath(a.server_tree)), "venv", "bin", "python")
    if not os.path.exists(py):
        py = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.server_tree))), "..", "venv", "bin",
                          "python")
    work = a.work or tempfile.mkdtemp(prefix="nettest-")
    os.makedirs(work, exist_ok=True)
    lo, hi = (int(x) for x in a.relay_ports.split("-"))
    b = Bench(a.server_tree, py, work, quirks=OLD_QUIRKS if a.quirks == "old" else (), verbose=a.verbose,
              vport_ttl=a.idle or None, http_port=a.http_port, stun_port=a.stun_port, relay=(lo, hi),
              ssinfo_port=a.ssinfo_port or None,
              env={"BB_PUBLIC_HOST": "127.0.0.1"} if a.serve else None)
    if a.serve:
        # Block the signals first: sigwait only takes blocked ones, and an
        # unblocked SIGTERM would end this process with the server still up.
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT, signal.SIGTERM})
        b.start_server()
        print(f"server up: http {b.http_base}, stun {b.stun_addr[0]}:{b.stun_addr[1]}, relay {lo}-{hi}; "
              f"log {b.server_log}", flush=True)
        try:
            signal.sigwait({signal.SIGINT, signal.SIGTERM})
        finally:
            b.stop_server()
        return 0
    results = []
    names = a.only if a.only else list(SCENARIOS)
    for n in names:
        if n in SCENARIOS:
            results.append(run_one(b, n, SCENARIOS[n]))
    if a.nat_matrix:
        for hn, gn in NAT_MATRIX:
            results.append(run_one(b, "coop2", sc_coop2, hn, gn))
    if a.idle:
        results.append(run_one(b, "idle_vport", sc_idle_vport))
    print("\n== summary (quirks %s, server %s)" % (a.quirks, a.server_tree))
    for c in results:
        print(f"   {'PASS' if c.passed else 'FAIL'}  {c.name}")
    print(f"   logs in {work}")
    return 0 if all(c.passed for c in results) else 1


if __name__ == "__main__":
    sys.exit(main())
