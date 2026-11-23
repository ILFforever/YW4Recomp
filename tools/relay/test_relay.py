"""Tests the relay (relay.py) with two fake games: a host makes a room, a station finds and joins it, packets go
both ways and to everyone, the host updates its advertise data, the station leaves, the host closes the room.

    python tools/relay/test_relay.py      (starts a relay on a free port itself)
"""
from __future__ import annotations

import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

NETINFO = 0x480
LDN = 0x50
NODES = LDN + 0x18


class Game:
    def __init__(self, port: int, room: bytes = b"test"):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.send(1, room.ljust(64, b"\0"))
        kind, p = self.recv()
        assert kind == 1 and struct.unpack_from("<I", p)[0] == 3 and len(p) == 12, (kind, p)  # protocol 3, token
        self.token = p[4:12]
        self.port = port

    def udp(self) -> socket.socket:
        """This game's UDP side (protocol 3): bound to the relay, answered BIND."""
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.settimeout(3)
        u.connect(("127.0.0.1", self.port))
        u.send(self.token + bytes([1]))
        assert u.recv(64) == bytes([1]), "no UDP BIND answer"
        return u

    def send(self, kind: int, p: bytes = b"") -> None:
        self.s.sendall(struct.pack("<IB", len(p) + 1, kind) + p)

    def recv(self) -> tuple[int, bytes]:
        head = self._read(4)
        body = self._read(struct.unpack("<I", head)[0])
        return body[0], body[1:]

    def recv_type(self, want: int) -> bytes:
        while True:
            kind, p = self.recv()
            if kind == want:
                return p

    def _read(self, n: int) -> bytes:
        b = b""
        while len(b) < n:
            chunk = self.s.recv(n - len(b))
            assert chunk, "relay closed the connection"
            b += chunk
        return b


def network(lcid: int, name: bytes, session: bytes = bytes(range(16))) -> bytes:
    info = bytearray(NETINFO)
    struct.pack_into("<Q", info, 0, lcid)
    info[0x10:0x20] = session
    info[LDN + 0x16] = 4                # max nodes
    info[NODES + 0xC:NODES + 0xC + len(name)] = name
    return bytes(info)


