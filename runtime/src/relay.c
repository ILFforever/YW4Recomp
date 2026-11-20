/* The connection to the relay (relay.h, tools/relay/relay.py, decision 0082).
 *
 * Frames both ways: u32 length (little endian, of what follows), u8 type, payload. One request waits for its
 * answer at a time (ldn's calls are few and the game makes them one after another); pushed frames (room changes,
 * packets) go to ldn.c and socket.c from the reader thread. If the connection drops, the room is lost for us
 * (SignalLost) and the next request connects again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET RSock;
#define RBAD INVALID_SOCKET
#define rclose closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int RSock;
#define RBAD (-1)
#define rclose close
#endif

#include "host.h"
#include "relay.h"

#define MAX_FRAME (1u << 16)

static HostLock lock = HOST_LOCK_INIT;      /* the connection and the answer slot */
static HostLock send_lock = HOST_LOCK_INIT; /* whole frames out */
static HostCond answered = HOST_COND_INIT;
static RSock sock = RBAD;
static unsigned generation;                 /* +1 per connection: the reader of an old one stops */
static int server_protocol;                 /* the relay's, from its hello's answer (0 until it comes) */
static int64_t last_rx;                     /* when a frame last came from the relay (host_now_ns) */

/* Game packets over UDP (protocol 3, 2026-11-20; tools/relay/relay.py says the format): over TCP one lost packet holds
 * back every packet behind it until it is sent again (stutters on long routes, owner's US East drop). The relay's
 * welcome brings a token; the game's datagrams start with it. BIND (with the heartbeat, every 3 s) tells the relay
 * where the game is through its home router and keeps that open; the relay's answer says UDP gets through, and only
 * then do packets go that way. No UDP from the relay for 10 s: TCP again. Control (rooms, joining) stays on TCP. */
enum { UDP_BIND = 1, UDP_PACKET = 8, UDP_MAX = 1400 };  /* 1400 + 21 + 28 = 1449 bytes: under 1492, no fragments */
static RSock usock = RBAD;                  /* the UDP side, connected to the relay (lock) */
static uint8_t udp_token[8];
static int udp_ok;                          /* the relay answered over UDP lately: packets go that way */
static int64_t udp_rx;                      /* when a datagram last came */
static struct sockaddr_storage peer;        /* the relay's address, from the TCP connection (lock) */
static int peer_len;
static uint8_t want;                        /* the answer type a request waits for, 0 for none */
static uint8_t *answer;
static size_t answer_cap;
static long answer_len;

/* Which server (2026-11-19, owner: "the game's Online goes to our server, local play stays local"). The trade and
 * Blasters menus' Online / Local choice is caught where the game stores it (yw4_hook_net_mode, below) and the game
 * is told Local, so both run the local-play flow (ldn) and only the server differs:
 *   Online: YW4_ONLINE_SERVER (the overlay's Online play > Server; default: our US East server); the room code by
 *           activity: Ark Swap / Blasters public (empty: the public lobby) or private (YW4_SWAP_CODE /
 *           YW4_BLASTERS_CODE when YW4_SWAP_PRIVATE / YW4_BLASTERS_PRIVATE is 1)
 *   Local:  YW4_RELAY (a relay on the home network, or none: a Switch alone in its room), room code YW4_ROOM */
#define DEFAULT_ONLINE_SERVER "yw4-us.duckdns.org:41230" /* Oracle Cloud, US East (memory: online-play-2026-11-19) */
static int online = -1;                               /* the menus' last choice: 1 Online, 0 Local, -1 none yet */
static int activity;                                  /* RELAY_SWAP or RELAY_BLASTERS: whose room settings */

/* Our servers by name (DuckDNS, 2026-11-20, owner: so a server can move to another machine without a game update),
 * each with the address it had when this build was made: a name that can't be looked up (the DNS service down, no
 * DNS at all) is tried at that address. Settings saved before the names hold the address: it means the name. */
