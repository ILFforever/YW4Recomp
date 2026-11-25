/* The settings panel's list (settings.h): each row reads its setting's current option and steps it with
 * the setting's own cycle function, so the panel and the replays' keys always agree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_filesystem.h>

#include "events.h"
#include "host.h"
#include "bindings.h"
#include "framerate.h"
#include "gpu_dlss.h"
#include "gpu_dlssg.h"
#include "gpu_fg.h"
#include "gpu_fsr.h"
#include "gpu_xess.h"
#include "post.h"
#include "gbuffer.h"
#include "gpu.h"
#include "hud.h"
#include "cheats.h"
#include "discord.h"
#include "gamemenu.h"
#include "settings.h"
#include "theme.h"
#include "shadow.h"
#include "window.h"
#include "relay.h"

int drawdist_index(void);  /* drawdist.c: DRAW DISTANCE (TEST) */
void drawdist_cycle(void);

typedef struct {
    const char *category, *name, *help;
    const char *options[SETTINGS_MAX_OPTIONS];
    const char *notes[SETTINGS_MAX_OPTIONS];
    int noptions;
    int (*current)(void);  /* index into options */
    void (*next)(void);    /* one step forward, wrapping */
    const char *env;       /* the environment variable it starts from, and its value per option */
    const char *values[SETTINGS_MAX_OPTIONS];
    const char *(*disabled)(void);  /* NULL, or why it can't be used now (greyed out) */
    /* a slider (noptions 0): get/set the value, lo..hi in steps; 0 below lo is AUTO (auto_note says what it does) */
    float (*get)(void);
    void (*set)(float);
    float lo, hi, step;
    const char *auto_note;
    int percent;  /* a slider of 0..1 shown in percent: no AUTO, 0 is a value like any other (auto_note: what it does) */
    float (*parse)(const char *);  /* a slider's saved value as get() returns it (YW4_SET_AT); NULL: atof */
    /* a slider's names for the points along it, evenly spread from end to end (SHARPENING: OFF .. MAX at 0, 25, 50,
     * 75, 100%): the overlay marks them on the track, and the nearest one's name is shown instead of a number */
    const char *marks[SETTINGS_MAX_OPTIONS];
    /* NULL, or 1 when the overlay leaves the row out for now (DLSS MODEL without DLSS: owner, 2026-11-19); the old
     * panel greys it out instead (`disabled`), so a hidden row needs a `disabled` too */
    int (*hidden)(void);
} Setting;

/* A slider's value as shown and as saved. */
static void slider_text(const Setting *s, char *shown, size_t n1, char *saved, size_t n2) {
    float v = s->get();
    if (s->percent && s->marks[0]) {  /* named points (SHARPENING): the nearest one's name is all that shows */
        int n = 0;
        while (n < SETTINGS_MAX_OPTIONS && s->marks[n]) n++;
        float f = (v - s->lo) / (s->hi - s->lo);
        int k = (int)(f * (float)(n - 1) + 0.5f);
        if (shown) snprintf(shown, n1, "%s", s->marks[k < 0 ? 0 : k > n - 1 ? n - 1 : k]);
        if (saved) snprintf(saved, n2, "%.2f", v);
    } else if (s->percent) {
        if (shown) snprintf(shown, n1, "%d%%", (int)(v * 100 + 0.5f));
        if (saved) snprintf(saved, n2, "%.2f", v);
    } else if (v <= 0) {
        if (shown) snprintf(shown, n1, "AUTO");
        if (saved) snprintf(saved, n2, "auto");
    } else {
        if (shown) snprintf(shown, n1, "%.2fX", v);
        if (saved) snprintf(saved, n2, "%.2f", v);
    }
}
static void slider_step(const Setting *s, int dir) {
    float v = s->get();
    if (s->percent) {
        v = (float)(int)((v + dir * s->step) / s->step + 0.5f) * s->step;  /* on the steps */
        s->set(v < s->lo ? s->lo : v > s->hi ? s->hi : v);
        return;
    }
    if (v <= 0) v = dir > 0 ? s->lo : 0;
    else {
        v += dir * s->step;
        v = (float)(int)(v / s->step + 0.5f) * s->step;  /* on the steps */
        if (v < s->lo - s->step * 0.5f) v = 0;          /* below the lowest: AUTO */
        if (v > s->hi) v = s->hi;
    }
    s->set(v);
}

static int res_index(void) {
    int h = gpu_res_choice();  /* the choice (the images follow it a few frames later) */
    return h == 1080 ? 1 : h == 1440 ? 2 : h == 2160 ? 3 : 0;
}
static int aniso_index(void) {
    int a = gpu_aniso();
    return a == 2 ? 1 : a == 4 ? 2 : a == 8 ? 3 : a == 16 ? 4 : 0;
}
static int dof_index(void) { return 2 - gpu_dof(); }  /* 2 the game's, 1 half, 0 off */
static int shadow_res_index(void) {
    int r = gpu_shadow_res();
    return r == 2048 ? 1 : r == 4096 ? 2 : 0;
}
static int pcss_index(void) { return gpu_pcss_level() & 3; }
/* shadow_pass_ours, not shadow_mode_name: the mode is set only when the first shadows are drawn, and before that
 * the name said ORIGINAL, so the row showed GAME and a save then (the first start's, at boot) wrote
 * YW4_SHADOWS=game: every new install got the game's shadow pass, not ours (found 2026-11-16) */
static int shadow_pass_index(void) { return shadow_pass_ours(); }
static int gbuffer_index(void) { return gbuffer_native(); }
static int bloom_index(void) { return bloom_native(); }
static int shadow_mask_index(void) { return shadow_mask_native(); }
static int dof_pass_index(void) { return dof_native(); }
static int blur_fast_index(void) { return blur_fast(); }
static const char *mask_speedup_disabled(void) {
    if (!shadow_mask_native()) return "NEEDS SHADOW MASK SET TO CUSTOM";
    return gpu_pcss_level() ? NULL : "NEEDS SOFT SHADOWS";
}
static const char *contact_disabled(void) { return shadow_mask_native() ? NULL : "NEEDS SHADOW MASK SET TO CUSTOM"; }
static const char *lamp_shadows_disabled(void) {
    if (!shadow_pass_ours()) return "NEEDS SHADOW PASS SET TO CUSTOM";  /* the characters' draws come from it */
    return screen_passes_native() ? NULL : "NEEDS SCREEN PASSES SET TO CUSTOM";
}
/* Why a row is locked when AMD FSR and NVIDIA DLSS are not in the program. On Windows the builder left them out
 * (recomp.build --no-fsr / --no-dlss). On Linux what isn't there can't be built in yet, and "not in this build" read
 * like something had gone wrong (owner, 2026-11-12). Linux has DLSS and FSR upscaling and FSR frame generation (decision
 * 0073), but not NVIDIA's frame generation or Reflex: those come through Streamline, which is Windows only. */
#ifdef _WIN32
#define NOT_BUILT "NOT IN THIS BUILD"
#define NO_STREAMLINE 0
#else
#define NOT_BUILT "NOT SUPPORTED ON LINUX YET"
#define NO_STREAMLINE 1
#endif
static int reflex_index(void) { return dlssg_reflex(); }  /* what runs: on while DLSS frame generation is */
static const char *reflex_disabled(void) {
    if (NO_STREAMLINE || !gpu_dlss_built()) return NOT_BUILT;
    if (!dlssg_reflex_available()) return "NEEDS AN NVIDIA GPU";
    /* locked on, not off: the panels show a reason starting "ALWAYS ON" without their "Off for now" */
    return gpu_fg() == 2 && dlssg_available() ? "ALWAYS ON WITH DLSS FRAME GENERATION. NVIDIA REQUIRES IT" : NULL;
}
/* "NEEDS ANTI-ALIASING SET TO FSR, DLSS OR XESS": the choices this GPU and build can run (TAA first with taa) */
static const char *needs_aa(int taa) {
    const char *names[4];
    int n = 0;
    if (taa) names[n++] = "TAA";
    names[n++] = "FSR";
    if (gpu_dlss_available()) names[n++] = "DLSS";
    if (gpu_xess_available() == 1) names[n++] = "XESS";
    static char why[96];
    int o = snprintf(why, sizeof why, "NEEDS ANTI-ALIASING SET TO ");
    for (int i = 0; i < n && o > 0 && o < (int)sizeof why; i++)
        o += snprintf(why + o, sizeof why - (size_t)o, "%s%s", i == 0 ? "" : i == n - 1 ? " OR " : ", ", names[i]);
    return why;
}
static const char *fg_disabled(void) {
    if (!gpu_fsr_built() && (NO_STREAMLINE || !gpu_dlss_built())) return NOT_BUILT;
    if (!gpu_fsr_built() && !dlssg_available()) return "NOT SUPPORTED ON THIS GPU";
    return gpu_aa() >= 3 ? NULL : needs_aa(1);
}
static const char *sharpen_ui_disabled(void) { return gpu_sharpen() ? NULL : "NEEDS SHARPENING"; }
static float sharpen_parse(const char *v) { return (float)gpu_sharpen_parse(v) / 100.0f; }
static const char *upscale_disabled(void) {
    if (!gpu_fsr_built() && !gpu_dlss_built() && !gpu_xess_built()) return NOT_BUILT;
    if (!gpu_fsr_built() && !gpu_dlss_available() && gpu_xess_available() != 1) return "NEEDS AN NVIDIA RTX GPU FOR DLSS";
    return gpu_aa() >= 4 ? NULL : needs_aa(0);
}
/* DLSS MODEL: only while DLSS itself runs (anti-aliasing DLSS on a GPU that has it; else FSR stands in) */
static int dlss_model_hidden(void) { return gpu_aa() != 5 || !gpu_dlss_available(); }
static const char *dlss_model_disabled(void) {
    if (!gpu_dlss_built()) return NOT_BUILT;
    return dlss_model_hidden() ? "NEEDS ANTI-ALIASING SET TO DLSS" : NULL;
}

/* LEGACY MENUS: the keys from before the in-game overlay. Read once, after settings_load has filled the
 * environment. Only the window's real key presses ask (window.c); a replay's F1 and F2 still open the panel, so
 * the test recordings don't depend on this setting. */
