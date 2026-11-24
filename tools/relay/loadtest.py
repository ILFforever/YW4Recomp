"""Load test for the relay (decision 0082): N two-player rooms sending mission-like traffic at once, to find where a
server stops keeping up. Each fake player connects as a game does (TCP hello, a room made or joined, UDP bound),
then sends `--rate` UDP packets a second of `--size` bytes to the other player (a 2-player Blasters mission: about 62
packets a second of 900 bytes each way, measured on the Singapore relay, 2026-11-20). Every packet carries the time
it was sent, so the receiver measures the delay through the relay.

    python tools/relay/loadtest.py --host 127.0.0.1 --rooms 20 --seconds 20 [--pid 1234]
    python tools/relay/loadtest.py --host 161.118.234.155 --rooms 5 --ssh ubuntu@161.118.234.155

--pid (a relay on this machine) or --ssh (a server, with the yw4_relay key) reads the relay's CPU time before and
after: CPU per packet is what sets how many rooms a server can carry. Standard library only.
"""
from __future__ import annotations

import argparse
import os
import selectors
import socket
import struct
import subprocess
import time
from pathlib import Path

NETINFO = 0x480
LDN = 0x50
NODES = LDN + 0x18
LCID = 0x0100_86C0_0AF7_C000


def frame(kind: int, p: bytes = b"") -> bytes:
    return struct.pack("<IB", len(p) + 1, kind) + p


def recv_frame(s: socket.socket) -> tuple[int, bytes]:
    def read(n: int) -> bytes:
        b = b""
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c:
                raise ConnectionError("the relay closed the connection")
            b += c
        return b
    n = struct.unpack("<I", read(4))[0]
    body = read(n)
    return body[0], body[1:]


def recv_type(s: socket.socket, want: int) -> bytes:
    while True:
        k, p = recv_frame(s)
        if k == want:
            return p


class Player:
    def __init__(self, host: str, port: int, code: bytes):
        self.t = socket.create_connection((host, port), timeout=10)
        self.t.sendall(frame(1, code.ljust(64, b"\0")))
        p = recv_type(self.t, 1)
        if struct.unpack_from("<I", p)[0] < 3 or len(p) < 12:
            raise SystemExit("the relay doesn't speak protocol 3 (UDP)")
        self.token = p[4:12]
        self.ip = 0
        self.u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.u.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.u.connect((host, port))
        self.u.settimeout(5)
        self.u.send(self.token + b"\x01")
        if self.u.recv(64) != b"\x01":
            raise SystemExit("no UDP answer from the relay")
        self.u.setblocking(False)


def network(session: bytes) -> bytes:
    info = bytearray(NETINFO)
    struct.pack_into("<Q", info, 0, LCID)
    info[0x10:0x20] = session
    info[LDN + 0x16] = 2
    return bytes(info)


