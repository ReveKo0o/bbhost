#!/usr/bin/env python3
"""The verdict on a tools/nettest/game_trio.sh run: reads the three
instances' logs and the server's, prints what happened and PASS/FAIL."""
import os
import re
import sys

# A crash, not the startup line "patched DL_PANIC at ... -> host abort".
CRASH = r"Segmentation fault|dumped core|DL_PANIC\(|DL_PANIC:|host abort:|terminate called"
five = "--five" in sys.argv
out = [a for a in sys.argv[1:] if a != "--five"][0]


def lines(*parts):
    p = os.path.join(out, *parts)
    return open(p, errors="replace").read().splitlines() if os.path.exists(p) else []


if five:
    # game_five.sh: a host, two cooperators, two invaders.
    roles = ("host", "coop1", "coop2", "red1", "red2")
    logs = {r: lines(r, "drive", "run.log") for r in roles}
    srv = lines("server", "server.log")
    fails, notes = [], []

    def check(cond, what):
        (notes if cond else fails).append(("ok   " if cond else "FAIL ") + what)

    for r in roles:
        log = logs[r]
        check(bool(log), f"{r} ran ({len(log)} lines)")
        check(not any(re.search(CRASH, l) for l in log), f"{r} did not crash")
    joins = [l for l in logs["host"] if re.search(r"np: member \d+ \(Hunter\d\) joined room", l)]
    for l in joins:
        notes.append("     host: " + re.sub(r".*np: ", "", l))
    names = {re.search(r"\((Hunter\d)\)", l).group(1) for l in joins}
    check({"Hunter2", "Hunter3"} <= names, "both cooperators joined")
    check("Hunter4" in names or "Hunter5" in names, "an invader joined")
    check({"Hunter4", "Hunter5"} <= names, "both invaders joined: five in one room")
    for l in logs["host"]:
        if re.search(r"np test: flag|np: room \d+ gone|SummonType|summon_messenger", l):
            notes.append("     host: " + l.split("] ", 1)[-1][:150])
    for l in srv:
        if re.search(r"MP session (joined|guest left|full)|Room full", l):
            notes.append("     server: " + re.sub(r"^.*?\[INFO\] ", "", l)[:150])
    print("\n".join(notes + fails))
    print("game_five:", "PASS" if not fails else "FAIL")
    sys.exit(1 if fails else 0)

host = lines("host", "drive", "run.log")
g1 = lines("guest1", "drive", "run.log")
g2 = lines("guest2", "drive", "run.log")
srv = lines("server", "server.log")
fails, notes = [], []


def check(cond, what):
    (notes if cond else fails).append(("ok   " if cond else "FAIL ") + what)


def grep(log, pat):
    r = re.compile(pat)
    return [l for l in log if r.search(l)]


# A crash, not the startup line "patched DL_PANIC at ... -> host abort".
CRASH = r"Segmentation fault|dumped core|DL_PANIC\(|DL_PANIC:|host abort:|terminate called"


for name, log in (("host", host), ("guest1", g1), ("guest2", g2)):
    check(bool(log), f"{name} ran (log has {len(log)} lines)")
    check(bool(grep(log, r"net: relay on")), f"{name} sends through the relay frames")
    check(not grep(log, CRASH), f"{name} did not crash")

joins = grep(host, r"np: member \d+ \(Hunter[23]\) joined room")
lefts = grep(host, r"np: member \d+ \(Hunter[23]\) left")
notes.append("     host joins: " + "; ".join(re.sub(r".*np: ", "", l) for l in joins))
notes.append("     host leaves: " + "; ".join(re.sub(r".*np: ", "", l) for l in lefts))
check(len([l for l in joins if "Hunter2" in l]) >= 2, "the host saw guest1 join twice (first and after its return)")
check(len([l for l in joins if "Hunter3" in l]) >= 1, "the host saw guest2 join")
check(len([l for l in lefts if "Hunter2" in l]) >= 1, "the host saw guest1 leave")
check(not grep(host, r"session over \(last peer left\)|np: room \d+ gone"), "the host kept its room throughout")
check(len(grep(g1, r"np: joined room")) >= 2, "guest1 joined twice")
g2_joined = grep(g2, r"np: joined room")
check(len(g2_joined) == 1, f"guest2 joined once ({len(g2_joined)})")
# guest2 keeps the room until the host's own run ends (the script stops the
# host first; its heartbeat then lapses and guest2 hears ROOM_DESTROYED).
events = [l for l in g2 if re.search(r"np: (member \d+ \(Hunter2\) joined|room \d+ gone)", l)]
rejoin_at = max((i for i, l in enumerate(events) if "Hunter2) joined" in l), default=-1)
early_loss = [l for l in events[:rejoin_at] if "gone" in l]
late_loss = [l for l in events[rejoin_at + 1:] if "gone" in l]
check(rejoin_at >= 1 and not early_loss, "guest2 kept the room through guest1's leave and return")
if late_loss:
    notes.append("     guest2 at the end: " + re.sub(r".*np: ", "", late_loss[0]))
check(not grep(srv, r"join_room: no session found"), "no join went to a room the server no longer had")
check(not grep(srv, r"MP session closed \(host left\)"), "the server never closed the host's room")
traffic = grep(g2, r"net: p2p port \d+ 60 s: .*vport 30")
if traffic:
    m = re.search(r"vport 30 \(sock \d+\): in (\d+)/", traffic[-1])
    notes.append(f"     guest2's last 60-s line: vport 30 in {m.group(1) if m else '?'} datagrams")
for l in grep(srv, r"MP session (joined|guest left|rejoin)|NpM2 join_room: session"):
    notes.append("     server: " + re.sub(r"^.*?\[INFO\] ", "", l)[:160])
print("\n".join(notes + fails))
print("game_trio:", "PASS" if not fails else "FAIL")
sys.exit(1 if fails else 0)