static int legacy_keys = -1;
int settings_legacy_keys(void) {
    if (legacy_keys < 0) {
        const char *e = getenv("YW4_LEGACY_KEYS");
        legacy_keys = e && !strcmp(e, "1");  /* off by default (owner, 2026-11-08) */
    }
    return legacy_keys;
}
static void legacy_keys_toggle(void) { legacy_keys = !settings_legacy_keys(); }

/* The controls hint: the corner card at each start that says how to see the controls (window.c). On by default for
 * a player (a settings file is in use); off in test runs (YW4_SETTINGS_FILE=none), so their pictures stay the
 * game's alone, unless YW4_KEYS_NOTICE=1 asks. The switch is the launcher's (Settings, "Controls hint": it starts
 * the game with YW4_KEYS_NOTICE=0), not a row here (owner, 2026-11-13). */
static const char *settings_file(void);
int settings_keys_notice(void) {
    const char *e = getenv("YW4_KEYS_NOTICE");
    return e && *e ? strcmp(e, "0") != 0 : *settings_file() != 0;
}

/* Online play and Profile (decision 0082; owner's layout, 2026-11-19): the game's Online button goes to our relay
 * server (relay.c), Ark Swap and Blasters each public or private with their own code; the profile is the name the
 * other players see (YW4_NICKNAME, handed to the game by acc's GetProfile, svc_acc.c; none: "Player").
 * Typed values use the overlay's name popup (gamemenu_name_edit) and are saved by settings_save. */
static void put_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
/* an online setting (server, Public / Private, code): an open relay connection outside a room is dropped, so the
 * next request uses it (relay.c) */
static void online_env(const char *name, const char *value) {
    const char *was = getenv(name);
    int same = was && !strcmp(was, value);
    put_env(name, value);
    if (!same) relay_settings_changed();
}
static int env_on(const char *name) {
    const char *v = getenv(name);
    return v && !strcmp(v, "1");
}
static const char *player_name(void) {
    const char *n = getenv("YW4_NICKNAME");
    return n && *n ? n : "Player";
}
static int text_row_index(void) { return 0; }
static void player_name_set(const char *name) {
    put_env("YW4_NICKNAME", name && *name ? name : "Player");
    settings_save();
    fprintf(stderr, "yw4: settings: player name %s\n", player_name());
}
static void player_name_edit(void) { gamemenu_name_edit("Your name when playing together", 10, "Player", player_name(), player_name_set); }

/* the server: ours, by region, by name (relay.c: each name falls back to its built-in address; the names so a server
 * can move without a game update, 2026-11-20). Settings saved before the names hold the address: the same server. */
static const char *const servers[] = {"yw4-us.duckdns.org:41230", "yw4-sg.duckdns.org:41230"};
static const char *const servers_before[] = {"129.153.179.140:41230", "161.118.234.155:41230"};
static int server_index(void) {
    const char *v = getenv("YW4_ONLINE_SERVER");
    for (int i = 0; v && i < 2; i++)
        if (!strcmp(v, servers[i]) || !strcmp(v, servers_before[i])) return i;
    return 0;
}
static void server_cycle(void) { online_env("YW4_ONLINE_SERVER", servers[(server_index() + 1) % 2]); }

/* Public / Private and the code, per activity: private rooms are seen only by players with the same code */
static int swap_private(void) { return env_on("YW4_SWAP_PRIVATE"); }
static void swap_private_toggle(void) { online_env("YW4_SWAP_PRIVATE", swap_private() ? "0" : "1"); }
static int blasters_private(void) { return env_on("YW4_BLASTERS_PRIVATE"); }
static void blasters_private_toggle(void) { online_env("YW4_BLASTERS_PRIVATE", blasters_private() ? "0" : "1"); }
static const char *code_of(const char *env) {
    const char *v = getenv(env);
    return v && *v ? v : "";
}
static void swap_code_set(const char *c) { online_env("YW4_SWAP_CODE", c ? c : ""), settings_save(); }
static void swap_code_edit(void) { gamemenu_name_edit("Ark Swap code", 16, "", code_of("YW4_SWAP_CODE"), swap_code_set); }
static void blasters_code_set(const char *c) { online_env("YW4_BLASTERS_CODE", c ? c : ""), settings_save(); }
static void blasters_code_edit(void) { gamemenu_name_edit("Blasters code", 16, "", code_of("YW4_BLASTERS_CODE"), blasters_code_set); }
static const char *swap_code_off(void) { return swap_private() ? NULL : "SET ARK SWAP TO PRIVATE TO USE A CODE."; }
static const char *blasters_code_off(void) { return blasters_private() ? NULL : "SET BLASTERS TO PRIVATE TO USE A CODE."; }

/* A typed row's settings line and what it shows (NULL: not a typed row) */
static const char *typed_env(const Setting *s) {
    return s->next == player_name_edit ? "YW4_NICKNAME" : s->next == swap_code_edit ? "YW4_SWAP_CODE"
         : s->next == blasters_code_edit ? "YW4_BLASTERS_CODE" : NULL;
}
static const char *typed_shown(const Setting *s) {
    if (s->next == player_name_edit) return player_name();
    const char *c = code_of(typed_env(s));
    return *c ? c : "Not set";
}

/* ---- the overlay's Online tab (gamemenu.c draw_online_page; settings.h) */
static const char *const server_names[] = {"US East", "Singapore"};
static const char *const server_regions[] = {"North America and Europe", "Asia and Oceania"};
int settings_online_servers(void) { return 2; }
const char *settings_online_server_name(int i) { return server_names[i & 1]; }
const char *settings_online_server_region(int i) { return server_regions[i & 1]; }
const char *settings_online_server_address(int i) { return servers[i & 1]; }
int settings_online_server(void) { return server_index(); }
void settings_online_server_set(int i) { online_env("YW4_ONLINE_SERVER", servers[i & 1]), settings_save(); }
int settings_online_private(int blasters) { return blasters ? blasters_private() : swap_private(); }
void settings_online_private_set(int blasters, int on) {
    online_env(blasters ? "YW4_BLASTERS_PRIVATE" : "YW4_SWAP_PRIVATE", on ? "1" : "0");
    settings_save();
}
const char *settings_online_code(int blasters) { return code_of(blasters ? "YW4_BLASTERS_CODE" : "YW4_SWAP_CODE"); }
void settings_online_code_edit(int blasters) { (blasters ? blasters_code_edit : swap_code_edit)(); }
const char *settings_player_name(void) { return player_name(); }
void settings_player_name_edit(void) { player_name_edit(); }

/* ---- Display > Graphics card (window.h): Auto, then each card that can draw to the window. Kept in YW4_GPU ("auto"
 * or the card's full name; any part of a name works by hand); the game picks its card when Vulkan starts, so a change
 * applies at the next start. The row shows short names ("RTX 4070 Laptop GPU", "Radeon 780M"), the file the full one. */
enum { VK_NAME_MAX = 256 };  /* VK_MAX_PHYSICAL_DEVICE_NAME_SIZE */
static char gpu_shown[SETTINGS_MAX_OPTIONS][64], gpu_saved[SETTINGS_MAX_OPTIONS][VK_NAME_MAX];

static const char *gpu_choice(void) {
    const char *e = getenv("YW4_GPU");
    return e && *e ? e : "auto";
}

static int gpu_card_index(void) {
    const char *c = gpu_choice();
    if (window_gpu_is_auto(c)) return 0;
    int i = window_gpu_find(c);
    return i < 0 || i + 1 >= SETTINGS_MAX_OPTIONS ? 0 : i + 1;
}

static int gpu_card_options(void) {
    int n = window_gpu_count() + 1;
    return n > SETTINGS_MAX_OPTIONS ? SETTINGS_MAX_OPTIONS : n;
}

/* A card's name without its maker's words: "NVIDIA GeForce RTX 4070 Laptop GPU" -> "RTX 4070 Laptop GPU",
 * "AMD Radeon 780M Graphics" -> "Radeon 780M", "Intel(R) Arc(TM) Graphics" -> "Arc Graphics". */
static void gpu_short_name(const char *full, char *out, size_t n) {
    static const char *const drop[] = {"NVIDIA ", "GeForce ", "AMD ", "Intel(R) ", "(TM)", "(R)", " Graphics"};
    char s[VK_NAME_MAX];
    snprintf(s, sizeof s, "%s", full);
    for (size_t d = 0; d < sizeof drop / sizeof *drop; d++) {
        if (!strcmp(drop[d], " Graphics") && strstr(s, "Arc")) continue;  /* "Arc Graphics" is the name */
        char *p;
        while ((p = strstr(s, drop[d]))) memmove(p, p + strlen(drop[d]), strlen(p + strlen(drop[d])) + 1);
    }
    snprintf(out, n, "%s", *s ? s : full);
}

static void gpu_card_set(int k) {
    int n = gpu_card_options();
    k = ((k % n) + n) % n;
    const char *v = k ? window_gpu_name(k - 1) : "auto";
#ifdef _WIN32
    _putenv_s("YW4_GPU", v);
#else
    setenv("YW4_GPU", v, 1);
#endif
    fprintf(stderr, "yw4: settings: graphics card %s (from the next start)\n", v);
}
static void gpu_card_cycle(void) { gpu_card_set(gpu_card_index() + 1); }
static void gpu_card_step(int dir) { gpu_card_set(gpu_card_index() + dir); }
static const char *gpu_card_disabled(void) { return window_gpu_count() < 2 ? "THIS PC HAS ONE GRAPHICS CARD" : NULL; }

/* HUD LAYOUT changes nothing at 16:9 and 16:10 (the HUD fills the width there). */
static const char *hud_edges_disabled(void) {
    uint32_t num, den;
    gpu_wide_factor(&num, &den);
    return num > den ? NULL : "ONLY FOR SHAPES WIDER THAN 16:9";
}

