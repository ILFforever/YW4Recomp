/* Discord's local IPC (discord_ipc.h, decision 0077). */
#include "discord_ipc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* ---- the messages */

size_t discord_frame(uint8_t *out, size_t cap, uint32_t op, const char *json) {
    size_t len = strlen(json);
    if (len > 0xFFFFFFF0u || len + 8 > cap) return 0;
    uint32_t h[2] = {op, (uint32_t)len};
    for (int i = 0; i < 2; i++)
        for (int b = 0; b < 4; b++) out[i * 4 + b] = (uint8_t)(h[i] >> (8 * b));  /* little-endian on any host */
    memcpy(out + 8, json, len);
    return len + 8;
}

/* Appending to a buffer: `ok` turns 0 once something didn't fit. */
typedef struct {
    char *p;
    size_t n, len;
    int ok;
} Out;

static void put(Out *o, const char *fmt, ...) {
    if (!o->ok) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o->p + o->len, o->n - o->len, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= o->n - o->len) o->ok = 0;
    else o->len += (size_t)k;
}

/* A JSON string: quotes, backslashes and control characters escaped; UTF-8 passes as it is. */
static void put_string(Out *o, const char *s) {
    put(o, "\"");
    for (; *s && o->ok; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') put(o, "\\%c", c);
        else if (c < 0x20) put(o, "\\u%04x", c);
        else put(o, "%c", c);
    }
    put(o, "\"");
}

size_t discord_handshake_json(char *out, size_t n, const char *client_id) {
    Out o = {out, n, 0, n > 0};
    put(&o, "{\"v\":1,\"client_id\":");
    put_string(&o, client_id);
    put(&o, "}");
    return o.ok ? o.len : 0;
}

static void put_field(Out *o, int *first, const char *key, const char *value) {
    if (!value || !*value) return;
    put(o, "%s\"%s\":", *first ? "" : ",", key);
    put_string(o, value);
    *first = 0;
}

size_t discord_activity_json(char *out, size_t n, long pid, const DiscordActivity *a, unsigned nonce) {
    Out o = {out, n, 0, n > 0};
    put(&o, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":", pid);
    if (!a) {
        put(&o, "null");
    } else {
        int first = 1;
        put(&o, "{");
        put_field(&o, &first, "details", a->details);
        put_field(&o, &first, "state", a->state);
        if (a->start > 0) put(&o, "%s\"timestamps\":{\"start\":%lld}", first ? "" : ",", (long long)a->start), first = 0;
        if ((a->large_image && *a->large_image) || (a->large_text && *a->large_text)) {
            int f2 = 1;
            put(&o, "%s\"assets\":{", first ? "" : ",");
            put_field(&o, &f2, "large_image", a->large_image);
            put_field(&o, &f2, "large_text", a->large_text);
            put(&o, "}");
            first = 0;
        }
        if (a->button_label && *a->button_label && a->button_url && *a->button_url) {
            int f2 = 1;
            put(&o, "%s\"buttons\":[{", first ? "" : ",");
            put_field(&o, &f2, "label", a->button_label);
            put_field(&o, &f2, "url", a->button_url);
            put(&o, "}]");
            first = 0;
        }
        put(&o, "}");
    }
    put(&o, "},\"nonce\":\"%u\"}", nonce);
    return o.ok ? o.len : 0;
}

/* ---- the connection */

#ifdef _WIN32

static int open_path(DiscordIpc *c, const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    c->h = (intptr_t)h, c->open = 1;
    return 0;
}

int discord_ipc_connect(DiscordIpc *c, const char *path) {
    c->open = 0;
    if (path) return open_path(c, path);
    for (int i = 0; i < 10; i++) {
        char p[64];
        snprintf(p, sizeof p, "\\\\.\\pipe\\discord-ipc-%d", i);
        if (!open_path(c, p)) return 0;
    }
    return -1;
}

void discord_ipc_close(DiscordIpc *c) {
    if (c->open) CloseHandle((HANDLE)c->h);
    c->open = 0;
}

static int write_all(DiscordIpc *c, const uint8_t *p, size_t n) {
    while (n) {
        DWORD done = 0;
        if (!WriteFile((HANDLE)c->h, p, (DWORD)n, &done, NULL) || !done) return -1;
        p += done, n -= done;
    }
    return 0;
}

