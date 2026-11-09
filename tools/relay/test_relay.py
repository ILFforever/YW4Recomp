"""Tests the relay (relay.py) with two fake games: a host makes a room, a station finds and joins it, packets go
both ways and to everyone, the host updates its advertise data, the station leaves, the host closes the room.

    python tools/relay/test_relay.py      (starts a relay on a free port itself)
"""
from __future__ import annotations

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
        assert kind == 1 and struct.unpack("<I", p)[0] == 1, (kind, p)

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


def network(lcid: int, name: bytes) -> bytes:
    info = bytearray(NETINFO)
    struct.pack_into("<Q", info, 0, lcid)
    info[0x10:0x20] = bytes(range(16))  # session id
    info[LDN + 0x16] = 4                # max nodes
    info[NODES + 0xC:NODES + 0xC + len(name)] = name
    return bytes(info)


def main() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    relay = subprocess.Popen([sys.executable, str(Path(__file__).with_name("relay.py")), "--port", str(port),
                              "--host", "127.0.0.1"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
        print("station: finds the room; another room code finds nothing")

        node = bytearray(0x40)
        node[0xC:0xC + 6] = b"Friend"
        station.send(5, found[0x10:0x20] + bytes(node))
        p = station.recv_type(5)
        assert p[0] == 1 and struct.unpack_from("<I", p, 1)[0] == 0xA9FE0102
        info = host.recv_type(6)
        assert info[LDN + 0x17] == 2 and info[NODES + 0x40 + 0xC:NODES + 0x40 + 0xC + 6] == b"Friend"
        print("station: joins as 169.254.1.2; host sees 2 players")

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
        print("all relay tests passed")
        return 0
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0]
        if "--verbose" in sys.argv:
            print(out)


if __name__ == "__main__":
    sys.exit(main())