/* The row's names and notes from the cards found (fill_rows): Auto says which card it picks. */
static void gpu_card_fill(SettingRow *r) {
    int n = gpu_card_options(), used = window_gpu_used(), autopick = window_gpu_auto();
    char name[64];
    for (int k = 0; k < n; k++) {
        int card = k ? k - 1 : autopick;
        gpu_short_name(window_gpu_name(card), name, sizeof name);
        if (k) snprintf(gpu_shown[k], sizeof gpu_shown[k], "%s", name);
        else if (autopick >= 0) snprintf(gpu_shown[0], sizeof gpu_shown[0], "Auto (%s)", name);
        else snprintf(gpu_shown[0], sizeof gpu_shown[0], "Auto");
        snprintf(gpu_saved[k], sizeof gpu_saved[k], "%s", k ? window_gpu_name(k - 1) : "auto");
        r->options[k] = gpu_shown[k];
        r->notes[k] = card == used ? "THE ONE IN USE" : "TAKES EFFECT AFTER A RESTART";
    }
    r->noptions = n, r->current = gpu_card_index(), r->verbatim = 1;
}

/* The player's settings, in the order shown; a row starts a new group when its category differs from the row
 * before. The last row opens the developer page. The texts use the HUD font's characters (hud.c): upper case,
 * digits, . , : % / - ( ) ' ! ? */
