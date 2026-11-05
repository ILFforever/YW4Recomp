/* The overlay's pause (2026-11-05): the game's own pause, its pause screen hidden.
 *
 * How the game pauses (found with the scene-open log below, YW4_MENU_LOG): its main loop is a state machine
 * (f_846d20: the current state's Enter / Update / Exit, vtable slots 2 / 3 / 4). Plus in the field switches it
 * from the field's state to yw::MenuState (vtable main+0x14ccea0). MenuState's Enter (f_866540) opens a scene,
 * f_880750(out, "SceneMenu", 1, params), with the menu the field asked for in params: the menu "gameover_menu"
 * (CRC-32 0xf30e0a97) with the sub-menu "pause_menu" (0xd9d00e37), the "Paused" screen (Continue / Return to
 * Title; the Lua script pause_menu.lua builds it on the game-over screen's layers). MenuState's Update (f_8667d0)
 * waits while that scene exists; once Continue has closed it, it goes back to the field's state, and the state's
 * Exit (f_866cc0) runs.
 *
 * The pause screen's scene is what freezes the world: tried 2026-11-05, MenuState without the scene (the
 * scene-open hook handing back "no scene") stops the player and the camera, but people and Yo-kai on the map go
 * on moving (and Whisper comes out, as for a menu). So the screen opens as usual and the window hides it behind
 * the picture kept at the press (window.c). Here the hooks only say where the game is: paused (the pause scene
 * opened for the overlay's Plus) and resumed (MenuState's Exit), so the window knows when to press A on Continue
 * (again until the game has resumed) and when to show the live picture again, without guessing from the picture. */
#include <stdio.h>
#include <stdlib.h>

#include "kernel.h"
#include "pause.h"
#include "yw4rt.h"

#define ENTER_CALL_RET 0x8665d8u  /* MenuState's Enter, just after its f_880750 call (main-relative) */
#define MENU_GAMEOVER 0xf30e0a97u  /* CRC-32 of the names in data_path.cfg.bin */
#define MENU_PAUSE 0xd9d00e37u

static int state = PAUSE_OFF;

int pause_state(void) { return __atomic_load_n(&state, __ATOMIC_ACQUIRE); }
static int set_from(int from, int to) {
    return __atomic_compare_exchange_n(&state, &from, to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
void pause_request(void) { __atomic_store_n(&state, PAUSE_WANTED, __ATOMIC_RELEASE); }
int pause_resume(void) { return set_from(PAUSE_ON, PAUSE_RESUME); }
void pause_forget(void) { __atomic_store_n(&state, PAUSE_OFF, __ATOMIC_RELEASE); }

/* f_880750(out, scene name, flag, params): opens a scene. params: entries of 0x14 bytes, a type (2) and a hash;
 * for SceneMenu the first is the menu, the second the sub-menu. */
int yw4_hook_scene_open(CPUState *ctx) {
    static int log = -1;
    if (log < 0) log = getenv("YW4_MENU_LOG") != NULL;
    if (log) {  /* which scene, with what, from where: how the pause was found */
        char name[40] = {0};
        for (int i = 0; i < 39 && ctx->x[1]; i++)
            if (!(name[i] = (char)ld8(ctx->x[1] + i))) break;
        fprintf(stderr, "yw4: scene open '%s' params", name);
        for (int i = 0; i < 12 && ctx->x[3]; i++) fprintf(stderr, " %08x", ld32(ctx->x[3] + 4 * i));
        uint64_t st[10];
        int n = yw4_call_stack(ctx, st, 10);
        fprintf(stderr, " from main+%#llx:", (unsigned long long)(ctx->x[30] - yw4_image_base));
        for (int i = 0; i < n; i++) fprintf(stderr, " %#llx", (unsigned long long)(st[i] - yw4_image_base));
        fprintf(stderr, "\n");
    }
    if (ctx->x[30] == yw4_image_base + ENTER_CALL_RET && ctx->x[3] && ld32(ctx->x[3] + 4) == MENU_GAMEOVER &&
        ld32(ctx->x[3] + 0x18) == MENU_PAUSE && set_from(PAUSE_WANTED, PAUSE_ON))
        fprintf(stderr, "yw4: pause: the game is paused (its pause screen hidden)\n");
    return 0;
}

/* MenuState's Exit (f_866cc0): the game left its menu state, here its pause. */
int yw4_hook_menu_state_exit(CPUState *ctx) {
    (void)ctx;
    if (set_from(PAUSE_RESUME, PAUSE_OFF) || set_from(PAUSE_ON, PAUSE_OFF))
        fprintf(stderr, "yw4: pause: the game resumed\n");
    return 0;
}
