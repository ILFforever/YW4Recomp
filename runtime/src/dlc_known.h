/* The add-ons (DLC) the game unlocks when it finds them on boot (decision 0049): defined in
 * <work_dir>/build/dlc_known.c, which recomp.build writes from target.toml's [dlc] table. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t index;      /* add-on index: its title ID - the application's - 0x1000 */
    const char *sha256;  /* of its content NCA, lower-case hex */
} Yw4DlcKnown;

extern const Yw4DlcKnown yw4_dlc_known[];
extern const size_t yw4_ndlc_known;
