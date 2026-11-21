/* The relay for playing together over the internet (decision 0082), in C (owner, 2026-11-20: the Python one spent
 * its CPU on each packet's overhead; the US East server has 1/8 of a CPU). The same protocol and behaviour as
 * relay.py, which stays as the readable description and the tests' reference (python tools/relay/test_relay.py runs
 * either: YW4_RELAY_CMD=path/to/yw4-relay).
 *
 * Every game connects here over TCP; its game packets also come and go by UDP on the same port (protocol 3). Rooms
 * are NetworkInfo blobs (0x480 bytes, Switchbrew "LDN services"), packets are opaque.
 *
 *     yw4-relay [--port 41230] [--host 0.0.0.0]
 *
 * Build (Linux, no libraries):  cc -O2 -o yw4-relay relay.c
 * Static, for the servers:      zig cc -O2 -target x86_64-linux-musl -static -o yw4-relay relay.c  (aarch64-linux-musl
 *                               for an ARM server)
 *
 * Frames both ways over TCP: u32 length (little endian, of what follows), u8 type, payload.
 *     client -> relay                                   relay -> client
 *     1 HELLO   room code (64 bytes, NUL padded)         1 WELCOME  u32 protocol, 8-byte UDP token
 *     2 CREATE  NetworkInfo                              2 CREATED  u32 ip, NetworkInfo (short: refused)
 *     3 UPDATE  NetworkInfo                              6 NETINFO  NetworkInfo (the room changed)
 *     4 SCAN    u64 local communication id (0: any)      4 FOUND    u32 count, count x NetworkInfo (24 at most)
 *     5 JOIN    16-byte session id, NodeInfo (0x40)      5 JOINED   u8 ok, u32 ip, NetworkInfo
 *     7 LEAVE   -                                        7 CLOSED   u8 reason
 *     8 PACKET  u32 dst ip, u16 dst port, u32 src ip,    8 PACKET   the same, the sender filled in by the relay
 *               u16 src port, payload
 *     9 REJECT  u32 ip                                  10 PONG     -
 *    10 PING    -
 * UDP datagrams: from a game, token (8) + kind + payload (1 BIND: empty, answered with a BIND; 8 PACKET: the TCP
 * PACKET's payload); to a game, kind + payload.
 * Room codes: case counts, spaces around them don't. Addresses: u32 in host order, the host 169.254.1.1, the others
 * .2, .3, ... */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PROTOCOL 3
#define NETINFO 0x480
#define LDN 0x50
#define NODES (LDN + 0x18)
#define NODE 0x40
#define BASE_IP 0xA9FE0100u
#define MAX_FRAME (1 << 16)

/* How many games and rooms at once: --max-clients / --max-rooms, per server (the 1-GB US East machine keeps the
 * defaults; Singapore, 4 CPUs and 24 GB since 2026-11-21, runs with more). Each connected game holds its 64 KiB
 * input buffer and an output buffer, so memory, the systemd MemoryMax and the open-file limit go with --max-clients. */
static int MAX_CLIENTS = 1000;
static int max_clients_per_addr = 16;  /* --per-addr-clients: raised for a load test from one PC */
static int MAX_ROOMS = 300;
static int max_rooms_per_addr = 4;     /* --per-addr-rooms */
#define PACKETS_PER_S 400.0
#define PACKETS_BURST 800.0
#define HELLO_WITHIN 10.0
#define SILENT_DROP 15.0
#define SUMMARY_EVERY 300.0
#define UDP_STALE 10.0
#define UDP_MAX 1400
#define OUT_MAX (8u << 20)  /* a client that lets this much pile up unread is dropped */

enum { HELLO = 1, CREATE, UPDATE, SCAN, JOIN, NETINFO_MSG, LEAVE, PACKET, REJECT, PING };
enum { UDP_BIND = 1, UDP_PACKET = 8 };

