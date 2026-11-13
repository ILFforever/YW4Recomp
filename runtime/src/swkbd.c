/* nn::swkbd, the software keyboard, as an override of the sdk's ShowKeyboard (decision 0022).
 *
 * On the Switch ShowKeyboard launches the keyboard library applet and waits for its answer.
 * We have no applets: the player types the name in the overlay's popup (gamemenu.h), or in replays the call
 * answers at once: YW4_PLAYER_NAME, else the character's default name (below). The game's name entry calls it with String = { buffer; size_t size } (size =
 * GetRequiredStringBufferSize) and its KeyboardConfig asks for UTF-8 (offset 0x3bd) and at most 8 characters
 * (0x3ac); offsets from the sdk's keyboardConfig2kbdConfig, values measured 2026-11-07. Until then the answer was
 * UTF-16, which the game read as one letter: new saves named the boy "N". Typing a name: next (textin.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamemenu.h"
#include "host.h"
#include "romfs.h"
#include "textin.h"
#include "window.h"
#include "yw4load.h"
#include "yw4rt.h"

#define DEFAULT_NAME "Nate"
#define DEFAULT_GIRL "Katie"  /* the girl's name (the second question of a new game) */

/* Typed text, made full-width (hook on f_9997e0, main+0x9997e0; owner, 2026-11-14). After an OK answer the game's
 * keyboard handler (f_8aa550 / f_8a9xxx, from f_959be0) runs the typed UTF-8 text through f_9997e0(dst, src), which
 * maps each character through a 156-entry table at main+0x12be9d0 (u16 from, u16 to): every ASCII character to its
 * full-width form (A -> Ａ, space -> U+3000) and half-width katakana to normal katakana. Names in English then
 * became Ｎａｔｅ, which the game draws a wide cell per letter ("N a t e"; decision 0066).
 * This does the same, with the game's own table, except that ASCII stays as typed: katakana are still made
 * full-size, so Japanese names are as before. Output as the game's: at most 0x100 bytes with the closing 0. */
int yw4_hook_kbd_fullwidth(CPUState *ctx) {
    /* only the answer the game keeps (f_8aa550 calls it from +0xa4): the game's name check converts too and then
     * allows full-width letters only, so it keeps the game's conversion ("This name cannot be used" otherwise) */
    if (ctx->x[30] != *yw4_modules[YW4_MAIN].base + 0x8aa5f8) return 0;
    uint8_t *dst = (uint8_t *)(uintptr_t)ctx->x[0], *end = dst + 0x100;
    const uint8_t *src = (const uint8_t *)(uintptr_t)ctx->x[1];
    const uint16_t *table = (const uint16_t *)(uintptr_t)(*yw4_modules[YW4_MAIN].base + 0x12be9d0);
    enum { ENTRIES = 0x9c };
    while (*src) {
        /* one UTF-8 character */
        uint32_t c = *src;
        int len = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (len > 1) c &= 0x3f >> (len - 1);
        int k = 1;
        for (; k < len && (src[k] & 0xc0) == 0x80; k++) c = c << 6 | (src[k] & 0x3f);
        src += k;
        /* the game's table (sorted by its first column), not for ASCII */
        if (c >= 0x80)
            for (int lo = 0, hi = ENTRIES - 2; lo <= hi;) {  /* the last entry is the 0, 0 end */
                int mid = (lo + hi) / 2;
                uint16_t from = table[mid * 2];
                if (from == c) { c = table[mid * 2 + 1]; break; }
                if (from < c) lo = mid + 1;
                else hi = mid - 1;
            }
        uint8_t out[4];
        int n = c < 0x80 ? (out[0] = (uint8_t)c, 1)
              : c < 0x800 ? (out[0] = (uint8_t)(0xc0 | c >> 6), out[1] = (uint8_t)(0x80 | (c & 0x3f)), 2)
              : (out[0] = (uint8_t)(0xe0 | (c >> 12 & 0x0f)), out[1] = (uint8_t)(0x80 | (c >> 6 & 0x3f)),
                 out[2] = (uint8_t)(0x80 | (c & 0x3f)), 3);
        if (dst + n >= end) break;  /* room for the closing 0 */
        memcpy(dst, out, (size_t)n);
        dst += n;
    }
    *dst = 0;
    ctx->x[0] = (uint64_t)(uintptr_t)dst;
    return 1;  /* done: the game's own conversion is skipped */
}

