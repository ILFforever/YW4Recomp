/* The settings panel's list (settings.h): each row reads its setting's current option and steps it with
 * the setting's own cycle function, so the panel and the F keys always agree. */
#include <string.h>

#include "gbuffer.h"
#include "gpu.h"
#include "settings.h"
#include "shadow.h"

const char *shadow_mode_name(void);

typedef struct {
    const char *name;
    const char *options[SETTINGS_MAX_OPTIONS];
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

static const Setting settings[] = {
    {"3D VIEW", {"1600X900", "1920X1080", "2560X1440", "3840X2160"}, 4, res_index, gpu_res_cycle},
    {"TEXTURE FILTERING", {"GAME", "X2", "X4", "X8", "X16"}, 5, aniso_index, gpu_aniso_cycle},
    {"DISTANCE BLUR", {"GAME", "HALF", "OFF"}, 3, dof_index, gpu_dof_cycle},
    {"EFFECTS RESOLUTION", {"HALF", "FULL"}, 2, gpu_effects_full, gpu_effects_cycle},
    {"SHADOW MAP", {"1024", "2048", "4096"}, 3, shadow_res_index, gpu_shadow_res_cycle},
    {"SOFT SHADOWS", {"OFF", "X1", "X2", "X4"}, 4, pcss_index, gpu_pcss_cycle},
    {"SHADOW PASS", {"GAME", "OURS"}, 2, shadow_pass_index, shadow_toggle},
    {"G-BUFFER", {"GAME", "OURS"}, 2, gbuffer_index, gbuffer_toggle},
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
        rows[i].name = settings[i].name;
        memcpy(rows[i].options, settings[i].options, sizeof rows[i].options);
        rows[i].noptions = settings[i].noptions;
        rows[i].current = settings[i].current();
    }
    *sel = selected;
    return n;
}