static const Setting player[] = {
    /* Online play (owner, 2026-11-19: sections, a Public / Private switch and a code for each): the server and who
     * sees your rooms when you press the game's Online button; Profile: what the others see of you. Legacy menus
     * went back to Advanced. */
    {"ONLINE PLAY", "SERVER",
     "OUR SERVER FOR THE GAME'S ONLINE BUTTON (ARK SWAP AND BLASTERS). PICK THE ONE NEAREST YOU: PLAYERS ONLY MEET "
     "OTHERS ON THE SAME SERVER.",
     {"US EAST", "SINGAPORE"}, {"NORTH AMERICA AND EUROPE", "ASIA AND OCEANIA"}, 2, server_index, server_cycle,
     "YW4_ONLINE_SERVER", {"yw4-us.duckdns.org:41230", "yw4-sg.duckdns.org:41230"}},
    {"ONLINE PLAY", "ARK SWAP",
     "WHO SEES YOUR ARK SWAP ROOM WHEN YOU PRESS ONLINE. PUBLIC: EVERYONE ON THE SERVER. PRIVATE: ONLY PLAYERS WITH "
     "THE SAME ARK SWAP CODE.",
     {"PUBLIC", "PRIVATE"}, {"EVERYONE ON THE SERVER", "ONLY PLAYERS WITH YOUR CODE"}, 2, swap_private,
     swap_private_toggle, "YW4_SWAP_PRIVATE", {"0", "1"}},
    {"ONLINE PLAY", "ARK SWAP CODE",
     "THE CODE FOR PRIVATE ARK SWAP: AGREE ON ONE WITH YOUR FRIENDS. UP TO 16 LETTERS. ENTER OR A: CHANGE IT.",
     {"NOT SET"}, {""}, 1, text_row_index, swap_code_edit, "YW4_SWAP_CODE", {""}, swap_code_off},
    {"ONLINE PLAY", "BLASTERS",
     "WHO SEES YOUR BLASTERS ROOM WHEN YOU PRESS ONLINE. PUBLIC: EVERYONE ON THE SERVER. PRIVATE: ONLY PLAYERS WITH "
     "THE SAME BLASTERS CODE.",
     {"PUBLIC", "PRIVATE"}, {"EVERYONE ON THE SERVER", "ONLY PLAYERS WITH YOUR CODE"}, 2, blasters_private,
     blasters_private_toggle, "YW4_BLASTERS_PRIVATE", {"0", "1"}},
    {"ONLINE PLAY", "BLASTERS CODE",
     "THE CODE FOR PRIVATE BLASTERS: AGREE ON ONE WITH YOUR FRIENDS. UP TO 16 LETTERS. ENTER OR A: CHANGE IT.",
     {"NOT SET"}, {""}, 1, text_row_index, blasters_code_edit, "YW4_BLASTERS_CODE", {""}, blasters_code_off},
    {"PROFILE", "PLAYER NAME",
     "YOUR USERNAME SHOWN TO OTHER PLAYERS WHEN YOU PLAY TOGETHER ONLINE: TRADES AND BLASTERS. SUPPORTS UP TO "
     "10 LETTERS. NAMES ARE ONLY SEEN BY THE PLAYERS IN YOUR ROOM. ENTER OR A: CHANGE IT.",
     {"PLAYER"}, {""}, 1, text_row_index, player_name_edit, "YW4_NICKNAME", {""}},
    {"PROFILE", "DISCORD STATUS",
     /* (decision 0077: Discord's local IPC, the Discord app on this PC; nothing goes anywhere else) */
     "SHOWS ON YOUR DISCORD PROFILE THAT YOU ARE PLAYING YO-KAI WATCH 4, WHERE YOU ARE (THE MAIN MENU, THE AREA, A "
     "BATTLE), YOUR WATCH RANK AND HOW LONG YOU HAVE PLAYED. NEEDS THE DISCORD APP OPEN ON THIS PC. OFF: NOTHING IS "
     "SENT.",
     {"OFF", "ON"},
     {"NOTHING SHOWN ON DISCORD", "SHOWN ON YOUR DISCORD PROFILE"}, 2,
     discord_enabled, discord_toggle, "YW4_DISCORD", {"0", "1"}, discord_disabled},
    {"DISPLAY", "GRAPHICS CARD",
     "THE GRAPHICS CARD THE GAME RUNS ON, FOR PCS WITH TWO (MOST GAMING LAPTOPS). AUTO PICKS THE FASTER ONE. THE "
     "GAME CHOOSES IT WHEN IT STARTS, SO A CHANGE TAKES EFFECT AFTER A RESTART.",
     {gpu_shown[0], gpu_shown[1], gpu_shown[2], gpu_shown[3], gpu_shown[4], gpu_shown[5], gpu_shown[6], gpu_shown[7]},
     {NULL}, SETTINGS_MAX_OPTIONS, gpu_card_index, gpu_card_cycle, "YW4_GPU",
     {gpu_saved[0], gpu_saved[1], gpu_saved[2], gpu_saved[3], gpu_saved[4], gpu_saved[5], gpu_saved[6], gpu_saved[7]},
     gpu_card_disabled},
    {"DISPLAY", "WINDOW MODE",
     "A WINDOW, OR THE WHOLE SCREEN WITHOUT BORDERS. THE MONITOR'S OWN MODE NEVER CHANGES, SO ALT+TAB IS INSTANT. "
     "F11 SWITCHES TOO.",
     {"WINDOWED", "FULL SCREEN"}, {"MOVE AND RESIZE IT", "FILLS THE SCREEN"}, 2, window_fullscreen_index,
     window_fullscreen_cycle, "YW4_WINDOW", {"windowed", "fullscreen"}},
    {"DISPLAY", "RESOLUTION",
     "THE RESOLUTION THE WORLD IS DRAWN AT. HIGHER IS SHARPER AND COSTS GPU TIME. MENUS AND TEXT ARE DRAWN AT THE "
     "SAME SIZE.",
     {"1600X900", "1920X1080", "2560X1440", "3840X2160"},
     {"THE SWITCH'S DEFAULT", "FULL HD", "SHARP, A LITTLE GPU TIME", "SHARPEST, THE MOST GPU TIME"}, 4,
     res_index, gpu_res_cycle, "YW4_RES", {"900", "1080", "1440", "2160"}},
    {"DISPLAY", "ASPECT RATIO",
     "WIDER THAN THE SWITCH'S 16:9 FOR WIDE MONITORS AND PHONES (STREAMING): THE WORLD SHOWS MORE AT THE SIDES, THE "
     "MENUS STAY 16:9 IN THE MIDDLE.",
     {"16:9", "16:10", "18:9", "19:9", "19.5:9", "20:9", "21:9", "32:9"},
     {"THE SWITCH'S DEFAULT", "A LITTLE TALLER: LESS AT THE SIDES", "OLDER PHONES (2017-2018)",
      "SOME PHONES (2018-2019)", "IPHONES AND GALAXY S PHONES", "MOST OTHER ANDROID PHONES", "ULTRAWIDE",
      "SUPER ULTRAWIDE"},
     8, gpu_wide_choice, gpu_wide_cycle, "YW4_ASPECT",
     {"16:9", "16:10", "18:9", "19:9", "19.5:9", "20:9", "21:9", "32:9"}},
    {"DISPLAY", "HUD LAYOUT",
     "WHERE THE GAME'S HUD SITS ON A SCREEN WIDER THAN 16:9 WHILE YOU EXPLORE: THE SHORTCUTS AND MINIMAP AT THE "
     "SCREEN'S EDGES, OR IN THE 16:9 MIDDLE. MENUS AND BATTLES STAY IN THE MIDDLE.",
     {"CENTRED", "SCREEN EDGES"}, {"AS ON THE SWITCH", "SHORTCUTS LEFT, MINIMAP RIGHT"}, 2, gpu_hud_edges,
     gpu_hud_edges_cycle, "YW4_HUD_EDGES", {"0", "1"}, hud_edges_disabled},
    {"DISPLAY", "FRAME RATE",
     "HOW MANY PICTURES A SECOND. THE GAME MOVES BY REAL TIME, SO IT PLAYS AT THE SAME SPEED AT ANY RATE. HIGHER "
     "COSTS MORE GPU AND CPU TIME.",
     {"30", "60", "120", "144", "VSYNC", "UNLIMITED"},
     {"THE SWITCH'S DEFAULT", "SMOOTH", "FOR 120 HZ MONITORS", "FOR 144 HZ MONITORS",
      "THE MONITOR'S OWN REFRESH RATE, SMOOTHEST (APPLIES AFTER A RESTART)", "AS FAST AS THE PC CAN: MOTION CAN JUDDER"}, 6,
     framerate_index, framerate_cycle, "YW4_FPS", {"30", "60", "120", "144", "vsync", "0"}},
    {"DISPLAY", "HUD SCALE",
     "HOW BIG THE PERFORMANCE HUD (F3) IS IN THE WINDOW'S CORNER, IN STEPS OF 0.05. ALL THE WAY LEFT: AUTO.",
     {NULL}, {NULL}, 0, NULL, NULL, "YW4_HUD_SCALE", {NULL}, NULL, hud_scale_get, hud_scale_set, 0.75f, 3.0f, 0.05f,
     "GROWS WITH THE WINDOW"},
    {"DISPLAY", "HUD OPACITY",
     "HOW SOLID THE BACKGROUND OF THE PERFORMANCE HUD (F3) IS. THE NUMBERS STAY SOLID. ALL THE WAY LEFT: ONLY THE NUMBERS.",
     {NULL}, {NULL}, 0, NULL, NULL, "YW4_HUD_OPACITY", {NULL}, NULL, hud_opacity_get, hud_opacity_set, 0.0f, 1.0f, 0.05f,
     "ONLY THE NUMBERS", 1},
    {"ANTI-ALIASING AND UPSCALING", "ANTI-ALIASING",
     "SMOOTHS THE JAGGED EDGES OF OBJECTS. APPLIED BEFORE THE MENUS ARE DRAWN, SO TEXT STAYS SHARP.",
     {"OFF", "FXAA", "SMAA", "TAA", "FSR", "DLSS", "XESS"},
     {"THE SWITCH'S DEFAULT: JAGGED EDGES", "SMOOTHEST, TEXTURES A LITTLE SOFTER", "CLEAN EDGES, TEXTURES STAY SHARP",
      "STEADY EDGES: NO SHIMMER ON FENCES, LEAVES AND SHADOWS", "AMD FSR: STEADY EDGES, BEST WITH UPSCALING",
      "NVIDIA DLSS: AI UPSCALING, NVIDIA RTX GPUS",
      "INTEL XESS: AI UPSCALING, ANY GPU, FASTEST ON INTEL ARC"}, 7, gpu_aa,
     gpu_aa_cycle, "YW4_AA", {"0", "1", "2", "3", "4", "5", "6"}},
    {"ANTI-ALIASING AND UPSCALING", "DLSS MODEL",
     /* (decision 0040: K, or M with no trail behind characters in the night rain; shown only with DLSS) */
     "WHICH NVIDIA DLSS MODEL REBUILDS THE PICTURE. HIGH QUALITY REMOVES THE TRAILS BEHIND MOVING CHARACTERS IN "
     "THE RAIN, BUT COSTS MORE GPU TIME: PAIR IT WITH UPSCALING SET TO QUALITY.",
     {"STANDARD", "HIGH QUALITY"},
     {"FASTER (RECOMMENDED)", "NO TRAILS IN THE RAIN, FEWER FRAMES"}, 2,
     gpu_dlss_model_setting, gpu_dlss_model_cycle, "YW4_DLSS_MODEL", {"k", "m"}, dlss_model_disabled,
     NULL, NULL, 0, 0, 0, NULL, 0, NULL, {NULL}, dlss_model_hidden},
    {"ANTI-ALIASING AND UPSCALING", "UPSCALING",
     "DRAWS THE WORLD SMALLER THAN THE RESOLUTION, THEN AMD FSR, NVIDIA DLSS OR INTEL XESS REBUILDS IT AT FULL SIZE: "
     "MORE FRAMES. MENUS AND TEXT STAY FULL SIZE. NEEDS ANTI-ALIASING SET TO FSR, DLSS OR XESS.",
     {"OFF (NATIVE)", "QUALITY", "BALANCED", "PERFORMANCE"},
     {"FULL RESOLUTION: THE UPSCALER ONLY SMOOTHS EDGES", "1.5X SMALLER: CLOSE TO FULL, FASTER", "1.7X SMALLER: FASTER STILL",
      "2X SMALLER: FASTEST, SOFTER"}, 4,
     gpu_upscale_setting, gpu_upscale_cycle, "YW4_UPSCALE", {"0", "1", "2", "3"}, upscale_disabled},
    {"ANTI-ALIASING AND UPSCALING", "SHARPENING",
     /* (decision 0076: TAA's and DLSS's last pass, FSR's own sharpener, a pass of its own with the others; a slider
      * since issue #11, the five steps it replaced marked along it) */
     "MAKES THE WORLD CRISPER: SMOOTHING EDGES AND UPSCALING SOFTEN IT A LITTLE. WORKS WITH EVERY ANTI-ALIASING. "
     "MEDIUM IS RECOMMENDED; AT HIGH AND MAX FINE DETAIL CAN LOOK GRAINY. MENUS AND TEXT ARE NOT CHANGED.",
     {NULL}, {NULL}, 0, NULL, NULL, "YW4_SHARPEN", {NULL}, NULL, gpu_sharpen_get, gpu_sharpen_set, 0.0f, 1.0f, 0.25f,
     "AS THE ANTI-ALIASING LEAVES IT", 1, sharpen_parse, {"OFF", "LOW", "MEDIUM", "HIGH", "MAX"}},
    {"ANTI-ALIASING AND UPSCALING", "SHARPEN TEXT AND MENUS",
     /* (decision 0076: at the present, only the pixels the game's UI changed; our overlay is never sharpened) */
     "SHARPENS THE GAME'S MENUS, TEXT AND HUD TOO, AS MUCH AS SHARPENING DOES THE WORLD. HELPS AT LOW RESOLUTIONS. "
     "THIS OVERLAY IS NOT CHANGED.",
     {"OFF", "ON"},
     {"ONLY THE WORLD IS SHARPENED", "MENUS, TEXT AND HUD TOO"}, 2,
     gpu_sharpen_ui, gpu_sharpen_ui_toggle, "YW4_SHARPEN_UI", {"0", "1"}, sharpen_ui_disabled},
    {"FRAME GENERATION AND LATENCY", "FRAME GENERATION",
     "MAKES A PICTURE BETWEEN EACH TWO OF THE GAME'S: ABOUT TWICE AS MANY SHOWN. ADDS A LITTLE INPUT DELAY: BEST "
     "WHEN THE GAME ALREADY RUNS AT 60 OR MORE. FRAME RATE STILL LIMITS THE GAME'S OWN PICTURES. NEEDS "
     "ANTI-ALIASING SET TO TAA, FSR OR DLSS.",
     {"OFF", "FSR", "DLSS"},
     {"ONE PICTURE PER GAME FRAME", "AMD FSR: TWICE THE PICTURES SHOWN, ANY GPU",
      "NVIDIA DLSS: RTX 40 AND NEWER, TURNS REFLEX ON"}, 3, gpu_fg, gpu_fg_cycle,
     "YW4_FG", {"0", "1", "2"}, fg_disabled},
    {"FRAME GENERATION AND LATENCY", "REFLEX",
     /* (the game waits so each frame starts as late as it can; where the CPU limits, about 14% fewer frames in a busy
      * town) */
     "NVIDIA REFLEX: LESS INPUT DELAY. COSTS A LITTLE FRAME RATE WHERE THE CPU LIMITS IT. NVIDIA GPUS.",
     {"OFF", "ON", "ON + BOOST"},
     {"THE GAME'S OWN TIMING", "LESS INPUT DELAY", "LESS DELAY STILL: THE GPU KEPT AT HIGH CLOCKS, MORE POWER"}, 3,
     reflex_index, dlssg_reflex_cycle, "YW4_REFLEX", {"0", "1", "2"}, reflex_disabled},
    {"DETAIL", "TEXTURE FILTERING",
     "KEEPS GROUND, ROADS AND WALLS SHARP WHEN SEEN FROM AFAR OR AT AN ANGLE.",
     {"OFF", "X2", "X4", "X8", "X16"},
     {"THE SWITCH'S DEFAULT: BLURRY IN THE DISTANCE", "A LITTLE SHARPER", "SHARPER", "SHARPER STILL", "SHARPEST, NO REAL COST"},
     5, aniso_index, gpu_aniso_cycle, "YW4_ANISO", {"0", "2", "4", "8", "16"}},
    {"DETAIL", "EFFECTS RESOLUTION",
     "SHADOWS, LAMP LIGHT AND SHADING ARE DRAWN AT HALF SIZE, THEN SPREAD OVER THE PICTURE.",
     {"HALF", "FULL"},
     {"THE SWITCH'S DEFAULT: SOFTER, BLOCKY EDGES", "CLEANER SHADOW EDGES, ABOUT 2 MS"}, 2, gpu_effects_full,
     gpu_effects_cycle, "YW4_EFFECTS", {"0", "1"}},
    {"DETAIL", "DISTANCE BLUR",
     "THE GAME BLURS FAR AWAY THINGS ON PURPOSE, FOR A SOFT, MINIATURE LOOK.",
     {"FULL", "HALF", "OFF"},
     {"THE SWITCH'S DEFAULT", "A LIGHTER BLUR", "EVERYTHING SHARP"}, 3, dof_index, gpu_dof_cycle, "YW4_DOF", {"2", "1", "0"}},
    {"DETAIL", "DRAW DISTANCE",
     /* (drawdist.c: the map parts' far clip and the map's build radius, times this; the owner's walk on an RTX 4070
      * laptop, 2026-11-20: 52.7 / 51.7 / 52.9 fps, no slow frames: free) */
     "HOW FAR AWAY LAMPS, PLANTS, SIGNS AND OTHER SMALL THINGS ARE DRAWN. THE SWITCH STOPS AT A SHORT DISTANCE, SO THEY "
     "POP IN AS YOU WALK UP. NO NOTICEABLE COST.",
     {"DEFAULT", "MEDIUM", "FAR", "VERY FAR"},
     {"THE SWITCH'S DEFAULT: THEY POP IN CLOSE BY", "TWICE AS FAR", "FOUR TIMES AS FAR (RECOMMENDED)",
      "TEN TIMES AS FAR"}, 4,
     drawdist_index, drawdist_cycle, "YW4_DRAW_DISTANCE", {"1", "2", "4", "10"}},
    {"SHADOWS", "SHADOW MAP",
     "HOW DETAILED SHADOWS ARE. BIGGER IS SHARPER, MOST OF ALL FAR FROM THE CAMERA.",
     {"1024", "2048", "4096"},
     {"THE SWITCH'S DEFAULT", "SHARPER", "SHARPEST (RECOMMENDED)"}, 3, shadow_res_index, gpu_shadow_res_cycle, "YW4_SHADOW_RES", {"1024", "2048", "4096"}},
    {"SHADOWS", "SOFT SHADOWS",
     "SHADOWS GET SOFTER THE FURTHER THEY FALL FROM WHAT CASTS THEM, LIKE REAL ONES. TREE SHADE STOPS "
     "SHIMMERING.",
     {"OFF", "X1", "X2", "X4"},
     {"THE SWITCH'S DEFAULT: HARD SHADOWS", "SLIGHTLY SOFT", "SOFT (RECOMMENDED)", "VERY SOFT"}, 4, pcss_index, gpu_pcss_cycle, "YW4_PCSS", {"0", "1", "2", "3"}},
    {"SHADOWS", "CONTACT SHADOWS",
     /* (the shadow map is too coarse for these; worked out from the picture, about 0.85 ms at 4K) */
     "SMALL SHADOWS WHERE THINGS TOUCH: INSIDE BUSHES AND TREES, UNDER LEDGES AND EAVES. ALMOST NO COST.",
     {"OFF", "ON"},
     {"THE SWITCH'S DEFAULT", "MORE DEPTH IN LEAVES AND UNDER EDGES"}, 2, gpu_contact,
     gpu_contact_toggle, "YW4_CONTACT", {"0", "1"}, contact_disabled},
    {"SHADOWS", "POINT LIGHT SHADOWS",
     /* (the game's lamp lights cast none; shadow maps of the characters for the 6 lit lamps nearest them, decision
      * 0081; the levels: how much of the lamp's light a shadow takes away, 0.25 / 0.4 / 0.6 / 1) */
     "EXPERIMENTAL: AT NIGHT, PEOPLE AND YO-KAI CAST SHADOWS FROM THE LIGHTS NEAREST THEM: STREET LAMPS, PORCH "
     "LIGHTS, LIT WINDOWS.",
     {"OFF", "LOW", "MEDIUM", "HIGH", "MAX"},
     {"THE SWITCH'S DEFAULT: LIGHTS CAST NO SHADOWS", "FAINT SHADOWS", "SOFT SHADOWS (RECOMMENDED)", "DARK SHADOWS",
      "DARKEST SHADOWS"}, 5, gpu_lamp_shadow_level,
     gpu_lamp_shadow_cycle, "YW4_LAMP_SHADOWS", {"0", "0.25", "0.4", "0.6", "1"}, lamp_shadows_disabled},
    /* Advanced: before the developer page's rows (owner, 2026-11-19: back from General) */
    {"ADVANCED", "LEGACY MENUS",
     "THE OLD KEYS: F1 OPENS THE OLD SETTINGS PANEL AND ITS DEVELOPER OPTIONS, F2 THE OLD CONTROLS PAGE, F3 SHOWS OR "
     "HIDES THE PERFORMANCE HUD. OFF, F1 AND F2 OPEN THIS MENU'S SETTINGS AND CONTROLS PAGES (IT HAS ALL OF IT, THE "
     "RENDERER SWITCHES AND SPEED-UPS UNDER ADVANCED) AND F3 STEPS THE PERFORMANCE HUD: OFF, COMPACT, DETAILED.",
     {"OFF", "ON"},
     {"F1 AND F2 OPEN THIS MENU'S PAGES, F3 THE HUD", "F1, F2 AND F3 OPEN THE OLD MENUS"}, 2, settings_legacy_keys, legacy_keys_toggle,
     "YW4_LEGACY_KEYS", {"0", "1"}},
};
/* The developer page (DEVELOPER OPTIONS): behind-the-scenes switches for testing the port, which the picture
 * doesn't show: who draws each part of it, and speed-ups that look the same. The defaults are the recommended
 * ones. */
