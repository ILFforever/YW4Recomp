/* What Discord Rich Presence shows about the game (decision 0077): read on the game's thread, handed to the presence
 * thread (discord.c) under a lock.
 *
 * Where the values are: the game data object at *(*(main+0x1548fd8) + 0x5778), the one the game copies into the save
 * summary (HEADERSAVE) when it saves (f_32ec30, main+0x32ec30): +0x17 the Watch Rank (one byte, 3 = B), +0x30 the
 * current map's ID. The save summary turns the map ID into a location text ID with f_70ff90 (main+0x70ff90, takes a
 * pointer to the ID; nonzero: that text) or else f_70fdc0 (main+0x70fdc0); the Diary (f_761aa0) looks the text up with
 * f_9b10d0(table, text, 0) in the text tables at *(G + 8) then *(G + 0), each used only once its vtable's slot 3 (+0x18)
 * says it is ready; the entry's first word is the string (UTF-8; 0: none).
 *
 * Those are the game's functions, so they run here, in the field's player controller hook on the game's thread, and
 * only when the map ID changed, not on the presence thread. */
#include "discord_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "battle.h"
#include "host.h"
#include "pause.h"
#include "relay.h"
#include "yw4load.h"

enum {
    GAME_PTR = 0x1548fd8,   /* G: the game's globals */
    G_DATA = 0x5778,        /* G + this: the game data object */
    DATA_RANK = 0x17,       /* u8 */
    DATA_MAP = 0x30,        /* u32 */
    FN_MAP_TEXT_SPECIAL = 0x70ff90,
    FN_MAP_TEXT = 0x70fdc0,
    FN_TEXT_FIND = 0x9b10d0,
};

static HostLock lock = HOST_LOCK_INIT;
static char area[DISCORD_AREA_MAX];  /* under lock */
static char sub[DISCORD_AREA_MAX];   /* under lock: the zone's name, "" none */
static int rank = -1;                /* under lock */
static uint32_t last_map;
static int have_map;

/* Guest stack below the hook's frame: arguments passed by pointer, then the called functions' own frames. */
enum { STACK_ARG = 0x10, STACK_BUF = 0x300, BUF_SIZE = 0x100, STACK_CALL = 0x400 };

/* Calls a game function with x0..x3 on a frame below the hook's stack; returns x0. ctx is put back. */
static uint64_t guest_call4(CPUState *ctx, uint64_t fn_addr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    GuestFn fn = yw4_lookup(fn_addr);
    if (!fn) return 0;
    CPUState saved = *ctx;
    ctx->sp = (saved.sp - STACK_CALL) & ~(uint64_t)15;
    ctx->x[0] = a0, ctx->x[1] = a1, ctx->x[2] = a2, ctx->x[3] = a3;
    ctx->x[30] = saved.x[30];
    fn(ctx);
    uint64_t r = ctx->x[0];
    *ctx = saved;
    return r;
}
static uint64_t guest_call(CPUState *ctx, uint64_t fn_addr, uint64_t a0, uint64_t a1, uint64_t a2) {
    return guest_call4(ctx, fn_addr, a0, a1, a2, 0);
}

/* The text's string from one of the game's text tables (as the Diary does it), or NULL. */
static const char *text_in(CPUState *ctx, uint64_t table, uint32_t text) {
    if (!table) return NULL;
    uint64_t vt = *(const uint64_t *)(uintptr_t)table;
    if (!vt || !(guest_call(ctx, *(const uint64_t *)(uintptr_t)(vt + 0x18), table, 0, 0) & 1)) return NULL;
    uint64_t e = guest_call(ctx, yw4_image_base + FN_TEXT_FIND, table, text, 0);
    if (!e) return NULL;
    uint64_t s = *(const uint64_t *)(uintptr_t)e;
    return s ? (const char *)(uintptr_t)s : NULL;
}

/* Copies at most n - 1 bytes, not cutting a UTF-8 character in two. */
static void copy_utf8(char *out, size_t n, const char *s) {
    size_t k = strlen(s);
    if (k >= n) {
        k = n - 1;
        while (k > 0 && ((unsigned char)s[k] & 0xc0) == 0x80) k--;  /* s[k] would start the cut character */
    }
    memcpy(out, s, k);
    out[k] = 0;
}

