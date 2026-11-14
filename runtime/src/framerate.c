/* The frame rate: 30 (the game's), 60 (the default), 120, 144, the display's refresh (vsync) or no limit, switchable while playing (YW4_FPS, panel FRAME RATE).
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
 * Vsync (YW4_FPS=vsync): no wait here; the swapchain presents with FIFO, so each frame waits for the display's
 * refresh and the rate locks to it (120 on a 120 Hz screen). Running free at about the refresh rate shows the
 * frames at uneven refreshes: the menus' scrolling background stuttered (owner, 2026-10-30, 120 Hz screen;
 * smooth at 30, an even divisor). Known gap: chosen at start it locks (frame intervals 8.3 ms, 95% within 8.6 on
 * the owner's 120 Hz laptop screen); switched to while playing, the swapchain is made again with FIFO, yet the
 * intervals stay loose (95% within 11 ms) on that hybrid-GPU laptop, so the panel says it applies after a restart.
 * A wait on the display's own refresh (DwmFlush) may fix it. While frame generation runs it paces the frames itself
 * (MAILBOX), so this applies only without it (2D screens drop it: gpu_fg.c, gpu_dlssg.cpp).
 *
 * YW4_FRAME_VSYNCS=<n>: the old way, the game's own pacing with that n (2 = the game's original), fixed for
 * the run; the panel row then does nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framerate.h"
#include "host.h"
#include "kernel.h"
#include "window.h"
#include "yw4load.h"
#include "yw4rt.h"

static const int rates[] = {30, 60, 120, 144, -1, 0};  /* -1: the display's refresh (vsync) */
enum { NRATES = sizeof rates / sizeof *rates };

static int index_v = -1;
static int game_paces, game_n;  /* YW4_FRAME_VSYNCS given: the game's limiter with that n, not ours */

int framerate_index(void) {
    int i = __atomic_load_n(&index_v, __ATOMIC_ACQUIRE);
    if (i >= 0) return i;
    const char *e = getenv("YW4_FPS");
    int want = e && !strcmp(e, "vsync") ? -1 : e && *e ? atoi(e) : 60;  /* 60 by default (owner, 2026-11-13); 30 is the game's own */
    i = 1;  /* an unknown value: 60 */
    for (int k = 0; k < NRATES; k++)
        if (rates[k] == want) i = k;
    __atomic_store_n(&index_v, i, __ATOMIC_RELEASE);
    return i;
}

void framerate_cycle(void) {
    int i = (framerate_index() + 1) % NRATES;
    __atomic_store_n(&index_v, i, __ATOMIC_RELEASE);
    if (rates[i] < 0) fprintf(stderr, "yw4: frame rate: the display's refresh (settings panel)\n");
    else if (rates[i]) fprintf(stderr, "yw4: frame rate: %d frames/s (settings panel)\n", rates[i]);
    else fprintf(stderr, "yw4: frame rate: no limit (settings panel)\n");
}

int framerate_hz(void) { return game_paces ? (game_n ? 60 / game_n : 0) : rates[framerate_index()]; }

/* How a cap of hz (30 or 60) is held: 0 by the emulated vsync thread's tick (ours), 1 by a deadline aligned to the
 * display's refresh (from Quandiy's fork). Measured 2026-11-12 (decision 0064): at 30 refresh has about an eighth
 * of the tick's long frames on 120 and 60 Hz displays, at 60 the two are the same and refresh costs CPU, so the
 * default is refresh at 30 and the tick at 60. YW4_PACE=refresh or tick: that way at both. Read once. */
int framerate_pacing(int hz) {
    static int force = -2;  /* -1 the default, 0 tick, 1 refresh */
    if (force == -2) {
        const char *e = getenv("YW4_PACE");
        force = e && !strcmp(e, "refresh") ? 1 : e && !strcmp(e, "tick") ? 0 : -1;
    }
    return force >= 0 ? force : hz == 30;
}

void framerate_wait(void) {
    static int said_tick;       /* the rate the log last named the tick wait for (0: not, or the other way since) */
    static uint64_t next_flip;  /* vsync count the next frame may show at (30 / 60) */
    static int64_t due;         /* host time the next frame may show at (120 / 144) */
    if (game_paces) return;     /* the game's limiter and interval did it */
    int hz = framerate_hz();
    yw4_vsync_start();
    /* Refresh (the default at 30, framerate_pacing; from Quandiy's fork, where it was the default at both): a
     * deadline on this thread, its period a whole number of the display's refreshes (4 x 8.33 ms on a 120 Hz screen
     * at 30, so every frame shows for the same refreshes and the phase doesn't drift). A late frame starts the
     * deadline again from now, where the tick wait would lose a whole tick: at 30 that turned frames a little over
     * 33 ms into 50 ms ones (decision 0064). */
    int pace_refresh = framerate_pacing(hz);
    if ((hz == 30 || hz == 60) && pace_refresh) {
        next_flip = 0;
        double refresh = window_refresh_hz();
        int64_t period = 1000000000ll / hz;
        if (refresh > 1.0) {
            double k = refresh / hz;
            long whole = (long)(k + 0.5);
            if (whole >= 1 && k / (double)whole > 0.99 && k / (double)whole < 1.01)
                period = (int64_t)((double)whole * 1e9 / refresh + 0.5);
        }
        static int64_t said_period;  /* the log names the way and its period, again when either changes */
        if (period != said_period) {
            said_period = period, said_tick = 0;
            fprintf(stderr, "yw4: pacing: refresh, period %.3f ms at %.2f Hz (%d frames/s)\n", (double)period / 1e6,
                    refresh, hz);
        }
        int64_t now = host_now_ns();
        if (!due || now - due > period / 2) due = now;  /* first frame, or late: start again from now, no burst */
        if (due - now > 1000000) host_sleep_ns(due - now - 1000000);  /* the timer is good to ~1 ms: spin the rest */
        while (host_now_ns() < due) {}
        due += period;
        return;
    }
    if (hz == 30 || hz == 60) {
        if (said_tick != hz) {
            said_tick = hz;
            fprintf(stderr, "yw4: pacing: tick (%d frames/s: %s 60 Hz vsync tick)\n", hz, hz == 30 ? "every 2nd" : "every");
        }
        uint64_t now = yw4_vsync_count();
        if (next_flip < now + 1) next_flip = now + 1;
        yw4_wait_vsync(next_flip);
        next_flip += hz == 30 ? 2 : 1;
        due = 0;
        return;
    }
    next_flip = 0;
    if (hz <= 0) return;  /* no limit, or vsync: the swapchain's FIFO waits */
    int64_t period = 1000000000ll / hz, now = host_now_ns();
    if (!due || now - due > 3 * period) due = now;  /* first frame, or fell far behind: don't burst to catch up */
    if (due - now > 1000000) host_sleep_ns(due - now - 1000000);  /* the timer is good to ~1 ms: spin the rest */
    while (host_now_ns() < due) {}
    due += period;
}

