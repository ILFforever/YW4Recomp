/* nn::socket, native on the host's sockets (decision 0057).
 *
 * The sdk's nn::socket functions send IPC to bsd:u; here they are native overrides on Winsock (BSD sockets
 * elsewhere). What differs from the host and is translated:
 *  - sockaddr: the Switch's is BSD's (u8 length, u8 family), the host's starts with a u16 family;
 *  - poll events and errno: the Switch uses Linux's numbers (POLLIN 1, POLLOUT 4; EAGAIN 11, ...);
 *  - descriptors: the game's are small numbers of ours, each naming a host socket.
 *
 * UDP is the local-play network's (169.254.0.0/16, nn::ldn): every UDP socket is a host socket on 127.0.0.1 with
 * a port of the host's choosing, and the port the game asked for is only remembered, so two games on one PC
 * don't collide. Packets to another player go to the relay (relay.c); packets from the relay are handed to the
 * socket bound to their port through a loopback "doorbell" socket, with their sender in front (6 bytes), which
 * RecvFrom takes off again. So Poll and blocking reads work on the host socket as they are.
 * Every call is logged ("yw4: socket: ...") unless YW4_LDN_LOG=0.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET HSock;
#define HBAD INVALID_SOCKET
#define hclose closesocket
#define hpoll WSAPoll
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int HSock;
#define HBAD (-1)
#define hclose close
#define hpoll poll
#endif

#undef GPTR  /* WinBase.h: a GlobalAlloc flag; ours is the guest pointer */
#include "hle.h"
#include "host.h"
#include "yw4load.h"
#include "relay.h"

/* the Switch's errno numbers (Linux's) */
enum { G_EPERM = 1, G_EINTR = 4, G_EBADF = 9, G_EAGAIN = 11, G_ENOMEM = 12, G_EACCES = 13, G_EFAULT = 14,
       G_EINVAL = 22, G_EMFILE = 24, G_ENOSYS = 38, G_ENOTSOCK = 88, G_EMSGSIZE = 90, G_ENOPROTOOPT = 92,
       G_EAFNOSUPPORT = 97, G_EADDRINUSE = 98, G_EADDRNOTAVAIL = 99, G_ENETUNREACH = 101, G_ECONNRESET = 104,
       G_ENOTCONN = 107, G_ETIMEDOUT = 110, G_ECONNREFUSED = 111, G_EHOSTUNREACH = 113, G_EALREADY = 114,
       G_EINPROGRESS = 115 };

static _Thread_local int last_errno;

static int verbose(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("YW4_LDN_LOG");
        v = !(e && *e == '0');
    }
    return v;
}
#define LOG(...) do { if (verbose()) fprintf(stderr, "yw4: socket: " __VA_ARGS__); } while (0)

#define MAX_FDS 64
typedef struct {
    HSock h;
    int used, nonblock, type;
    uint32_t guest_addr;  /* the address the game bound (network order), as GetSockName reports it */
    uint16_t guest_port;  /* network order: the port the game asked for, which the relay's packets name */
    int bound;
    uint32_t peer_addr;   /* a UDP socket's Connect: the default destination (network order) */
    uint16_t peer_port;
} Fd;
static HSock bell = HBAD;   /* loopback socket the relay's packets are rung through */
static HostLock bell_lock = HOST_LOCK_INIT;
static Fd fds[MAX_FDS];
static HostLock fd_lock = HOST_LOCK_INIT;

