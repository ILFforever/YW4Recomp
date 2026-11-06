/* Typing text in the overlay (textin.h). The typing comes from the window's thread (keys) or a replay (the game's
 * input thread); the present draws the text: one lock keeps a field whole while either reads or changes it. */
#include <stdio.h>
#include <string.h>

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
        if (f) f->cursor = f->len;  /* typing goes on at the end */
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

void textin_type(const char *s) {
    host_lock(&lock);
    TextField *f = active;
    if (!f) {
        host_unlock(&lock);
        return;
    }
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
    if (n > 0) {
        memmove(f->text + f->cursor + n, f->text + f->cursor, (size_t)(f->len - f->cursor + 1));
        memcpy(f->text + f->cursor, add, (size_t)n);
        f->len += n, f->cursor += n;
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
    switch (op) {
    case TI_LEFT:
        if (c > 0) do c--; while (c > 0 && cont((unsigned char)f->text[c]));
        break;
    case TI_RIGHT:
        if (c < f->len) do c++; while (c < f->len && cont((unsigned char)f->text[c]));
        break;
    case TI_HOME: c = 0; break;
    case TI_END: c = f->len; break;
    case TI_BACK:
        if (c > 0) {
            int s = c;
            do s--; while (s > 0 && cont((unsigned char)f->text[s]));
            memmove(f->text + s, f->text + c, (size_t)(f->len - c + 1));
            f->len -= c - s, c = s;
        }
        break;
    case TI_DEL:
        if (c < f->len) {
            int e = c;
            do e++; while (e < f->len && cont((unsigned char)f->text[e]));
            memmove(f->text + c, f->text + e, (size_t)(f->len - e + 1));
            f->len -= e - c;
        }
        break;
    case TI_CLEAR: f->len = 0, f->text[0] = 0, c = 0; break;
    case TI_DONE: active = NULL; break;
    }
    f->cursor = c;
    changed();
    host_unlock(&lock);
}

void textin_read(const TextField *f, char *out, size_t n, int *cursor) {
    host_lock(&lock);
    snprintf(out, n, "%s", f->text);
    if (cursor) *cursor = f->cursor < (int)n ? f->cursor : (int)n - 1;
    host_unlock(&lock);
}

void textin_clear(TextField *f) {
    host_lock(&lock);
    f->len = 0, f->cursor = 0, f->text[0] = 0;
    if (active == f) active = NULL;
    changed();
    host_unlock(&lock);
}