static const Setting developer[] = {
    {"RENDERER", "SHADOW PASS",
     "WHO DRAWS WHAT CASTS SHADOWS: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER, FASTER (RECOMMENDED)"}, 2, shadow_pass_index, shadow_toggle, "YW4_SHADOWS", {"game", "native"}},
    {"RENDERER", "G-BUFFER",
     "WHO DRAWS THE WORLD'S SURFACES: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME. OURS LETS FUTURE "
     "UPGRADES IN.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER, FASTER (RECOMMENDED)"}, 2, gbuffer_index, gbuffer_toggle, "YW4_GBUFFER", {"game", "native"}},
    {"RENDERER", "BLOOM",
     "WHO DRAWS THE GLOW AROUND BRIGHT THINGS: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME. OURS LETS "
     "FUTURE UPGRADES IN.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER (RECOMMENDED)"}, 2, bloom_index, bloom_toggle, "YW4_BLOOM", {"game", "native"}},
    {"RENDERER", "DISTANCE BLUR PASS",
     "WHO DRAWS THE BLUR OF FAR-AWAY THINGS: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME. OURS LETS "
     "FUTURE UPGRADES IN.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER (RECOMMENDED)"}, 2, dof_pass_index, dof_toggle, "YW4_DOF_PASS",
     {"game", "native"}},
    {"RENDERER", "SHADOW MASK",
     "WHO WORKS OUT WHERE THE SUN'S SHADOWS FALL ON SCREEN: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE "
     "SAME. OURS LETS FUTURE UPGRADES IN.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER (RECOMMENDED)"}, 2, shadow_mask_index, shadow_mask_toggle,
     "YW4_SHADOW_MASK", {"game", "native"}},
    {"RENDERER", "SCREEN PASSES",
     "WHO WORKS OUT THE LIGHTING STEPS ON SCREEN: LAMP LIGHTS, SKY AND FOG, TONE MAPPING, THE GAME'S OWN "
     "ANTI-ALIASING AND THE COPIES BETWEEN THEM. THE PICTURE IS THE SAME. OURS LETS FUTURE UPGRADES IN.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER (RECOMMENDED)"}, 2, screen_passes_native, screen_passes_toggle,
     "YW4_SCREEN_PASSES", {"game", "native"}},
    {"SPEED-UPS", "BATTLE BLUR",
     "AS A BATTLE STARTS THE GAME BLURS THE PICTURE WITH UP TO 17 SMALL BLURS IN A ROW. FAST GETS THE SAME WIDTH "
     "WITH A QUARTER AS MANY, WIDER ONES: ABOUT 3 MS LESS GPU TIME THEN, THE SAME LOOK.",
     {"GAME'S", "FAST"},
     {"THE GAME'S ROUNDS", "A QUARTER OF THE ROUNDS (RECOMMENDED)"}, 2, blur_fast_index, blur_fast_toggle, "YW4_BLUR_FAST", {"0", "1"}},
    {"SPEED-UPS", "SOFT SHADOW FILTER",
     "THE GAME LOOKS UP EACH PIXEL'S SHADOW AT FIVE POINTS CLOSE TOGETHER. WITH SOFT SHADOWS EACH LOOKUP IS A WIDE "
     "16-SAMPLE FILTER, SO ONE AT THE CENTRE COVERS THE SAME GROUND: ABOUT 1 MS LESS GPU TIME AT 4K.",
     {"GAME'S", "FAST"},
     {"FIVE FILTERED LOOKUPS PER PIXEL", "ONE FILTERED LOOKUP PER PIXEL (RECOMMENDED)"}, 2, gpu_mask_filter_once,
     gpu_mask_filter_toggle, "YW4_MASK_FILTER", {"five", "once"}, mask_speedup_disabled},
    {"SPEED-UPS", "SOFT SHADOW SAMPLES",
     "HOW MANY SAMPLES MAKE EACH SOFT SHADOW EDGE. 8 TURNS THEM EACH FRAME SO THE ANTI-ALIASING (TAA, FSR, DLSS) "
     "AVERAGES THEM OVER FRAMES: CHEAPER, MAY SHIMMER A LITTLE IN MOTION. WITHOUT THAT ANTI-ALIASING EDGES GET GRAINY.",
     {"16", "8"},
     {"16 SAMPLES", "8 SAMPLES, TURNED EACH FRAME"}, 2, gpu_pcss_filter8, gpu_pcss_filter8_toggle, "YW4_PCSS_FILTER_TAPS",
     {"16", "8"}, mask_speedup_disabled},
};
/* The overlay's Cheats page (gamemenu.c; owner, 2026-11-15, canvas https://claude.ai/artifact/XcwzdE1gju4aNQJgxgE2e5
 * option B): its own page in the side menu, never on the Settings page or the old panel; hidden, and every cheat off,
 * with the launcher's "Cheats" off (YW4_CHEATS=0, cheats.h). Saved in the settings file like the rest. */
static const Setting cheats[] = {
    {"CHEATS", "UNLIMITED CRANK-A-KAI",
     "THE DAY'S CRANKS NEVER GO DOWN, SO YOU CAN CRANK AS LONG AS YOU HAVE COINS. EACH CRANK STILL COSTS A COIN "
     "UNLESS UNLIMITED COINS IS ON. IT TAKES EFFECT AT ONCE. OFF: THE GAME'S OWN DAILY LIMIT.",
     {"OFF", "ON"},
     {"THE GAME'S OWN DAILY LIMIT", "CRANKS NEVER RUN OUT"}, 2, cheat_crank_get, cheat_crank_toggle,
     "YW4_CHEAT_CRANK", {"0", "1"}},
    {"CHEATS", "UNLIMITED COINS",
     "CRANKING NEVER USES UP A COIN: EVERY CRANK-A-KAI COIN AND ONI COIN STAYS IN YOUR BAG. ONLY THE DAY'S CRANKS "
     "LIMIT YOU (UNLESS UNLIMITED CRANK-A-KAI IS ON). IT TAKES EFFECT AT ONCE. OFF: EACH CRANK COSTS A COIN.",
     {"OFF", "ON"},
     {"EACH CRANK COSTS A COIN", "COINS NEVER RUN OUT"}, 2, cheat_coins_get, cheat_coins_toggle,
     "YW4_CHEAT_COINS", {"0", "1"}},
};
enum { NPLAYER = sizeof player / sizeof *player, NDEVELOPER = sizeof developer / sizeof *developer,
       NCHEATS = sizeof cheats / sizeof *cheats, NSETTINGS = NPLAYER + NDEVELOPER + NCHEATS, LINK_ROW = NPLAYER };

/* Every setting, both pages and the cheats: for the settings file. */
static const Setting *setting(int i) {
    return i < NPLAYER ? &player[i] : i < NPLAYER + NDEVELOPER ? &developer[i - NPLAYER] : &cheats[i - NPLAYER - NDEVELOPER];
}

