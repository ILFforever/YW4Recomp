/* Discord Rich Presence (discord.h, decision 0077). A thread of its own, once a second: while DISCORD STATUS is on and
 * the Discord app runs on this PC, the profile shows the game (the Discord application's name, its icon) and the time
 * since the game started, over Discord's local IPC (discord_ipc.c). Nothing about the game's state is read: the game's
 * threads never meet this one.
 *
 * Quiet by design: no Discord, no window, no log spam. It tries to connect every 15 s while on, sets the activity once
 * per connection, and clears it when the setting is turned off. */
#include "discord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "discord_ipc.h"
#include "host.h"

#ifdef _WIN32
#include <windows.h>
static long this_pid(void) { return (long)GetCurrentProcessId(); }
#else
#include <unistd.h>
static long this_pid(void) { return (long)getpid(); }
#endif

enum { TICK_MS = 1000, RETRY_S = 15 };

static int enabled = -1;

static const char *app_id(void) {
    const char *e = getenv("YW4_DISCORD_APP_ID");
    return e && *e ? e : YW4_DISCORD_APP_ID;
}

int discord_enabled(void) {
    int on = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
    if (on < 0) {
        const char *e = getenv("YW4_DISCORD");
        on = e && atoi(e) == 1;
        __atomic_store_n(&enabled, on, __ATOMIC_RELEASE);
    }
    return on && *app_id();
}

void discord_toggle(void) {
    int on = !discord_enabled();
    __atomic_store_n(&enabled, on, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: discord: status %s\n", on ? "on" : "off");
}

const char *discord_disabled(void) { return *app_id() ? NULL : "NOT SET UP IN THIS BUILD"; }

/* Connects and says hello: 0 once Discord answered the handshake with READY. */
static int connect_discord(DiscordIpc *c) {
    char json[256], reply[2048];
    uint32_t op;
    if (discord_ipc_connect(c, getenv("YW4_DISCORD_IPC_PATH"))) return -1;  /* the path: tests */
    if (!discord_handshake_json(json, sizeof json, app_id()) || discord_ipc_send(c, DISCORD_OP_HANDSHAKE, json)) {
        discord_ipc_close(c);
        return -1;
    }
    if (discord_ipc_recv(c, &op, reply, sizeof reply, 3000) != 1 || op != DISCORD_OP_FRAME || !strstr(reply, "READY")) {
        discord_ipc_close(c);  /* a wrong ID gets a CLOSE frame here */
        return -1;
    }
    fprintf(stderr, "yw4: discord: connected\n");
    return 0;
}

/* Reads what Discord sent since (its answers to our updates, a ping now and then) so its pipe never fills. */
static void drain(DiscordIpc *c) {
    char buf[1024];
    uint32_t op;
    while (c->open && discord_ipc_recv(c, &op, buf, sizeof buf, 0) == 1) {
        if (op == DISCORD_OP_PING) discord_ipc_send(c, DISCORD_OP_PONG, buf);
        else if (op == DISCORD_OP_CLOSE) discord_ipc_close(c);
    }
}

static void presence_thread(void *arg) {
    (void)arg;
    DiscordIpc c = {0};
    const int64_t start = (int64_t)time(NULL);
    int64_t next_try = 0;
    int shown = 0;  /* the activity is set on this connection */
    unsigned nonce = 0;
    for (;;) {
        host_sleep_ns((int64_t)TICK_MS * 1000000);
        int64_t now = (int64_t)time(NULL);
        if (!discord_enabled()) {
            if (c.open) {  /* turned off: clear the profile's activity, then let go */
                char json[256];
                if (discord_activity_json(json, sizeof json, this_pid(), NULL, ++nonce))
                    discord_ipc_send(&c, DISCORD_OP_FRAME, json);
                discord_ipc_close(&c);
                fprintf(stderr, "yw4: discord: status cleared\n");
            }
            shown = 0;
            continue;
        }
        if (!c.open) {
            shown = 0;
            if (now < next_try) continue;
            next_try = now + RETRY_S;
            if (connect_discord(&c)) continue;
        }
        drain(&c);
        if (!c.open || shown) continue;
        /* the game's name is the application's; "icon" the art uploaded to it (none yet: no picture) */
        DiscordActivity a = {.large_image = "icon", .large_text = "Yo-kai Watch 4", .start = start};
        char json[512];
        if (!discord_activity_json(json, sizeof json, this_pid(), &a, ++nonce)) continue;
        if (discord_ipc_send(&c, DISCORD_OP_FRAME, json)) continue;  /* gone: tried again later */
        shown = 1;
    }
}

void discord_start(void) {
    if (!*app_id()) return;  /* no application: nothing to show, no thread */
    if (host_thread_start(presence_thread, NULL, 1 << 16))
        fprintf(stderr, "yw4: discord: could not start its thread\n");
}
