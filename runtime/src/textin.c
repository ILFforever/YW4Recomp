/* Typing text in the overlay (textin.h). The typing comes from the window's thread (keys) or a replay (the game's
 * input thread); the present draws the text: one lock keeps a field whole while either reads or changes it. */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "host.h"
#include "textin.h"

static HostLock lock = HOST_LOCK_INIT;
static TextField *active;
static uint32_t changes;

static void changed(void) { __atomic_add_fetch(&changes, 1, __ATOMIC_RELEASE); }
uint32_t textin_changes(void) { return __atomic_load_n(&changes, __ATOMIC_ACQUIRE); }

void textin_focus(TextField *f) {
    host_lock(&lock);
    if (active != f) {
        active = f;
        if (f) f->cursor = f->sel_anchor = f->len;  /* typing goes on at the end */
        changed();
    }
    host_unlock(&lock);
}

int textin_active(void) {
    host_lock(&lock);
    int a = active != NULL;
    host_unlock(&lock);
    return a;
}

int textin_has(const TextField *f) {
    host_lock(&lock);
    int a = f && active == f;
    host_unlock(&lock);
    return a;
}

/* UTF-8: a byte that continues a character (10xxxxxx) */
static int cont(unsigned char b) { return (b & 0xC0) == 0x80; }

static int delete_selection(TextField *f) {
    if (f->sel_anchor < 0 || f->sel_anchor == f->cursor) return 0;
    int s = f->sel_anchor < f->cursor ? f->sel_anchor : f->cursor;
    int e = f->sel_anchor > f->cursor ? f->sel_anchor : f->cursor;
    if (s < 0) s = 0;
    if (e > f->len) e = f->len;
    if (e > s) {
        memmove(f->text + s, f->text + e, (size_t)(f->len - e + 1));
        f->len -= (e - s);
        f->cursor = f->sel_anchor = s;
        return 1;
    }
    return 0;
}

void textin_type(const char *s) {
    host_lock(&lock);
    TextField *f = active;
    if (!f) {
        host_unlock(&lock);
        return;
    }
    delete_selection(f);
    char add[TEXTIN_MAX];
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < (int)sizeof add - 1; p++) {
        unsigned char ch = *p;
        if (ch == '\r') continue;
        if (ch == '\n' || ch == '\t') ch = ' ';
        else if (ch < 32 || ch == 127) continue;
        add[n++] = (char)ch;
    }
    int room = TEXTIN_MAX - 1 - f->len;
    if (n > room) {  /* cut at a whole character */
        n = room;
        while (n > 0 && cont((unsigned char)add[n])) n--;
    }
    if (f->max_chars > 0) {  /* a field with a letter limit (a name: 8): what goes past it is left out */
        int have = 0, k = 0;
        for (int i = 0; i < f->len; i++) have += !cont((unsigned char)f->text[i]);
        for (int left = f->max_chars - have; k < n; k++)
            if (!cont((unsigned char)add[k]) && left-- <= 0) break;
        n = k;
    }
    if (n > 0) {
        memmove(f->text + f->cursor + n, f->text + f->cursor, (size_t)(f->len - f->cursor + 1));
        memcpy(f->text + f->cursor, add, (size_t)n);
        f->len += n, f->cursor += n;
        f->sel_anchor = f->cursor;
        changed();
    }
    host_unlock(&lock);
}

void textin_edit(int op) {
    host_lock(&lock);
    TextField *f = active;
    if (!f) {
        host_unlock(&lock);
        return;
    }
    int c = f->cursor;
    int has_sel = f->sel_anchor >= 0 && f->sel_anchor != f->cursor;
    int s = f->sel_anchor < f->cursor ? f->sel_anchor : f->cursor;
    int e = f->sel_anchor > f->cursor ? f->sel_anchor : f->cursor;
    switch (op) {
    case TI_LEFT:
        if (has_sel) c = s;
        else if (c > 0) do c--; while (c > 0 && cont((unsigned char)f->text[c]));
        f->sel_anchor = c;
        break;
    case TI_RIGHT:
        if (has_sel) c = e;
        else if (c < f->len) do c++; while (c < f->len && cont((unsigned char)f->text[c]));
        f->sel_anchor = c;
        break;
    case TI_HOME: c = f->sel_anchor = 0; break;
    case TI_END: c = f->sel_anchor = f->len; break;
    case TI_SLEFT:
        if (c > 0) do c--; while (c > 0 && cont((unsigned char)f->text[c]));
        break;
    case TI_SRIGHT:
        if (c < f->len) do c++; while (c < f->len && cont((unsigned char)f->text[c]));
        break;
    case TI_SHOME: c = 0; break;
    case TI_SEND: c = f->len; break;
    case TI_ALL: f->sel_anchor = 0; c = f->len; break;
    case TI_BACK:
        if (has_sel) {
            delete_selection(f);
            c = f->cursor;
        } else if (c > 0) {
            int st = c;
            do st--; while (st > 0 && cont((unsigned char)f->text[st]));
            memmove(f->text + st, f->text + c, (size_t)(f->len - c + 1));
            f->len -= c - st, c = st;
            f->sel_anchor = c;
        }
        break;
    case TI_DEL:
        if (has_sel) {
            delete_selection(f);
            c = f->cursor;
        } else if (c < f->len) {
            int ed = c;
            do ed++; while (ed < f->len && cont((unsigned char)f->text[ed]));
            memmove(f->text + c, f->text + ed, (size_t)(f->len - ed + 1));
            f->len -= ed - c;
            f->sel_anchor = c;
        }
        break;
    case TI_COPY:
        if (has_sel) {
            char buf[TEXTIN_MAX];
            int n = e - s < (int)sizeof buf - 1 ? e - s : (int)sizeof buf - 1;
            memcpy(buf, f->text + s, (size_t)n);
            buf[n] = 0;
            SDL_SetClipboardText(buf);
        }
        break;
    case TI_CUT:
        if (has_sel) {
            char buf[TEXTIN_MAX];
            int n = e - s < (int)sizeof buf - 1 ? e - s : (int)sizeof buf - 1;
            memcpy(buf, f->text + s, (size_t)n);
            buf[n] = 0;
            SDL_SetClipboardText(buf);
            delete_selection(f);
            c = f->cursor;
        }
        break;
    case TI_CLEAR: f->len = 0, f->text[0] = 0, c = f->sel_anchor = 0; break;
    case TI_DONE: active = NULL; break;
    }
    f->cursor = c;
    changed();
    host_unlock(&lock);
}