static const char *const link_help =
    "SWITCHES FOR TESTING THE PORT: WHO DRAWS EACH PART OF THE PICTURE, THE GAME OR OUR OWN RENDERER, AND "
    "SPEED-UPS THAT LOOK THE SAME. THE PICTURE DOESN'T CHANGE; THE DEFAULTS ARE THE RECOMMENDED ONES.";

static int legacy_open, page, developer_page, selected[2];  /* selected: on the player's page, the developer page */

/* ---- the settings file: YW4_SETTINGS_FILE, or settings.ini next to the executable; "none": no file */

static char file_path[1024];

static const char *settings_file(void);
const char *settings_file_name(void) { return settings_file(); }

static const char *settings_file(void) {
    if (!file_path[0]) {
        const char *e = getenv("YW4_SETTINGS_FILE");
        if (e && (!*e || !strcmp(e, "none"))) return "";
        const char *base = e ? "" : SDL_GetBasePath();
        snprintf(file_path, sizeof file_path, "%s%s", base ? base : "", e ? e : "settings.ini");
    }
    return file_path;
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

/* Graphics not picked yet: a settings file is in use (not YW4_SETTINGS_FILE=none) but doesn't exist, or exists
 * without a YW4_SHADOW_RES line. The game's own save writes every setting, so only a file someone else started has
 * none: the launcher writes YW4_WINDOW (Full screen at start) and the cheats into settings.ini, and a player who
 * flipped one before the first start used to skip this and got the high defaults on any GPU (issue #14: an Intel
 * HD 520, device lost in the first cutscene). settings_first_run then picks the graphics for the GPU once and writes
 * the file, keeping the lines that were there. */
static int first_run;

/* Lines a player adds to settings.ini by hand when a bug report asks for them (no panel row): read like the panel's,
 * and written back as they were, so the game's own saves don't drop them. YW4_PERF=1: the per-frame performance CSV
 * (perf.c; owner, 2026-11-16: "we are asking them", so a line in the file is enough). YW4_RELAY (host:port),
 * YW4_ROOM: local play through a home-network relay (relay.c, ldn.c; decision 0082; the Online button's server and
 * codes are Online play rows). YW4_VRAM_GEOMETRY=0: vertices from the game's own memory, no copies in video memory
 * (issue #25: the minimap drawn over the screen while biking, RX 9070 XT only); YW4_IMAGE_BLOCKS=0: every image in
 * memory of its own, none sharing a block (the same report: two images overlapping in memory). YW4_POOL_CACHED=0:
 * the game's memory imported as before decision 0086 (the uncached type on AMD), to test that fix both ways. */
static const char *const by_hand[] = {"YW4_PERF", "YW4_RELAY", "YW4_ROOM", "YW4_VRAM_GEOMETRY", "YW4_IMAGE_BLOCKS",
                                      "YW4_POOL_CACHED"};
enum { BY_HAND = sizeof by_hand / sizeof *by_hand };
static char by_hand_value[BY_HAND][64];

void settings_load(void) {
    const char *path = settings_file();
    FILE *f = *path ? fopen(path, "r") : NULL;
    if (!f) {
        first_run = *path != 0;
        return;
    }
    char line[256];
    int n = 0, graphics = 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        if (!strcmp(line, "YW4_SHADOW_RES")) graphics = 1;
        char *value = eq + 1;
        value[strcspn(value, "\r\n")] = 0;
        for (int i = 0; i < NSETTINGS; i++)  /* only the panel's settings, and never over the environment */
            if (!strcmp(line, setting(i)->env) && !getenv(line)) set_env(line, value), n++;
        n += bindings_load_line(line, value);
        if ((!strcmp(line, "YW4_THEME") || !strcmp(line, "YW4_WINDOW_SIZE") || !strcmp(line, "YW4_KEYBOARD")) &&
            !getenv(line))
            set_env(line, value), n++;
        for (int i = 0; i < BY_HAND; i++)
            if (!strcmp(line, by_hand[i])) {
                snprintf(by_hand_value[i], sizeof by_hand_value[i], "%s", value);
                const char *now = getenv(line);  /* set but empty counts as not set (set_env doesn't replace it) */
                if (!now || !*now) {
#ifdef _WIN32
                    _putenv_s(line, value);
#else
                    setenv(line, value, 1);
#endif
                    n++;
                }
                fprintf(stderr, "yw4: settings: %s=%s (added by hand)\n", line, value);
            }
    }
    fclose(f);
    first_run = !graphics;
    fprintf(stderr, "yw4: settings: %d from %s%s\n", n, path, graphics ? "" : " (graphics not picked yet)");
}

/* settings_save runs on the window thread (F1/F11 pages) and on the game thread (the name popup's keyboard language,
 * gamemenu.c): one at a time, or two writers would mix their lines. */
static HostLock save_lock = HOST_LOCK_INIT;
static void write_settings(FILE *f);

/* Written to settings.ini.tmp, put on the disk, then renamed over settings.ini in one step: a crash, a force-close
 * or a power cut in the middle leaves the old file whole, never an empty one that the next start would take for a
 * first run (graphics and key bindings back to defaults; review 2026-11-19). */
void settings_save(void) {
    const char *path = settings_file();
    if (!*path) return;
    host_lock(&save_lock);
    char tmp[1040];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        write_settings(f);
        int bad = ferror(f) || host_file_sync(f);
        if (fclose(f) || bad || host_replace_file(tmp, path)) {
            fprintf(stderr, "yw4: settings: couldn't save %s (the last saved file stays)\n", path);
            remove(tmp);
        }
    }
    host_unlock(&save_lock);
}

/* A setting's name and value as the control file's "list" prints them, read the way write_settings reads each kind
 * of row (typed, Graphics card, slider, options): "" for none (an unset typed row, a choice out of range). */
static const char *setting_line(const Setting *t, char *v, size_t n) {
    v[0] = 0;
    if (typed_env(t)) {  /* typed (a name, a code), not one of the options */
        const char *e = getenv(typed_env(t));
        snprintf(v, n, "%s", e ? e : "");
        return typed_env(t);
    }
    if (t->next == gpu_card_cycle) {  /* Graphics card: the choice as kept, even before the cards are known */
        snprintf(v, n, "%s", gpu_choice());
        return "YW4_GPU";
    }
    if (!t->noptions) {
        slider_text(t, NULL, 0, v, n);
        return t->env;
    }
    int c = t->current();
    if (c >= 0 && c < t->noptions) snprintf(v, n, "%s", t->values[c] ? t->values[c] : "");
    return t->env;
}

static void write_settings(FILE *f) {
    fprintf(f, "# The settings panel's choices (F1), written when one changes. An environment variable of the same\n"
               "# name wins over its line here.\n");
    for (int i = 0; i < NSETTINGS; i++) {
        const Setting *t = setting(i);
        if (typed_env(t)) {  /* typed (a name, a code), not one of the options; empty: no line */
            const char *v = getenv(typed_env(t));
            if (v && *v) fprintf(f, "%s=%s\n", typed_env(t), v);
            continue;
        }
        if (t->next == gpu_card_cycle) {  /* Graphics card: the choice as kept, even before the cards are known */
            fprintf(f, "YW4_GPU=%s\n", gpu_choice());
            continue;
        }
        if (!t->noptions) {
            char v[16];
            slider_text(t, NULL, 0, v, sizeof v);
            fprintf(f, "%s=%s\n", t->env, v);
            continue;
        }
        int c = t->current();
        if (c >= 0 && c < t->noptions) fprintf(f, "%s=%s\n", t->env, t->values[c]);
    }
    /* the performance HUD (F3, the in-game overlay's Performance page) */
    fprintf(f, "YW4_HUD=%d\nYW4_HUD_CORNER=%d\nYW4_HUD_PARTS=%u\n", window_hud_mode(), window_hud_corner(),
            window_hud_parts());
    fprintf(f, "YW4_THEME=%s\n", theme_id());  /* the overlay's Look > Theme (theme.h) */
    int ww, wh;
    window_size_kept(&ww, &wh);  /* the size the player left the window at (window.c) */
    if (ww && wh) fprintf(f, "YW4_WINDOW_SIZE=%dx%d\n", ww, wh);
    const char *kb = getenv("YW4_KEYBOARD");  /* the name keyboard's last language (gamemenu.c) */
    if (kb && *kb) fprintf(f, "YW4_KEYBOARD=%s\n", kb);
    for (int i = 0; i < BY_HAND; i++)  /* the lines added by hand, as they were */
        if (by_hand_value[i][0]) fprintf(f, "%s=%s\n", by_hand[i], by_hand_value[i]);
    bindings_save(f);
}

int settings_legacy_open(void) { return __atomic_load_n(&legacy_open, __ATOMIC_ACQUIRE); }

void settings_legacy_toggle(void) {
    bindings_cancel();
    developer_page = 0;
    __atomic_store_n(&legacy_open, !settings_legacy_open(), __ATOMIC_RELEASE);
}

int settings_legacy_page(void) { return __atomic_load_n(&page, __ATOMIC_ACQUIRE); }