/* The game's time step of its latest frame, seconds (main+0x586527c, nnMain stores it each frame). */
float framerate_game_dt(void) {
    return *(const float *)(uintptr_t)(*yw4_modules[YW4_MAIN].base + 0x586527c);
}

int yw4_hook_nnmain(CPUState *ctx) {
    (void)ctx;
    uint8_t *n = (uint8_t *)(uintptr_t)(*yw4_modules[YW4_MAIN].base + 0x56b82e8);
    const char *e = getenv("YW4_FRAME_VSYNCS");
    game_paces = e && *e;
    int v = game_paces ? atoi(e) : 0;
    game_n = v;
    if (game_paces) fprintf(stderr, "yw4: frame rate: the game's own pacing, %d vsyncs per frame (game's %d)\n", v, *n);
    else if (framerate_hz() < 0) fprintf(stderr, "yw4: frame rate: the display's refresh, ours (the game's limiter off)\n");
    else if (framerate_hz()) fprintf(stderr, "yw4: frame rate: %d frames/s, ours (the game's limiter off)\n", framerate_hz());
    else fprintf(stderr, "yw4: frame rate: no limit, ours (the game's limiter off)\n");
    *n = (uint8_t)v;
    return 0;
}

/* Motions that must last a moment before a character may leave them, above 30 frames/s (issue #9).
 *
 * main+0x1c6170(action, dt) runs once a frame for each character's action state machine (main+0x1bcd70). It adds
 * dt to the time spent in the current motion (action+0xafc) and, once that reaches the motion's least time
 * (motion+0xb8, motion = action+0xae8, when bit 0 of motion+0xc1 is set), sets "may leave" (action+0xb1e). The
 * state machine (main+0x1c2aa0) only moves on from some states once that is set; while it waits it asks for the
 * same state again, which starts the motion again and sets its time back to 0.
 *
 * The jump's take-off (state 0x11) has a least time of about one 60 Hz frame. At 30 and 60 frames/s the first
 * frame's dt reaches it, the jump moves on to the air (state 0x10) with its full height (1.4). At 120 one dt
 * (1/120 s) falls short, the take-off restarts every frame and its time never grows: the player stays in the
 * take-off, whose height is 0.8, and a running jump is a small hop (measured 2026-11-14 with YW4_REPLAY_HZ: peak
 * +0.63 at 120 against +1.38 at 60).
 *
 * The game was made for 30 frames/s, where a motion's first frame always counts 1/30 s. So here a motion's time
 * counts at least 1/30 s for this check: after the game's own update, a motion that hasn't reached its least time
 * but whose least time is 1/30 s or less may be left, as it could after one frame on the Switch. Longer least
 * times are counted as before, and at 30 frames/s (dt >= 1/30) nothing changes. YW4_MOTION_TIME_FIX=0 turns it
 * off (to compare). */
static _Thread_local int motion_in_original;

int yw4_hook_motion_time(CPUState *ctx) {
    static int on = -1;
    if (on < 0) on = !(getenv("YW4_MOTION_TIME_FIX") && !strcmp(getenv("YW4_MOTION_TIME_FIX"), "0"));
    if (!on || motion_in_original) return 0;
    static GuestFn original;
    if (!original) original = yw4_lookup(*yw4_modules[YW4_MAIN].base + 0x1c6170);
    if (!original) return 0;
    const float least_frame = 1.0f / 30.0f;
    uint64_t action = ctx->x[0];
    float dt = ctx->v[0].f[0];
    motion_in_original = 1;
    original(ctx);
    motion_in_original = 0;
    if (!(dt < least_frame)) return 1;
    uint8_t *may_leave = (uint8_t *)(uintptr_t)(action + 0xb1e);
    uint64_t motion = *(const uint64_t *)(uintptr_t)(action + 0xae8);
    if (*may_leave || !motion || !(*(const uint8_t *)(uintptr_t)(motion + 0xc1) & 1)) return 1;
    float in_motion = *(const float *)(uintptr_t)(action + 0xafc);
    float least = *(const float *)(uintptr_t)(motion + 0xb8);
    if (in_motion < least_frame && least <= least_frame) *may_leave = 1;
    return 1;
}