def relay_cpu(args) -> float | None:
    """The relay's CPU seconds so far (utime + stime), from /proc here (--pid) or on a server (--ssh)."""
    cmd = None
    if args.pid:
        try:
            with open(f"/proc/{args.pid}/stat") as fh:
                stat = fh.read()
            f = stat.rsplit(")", 1)[1].split()  # after the command name, which may hold spaces: state is f[0]
            return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")
        except (OSError, ValueError, IndexError):
            return None
    if args.ssh:
        cmd = ["ssh", "-i", str(Path.home() / ".ssh" / "yw4_relay"), "-o", "BatchMode=yes", args.ssh,
               "p=$(systemctl show yw4-relay -p MainPID --value); awk '{sub(/^.*[)] /,\"\"); print $12+$13}' /proc/$p/stat; getconf CLK_TCK"]
    if not cmd:
        return None
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.split()
        return int(out[0]) / int(out[1])
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
        return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=41230)
    ap.add_argument("--rooms", type=int, default=10)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--rate", type=float, default=62, help="packets a second per player (default 62)")
    ap.add_argument("--size", type=int, default=900, help="bytes per packet (default 900)")
    ap.add_argument("--pid", type=int, help="the relay's process id, on this machine")
    ap.add_argument("--ssh", help="user@server: read the relay's CPU there")
    args = ap.parse_args()

    players: list[Player] = []
    code = b"load-" + os.urandom(4).hex().encode()  # a code nobody else uses: players never see these rooms
    for r in range(args.rooms):
        host, guest = Player(args.host, args.port, code), Player(args.host, args.port, code)
        session = os.urandom(16)
        host.t.sendall(frame(2, network(session)))
        host.ip = struct.unpack_from("<I", recv_type(host.t, 2))[0]
        guest.t.sendall(frame(5, session + bytes(0x40)))
        p = recv_type(guest.t, 5)
        if not p or p[0] != 1:
            raise SystemExit(f"room {r}: join refused (server limits? 4 rooms per address by default)")
        guest.ip = struct.unpack_from("<I", p, 1)[0]
        host.peer, guest.peer = guest.ip, host.ip
        players += [host, guest]
    print(f"{args.rooms} rooms, {len(players)} players: {args.rate:.0f} packets/s of {args.size} bytes each, "
          f"{args.seconds:.0f} s", flush=True)

    sel = selectors.DefaultSelector()
    for p in players:
        sel.register(p.u, selectors.EVENT_READ)
    pad = bytes(max(0, args.size - 12 - 16))
    sent = got = 0
    delays: list[float] = []
    cpu0 = relay_cpu(args)
    start = time.perf_counter()
    step = 1.0 / args.rate
    next_send = start
    while True:
        now = time.perf_counter()
        if now - start >= args.seconds:
            break
        if now >= next_send:  # every player sends one packet to the other
            for p in players:
                body = struct.pack("<IHIH", p.peer, 49152, 0, 49152) + struct.pack("<dQ", now, sent) + pad
                try:
                    p.u.send(p.token + b"\x08" + body)
                    sent += 1
                except (BlockingIOError, OSError):
                    pass
            next_send += step
        for key, _ in sel.select(timeout=max(0.0, min(next_send - time.perf_counter(), 0.01))):
            while True:
                try:
                    d = key.fileobj.recv(65536)
                except (BlockingIOError, OSError):
                    break
                if len(d) >= 13 + 16 and d[0] == 8:
                    got += 1
                    delays.append(time.perf_counter() - struct.unpack_from("<d", d, 13)[0])
    end = time.perf_counter() + 1.0  # the last ones on their way
    while time.perf_counter() < end:
        for key, _ in sel.select(timeout=0.05):
            while True:
                try:
                    d = key.fileobj.recv(65536)
                except (BlockingIOError, OSError):
                    break
                if len(d) >= 13 + 16 and d[0] == 8:
                    got += 1
                    delays.append(time.perf_counter() - struct.unpack_from("<d", d, 13)[0])
    elapsed = time.perf_counter() - start
    cpu1 = relay_cpu(args)
    for p in players:
        p.t.close()
        p.u.close()

    delays.sort()
    pct = lambda q: delays[min(len(delays) - 1, int(len(delays) * q))] * 1000 if delays else float("nan")
    loss = 100.0 * (1 - got / sent) if sent else 0.0
    print(f"sent {sent}, received {got} ({loss:.2f}% lost), {sent / args.seconds:.0f} packets/s into the relay")
    print(f"delay through the relay: p50 {pct(0.5):.1f} ms, p95 {pct(0.95):.1f} ms, p99 {pct(0.99):.1f} ms, "
          f"worst {pct(1.0):.1f} ms")
    if cpu0 is not None and cpu1 is not None:
        used = cpu1 - cpu0
        print(f"relay CPU: {used:.2f} s in {elapsed:.0f} s = {100 * used / elapsed:.1f}% of one core; "
              f"{1e6 * used / max(sent, 1):.1f} us per packet in (each forwarded once)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