/* Invokes the game's internal text check callback (SetTextCheckCallback), if registered.
 * Returns 0 if valid (accepted) or if text checking is inactive; non-zero if rejected by the game. */
static int check_name(CPUState *c, const uint8_t *a, const char *name, int utf8, int max) {
    if (!a[0x3d0]) return 0;
    uint64_t cb = *(const uint64_t *)(a + 0x3d8);
    if (!cb) return 0;

    uint16_t *buf = (uint16_t *)(uintptr_t)((uint64_t *)(uintptr_t)c->x[0])[0];
    uint64_t orig_size = ((uint64_t *)(uintptr_t)c->x[0])[1];
    if (!buf || !orig_size) return 0;

    uint64_t chars = 0, n = 0;
    if (utf8) {
        uint8_t *out = (uint8_t *)buf;
        for (const unsigned char *p = (const unsigned char *)name; *p && n + 4 < orig_size;) {
            int len = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
            if (max > 0 && (int)chars >= max) break;
            for (int k = 0; k < len && p[k]; k++) out[n++] = p[k];
            p += len, chars++;
        }
        out[n] = 0;
    } else {
        uint64_t room = orig_size / 2;
        for (const unsigned char *p = (const unsigned char *)name; *p && n + 1 < room;) {
            uint32_t cp = *p;
            int len = 1;
            if (cp >= 0xE0) cp = (cp & 15) << 12 | (p[1] & 63) << 6 | (p[2] & 63), len = 3;
            else if (cp >= 0xC0) cp = (cp & 31) << 6 | (p[1] & 63), len = 2;
            if (max > 0 && (int)chars >= max) break;
            buf[n++] = (uint16_t)cp, p += len, chars++;
        }
        buf[n] = 0;
    }

    ((uint64_t *)(uintptr_t)c->x[0])[1] = utf8 ? n : n * 2;

    uint64_t out_len = 0;
    CPUState call_ctx = *c;
    call_ctx.x[0] = *(const uint64_t *)(a + 0x4e0);
    call_ctx.x[1] = (uint64_t)(uintptr_t)&out_len;
    call_ctx.x[2] = c->x[0];
    call_ctx.x[30] = 0;

    GuestFn fn = yw4_resolve(cb);
    fn(&call_ctx);

    ((uint64_t *)(uintptr_t)c->x[0])[1] = orig_size;

    uint32_t result = (uint32_t)call_ctx.x[0];
    fprintf(stderr, "yw4: swkbd: textcheck \"%s\" (callback 0x%llx) -> result %u\n",
            name, (unsigned long long)cb, result);
    return result != 0;
}

/* A main character's default name for the token the game starts name entry with ("<NAME_KETA_8888>"...), in the
 * game's language. The names are in data/common/text/ja/chara_text.cfg.bin, as six names, Neptuna (マグロドン),
 * then the six tokens in the same order (measured in the game's file and the English patch's, 2026-11-13). A
 * translation mod replaces that file with a loose one, read here; the game's own copy is inside text_all.cpk
 * (compressed), so without a mod the game's Japanese names below are used. NULL: not a main character's token. */