static int read_all(DiscordIpc *c, uint8_t *p, size_t n) {
    while (n) {
        DWORD done = 0;
        if (!ReadFile((HANDLE)c->h, p, (DWORD)n, &done, NULL) || !done) return -1;
        p += done, n -= done;
    }
    return 0;
}

/* 1 once a frame's header can be read, 0 at the end of wait_ms, -1 the pipe is gone */
static int wait_readable(DiscordIpc *c, int wait_ms) {
    for (int waited = 0;; waited += 5) {
        DWORD avail = 0;
        if (!PeekNamedPipe((HANDLE)c->h, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail >= 8) return 1;
        if (waited >= wait_ms) return 0;
        Sleep(5);
    }
}

#else

static int open_path(DiscordIpc *c, const char *path) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof a.sun_path) return -1;
    strcpy(a.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    c->h = fd, c->open = 1;
    return 0;
}

int discord_ipc_connect(DiscordIpc *c, const char *path) {
    c->open = 0;
    if (path) return open_path(c, path);
    const char *vars[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    const char *dirs[5];
    int nd = 0;
    for (int i = 0; i < 4; i++) {
        const char *d = getenv(vars[i]);
        if (d && *d) dirs[nd++] = d;
    }
    dirs[nd++] = "/tmp";
    /* the Discord app itself, then its Flatpak and Snap packages, which put the socket in a folder of their own */
    static const char *const sub[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};
    for (int d = 0; d < nd; d++)
        for (int s = 0; s < 3; s++)
            for (int i = 0; i < 10; i++) {
                char p[256];
                snprintf(p, sizeof p, "%s/%sdiscord-ipc-%d", dirs[d], sub[s], i);
                if (!open_path(c, p)) return 0;
            }
    return -1;
}

void discord_ipc_close(DiscordIpc *c) {
    if (c->open) close((int)c->h);
    c->open = 0;
}

static int write_all(DiscordIpc *c, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t k = send((int)c->h, p, n, MSG_NOSIGNAL);  /* a closed socket is an error here, not SIGPIPE */
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k, n -= (size_t)k;
    }
    return 0;
}

static int read_all(DiscordIpc *c, uint8_t *p, size_t n) {
    while (n) {
        ssize_t k = read((int)c->h, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k, n -= (size_t)k;
    }
    return 0;
}

static int wait_readable(DiscordIpc *c, int wait_ms) {
    struct pollfd f = {.fd = (int)c->h, .events = POLLIN};
    int r = poll(&f, 1, wait_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (!r) return 0;
    return f.revents & (POLLIN | POLLHUP | POLLERR) ? 1 : 0;  /* a hang-up shows as a failed read next */
}

#endif

int discord_ipc_send(DiscordIpc *c, uint32_t op, const char *json) {
    if (!c->open) return -1;
    size_t cap = strlen(json) + 8;
    uint8_t *f = malloc(cap);
    int r = f && discord_frame(f, cap, op, json) && !write_all(c, f, cap) ? 0 : -1;
    free(f);
    if (r) discord_ipc_close(c);
    return r;
}

int discord_ipc_recv(DiscordIpc *c, uint32_t *op, char *buf, size_t n, int wait_ms) {
    if (!c->open) return -1;
    int w = wait_readable(c, wait_ms);
    if (w <= 0) {
        if (w < 0) discord_ipc_close(c);
        return w;
    }
    uint8_t h[8];
    if (read_all(c, h, 8)) {
        discord_ipc_close(c);
        return -1;
    }
    uint32_t o = h[0] | h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    uint32_t len = h[4] | h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
    if (len > (1u << 20)) {  /* not Discord's: nothing it sends is this large */
        discord_ipc_close(c);
        return -1;
    }
    for (uint32_t got = 0; got < len;) {  /* all of it read, what fits kept */
        uint8_t tmp[512];
        uint32_t k = len - got < sizeof tmp ? len - got : (uint32_t)sizeof tmp;
        if (read_all(c, tmp, k)) {
            discord_ipc_close(c);
            return -1;
        }
        if (n && got < n - 1) memcpy(buf + got, tmp, got + k < n - 1 ? k : n - 1 - got);
        got += k;
    }
    if (n) buf[len < n - 1 ? len : n - 1] = 0;
    *op = o;
    return 1;
}
