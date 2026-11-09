"""The relay for playing together over the internet (decision 0057).

Every game connects here over TCP. The game's local play (nn::ldn) creates, finds and joins rooms here, and the
UDP packets between the players of a room pass through here. The relay knows nothing of the game: rooms are
NetworkInfo blobs (0x480 bytes, Switchbrew "LDN services"), packets are opaque.

    python tools/relay/relay.py [--port 41230]

Players see each other only with the same room code (YW4_ROOM, empty is a room code too). Standard library only.

Protocol, both ways: frames of u32 length (little endian, the bytes after it), u8 type, payload.

    client -> relay                                   relay -> client
    1 HELLO   room code (64 bytes, NUL padded)         1 WELCOME  u32 protocol version
    2 CREATE  NetworkInfo                              2 CREATED  u32 ip, NetworkInfo
    3 UPDATE  NetworkInfo (the host's new advertise    6 NETINFO  NetworkInfo (the room changed: a node came or
              data, accept policy...)                             went, or the host updated it)
    4 SCAN    u64 local communication id (0: any)      4 FOUND    u32 count, count x NetworkInfo
    5 JOIN    16-byte session id, NodeInfo (0x40)      5 JOINED   u8 ok, u32 ip, NetworkInfo
    7 LEAVE   -                                        7 CLOSED   u8 reason (the room is gone, or we were rejected)
    8 PACKET  u32 dst ip, u16 dst port, u32 src ip,    8 PACKET   the same, from another member
              u16 src port, payload
    9 REJECT  u32 ip (the host sends a node away)

Addresses are u32 in host order (nn::ldn's). The host is 169.254.1.1, the others .2, .3, ... in join order.
"""
from __future__ import annotations

import argparse
import asyncio
import struct
import time

PROTOCOL = 1
NETINFO = 0x480
LDN = 0x50            # LdnNetworkInfo inside NetworkInfo
NODES = LDN + 0x18    # 8 NodeInfo of 0x40
NODE = 0x40
BASE_IP = 0xA9FE0100  # 169.254.1.0
MAX_FRAME = 1 << 16

HELLO, CREATE, UPDATE, SCAN, JOIN, NETINFO_MSG, LEAVE, PACKET, REJECT = 1, 2, 3, 4, 5, 6, 7, 8, 9


def ip_str(a: int) -> str:
    return f"{a >> 24}.{a >> 16 & 255}.{a >> 8 & 255}.{a & 255}"


class Room:
    def __init__(self, code: bytes, host: "Client", info: bytes):
        self.code, self.host = code, host
        self.info = bytearray(info)
        self.members: dict[int, Client] = {}  # ip -> client
        self.session = bytes(self.info[0x10:0x20])
        self.lcid = struct.unpack_from("<Q", self.info, 0)[0]

    def node_count(self) -> int:
        return sum(1 for i in range(8) if self.info[NODES + NODE * i + 0xB])

    def rebuild_nodes(self) -> None:
        """The node table from the members: index by ip (host .1 = node 0)."""
        for i in range(8):
            off = NODES + NODE * i
            ip = BASE_IP + 1 + i
            c = self.members.get(ip)
            if c is None:
                self.info[off:off + NODE] = bytes(NODE)
                continue
            node = bytearray(c.node)
            struct.pack_into("<I", node, 0, ip)
            node[4:10] = bytes([0x02, 0x00, 0x59, 0x57, 0x34, i + 1])  # MAC, as the game's own host node has
            node[0xA] = i
            node[0xB] = 1
            self.info[off:off + NODE] = node
        self.info[LDN + 0x17] = self.node_count()

    def free_ip(self) -> int | None:
        maxn = self.info[LDN + 0x16] or 8
        for i in range(min(maxn, 8)):
            ip = BASE_IP + 1 + i
            if ip not in self.members:
                return ip
        return None

    async def broadcast_info(self, skip: "Client | None" = None) -> None:
        for c in list(self.members.values()):
            if c is not skip:
                await c.send(NETINFO_MSG, bytes(self.info))


class Relay:
    def __init__(self) -> None:
        self.rooms: dict[bytes, Room] = {}  # session id -> room
        self.clients: set[Client] = set()

    def rooms_for(self, code: bytes, lcid: int) -> list[Room]:
        return [r for r in self.rooms.values() if r.code == code and (lcid == 0 or r.lcid == lcid)]