def main() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    # YW4_RELAY_CMD: the relay to test (the C one: a path to yw4-relay); else relay.py
    cmd = [os.environ["YW4_RELAY_CMD"]] if os.environ.get("YW4_RELAY_CMD") else \
        [sys.executable, str(Path(__file__).with_name("relay.py"))]
    relay = subprocess.Popen(cmd + ["--port", str(port), "--host", "127.0.0.1"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        host, station, stranger = Game(port), Game(port), Game(port, b"other")

        host.send(2, network(0x0100_86C0_0AF7_C000, b"Lennox"))
        p = host.recv_type(2)
        ip, info = struct.unpack_from("<I", p)[0], p[4:]
        assert ip == 0xA9FE0101 and info[LDN + 0x17] == 1, (hex(ip), info[LDN + 0x17])
        print("host: room made, 169.254.1.1, 1 player")

        station.send(4, struct.pack("<Q", 0x0100_86C0_0AF7_C000))
        p = station.recv_type(4)
        assert struct.unpack_from("<I", p)[0] == 1
        found = p[4:4 + NETINFO]
        assert found[NODES + 0xC:NODES + 0xC + 6] == b"Lennox"
        stranger.send(4, struct.pack("<Q", 0))
        assert struct.unpack_from("<I", stranger.recv_type(4))[0] == 0
        upper = Game(port, b"TEST")  # case counts (owner, 2026-11-20)
        upper.send(4, struct.pack("<Q", 0))
        assert struct.unpack_from("<I", upper.recv_type(4))[0] == 0
        upper.s.close()
        print("station: finds the room; another room code, or the same in other case, finds nothing")

        node = bytearray(0x40)
        node[0xC:0xC + 6] = b"Friend"
        station.send(5, found[0x10:0x20] + bytes(node))
        p = station.recv_type(5)
        assert p[0] == 1 and struct.unpack_from("<I", p, 1)[0] == 0xA9FE0102
        info = host.recv_type(6)
        assert info[LDN + 0x17] == 2 and info[NODES + 0x40 + 0xC:NODES + 0x40 + 0xC + 6] == b"Friend"
        print("station: joins as 169.254.1.2; host sees 2 players")

        # the host joining its own room is refused, and the room (and the server) live on: it used to close the room
        # and crash the relay on r->host->name (code audit, 2026-11-24)
        host.send(5, found[0x10:0x20] + bytes(0x40))
        p = host.recv_type(5)
        assert p[0] == 0, "the host joined its own room"
        assert relay.poll() is None, "the relay died"
        print("host: joining its own room is refused; the room stays")

        # a packet from the station to the host, its sender filled in by the relay
        station.send(8, struct.pack("<IHIH", 0xA9FE0101, 49152, 0, 50000) + b"hello host")
        p = host.recv_type(8)
        dst, dport, src, sport = struct.unpack_from("<IHIH", p)
        assert (dst, dport, src, sport, p[12:]) == (0xA9FE0101, 49152, 0xA9FE0102, 50000, b"hello host")
        # a broadcast from the host
        host.send(8, struct.pack("<IHIH", 0xA9FE01FF, 49152, 0xA9FE0101, 49152) + b"to all")
        p = station.recv_type(8)
        assert struct.unpack_from("<I", p, 6)[0] == 0xA9FE0101 and p[12:] == b"to all"
        print("packets: station -> host and host -> everyone arrive with the right sender")

        # UDP (protocol 3): once both are bound, packets go and come by UDP; a datagram with a wrong token is ignored
        hu, su = host.udp(), station.udp()
        su.send(station.token + bytes([8]) + struct.pack("<IHIH", 0xA9FE0101, 49152, 0, 50000) + b"by udp")
        p = hu.recv(2048)
        assert p[0] == 8 and struct.unpack_from("<I", p, 7)[0] == 0xA9FE0102 and p[13:] == b"by udp", p
        hu.send(host.token + bytes([8]) + struct.pack("<IHIH", 0xA9FE01FF, 49152, 0, 49152) + b"udp to all")
        p = su.recv(2048)
        assert p[0] == 8 and p[13:] == b"udp to all", p
        su.send(bytes(8) + bytes([1]))  # not a token the relay gave
        su.settimeout(0.5)
        try:
            extra = su.recv(64)
        except socket.timeout:
            extra = b""
        assert extra == b"", "the relay answered a datagram without a valid token"
        print("UDP: bound, packets both ways by UDP, a wrong token gets no answer")

        upd = bytearray(info)
        upd[LDN + 0x21C:LDN + 0x21C + 4] = b"ADV!"
        host.send(3, bytes(upd))
        info = station.recv_type(6)
        assert info[LDN + 0x21C:LDN + 0x21C + 4] == b"ADV!" and info[LDN + 0x17] == 2
        print("host: new advertise data reaches the station, node table kept")

        station.send(7)
        info = host.recv_type(6)
        assert info[LDN + 0x17] == 1
        print("station: leaves; host sees 1 player")

        station.send(5, found[0x10:0x20] + bytes(node))
        station.recv_type(5)
        host.recv_type(6)
        host.send(7)
        assert station.recv_type(7)[0] == 3
        station.send(4, struct.pack("<Q", 0))
        assert struct.unpack_from("<I", station.recv_type(4))[0] == 0
        print("host: closes the room; the station is told (DestroyedByUser), the room is gone")

        host.send(10)
        assert host.recv_type(10) == b""
        print("heartbeat: PING gets a PONG")

        # 2026-11-20: an old room's host leaving took a newer room with the same session id with it
        lcid = 0x0100_86C0_0AF7_C000
        old, new, finder = Game(port, b"same"), Game(port, b"same"), Game(port, b"same")
        old.send(2, network(lcid, b"Old"))
        old.recv_type(2)
        new.send(2, network(lcid, b"New"))
        new.recv_type(2)
        old.s.close()
        time.sleep(0.3)
        finder.send(4, struct.pack("<Q", lcid))
        p = finder.recv_type(4)
        assert struct.unpack_from("<I", p)[0] == 1 and p[4 + NODES + 0xC:4 + NODES + 0xF] == b"New", p[:8]
        print("same session id: the old host leaving keeps the newer room")
        for g in (new, finder):
            g.s.close()

        # rooms per address: 4 (all these come from 127.0.0.1); the 5th is answered short
        games = [Game(port, b"many") for _ in range(5)]
        for i, g in enumerate(games):
            g.send(2, network(lcid, b"R", bytes([i + 1]) * 16))
        answers = [len(g.recv_type(2)) for g in games]
        assert answers[:4] == [4 + NETINFO] * 4 and answers[4] == 0, answers
        print("limits: a 5th room from one address is refused, short")
        for g in games:
            g.s.close()

        # a first frame that isn't the hello closes the connection
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
        s.sendall(struct.pack("<IB", 9, 4) + bytes(8))
        assert s.recv(16) == b"", "the relay answered a client that never said hello"
        s.close()
        print("hello first: anything else first is closed")
        ok = True
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0]
        if "--verbose" in sys.argv:
            print(out)
    assert "127.0.0.1" not in out, "a client's address is in the relay's log"
    print("privacy: no client address in the relay's log")
    print("all relay tests passed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