static int host_errno(void) {
#ifdef _WIN32
    switch (WSAGetLastError()) {
    case WSAEWOULDBLOCK: return G_EAGAIN;
    case WSAEINTR: return G_EINTR;
    case WSAEBADF: case WSAENOTSOCK: return G_EBADF;
    case WSAEACCES: return G_EACCES;
    case WSAEFAULT: return G_EFAULT;
    case WSAEINVAL: return G_EINVAL;
    case WSAEMFILE: return G_EMFILE;
    case WSAEMSGSIZE: return G_EMSGSIZE;
    case WSAENOPROTOOPT: return G_ENOPROTOOPT;
    case WSAEAFNOSUPPORT: return G_EAFNOSUPPORT;
    case WSAEADDRINUSE: return G_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return G_EADDRNOTAVAIL;
    case WSAENETUNREACH: return G_ENETUNREACH;
    case WSAECONNRESET: return G_ECONNRESET;
    case WSAENOTCONN: return G_ENOTCONN;
    case WSAETIMEDOUT: return G_ETIMEDOUT;
    case WSAECONNREFUSED: return G_ECONNREFUSED;
    case WSAEHOSTUNREACH: return G_EHOSTUNREACH;
    case WSAEALREADY: return G_EALREADY;
    case WSAEINPROGRESS: return G_EINPROGRESS;
    default: return G_EINVAL;
    }
#else
    return errno;  /* Linux host: the same numbers */
#endif
}

/* result -1 with errno set, else the value; x0 is an int */
static void ret(CPUState *c, int64_t v, int err) {
    if (v < 0) last_errno = err;
    c->x[0] = (uint64_t)v;
}

static Fd *get(int fd) {
    return fd >= 0 && fd < MAX_FDS && fds[fd].used ? &fds[fd] : NULL;
}

static int is_local_net(uint32_t a_be) { return (ntohl(a_be) >> 16) == 0xA9FE; }  /* 169.254.0.0/16 */

