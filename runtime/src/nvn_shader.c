/* Which translated shader a program stage is (decision 0020), and NVN usage statistics.
 *
 * The control section the game passes to nvnProgramSetShaders holds the NV_gpu_program5
 * assembly the stage was compiled from. Its FNV-1a 64 hash, from "!!NV" through "END",
 * names the translated shader: <work_dir>/shaders/glsl/<key>.<stage> (recomp/shader.py).
 *
 * YW4_NVN_STATS=1 prints, every 300 frames, how the game feeds the GPU: memory pools by
 * flags, texture uploads, draws and programs, and whether every program's shader exists in
 * YW4_SHADER_DIR. Measurements for the draw path's design; not needed to run.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define SCAN_LIMIT (1u << 20)  /* the assembly sits within the first KiBs of a control section */

uint64_t nvn_shader_key(uint64_t control) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)control;
    uint32_t a = 0;
    while (a + 4 <= SCAN_LIMIT && memcmp(p + a, "!!NV", 4)) a++;
    if (a + 4 > SCAN_LIMIT) return 0;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = a; i < a + SCAN_LIMIT; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
        /* "\nEND" not followed by a capital letter (ENDIF, ENDREP) ends the program */
        if (i >= a + 3 && !memcmp(p + i - 3, "\nEND", 4) && !(p[i + 1] >= 'A' && p[i + 1] <= 'Z')) return h;
    }
    return 0;
}

/* ---- statistics */

static int stats_on = -1;
int nvn_stats_enabled(void) {
    if (stats_on < 0) {
        const char *e = getenv("YW4_NVN_STATS");
        stats_on = e && *e && *e != '0';
    }
    return stats_on;
}

enum { MAX_KEYS = 4096, MAX_POOL_KINDS = 16 };
static uint64_t keys[MAX_KEYS];
static uint8_t key_known[MAX_KEYS];
static int nkeys, nmissing;
static struct { uint32_t flags, count; uint64_t bytes; } pools[MAX_POOL_KINDS];
static int npool_kinds;
static uint64_t texel_writes, texel_bytes, binds, draws_by_kind[4];
static uint64_t frame_programs[256];
static int nframe_programs;

/* Remembers a shader key; checks once whether its translation exists. */
void nvn_stats_shader(uint64_t key, const char *stage) {
    if (!nvn_stats_enabled() || !key) return;
    for (int i = 0; i < nkeys; i++)
        if (keys[i] == key) return;
    if (nkeys == MAX_KEYS) return;
    const char *dir = getenv("YW4_SHADER_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/%016" PRIx64 ".%s", dir ? dir : "work/shaders/glsl", key, stage);
    FILE *f = fopen(path, "rb");
    key_known[nkeys] = f != NULL;
    if (f) fclose(f);
    else if (nmissing++ < 10) fprintf(stderr, "yw4: NVN stats: no translated shader %s\n", path);
    keys[nkeys++] = key;
}

void nvn_stats_pool(uint32_t flags, uint64_t size) {
    if (!nvn_stats_enabled()) return;
    int i = 0;
    while (i < npool_kinds && pools[i].flags != flags) i++;
    if (i == MAX_POOL_KINDS) return;
    if (i == npool_kinds) pools[npool_kinds++].flags = flags;
    pools[i].count++, pools[i].bytes += size;
}

void nvn_stats_texels(uint64_t bytes) { texel_writes++, texel_bytes += bytes; }

/* kind: 0 draw, 1 indexed draw, 2 indirect draw, 3 dispatch */
void nvn_stats_draw(int kind) { draws_by_kind[kind]++; }

void nvn_stats_program(const void *program) {
    binds++;
    uint64_t p = (uint64_t)(uintptr_t)program;
    for (int i = 0; i < nframe_programs; i++)
        if (frame_programs[i] == p) return;
    if (nframe_programs < 256) frame_programs[nframe_programs++] = p;
}

void nvn_stats_frame(uint64_t frame) {
    if (!nvn_stats_enabled()) return;
    if (frame % 300 == 0) {
        int known = 0;
        for (int i = 0; i < nkeys; i++) known += key_known[i];
        fprintf(stderr, "yw4: NVN stats frame %" PRIu64 ": shaders seen %d (translated %d); last frame: %" PRIu64
                " draws, %" PRIu64 " indexed, %" PRIu64 " indirect, %" PRIu64 " dispatches, %" PRIu64
                " program binds, %d programs; since last: %" PRIu64 " WriteTexels (%" PRIu64 " KiB)\n",
                frame, nkeys, known, draws_by_kind[0], draws_by_kind[1], draws_by_kind[2], draws_by_kind[3], binds,
                nframe_programs, texel_writes, texel_bytes >> 10);
        for (int i = 0; i < npool_kinds; i++)
            fprintf(stderr, "yw4: NVN stats   pools flags %#x: %u, %" PRIu64 " MiB\n", pools[i].flags, pools[i].count,
                    pools[i].bytes >> 20);
        texel_writes = texel_bytes = 0;
    }
    memset(draws_by_kind, 0, sizeof draws_by_kind);
    binds = 0, nframe_programs = 0;
}
