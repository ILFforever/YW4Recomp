/* The game's frame rate (experiment, YW4_FRAME_VSYNCS).
 *
 * nnMain paces itself. A static constructor sets the byte at main+0x56b82e8 to 2 (vsyncs per frame, n) and
 * main+0x56b82e9 to 1 (limiter on). nnMain reads n once at start:
 *   - present interval n (nvnWindowSetPresentInterval), so the display shows each frame for n vsyncs;
 *   - its own limiter (main+0x950c3c): it sleeps until the frame took (n - 1) / 60 + 0.015 s (31.7 ms for 2),
 *     then spins on the tick counter;
 *   - 60 / n as the rate for a once-a-second load average (main+0x9503c0) that nudges a quality value.
 * The game's time step is not n: every frame it measures the real time since the last one (system tick,
 * clamped to 0.2 s, main+0x94ee78) and stores it at main+0x586527c, and the tasks scale their work by it.
 * So moving faster than 30 frames/s keeps the game's speed (walk replays at 60 and ~140 frames/s end in the
 * same place); parts that count frames instead would run fast, none seen yet.
 *
 * YW4_FRAME_VSYNCS=1: 60 frames/s (15 ms limiter, interval 1). 0: no limit (limiter target below zero,
 * interval 0: nvn_objects.c skips the vsync wait, window.c presents with MAILBOX or IMMEDIATE). */
#include <stdio.h>
#include <stdlib.h>

#include "yw4load.h"
#include "yw4rt.h"

int yw4_hook_nnmain(CPUState *ctx) {
    (void)ctx;
    const char *e = getenv("YW4_FRAME_VSYNCS");
    if (e && *e) {
        uint8_t *n = (uint8_t *)(uintptr_t)(*yw4_modules[YW4_MAIN].base + 0x56b82e8);
        fprintf(stderr, "yw4: frame rate: %d vsyncs per frame (game's %d)\n", atoi(e), *n);
        *n = (uint8_t)atoi(e);
    }
    return 0;
}
