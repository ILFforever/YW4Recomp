/* Merging several mods' copies of one game data file (decision 0068). */
#ifndef YW4_MODMERGE_H
#define YW4_MODMERGE_H

#include <stddef.h>
#include <stdint.h>

/* One file out of a CRI .cpk archive (the game's archives: names in a TOC, CRILAYLA compression), by its full path
 * as the archive lists it ("data/common/gamedata/item/item_config_0.13.66.cfg.bin"). malloc'd, NULL if not there. */
uint8_t *cpk_read_file(const char *cpk_path, const char *inner_path, size_t *n);

/* Level-5 .cfg.bin files: base = the game's copy, mods[0..nmods) the mods' copies in load order. Every entry a mod
 * changed, added or removed against the base is kept, from every mod. Returns the merged file (malloc'd), or NULL
 * when it can't be merged: two mods change the same entries (*clash_a, *clash_b: their indices), or a copy isn't a
 * cfg.bin this code understands (both -1). */
uint8_t *cfgbin_merge(const uint8_t *base, size_t nbase, const uint8_t *const *mods, const size_t *nmod, int nmods,
                      size_t *out_n, int *clash_a, int *clash_b);

/* The file written back from its own entries, for the self-check: equal to the input when the writer is right. */
uint8_t *cfgbin_rewrite(const uint8_t *b, size_t n, size_t *out_n);

#endif