typedef struct Room Room;
typedef struct {
    int used, fd, closing, got_hello, beats;
    char addr[48];   /* kept for the limits, never logged */
    char name[32];   /* the logs' name: a hash of the address, and the port */
    uint8_t code[65];
    Room *room;
    uint32_t ip;
    uint8_t node[NODE];
    uint8_t token[8];
    struct sockaddr_in uaddr;
    int has_uaddr;
    double udp_seen, last_rx, connected_at, tokens, tokens_at, warned;
    uint8_t *in;     /* MAX_FRAME + 4 */
    size_t in_len;
    uint8_t *out;
    size_t out_len, out_off, out_cap;
    int want_out;
} Client;

struct Room {
    int used, listed;  /* listed: findable (a newer room with the same session id unlists an older one) */
    uint8_t code[65];
    Client *host;
    uint8_t info[NETINFO];
    Client *members[8];  /* by ip: BASE_IP + 1 + i */
    double started, reported;
    uint64_t pk_in, by_in, pk_out, by_out, last_in, last_out, in_udp, out_udp;
    int most;
};

static Client *clients;  /* MAX_CLIENTS of them, made at start */
static Room *rooms;      /* MAX_ROOMS */
static int ep, tcp_fd, udp_fd;
static uint64_t frames_in, bytes_in, dropped, refused;
static uint8_t salt[16];

