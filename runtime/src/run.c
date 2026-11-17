/* Running modes (issue #18): run without holding B, by a toggle or always, with B left to the game.
 *
 * How the game runs (traced 2026-11-17 with YW4_WATCHW; work/re): the field's player controller, main+0x1fe950
 * (x0 = the controller, one object, called once a frame), sets "run this frame" (ctrl+0x5d) when B's hold time
 * (ctrl+0x54, growing by dt while B is down) reaches a threshold from the game's config, OR while a carry-on timer
 * (ctrl+0x58, counted down by dt every frame, refilled when B is let go) is above zero. A tap of B jumps through its
 * own path. "Run" then goes cmd+0xa9 -> action+0x234 -> action+0x614 == 2 -> the run speed (9.0 against 6.55).
 *
 * Here a hook runs before the controller: while running is wanted and the real B is up, it sets the controller up
 * as if B had been held past the threshold, the path that runs in any action state (the carry-on timer alone is
 * cleared outside the "moving" state, 7, and dropped out on a walk with turns; tested 2026-11-17). The controller
 * clears both values itself at the end of the frame. While the real B is down the hook stays out, so the game's
 * own hold and tap, and the jump, are untouched. To walk at once in ALWAYS, the carry-on timer is cleared while
 * the Run key is held.
 *
 * The modes (the Controls page's RUNNING row; YW4_RUN_MODE=0/1/2/3; owner, 2026-11-18), with one Run input (Left
 * Shift on the keyboard; the left stick press on a pad, shared there with Close hints, no button being free):
 *   DEFAULT  the game's: B holds to run, taps to jump. The Run input does nothing.
 *   ALWAYS   always run; hold Run to walk.
 *   TOGGLE   Run switches running on and off. Off again when a battle starts.
 *   HOLD     hold Run to run. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "battle.h"
#include "bindings.h"
#include "run.h"
#include "yw4load.h"
#include "yw4rt.h"

static int mode = -1;

/* B is down, as the game's input object has it for this frame (pointer at main+0x14d1008; +0x8 the held buttons in
 * the game's bits, B = 0x4000, mapped from the Switch's by the table at main+0x12bdfb0). */
static int b_down(uint64_t base) {
    uint64_t input = *(const uint64_t *)(uintptr_t)(base + 0x14d1008);
    return input && (*(const uint32_t *)(uintptr_t)(input + 0x8) & 0x4000u) != 0;
}

int run_mode(void) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("YW4_RUN_MODE");
        m = e && *e ? atoi(e) : RUN_DEFAULT;
        if (m < 0 || m >= RUN_MODES) m = RUN_DEFAULT;
        __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    }
    return m;
}
void run_mode_set(int m) {
    if (m < 0 || m >= RUN_MODES) m = RUN_DEFAULT;
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
}
const char *run_mode_name(int m) {
    static const char *const names[RUN_MODES] = {"DEFAULT", "ALWAYS", "TOGGLE", "HOLD"};
    return m >= 0 && m < RUN_MODES ? names[m] : names[0];
}
const char *run_mode_save(int m) {
    static const char *const values[RUN_MODES] = {"0", "1", "2", "3"};
    return m >= 0 && m < RUN_MODES ? values[m] : values[0];
}

/* YW4_RUN_KEY_TEST=<from>-<to>[,<from>-<to>...]: tests, Run held between those presented frames (a replay
 * feeds the Switch's buttons, not keys, so it can't hold the key itself). */
extern uint64_t yw4_frames_presented;  /* dispatch.c */
static int key_test(void) {
    enum { MAX = 8 };
    static int read, n;
    static unsigned long long from[MAX], to[MAX];
    if (!read) {
        read = 1;
        const char *e = getenv("YW4_RUN_KEY_TEST");
        for (int used; e && *e && n < MAX && sscanf(e, "%llu-%llu%n", &from[n], &to[n], &used) == 2; n++) {
            e += used;
            if (*e == ',') e++;
        }
    }
    uint64_t f = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++)
        if (f >= from[i] && f <= to[i]) return 1;
    return 0;
}

