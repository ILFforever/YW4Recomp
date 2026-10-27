/* Widescreen (decision 0033): the 3D view in another shape than the game's 16:9, with the UI kept 16:9 in the
 * middle.
 *
 * The camera: every frame f_97f510 walks the engine's cameras (array at [main+0x5865720], count at
 * main+0x5865728) and copies each one's settings into its working fields: +0x2e4 field of view (vertical, 45
 * degrees in town) to +0x1a4, +0x2e8 aspect (16/9) to +0x1a8, near and far. The projection is built from the
 * setting (+0x2e8: changing only +0x1a8 changed nothing), so the hook sets +0x2e8 before the copy. The
 * vertical view stays the same and the sides grow; the game's culling follows the camera's projection.
 *
 * The images (gpu.c): every screen-sized target is made wider (or narrower) by the factor below on top of the
 * resolution's; viewports and scissors follow, the UI draws keep the game's width in the middle (gpu_draw.c).
 * YW4_ASPECT=16:9 / 16:10 / 21:9 / 32:9 at start; the settings panel changes it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "yw4load.h"
#include "yw4rt.h"

static const struct {
    const char *name;
    float aspect;
    uint32_t num, den;  /* the width factor against 16:9 */
} shapes[] = {{"16:9", 16.0f / 9.0f, 1, 1}, {"16:10", 1.6f, 9, 10}, {"21:9", 64.0f / 27.0f, 4, 3}, {"32:9", 32.0f / 9.0f, 2, 1}};
enum { SHAPES = sizeof shapes / sizeof *shapes };

static int shape = -1;

int gpu_wide(void) {
    int s = __atomic_load_n(&shape, __ATOMIC_ACQUIRE);
    if (s < 0) {
        const char *e = getenv("YW4_ASPECT");
        s = 0;
        for (int i = 0; e && i < SHAPES; i++)
            if (!strcmp(e, shapes[i].name)) s = i;
        __atomic_store_n(&shape, s, __ATOMIC_RELEASE);
    }
    return s;
}

void gpu_wide_cycle(void) {
    int s = (gpu_wide() + 1) % SHAPES;
    __atomic_store_n(&shape, s, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: picture shape %s\n", shapes[s].name);
}

void gpu_wide_factor(uint32_t *num, uint32_t *den) {
    int s = gpu_wide();
    *num = shapes[s].num, *den = shapes[s].den;
}

/* Is v one of the shapes' aspects (the game's 16:9, or one we set)? */
static int ours(float v) {
    for (int i = 0; i < SHAPES; i++)
        if (v > shapes[i].aspect - 0.0005f && v < shapes[i].aspect + 0.0005f) return 1;
    return 0;
}

/* hook on f_97f510 (recomp/overrides.toml), before its body: the cameras whose aspect setting is 16:9 or one
 * of ours get the chosen one (so going back to 16:9 restores them); other aspects are the game's own. */
int yw4_hook_camera_update(CPUState *ctx) {
    (void)ctx;
    uint64_t main_base = *yw4_modules[YW4_MAIN].base;
    uint64_t list = *(const uint64_t *)(uintptr_t)(main_base + 0x5865720);
    uint32_t n = *(const uint32_t *)(uintptr_t)(main_base + 0x5865728);
    float aspect = shapes[gpu_wide()].aspect;
    for (uint32_t i = 0; list && i < n; i++) {
        uint64_t cam = *(const uint64_t *)(uintptr_t)(list + 8ull * i);
        float *set = cam ? (float *)(uintptr_t)(cam + 0x2e8) : NULL;
        if (set && *set != aspect && ours(*set)) *set = aspect;
    }
    return 0;  /* the body copies it into the camera's projection fields */
}
