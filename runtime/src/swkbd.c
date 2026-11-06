/* nn::swkbd, the software keyboard, as an override of the sdk's ShowKeyboard (decision 0022).
 *
 * On the Switch ShowKeyboard launches the keyboard library applet and waits for its answer.
 * We have no applets, so the call answers at once with a fixed name: YW4_PLAYER_NAME, else "Nate" (or "Katie" when
 * the question is the girl's name). The game's name entry calls it with String = { buffer; size_t size } (size =
 * GetRequiredStringBufferSize) and its KeyboardConfig asks for UTF-8 (offset 0x3bd) and at most 8 characters
 * (0x3ac); offsets from the sdk's keyboardConfig2kbdConfig, values measured 2026-11-07. Until then the answer was
 * UTF-16, which the game read as one letter: new saves named the boy "N". Typing a name: next (textin.h).
 */
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "yw4load.h"

#define DEFAULT_NAME "Nate"
#define DEFAULT_GIRL "Katie"  /* the girl's name (the second question of a new game) */

/* ShowKeyboard(String *out, const ShowKeyboardArg &) -> Result */
void yw4_swkbd_ShowKeyboard(CPUState *c) {
    uint16_t *buf = (uint16_t *)(uintptr_t)((uint64_t *)(uintptr_t)c->x[0])[0];
    uint64_t size = ((uint64_t *)(uintptr_t)c->x[0])[1];
    {  /* what the game asks (KeyboardConfig, offsets from the sdk's keyboardConfig2kbdConfig, 2026-11-07) */
        const uint8_t *a = (const uint8_t *)(uintptr_t)c->x[1];
        char head[200] = "", guide[200] = "";
        for (int i = 0, n = 0; i < 64 && n < 190; i++) {
            uint16_t ch = *(const uint16_t *)(a + 0x24 + 2 * i);
            if (!ch) break;
            n += snprintf(head + n, sizeof head - n, ch < 128 ? "%c" : "\\u%04x", ch);
        }
        for (int i = 0, n = 0; i < 256 && n < 190; i++) {
            uint16_t ch = *(const uint16_t *)(a + 0x1a8 + 2 * i);
            if (!ch) break;
            n += snprintf(guide + n, sizeof guide - n, ch < 128 ? "%c" : "\\u%04x", ch);
        }
        fprintf(stderr, "yw4: swkbd: mode %u header \"%s\" guide \"%s\" max %d min %d password %u form %u newline %u "
                "utf8 %u initial at %d len %d textcheck %u callback 0x%llx\n",
                *(const uint32_t *)a, head, guide, *(const int32_t *)(a + 0x3ac), *(const int32_t *)(a + 0x3b0),
                *(const uint32_t *)(a + 0x3b4), *(const uint32_t *)(a + 0x3b8), a[0x3bc], a[0x3bd],
                *(const int32_t *)(a + 0x3c0), *(const int32_t *)(a + 0x3c4), a[0x3d0],
                (unsigned long long)*(const uint64_t *)(a + 0x3d8));
    }
    const uint8_t *a = (const uint8_t *)(uintptr_t)c->x[1];
    int utf8 = a[0x3bd], max = *(const int32_t *)(a + 0x3ac);
    /* the game's own starting text: UTF-16 in the work buffer (ShowKeyboardArg + 0x4d0), at the offset 0x3c0 holds */
    char initial[96] = "";
    {
        const uint8_t *work = (const uint8_t *)(uintptr_t)*(const uint64_t *)(a + 0x4d0);
        int off = *(const int32_t *)(a + 0x3c0), len = *(const int32_t *)(a + 0x3c4), n = 0;
        if (work && off >= 0 && len > 0)
            for (int i = 0; i < len && i < 32; i++) {
                uint16_t ch = *(const uint16_t *)(work + off + 2 * i);
                if (!ch) break;
                if (ch < 0x80) initial[n++] = (char)ch;
                else if (ch < 0x800) initial[n++] = (char)(0xC0 | ch >> 6), initial[n++] = (char)(0x80 | (ch & 63));
                else initial[n++] = (char)(0xE0 | ch >> 12), initial[n++] = (char)(0x80 | (ch >> 6 & 63)),
                     initial[n++] = (char)(0x80 | (ch & 63));
                if (n > (int)sizeof initial - 4) break;
            }
        initial[n] = 0;
    }
    const char *name = getenv("YW4_PLAYER_NAME");
    /* the game's starting text is a width placeholder at name entry ("<NAME_KETA_8888>", measured 2026-11-07), not a
     * name: one in <...> is left out */
    if (initial[0] == '<') initial[0] = 0;
    /* a new game asks twice: the boy's name, then "the girl he loves" (English header; 女 in Japanese) */
    int girl = 0;
    for (int i = 0; i < 64; i++) {
        uint16_t ch = *(const uint16_t *)(a + 0x24 + 2 * i);
        if (!ch) break;
        if (ch == 0x5973 || (ch == 'g' && *(const uint16_t *)(a + 0x26 + 2 * i) == 'i' &&
                             *(const uint16_t *)(a + 0x28 + 2 * i) == 'r' && *(const uint16_t *)(a + 0x2a + 2 * i) == 'l'))
            girl = 1;
    }
    if (!name || !*name) name = initial[0] ? initial : girl ? DEFAULT_GIRL : DEFAULT_NAME;
    /* At most `max` characters (0: no limit), and what the game asked for: UTF-8 when the config says so (the game
     * does, 2026-11-07: before, a UTF-16 answer made the name its first letter, "N"), else UTF-16. */
    uint64_t chars = 0, n = 0;
    if (utf8) {
        uint8_t *out = (uint8_t *)buf;
        for (const unsigned char *p = (const unsigned char *)name; buf && *p && n + 4 < size;) {
            int len = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
            if (max > 0 && (int)chars >= max) break;
            for (int k = 0; k < len && p[k]; k++) out[n++] = p[k];
            p += len, chars++;
        }
        if (buf && size) out[n] = 0;
    } else {
        uint64_t room = size / 2;
        for (const unsigned char *p = (const unsigned char *)name; buf && *p && n + 1 < room;) {
            uint32_t cp = *p;
            int len = 1;
            if (cp >= 0xE0) cp = (cp & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63), len = 3;
            else if (cp >= 0xC0) cp = (cp & 31) << 6 | (p[1] & 63), len = 2;
            if (max > 0 && (int)chars >= max) break;
            buf[n++] = (uint16_t)cp, p += len, chars++;
        }
        if (buf && room) buf[n] = 0;
    }
    fprintf(stderr, "yw4: swkbd: answered with \"%s\" (%s, %llu characters, buffer %llu bytes; the game's text \"%s\")\n",
            name, utf8 ? "UTF-8" : "UTF-16", (unsigned long long)chars, (unsigned long long)size, initial);
    c->x[0] = 0;
}
