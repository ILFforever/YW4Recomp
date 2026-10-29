/* The frame rate: 30 (the game's), 60, 120, 144 or no limit, switchable while playing (YW4_FPS, panel FRAME RATE).
 *
 * nnMain paces itself. A static constructor sets the byte at main+0x56b82e8 to 2 (vsyncs per frame, n) and
 * main+0x56b82e9 to 1 (limiter on). nnMain reads n once at start:
 *   - present interval n (nvnWindowSetPresentInterval), so the display shows each frame for n vsyncs;
 *   - its own limiter (main+0x950c3c): it sleeps until the frame took (n - 1) / 60 + 0.015 s (31.7 ms for 2),
 *     then spins on the tick counter;
 *   - 60 / n as the rate for a once-a-second GPU load check (main+0x9503c0): GPU time from two timestamps, over
 *     96% of a frame lowers the dynamic resolution (u16 main+0x56b82ce, the 3D view's lines, 900 at most) by 18
 *     lines, under 88% raises it. Our timestamps are not the GPU's (nvn_cmdbuf.c writes none with the GPU on),
 *     so it stays at 900 whatever n says (watched 2026-10-29: 12,413 writes, all 900).
 * The game's time step is not n: every frame it measures the real time since the last one (system tick,
 * clamped to 0.2 s, main+0x94ee78) and stores it at main+0x586527c, and the tasks scale their work by it.
 * So faster frames keep the game's speed: walk replays at 60 and ~140 frames/s end in the same place, and
 * battles play right at high frame rates (owner 2026-10-29).
 *
 * Because n is read only once, the hook on nnMain's start sets it to 0 (no limiter, interval 0) and the rate
 * is held here instead, at the present: 30 and 60 wait for the 60 Hz vsync (every 2nd / every one, as the
 * game's own interval did), 120 and 144 sleep to a timer, unlimited doesn't wait. Above 60 the window
 * presents with MAILBOX or IMMEDIATE (window.c), so the monitor's refresh doesn't cap it.
 *
 * YW4_FRAME_VSYNCS=<n>: the old way, the game's own pacing with that n (2 = the game's original), fixed for
 * the run; the panel row then does nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framerate.h"
#include "host.h"
#include "kernel.h"
#include "yw4load.h"
#include "yw4rt.h"

static const int rates[] = {30, 60, 120, 144, 0};
enum { NRATES = sizeof rates / sizeof *rates };

static int index_v = -1;
static int game_paces, game_n;  /* YW4_FRAME_VSYNCS given: the game's limiter with that n, not ours */

int framerate_index(void) {
    int i = __atomic_load_n(&index_v, __ATOMIC_ACQUIRE);
    if (i >= 0) return i;
    const char *e = getenv("YW4_FPS");
    int want = e && *e ? atoi(e) : 30;
    i = 0;
    for (int k = 0; k < NRATES; k++)
        if (rates[k] == want) i = k;
    __atomic_store_n(&index_v, i, __ATOMIC_RELEASE);
    return i;
}

void framerate_cycle(void) {
    int i = (framerate_index() + 1) % NRATES;
    __atomic_store_n(&index_v, i, __ATOMIC_RELEASE);
    if (rates[i]) fprintf(stderr, "yw4: frame rate: %d frames/s (settings panel)\n", rates[i]);
    else fprintf(stderr, "yw4: frame rate: no limit (settings panel)\n");
}

int framerate_hz(void) { return game_paces ? (game_n ? 60 / game_n : 0) : rates[framerate_index()]; }

void framerate_wait(void) {
    static uint64_t next_flip;  /* vsync count the next frame may show at (30 / 60) */
    static int64_t due;         /* host time the next frame may show at (120 / 144) */
    if (game_paces) return;     /* the game's limiter and interval did it */
    int hz = framerate_hz();
    yw4_vsync_start();
    if (hz == 30 || hz == 60) {
        uint64_t now = yw4_vsync_count();
        if (next_flip < now + 1) next_flip = now + 1;
        yw4_wait_vsync(next_flip);
        next_flip += hz == 30 ? 2 : 1;
        due = 0;
        return;
    }
    next_flip = 0;
    if (!hz) return;
    int64_t period = 1000000000ll / hz, now = host_now_ns();
    if (!due || now - due > 3 * period) due = now;  /* first frame, or fell far behind: don't burst to catch up */
    if (due - now > 1000000) host_sleep_ns(due - now - 1000000);  /* the timer is good to ~1 ms: spin the rest */
    while (host_now_ns() < due) {}
    due += period;
}

int yw4_hook_nnmain(CPUState *ctx) {
    (void)ctx;
    uint8_t *n = (uint8_t *)(uintptr_t)(*yw4_modules[YW4_MAIN].base + 0x56b82e8);
    const char *e = getenv("YW4_FRAME_VSYNCS");
    game_paces = e && *e;
    int v = game_paces ? atoi(e) : 0;
    game_n = v;
    if (game_paces) fprintf(stderr, "yw4: frame rate: the game's own pacing, %d vsyncs per frame (game's %d)\n", v, *n);
    else if (framerate_hz()) fprintf(stderr, "yw4: frame rate: %d frames/s, ours (the game's limiter off)\n", framerate_hz());
    else fprintf(stderr, "yw4: frame rate: no limit, ours (the game's limiter off)\n");
    *n = (uint8_t)v;
    return 0;
}