/* ---- small things */

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void logf_(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const char *who, const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (who) printf("%02d:%02d:%02d %s: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, who, line);
    else printf("%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
    fflush(stdout);
}

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void random_bytes(void *p, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    while (fd >= 0 && got < n) {
        ssize_t r = read(fd, (uint8_t *)p + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (fd >= 0) close(fd);
    if (got < n) {  /* never expected; better than zeros */
        uint64_t x = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)p;
        for (size_t i = got; i < n; i++) x = x * 6364136223846793005ull + 1442695040888963407ull, ((uint8_t *)p)[i] = (uint8_t)(x >> 56);
    }
}

/* SipHash-2-4: the logs' client names, keyed with `salt` (new at each start), so a name can't be turned back into an
 * address or followed across restarts */
static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
static uint64_t siphash(const uint8_t *k, const uint8_t *m, size_t n) {
    uint64_t k0, k1;
    memcpy(&k0, k, 8), memcpy(&k1, k + 8, 8);
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
#define ROUND v0 += v1, v1 = rotl(v1, 13), v1 ^= v0, v0 = rotl(v0, 32), v2 += v3, v3 = rotl(v3, 16), v3 ^= v2, \
              v0 += v3, v3 = rotl(v3, 21), v3 ^= v0, v2 += v1, v1 = rotl(v1, 17), v1 ^= v2, v2 = rotl(v2, 32)
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, m + i, 8);
        v3 ^= w; ROUND; ROUND; v0 ^= w;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t j = 0; i + j < n; j++) b |= (uint64_t)m[i + j] << (8 * j);
    v3 ^= b; ROUND; ROUND; v0 ^= b;
    v2 ^= 0xff;
    ROUND; ROUND; ROUND; ROUND;
#undef ROUND
    return v0 ^ v1 ^ v2 ^ v3;
}

static void ip_str(uint32_t a, char *out, size_t n) {
    snprintf(out, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

/* ---- sending: whole frames into the client's out buffer, written as the socket takes them */

static void set_want_out(Client *c, int want) {
    if (c->want_out == want || c->closing) return;
    struct epoll_event e = {.events = EPOLLIN | (want ? EPOLLOUT : 0), .data.u64 = (uint64_t)(c - clients) + 1};
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &e);
    c->want_out = want;
}

static void flush_out(Client *c) {
    while (c->out_off < c->out_len) {
        ssize_t r = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (r > 0) { c->out_off += (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { set_want_out(c, 1); return; }
        if (r < 0 && errno == EINTR) continue;
        c->closing = 1;
        return;
    }
    c->out_off = c->out_len = 0;
    set_want_out(c, 0);
}

static void send_frame2(Client *c, int kind, const void *a, size_t an, const void *b, size_t bn) {
    if (c->closing) return;
    size_t need = c->out_len + 5 + an + bn;
    if (need > OUT_MAX) {  /* it reads nothing: let it go rather than hold its traffic */
        logf_(c->name, "too slow to read its traffic: dropped");
        c->closing = 1;
        return;
    }
    if (c->out_off && c->out_off == c->out_len) c->out_off = c->out_len = 0, need = 5 + an + bn;
    if (need > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < need) cap *= 2;
        uint8_t *p = realloc(c->out, cap);
        if (!p) { c->closing = 1; return; }
        c->out = p, c->out_cap = cap;
    }
    uint8_t *w = c->out + c->out_len;
    put32(w, (uint32_t)(an + bn + 1));
    w[4] = (uint8_t)kind;
    if (an) memcpy(w + 5, a, an);
    if (bn) memcpy(w + 5 + an, b, bn);
    int was_empty = c->out_off == c->out_len;
    c->out_len += 5 + an + bn;
    if (was_empty) flush_out(c);
}
static void send_frame(Client *c, int kind, const void *p, size_t n) { send_frame2(c, kind, p, n, NULL, 0); }

/* ---- rooms */

static int node_count(const Room *r) {
    int n = 0;
    for (int i = 0; i < 8; i++) n += r->info[NODES + NODE * i + 0xB] != 0;
    return n;
}

static void rebuild_nodes(Room *r) {
    for (int i = 0; i < 8; i++) {
        uint8_t *off = r->info + NODES + NODE * i;
        Client *c = r->members[i];
        if (!c) { memset(off, 0, NODE); continue; }
        uint8_t node[NODE];
        memcpy(node, c->node, NODE);
        put32(node, BASE_IP + 1 + (uint32_t)i);
        const uint8_t mac[6] = {0x02, 0x00, 0x59, 0x57, 0x34, (uint8_t)(i + 1)};
        memcpy(node + 4, mac, 6);
        node[0xA] = (uint8_t)i;
        node[0xB] = 1;
        memcpy(off, node, NODE);
    }
    r->info[LDN + 0x17] = (uint8_t)node_count(r);
}

static uint32_t free_ip(const Room *r) {
    int maxn = r->info[LDN + 0x16] ? r->info[LDN + 0x16] : 8;
    if (maxn > 8) maxn = 8;
    for (int i = 0; i < maxn; i++)
        if (!r->members[i]) return BASE_IP + 1 + (uint32_t)i;
    return 0;
}

static void broadcast_info(Room *r, Client *skip) {
    for (int i = 0; i < 8; i++)
        if (r->members[i] && r->members[i] != skip) send_frame(r->members[i], NETINFO_MSG, r->info, NETINFO);
}

static uint64_t room_lcid(const Room *r) { uint64_t v; memcpy(&v, r->info, 8); return v; }

static void traffic(Room *r, int total, char *out, size_t n) {
    double now = now_s(), secs;
    uint64_t b_out, b_in;
    if (total) secs = now - r->started, b_out = r->by_out, b_in = r->by_in;
    else {
        secs = now - r->reported, b_out = r->by_out - r->last_out, b_in = r->by_in - r->last_in;
        r->reported = now, r->last_out = r->by_out, r->last_in = r->by_in;
    }
    double rate = secs > 0 ? (double)b_out / secs : 0;
    char what[256];
    if (total)
        snprintf(what, sizeof what, "%.0f s, up to %d players: %llu packets in, %llu out, %.2f MB in, %.2f MB out", secs,
                 r->most, (unsigned long long)r->pk_in, (unsigned long long)r->pk_out, (double)r->by_in / 1e6,
                 (double)r->by_out / 1e6);
    else
        snprintf(what, sizeof what, "last %.0f s: %.1f KB in, %.1f KB out", secs, (double)b_in / 1e3, (double)b_out / 1e3);
    snprintf(out, n, "%s; out %.1f KB/s = %.0f MB per hour; UDP: %llu of %llu in, %llu of %llu out", what, rate / 1e3,
             rate * 3600 / 1e6, (unsigned long long)r->in_udp, (unsigned long long)r->pk_in,
             (unsigned long long)r->out_udp, (unsigned long long)r->pk_out);
}

static int rooms_of(const char *addr) {
    int n = 0;
    for (int i = 0; i < MAX_ROOMS; i++) n += rooms[i].used && !strcmp(rooms[i].host->addr, addr);
    return n;
}

static int room_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_ROOMS; i++) n += rooms[i].used;
    return n;
}

static Room *room_by_session(const uint8_t *session) {
    for (int i = 0; i < MAX_ROOMS; i++)
        if (rooms[i].used && rooms[i].listed && !memcmp(rooms[i].info + 0x10, session, 16)) return &rooms[i];
    return NULL;
}

/* ---- clients */

static void leave(Client *c) {
    Room *r = c->room;
    c->room = NULL;
    if (!r) return;
    for (int i = 0; i < 8; i++)
        if (r->members[i] == c) r->members[i] = NULL;
    if (r->host == c) {
        int others = 0;
        for (int i = 0; i < 8; i++) others += r->members[i] != NULL;
        logf_(c->name, "closes the room (%d others in it)", others);
        if (r->pk_in) {
            char t[512];
            traffic(r, 1, t, sizeof t);
            logf_(c->name, "room traffic: %s", t);
        }
        for (int i = 0; i < 8; i++) {
            Client *m = r->members[i];
            if (!m) continue;
            m->room = NULL;
            const uint8_t reason = 3;  /* DestroyedByUser */
            send_frame(m, LEAVE, &reason, 1);
        }
        memset(r, 0, sizeof *r);
    } else {
        char ip[20];
        ip_str(c->ip, ip, sizeof ip);
        logf_(c->name, "leaves the room (%s)", ip);
        rebuild_nodes(r);
        broadcast_info(r, NULL);
    }
    c->ip = 0;
}

static int udp_live(const Client *c, double now) { return c->has_uaddr && now - c->udp_seen < UDP_STALE; }

static void deliver(Client *to, const uint8_t *frame, size_t n, Room *r, double now) {
    if (udp_live(to, now) && n - 12 <= UDP_MAX) {
        uint8_t d[1 + 12 + UDP_MAX];
        d[0] = UDP_PACKET;
        memcpy(d + 1, frame, n);
        sendto(udp_fd, d, n + 1, MSG_DONTWAIT, (struct sockaddr *)&to->uaddr, sizeof to->uaddr);  /* full: lost, as UDP */
        r->out_udp++;
    } else {
        send_frame(to, PACKET, frame, n);
    }
}

/* a game packet from c (u32 dst, u16 dport, u32 src, u16 sport, payload), by TCP or UDP */
static void packet(Client *c, const uint8_t *p, size_t n, int via_udp) {
    Room *r = c->room;
    if (!r || n < 12 || n > MAX_FRAME) return;
    double now = now_s();
    c->tokens += (now - c->tokens_at) * PACKETS_PER_S;
    if (c->tokens > PACKETS_BURST) c->tokens = PACKETS_BURST;
    c->tokens_at = now;
    if (c->tokens < 1) {
        dropped++;
        if (now - c->warned >= 60) {
            c->warned = now;
            logf_(c->name, "over %.0f packets a second: the rest dropped", PACKETS_PER_S);
        }
        return;
    }
    c->tokens -= 1;
    uint8_t frame[MAX_FRAME];
    memcpy(frame, p, n);
    put32(frame + 6, c->ip);  /* the sender's address is the relay's word, not the sender's */
    uint32_t dst = get32(p);
    Client *targets[8];
    int nt = 0;
    if (dst == 0xFFFFFFFFu || (dst & 0xFF) == 0xFF) {
        for (int i = 0; i < 8; i++)
            if (r->members[i] && r->members[i] != c) targets[nt++] = r->members[i];
    } else if (dst - BASE_IP - 1 < 8) {
        Client *t = r->members[dst - BASE_IP - 1];
        if (t && t != c) targets[nt++] = t;
    }
    r->pk_in++;
    r->in_udp += via_udp != 0;
    r->by_in += n + 5;
    r->pk_out += (uint64_t)nt;
    r->by_out += (uint64_t)(n + 5) * (uint64_t)nt;
    if (now - r->reported >= 60) {
        int players = 0;
        for (int i = 0; i < 8; i++) players += r->members[i] != NULL;
        char t[512];
        traffic(r, 0, t, sizeof t);
        logf_(r->host->name, "room traffic, %d players, %s", players, t);
    }
    for (int i = 0; i < nt; i++) deliver(targets[i], frame, n, r, now);
}

static void handle(Client *c, int kind, const uint8_t *p, size_t n) {
    if (kind == HELLO) {
        size_t len = n < 64 ? n : 64;
        while (len && p[len - 1] == 0) len--;
        size_t s = 0;
        while (s < len && (p[s] == ' ' || (p[s] >= '\t' && p[s] <= '\r'))) s++;
        while (len > s && (p[len - 1] == ' ' || (p[len - 1] >= '\t' && p[len - 1] <= '\r'))) len--;
        memset(c->code, 0, sizeof c->code);
        memcpy(c->code, p + s, len - s);
        char shown[80];
        size_t k = 0;
        for (size_t i = 0; i < len - s && k < sizeof shown - 1; i++)
            shown[k++] = c->code[i] >= 0x20 && c->code[i] < 0x7F && c->code[i] != '\'' ? (char)c->code[i] : '?';
        shown[k] = 0;
        logf_(c->name, "hello, room code '%s'", shown);
        uint8_t w[12];
        put32(w, PROTOCOL);
        memcpy(w + 4, c->token, 8);
        send_frame(c, HELLO, w, sizeof w);  /* older games read the version only */
    } else if (kind == PING) {
        c->beats = 1;
        send_frame(c, PING, NULL, 0);
    } else if (kind == CREATE && n >= NETINFO) {
        leave(c);
        Room *r = NULL;
        if (room_count() < MAX_ROOMS && rooms_of(c->addr) < max_rooms_per_addr)
            for (int i = 0; i < MAX_ROOMS && !r; i++)
                if (!rooms[i].used) r = &rooms[i];
        if (!r) {
            refused++;
            logf_(c->name, "room refused (too many rooms)");
            send_frame(c, CREATE, NULL, 0);  /* short: the game keeps the room to itself */
            return;
        }
        for (int i = 0; i < MAX_ROOMS; i++)  /* an older room with this session id is no longer found */
            if (rooms[i].used && !memcmp(rooms[i].info + 0x10, p + 0x10, 16)) rooms[i].listed = 0;
        memset(r, 0, sizeof *r);
        r->used = r->listed = 1;
        memcpy(r->code, c->code, sizeof r->code);
        r->host = c;
        memcpy(r->info, p, NETINFO);
        r->started = r->reported = now_s();
        r->most = 1;
        c->room = r, c->ip = BASE_IP + 1;
        memcpy(c->node, p + NODES, NODE);  /* the host's own node, as its game made it */
        r->members[0] = c;
        rebuild_nodes(r);
        logf_(c->name, "creates a room (game %016llx, up to %d players)", (unsigned long long)room_lcid(r),
              r->info[LDN + 0x16]);
        uint8_t ip[4];
        put32(ip, c->ip);
        send_frame2(c, CREATE, ip, 4, r->info, NETINFO);
    } else if (kind == UPDATE && n >= NETINFO && c->room && c->room->host == c) {
        Room *r = c->room;
        uint8_t nodes[8 * NODE];
        memcpy(nodes, r->info + NODES, sizeof nodes);
        memcpy(r->info, p, NETINFO);
        memcpy(r->info + NODES, nodes, sizeof nodes);  /* the relay keeps the node table */
        r->info[LDN + 0x17] = (uint8_t)node_count(r);
        broadcast_info(r, c);  /* the host knows its own change */
    } else if (kind == SCAN && n >= 8) {
        uint64_t lcid;
        memcpy(&lcid, p, 8);
        static uint8_t found[4 + 24 * NETINFO];
        uint32_t count = 0;
        for (int i = 0; i < MAX_ROOMS && count < 24; i++) {
            Room *r = &rooms[i];
            if (!r->used || !r->listed || r == c->room || strcmp((char *)r->code, (char *)c->code)) continue;
            if (lcid && room_lcid(r) != lcid) continue;
            memcpy(found + 4 + count * NETINFO, r->info, NETINFO);
            count++;
        }
        put32(found, count);
        send_frame(c, SCAN, found, 4 + count * NETINFO);
    } else if (kind == JOIN && n >= 16 + NODE) {
        Room *r = room_by_session(p);
        uint32_t ip = r && !strcmp((char *)r->code, (char *)c->code) ? free_ip(r) : 0;
        if (!r || !ip || r->info[LDN + 0x12] == 1) {  /* gone, full, or AlwaysReject */
            logf_(c->name, "join refused");
            static uint8_t no[5 + NETINFO];
            send_frame(c, JOIN, no, sizeof no);
            return;
        }
        leave(c);
        c->room = r, c->ip = ip;
        memcpy(c->node, p + 16, NODE);
        r->members[ip - BASE_IP - 1] = c;
        int members = 0;
        for (int i = 0; i < 8; i++) members += r->members[i] != NULL;
        if (members > r->most) r->most = members;
        rebuild_nodes(r);
        char ips[20];
        ip_str(ip, ips, sizeof ips);
        logf_(c->name, "joins %s's room as %s", r->host->name, ips);
        uint8_t head[5];
        head[0] = 1;
        put32(head + 1, ip);
        send_frame2(c, JOIN, head, 5, r->info, NETINFO);
        broadcast_info(r, c);
    } else if (kind == LEAVE) {
        leave(c);
    } else if (kind == REJECT && n >= 4 && c->room && c->room->host == c) {
        uint32_t ip = get32(p);
        Client *t = ip - BASE_IP - 1 < 8 ? c->room->members[ip - BASE_IP - 1] : NULL;
        if (t && t != c) {
            leave(t);
            const uint8_t reason = 5;  /* Rejected */
            send_frame(t, LEAVE, &reason, 1);
        }
    } else if (kind == PACKET) {
        packet(c, p, n, 0);
    }
}

static void close_client(Client *c) {
    if (!c->used) return;
    leave(c);
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    logf_(c->name, "gone");
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
}

static void read_client(Client *c) {
    for (;;) {
        ssize_t r = recv(c->fd, c->in + c->in_len, MAX_FRAME + 4 - c->in_len, 0);
        if (r == 0) { c->closing = 1; return; }
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) c->closing = 1;
            return;
        }
        c->in_len += (size_t)r;
        size_t at = 0;
        while (c->in_len - at >= 4 && !c->closing) {
            uint32_t n = get32(c->in + at);
            if (n < 1 || n > MAX_FRAME) { c->closing = 1; break; }
            if (c->in_len - at - 4 < n) break;
            const uint8_t *body = c->in + at + 4;
            if (!c->got_hello && body[0] != HELLO) { c->closing = 1; break; }  /* the hello first */
            c->got_hello = 1;
            c->last_rx = now_s();
            frames_in++;
            bytes_in += n + 4;
            handle(c, body[0], body + 1, n - 1);
            at += 4 + n;
        }
        if (at) memmove(c->in, c->in + at, c->in_len - at), c->in_len -= at;
        if (c->closing) return;
    }
}