void settings_legacy_show(int p) {
    bindings_cancel();
    if (settings_legacy_open() && settings_legacy_page() == p) {
        developer_page = 0;
        __atomic_store_n(&legacy_open, 0, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&page, p, __ATOMIC_RELEASE);
    __atomic_store_n(&legacy_open, 1, __ATOMIC_RELEASE);
}

static void setting_step(const Setting *s, int dir) {
    if (!s->noptions) {
        slider_step(s, dir > 0 ? 1 : -1);
        return;
    }
    /* anti-aliasing and frame generation skip what this GPU or build can't run, and every change starts a real
     * switch (frame generation's safe frames): they step back themselves, so a step back is one switch */
    if (s->next == gpu_aa_cycle) { gpu_aa_step(dir > 0 ? 1 : -1); return; }
    if (s->next == gpu_fg_cycle) { gpu_fg_step(dir > 0 ? 1 : -1); return; }
    if (s->next == gpu_card_cycle) { gpu_card_step(dir > 0 ? 1 : -1); return; }
    if (dir > 0) { s->next(); return; }
    for (int i = 0; i < s->noptions - 1; i++) s->next();  /* one step back: the other cycles go forward only */
}

void settings_legacy_action(int action) {
    if (action == SETTINGS_PAGE) {
        bindings_cancel();
        __atomic_store_n(&page, !settings_legacy_page(), __ATOMIC_RELEASE);
        return;
    }
    if (action == SETTINGS_CLOSE) {
        bindings_cancel();
        if (!settings_legacy_page() && developer_page) {  /* Escape or B on the developer page: back */
            developer_page = 0;
            return;
        }
        __atomic_store_n(&legacy_open, 0, __ATOMIC_RELEASE);
        return;
    }
    if (settings_legacy_page()) {
        bindings_action(action);
        return;
    }
    /* the player's page has one row more than its settings: DEVELOPER OPTIONS */
    int *sel = &selected[developer_page], nrows = developer_page ? NDEVELOPER : NPLAYER + 1;
    if (!developer_page && *sel == LINK_ROW && (action == SETTINGS_ACCEPT || action == SETTINGS_RIGHT)) {
        developer_page = 1;
        return;
    }
    if (action == SETTINGS_UP || action == SETTINGS_DOWN) {
        *sel = (*sel + (action == SETTINGS_UP ? nrows - 1 : 1)) % nrows;
        return;
    }
    if (!developer_page && *sel == LINK_ROW) return;
    const Setting *s = developer_page ? &developer[*sel] : &player[*sel];
    if (typed_env(s)) {
        /* A name or a code is typed on the overlay's keyboard popup (this panel's font has no lower case): Enter, A
         * or D close the panel and open it, and the panel comes back when it ends (gamemenu.c prompt_close). A
         * greyed-out row (a code while the room is public) does nothing. */
        if ((action == SETTINGS_ACCEPT || action == SETTINGS_LEFT || action == SETTINGS_RIGHT) &&
            !(s->disabled && s->disabled())) {
            int overlay_was_open = gamemenu_open();
            __atomic_store_n(&legacy_open, 0, __ATOMIC_RELEASE);
            s->next();
            if (!gamemenu_prompting()) __atomic_store_n(&legacy_open, 1, __ATOMIC_RELEASE);  /* no popup came up */
            else if (!overlay_was_open) gamemenu_prompt_return_to_panel();
        }
        return;
    }
    if ((action == SETTINGS_LEFT || action == SETTINGS_RIGHT) && s->disabled && s->disabled()) return;  /* greyed out */
    switch (action) {
    case SETTINGS_RIGHT: setting_step(s, 1), settings_save(); break;
    case SETTINGS_LEFT:  setting_step(s, -1), settings_save(); break;
    }
}

int settings_legacy_developer(void) { return developer_page; }

static int fill_rows(SettingRow *rows, int max, const Setting *list, int count) {
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) {
        memset(&rows[i], 0, sizeof rows[i]);
        rows[i].category = list[i].category, rows[i].name = list[i].name, rows[i].help = list[i].help;
        memcpy(rows[i].options, list[i].options, sizeof rows[i].options);
        memcpy(rows[i].notes, list[i].notes, sizeof rows[i].notes);
        rows[i].noptions = list[i].noptions;
        if (typed_env(&list[i])) rows[i].options[0] = typed_shown(&list[i]), rows[i].verbatim = rows[i].textbox = 1;
        /* leave out options this GPU or build can't run, only from the end of the list (FSR, DLSS, then XeSS): an
         * option in the middle stays listed, as the row's index is the mode (the cycle skips it) */
        if (list[i].next == gpu_aa_cycle || list[i].next == gpu_fg_cycle) {
            int aa = list[i].next == gpu_aa_cycle;
            int dlss_ok = gpu_dlss_built() && (aa ? gpu_dlss_available() : dlssg_available());
            int n = aa ? 7 : 3;
            if (aa && gpu_xess_available() != 1) n = 6;   /* XeSS is last (anti-aliasing only) */
            if (n == (aa ? 6 : 3) && !dlss_ok) n--;      /* then DLSS */
            if (n == (aa ? 5 : 2) && !gpu_fsr_built()) n--;  /* and FSR before it */
            rows[i].noptions = n;
        }
        if (list[i].next == gpu_card_cycle) gpu_card_fill(&rows[i]);  /* the cards found (window.c) */
        if (!list[i].noptions) {  /* a slider: its text, its position, and one note for the description */
            float v = list[i].get();
            rows[i].slider = 1;
            rows[i].frac = v <= 0 ? 0.0f : (v - list[i].lo) / (list[i].hi - list[i].lo);
            slider_text(&list[i], rows[i].value, sizeof rows[i].value, NULL, 0);
            rows[i].options[0] = rows[i].value;
            rows[i].notes[0] = v <= 0 ? list[i].auto_note : "";
            rows[i].current = 0;
            for (int k = 0; k < SETTINGS_MAX_OPTIONS && list[i].marks[k]; k++) rows[i].marks[rows[i].nmarks++] = list[i].marks[k];
        } else {
            rows[i].current = list[i].current();
        }
        rows[i].disabled = list[i].disabled ? list[i].disabled() : NULL;
    }
    return n;
}

int settings_rows(SettingRow *rows, int max, int *sel) {
    int n = fill_rows(rows, max, developer_page ? developer : player, developer_page ? NDEVELOPER : NPLAYER);
    if (!developer_page && n < max) {
        memset(&rows[n], 0, sizeof rows[n]);
        rows[n].category = "ADVANCED", rows[n].name = "DEVELOPER OPTIONS", rows[n].help = link_help;
        rows[n++].link = 1;
    }
    *sel = selected[developer_page];
    return n;
}

/* The in-game overlay's settings page (gamemenu.c): the player's settings, without the developer page's link, and
 * changing one of them by row (as Left / Right on it), whatever the panel shows. */
static int hud_slider_row(const Setting *s) { return s->get == hud_scale_get || s->get == hud_opacity_get; }
/* A player setting the overlay's Settings page lists now: not the HUD's sliders, not a row hidden for now. The row
 * list (settings_player_rows), the Theme row's place and a row's setting (player_row_setting) all count with it. */
/* Online play and Profile are the overlay's Online tab (owner, 2026-11-19: "move online stuff out of settings"),
 * settings_online_* below; they stay in player[] for the settings file and the old panel */
static int online_row(const Setting *s) { return !strcmp(s->category, "ONLINE PLAY") || !strcmp(s->category, "PROFILE"); }
static int overlay_row(const Setting *s) { return !hud_slider_row(s) && !(s->hidden && s->hidden()) && !online_row(s); }

/* The HUD's sliders live on the overlay's Overlay page, next to their preview, so its Settings page leaves them out
 * (the old panel keeps them). */
/* The overlay's Look > Theme row (theme.h): after the player's settings, before Advanced, overlay only (the old panel's pixel font has no lower
 * case for the names); its value one theme's name, Left / Right step through them. */
static int theme_row(void) {
    int n = 0;
    for (int i = 0; i < NPLAYER && strcmp(player[i].category, "ADVANCED"); i++) n += overlay_row(&player[i]);
    return n;
}

static void fill_theme_row(SettingRow *r) {
    static char note[32];
    memset(r, 0, sizeof *r);
    r->category = "LOOK", r->name = "THEME", r->verbatim = 1;
    r->help = "THE COLOURS OF THIS MENU AND THE PERFORMANCE HUD: THE SAME THEMES AS THE LAUNCHER'S.";
    snprintf(note, sizeof note, "%d / %d", theme_index() + 1, theme_count());
    r->options[0] = theme_name(theme_index()), r->notes[0] = note, r->noptions = 1, r->current = 0;
}

/* The overlay's rows: the player's settings (the graphics sections; Online play and Profile are left out, they are on
 * the Online tab), Look > Theme, then Advanced: Legacy menus and the developer page's settings (owner, 2026-11-11: the
 * renderer switches and speed-ups were only on the old panel). Every row but Legacy menus changes the picture, so
 * Preview can show it (`look`). */
int settings_player_rows(SettingRow *rows, int max) {
    int n = 0, at = theme_row();
    int themed = 0;
    for (int i = 0; i < NPLAYER && n < max; i++) {
        if (!themed && !strcmp(player[i].category, "ADVANCED") && n == at)  /* Look > Theme just before Advanced */
            fill_theme_row(&rows[n++]), rows[n - 1].look = 1, themed = 1;
        if (n < max && overlay_row(&player[i])) {
            n += fill_rows(rows + n, 1, &player[i], 1);
            rows[n - 1].look = strcmp(player[i].category, "ONLINE PLAY") && strcmp(player[i].category, "PROFILE") &&
                               strcmp(player[i].category, "ADVANCED");
        }
    }
    if (!themed && n == at && n < max) fill_theme_row(&rows[n++]), rows[n - 1].look = 1;
    for (int i = 0; i < NDEVELOPER && n < max; i++) {
        n += fill_rows(rows + n, 1, &developer[i], 1);
        rows[n - 1].category = "ADVANCED", rows[n - 1].look = 1;
    }
    return n;
}

static const Setting *hud_slider(int opacity) {
    for (int i = 0; i < NPLAYER; i++)
        if (player[i].get == (opacity ? hud_opacity_get : hud_scale_get)) return &player[i];
    return NULL;
}

void settings_hud_slider(int opacity, float *frac, char *text, size_t n) {
    const Setting *s = hud_slider(opacity);
    float v = s ? s->get() : 0;
    *frac = !s || v <= 0 ? 0.0f : (v - s->lo) / (s->hi - s->lo);
    if (opacity) snprintf(text, n, "%d%%", (int)(v * 100 + 0.5f));
    else if (v <= 0) snprintf(text, n, "Auto");
    else snprintf(text, n, "%.2fx", v);
}

void settings_hud_step(int opacity, int dir) {
    const Setting *s = hud_slider(opacity);
    if (s) slider_step(s, dir > 0 ? 1 : -1), settings_save();
}

void settings_hud_set(int opacity, float frac) {
    const Setting *s = hud_slider(opacity);
    if (!s) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float v = s->lo + frac * (s->hi - s->lo);
    v = (float)(int)(v / s->step + 0.5f) * s->step;  /* on the steps */
    s->set(v < s->lo ? s->lo : v > s->hi ? s->hi : v);
    settings_save();
}