class Client:
    def __init__(self, relay: Relay, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.relay, self.reader, self.writer = relay, reader, writer
        self.code = b""
        self.room: Room | None = None
        self.ip = 0
        self.node = bytes(NODE)
        peer = writer.get_extra_info("peername")
        self.name = f"{peer[0]}:{peer[1]}" if peer else "?"
        self.lock = asyncio.Lock()

    async def send(self, kind: int, payload: bytes = b"") -> None:
        async with self.lock:
            try:
                self.writer.write(struct.pack("<IB", len(payload) + 1, kind) + payload)
                await self.writer.drain()
            except (ConnectionError, OSError):
                pass

    def log(self, msg: str) -> None:
        print(f"{time.strftime('%H:%M:%S')} {self.name}: {msg}", flush=True)

    async def leave(self, reason: int = 0) -> None:
        room, self.room = self.room, None
        if room is None:
            return
        room.members.pop(self.ip, None)
        if room.host is self:
            self.log(f"closes the room ({len(room.members)} others in it)")
            self.relay.rooms.pop(room.session, None)
            for c in list(room.members.values()):
                c.room = None
                await c.send(LEAVE, bytes([3]))  # DestroyedByUser
        else:
            self.log(f"leaves the room ({ip_str(self.ip)})")
            room.rebuild_nodes()
            await room.broadcast_info()
        self.ip = 0

    async def handle(self, kind: int, p: bytes) -> None:
        relay = self.relay
        if kind == HELLO:
            self.code = p[:64].rstrip(b"\0")
            self.log(f"hello, room code {self.code.decode(errors='replace')!r}")
            await self.send(HELLO, struct.pack("<I", PROTOCOL))
        elif kind == CREATE and len(p) >= NETINFO:
            await self.leave()
            room = Room(self.code, self, p[:NETINFO])
            self.room, self.ip = room, BASE_IP + 1
            self.node = bytes(p[NODES:NODES + NODE])  # the host's own node, as its game made it
            room.members[self.ip] = self
            room.rebuild_nodes()
            relay.rooms[room.session] = room
            self.log(f"creates a room (game {room.lcid:016x}, up to {room.info[LDN + 0x16]} players)")
            await self.send(CREATE, struct.pack("<I", self.ip) + bytes(room.info))
        elif kind == UPDATE and len(p) >= NETINFO and self.room and self.room.host is self:
            room = self.room
            nodes = bytes(room.info[NODES:NODES + 8 * NODE])
            room.info[:] = p[:NETINFO]
            room.info[NODES:NODES + 8 * NODE] = nodes  # the relay keeps the node table
            room.info[LDN + 0x17] = room.node_count()
            await room.broadcast_info(skip=self)  # the host knows its own change
        elif kind == SCAN and len(p) >= 8:
            lcid = struct.unpack_from("<Q", p)[0]
            found = [r for r in relay.rooms_for(self.code, lcid) if r is not self.room]
            await self.send(SCAN, struct.pack("<I", len(found)) + b"".join(bytes(r.info) for r in found[:24]))
        elif kind == JOIN and len(p) >= 16 + NODE:
            room = relay.rooms.get(bytes(p[:16]))
            ip = room.free_ip() if room and room.code == self.code else None
            if room is None or ip is None or room.info[LDN + 0x12] == 1:  # gone, full, or AlwaysReject
                self.log("join refused")
                await self.send(JOIN, struct.pack("<BI", 0, 0) + bytes(NETINFO))
                return
            await self.leave()
            self.room, self.ip, self.node = room, ip, bytes(p[16:16 + NODE])
            room.members[ip] = self
            room.rebuild_nodes()
            self.log(f"joins {room.host.name}'s room as {ip_str(ip)}")
            await self.send(JOIN, struct.pack("<BI", 1, ip) + bytes(room.info))
            for c in list(room.members.values()):
                if c is not self:
                    await c.send(NETINFO_MSG, bytes(room.info))
        elif kind == LEAVE:
            await self.leave()
        elif kind == REJECT and len(p) >= 4 and self.room and self.room.host is self:
            ip = struct.unpack_from("<I", p)[0]
            c = self.room.members.get(ip)
            if c and c is not self:
                await c.leave()
                await c.send(LEAVE, bytes([5]))  # Rejected
        elif kind == PACKET and len(p) >= 12 and self.room:
            dst = struct.unpack_from("<I", p)[0]
            src = struct.pack("<I", self.ip)  # the sender's address is the relay's word, not the sender's
            frame = p[:6] + src + p[10:]
            if dst == 0xFFFFFFFF or dst & 0xFF == 0xFF:
                targets = [c for ip, c in self.room.members.items() if ip != self.ip]
            else:
                c = self.room.members.get(dst)
                targets = [c] if c and c is not self else []
            for c in targets:
                await c.send(PACKET, frame)

    async def run(self) -> None:
        self.log("connected")
        try:
            while True:
                head = await self.reader.readexactly(4)
                n = struct.unpack("<I", head)[0]
                if not 1 <= n <= MAX_FRAME:
                    break
                body = await self.reader.readexactly(n)
                await self.handle(body[0], body[1:])
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            await self.leave()
            self.relay.clients.discard(self)
            self.log("gone")
            self.writer.close()


async def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=41230)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()
    relay = Relay()

    async def accept(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        sock = writer.get_extra_info("socket")
        if sock is not None:
            import socket
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c = Client(relay, reader, writer)
        relay.clients.add(c)
        await c.run()

    server = await asyncio.start_server(accept, args.host, args.port)
    print(f"YW4 Recomp relay on port {args.port} (protocol {PROTOCOL}). Ctrl+C stops it.", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
