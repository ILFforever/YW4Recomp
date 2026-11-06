/* Typing text in the overlay (2026-11-07): the bug report's "What happened?" box first; the game's own name entry
 * (swkbd.c, which still answers a fixed name) can use the same later. One text field at a time has the typing
 * focus. The window (window.c) starts the system's text input while a field has it (SDL_StartTextInput, so a
 * keyboard layout and an IME work), hands it what is typed (textin_type) and the editing keys (textin_edit), and
 * sends every other key nowhere: W A S D type letters instead of moving. Enter, Escape or Tab, or B on a pad, end
 * the typing (TI_DONE). Text is UTF-8, kept as one paragraph (a line break typed or pasted becomes a space).
 * Replays: "t <tick> <text>" types text, the keys T_LEFT... edit (svc_hid.c). */
#ifndef YW4_TEXTIN_H
#define YW4_TEXTIN_H

#include <stddef.h>
#include <stdint.h>

enum { TEXTIN_MAX = 1024 };  /* bytes, with the ending zero */

typedef struct {
    char text[TEXTIN_MAX];
    int len;     /* bytes */
    int cursor;  /* a byte offset at the start of a character */
    int max_chars;  /* at most this many characters (0: no limit beyond TEXTIN_MAX) */
} TextField;

enum { TI_LEFT, TI_RIGHT, TI_HOME, TI_END, TI_BACK, TI_DEL, TI_DONE, TI_CLEAR };

/* Gives f the typing focus (NULL: none); textin_active: a field has it. */
void textin_focus(TextField *f);
int textin_active(void);
int textin_has(const TextField *f);
/* Text typed or pasted, at the cursor (control characters dropped, line breaks as spaces; what doesn't fit is
 * cut at a whole character). */
void textin_type(const char *utf8);
void textin_edit(int op);
/* f's text and cursor as they are now, for drawing (the typing runs on the window's thread). */
void textin_read(const TextField *f, char *out, size_t n, int *cursor);
/* f emptied (a new report). */
void textin_clear(TextField *f);
/* Changes each time a field's text, cursor or focus changes (the overlay draws again then). */
uint32_t textin_changes(void);

#endif