static void accept_clients(void) {
    for (;;) {
        struct sockaddr_in a;
        socklen_t al = sizeof a;
        int fd = accept4(tcp_fd, (struct sockaddr *)&a, &al, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        char addr[48];
        inet_ntop(AF_INET, &a.sin_addr, addr, sizeof addr);
        char name[32];
        snprintf(name, sizeof name, "%08x:%u", (uint32_t)(siphash(salt, (const uint8_t *)addr, strlen(addr)) >> 32),
                 ntohs(a.sin_port));
        int slot = -1, same = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].used) { if (slot < 0) slot = i; continue; }
            same += !strcmp(clients[i].addr, addr);
        }
        if (slot < 0 || same >= max_clients_per_addr) {
            refused++;
            logf_(name, "refused (too many connections)");
            close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        /* a game that vanished without closing is noticed in about 25 s: keepalive from 10 s, every 5 s, 3 missed;
         * with data waiting for its acknowledgement, TCP_USER_TIMEOUT 20 s */
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
        int idle = 10, intvl = 5, cnt = 3;
        unsigned ut = 20000;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ut, sizeof ut);
        Client *c = &clients[slot];
        memset(c, 0, sizeof *c);
        c->in = malloc(MAX_FRAME + 4);
        if (!c->in) { close(fd); continue; }
        c->used = 1, c->fd = fd;
        snprintf(c->addr, sizeof c->addr, "%s", addr);
        snprintf(c->name, sizeof c->name, "%s", name);
        c->connected_at = c->last_rx = c->tokens_at = now_s();
        c->tokens = PACKETS_BURST;
        random_bytes(c->token + 2, 6);  /* the slot in its first two bytes: found at once from a datagram */
        c->token[0] = (uint8_t)slot, c->token[1] = (uint8_t)(slot >> 8);
        struct epoll_event e = {.events = EPOLLIN, .data.u64 = (uint64_t)slot + 1};
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
        logf_(c->name, "connected");
    }
}