/* the Switch's sockaddr_in (BSD: u8 len, u8 family, u16 port, u32 addr, 8 zero) to the host's */
static int from_guest(uint64_t gaddr, uint32_t len, struct sockaddr_in *out, uint32_t *guest_ip) {
    if (!gaddr || len < 8) return -1;
    const uint8_t *p = GPTR(gaddr);
    if (p[1] != 2) return -1;  /* AF_INET only */
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, p + 2, 2);
    memcpy(&out->sin_addr, p + 4, 4);
    if (guest_ip) *guest_ip = out->sin_addr.s_addr;
    return 0;
}
static void to_guest(uint64_t gaddr, uint64_t glen, uint32_t ip_be, uint16_t port_be) {
    if (!gaddr) return;
    uint8_t s[16] = {16, 2};
    memcpy(s + 2, &port_be, 2);
    memcpy(s + 4, &ip_be, 4);
    uint32_t room = glen ? *(uint32_t *)GPTR(glen) : 16;
    memcpy(GPTR(gaddr), s, room < 16 ? room : 16);
    if (glen) *(uint32_t *)GPTR(glen) = 16;
}
static const char *ip_str(uint32_t be, uint16_t port_be, char *buf) {
    uint32_t a = ntohl(be);
    sprintf(buf, "%u.%u.%u.%u:%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255, ntohs(port_be));
    return buf;
}

static void set_nonblock(Fd *f, int on) {
    f->nonblock = on;
#ifdef _WIN32
    u_long v = (u_long)on;
    ioctlsocket(f->h, FIONBIO, &v);
#else
    int fl = fcntl(f->h, F_GETFL, 0);
    fcntl(f->h, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
#endif
}

/* an unbound UDP socket that sends gets a port, as bind(0) would give it */
static void autobind(Fd *f) {
    if (f->bound) return;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (bind(f->h, (struct sockaddr *)&a, sizeof a) == 0) {
        socklen_t l = sizeof a;
        getsockname(f->h, (struct sockaddr *)&a, &l);
        f->guest_port = a.sin_port;
        f->bound = 1;
    }
}

static HSock bell_socket(void) {
    host_lock(&bell_lock);
    if (bell == HBAD) {
        HSock b = socket(AF_INET, SOCK_DGRAM, 17);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(0x7F000001);
        if (b != HBAD && bind(b, (struct sockaddr *)&a, sizeof a) == 0) bell = b;
        else if (b != HBAD) hclose(b);
    }
    HSock b = bell;
    host_unlock(&bell_lock);
    return b;
}
static uint16_t bell_port(void) {
    static uint16_t port;
    if (!port && bell_socket() != HBAD) {
        struct sockaddr_in a;
        socklen_t l = sizeof a;
        getsockname(bell, (struct sockaddr *)&a, &l);
        port = a.sin_port;
    }
    return port;
}

/* a packet for the game (relay.c's reader, or our own to ourselves): rung into the socket bound to its port,
 * its sender in front. Addresses and ports in host order. */
void socket_relay_packet(uint32_t dst, uint16_t dport, uint32_t src, uint16_t sport, const uint8_t *data, size_t n) {
    (void)dst;
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(0x7F000001);
    host_lock(&fd_lock);
    for (int i = 0; i < MAX_FDS; i++)
        if (fds[i].used && fds[i].type == 2 && fds[i].bound && ntohs(fds[i].guest_port) == dport) {
            struct sockaddr_in a;
            socklen_t l = sizeof a;
            getsockname(fds[i].h, (struct sockaddr *)&a, &l);
            to.sin_port = a.sin_port;
            break;
        }
    host_unlock(&fd_lock);
    if (!to.sin_port || n > 65000) return;
    HSock b = bell_socket();
    if (b == HBAD) return;
    uint8_t *buf = malloc(n + 6);
    if (!buf) return;
    memcpy(buf, &src, 4);
    memcpy(buf + 4, &sport, 2);
    memcpy(buf + 6, data, n);
    sendto(b, (const char *)buf, (int)(n + 6), 0, (struct sockaddr *)&to, sizeof to);
    free(buf);
}

/* UDP to the local-play network (addresses network order): to the relay, or to ourselves */
static int send_local(Fd *f, uint32_t dst_be, uint16_t port_be, const void *data, size_t n) {
    autobind(f);
    uint32_t me = ldn_my_address(), dst = ntohl(dst_be);
    if (dst == me) {
        socket_relay_packet(dst, ntohs(port_be), me, ntohs(f->guest_port), data, n);
        return 0;
    }
    if (!me) return -1;  /* in no network */
    return relay_send_packet(dst, ntohs(port_be), me, ntohs(f->guest_port), data, n);
}

/* ---- the overrides */

void yw4_socket_Initialize(CPUState *c) {
#ifdef _WIN32
    static int done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = 1;
    }
#endif
    LOG("Initialize\n");
    c->x[0] = 0;
}
void yw4_socket_Finalize(CPUState *c) { LOG("Finalize\n"); c->x[0] = 0; }
void yw4_socket_GetLastErrno(CPUState *c) { c->x[0] = (uint64_t)last_errno; }

/* Socket(family, type, protocol) */
void yw4_socket_Socket(CPUState *c) {
    int fam = (int)c->x[0], type = (int)c->x[1], proto = (int)c->x[2];
    int nb = (type & 0x800) != 0;  /* SOCK_NONBLOCK, Linux's */
    type &= 0xF;
    HSock h = fam == 2 ? socket(AF_INET, type, proto) : HBAD;
    if (h == HBAD) {
        LOG("Socket(%d, %d, %d) failed\n", fam, type, proto);
        ret(c, -1, fam == 2 ? host_errno() : G_EAFNOSUPPORT);
        return;
    }
    host_lock(&fd_lock);
    int fd = 3;
    while (fd < MAX_FDS && fds[fd].used) fd++;
    if (fd == MAX_FDS) {
        host_unlock(&fd_lock);
        hclose(h);
        ret(c, -1, G_EMFILE);
        return;
    }
    fds[fd] = (Fd){.h = h, .used = 1, .type = type};
    if (nb) set_nonblock(&fds[fd], 1);
    host_unlock(&fd_lock);
    LOG("Socket(%d, %s, %d) -> %d\n", fam, type == 2 ? "udp" : type == 1 ? "tcp" : "?", proto, fd);
    c->x[0] = (uint64_t)fd;
}

void yw4_socket_Close(CPUState *c) {
    int fd = (int)c->x[0];
    host_lock(&fd_lock);
    Fd *f = get(fd);
    if (f) {
        hclose(f->h);
        f->used = 0;
    }
    host_unlock(&fd_lock);
    LOG("Close(%d)\n", fd);
    ret(c, f ? 0 : -1, G_EBADF);
}

void yw4_socket_Shutdown(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    LOG("Shutdown(%d, %d)\n", (int)c->x[0], (int)c->x[1]);
    ret(c, f ? shutdown(f->h, (int)c->x[1]) : -1, f ? host_errno() : G_EBADF);
}

/* Bind(fd, const sockaddr *, len) */
void yw4_socket_Bind(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    struct sockaddr_in a;
    uint32_t gip = 0;
    char b[32];
    if (!f || from_guest(c->x[1], (uint32_t)c->x[2], &a, &gip)) { ret(c, -1, f ? G_EINVAL : G_EBADF); return; }
    uint16_t want_port = a.sin_port;
    if (f->type == 2 || is_local_net(gip)) {  /* the local-play network: this PC, a port of the host's choosing */
        a.sin_addr.s_addr = htonl(0x7F000001);
        if (f->type == 2) a.sin_port = 0;
    }
    int r = bind(f->h, (struct sockaddr *)&a, sizeof a);
    if (r == 0) {
        struct sockaddr_in got;
        socklen_t gl = sizeof got;
        getsockname(f->h, (struct sockaddr *)&got, &gl);
        f->guest_addr = gip;
        f->guest_port = want_port ? want_port : got.sin_port;
        f->bound = 1;
    }
    LOG("Bind(%d, %s)%s\n", (int)c->x[0], ip_str(gip, a.sin_port, b), r ? " failed" : "");
    ret(c, r, host_errno());
}

void yw4_socket_GetSockName(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    if (!f) { ret(c, -1, G_EBADF); return; }
    uint32_t me = htonl(ldn_my_address());
    to_guest(c->x[1], c->x[2], f->guest_addr ? f->guest_addr : me, f->guest_port);
    c->x[0] = 0;
}

void yw4_socket_Connect(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    struct sockaddr_in a;
    uint32_t gip = 0;
    char b[32];
    if (!f || from_guest(c->x[1], (uint32_t)c->x[2], &a, &gip)) { ret(c, -1, f ? G_EINVAL : G_EBADF); return; }
    LOG("Connect(%d, %s)\n", (int)c->x[0], ip_str(gip, a.sin_port, b));
    if (f->type == 2) {  /* UDP: only remembers the destination */
        f->peer_addr = gip, f->peer_port = a.sin_port;
        autobind(f);
        c->x[0] = 0;
        return;
    }
    if (is_local_net(gip)) { ret(c, -1, G_ECONNREFUSED); return; }  /* TCP in the local network: not carried */
    int r = connect(f->h, (struct sockaddr *)&a, sizeof a);
    ret(c, r, host_errno());
}

void yw4_socket_Listen(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    LOG("Listen(%d, %d)\n", (int)c->x[0], (int)c->x[1]);
    ret(c, f ? listen(f->h, (int)c->x[1]) : -1, f ? host_errno() : G_EBADF);
}

void yw4_socket_Accept(CPUState *c) {
    LOG("Accept(%d): not yet\n", (int)c->x[0]);
    ret(c, -1, G_EAGAIN);
}

/* SendTo(fd, buf, len, flags, const sockaddr *, addrlen) */
void yw4_socket_SendTo(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    size_t n = c->x[2];
    if (!f) { ret(c, -1, G_EBADF); return; }
    struct sockaddr_in a = {0};
    uint32_t gip = 0;
    if (!c->x[4]) {  /* connected socket */
        if (f->type != 2 || !f->peer_port) {
            int r = send(f->h, GPTR(c->x[1]), (int)n, 0);
            ret(c, r, host_errno());
            return;
        }
        gip = f->peer_addr, a.sin_port = f->peer_port;
    } else if (from_guest(c->x[4], (uint32_t)c->x[5], &a, &gip)) {
        ret(c, -1, G_EINVAL);
        return;
    }
    if (f->type == 2 && (is_local_net(gip) || gip == 0xFFFFFFFFu)) {
        static int logged;
        char b[32];
        int r = send_local(f, gip == 0xFFFFFFFFu ? htonl(0xA9FE01FF) : gip, a.sin_port, GPTR(c->x[1]), n);
        if (logged++ < 30) LOG("SendTo(%d, %zu bytes, %s)%s\n", (int)c->x[0], n, ip_str(gip, a.sin_port, b),
                               r ? ": in no room, dropped" : "");
        c->x[0] = n;  /* UDP: sent, whether anyone hears it or not */
        return;
    }
    int r = sendto(f->h, GPTR(c->x[1]), (int)n, 0, (struct sockaddr *)&a, sizeof a);
    ret(c, r, host_errno());
}
void yw4_socket_Send(CPUState *c) {
    c->x[4] = 0;
    yw4_socket_SendTo(c);
}

/* RecvFrom(fd, buf, len, flags, sockaddr *, socklen *) */
void yw4_socket_RecvFrom(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    if (!f) { ret(c, -1, G_EBADF); return; }
    int flags = (int)c->x[3];
    int dontwait = (flags & 0x40) != 0;  /* MSG_DONTWAIT, Linux's */
    if (dontwait && !f->nonblock) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(f->h, &rs);
        struct timeval tv = {0, 0};
        if (select((int)f->h + 1, &rs, NULL, NULL, &tv) <= 0) { ret(c, -1, G_EAGAIN); return; }
    }
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    if (f->type != 2) {
        int r = recvfrom(f->h, GPTR(c->x[1]), (int)c->x[2], flags & 0x2 /* MSG_PEEK */, (struct sockaddr *)&a, &al);
        if (r >= 0 && c->x[4]) to_guest(c->x[4], c->x[5], a.sin_addr.s_addr, a.sin_port);
        ret(c, r, host_errno());
        return;
    }
    /* UDP: what the doorbell rang in carries its sender in front */
    static _Thread_local uint8_t *buf;
    if (!buf && !(buf = malloc(65536 + 6))) { ret(c, -1, G_ENOMEM); return; }
    size_t want = c->x[2] < 65536 ? c->x[2] : 65536;
    int r = recvfrom(f->h, (char *)buf, (int)want + 6, flags & 0x2 /* MSG_PEEK */, (struct sockaddr *)&a, &al);
    if (r < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEMSGSIZE) r = (int)want + 6;  /* truncated, as on the Switch */
        else
#endif
        { ret(c, -1, host_errno()); return; }
    }
    uint32_t src_be = a.sin_addr.s_addr;
    uint16_t sport_be = a.sin_port;
    const uint8_t *data = buf;
    if (a.sin_port == bell_port() && r >= 6) {
        uint32_t src;
        uint16_t sport;
        memcpy(&src, buf, 4);
        memcpy(&sport, buf + 4, 2);
        src_be = htonl(src), sport_be = htons(sport);
        data += 6, r -= 6;
    }
    if ((size_t)r > want) r = (int)want;
    memcpy(GPTR(c->x[1]), data, (size_t)r);
    if (c->x[4]) to_guest(c->x[4], c->x[5], src_be, sport_be);
    c->x[0] = (uint64_t)r;
}
void yw4_socket_Recv(CPUState *c) {
    c->x[4] = 0, c->x[5] = 0;
    yw4_socket_RecvFrom(c);
}