/* A setting by its variable's name stepped until it shows `value` (as the panel would change it); 0 when found. */
static int set_value(const char *env, const char *value, size_t vlen) {
    for (int i = 0; i < NSETTINGS; i++) {
        const Setting *s = setting(i);
        if (!s->env || strcmp(s->env, env)) continue;
        if (!s->noptions) {  /* a slider: the value as the settings file has it */
            char v[32];
            snprintf(v, sizeof v, "%.*s", (int)(vlen < sizeof v - 1 ? vlen : sizeof v - 1), value);
            s->set(s->parse ? s->parse(v) : (float)atof(v));
            return 0;
        }
        for (int k = 0; k < s->noptions; k++) {
            const char *v = s->values[s->current()];
            if (v && strlen(v) == vlen && !strncmp(v, value, vlen)) break;
            setting_step(s, 1);
        }
        return 0;
    }
    return -1;
}

/* First start of a new install (owner, 2026-11-11: integrated GPUs crawled with the high defaults): `low` (an
 * integrated GPU, or under 4 GB of video memory) sets the shadows and effects to the Switch's; else the owner's
 * own high set (below). A variable set in the environment is left alone. The file is written either way,
 * so this runs once. YW4_FIRST_RUN_LEVEL=low|high (tests) overrides the GPU's level. 1 when it ran. */
int settings_first_run(int low) {
    if (!first_run) return 0;
    first_run = 0;
    const char *force = getenv("YW4_FIRST_RUN_LEVEL");
    if (force && *force) low = !strcmp(force, "low");
    static const char *const lowered[][2] = {
        {"YW4_SHADOW_RES", "1024"}, {"YW4_PCSS", "0"}, {"YW4_CONTACT", "0"}, {"YW4_LAMP_SHADOWS", "0"},
        {"YW4_EFFECTS", "0"}};
    /* A dedicated GPU gets the owner's own settings (owner, 2026-11-13), but 1440p, not 4K, and 120 frames/s
     * (owner, 2026-11-13; others start at 60). DLSS and its frame generation and Reflex only where the GPU has them;
     * else FSR anti-aliasing and no frame generation. */
    const char *aa = gpu_dlss_available() ? "5" : "4", *fg = dlssg_available() ? "2" : "0";
    const char *reflex = dlssg_reflex_available() ? "1" : "0";
    const char *const raised[][2] = {
        {"YW4_RES", "1440"}, {"YW4_FPS", "120"}, {"YW4_AA", aa}, {"YW4_UPSCALE", "0"}, {"YW4_FG", fg}, {"YW4_REFLEX", reflex},
        {"YW4_ANISO", "16"}, {"YW4_EFFECTS", "1"}, {"YW4_DOF", "2"}, {"YW4_SHADOW_RES", "4096"}, {"YW4_PCSS", "2"},
        {"YW4_CONTACT", "1"}};  /* (point light shadows: experimental, off for everyone at first) */
    for (size_t i = 0; low && i < sizeof lowered / sizeof *lowered; i++)
        if (!getenv(lowered[i][0])) set_value(lowered[i][0], lowered[i][1], strlen(lowered[i][1]));
    for (size_t i = 0; !low && i < sizeof raised / sizeof *raised; i++)
        if (!getenv(raised[i][0])) set_value(raised[i][0], raised[i][1], strlen(raised[i][1]));
    settings_save();
    fprintf(stderr, "yw4: settings: first start: %s graphics for this GPU, written to %s\n", low ? "lighter" : "high",
            settings_file());
    return 1;
}

/* One setting changed live as the panel would change it, one step at a time until it shows that value (not saved);
 * a slider (SHARPENING) set straight to the value, as the file has it. Returns the value it shows now in buf, or NULL
 * for a name that is no setting. */
static const char *settings_apply_live(const char *name, size_t nlen, const char *val, size_t vlen, char *buf,
                                       size_t bn) {
    for (int i = 0; i < NSETTINGS; i++) {
        const Setting *s = setting(i);
        if (!s->env || strlen(s->env) != nlen || strncmp(s->env, name, nlen)) continue;
        if (!s->noptions) {
            set_value(s->env, val, vlen);
            slider_text(s, NULL, 0, buf, bn);
        } else {
            for (int k = 0; k < s->noptions; k++) {
                const char *v = s->values[s->current()];
                if (v && strlen(v) == vlen && !strncmp(v, val, vlen)) break;
                setting_step(s, 1);
            }
            snprintf(buf, bn, "%s", s->values[s->current()] ? s->values[s->current()] : "");
        }
        return buf;
    }
    return NULL;
}

/* YW4_SET_AT=<frame>:<NAME>=<value>,... (tests): at those presented frames a setting changes live (settings_apply_live),
 * e.g. YW4_SET_AT=3000:YW4_ASPECT=32:9,3300:YW4_UPSCALE=1. */
void settings_test_at(uint64_t frame) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = getenv("YW4_SET_AT");
    for (const char *p = e; p && *p;) {
        char *end;
        unsigned long long f = strtoull(p, &end, 10);
        if (*end != ':') return;
        const char *name = end + 1, *eq = strchr(name, '='), *next = strchr(name, ',');
        if (!eq || (next && next < eq)) return;
        size_t vlen = next ? (size_t)(next - eq - 1) : strlen(eq + 1);
        char now[64];
        if (f == frame && settings_apply_live(name, (size_t)(eq - name), eq + 1, vlen, now, sizeof now)) {
            fprintf(stderr, "yw4: settings: %.*s is %s at frame %llu (YW4_SET_AT)\n", (int)(eq - name), name, now, f);
            yw4_event("set", f, "\"name\":\"%.*s\",\"value\":\"%s\"", (int)(eq - name), name, now);
        }
        p = next ? next + 1 : NULL;
    }
}

/* The control file (owner, 2026-11-20: "a direct way for you to control live game settings without restarting"):
 * yw4_control.txt next to the game (YW4_CONTROL_FILE: elsewhere), looked at every 15 presented frames. Each line is
 * NAME=value (a settings.ini name: changed live, as YW4_SET_AT does, not saved) or "list" (every setting's value now);
 * # starts a comment. The file is read and deleted, and the answers go to the log and to yw4_control.out.txt beside
 * it (replaced each time). Write it whole: to a temporary name, then rename. Local files only: no network. */
static char control_path[1024];
void settings_control_poll(uint64_t frame) {
    if (frame % 15) return;
    if (!control_path[0]) {
        const char *e = getenv("YW4_CONTROL_FILE");
        const char *base = e && *e ? "" : SDL_GetBasePath();
        snprintf(control_path, sizeof control_path, "%s%s", base ? base : "", e && *e ? e : "yw4_control.txt");
    }
    FILE *f = fopen(control_path, "rb");
    if (!f) return;
    char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    remove(control_path);
    char out_path[1040];
    snprintf(out_path, sizeof out_path, "%.*s.out.txt", (int)(strlen(control_path) > 4 && !strcmp(control_path +
             strlen(control_path) - 4, ".txt") ? strlen(control_path) - 4 : strlen(control_path)), control_path);
    FILE *o = fopen(out_path, "wb");
    if (o) fprintf(o, "frame %llu\n", (unsigned long long)frame);
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
        while (*line == ' ') line++;
        if (!*line || *line == '#') continue;
        if (!strcmp(line, "list")) {
            for (int i = 0; i < NSETTINGS; i++) {
                const Setting *s = setting(i);
                char v[256];
                const char *key = setting_line(s, v, sizeof v);
                if (!key) continue;
                if (o) fprintf(o, "%s=%s\n", key, v);
            }
            fprintf(stderr, "yw4: control: list (%s)\n", out_path);
            continue;
        }
        char *eq = strchr(line, '='), now[64];
        const char *r = eq ? settings_apply_live(line, (size_t)(eq - line), eq + 1, strlen(eq + 1), now, sizeof now)
                           : NULL;
        if (r) {
            fprintf(stderr, "yw4: control: %.*s is %s at frame %llu\n", (int)(eq - line), line, r,
                    (unsigned long long)frame);
            yw4_event("set", frame, "\"name\":\"%.*s\",\"value\":\"%s\"", (int)(eq - line), line, r);
            if (o) fprintf(o, "%.*s=%s ok\n", (int)(eq - line), line, r);
        } else {
            fprintf(stderr, "yw4: control: not a setting: %s\n", line);
            if (o) fprintf(o, "%s: not a setting\n", line);
        }
    }
    if (o) fclose(o);
}

/* The setting behind an overlay row (settings_player_rows' order, the theme row taken out); NULL for none. */
static const Setting *player_row_setting(int row) {
    int at = theme_row();
    if (row == at) return NULL;
    if (row > at) row--;
    const Setting *s = NULL;
    int seen = 0;
    for (int i = 0; i < NPLAYER && !s; i++)
        if (overlay_row(&player[i]) && seen++ == row) s = &player[i];
    if (!s && row - seen >= 0 && row - seen < NDEVELOPER) s = &developer[row - seen];  /* Advanced's developer rows */
    return s;
}

void settings_change(int row, int dir) {
    if (row == theme_row()) {  /* Look > Theme */
        theme_step(dir);
        settings_save();
        return;
    }
    const Setting *s = player_row_setting(row);
    if (!s) return;
    if (s->disabled && s->disabled()) return;  /* greyed out */
    setting_step(s, dir);
    settings_save();
}

void settings_slide(int row, float frac) {
    const Setting *s = player_row_setting(row);
    if (!s || s->noptions || (s->disabled && s->disabled())) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float v = s->lo + frac * (s->hi - s->lo);
    v = (float)(int)(v / s->step + 0.5f) * s->step;  /* on the steps */
    float d = v - s->get();
    if (d > -s->step * 0.5f && d < s->step * 0.5f) return;  /* a drag within one step: no save per mouse move */
    s->set(v < s->lo ? s->lo : v > s->hi ? s->hi : v);
    settings_save();
}

int settings_cheat_rows(SettingRow *rows, int max) {
    return cheats_allowed() ? fill_rows(rows, max, cheats, NCHEATS) : 0;
}

void settings_cheat_change(int row, int dir) {
    if (!cheats_allowed() || row < 0 || row >= NCHEATS) return;
    setting_step(&cheats[row], dir);
    settings_save();
}