static void read_udp(void) {
    uint8_t d[MAX_FRAME];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(udp_fd, d, sizeof d, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n < 0) return;
        if (n < 9) continue;
        unsigned slot = d[0] | (unsigned)d[1] << 8;
        if (slot >= MAX_CLIENTS) continue;
        Client *c = &clients[slot];
        if (!c->used || c->closing || !c->got_hello || memcmp(c->token, d, 8)) continue;  /* not one of ours: silence */
        int first = !c->has_uaddr;
        c->uaddr = from, c->has_uaddr = 1;
        c->udp_seen = c->last_rx = now_s();
        frames_in++;
        bytes_in += (uint64_t)n;
        if (first) logf_(c->name, "game packets over UDP");
        if (d[8] == UDP_BIND) {
            const uint8_t b = UDP_BIND;
            sendto(udp_fd, &b, 1, MSG_DONTWAIT, (struct sockaddr *)&from, fl);
        } else if (d[8] == UDP_PACKET) {
            packet(c, d + 9, (size_t)n - 9, 1);
        }
    }
}

static void summary(double secs) {
    int conns = 0, addrs = 0, nrooms = 0, players = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].used) continue;
        conns++;
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) seen = clients[j].used && !strcmp(clients[j].addr, clients[i].addr);
        addrs += !seen;
    }
    for (int i = 0; i < MAX_ROOMS; i++)
        if (rooms[i].used) {
            nrooms++;
            for (int k = 0; k < 8; k++) players += rooms[i].members[k] != NULL;
        }
    char line[512];
    snprintf(line, sizeof line,
             "status: %d connections from %d addresses, %d rooms, %d players in them; last %.0f s: %llu frames, "
             "%.2f MB in, %llu packets over the rate dropped, %llu connections or rooms refused",
             conns, addrs, nrooms, players, secs, (unsigned long long)frames_in, (double)bytes_in / 1e6,
             (unsigned long long)dropped, (unsigned long long)refused);
    logf_(NULL, "%s", line);
    frames_in = bytes_in = dropped = refused = 0;
}