/* Poll(pollfd *, nfds, timeout ms): Linux's event bits */
void yw4_socket_Poll(CPUState *c) {
    uint8_t *p = GPTR(c->x[0]);
    size_t n = c->x[1];
    int timeout = (int)c->x[2];
    if (n > MAX_FDS) { ret(c, -1, G_EINVAL); return; }
#ifdef _WIN32
    WSAPOLLFD h[MAX_FDS];
#else
    struct pollfd h[MAX_FDS];
#endif
    for (size_t i = 0; i < n; i++) {
        int32_t fd;
        int16_t ev;
        memcpy(&fd, p + 8 * i, 4);
        memcpy(&ev, p + 8 * i + 4, 2);
        Fd *f = get(fd);
        h[i].fd = f ? f->h : HBAD;
        h[i].events = (short)((ev & 1 ? POLLIN : 0) | (ev & 4 ? POLLOUT : 0));
        h[i].revents = 0;
    }
    int r = n ? hpoll(h, (unsigned)n, timeout) : 0;
    if (!n && timeout > 0) host_sleep_ns((int64_t)timeout * 1000000);
    for (size_t i = 0; i < n; i++) {
        int16_t rev = (int16_t)((h[i].revents & POLLIN ? 1 : 0) | (h[i].revents & POLLOUT ? 4 : 0) |
                                (h[i].revents & POLLERR ? 8 : 0) | (h[i].revents & POLLHUP ? 0x10 : 0) |
                                (h[i].revents & POLLNVAL ? 0x20 : 0));
        memcpy(p + 8 * i + 6, &rev, 2);
    }
    ret(c, r, host_errno());
}