static const struct { const char *host, *ip; } known[] = {
    {"yw4-us.duckdns.org", "129.153.179.140"},  /* US East (Ashburn) */
    {"yw4-sg.duckdns.org", "161.118.234.155"},  /* Singapore */
};

/* host:port's address for a socket of this type: the name looked up, else its built-in address (0, or -1) */
static int lookup(const char *host, const char *port, int socktype, struct addrinfo **res) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC, hints.ai_socktype = socktype;
    *res = NULL;
    if (!getaddrinfo(host, port, &hints, res) && *res) return 0;
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(host, known[i].host) && !getaddrinfo(known[i].ip, port, &hints, res) && *res) {
            fprintf(stderr, "yw4: relay: %s can't be looked up: its built-in address %s\n", host, known[i].ip);
            return 0;
        }
    return -1;
}

static const char *target(void) {
    const char *e;
    if (online == 1) {
        e = getenv("YW4_ONLINE_SERVER");
        if (!e || !*e) return DEFAULT_ONLINE_SERVER;
        static char named[96];  /* an address saved before the names: its name */
        for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
            size_t n = strlen(known[i].ip);
            if (!strncmp(e, known[i].ip, n) && (e[n] == ':' || !e[n])) {
                snprintf(named, sizeof named, "%s%s", known[i].host, e + n);
                return named;
            }
        }
        return e;
    }
    return (e = getenv("YW4_RELAY")) && *e ? e : NULL;
}

static const char *room_code(void) {
    const char *e;
    if (online != 1) return (e = getenv("YW4_ROOM")) ? e : "";
    e = getenv(activity == RELAY_BLASTERS ? "YW4_BLASTERS_PRIVATE" : "YW4_SWAP_PRIVATE");
    if (!e || strcmp(e, "1")) return "";  /* public: the lobby everyone on the server shares */
    e = getenv(activity == RELAY_BLASTERS ? "YW4_BLASTERS_CODE" : "YW4_SWAP_CODE");
    return e ? e : "";  /* private without a code yet: the public lobby too (the overlay says "Not set") */
}

int relay_enabled(void) { return target() != NULL; }

void relay_choose(int on, int what) {
    host_lock(&lock);
    int changed = on != online || (on && what != activity);
    online = on, activity = what;
    if (changed && sock != RBAD) {  /* open to the other server: the next request connects to this one */
        generation++;               /* its reader ends without reporting a lost room */
        rclose(sock);
        sock = RBAD;
        host_cond_broadcast(&answered);
    }
    host_unlock(&lock);
    const char *t = target();
    fprintf(stderr, "yw4: relay: the menu chose %s: %s, room code \"%s\"\n", on ? "Online" : "Local",
            t ? t : "no relay (alone in the room; set YW4_RELAY for a home-network one)", room_code());
}

void relay_settings_changed(void) {
    if (ldn_in_room()) return;
    host_lock(&lock);
    int was = sock != RBAD;
    if (was) {  /* as relay_choose: its reader ends without reporting a lost room */
        generation++;
        rclose(sock);
        sock = RBAD;
        host_cond_broadcast(&answered);
    }
    host_unlock(&lock);
    if (was) {
        const char *t = target();
        fprintf(stderr, "yw4: relay: settings changed, the next request connects to %s, room code \"%s\"\n",
                t ? t : "no relay", room_code());
    }
}

