/* The settings panel's list (settings.h): each row reads its setting's current option and steps it with
 * the setting's own cycle function, so the panel and the replays' keys always agree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_filesystem.h>

#include "bindings.h"
#include "gbuffer.h"
#include "gpu.h"
#include "settings.h"
#include "shadow.h"

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
} Setting;

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

/* In the order shown; a row starts a new group when its category differs from the row before. The texts
 * use the HUD font's characters (hud.c): upper case, digits, . , : % / - ( ) ' ! ? */
static const Setting settings[] = {
    {"PICTURE", "3D VIEW",
     "THE RESOLUTION THE WORLD IS DRAWN AT. HIGHER IS SHARPER AND COSTS GPU TIME. MENUS AND TEXT ARE DRAWN AT THE "
     "SAME SIZE.",
     {"1600X900", "1920X1080", "2560X1440", "3840X2160"},
     {"THE SWITCH'S RESOLUTION", "FULL HD", "SHARP, A LITTLE GPU TIME", "SHARPEST, THE MOST GPU TIME"}, 4,
     res_index, gpu_res_cycle, "YW4_RES", {"900", "1080", "1440", "2160"}},
    {"PICTURE", "ANTI-ALIASING",
     "SMOOTHS THE JAGGED EDGES OF OBJECTS. APPLIED BEFORE THE MENUS ARE DRAWN, SO TEXT STAYS SHARP.",
     {"OFF", "FXAA", "SMAA", "TAA"},
     {"THE GAME'S EDGES", "SMOOTHEST, TEXTURES A LITTLE SOFTER", "CLEAN EDGES, TEXTURES STAY SHARP",
      "STEADY EDGES: NO SHIMMER ON FENCES, LEAVES AND SHADOWS"}, 4, gpu_aa,
     gpu_aa_cycle, "YW4_AA", {"0", "1", "2", "3"}},
    {"PICTURE", "TEXTURE FILTERING",
     "KEEPS GROUND, ROADS AND WALLS SHARP WHEN SEEN FROM AFAR OR AT AN ANGLE.",
     {"GAME", "X2", "X4", "X8", "X16"},
     {"THE GAME'S: BLURRY IN THE DISTANCE", "A LITTLE SHARPER", "SHARPER", "SHARPER STILL", "SHARPEST, NO REAL COST"},
     5, aniso_index, gpu_aniso_cycle, "YW4_ANISO", {"0", "2", "4", "8", "16"}},
    {"EFFECTS", "DISTANCE BLUR",
     "THE GAME BLURS FAR AWAY THINGS ON PURPOSE, FOR A SOFT, MINIATURE LOOK.",
     {"GAME", "HALF", "OFF"},
     {"AS ON THE SWITCH", "A LIGHTER BLUR", "EVERYTHING SHARP"}, 3, dof_index, gpu_dof_cycle, "YW4_DOF", {"2", "1", "0"}},
    {"EFFECTS", "EFFECTS RESOLUTION",
     "SHADOWS, LAMP LIGHT AND SHADING ARE DRAWN AT HALF SIZE, THEN SPREAD OVER THE PICTURE.",
     {"HALF", "FULL"},
     {"THE GAME'S: SOFTER, BLOCKY EDGES", "CLEANER SHADOW EDGES, ABOUT 2 MS"}, 2, gpu_effects_full,
     gpu_effects_cycle, "YW4_EFFECTS", {"0", "1"}},
    {"SHADOWS", "SHADOW MAP",
     "HOW DETAILED SHADOWS ARE. BIGGER IS SHARPER, MOST OF ALL FAR FROM THE CAMERA.",
     {"1024", "2048", "4096"},
     {"THE GAME'S", "SHARPER", "SHARPEST (RECOMMENDED)"}, 3, shadow_res_index, gpu_shadow_res_cycle, "YW4_SHADOW_RES", {"1024", "2048", "4096"}},
    {"SHADOWS", "SOFT SHADOWS",
     "SHADOWS GET SOFTER THE FURTHER THEY FALL FROM WHAT CASTS THEM, LIKE REAL ONES. TREE SHADE STOPS "
     "SHIMMERING.",
     {"OFF", "X1", "X2", "X4"},
     {"THE GAME'S HARD SHADOWS", "SLIGHTLY SOFT", "SOFT (RECOMMENDED)", "VERY SOFT"}, 4, pcss_index, gpu_pcss_cycle, "YW4_PCSS", {"0", "1", "2", "3"}},
    {"RENDERER", "SHADOW PASS",
     "WHO DRAWS WHAT CASTS SHADOWS: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER, A LITTLE FASTER"}, 2, shadow_pass_index, shadow_toggle, "YW4_SHADOWS", {"game", "native"}},
    {"RENDERER", "G-BUFFER",
     "WHO DRAWS THE WORLD'S SURFACES: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME. OURS LETS FUTURE "
     "UPGRADES IN.",
     {"GAME", "CUSTOM"},
     {"THE GAME'S RENDERER", "OUR CUSTOM RENDERER"}, 2, gbuffer_index, gbuffer_toggle, "YW4_GBUFFER", {"game", "native"}},
};
enum { NSETTINGS = sizeof settings / sizeof *settings };

static int panel_open, page, selected;

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
            if (!strcmp(line, settings[i].env) && !getenv(line)) set_env(line, value), n++;
        n += bindings_load_line(line, value);
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
        int c = settings[i].current();
        if (c >= 0 && c < settings[i].noptions) fprintf(f, "%s=%s\n", settings[i].env, settings[i].values[c]);
    }
    bindings_save(f);
    fclose(f);
}

int settings_open(void) { return __atomic_load_n(&panel_open, __ATOMIC_ACQUIRE); }

void settings_toggle(void) {
    bindings_cancel();
    __atomic_store_n(&panel_open, !settings_open(), __ATOMIC_RELEASE);
}

int settings_page(void) { return __atomic_load_n(&page, __ATOMIC_ACQUIRE); }

void settings_show(int p) {
    bindings_cancel();
    if (settings_open() && settings_page() == p) {
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
        __atomic_store_n(&panel_open, 0, __ATOMIC_RELEASE);
        return;
    }
    if (settings_page()) {
        bindings_action(action);
        return;
    }
    const Setting *s = &settings[selected];
    switch (action) {
    case SETTINGS_UP: selected = (selected + NSETTINGS - 1) % NSETTINGS; break;
    case SETTINGS_DOWN: selected = (selected + 1) % NSETTINGS; break;
    case SETTINGS_RIGHT: s->next(), settings_save(); break;
    case SETTINGS_LEFT:  /* one step back: the cycle goes forward only */
        for (int i = 0; i < s->noptions - 1; i++) s->next();
        settings_save();
        break;
    }
}

int settings_rows(SettingRow *rows, int max, int *sel) {
    int n = NSETTINGS < max ? NSETTINGS : max;
    for (int i = 0; i < n; i++) {
        rows[i].category = settings[i].category, rows[i].name = settings[i].name, rows[i].help = settings[i].help;
        memcpy(rows[i].options, settings[i].options, sizeof rows[i].options);
        memcpy(rows[i].notes, settings[i].notes, sizeof rows[i].notes);
        rows[i].noptions = settings[i].noptions;
        rows[i].current = settings[i].current();
    }
    *sel = selected;
    return n;
}
