/* Battles: whether the game's battle scene is running, for the controls' battle set of keys (bindings.c).
 *
 * The battle scene is a yw::CSceneBattle (vtable main+0x14cbcf0, 18 functions). Of its own functions, four run
 * once every frame of a battle and none outside one (counted 2026-11-01 in the owner's battle: 600 calls per 600
 * frames, 0 before and after; two others ran once at its start). The hook on one of them notes the time; a
 * battle is on while that was less than a quarter second ago. The game's own code then runs as usual.
 * Blasters' missions are a yw::CSceneMultiPlay, a CSceneBattle with its own function in that slot (main+0x82ff80):
 * hooked the same way (2026-11-20). */
#include <stdint.h>

#include "battle.h"
#include "host.h"
#include "yw4rt.h"

static int64_t last_frame_ns;

int yw4_hook_battle_frame(CPUState *ctx) {
    (void)ctx;
    __atomic_store_n(&last_frame_ns, host_now_ns(), __ATOMIC_RELEASE);
    return 0;
}

int battle_active(void) {
    int64_t t = __atomic_load_n(&last_frame_ns, __ATOMIC_ACQUIRE);
    return t && host_now_ns() - t < 250000000;
}

/* Blasters' missions (CSceneMultiPlay's frame, main+0x82ff80): a battle too, and noted apart (bindings.c: a button
 * with no battle key takes the field set's there) */
static int64_t busters_frame_ns;

int yw4_hook_busters_frame(CPUState *ctx) {
    int64_t now = host_now_ns();
    __atomic_store_n(&busters_frame_ns, now, __ATOMIC_RELEASE);
    __atomic_store_n(&last_frame_ns, now, __ATOMIC_RELEASE);
    (void)ctx;
    return 0;
}

int battle_busters(void) {
    int64_t t = __atomic_load_n(&busters_frame_ns, __ATOMIC_ACQUIRE);
    return t && host_now_ns() - t < 250000000;
}