/* ---- The zone (sub-area: "Meeting Bridge" in Springdale City), as the expanded minimap's label finds it
 * (f_758430, main+0x758430; read 2026-11-18). The field manager FM = *(main+0x154b190); the zone slot is the byte at
 * the game data object's +0x301; FM + slot*0x14 + 0x44 is the zone's handle (low 16 bits an index below 0x200,
 * checked against FM + index*0xa60 + 0x148), whose record +0x15c is a block (below 0x400) at FM + block*0x100:
 * +0x18dab0 is -1 for none, +0x18db90 the zone's number. The map's internal name comes from f_4d8e60(*(G+0x230),
 * map ID of FM's current map: FM + *(FM+0x90)*0xb0 + 0x161b34) in the name table at *(*(G+0x230)+0x38). The label's
 * text ID is the CRC-32 (the game's table at main+0x12bd348) of sprintf(main+0x12524ff, map name, number). No such
 * text: the map's own name, as now. */
enum {
    FIELD_PTR = 0x154b190,
    DATA_ZONE_SLOT = 0x301,
    FN_MAP_RECORD = 0x4d8e60,
    FN_SPRINTF = 0x14b9c0,
    ZONE_FORMAT = 0x12524ff,
    CRC_TABLE = 0x12bd348,
};

typedef struct {
    uint32_t handle, block, map_id, number;  /* handle 0: not in a zone */
} Zone;
static Zone last_zone;  /* the game's thread only */

static int read_zone(uint64_t data, Zone *z) {
    memset(z, 0, sizeof *z);
    uint64_t fm = *(const uint64_t *)(uintptr_t)(yw4_image_base + FIELD_PTR);
    if (!fm) return 0;
    unsigned slot = *(const uint8_t *)(uintptr_t)(data + DATA_ZONE_SLOT);
    uint32_t h = *(const uint32_t *)(uintptr_t)(fm + slot * 0x14ull + 0x44);
    if (!h || (h & 0xffff) > 0x1ff) return 1;
    uint64_t rec = fm + (h & 0xffff) * 0xa60ull;
    if (*(const uint32_t *)(uintptr_t)(rec + 0x148) != h) return 1;
    uint32_t block = *(const uint32_t *)(uintptr_t)(rec + 0x15c);
    if (block > 0x3ff) return 1;
    uint64_t zb = fm + ((uint64_t)block << 8);
    if (*(const int32_t *)(uintptr_t)(zb + 0x18dab0) == -1) return 1;
    uint32_t mi = *(const uint32_t *)(uintptr_t)(fm + 0x90);
    z->handle = h, z->block = block;
    z->map_id = mi > 0x3ff ? 0 : *(const uint32_t *)(uintptr_t)(fm + mi * 0xb0ull + 0x161b34);
    z->number = *(const uint32_t *)(uintptr_t)(zb + 0x18db90);
    return 1;
}