static int listen_on(const char *host, int port, int type) {
    int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || bind(fd, (struct sockaddr *)&a, sizeof a) ||
        (type == SOCK_STREAM && listen(fd, 128))) {
        close(fd);
        return -1;
    }
    if (type == SOCK_DGRAM) {  /* bursts of a mission: room for them while the loop is busy */
        int big = 4 << 20;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    }
    return fd;
}

int main(int argc, char **argv) {
    int port = 41230;
    const char *host = "0.0.0.0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--per-addr-clients") && i + 1 < argc) max_clients_per_addr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--per-addr-rooms") && i + 1 < argc) max_rooms_per_addr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-clients") && i + 1 < argc) MAX_CLIENTS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-rooms") && i + 1 < argc) MAX_ROOMS = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--port 41230] [--host 0.0.0.0] [--per-addr-clients 16] [--per-addr-rooms 4] "
                            "[--max-clients 1000] [--max-rooms 300]\n", argv[0]);
            return 2;
        }
    }
    if (MAX_CLIENTS < 1 || MAX_ROOMS < 1) {
        fprintf(stderr, "--max-clients and --max-rooms must be at least 1\n");
        return 2;
    }
    clients = calloc((size_t)MAX_CLIENTS, sizeof *clients);
    rooms = calloc((size_t)MAX_ROOMS, sizeof *rooms);
    if (!clients || !rooms) {
        fprintf(stderr, "no memory for %d clients and %d rooms\n", MAX_CLIENTS, MAX_ROOMS);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    random_bytes(salt, sizeof salt);
    tcp_fd = listen_on(host, port, SOCK_STREAM);
    udp_fd = listen_on(host, port, SOCK_DGRAM);
    ep = epoll_create1(EPOLL_CLOEXEC);
    if (tcp_fd < 0 || udp_fd < 0 || ep < 0) {
        fprintf(stderr, "can't listen on %s:%d: %s\n", host, port, strerror(errno));
        return 1;
    }
    struct epoll_event e = {.events = EPOLLIN, .data.u64 = 0};
    epoll_ctl(ep, EPOLL_CTL_ADD, tcp_fd, &e);
    e.data.u64 = UINT64_MAX;
    epoll_ctl(ep, EPOLL_CTL_ADD, udp_fd, &e);
    printf("YW4 Recomp relay on port %d, TCP and UDP (protocol %d, in C), up to %d games and %d rooms. Ctrl+C stops "
           "it.\n", port, PROTOCOL, MAX_CLIENTS, MAX_ROOMS);
    fflush(stdout);
    double last_watch = now_s(), last_summary = last_watch;
    struct epoll_event ev[256];
    for (;;) {
        int n = epoll_wait(ep, ev, 256, 1000);
        for (int i = 0; i < n; i++) {
            uint64_t id = ev[i].data.u64;
            if (id == 0) { accept_clients(); continue; }
            if (id == UINT64_MAX) { read_udp(); continue; }
            Client *c = &clients[id - 1];
            if (!c->used) continue;
            if (ev[i].events & (EPOLLERR | EPOLLHUP)) c->closing = 1;
            if (!c->closing && (ev[i].events & EPOLLIN)) read_client(c);
            if (!c->closing && (ev[i].events & EPOLLOUT)) flush_out(c);
        }
        double now = now_s();
        if (now - last_watch >= 1.0) {  /* the hello within 10 s; heartbeat clients silent for 15 s are gone */
            last_watch = now;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                Client *c = &clients[i];
                if (!c->used || c->closing) continue;
                if (!c->got_hello && now - c->connected_at > HELLO_WITHIN) c->closing = 1;
                else if (c->beats && now - c->last_rx > SILENT_DROP) {
                    logf_(c->name, "silent for %.0f s: dropped", now - c->last_rx);
                    c->closing = 1;
                }
            }
        }
        if (now - last_summary >= SUMMARY_EVERY) summary(now - last_summary), last_summary = now;
        for (int i = 0; i < MAX_CLIENTS; i++)  /* closed after the work, so nobody frees a client still in use */
            if (clients[i].used && clients[i].closing) close_client(&clients[i]);
    }
}