static const char *main_name_for(const char *token) {
    static const char *const tokens[] = {"<NAME_TOUMA", "<NAME_NATSUME", "<NAME_AKINORI",
                                         "<NAME_SHIN",  "<NAME_KETA",    "<NAME_FUMI"};
    static const char *const japanese[] = {"トウマ", "ナツメ", "アキノリ", "シン", "ケータ", "フミ"};
    static char from_mod[6][64];
    static int loaded;  /* 0 not yet, 1 from the mod's file, -1 the Japanese names */
    int k = -1;
    for (int i = 0; i < 6; i++)
        if (!strncmp(token, tokens[i], strlen(tokens[i]))) k = i;
    if (k < 0) return NULL;
    if (!loaded) {
        loaded = -1;
        const char *path = romfs_host_path("data/common/text/ja/chara_text.cfg.bin");
        FILE *f = path ? fopen(path, "rb") : NULL;
        char *d = NULL;
        long n = 0;
        if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)n + 1)) &&
            fread(d, 1, (size_t)n, f) == (size_t)n) {
            d[n] = 0;
            /* the first token, then the strings before it: Neptuna, then the six names, last first */
            const char *t = NULL;
            for (long i = 0; i + 16 <= n && !t; i++)
                if (!memcmp(d + i, "<NAME_TOUMA_888>", 16) && i > 0 && !d[i - 1]) t = d + i;
            const char *starts[7];
            int got = 0;
            for (const char *p = t ? t - 1 : NULL; p && got < 7 && p > d;) {
                const char *q = p - 1;  /* p is the zero that ends the string before */
                while (q > d && q[-1]) q--;
                const char *s = q;  /* the first string follows the table's 0xFF filler (never in UTF-8) */
                while (s < p && (unsigned char)*s == 0xFF) s++;
                if (p - s <= 0 || p - s >= 64) break;
                starts[got++] = s, p = q - 1;
            }
            if (got == 7) {
                for (int i = 0; i < 6; i++) snprintf(from_mod[i], sizeof from_mod[i], "%s", starts[6 - i]);
                loaded = 1;
            }
            fprintf(stderr, "yw4: swkbd: main characters' names: %s (%s)\n",
                    loaded == 1 ? "the mod's" : "not found in the mod's file, the game's Japanese ones", path);
        }
        if (f) fclose(f);
        free(d);
    }
    return loaded == 1 ? from_mod[k] : japanese[k];
}

