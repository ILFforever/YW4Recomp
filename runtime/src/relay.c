/* The connection to the relay (relay.h, tools/relay/relay.py, decision 0057).
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
static uint8_t want;                        /* the answer type a request waits for, 0 for none */
static uint8_t *answer;
static size_t answer_cap;
static long answer_len;

int relay_enabled(void) {
    const char *e = getenv("YW4_RELAY");
    return e && *e;
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

/* the connection, made on first use; lock held */
static int connect_locked(void) {
    if (sock != RBAD) return 0;
    if (!relay_enabled()) return -1;
#ifdef _WIN32
    static int wsa;
    if (!wsa) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        wsa = 1;
    }
#endif
    char host[256];
    snprintf(host, sizeof host, "%s", getenv("YW4_RELAY"));
    const char *port = "41230";
    char *colon = strrchr(host, ':');
    if (colon) *colon = 0, port = colon + 1;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) {
        fprintf(stderr, "yw4: relay: can't find %s\n", host);
        return -1;
    }
    RSock s = RBAD;
    for (struct addrinfo *a = res; a && s == RBAD; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s != RBAD && connect(s, a->ai_addr, (int)a->ai_addrlen)) {
            rclose(s);
            s = RBAD;
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
    const char *code = getenv("YW4_ROOM");
    if (code) memcpy(room, code, strlen(code) < 64 ? strlen(code) : 64);
    if (write_frame(s, RELAY_HELLO, room, sizeof room)) {
        rclose(s);
        return -1;
    }
    sock = s;
    generation++;
    Reader *r = malloc(sizeof *r);
    if (!r) return -1;
    r->s = s, r->gen = generation;
    host_thread_start(reader, r, 1 << 16);
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
    if (n > MAX_FRAME - 64) return -1;
    uint8_t head[12];
    memcpy(head, &dst, 4), memcpy(head + 4, &dport, 2), memcpy(head + 6, &src, 4), memcpy(head + 10, &sport, 2);
    host_lock(&lock);
    RSock s = sock;
    host_unlock(&lock);
    return s == RBAD ? -1 : write_frame2(s, RELAY_PACKET, head, 12, data, n);
}
