/* nn::swkbd, the software keyboard, as an override of the sdk's ShowKeyboard (decision 0022).
 *
 * On the Switch ShowKeyboard launches the keyboard library applet and waits for its answer.
 * We have no applets, so the call answers at once with a fixed name. The game's name entry
 * (a new game asks for the player's name) calls it with String = { char16_t *buffer;
 * size_t size } (size = GetRequiredStringBufferSize, measured in the game's caller) and
 * reads UTF-16 text back from the buffer. Typing a real name (SDL text input) comes later.
 */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "yw4load.h"

#define DEFAULT_NAME "Nate"

/* ShowKeyboard(String *out, const ShowKeyboardArg &) -> Result */
void yw4_swkbd_ShowKeyboard(CPUState *c) {
    uint16_t *buf = (uint16_t *)(uintptr_t)((uint64_t *)(uintptr_t)c->x[0])[0];
    uint64_t size = ((uint64_t *)(uintptr_t)c->x[0])[1];
    const char *name = getenv("YW4_PLAYER_NAME");
    if (!name || !*name) name = DEFAULT_NAME;
    uint64_t room = size / 2;
    uint64_t n = 0;
    if (buf && room) {
        for (; name[n] && n + 1 < room; n++) buf[n] = (uint8_t)name[n];  /* ASCII only for now */
        buf[n] = 0;
    }
    fprintf(stderr, "yw4: swkbd: answered with \"%s\" (buffer %llu bytes)\n", name, (unsigned long long)size);
    c->x[0] = 0;
}