static int read_all(RSock s, uint8_t *p, size_t n) {
    while (n) {
        int r = recv(s, (char *)p, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
        if (r <= 0) return -1;
        p += r, n -= (size_t)r;
    }
    return 0;
}

/* one frame from two pieces (a header and a payload), sent whole under send_lock */
static int write_frame2(RSock s, uint8_t type, const void *a, size_t an, const void *b, size_t bn) {
    uint8_t head[5];
    uint32_t len = (uint32_t)(an + bn) + 1;
    memcpy(head, &len, 4);
    head[4] = type;
    const struct { const void *p; size_t n; } parts[3] = {{head, 5}, {a, an}, {b, bn}};
    host_lock(&send_lock);
    int ok = 1;
    for (int i = 0; ok && i < 3; i++)
        for (size_t done = 0; ok && done < parts[i].n;) {
            int r = send(s, (const char *)parts[i].p + done, (int)(parts[i].n - done), 0);
            if (r <= 0) ok = 0; else done += (size_t)r;
        }
    host_unlock(&send_lock);
    return ok ? 0 : -1;
}
static int write_frame(RSock s, uint8_t type, const void *p, size_t n) { return write_frame2(s, type, p, n, NULL, 0); }

typedef struct { RSock s; unsigned gen; } Reader;

/* BIND to the relay over UDP (lock held) */
static void udp_bind_locked(void) {
    if (usock == RBAD) return;
    uint8_t d[9];
    memcpy(d, udp_token, 8);
    d[8] = UDP_BIND;
    send(usock, (const char *)d, sizeof d, 0);
}

/* The UDP side's reader: BIND answers (UDP works) and packets from the other players. Ends with its connection. */
static void udp_reader(void *arg) {
    Reader r = *(Reader *)arg;
    free(arg);
    uint8_t *buf = malloc(2048);
    for (;;) {
        int n = buf ? recv(r.s, (char *)buf, 2048, 0) : -1;  /* 1 s at most (SO_RCVTIMEO) */
        host_lock(&lock);
        int current = r.gen == generation && usock == r.s && sock != RBAD;
        host_unlock(&lock);
        if (!current || !buf) break;
        if (n <= 0) {  /* the timeout, or the network said the port is closed: wait, the heartbeat binds again */
            host_sleep_ns(50000000);
            continue;
        }
        __atomic_store_n(&udp_rx, host_now_ns(), __ATOMIC_RELEASE);
        __atomic_store_n(&last_rx, host_now_ns(), __ATOMIC_RELEASE);
        if (buf[0] == UDP_BIND) {
            if (!__atomic_exchange_n(&udp_ok, 1, __ATOMIC_ACQ_REL))
                fprintf(stderr, "yw4: relay: game packets over UDP\n");
        } else if (buf[0] == UDP_PACKET && n >= 13) {
            uint32_t dst, src;
            uint16_t dport, sport;
            const uint8_t *p = buf + 1;
            memcpy(&dst, p, 4), memcpy(&dport, p + 4, 2), memcpy(&src, p + 6, 4), memcpy(&sport, p + 10, 2);
            socket_relay_packet(dst, dport, src, sport, p + 12, (size_t)n - 13);
        }
    }
    free(buf);
    host_lock(&lock);
    if (usock == r.s) usock = RBAD, __atomic_store_n(&udp_ok, 0, __ATOMIC_RELEASE);
    host_unlock(&lock);
    rclose(r.s);
}

/* The welcome's token came (protocol 3): the UDP side, to the relay's address, and its first BIND. If UDP can't get
 * through, nothing answers and everything stays on TCP. */
static void udp_start(unsigned gen, const uint8_t *token) {
    host_lock(&lock);
    if (gen != generation || usock != RBAD || !peer_len) {
        host_unlock(&lock);
        return;
    }
    RSock u = socket(peer.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (u == RBAD || connect(u, (struct sockaddr *)&peer, peer_len)) {
        if (u != RBAD) rclose(u);
        host_unlock(&lock);
        return;
    }
#ifdef _WIN32
    DWORD to = 1000;
    BOOL no = FALSE;  /* a "port unreachable" reply must not end the socket on Windows */
    DWORD got = 0;
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    WSAIoctl(u, SIO_UDP_CONNRESET, &no, sizeof no, NULL, 0, &got, NULL, NULL);
#else
    struct timeval to = {1, 0};
#endif
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    Reader *a = malloc(sizeof *a);
    if (!a) {
        rclose(u);
        host_unlock(&lock);
        return;
    }
    memcpy(udp_token, token, 8);
    usock = u;
    __atomic_store_n(&udp_ok, 0, __ATOMIC_RELEASE);
    a->s = u, a->gen = gen;
    host_thread_start(udp_reader, a, 1 << 16);
    udp_bind_locked();
    host_unlock(&lock);
}

static void reader(void *arg) {
    Reader r = *(Reader *)arg;
    free(arg);
    uint8_t *buf = malloc(MAX_FRAME);
    for (;;) {
        uint32_t len;
        if (!buf || read_all(r.s, (uint8_t *)&len, 4) || !len || len > MAX_FRAME || read_all(r.s, buf, len)) break;
        uint8_t type = buf[0];
        const uint8_t *p = buf + 1;
        size_t n = len - 1;
        __atomic_store_n(&last_rx, host_now_ns(), __ATOMIC_RELEASE);
        if (type == RELAY_PING) continue;  /* the heartbeat's answer: only that the relay is there */
        if (type == RELAY_HELLO && n >= 4) {
            uint32_t v;
            memcpy(&v, p, 4);
            __atomic_store_n(&server_protocol, (int)v, __ATOMIC_RELEASE);
            if (v >= 3 && n >= 12) udp_start(r.gen, p + 4);
            continue;
        }
        if (type == RELAY_NETINFO && n >= 0x480) {
            ldn_relay_netinfo(p);
        } else if (type == RELAY_LEAVE) {
            ldn_relay_closed(n ? p[0] : 2);
        } else if (type == RELAY_PACKET && n >= 12) {
            uint32_t dst, src;
            uint16_t dport, sport;
            memcpy(&dst, p, 4), memcpy(&dport, p + 4, 2), memcpy(&src, p + 6, 4), memcpy(&sport, p + 10, 2);
            socket_relay_packet(dst, dport, src, sport, p + 12, n - 12);
        } else {
            host_lock(&lock);
            if (type == want && r.gen == generation) {
                size_t k = n < answer_cap ? n : answer_cap;
                memcpy(answer, p, k);
                answer_len = (long)k;
                want = 0;
                host_cond_broadcast(&answered);
            }
            host_unlock(&lock);
        }
    }
    free(buf);
    host_lock(&lock);
    int current = r.gen == generation;
    if (current) {
        rclose(sock);
        sock = RBAD;
        host_cond_broadcast(&answered);  /* a waiting request gives up */
    }
    host_unlock(&lock);
    if (current) {
        fprintf(stderr, "yw4: relay: connection lost\n");
        ldn_relay_closed(6);  /* SignalLost */
    }
}

/* The heartbeat (protocol 2, 2026-11-20): a PING every 3 s, which the relay answers; it drops a game silent for 15 s
 * (a frozen or vanished one, which TCP alone can't see), and the game drops a relay silent for 12 s, so a dead server
 * shows as a lost room in seconds, not minutes. Only once the relay's hello says protocol 2 (an older relay ignores
 * PING and would look silent). One per connection; it ends with it. */
static void heartbeat(void *arg) {
    Reader r = *(Reader *)arg;
    free(arg);
    for (;;) {
        host_sleep_ns(3000000000LL);
        host_lock(&lock);
        int current = r.gen == generation && sock == r.s;
        if (current && __atomic_load_n(&server_protocol, __ATOMIC_ACQUIRE) >= 2) {
            int64_t quiet = host_now_ns() - __atomic_load_n(&last_rx, __ATOMIC_ACQUIRE);
            if (quiet > 12000000000LL) {
                fprintf(stderr, "yw4: relay: nothing from the relay for %lld s: the connection is gone\n",
                        (long long)(quiet / 1000000000));
#ifdef _WIN32
                shutdown(r.s, SD_BOTH);  /* the reader's recv fails: the usual "connection lost" */
#else
                shutdown(r.s, SHUT_RDWR);
#endif
                current = 0;
            } else {
                write_frame(r.s, RELAY_PING, NULL, 0);
                udp_bind_locked();  /* keeps the home router's UDP mapping open; the answer says UDP still works */
                if (__atomic_load_n(&udp_ok, __ATOMIC_ACQUIRE) &&
                    host_now_ns() - __atomic_load_n(&udp_rx, __ATOMIC_ACQUIRE) > 10000000000LL) {
                    __atomic_store_n(&udp_ok, 0, __ATOMIC_RELEASE);
                    fprintf(stderr, "yw4: relay: no UDP from the relay for 10 s: game packets by TCP\n");
                }
            }
        }
        host_unlock(&lock);
        if (!current) return;
    }
}

static void net_start(void) {
#ifdef _WIN32
    static int wsa;
    if (!__atomic_exchange_n(&wsa, 1, __ATOMIC_ACQ_REL)) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
    }
#endif
}

/* the connection, made on first use; lock held */
static int connect_locked(void) {
    if (sock != RBAD) return 0;
    if (!relay_enabled()) return -1;
    net_start();
    char host[256];
    snprintf(host, sizeof host, "%s", target());
    const char *port = "41230";
    char *colon = strrchr(host, ':');
    if (colon) *colon = 0, port = colon + 1;
    struct addrinfo *res = NULL;
    if (lookup(host, port, SOCK_STREAM, &res)) {
        fprintf(stderr, "yw4: relay: can't find %s\n", host);
        return -1;
    }
    RSock s = RBAD;
    for (struct addrinfo *a = res; a && s == RBAD; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s != RBAD && connect(s, a->ai_addr, (int)a->ai_addrlen)) {
            rclose(s);
            s = RBAD;
        } else if (s != RBAD && a->ai_addrlen <= sizeof peer) {
            memcpy(&peer, a->ai_addr, a->ai_addrlen);  /* the UDP side goes to the same address */
            peer_len = (int)a->ai_addrlen;
        }
    }
    freeaddrinfo(res);
    if (s == RBAD) {
        fprintf(stderr, "yw4: relay: can't connect to %s:%s\n", host, port);
        return -1;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    uint8_t room[64] = {0};
    const char *code = room_code();
    memcpy(room, code, strlen(code) < 64 ? strlen(code) : 64);
    if (write_frame(s, RELAY_HELLO, room, sizeof room)) {
        rclose(s);
        return -1;
    }
    sock = s;
    generation++;
    __atomic_store_n(&server_protocol, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&last_rx, host_now_ns(), __ATOMIC_RELEASE);
    Reader *r = malloc(sizeof *r), *h = malloc(sizeof *h);  /* each thread frees its own */
    if (!r || !h) {
        free(r), free(h);
        return -1;
    }
    r->s = s, r->gen = generation;
    *h = *r;
    host_thread_start(reader, r, 1 << 16);
    host_thread_start(heartbeat, h, 1 << 15);
    fprintf(stderr, "yw4: relay: connected to %s:%s, room code \"%s\"\n", host, port, code ? code : "");
    return 0;
}

long relay_request(uint8_t type, const void *p, size_t n, uint8_t *out, size_t cap, int timeout_ms) {
    host_lock(&lock);
    if (connect_locked()) {
        host_unlock(&lock);
        return -1;
    }
    want = type, answer = out, answer_cap = cap, answer_len = -1;
    RSock s = sock;
    host_unlock(&lock);
    if (write_frame(s, type, p, n)) return -1;
    host_lock(&lock);
    int64_t end = host_now_ns() + (int64_t)timeout_ms * 1000000;
    while (want == type && sock == s && host_now_ns() < end) host_cond_wait(&answered, &lock, end - host_now_ns());
    long got = want == 0 ? answer_len : -1;
    want = 0;
    host_unlock(&lock);
    return got;
}

int relay_send(uint8_t type, const void *p, size_t n) {
    host_lock(&lock);
    RSock s = sock;
    host_unlock(&lock);
    return s == RBAD ? -1 : write_frame(s, type, p, n);
}

int relay_send_packet(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport, const void *data, size_t n) {
    static int64_t last_tx;
    socket_note_gap("our packets out", &last_tx);  /* a gap: this game went quiet (socket.c) */
    if (n > MAX_FRAME - 64) return -1;
    uint8_t head[12];
    memcpy(head, &dst, 4), memcpy(head + 4, &dport, 2), memcpy(head + 6, &src, 4), memcpy(head + 10, &sport, 2);
    host_lock(&lock);
    if (__atomic_load_n(&udp_ok, __ATOMIC_ACQUIRE) && usock != RBAD && n <= UDP_MAX) {
        uint8_t d[8 + 1 + 12 + UDP_MAX];
        memcpy(d, udp_token, 8);
        d[8] = UDP_PACKET;
        memcpy(d + 9, head, 12);
        memcpy(d + 21, data, n);
        int sent = (int)send(usock, (const char *)d, (int)(21 + n), 0);
        if (sent == (int)(21 + n)) {
            host_unlock(&lock);
            return 0;
        }  /* else by TCP */
    }
    RSock s = sock;
    host_unlock(&lock);
    return s == RBAD ? -1 : write_frame2(s, RELAY_PACKET, head, 12, data, n);
}

/* ---- the overlay's Online tab: each server's ping (decision 0082). A connection of its own per server: connected,
 * then HELLO -> WELCOME timed, the best of three (one network round trip and the relay's answer, as a player's room
 * request takes). On a thread of its own; the page reads the results as they come. */
enum { PING_MAX = 4 };
static int ping_ms[PING_MAX];  /* -1 measuring, -2 no answer */
static char ping_addr[PING_MAX][64];
static int ping_n, ping_busy;

static int ping_once(const char *addr) {
    char host[64];
    snprintf(host, sizeof host, "%s", addr);
    const char *port = "41230";
    char *colon = strrchr(host, ':');
    if (colon) *colon = 0, port = colon + 1;
    struct addrinfo *res = NULL;
    if (lookup(host, port, SOCK_STREAM, &res)) return -2;
    RSock s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    int ms = -2;
    if (s != RBAD && !connect(s, res->ai_addr, (int)res->ai_addrlen)) {
#ifdef _WIN32
        DWORD to = 3000;
#else
        struct timeval to = {3, 0};
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        uint8_t room[64] = "ping", ans[9];
        int64_t t0 = host_now_ns();
        if (!write_frame(s, RELAY_HELLO, room, sizeof room) && !read_all(s, ans, sizeof ans) && ans[4] == RELAY_HELLO)
            ms = (int)((host_now_ns() - t0 + 500000) / 1000000);
    }
    if (s != RBAD) rclose(s);
    freeaddrinfo(res);
    return ms;
}

static void ping_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < ping_n; i++) {
        int best = -2;
        for (int k = 0; k < 3; k++) {
            int ms = ping_once(ping_addr[i]);
            if (ms >= 0 && (best < 0 || ms < best)) best = ms;
            if (ms == -2 && k == 0) break;  /* no answer at all: don't wait three times */
        }
        __atomic_store_n(&ping_ms[i], best, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&ping_busy, 0, __ATOMIC_RELEASE);
}

void relay_ping_start(const char *const *addrs, int n) {
    if (__atomic_exchange_n(&ping_busy, 1, __ATOMIC_ACQ_REL)) return;  /* already measuring */
    net_start();
    ping_n = n < PING_MAX ? n : PING_MAX;
    for (int i = 0; i < ping_n; i++) {
        snprintf(ping_addr[i], sizeof ping_addr[i], "%s", addrs[i]);
        __atomic_store_n(&ping_ms[i], -1, __ATOMIC_RELEASE);
    }
    if (host_thread_start(ping_thread, NULL, 1 << 16)) __atomic_store_n(&ping_busy, 0, __ATOMIC_RELEASE);
}

int relay_ping_ms(int i) { return i >= 0 && i < ping_n ? __atomic_load_n(&ping_ms[i], __ATOMIC_ACQUIRE) : -1; }