/* The zone's name, or NULL (no zone, or a zone without a name of its own). */
static const char *zone_name(CPUState *ctx, uint64_t g, const Zone *z) {
    if (!z->handle) return NULL;
    uint64_t names = *(const uint64_t *)(uintptr_t)(g + 0x230);
    if (!names) return NULL;
    uint64_t r = guest_call(ctx, yw4_image_base + FN_MAP_RECORD, names, z->map_id, 0);
    if (!r) return NULL;
    int32_t off = *(const int32_t *)(uintptr_t)(r + 4);
    uint64_t str = 0;
    if (off >= 0) {
        uint64_t t = *(const uint64_t *)(uintptr_t)(names + 0x38);
        str = t + *(const uint16_t *)(uintptr_t)(t + 0xa) * 4ull + *(const uint32_t *)(uintptr_t)(t + 0x38) + (uint64_t)off;
    }
    uint64_t buf = (ctx->sp - STACK_BUF) & ~(uint64_t)15;
    memset((void *)(uintptr_t)buf, 0, BUF_SIZE);
    guest_call4(ctx, yw4_image_base + FN_SPRINTF, buf, yw4_image_base + ZONE_FORMAT, str, z->number);
    const uint8_t *p = (const uint8_t *)(uintptr_t)buf;
    if (!*p) return NULL;
    const uint32_t *table = (const uint32_t *)(uintptr_t)(yw4_image_base + CRC_TABLE);
    uint32_t crc = ~0u;
    for (int i = 0; i < BUF_SIZE && p[i]; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    uint32_t text = ~crc;
    const char *s = text_in(ctx, *(const uint64_t *)(uintptr_t)(g + 8), text);
    if (!s) s = text_in(ctx, *(const uint64_t *)(uintptr_t)g, text);
    if (getenv("YW4_DISCORD_LOG"))
        fprintf(stderr, "yw4: discord: zone '%s' (map %#x) -> text %#x '%s'\n", (const char *)p, z->map_id, text,
                s ? s : "");
    return s && *s ? s : NULL;
}

void discord_game_field_frame(CPUState *ctx) {
    if (!discord_enabled()) return;
    uint64_t g = *(const uint64_t *)(uintptr_t)(yw4_image_base + GAME_PTR);
    uint64_t data = g ? *(const uint64_t *)(uintptr_t)(g + G_DATA) : 0;
    if (!data) return;
    int r = *(const uint8_t *)(uintptr_t)(data + DATA_RANK);
    uint32_t map = *(const uint32_t *)(uintptr_t)(data + DATA_MAP);
    /* 0 while a save is still loading (its rank too: 0 = E); its "text" is some other string (seen 2026-11-18) */
    if (!map) return;
    Zone z;
    read_zone(data, &z);
    /* on the way to another area the field's map (and its zone) turns over a moment before the data's map ID: the
     * zone waits until both name the same map, so the card never shows the new street in the old area */
    if (z.handle && z.map_id != map) {
        if (map != last_map) memset(&z, 0, sizeof z);  /* a new area: its own name for now */
        else z = last_zone;
    }
    if (__atomic_load_n(&have_map, __ATOMIC_ACQUIRE) && map == last_map) {
        const char *zn = NULL;
        int zone_changed = memcmp(&z, &last_zone, sizeof z) != 0;
        if (zone_changed) last_zone = z, zn = zone_name(ctx, g, &z);
        host_lock(&lock);
        rank = r;
        if (zone_changed) copy_utf8(sub, sizeof sub, zn ? zn : "");
        host_unlock(&lock);
        if (zone_changed) fprintf(stderr, "yw4: discord: zone '%s'\n", zn ? zn : "");
        return;
    }
    last_zone = z;
    const char *zn = zone_name(ctx, g, &z);
    last_map = map;
    __atomic_store_n(&have_map, 1, __ATOMIC_RELEASE);
    /* the map ID -> its location text, as the save summary does */
    uint64_t arg = (ctx->sp - STACK_ARG) & ~(uint64_t)15;  /* above the called functions' frames */
    *(uint32_t *)(uintptr_t)arg = map;
    uint32_t text = (uint32_t)guest_call(ctx, yw4_image_base + FN_MAP_TEXT_SPECIAL, arg, 0, 0);
    if (!text) text = (uint32_t)guest_call(ctx, yw4_image_base + FN_MAP_TEXT, map, 0, 0);
    const char *name = NULL;
    if (text) {
        name = text_in(ctx, *(const uint64_t *)(uintptr_t)(g + 8), text);
        if (!name) name = text_in(ctx, *(const uint64_t *)(uintptr_t)g, text);
    }
    host_lock(&lock);
    copy_utf8(area, sizeof area, name ? name : "");
    copy_utf8(sub, sizeof sub, zn ? zn : "");
    rank = r;
    host_unlock(&lock);
    fprintf(stderr, "yw4: discord: area '%s' (map %#x, text %#x), zone '%s', watch rank %d\n", name ? name : "", map,
            text, zn ? zn : "", r);
}

void discord_game_read(DiscordGame *out) {
    memset(out, 0, sizeof *out);
    out->rank = -1;
    if (scene_on_title()) {  /* the next game loaded may be another save: its area is looked up anew */
        out->where = DISCORD_AT_TITLE;
        host_lock(&lock);
        area[0] = sub[0] = 0, rank = -1;
        host_unlock(&lock);
        __atomic_store_n(&have_map, 0, __ATOMIC_RELEASE);
        return;
    }
    host_lock(&lock);
    copy_utf8(out->area, sizeof out->area, area);
    copy_utf8(out->sub, sizeof out->sub, sub);
    out->rank = rank;
    host_unlock(&lock);
    out->where = battle_active() ? DISCORD_AT_BATTLE : out->area[0] || out->rank >= 0 ? DISCORD_AT_FIELD : DISCORD_AT_START;
    /* in a local play room (ldn.c): that first. A trade's room is scene 1 for 2 (measured 2026-11-19), a Blasters
     * team's scene 3 for 4 (2026-11-20) */
    int scene = 0;
    if (ldn_room(&out->players, &out->max_players, &scene)) {
        out->where = scene == 1 && out->max_players == 2 ? DISCORD_AT_TRADE
                     : scene == 3                        ? DISCORD_AT_BLASTERS
                                                         : DISCORD_AT_TOGETHER;
        out->mission = battle_busters();
    }
}
