/* The settings panel's list (settings.h): each row reads its setting's current option and steps it with
 * the setting's own cycle function, so the panel and the replays' keys always agree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_filesystem.h>

#include "bindings.h"
#include "framerate.h"
#include "gpu_dlss.h"
#include "gpu_dlssg.h"
#include "gpu_fg.h"
#include "gpu_fsr.h"
#include "post.h"
#include "gbuffer.h"
#include "gpu.h"
#include "hud.h"
#include "settings.h"
#include "theme.h"
#include "shadow.h"
#include "window.h"

const char *shadow_mode_name(void);

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
} Setting;

/* A slider's value as shown and as saved. */
static void slider_text(const Setting *s, char *shown, size_t n1, char *saved, size_t n2) {
    float v = s->get();
    if (s->percent) {
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
    int h = gpu_res_height();
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
static int shadow_pass_index(void) { return !strcmp(shadow_mode_name(), "CUSTOM"); }
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
static int reflex_index(void) { return dlssg_reflex(); }  /* what runs: on while DLSS frame generation is */
static const char *reflex_disabled(void) {
    if (!gpu_dlss_built()) return "NOT IN THIS BUILD";
    if (!dlssg_reflex_available()) return "NEEDS AN NVIDIA GPU";
    return gpu_fg() == 2 && dlssg_available() ? "ON WHILE FRAME GENERATION IS DLSS: NVIDIA REQUIRES IT" : NULL;
}
static const char *fg_disabled(void) {
    if (!gpu_fsr_built() && !gpu_dlss_built()) return "NOT IN THIS BUILD";
    return gpu_aa() >= 3 ? NULL : "NEEDS ANTI-ALIASING SET TO TAA, FSR OR DLSS";
}
static const char *upscale_disabled(void) {
    if (!gpu_fsr_built() && !gpu_dlss_built()) return "NOT IN THIS BUILD";
    return gpu_aa() >= 4 ? NULL : "NEEDS ANTI-ALIASING SET TO FSR OR DLSS";
}

/* LEGACY MENUS: the keys from before the in-game overlay. Read once, after settings_load has filled the
 * environment. Only the window's real key presses ask (window.c); a replay's F1 and F2 still open the panel, so
 * the test recordings don't depend on this setting. */
static int legacy_keys = -1;
int settings_legacy_keys(void) {
    if (legacy_keys < 0) {
        const char *e = getenv("YW4_LEGACY_KEYS");
        legacy_keys = !(e && !strcmp(e, "0"));
    }
    return legacy_keys;
}
static void legacy_keys_toggle(void) { legacy_keys = !settings_legacy_keys(); }

/* The player's settings, in the order shown; a row starts a new group when its category differs from the row
 * before. The last row opens the developer page. The texts use the HUD font's characters (hud.c): upper case,
 * digits, . , : % / - ( ) ' ! ? */
static const Setting player[] = {
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
     "WIDER THAN THE SWITCH'S 16:9 FOR WIDE MONITORS: THE WORLD SHOWS MORE AT THE SIDES, THE MENUS STAY 16:9 IN "
     "THE MIDDLE.",
     {"16:9", "16:10", "21:9", "32:9"},
     {"THE SWITCH'S DEFAULT", "A LITTLE TALLER: LESS AT THE SIDES", "ULTRAWIDE", "SUPER ULTRAWIDE"}, 4, gpu_wide,
     gpu_wide_cycle, "YW4_ASPECT", {"16:9", "16:10", "21:9", "32:9"}},
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
     {"OFF", "FXAA", "SMAA", "TAA", "FSR", "DLSS"},
     {"THE SWITCH'S DEFAULT: JAGGED EDGES", "SMOOTHEST, TEXTURES A LITTLE SOFTER", "CLEAN EDGES, TEXTURES STAY SHARP",
      "STEADY EDGES: NO SHIMMER ON FENCES, LEAVES AND SHADOWS", "AMD FSR: STEADY EDGES, BEST WITH UPSCALING",
      "NVIDIA DLSS: AI UPSCALING, NVIDIA RTX GPUS (ELSE FSR)"}, 6, gpu_aa,
     gpu_aa_cycle, "YW4_AA", {"0", "1", "2", "3", "4", "5"}},
    {"ANTI-ALIASING AND UPSCALING", "UPSCALING",
     "DRAWS THE WORLD SMALLER THAN THE RESOLUTION, THEN AMD FSR OR NVIDIA DLSS REBUILDS IT AT FULL SIZE: MORE "
     "FRAMES. MENUS AND TEXT STAY FULL SIZE. NEEDS ANTI-ALIASING SET TO FSR OR DLSS.",
     {"OFF", "QUALITY", "BALANCED", "PERFORMANCE"},
     {"THE WORLD AT FULL RESOLUTION", "1.5X SMALLER: CLOSE TO FULL, FASTER", "1.7X SMALLER: FASTER STILL",
      "2X SMALLER: FASTEST, SOFTER"}, 4,
     gpu_upscale_setting, gpu_upscale_cycle, "YW4_UPSCALE", {"0", "1", "2", "3"}, upscale_disabled},
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
    {"ADVANCED", "LEGACY MENUS",
     "THE OLD KEYS: F1 OPENS THE OLD SETTINGS PANEL AND ITS DEVELOPER OPTIONS, F2 THE OLD CONTROLS PAGE, F3 SHOWS OR "
     "HIDES THE PERFORMANCE HUD. OFF, THE THREE KEYS DO NOTHING: THIS MENU HAS ALL OF IT BUT THE DEVELOPER OPTIONS.",
     {"OFF", "ON"},
     {"F1, F2 AND F3 DO NOTHING", "F1, F2 AND F3 WORK"}, 2, settings_legacy_keys, legacy_keys_toggle,
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
    {"SPEED-UPS", "SOFT SHADOW SEARCH",
     "HOW MANY SAMPLES SOFT SHADOWS TAKE TO FIND WHAT CASTS THE SHADOW, WHICH SETS HOW SOFT ITS EDGE IS. 8 IS "
     "ABOUT 0.25 MS FASTER AT 4K, BUT SMALL SHADOW SPOTS CAN APPEAR ON SOME OBJECTS.",
     {"16", "8"},
     {"16 SAMPLES", "8 SAMPLES"}, 2, gpu_pcss_search8, gpu_pcss_search8_toggle, "YW4_PCSS_SEARCH_TAPS", {"16", "8"},
     mask_speedup_disabled},
    {"SPEED-UPS", "SOFT SHADOW SAMPLES",
     "HOW MANY SAMPLES MAKE EACH SOFT SHADOW EDGE. 8 TURNS THEM EACH FRAME SO THE ANTI-ALIASING (TAA, FSR, DLSS) "
     "AVERAGES THEM OVER FRAMES: CHEAPER, MAY SHIMMER A LITTLE IN MOTION. WITHOUT THAT ANTI-ALIASING EDGES GET GRAINY.",
     {"16", "8"},
     {"16 SAMPLES", "8 SAMPLES, TURNED EACH FRAME"}, 2, gpu_pcss_filter8, gpu_pcss_filter8_toggle, "YW4_PCSS_FILTER_TAPS",
     {"16", "8"}, mask_speedup_disabled},
};
enum { NPLAYER = sizeof player / sizeof *player, NDEVELOPER = sizeof developer / sizeof *developer,
       NSETTINGS = NPLAYER + NDEVELOPER, LINK_ROW = NPLAYER };

/* Every setting, both pages: for the settings file. */
static const Setting *setting(int i) { return i < NPLAYER ? &player[i] : &developer[i - NPLAYER]; }

static const char *const link_help =
    "SWITCHES FOR TESTING THE PORT: WHO DRAWS EACH PART OF THE PICTURE, THE GAME OR OUR OWN RENDERER, AND "
    "SPEED-UPS THAT LOOK THE SAME. THE PICTURE DOESN'T CHANGE; THE DEFAULTS ARE THE RECOMMENDED ONES.";

static int panel_open, page, developer_page, selected[2];  /* selected: on the player's page, the developer page */

/* ---- the settings file: YW4_SETTINGS_FILE, or settings.ini next to the executable; "none": no file */

static char file_path[1024];

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

void settings_load(void) {
    const char *path = settings_file();
    FILE *f = *path ? fopen(path, "r") : NULL;
    if (!f) return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *value = eq + 1;
        value[strcspn(value, "\r\n")] = 0;
        for (int i = 0; i < NSETTINGS; i++)  /* only the panel's settings, and never over the environment */
            if (!strcmp(line, setting(i)->env) && !getenv(line)) set_env(line, value), n++;
        n += bindings_load_line(line, value);
        if (!strcmp(line, "YW4_THEME") && !getenv(line)) set_env(line, value), n++;
    }
    fclose(f);
    fprintf(stderr, "yw4: settings: %d from %s\n", n, path);
}