/* nn::ngc::ProfanityFilter: accepts names without requiring the Switch NgWord2 system archive */
void yw4_ngc_ProfanityFilter_Initialize(CPUState *c) { c->x[0] = 0; }
void yw4_ngc_ProfanityFilter_Initialize_b(CPUState *c) { c->x[0] = 0; }
void yw4_ngc_ProfanityFilter_Finalize(CPUState *c) { c->x[0] = 0; }
void yw4_ngc_ProfanityFilter_GetContentVersion(CPUState *c) { c->x[0] = 1; }
void yw4_ngc_ProfanityFilter_MaskProfanityWordsInText_c(CPUState *c) {
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 0;
    c->x[0] = 0;
}
void yw4_ngc_ProfanityFilter_MaskProfanityWordsInText_ds(CPUState *c) {
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 0;
    c->x[0] = 0;
}
void yw4_ngc_ProfanityFilter_MaskProfanityWordsInText_t(CPUState *c) {
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = 0;
    c->x[0] = 0;
}
void yw4_ngc_ProfanityFilter_CheckProfanityWords_c(CPUState *c) {
    if (c->x[1]) memset((void *)(uintptr_t)c->x[1], 0, c->x[4] * sizeof(uint32_t));
    c->x[0] = 0;
}
void yw4_ngc_ProfanityFilter_CheckProfanityWords_ds(CPUState *c) {
    if (c->x[1]) memset((void *)(uintptr_t)c->x[1], 0, c->x[4] * sizeof(uint32_t));
    c->x[0] = 0;
}
void yw4_ngc_ProfanityFilter_CheckProfanityWords_t(CPUState *c) {
    if (c->x[1]) memset((void *)(uintptr_t)c->x[1], 0, c->x[4] * sizeof(uint32_t));
    c->x[0] = 0;
}

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
    char token[96];
    snprintf(token, sizeof token, "%s", initial);  /* as the game gave it, for the log */
    if (initial[0] == '<') {
        /* The game's starting text is a name's token (the one its texts use for that character's name), e.g.
         * "<NAME_KETA_8888>" for the boy: each main character's default name in the English patch (system_text,
         * 2026-11-07); "My name is..." asks three times, so the token, not the question, tells them apart. Other
         * tokens: no default. A Yo-kai's nickname or a My Set's name starts with real text, kept as is. */
        const char *known = main_name_for(initial);
        snprintf(initial, sizeof initial, "%s", known ? known : "");
    }
    /* a new game asks twice: the boy's name, then "the girl he loves" (English header; 女 in Japanese) */
    int girl = 0;
    for (int i = 0; i < 64; i++) {
        uint16_t ch = *(const uint16_t *)(a + 0x24 + 2 * i);
        if (!ch) break;
        if (ch == 0x5973 || (ch == 'g' && *(const uint16_t *)(a + 0x26 + 2 * i) == 'i' &&
                             *(const uint16_t *)(a + 0x28 + 2 * i) == 'r' && *(const uint16_t *)(a + 0x2a + 2 * i) == 'l'))
            girl = 1;
    }
    /* the popup's default: only what the game gave (a character's name, a Yo-kai's name...); a password has none.
     * Replays answer at once: there "Nate" (or "Katie") when the game gave nothing. */
    const char *fallback = initial[0] ? initial : girl ? DEFAULT_GIRL : DEFAULT_NAME;
    int min = *(const int32_t *)(a + 0x3b0), main_name = token[0] == '<';
    /* The player types it (board N1): the overlay's popup over the game's last frame, which this thread shows again
     * while it waits (the game waits here too, as on the Switch). Not in replays (their clock counts frames, which
     * stop here) or with YW4_PLAYER_NAME: those answer at once. */
    static char typed[TEXTIN_MAX];
    int checked = 0;
    const char *replay = getenv("YW4_REPLAY"), *force = getenv("YW4_NAME_PROMPT");  /* =1: the popup in replays too */
    if ((!name || !*name) && (!(replay && *replay) || (force && *force == '1')) && window_repaint(1) == 0) {
        char title[200];
        int n = 0;
        for (int i = 0; i < 64 && n < (int)sizeof title - 4; i++) {  /* the game's question, UTF-16 to UTF-8 */
            uint16_t ch = *(const uint16_t *)(a + 0x24 + 2 * i);
            if (!ch) break;
            if (ch < 0x80) title[n++] = (char)ch;
            else if (ch < 0x800) title[n++] = (char)(0xC0 | ch >> 6), title[n++] = (char)(0x80 | (ch & 63));
            else title[n++] = (char)(0xE0 | ch >> 12), title[n++] = (char)(0x80 | (ch >> 6 & 63)),
                 title[n++] = (char)(0x80 | (ch & 63));
        }
        title[n] = 0;
        /* Cancel everywhere but a main character's name (the start of a new game: how the game takes a cancelled
         * name there isn't known, so it isn't offered) */
        for (;;) {
            gamemenu_prompt_options(min, !main_name);
            gamemenu_prompt_begin(title, max, initial);
            int how;
            while (!(how = gamemenu_prompt_take(typed, sizeof typed))) {
                window_repaint(0);
                host_sleep_ns(16 * 1000000LL);
            }
            window_repaint(0);  /* the popup gone before the game goes on */
            if (how == 2) {  /* Cancel: what the sdk returns when the keyboard is closed (nn::swkbd's 0x29f, read from
                              * ShowKeyboard), the string left as it is */
                fprintf(stderr, "yw4: swkbd: cancelled\n");
                c->x[0] = 0x29f;
                return;
            }
            const char *cand = typed[0] ? typed : fallback;
            if (!check_name(c, a, cand, utf8, max)) {
                name = cand;
                checked = 1;
                break;
            }
            fprintf(stderr, "yw4: swkbd: name \"%s\" rejected by text check callback\n", cand);
            gamemenu_message_begin(title, "This name cannot be used.");
            char dummy[16];
            while (!gamemenu_prompt_take(dummy, sizeof dummy)) {
                window_repaint(0);
                host_sleep_ns(16 * 1000000LL);
            }
            window_repaint(0);
            snprintf(initial, sizeof initial, "%s", cand);
        }
    }
    if (!name || !*name) name = fallback;
    if (!checked) check_name(c, a, name, utf8, max);
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
            name, utf8 ? "UTF-8" : "UTF-16", (unsigned long long)chars, (unsigned long long)size, token);
    c->x[0] = 0;
}