/* YW4_RUN_LOG=1: every 30th call, what the hook saw and did, and the controller's values after the game's code. */
static int log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("YW4_RUN_LOG") && *getenv("YW4_RUN_LOG") != '0';
    return on;
}
static int hook_inner(CPUState *ctx);
static _Thread_local int in_original;
int yw4_hook_player_run(CPUState *ctx) {
    if (in_original) return 0;
    if (!log_on()) return hook_inner(ctx);
    static long calls;
    uint64_t ctrl = ctx->x[0], base = *yw4_modules[YW4_MAIN].base;
    float hold0 = *(const float *)(uintptr_t)(ctrl + 0x54), carry0 = *(const float *)(uintptr_t)(ctrl + 0x58);
    hook_inner(ctx);
    float hold1 = *(const float *)(uintptr_t)(ctrl + 0x54);
    uint8_t held5c = *(const uint8_t *)(uintptr_t)(ctrl + 0x5c);
    static GuestFn original;
    if (!original) original = yw4_lookup(base + 0x1fe950);
    if (!original) return 0;
    in_original = 1;
    original(ctx);
    in_original = 0;
    if (calls++ % 30 == 0)
        fprintf(stderr, "yw4: run: frame %llu mode %d key %d B %d threshold %g | before: hold %g carry %g | set: hold %g "
                "5c %u | after the game: ran %u hold %g carry %g\n",
                (unsigned long long)__atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED), run_mode(),
                bindings_run_held() || key_test(), b_down(base),
                (double)*(const float *)(uintptr_t)(base + 0x1531390), (double)hold0, (double)carry0, (double)hold1,
                held5c, *(const uint8_t *)(uintptr_t)(ctrl + 0x5d), (double)*(const float *)(uintptr_t)(ctrl + 0x54),
                (double)*(const float *)(uintptr_t)(ctrl + 0x58));
    return 1;
}

/* Runs first, then the game's controller (returns 0). */
static int hook_inner(CPUState *ctx) {
    static int toggled, was_held;
    int m = run_mode(), held = bindings_run_held() || key_test();
    if (battle_active()) {  /* not the field's controller's business; a toggle ends with the battle */
        toggled = 0, was_held = held;
        return 0;
    }
    if (m == RUN_TOGGLE && held && !was_held) toggled = !toggled;
    if (m != RUN_TOGGLE) toggled = 0;
    was_held = held;
    int run = m == RUN_ALWAYS ? !held : m == RUN_TOGGLE ? toggled : m == RUN_HOLD ? held : 0;
    uint64_t ctrl = ctx->x[0], base = *yw4_modules[YW4_MAIN].base;
    /* the frame B goes up the game judges the press by its hold time (a tap jumps): that frame stays the game's too,
     * or the fake hold below would turn every tap into a long hold (no jump; tested 2026-11-17) */
    static int b_was_down, was_run;
    int b_now = b_down(base), b_released = b_was_down && !b_now;
    b_was_down = b_now;
    /* running switched off (TOGGLE off, Run let go in HOLD): the carry-on timer the game set at the end of
     * the last faked frame (about 0.55 s, as after a long B hold) is cleared, so the walk starts at once (owner,
     * 2026-11-17: "faster"). A real B hold still carries on as the game does. */
    int stopped = was_run && !run;
    was_run = run;
    if (run && !b_now && !b_released) {
        /* as if B had been held long enough: B down last frame (ctrl+0x5c, so the hold time isn't cleared) and the
         * hold time (ctrl+0x54) at the run threshold, which runs in any action state. The controller itself puts
         * both back to 0 at the end of the frame, B being up. While the real B is down this stays out: the game
         * sees its own press from a hold time of 0, so a tap still jumps. */
        /* the game's check is "greater than" (b.gt), so a little over: the threshold alone never ran (2026-11-17) */
        float over = *(const float *)(uintptr_t)(base + 0x1531390) + 0.05f;
        float *hold = (float *)(uintptr_t)(ctrl + 0x54);
        *(uint8_t *)(uintptr_t)(ctrl + 0x5c) = 1;
        if (!(*hold >= over)) *hold = over;
    } else if (!run && !b_now && (stopped || m == RUN_ALWAYS)) {
        /* walking now: the carry-on timer the game left from the "long hold" above (ctrl+0x58) cleared */
        *(float *)(uintptr_t)(ctrl + 0x58) = 0.0f;
    }
    return 0;
}