void settings_save(void) {
    const char *path = settings_file();
    FILE *f = *path ? fopen(path, "w") : NULL;
    if (!f) return;
    fprintf(f, "# The settings panel's choices (F1), written when one changes. An environment variable of the same\n"
               "# name wins over its line here.\n");
    for (int i = 0; i < NSETTINGS; i++) {
        const Setting *t = setting(i);
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
    bindings_save(f);
    fclose(f);
}

int settings_open(void) { return __atomic_load_n(&panel_open, __ATOMIC_ACQUIRE); }

void settings_toggle(void) {
    bindings_cancel();
    developer_page = 0;
    __atomic_store_n(&panel_open, !settings_open(), __ATOMIC_RELEASE);
}

int settings_page(void) { return __atomic_load_n(&page, __ATOMIC_ACQUIRE); }

void settings_show(int p) {
    bindings_cancel();
    if (settings_open() && settings_page() == p) {
        developer_page = 0;
        __atomic_store_n(&panel_open, 0, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&page, p, __ATOMIC_RELEASE);
    __atomic_store_n(&panel_open, 1, __ATOMIC_RELEASE);
}

void settings_action(int action) {
    if (action == SETTINGS_PAGE) {
        bindings_cancel();
        __atomic_store_n(&page, !settings_page(), __ATOMIC_RELEASE);
        return;
    }
    if (action == SETTINGS_CLOSE) {
        bindings_cancel();
        if (!settings_page() && developer_page) {  /* Escape or B on the developer page: back */
            developer_page = 0;
            return;
        }
        __atomic_store_n(&panel_open, 0, __ATOMIC_RELEASE);
        return;
    }
    if (settings_page()) {
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
    if ((action == SETTINGS_LEFT || action == SETTINGS_RIGHT) && s->disabled && s->disabled()) return;  /* greyed out */
    if (!s->noptions) {  /* a slider */
        if (action == SETTINGS_LEFT || action == SETTINGS_RIGHT) slider_step(s, action == SETTINGS_RIGHT ? 1 : -1), settings_save();
        return;
    }
    switch (action) {
    case SETTINGS_RIGHT: s->next(), settings_save(); break;
    case SETTINGS_LEFT:  /* one step back: the cycle goes forward only */
        for (int i = 0; i < s->noptions - 1; i++) s->next();
        settings_save();
        break;
    }
}

int settings_developer(void) { return developer_page; }

static int fill_rows(SettingRow *rows, int max, const Setting *list, int count) {
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) {
        memset(&rows[i], 0, sizeof rows[i]);
        rows[i].category = list[i].category, rows[i].name = list[i].name, rows[i].help = list[i].help;
        memcpy(rows[i].options, list[i].options, sizeof rows[i].options);
        memcpy(rows[i].notes, list[i].notes, sizeof rows[i].notes);
        rows[i].noptions = list[i].noptions;
        if (!list[i].noptions) {  /* a slider: its text, its position, and one note for the description */
            float v = list[i].get();
            rows[i].slider = 1;
            rows[i].frac = v <= 0 ? 0.0f : (v - list[i].lo) / (list[i].hi - list[i].lo);
            slider_text(&list[i], rows[i].value, sizeof rows[i].value, NULL, 0);
            rows[i].options[0] = rows[i].value;
            rows[i].notes[0] = v <= 0 ? list[i].auto_note : "";
            rows[i].current = 0;
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

/* The HUD's sliders live on the overlay's Overlay page, next to their preview, so its Settings page leaves them out
 * (the old panel keeps them). */
/* The overlay's Look > Theme row (theme.h): before Advanced, overlay only (the old panel's pixel font has no lower
 * case for the names); its value one theme's name, Left / Right step through them. */
static int theme_row(void) {
    int n = 0;
    for (int i = 0; i < NPLAYER && strcmp(player[i].category, "ADVANCED"); i++) n += !hud_slider_row(&player[i]);
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

int settings_player_rows(SettingRow *rows, int max) {
    int n = 0, at = theme_row();
    for (int i = 0; i < NPLAYER && n < max; i++) {
        if (n == at && !strcmp(player[i].category, "ADVANCED")) fill_theme_row(&rows[n++]);
        if (n < max && !hud_slider_row(&player[i])) n += fill_rows(rows + n, 1, &player[i], 1);
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

void settings_change(int row, int dir) {
    int at = theme_row();
    if (row == at) {  /* Look > Theme */
        theme_step(dir);
        settings_save();
        return;
    }
    if (row > at) row--;
    const Setting *s = NULL;
    for (int i = 0, seen = 0; i < NPLAYER && !s; i++)
        if (!hud_slider_row(&player[i]) && seen++ == row) s = &player[i];
    if (!s) return;
    if (s->disabled && s->disabled()) return;  /* greyed out */
    if (!s->noptions) slider_step(s, dir > 0 ? 1 : -1);
    else for (int i = 0; i < (dir > 0 ? 1 : s->noptions - 1); i++) s->next();  /* the cycle goes forward only */
    settings_save();
}
