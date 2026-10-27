/* The settings panel's list (settings.h): each row reads its setting's current option and steps it with
 * the setting's own cycle function, so the panel and the F keys always agree. */
#include <string.h>

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
     res_index, gpu_res_cycle},
    {"PICTURE", "ANTI-ALIASING",
     "SMOOTHS THE JAGGED EDGES OF OBJECTS. APPLIED BEFORE THE MENUS ARE DRAWN, SO TEXT STAYS SHARP.",
     {"OFF", "FXAA", "SMAA"},
     {"THE GAME'S EDGES", "SMOOTHEST, TEXTURES A LITTLE SOFTER", "CLEAN EDGES, TEXTURES STAY SHARP"}, 3, gpu_aa,
     gpu_aa_cycle},
    {"PICTURE", "TEXTURE FILTERING",
     "KEEPS GROUND, ROADS AND WALLS SHARP WHEN SEEN FROM AFAR OR AT AN ANGLE.",
     {"GAME", "X2", "X4", "X8", "X16"},
     {"THE GAME'S: BLURRY IN THE DISTANCE", "A LITTLE SHARPER", "SHARPER", "SHARPER STILL", "SHARPEST, NO REAL COST"},
     5, aniso_index, gpu_aniso_cycle},
    {"EFFECTS", "DISTANCE BLUR",
     "THE GAME BLURS FAR AWAY THINGS ON PURPOSE, FOR A SOFT, MINIATURE LOOK.",
     {"GAME", "HALF", "OFF"},
     {"AS ON THE SWITCH", "A LIGHTER BLUR", "EVERYTHING SHARP"}, 3, dof_index, gpu_dof_cycle},
    {"EFFECTS", "EFFECTS RESOLUTION",
     "SHADOWS, LAMP LIGHT AND SHADING ARE DRAWN AT HALF SIZE, THEN SPREAD OVER THE PICTURE.",
     {"HALF", "FULL"},
     {"THE GAME'S: SOFTER, BLOCKY EDGES", "CLEANER SHADOW EDGES, ABOUT 2 MS"}, 2, gpu_effects_full,
     gpu_effects_cycle},
    {"SHADOWS", "SHADOW MAP",
     "HOW DETAILED SHADOWS ARE. BIGGER IS SHARPER, MOST OF ALL FAR FROM THE CAMERA.",
     {"1024", "2048", "4096"},
     {"THE GAME'S", "SHARPER", "SHARPEST (RECOMMENDED)"}, 3, shadow_res_index, gpu_shadow_res_cycle},
    {"SHADOWS", "SOFT SHADOWS",
     "SHADOWS GET SOFTER THE FURTHER THEY FALL FROM WHAT CASTS THEM, LIKE REAL ONES. TREE SHADE STOPS "
     "SHIMMERING.",
     {"OFF", "X1", "X2", "X4"},
     {"THE GAME'S HARD SHADOWS", "SLIGHTLY SOFT", "SOFT (RECOMMENDED)", "VERY SOFT"}, 4, pcss_index, gpu_pcss_cycle},
    {"RENDERER", "SHADOW PASS",
     "WHO DRAWS WHAT CASTS SHADOWS: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME.",
     {"GAME", "OURS"},
     {"THE GAME'S RENDERER", "OUR RENDERER, A LITTLE FASTER"}, 2, shadow_pass_index, shadow_toggle},
    {"RENDERER", "G-BUFFER",
     "WHO DRAWS THE WORLD'S SURFACES: THE GAME OR OUR OWN RENDERER. THE PICTURE IS THE SAME. OURS LETS FUTURE "
     "UPGRADES IN.",
     {"GAME", "OURS"},
     {"THE GAME'S RENDERER", "OUR RENDERER"}, 2, gbuffer_index, gbuffer_toggle},
};
enum { NSETTINGS = sizeof settings / sizeof *settings };

static int open, selected;

int settings_open(void) { return __atomic_load_n(&open, __ATOMIC_ACQUIRE); }

void settings_toggle(void) { __atomic_store_n(&open, !settings_open(), __ATOMIC_RELEASE); }

void settings_action(int action) {
    const Setting *s = &settings[selected];
    switch (action) {
    case SETTINGS_UP: selected = (selected + NSETTINGS - 1) % NSETTINGS; break;
    case SETTINGS_DOWN: selected = (selected + 1) % NSETTINGS; break;
    case SETTINGS_RIGHT: s->next(); break;
    case SETTINGS_LEFT:  /* one step back: the cycle goes forward only */
        for (int i = 0; i < s->noptions - 1; i++) s->next();
        break;
    case SETTINGS_CLOSE: __atomic_store_n(&open, 0, __ATOMIC_RELEASE); break;
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
