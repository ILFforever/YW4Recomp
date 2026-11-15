"""Discord Rich Presence (runtime/src/discord.c, discord_ipc.c; decision 0077): the messages built byte for byte, and
the whole path against a stand-in Discord: the presence thread connects (Windows named pipe / Linux socket),
handshakes with the application ID, gets READY and sends the activity (the game's icon and the time it started)."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from recomp import check

RUNTIME = check.REPO / "runtime"
POSIX = [] if sys.platform == "win32" else ["-D_GNU_SOURCE", "-pthread"]

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "discord.h"
#include "discord_ipc.h"

#ifdef _WIN32
#include <windows.h>
#define PATH_FMT "\\\\.\\pipe\\yw4-discord-test-%lu"
static unsigned long my_pid(void) { return GetCurrentProcessId(); }
#else
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define PATH_FMT "/tmp/yw4-discord-test-%lu"
static unsigned long my_pid(void) { return (unsigned long)getpid(); }
#endif

/* ---- what discord.c needs from the host, faked */
void host_sleep_ns(long long ns) {
#ifdef _WIN32
    Sleep((DWORD)(ns / 1000000));
#else
    usleep((useconds_t)(ns / 1000));
#endif
}
static void (*thread_fn)(void *);
#ifdef _WIN32
static DWORD WINAPI trampoline(LPVOID a) { thread_fn(a); return 0; }
int host_thread_start(void (*fn)(void *), void *arg, size_t stack) {
    (void)stack; thread_fn = fn;
    return CreateThread(NULL, 0, trampoline, arg, 0, NULL) ? 0 : -1;
}
#else
static void *trampoline(void *a) { thread_fn(a); return NULL; }
int host_thread_start(void (*fn)(void *), void *arg, size_t stack) {
    (void)stack; thread_fn = fn; pthread_t t;
    return pthread_create(&t, NULL, trampoline, arg) ? -1 : 0;
}
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---- a stand-in Discord: one client, its frames read and answered */
#ifdef _WIN32
static HANDLE server;
static int server_open(const char *path) {
    server = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    return server == INVALID_HANDLE_VALUE ? -1 : 0;
}
static int server_accept(void) { return ConnectNamedPipe(server, NULL) || GetLastError() == ERROR_PIPE_CONNECTED ? 0 : -1; }
static int sread(void *p, unsigned n) {
    for (DWORD d = 0; n; n -= d, p = (char *)p + d) if (!ReadFile(server, p, n, &d, NULL) || !d) return -1;
    return 0;
}
static int swrite(const void *p, unsigned n) { DWORD d; return WriteFile(server, p, n, &d, NULL) && d == n ? 0 : -1; }
static void server_close(void) { CloseHandle(server); }
#else
static int lsock = -1, server = -1;
static char spath[128];
static int server_open(const char *path) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(spath, sizeof spath, "%s", path); unlink(path); strcpy(a.sun_path, path);
    lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    return lsock < 0 || bind(lsock, (struct sockaddr *)&a, sizeof a) || listen(lsock, 1) ? -1 : 0;
}
static int server_accept(void) { server = accept(lsock, NULL, NULL); return server < 0 ? -1 : 0; }
static int sread(void *p, unsigned n) {
    for (ssize_t d; n; n -= (unsigned)d, p = (char *)p + d) if ((d = read(server, p, n)) <= 0) return -1;
    return 0;
}
static int swrite(const void *p, unsigned n) { return write(server, p, n) == (ssize_t)n ? 0 : -1; }
static void server_close(void) { close(server); close(lsock); unlink(spath); }
#endif

static int frame_in(unsigned *op, char *json, unsigned cap) {
    unsigned char h[8];
    if (sread(h, 8)) return -1;
    *op = h[0] | h[1] << 8 | h[2] << 16 | (unsigned)h[3] << 24;
    unsigned len = h[4] | h[5] << 8 | h[6] << 16 | (unsigned)h[7] << 24;
    if (len >= cap || sread(json, len)) return -1;
    json[len] = 0;
    return 0;
}
static void frame_out(unsigned op, const char *json) {
    unsigned char f[512];
    size_t n = discord_frame(f, sizeof f, op, json);
    swrite(f, (unsigned)n);
}

int main(void) {
    /* ---- the messages */
    unsigned char f[64];
    size_t n = discord_frame(f, sizeof f, 1, "{}");
    CHECK(n == 10 && !memcmp(f, "\x01\0\0\0\x02\0\0\0{}", 10));
    CHECK(discord_frame(f, 9, 1, "{}") == 0);  /* doesn't fit */
    char j[512];
    discord_handshake_json(j, sizeof j, "123");
    CHECK(!strcmp(j, "{\"v\":1,\"client_id\":\"123\"}"));
    DiscordActivity a = {.details = "In a \"battle\"\\\n", .large_image = "icon", .large_text = "YW4", .start = 1700000000};
    discord_activity_json(j, sizeof j, 42, &a, 7);
    CHECK(!strcmp(j, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"details\":\"In a \\\"battle\\\"\\\\\\u000a\","
                     "\"timestamps\":{\"start\":1700000000},\"assets\":{\"large_image\":\"icon\",\"large_text\":\"YW4\"}}},\"nonce\":\"7\"}"));
    discord_activity_json(j, sizeof j, 42, NULL, 8);
    CHECK(!strcmp(j, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":null},\"nonce\":\"8\"}"));
    CHECK(discord_activity_json(j, 20, 42, &a, 7) == 0);  /* doesn't fit */

    /* ---- nobody listening: connect fails, quietly */
    DiscordIpc c;
    CHECK(discord_ipc_connect(&c, "/nonexistent/discord-ipc-0") != 0);

    /* ---- the presence thread against a stand-in Discord */
    char path[128];
    snprintf(path, sizeof path, PATH_FMT, my_pid());
    CHECK(!server_open(path));
    static char env[3][160];
    snprintf(env[0], sizeof env[0], "YW4_DISCORD_IPC_PATH=%s", path);
    putenv(env[0]);
    putenv((char *)"YW4_DISCORD_APP_ID=1234567890");
    putenv((char *)"YW4_DISCORD=1");
    CHECK(discord_enabled() == 1 && discord_disabled() == NULL);
    discord_start();
    CHECK(!server_accept());
    unsigned op;
    char json[2048];
    CHECK(!frame_in(&op, json, sizeof json));
    CHECK(op == 0 && !strcmp(json, "{\"v\":1,\"client_id\":\"1234567890\"}"));
    frame_out(1, "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\",\"data\":{\"v\":1}}");
    CHECK(!frame_in(&op, json, sizeof json));
    CHECK(op == 1 && strstr(json, "\"cmd\":\"SET_ACTIVITY\"") && !strstr(json, "\"details\"")
          && strstr(json, "\"large_text\":\"Yo-kai Watch 4\"") && strstr(json, "\"timestamps\":{\"start\":"));
    printf("activity: %s\n", json);
    server_close();
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
"""


class DiscordTest(unittest.TestCase):
    def test_messages_and_handshake(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            (t / "drv.c").write_text(DRIVER)
            exe = t / "discord_test.exe"
            src = RUNTIME / "src"
            r = subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", *POSIX, f"-I{src}", f"-I{RUNTIME / 'include'}",
                                "-o", str(exe), str(t / "drv.c"), str(src / "discord.c"), str(src / "discord_ipc.c")],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-3000:])
            got = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
            self.assertIn("all passed", got.stdout)


if __name__ == "__main__":
    unittest.main()
