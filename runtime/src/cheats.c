/* The game's "save one byte by ID" script command (main+0x296560, registered at startup as command 0xd0c06ed4 in the
 * table main+0xe7b0 fills): a script passes an ID and a value; the ID is found in a sorted table of (ID u32, slot
 * u16) pairs, and the value is stored as one byte at [save data + slot + 0x288]. The Crank-a-kai menu's script keeps
 * the day's cranks left this way (found 2026-11-15: one write per crank, 29 28 27; the save holds the count at
 * offset 0x822 of AUTOSAVE/data.bin with the day's limit, 30, in the next byte).
 *
 * The hook runs just before the store (main+0x2966a8): x9 holds the ID, x20 the new value, x8 the table entry.
 *
 * YW4_TRACE_SAVEBYTE=1: every store that changes a byte is logged (ID, slot, old -> new); unchanged ones are counted
 * and summed up every 1000. Tests: which IDs go through the command. In the owner's play (2026-11-15) only the
 * Crank-a-kai's three used it: a45a65ec (slot 11) cranks left today, 3 -> 30 at the day's reset then one down per
 * crank; 9076e662 (slot 12) the day's limit, 3 -> 30 at the reset; 2f508d80 (slot 13) a byte set at the reset (30 -> 5;
 * other runs 8, 15, 3).
 *
 * YW4_CHEAT_CRANK=1: unlimited cranks. A store that would lower the cranks left (a45a65ec) keeps the old value; the
 * day's reset (a raise) still happens, and the coins are spent as usual (an item count, elsewhere in the save). Read
 * at every store, so the overlay's Cheats page (settings.c) switches it while the game runs. Tested in the owner's
 * play, 2026-11-15: the count stayed at 30, coins went down.
 *
 * YW4_CHEATS=0 (the launcher's "Cheats" off): the Cheats page is hidden and no cheat acts, whatever settings.ini
 * says (owner, 2026-11-15, design canvas https://claude.ai/artifact/XcwzdE1gju4aNQJgxgE2e5 option B). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "yw4rt.h"

enum { CRANKS_LEFT = 0xa45a65ec };

static int trace = -1;
static unsigned long unchanged;
static int crank_on = -1;  /* -1: not read yet; the overlay's Cheats page sets 0 or 1 while the game runs */

static int env_on(const char *name) {
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

int cheats_allowed(void) {
    static int allowed = -1;
    if (allowed < 0) {
        const char *e = getenv("YW4_CHEATS");
        allowed = !(e && !strcmp(e, "0"));
    }
    return allowed;
}

int cheat_crank_get(void) {
    int v = __atomic_load_n(&crank_on, __ATOMIC_RELAXED);
    if (v < 0) {
        v = env_on("YW4_CHEAT_CRANK");
        __atomic_store_n(&crank_on, v, __ATOMIC_RELAXED);
    }
    return v;
}

/* The saved byte of ID `id`, found as the command finds it (main+0x296560): the globals at main+0x1548fd8, their ID
 * table at +0x138 (entries at +0x60: ID u32, slot u16 at +4, 8 bytes each; their count at +0x68), the save data at
 * +0x5768, the byte at slot + 0x288. NULL before the save data is there, or without the ID. */
static uint8_t *saved_byte(uint32_t id) {
    uint64_t globals, table, entries, base;
    memcpy(&globals, (const void *)(uintptr_t)(yw4_image_base + 0x1548fd8), 8);
    if (!globals) return NULL;
    memcpy(&table, (const void *)(uintptr_t)(globals + 0x138), 8);
    memcpy(&base, (const void *)(uintptr_t)(globals + 0x5768), 8);
    if (!table || !base) return NULL;
    memcpy(&entries, (const void *)(uintptr_t)(table + 0x60), 8);
    uint32_t count;
    memcpy(&count, (const void *)(uintptr_t)(table + 0x68), 4);
    if (!entries) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e;
        memcpy(&e, (const void *)(uintptr_t)(entries + i * 8ull), 4);
        if (e != id) continue;
        uint16_t slot;
        memcpy(&slot, (const void *)(uintptr_t)(entries + i * 8ull + 4), 2);
        return (uint8_t *)(uintptr_t)(base + slot + 0x288);
    }
    return NULL;
}

/* Turned on with no cranks left (0, or 255: -1 as a byte), the count becomes 1 so the next crank can happen; it then
 * stays there (owner, 2026-11-15). The Crank-a-kai menu shows it the next time it opens. */
void cheat_crank_toggle(void) {
    int v = !cheat_crank_get();
    __atomic_store_n(&crank_on, v, __ATOMIC_RELAXED);
    fprintf(stderr, "yw4: cheat: unlimited Crank-a-kai cranks %s\n", v ? "on" : "off");
    if (!v || !cheats_allowed()) return;
    uint8_t *left = saved_byte(CRANKS_LEFT);
    if (left && (*left == 0 || *left == 0xff)) {
        fprintf(stderr, "yw4: cheat: cranks left was %d, now 1\n", *left == 0xff ? -1 : 0);
        *left = 1;
    }
}

int yw4_hook_save_byte(CPUState *ctx) {
    if (trace < 0) trace = env_on("YW4_TRACE_SAVEBYTE");
    int crank = cheats_allowed() && cheat_crank_get();
    uint32_t id = (uint32_t)ctx->x[9];
    if (!trace && !(crank && id == CRANKS_LEFT)) return 0;
    uint8_t value = (uint8_t)ctx->x[20];
    uint16_t slot;
    memcpy(&slot, (const void *)(uintptr_t)(ctx->x[8] + 4), 2);
    uint64_t globals;  /* the same loads as the game's next instructions */
    memcpy(&globals, (const void *)(uintptr_t)(yw4_image_base + 0x1548fd8), 8);
    uint64_t base;
    memcpy(&base, (const void *)(uintptr_t)(globals + 0x5768), 8);
    uint8_t old = *(const uint8_t *)(uintptr_t)(base + slot + 0x288);
    if (crank && id == CRANKS_LEFT && value < old) {
        ctx->x[20] = old;  /* the game's store writes x20 */
        if (trace) fprintf(stderr, "yw4: cheat: cranks left kept at %u (would be %u)\n", (unsigned)old, (unsigned)value);
        return 0;
    }
    if (!trace) return 0;
    if (old != value)
        fprintf(stderr, "yw4: savebyte: id %08" PRIx32 " slot %u: %u -> %u\n", id, (unsigned)slot, (unsigned)old,
                (unsigned)value);
    else if (++unchanged % 1000 == 0)
        fprintf(stderr, "yw4: savebyte: %lu stores left a byte as it was\n", unchanged);
    return 0;
}
