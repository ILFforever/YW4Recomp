"""The relay for playing together over the internet (decision 0082).

Every game connects here over TCP. The game's local play (nn::ldn) creates, finds and joins rooms here, and the
UDP packets between the players of a room pass through here. The relay knows nothing of the game: rooms are
NetworkInfo blobs (0x480 bytes, Switchbrew "LDN services"), packets are opaque.

    python tools/relay/relay.py [--port 41230]

Players see each other only with the same room code (YW4_ROOM, empty is a room code too). Case counts ("KEY123" and
"key123" are different rooms: owner, 2026-11-20); spaces before and after don't.
Standard library only.

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
   10 PING    - (protocol 2: every 3 s)               10 PONG     - (the answer: the game knows the relay is there)

Addresses are u32 in host order (nn::ldn's). The host is 169.254.1.1, the others .2, .3, ... in join order.

Protection (2026-11-20): connections and rooms per address and in all, game packets per second per client, hello
within 10 s; a client that sends heartbeats and then goes silent for 15 s is dropped, any other one after about 25 s
of a dead connection (TCP keepalive). Logs name clients by a hash of their address (a new salt each start), never the
address, and a summary line every 5 minutes (tools/relay/status.py reads it from the servers).
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import os
import socket
import struct
import time

PROTOCOL = 3          # 2: PING / PONG heartbeats; 3: game packets over UDP (a token in the welcome)

# UDP (protocol 3), on the same port: datagrams of token (8 bytes, from the welcome) + kind + payload from the game,
# kind + payload to it. BIND (empty) says where the game is (its home router's address and port) and keeps that
# open, every 3 s with the heartbeat; the relay answers it, so the game knows UDP gets through, else it stays on TCP.
# PACKET: the TCP PACKET's payload. A player's packets go to it by UDP while it has been heard from that way lately.
UDP_BIND, UDP_PACKET = 1, 8
UDP_STALE = 10.0      # s without a datagram from a game: its packets go by TCP again
UDP_MAX = 1200        # payload bytes at most by UDP (no IP fragments); bigger go by TCP
NETINFO = 0x480
LDN = 0x50            # LdnNetworkInfo inside NetworkInfo
NODES = LDN + 0x18    # 8 NodeInfo of 0x40
NODE = 0x40
BASE_IP = 0xA9FE0100  # 169.254.1.0
MAX_FRAME = 1 << 16

# limits: generous for real play (a Blasters mission: about 60 packets a second per player, 4 players a room; two
# games on one PC plus the overlay's pings: a handful of connections from one address)
MAX_CLIENTS = 1000
MAX_CLIENTS_PER_ADDR = 16
MAX_ROOMS = 300
MAX_ROOMS_PER_ADDR = 4
PACKETS_PER_S = 400.0  # sustained, per client; more is dropped (not the connection)
PACKETS_BURST = 800.0
HELLO_WITHIN = 10.0    # s
SILENT_DROP = 15.0     # s, for clients that send heartbeats (protocol 2)
SUMMARY_EVERY = 300.0  # s

SALT = os.urandom(16)  # the logs' client names: a hash of the address with this, new at each start


def anon(addr: str) -> str:
    return hashlib.sha256(SALT + addr.encode()).hexdigest()[:8]


HELLO, CREATE, UPDATE, SCAN, JOIN, NETINFO_MSG, LEAVE, PACKET, REJECT, PING = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10


def ip_str(a: int) -> str:
    return f"{a >> 24}.{a >> 16 & 255}.{a >> 8 & 255}.{a & 255}"


class Room:
    def __init__(self, code: bytes, host: "Client", info: bytes):
        self.code, self.host = code, host
        self.info = bytearray(info)
        self.members: dict[int, Client] = {}  # ip -> client
        self.session = bytes(self.info[0x10:0x20])
        self.lcid = struct.unpack_from("<Q", self.info, 0)[0]
        # traffic, for sizing a server: game packets in from the players and out to them, in bytes as framed here
        # (TCP/IP headers not counted); logged every minute while packets flow and in total when the room closes
        self.started = self.reported = time.monotonic()
        self.pk_in = self.by_in = self.pk_out = self.by_out = 0
        self.last_in = self.last_out = 0
        self.most = 1
        self.in_udp = self.out_udp = 0  # of pk_in / pk_out, by UDP (protocol 3); the rest by TCP

    def traffic(self, total: bool) -> str:
        now = time.monotonic()
        if total:
            secs, b_out, b_in = now - self.started, self.by_out, self.by_in
        else:
            secs, b_out, b_in = now - self.reported, self.by_out - self.last_out, self.by_in - self.last_in
            self.reported, self.last_out, self.last_in = now, self.by_out, self.by_in
        rate = b_out / secs if secs > 0 else 0.0
        what = (f"{secs:.0f} s, up to {self.most} players: {self.pk_in} packets in, {self.pk_out} out, "
                f"{self.by_in / 1e6:.2f} MB in, {self.by_out / 1e6:.2f} MB out" if total else
                f"last {secs:.0f} s: {b_in / 1e3:.1f} KB in, {b_out / 1e3:.1f} KB out")
        return (f"{what}; out {rate / 1e3:.1f} KB/s = {rate * 3600 / 1e6:.0f} MB per hour; UDP: {self.in_udp} of "
                f"{self.pk_in} in, {self.out_udp} of {self.pk_out} out")

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
        self.frames_in = self.bytes_in = self.dropped = 0  # since the last summary
        self.refused = 0
        self.tokens: dict[bytes, Client] = {}   # UDP token -> its client (protocol 3)
        self.udp: asyncio.DatagramTransport | None = None


    def clients_of(self, addr: str) -> int:
        return sum(1 for c in self.clients if c.addr == addr)

    def rooms_of(self, addr: str) -> int:
        return sum(1 for r in self.rooms.values() if r.host.addr == addr)

    def summary(self, secs: float) -> str:
        addrs = len({c.addr for c in self.clients})
        players = sum(len(r.members) for r in self.rooms.values())
        line = (f"status: {len(self.clients)} connections from {addrs} addresses, {len(self.rooms)} rooms, "
                f"{players} players in them; last {secs:.0f} s: {self.frames_in} frames, "
                f"{self.bytes_in / 1e6:.2f} MB in, {self.dropped} packets over the rate dropped, "
                f"{self.refused} connections or rooms refused")
        self.frames_in = self.bytes_in = self.dropped = self.refused = 0
        return line

    def rooms_for(self, code: bytes, lcid: int) -> list[Room]:
        return [r for r in self.rooms.values() if r.code == code and (lcid == 0 or r.lcid == lcid)]


class UdpSide(asyncio.DatagramProtocol):
    """The relay's UDP port (protocol 3): BIND and PACKET datagrams from the games, each with its client's token."""

    def __init__(self, relay: Relay) -> None:
        self.relay = relay

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.relay.udp = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        relay = self.relay
        c = relay.tokens.get(data[:8]) if len(data) >= 9 else None
        if c is None:
            return  # not one of ours: no answer at all
        first = c.udp_addr is None
        c.udp_addr, c.udp_seen = addr, time.monotonic()
        c.last_rx = c.udp_seen
        relay.frames_in += 1
        relay.bytes_in += len(data)
        if first:
            c.log("game packets over UDP")
        kind = data[8]
        if kind == UDP_BIND:
            if relay.udp is not None:
                relay.udp.sendto(bytes([UDP_BIND]), addr)
        elif kind == UDP_PACKET:
            asyncio.ensure_future(c.packet(data[9:], via_udp=True))


class Client:
    def __init__(self, relay: Relay, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.relay, self.reader, self.writer = relay, reader, writer
        self.code = b""
        self.room: Room | None = None
        self.ip = 0
        self.node = bytes(NODE)
        peer = writer.get_extra_info("peername")
        self.addr = peer[0] if peer else "?"  # kept in memory for the limits, never logged
        self.name = f"{anon(self.addr)}:{peer[1]}" if peer else "?"
        self.lock = asyncio.Lock()
        self.last_rx = time.monotonic()
        self.beats = False                    # sends heartbeats (protocol 2): silence drops it
        self.tokens, self.tokens_at = PACKETS_BURST, time.monotonic()
        self.warned = 0.0
        self.token = os.urandom(8)            # its UDP datagrams start with this (protocol 3)
        self.udp_addr: tuple | None = None    # where its datagrams come from, never logged
        self.udp_seen = 0.0

    def udp_live(self) -> bool:
        return self.udp_addr is not None and time.monotonic() - self.udp_seen < UDP_STALE

    async def deliver(self, frame: bytes, room: "Room") -> None:
        """A game packet to this player: by UDP while that works for it, else by TCP."""
        udp = self.relay.udp
        if udp is not None and self.udp_live() and len(frame) - 12 <= UDP_MAX:
            udp.sendto(bytes([UDP_PACKET]) + frame, self.udp_addr)
            room.out_udp += 1
        else:
            await self.send(PACKET, frame)

    async def packet(self, p: bytes, via_udp: bool) -> None:
        """A game packet from this player (u32 dst, u16 dport, u32 src, u16 sport, payload), by TCP or UDP."""
        relay, room = self.relay, self.room
        if room is None or len(p) < 12:
            return
        now = time.monotonic()  # the rate: a token bucket, PACKETS_PER_S refilled, PACKETS_BURST deep
        self.tokens = min(PACKETS_BURST, self.tokens + (now - self.tokens_at) * PACKETS_PER_S)
        self.tokens_at = now
        if self.tokens < 1:
            relay.dropped += 1
            if now - self.warned >= 60:
                self.warned = now
                self.log(f"over {PACKETS_PER_S:.0f} packets a second: the rest dropped")
            return
        self.tokens -= 1
        dst = struct.unpack_from("<I", p)[0]
        src = struct.pack("<I", self.ip)  # the sender's address is the relay's word, not the sender's
        frame = p[:6] + src + p[10:]
        if dst == 0xFFFFFFFF or dst & 0xFF == 0xFF:
            targets = [c for ip, c in room.members.items() if ip != self.ip]
        else:
            c = room.members.get(dst)
            targets = [c] if c and c is not self else []
        room.pk_in += 1
        room.in_udp += via_udp
        room.by_in += len(p) + 5
        room.pk_out += len(targets)
        room.by_out += (len(frame) + 5) * len(targets)
        if now - room.reported >= 60:
            room.host.log(f"room traffic, {len(room.members)} players, {room.traffic(total=False)}")
        for c in targets:
            await c.deliver(frame, room)

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
            if room.pk_in:
                self.log(f"room traffic: {room.traffic(total=True)}")
            if self.relay.rooms.get(room.session) is room:  # not another room with the same session id
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
            self.code = p[:64].rstrip(b"\0").strip()  # case counts; "KEY " and "KEY" are the same room
            self.log(f"hello, room code {self.code.decode(errors='replace')!r}")
            relay.tokens[self.token] = self
            await self.send(HELLO, struct.pack("<I", PROTOCOL) + self.token)  # older games read the version only
        elif kind == PING:
            self.beats = True
            await self.send(PING)
        elif kind == CREATE and len(p) >= NETINFO:
            await self.leave()
            if len(relay.rooms) >= MAX_ROOMS or relay.rooms_of(self.addr) >= MAX_ROOMS_PER_ADDR:
                relay.refused += 1
                self.log("room refused (too many rooms)")
                await self.send(CREATE)  # short: the game keeps the room to itself
                return
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
            room.most = max(room.most, len(room.members))
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
        elif kind == PACKET:
            await self.packet(p, via_udp=False)

    async def run(self) -> None:
        self.log("connected")
        # a game that vanished without closing (killed, its network gone) is noticed in about 25 s, not hours: one
        # such connection held a room for 2 1/2 hours (2026-11-20; owner: 2 minutes too long). Idle: keepalive probes
        # from 10 s, every 5 s, 3 missed; with data waiting for its acknowledgement (where keepalive doesn't run):
        # TCP_USER_TIMEOUT 20 s (Linux). A player whose network stops for longer than that leaves the room.
        sock = self.writer.get_extra_info("socket")
        if sock is not None:
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
                for opt, val in (("TCP_KEEPIDLE", 10), ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3),
                                 ("TCP_USER_TIMEOUT", 20000)):
                    if hasattr(socket, opt):
                        sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), val)
            except OSError:
                pass
        try:
            first = True
            while True:
                if first:  # the hello within HELLO_WITHIN, or the connection is closed (half-open floods)
                    head = await asyncio.wait_for(self.reader.readexactly(4), HELLO_WITHIN)
                else:
                    head = await self.reader.readexactly(4)
                n = struct.unpack("<I", head)[0]
                if not 1 <= n <= MAX_FRAME:
                    break
                body = await self.reader.readexactly(n)
                if first and body[0] != HELLO:
                    break
                first = False
                self.last_rx = time.monotonic()
                self.relay.frames_in += 1
                self.relay.bytes_in += n + 4
                await self.handle(body[0], body[1:])
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError, OSError):
            pass
        finally:
            await self.leave()
            self.relay.clients.discard(self)
            self.relay.tokens.pop(self.token, None)
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
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c = Client(relay, reader, writer)
        if len(relay.clients) >= MAX_CLIENTS or relay.clients_of(c.addr) >= MAX_CLIENTS_PER_ADDR:
            relay.refused += 1
            c.log("refused (too many connections)")
            writer.transport.abort()
            return
        relay.clients.add(c)
        await c.run()

    async def watch() -> None:
        """Heartbeat clients gone silent are dropped; a summary line every SUMMARY_EVERY s."""
        last = time.monotonic()
        while True:
            await asyncio.sleep(5)
            now = time.monotonic()
            for c in list(relay.clients):
                if c.beats and now - c.last_rx > SILENT_DROP:
                    c.log(f"silent for {now - c.last_rx:.0f} s: dropped")
                    c.writer.transport.abort()
            if now - last >= SUMMARY_EVERY:
                print(f"{time.strftime('%H:%M:%S')} {relay.summary(now - last)}", flush=True)
                last = now

    server = await asyncio.start_server(accept, args.host, args.port)
    await asyncio.get_running_loop().create_datagram_endpoint(lambda: UdpSide(relay),
                                                              local_addr=(args.host, args.port))
    print(f"YW4 Recomp relay on port {args.port}, TCP and UDP (protocol {PROTOCOL}). Ctrl+C stops it.", flush=True)
    watcher = asyncio.create_task(watch())
    async with server:
        await server.serve_forever()
    watcher.cancel()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