/* Fcntl(fd, cmd, ...): F_GETFL 3, F_SETFL 4; O_NONBLOCK is Linux's 0x800 */
void yw4_socket_Fcntl(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    int cmd = (int)c->x[1];
    if (!f) { ret(c, -1, G_EBADF); return; }
    if (cmd == 3) { c->x[0] = f->nonblock ? 0x800 : 0; return; }
    if (cmd == 4) {
        set_nonblock(f, (c->x[2] & 0x800) != 0);
        LOG("Fcntl(%d, F_SETFL, %#x)\n", (int)c->x[0], (unsigned)c->x[2]);
        c->x[0] = 0;
        return;
    }
    LOG("Fcntl(%d, %d): not handled\n", (int)c->x[0], cmd);
    c->x[0] = 0;
}

/* SetSockOpt(fd, level, name, const void *, len): SOL_SOCKET (0xffff) options are BSD's, as Winsock's; the rest
 * is logged and accepted */
void yw4_socket_SetSockOpt(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    int level = (int)c->x[1], name = (int)c->x[2];
    uint32_t len = (uint32_t)c->x[4];
    if (!f) { ret(c, -1, G_EBADF); return; }
    int v = 0;
    if (len >= 4) memcpy(&v, GPTR(c->x[3]), 4);
    LOG("SetSockOpt(%d, level %#x, option %#x, value %d)\n", (int)c->x[0], level, name, v);
    if (level == 0xFFFF && name == 0x1100) { set_nonblock(f, v != 0); c->x[0] = 0; return; }  /* SO_NONBLOCK */
    if (level == 0xFFFF && (name == 4 || name == 0x20 || name == 0x1001 || name == 0x1002)) {
        setsockopt(f->h, SOL_SOCKET, name, (const char *)&v, sizeof v);
    }
    c->x[0] = 0;
}

void yw4_socket_GetSockOpt(CPUState *c) {
    Fd *f = get((int)c->x[0]);
    int level = (int)c->x[1], name = (int)c->x[2];
    if (!f) { ret(c, -1, G_EBADF); return; }
    int v = 0;
    socklen_t l = sizeof v;
    if (level == 0xFFFF) getsockopt(f->h, SOL_SOCKET, name == 0x1007 ? SO_ERROR : name, (char *)&v, &l);
    if (c->x[3]) memcpy(GPTR(c->x[3]), &v, 4);
    if (c->x[4]) *(uint32_t *)GPTR(c->x[4]) = 4;
    LOG("GetSockOpt(%d, level %#x, option %#x) -> %d\n", (int)c->x[0], level, name, v);
    c->x[0] = 0;
}

/* GetAddrInfo: no name lookups (the internet services are gone) */
void yw4_socket_GetAddrInfo(CPUState *c) {
    LOG("GetAddrInfo(\"%s\"): no lookups\n", c->x[0] ? (const char *)GPTR(c->x[0]) : "");
    if (c->x[3]) *(uint64_t *)GPTR(c->x[3]) = 0;
    c->x[0] = (uint64_t)(int64_t)-2;  /* EAI_NONAME */
}
