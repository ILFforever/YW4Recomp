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

enum { TICK_MS = 1000, RETRY_S = 15, UPDATE_S = 5 };

static int enabled = -1;

static const char *app_id(void) {
    const char *e = getenv("YW4_DISCORD_APP_ID");
    return e && *e ? e : YW4_DISCORD_APP_ID;
}

/* On unless the player turns it off (owner, 2026-11-18, for 1.7.0: most games and emulators show it, and Discord's
 * own Activity Privacy hides it for those who don't want it). Off in test runs (YW4_SETTINGS_FILE=none, as the
 * controls card), so they don't post to whoever runs them; YW4_DISCORD=0 / 1 decides either way. */
int discord_enabled(void) {
    int on = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
    if (on < 0) {
        const char *e = getenv("YW4_DISCORD"), *f = getenv("YW4_SETTINGS_FILE");
        int test_run = f && (!*f || !strcmp(f, "none"));
        on = e && *e ? atoi(e) == 1 : !test_run;
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

/* Emoji as UTF-8 bytes (each followed by a space, so no hex escape runs into the text) */
#define E_MAP "\xf0\x9f\x97\xba\xef\xb8\x8f "  /* world map */
#define E_WALK "\xf0\x9f\x9a\xb6 "             /* person walking */
#define E_LOOK "\xf0\x9f\x94\x8d "             /* magnifying glass */
#define E_GHOST "\xf0\x9f\x91\xbb "            /* ghost */
#define E_LEAF "\xf0\x9f\x8d\x83 "             /* leaves in the wind */
#define E_BLOSSOM "\xf0\x9f\x8c\xb8 "          /* cherry blossom */
#define E_TORCH "\xf0\x9f\x94\xa6 "            /* flashlight */
#define E_EYES "\xf0\x9f\x91\x80 "             /* eyes */
#define E_PAWS "\xf0\x9f\x90\xbe "             /* paw prints */
#define E_COMPASS "\xf0\x9f\xa7\xad "          /* compass */
#define E_SPARKLES "\xe2\x9c\xa8 "             /* sparkles */
#define E_RICEBALL "\xf0\x9f\x8d\x99 "         /* rice ball */
#define E_CAMERA "\xf0\x9f\x93\xb8 "           /* camera with flash */
#define E_BIKE "\xf0\x9f\x9a\xb2 "             /* bicycle */
#define E_BACKPACK "\xf0\x9f\x8e\x92 "         /* school backpack */
#define E_QUESTION "\xe2\x9d\x93 "             /* question mark */
#define E_ZANY "\xf0\x9f\xa4\xaa "             /* zany face */
#define E_CHIME "\xf0\x9f\x8e\x90 "            /* wind chime */
#define E_KEY "\xf0\x9f\x97\x9d\xef\xb8\x8f "  /* old key */
#define E_FIST "\xf0\x9f\x91\x8a "             /* oncoming fist */
#define E_BOOM "\xf0\x9f\x92\xa5 "             /* collision */
#define E_SHIELD "\xf0\x9f\x9b\xa1\xef\xb8\x8f "  /* shield */
#define E_FIRE "\xf0\x9f\x94\xa5 "             /* fire */
#define E_ZAP "\xe2\x9a\xa1 "                  /* high voltage */
#define E_SWORDS "\xe2\x9a\x94\xef\xb8\x8f "   /* crossed swords */
#define E_HOME "\xf0\x9f\x8f\xa0 "             /* house */
#define E_WATCH "\xe2\x8c\x9a "                /* watch */

void discord_presence_text(const DiscordGame *g, unsigned flavor, char *details, size_t dn, char *state, size_t sn) {
    static const char ranks[] = "EDCBAS";
    static const char *const field[DISCORD_FLAVORS] = {
        E_MAP "Exploring %s", E_WALK "Wandering around %s", E_LOOK "Searching for Yo-kai in %s",
        E_GHOST "Hunting Yo-kai in %s", E_LEAF "Taking a stroll in %s", E_BLOSSOM "Enjoying the sights in %s",
        E_TORCH "Poking around %s", E_EYES "Looking around %s", E_PAWS "Following Yo-kai tracks in %s",
        E_COMPASS "Finding the way through %s", E_SPARKLES "Sensing Yo-kai in %s", E_RICEBALL "Snacking in %s",
        E_CAMERA "Sightseeing in %s", E_WATCH "Scanning %s with the Watch", E_BIKE "Cruising around %s",
        E_BACKPACK "Adventuring in %s", E_QUESTION "Lost in %s", E_ZANY "Mucking about in %s",
        E_CHIME "Chasing the breeze in %s", E_KEY "Uncovering secrets in %s",
    };
    static const char *const battle[DISCORD_BATTLE_FLAVORS] = {
        E_SWORDS "Battling in %s", E_FIST "Fighting Yo-kai in %s", E_BOOM "In a Yo-kai brawl in %s",
        E_SHIELD "Holding the line in %s", E_FIRE "Turning up the heat in %s", E_ZAP "Unleashing a Soultimate in %s",
    };
    const char *place = g->sub[0] ? g->sub : g->area;  /* the zone when it has a name (the area is the icon's hover) */
    details[0] = state[0] = 0;
    switch (g->where) {
    case DISCORD_AT_TITLE: snprintf(details, dn, E_HOME "In the main menu"); return;
    case DISCORD_AT_BATTLE:
        if (place[0]) snprintf(details, dn, battle[flavor % DISCORD_BATTLE_FLAVORS], place);
        else snprintf(details, dn, E_SWORDS "In a battle");
        break;
    case DISCORD_AT_FIELD:
        if (place[0]) snprintf(details, dn, field[flavor % DISCORD_FLAVORS], place);
        break;
    default: return;  /* starting up: the game's name alone */
    }
    if (g->rank >= 0 && g->rank < (int)sizeof ranks - 1) snprintf(state, sn, E_WATCH "Watch Rank %c", ranks[g->rank]);
}

static void presence_thread(void *arg) {
    (void)arg;
    DiscordIpc c = {0};
    const int64_t start = (int64_t)time(NULL);
    int64_t next_try = 0, sent_at = 0;
    int shown = 0;  /* the activity is set on this connection */
    unsigned nonce = 0;
    char shown_details[160] = "", shown_state[64] = "";
    unsigned flavor = (unsigned)start % DISCORD_FLAVORS;  /* the first wording: any of them */
    unsigned battle_flavor = (unsigned)start % DISCORD_BATTLE_FLAVORS;
    int flavor_where = -1;
    char flavor_area[DISCORD_AREA_MAX] = "";
    uint32_t seed = (uint32_t)start | 1u;  /* our own xorshift: the C library's rand() is left alone */
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
        if (!c.open) continue;
        DiscordGame g;
        discord_game_read(&g);
        /* a new wording for each area (and on coming back to the field), not each update: no flicker */
        const char *place = g.sub[0] ? g.sub : g.area;  /* a new zone gets a new wording too */
        if (g.where != flavor_where || strcmp(place, flavor_area)) {
            if (g.where == DISCORD_AT_FIELD && (flavor_where != DISCORD_AT_BATTLE || strcmp(place, flavor_area))) {
                seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
                flavor = (flavor + 1 + seed % (DISCORD_FLAVORS - 1)) % DISCORD_FLAVORS;  /* never the same twice */
            }
            if (g.where == DISCORD_AT_BATTLE && flavor_where != DISCORD_AT_BATTLE) {  /* each battle its own */
                seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
                battle_flavor = (battle_flavor + 1 + seed % (DISCORD_BATTLE_FLAVORS - 1)) % DISCORD_BATTLE_FLAVORS;
            }
            flavor_where = g.where;
            snprintf(flavor_area, sizeof flavor_area, "%s", place);
        }
        char details[160], state[64];
        discord_presence_text(&g, g.where == DISCORD_AT_BATTLE ? battle_flavor : flavor, details, sizeof details, state,
                              sizeof state);
        if (shown && !strcmp(details, shown_details) && !strcmp(state, shown_state)) continue;
        /* Discord takes 5 updates per 20 s: one per UPDATE_S at most, the newest text (a quick battle in and out
         * shows only once it has lasted) */
        if (shown && now - sent_at < UPDATE_S) continue;
        /* the game's name is the application's; "icon" the art uploaded to it, its hover the area */
        DiscordActivity a = {.details = details, .state = state, .large_image = "icon",
                             .large_text = g.area[0] && g.where != DISCORD_AT_TITLE ? g.area : "Yo-kai Watch 4",
                             .start = start, .button_label = "Get YW4 Recomp",
                             .button_url = "https://ilfforever.github.io/YW4Recomp/"};
        char json[1024];
        if (!discord_activity_json(json, sizeof json, this_pid(), &a, ++nonce)) continue;
        if (discord_ipc_send(&c, DISCORD_OP_FRAME, json)) continue;  /* gone: tried again later */
        shown = 1, sent_at = now;
        snprintf(shown_details, sizeof shown_details, "%s", details);
        snprintf(shown_state, sizeof shown_state, "%s", state);
    }
}

void discord_start(void) {
    if (!*app_id()) return;  /* no application: nothing to show, no thread */
    if (host_thread_start(presence_thread, NULL, 1 << 16))
        fprintf(stderr, "yw4: discord: could not start its thread\n");
}