void textin_set_caret(int pos) {
    host_lock(&lock);
    TextField *f = active;
    if (f) {
        if (pos < 0) pos = 0;
        if (pos > f->len) pos = f->len;
        while (pos > 0 && pos < f->len && cont((unsigned char)f->text[pos])) pos++;
        f->cursor = f->sel_anchor = pos;
        changed();
    }
    host_unlock(&lock);
}

void textin_drag(int pos) {
    host_lock(&lock);
    TextField *f = active;
    if (f) {
        if (pos < 0) pos = 0;
        if (pos > f->len) pos = f->len;
        while (pos > 0 && pos < f->len && cont((unsigned char)f->text[pos])) pos++;
        if (f->cursor != pos) {
            f->cursor = pos;
            changed();
        }
    }
    host_unlock(&lock);
}

static int is_word_space(const char *text, int i) {
    unsigned char b = (unsigned char)text[i];
    if (b <= ' ' || b == 127) return 1;
    if (b == 0xE3 && (unsigned char)text[i + 1] == 0x80 && (unsigned char)text[i + 2] == 0x80) return 1;
    return 0;
}

void textin_select_word(int pos) {
    host_lock(&lock);
    TextField *f = active;
    if (f && f->len > 0) {
        if (pos < 0) pos = 0;
        if (pos >= f->len) pos = f->len - 1;
        while (pos > 0 && cont((unsigned char)f->text[pos])) pos--;
        int space = is_word_space(f->text, pos);
        int w_start = pos;
        while (w_start > 0) {
            int prev = w_start - 1;
            while (prev > 0 && cont((unsigned char)f->text[prev])) prev--;
            if (is_word_space(f->text, prev) != space) break;
            w_start = prev;
        }
        int w_end = pos;
        while (w_end < f->len) {
            int next = w_end + 1;
            while (next < f->len && cont((unsigned char)f->text[next])) next++;
            if (is_word_space(f->text, w_end) != space) break;
            w_end = next;
        }
        f->sel_anchor = w_start;
        f->cursor = w_end;
        changed();
    }
    host_unlock(&lock);
}

int textin_get_selection(char *out, size_t n) {
    host_lock(&lock);
    TextField *f = active;
    int len = 0;
    if (f && f->sel_anchor >= 0 && f->sel_anchor != f->cursor && out && n > 0) {
        int s = f->sel_anchor < f->cursor ? f->sel_anchor : f->cursor;
        int e = f->sel_anchor > f->cursor ? f->sel_anchor : f->cursor;
        len = e - s < (int)n - 1 ? e - s : (int)n - 1;
        memcpy(out, f->text + s, (size_t)len);
        out[len] = 0;
    } else if (out && n > 0) {
        out[0] = 0;
    }
    host_unlock(&lock);
    return len;
}

void textin_read(const TextField *f, char *out, size_t n, int *cursor, int *sel_anchor) {
    host_lock(&lock);
    snprintf(out, n, "%s", f->text);
    if (cursor) *cursor = f->cursor < (int)n ? f->cursor : (int)n - 1;
    if (sel_anchor) *sel_anchor = f->sel_anchor < (int)n ? f->sel_anchor : (int)n - 1;
    host_unlock(&lock);
}

void textin_clear(TextField *f) {
    host_lock(&lock);
    f->len = 0, f->cursor = 0, f->sel_anchor = 0, f->text[0] = 0;
    if (active == f) active = NULL;
    changed();
    host_unlock(&lock);
}
