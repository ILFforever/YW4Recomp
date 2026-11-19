/* The in-game overlay's side panel (gamemenu.h): which item is chosen, what each does, and how it looks. Runs on
 * the window's thread (key and pad events); the present reads the state to draw it. The layout's numbers are the
 * design's (Main.dc.html on the overlay's design canvas), in its CSS pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL_scancode.h>

#include "gamemenu.h"
#include "battle.h"
#include "bindings.h"
#include "mousemenu.h"
#include "bugreport.h"
#include "cheats.h"
#include "savefs.h"
#include "settings.h"
#include "textin.h"
#include "theme.h"
#include "window.h"
#include "host.h"
#include "romfs.h"
#include "discord.h"
#include "relay.h"

static int open_, sel, confirm, choice;
static int focus;             /* 0 the side panel, 1 the page it shows (Settings) */
static int srow;              /* the chosen row on the settings page (settings_player_rows) */
static int in_rows;           /* the settings page: 0 the sections list has the focus, 1 the chosen section's rows */
/* the settings page's rows as last drawn: the first shown of the section */
static int set_first;
static int prow;              /* the chosen row on the performance page: mode, corner, the four parts */
static uint32_t changes;      /* counts every change of what shows */
static int toast_wanted;      /* the note (toast_msg, top right) starts at the next look */
static uint64_t toast_until;  /* ms */
static int hover_kind = -1, hover_a, hover_b;  /* what the mouse is over (HIT_*, -1 nothing), for the hover looks */
static int ctrl_first;  /* the Controls table: its first row shown */
/* the Controls page's view: 1 the Bindings table (where it opens, as before), 0 the keyboard view (the owner's
 * mockup, 2026-11-11), switched by one button or P; the keyboard view's mode: 0 exploring, 1 battles, 2 cutscenes
 * (the exploring keys, only those that do something there) */
static int ctrl_view = 1, kv_mode;
static int mb_open;  /* the Controls page shows its Mouse buttons page (mouse_page_action) */
/* Pressing a real key lights it on the keyboard view (gamemenu_held): the drawn keys held now, by scancode (a gold
 * ring round each), the mouse's buttons held (bits: left, right, wheel), and the last lit key pressed, which stays
 * in focus as if the mouse pointed at it (a scancode or MOUSE_BIND_*, 0 none) */
static uint8_t kv_held[64];
static int kv_mouse_held, kv_focus;
static int preview, preview_hidden;  /* Preview: the game runs under the bar; the bar hidden (only its chip shows) */
static float pv_rect[4];             /* the part of the overlay the window shows while previewing (design units) */
static int bug_page, brow;           /* Report a bug: its page and chosen row (below, with the rest of it) */
static int crow;                     /* the chosen row on the Cheats page (settings_cheat_rows) */
static int orow;                     /* the chosen control on the Online page (ON_*) */

/* name entry (gamemenu.h): the popup's state; the game's thread waits in swkbd.c meanwhile */
static int prompting, prompt_finished;  /* prompt_finished: 1 answered, 2 cancelled */
static TextField prompt_text;
static char prompt_title[256], prompt_fallback[64], prompt_answer[TEXTIN_MAX];
static char prompt_body[512];  /* a message instead of a field ("" for a field) */
static const char *prompt_label = "Name";
static void (*prompt_done)(const char *);  /* not NULL: the overlay's own name entry (gamemenu_name_edit) */
static int prompt_min, prompt_cancel;  /* letters the game needs at least; Cancel offered */
/* the list popup (gamemenu_choice_begin): the New Game slot picker (savefs.c); prompting is 1 while it shows, the
 * title and prompt_body are its question; pick_n 0: not a list */
enum { PICK_MAX = 12 };
static char pick_rows[PICK_MAX][96];
static int pick_n, pick_sel, pick_answer = -1;
/* the Diary look of the list (gamemenu_diary_begin): its rows' details; diary_n slots, then the back row */
static GameMenuDiaryRow diary_rows[PICK_MAX];
static int diary_n;
/* the Diary's own title (the list's, kept while its question shows), its slots, the slot chosen when the question
 * was asked, and 1 while the question shows over it (gamemenu_diary_confirm) */
static char dy_title[256];
static int dy_slots, dy_list_sel, dy_confirm;
/* 1 once the Diary's list or question is answered: it stays on screen, taking no input, until the next one begins
 * or gamemenu_diary_end. The game asks for the next one a frame or more after the answer: closed in between, the
 * game showed for a frame on the way into and out of the question (owner, 2026-11-17: "flickers once when in and
 * once when out"). */
static int dy_hold;
static char diary_back[64];
static void changed(void);

int gamemenu_prompting(void) { return __atomic_load_n(&prompting, __ATOMIC_ACQUIRE); }

/* letters in the field, or in the default when it is empty */
static int prompt_chars(void) {
    char t[TEXTIN_MAX];
    textin_read(&prompt_text, t, sizeof t, NULL, NULL);
    const char *s = t[0] ? t : prompt_fallback;
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}
static int prompt_enough(void) { return prompt_body[0] || prompt_chars() >= (prompt_min > 0 ? prompt_min : 1); }

void gamemenu_prompt_options(int min_chars, int cancel) { prompt_min = min_chars, prompt_cancel = cancel; }

/* The on-screen keyboard (owner, 2026-11-07: "make it simple"): a letter grid in the popup while a controller is in
 * use (the hints follow the last device), as the Switch's own keyboard; QWERTY. D-pad moves, A types, B deletes (on an
 * empty field: Cancel where offered), Y capitals on or off, Done answers. The first letter is a
 * capital, the next ones small, as names are written. The mouse can press its keys too.
 * Languages (owner, 2026-11-13, mockup option A): a switcher above the keys as the iPhone's, "‹ English ›" and
 * "‹ 日本語 ›", each name in its own script; LB / RB (L1 / R1, Q / E) or its arrows step through them. English is the
 * QWERTY page; 日本語 has a hiragana and a katakana page, flipped by the あ / ア key, and comes back to the one used last.
 * A kana page is the gojūon table as Japanese keyboards lay it out: a column per consonant, the あ column on the right,
 * a i u e o downwards; its last row has ゛゜ (also Y) and 小, which change the letter before the cursor (か が,
 * は ば ぱ, つ っ). A name in the Japanese game opens on 日本語, with a translation mod (its own chara_text,
 * romfs_host_path) on English. The popup keeps the kana pages' height on the English page too. */
enum { OSK_COLS = 10, OSK_ROWS = 6 };  /* a kana page's rows: five of letters, then the last; ABC has four */
/* QWERTY (owner, 2026-11-07), the numbers on top as on a keyboard */
static const char *const osk_keys[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-.!"};
/* the kana columns from あ to わ ("": no key); katakana are the same letters moved by 0x60 */
static const char *const osk_kana[OSK_COLS][5] = {
    {"あ", "い", "う", "え", "お"}, {"か", "き", "く", "け", "こ"}, {"さ", "し", "す", "せ", "そ"},
    {"た", "ち", "つ", "て", "と"}, {"な", "に", "ぬ", "ね", "の"}, {"は", "ひ", "ふ", "へ", "ほ"},
    {"ま", "み", "む", "め", "も"}, {"や", "", "ゆ", "", "よ"},     {"ら", "り", "る", "れ", "ろ"},
    {"わ", "を", "ん", "ー", "！"}};
enum { OSK_SHIFT, OSK_SPACE, OSK_DELETE, OSK_DONE, OSK_VOICE, OSK_SMALL, OSK_SCRIPT };
typedef struct {
    const char *label;
    int c0, c1, act;  /* the columns it covers, [c0, c1) */
} OskKey;
static const OskKey osk_last_abc[] = {
    {"Shift", 0, 2, OSK_SHIFT}, {"Space", 2, 6, OSK_SPACE}, {"Delete", 6, 8, OSK_DELETE}, {"Done", 8, 10, OSK_DONE}};
/* OSK_SCRIPT's label is the other script's letter (ア on hiragana, あ on katakana), set where it's drawn */
static const OskKey osk_last_kana[] = {{"゛゜", 0, 2, OSK_VOICE},    {"小", 2, 3, OSK_SMALL},    {"ア", 3, 4, OSK_SCRIPT},
                                       {"Space", 4, 6, OSK_SPACE},  {"Delete", 6, 8, OSK_DELETE}, {"Done", 8, 10, OSK_DONE}};
/* osk_page: 0 English (QWERTY), 1 hiragana, 2 katakana; osk_kana_page: the 日本語 page used last */
static int osk_row, osk_col, osk_caps = 1, osk_shown, osk_page, osk_kana_page = 1;
/* The language a name's keyboard opens on: the one picked last (owner, 2026-11-13: "make it remember the last
 * language"), kept as YW4_KEYBOARD in the settings file (settings.c); before any pick, the mod guess below. */
static const char *const osk_lang_ids[3] = {"english", "hiragana", "katakana"};
static int osk_switched;  /* the language was changed in this popup: kept when it closes */
static int osk_remembered(void) {
    const char *e = getenv("YW4_KEYBOARD");
    for (int i = 0; e && i < 3; i++)
        if (!strcmp(e, osk_lang_ids[i])) return i;
    return -1;
}

static const OskKey *osk_last(int *n) {
    *n = osk_page ? 6 : 4;
    return osk_page ? osk_last_kana : osk_last_abc;
}
static int osk_letter_rows(void) { return osk_page ? 5 : 4; }
static int osk_last_key(int col) {
    int n;
    const OskKey *L = osk_last(&n);
    for (int k = 0; k < n; k++)
        if (col >= L[k].c0 && col < L[k].c1) return k;
    return n - 1;
}

static uint32_t osk_utf8_cp(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) return p[0];
    if (p[0] < 0xE0) return (uint32_t)(p[0] & 31) << 6 | (p[1] & 63);
    if (p[0] < 0xF0) return (uint32_t)(p[0] & 15) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63);
    return (uint32_t)(p[0] & 7) << 18 | (uint32_t)(p[1] & 63) << 12 | (uint32_t)(p[2] & 63) << 6 | (p[3] & 63);
}
static void osk_utf8_put(uint32_t cp, char out[5]) {  /* the kana are all 3 bytes; others as needed */
    if (cp < 0x80) out[0] = (char)cp, out[1] = 0;
    else if (cp < 0x800) out[0] = (char)(0xC0 | cp >> 6), out[1] = (char)(0x80 | (cp & 63)), out[2] = 0;
    else out[0] = (char)(0xE0 | cp >> 12), out[1] = (char)(0x80 | (cp >> 6 & 63)), out[2] = (char)(0x80 | (cp & 63)),
         out[3] = 0;
}

/* The letter key at (r, col) of the page, as UTF-8 ("" where the page has no key). */
static void osk_letter(int r, int col, char out[8]) {
    out[0] = 0;
    if (!osk_page) {
        out[0] = osk_keys[r][col], out[1] = 0;
        if (!osk_caps && out[0] >= 'A' && out[0] <= 'Z') out[0] = (char)(out[0] + 32);
        return;
    }
    snprintf(out, 8, "%s", osk_kana[OSK_COLS - 1 - col][r]);  /* the あ column on the right */
    uint32_t cp = osk_utf8_cp(out);
    if (osk_page == 2 && cp >= 0x3041 && cp <= 0x3096) osk_utf8_put(cp + 0x60, out);
}

/* ゛゜ and 小 on a kana letter (hiragana or katakana; anything else: unchanged). Unicode keeps each voiced letter
 * right after its plain one (か が), は's two after it (は ば ぱ), a small one right before its big one (ぁ あ). */
static uint32_t kana_change(uint32_t cp, int small) {
    uint32_t k = cp >= 0x30A1 && cp <= 0x30F6 ? 0x60 : 0, h = cp - k;
    if (!small) {
        if (h >= 0x304B && h <= 0x3062) h = (h - 0x304B) % 2 ? h - 1 : h + 1;       /* か..ぢ */
        else if (h >= 0x3064 && h <= 0x3069) h = (h - 0x3064) % 2 ? h - 1 : h + 1;  /* つ..ど */
        else if (h >= 0x306F && h <= 0x307D) h = (h - 0x306F) % 3 == 2 ? h - 2 : h + 1;  /* は..ぽ */
        else if (h == 0x3046 || h == 0x3094) h = h == 0x3046 ? 0x3094 : 0x3046;    /* う ゔ */
        else return cp;
    } else {
        if (h >= 0x3041 && h <= 0x304A) h = (h - 0x3041) % 2 ? h - 1 : h + 1;       /* ぁ..お */
        else if (h >= 0x3083 && h <= 0x3088) h = (h - 0x3083) % 2 ? h - 1 : h + 1;  /* ゃ..よ */
        else if (h == 0x3063 || h == 0x3064) h = h == 0x3063 ? 0x3064 : 0x3063;    /* っ つ */
        else if (h == 0x308E || h == 0x308F) h = h == 0x308E ? 0x308F : 0x308E;    /* ゎ わ */
        else return cp;
    }
    return h + k;
}
static void osk_change_last(int small) {  /* the letter before the cursor */
    char t[TEXTIN_MAX], ch[5];
    int cur = 0;
    textin_read(&prompt_text, t, sizeof t, &cur, NULL);
    if (cur <= 0) return;
    int s = cur - 1;
    while (s > 0 && ((unsigned char)t[s] & 0xC0) == 0x80) s--;
    uint32_t cp = osk_utf8_cp(t + s), to = kana_change(cp, small);
    if (to == cp) return;
    osk_utf8_put(to, ch);
    textin_focus(&prompt_text), textin_edit(TI_BACK), textin_type(ch);
}

static void prompt_finish(void);
static void osk_press(void) {
    if (osk_row < osk_letter_rows()) {
        char ch[8];
        osk_letter(osk_row, osk_col, ch);
        if (!ch[0]) return;
        textin_focus(&prompt_text);
        textin_type(ch);
        if (!osk_page && ch[0] != ' ') osk_caps = 0;  /* a capital first, then small letters */
        return;
    }
    int n;
    switch (osk_last(&n)[osk_last_key(osk_col)].act) {
    case OSK_SHIFT: osk_caps ^= 1; break;
    case OSK_SPACE: textin_focus(&prompt_text), textin_type(" "); break;
    case OSK_DELETE: textin_focus(&prompt_text), textin_edit(TI_BACK); break;
    case OSK_DONE: prompt_finish(); break;
    case OSK_VOICE: osk_change_last(0); break;
    case OSK_SMALL: osk_change_last(1); break;
    case OSK_SCRIPT: osk_page = osk_kana_page = osk_page == 1 ? 2 : 1, osk_switched = 1; break;  /* the cursor stays */
    }
}

static int osk_has(int r, int col) {  /* a key at (r, col): the last row, or a letter there */
    if (r == OSK_ROWS - 1) return 1;
    if (r >= osk_letter_rows()) return 0;
    char ch[8];
    osk_letter(r, col, ch);
    return ch[0] != 0;
}

static void osk_move(int a) {
    if (a == GM_UP || a == GM_DOWN) {  /* past the gaps (や's column; ABC's row left free) */
        int r = osk_row;
        do r = a == GM_UP ? (r + OSK_ROWS - 1) % OSK_ROWS : (r + 1) % OSK_ROWS;
        while (!osk_has(r, osk_col));
        osk_row = r;
    } else if (osk_row == OSK_ROWS - 1) {  /* the last row moves key by key */
        int n;
        const OskKey *L = osk_last(&n);
        int k = osk_last_key(osk_col);
        k = a == GM_LEFT ? (k + n - 1) % n : (k + 1) % n;
        osk_col = L[k].c0;
    } else {
        do osk_col = a == GM_LEFT ? (osk_col + OSK_COLS - 1) % OSK_COLS : (osk_col + 1) % OSK_COLS;
        while (!osk_has(osk_row, osk_col));
    }
}

static void osk_set_page(int p) {
    osk_page = p;
    if (p) osk_kana_page = p;
    if (!osk_has(osk_row, osk_col)) osk_row = osk_row >= osk_letter_rows() && osk_row < OSK_ROWS - 1 ? osk_letter_rows() - 1 : 0;
    if (!osk_has(osk_row, osk_col)) osk_row = 0;
}
/* The language switcher's next one, either way: English ⇄ 日本語 (more languages would join this loop). */
static void osk_step_language(void) { osk_set_page(osk_page ? 0 : osk_kana_page), osk_switched = 1; }

void gamemenu_prompt_begin(const char *title, int max_chars, const char *fallback) {
    snprintf(prompt_title, sizeof prompt_title, "%s", title && *title ? title : "Enter a name");
    snprintf(prompt_fallback, sizeof prompt_fallback, "%s", fallback ? fallback : "");
    prompt_body[0] = 0;
    /* the field's label from the game's question: a password, a code, else a name */
    char low[256];
    size_t i = 0;
    for (; prompt_title[i] && i < sizeof low - 1; i++)
        low[i] = (char)(prompt_title[i] >= 'A' && prompt_title[i] <= 'Z' ? prompt_title[i] + 32 : prompt_title[i]);
    low[i] = 0;
    prompt_label = strstr(low, "password") ? "Password" : strstr(low, "code") ? "Code" : "Name";
    textin_clear(&prompt_text);
    prompt_text.max_chars = max_chars > 0 ? max_chars : 0;
    prompt_text.bmp_only = 1;  /* the game's text: no emoji (textin.h) */
    /* A name: the language picked last; before any pick, 日本語 in the Japanese game, English with a translation mod
     * (it has its own character names, swkbd.c reads them from the same file). Passwords and codes: English. On a
     * kana page the cursor starts on あ itself (the top right), on English on Q. */
    int name = !strcmp(prompt_label, "Name"), saved = osk_remembered();
    if (!name) osk_page = 0;
    else if (saved >= 0) osk_page = saved;
    else osk_page = romfs_host_path("data/common/text/ja/chara_text.cfg.bin") ? 0 : osk_kana_page;
    if (osk_page) osk_kana_page = osk_page;
    int kana = osk_page != 0;
    osk_caps = 1, osk_switched = 0;
    if (kana) osk_row = 0, osk_col = OSK_COLS - 1;
    else osk_row = 1, osk_col = 0;
    __atomic_store_n(&prompt_finished, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&prompting, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    textin_focus(&prompt_text);  /* typing starts at once */
    changed();
    fprintf(stderr, "yw4: overlay: name entry \"%s\" (at most %d letters)\n", prompt_title, max_chars);
}

void gamemenu_name_edit(const char *title, int max_chars, const char *fallback, const char *initial,
                        void (*done)(const char *name)) {
    if (gamemenu_prompting()) return;  /* the game's own question is up */
    gamemenu_prompt_options(0, 1);
    gamemenu_prompt_begin(title, max_chars, fallback);
    prompt_done = done;
    if (initial && *initial) textin_type(initial);  /* the field is focused: the current name, to edit */
}

void gamemenu_message_begin(const char *title, const char *body) {
    snprintf(prompt_title, sizeof prompt_title, "%s", title);
    snprintf(prompt_body, sizeof prompt_body, "%s", body);
    prompt_fallback[0] = 0;
    textin_clear(&prompt_text);
    __atomic_store_n(&prompt_finished, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&prompting, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    changed();
    fprintf(stderr, "yw4: overlay: message \"%s\"\n", prompt_title);
}

/* A list's setup. The present thread draws while the game's thread sets a list up (no lock): its contents are
 * written first and which look it has last, and between two of the Diary's screens the look never passes through
 * the plain list's, which showed for a frame when answers came quickly (owner, 2026-11-17: "spamming ... can still
 * cause the overwrite to blink"). dslots: the Diary's list with that many slots; conf: the Diary's question, over
 * the list with list_sel chosen. */
static void choice_setup(const char *title, const char *body, const char *const *rows, int n, int sel, int dslots,
                         int conf, int list_sel) {
    if (n > PICK_MAX) n = PICK_MAX;
    if (n < 1) return;
    snprintf(prompt_title, sizeof prompt_title, "%s", title);
    snprintf(prompt_body, sizeof prompt_body, "%s", body ? body : "");
    for (int i = 0; i < n; i++) snprintf(pick_rows[i], sizeof pick_rows[i], "%s", rows[i]);
    prompt_fallback[0] = 0;
    textin_clear(&prompt_text);
    textin_focus(NULL);
    __atomic_store_n(&pick_answer, -1, __ATOMIC_RELEASE);
    sel = sel >= 0 && sel < n ? sel : 0;
    if (conf) {  /* the page under it shows list_sel from now on: then the question's own state */
        dy_list_sel = list_sel;
        __atomic_store_n(&dy_confirm, 1, __ATOMIC_RELEASE);
        pick_sel = sel;
        __atomic_store_n(&pick_n, n, __ATOMIC_RELEASE);
        __atomic_store_n(&diary_n, 0, __ATOMIC_RELEASE);
    } else if (dslots) {  /* the list's state, then its look */
        pick_sel = sel;
        __atomic_store_n(&pick_n, n, __ATOMIC_RELEASE);
        __atomic_store_n(&diary_n, dslots, __ATOMIC_RELEASE);
        __atomic_store_n(&dy_confirm, 0, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&diary_n, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&dy_confirm, 0, __ATOMIC_RELEASE);
        pick_sel = sel;
        __atomic_store_n(&pick_n, n, __ATOMIC_RELEASE);
    }
    dy_hold = 0;
    __atomic_store_n(&prompt_finished, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&prompting, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    changed();
    fprintf(stderr, "yw4: overlay: list \"%s\" (%d rows)\n", prompt_title, n);
}

void gamemenu_choice_begin(const char *title, const char *body, const char *const *rows, int n, int sel) {
    choice_setup(title, body, rows, n, sel, 0, 0, 0);
}

void gamemenu_diary_begin(const char *title, const GameMenuDiaryRow *rows, int n, int sel, const char *back) {
    if (n > PICK_MAX - 1) n = PICK_MAX - 1;
    if (n < 1) return;
    const char *names[PICK_MAX];
    for (int i = 0; i < n; i++) diary_rows[i] = rows[i], names[i] = rows[i].name[0] ? rows[i].name : "Empty Diary";
    snprintf(diary_back, sizeof diary_back, "%s", back ? back : "Back");
    names[n] = diary_back;
    dy_slots = n;
    snprintf(dy_title, sizeof dy_title, "%s", title);
    choice_setup(title, NULL, names, n + 1, sel, n, 0, 0);
}

void gamemenu_diary_confirm(const char *title, const char *body, const char *no, const char *yes) {
    const char *rows[2] = {no, yes};
    choice_setup(title, body, rows, 2, 0, 0, 1, pick_sel /* the slot just chosen: the list's answer */);
}

static void prompt_close(int how);
void gamemenu_diary_end(void) {
    if (!dy_hold) return;
    dy_hold = 0;
    __atomic_store_n(&pick_n, 0, __ATOMIC_RELEASE);
    diary_n = 0, dy_confirm = 0;
    prompt_body[0] = 0;
    prompt_close(1);
}

static void pick_finish(void) {  /* A, Enter or a click: the chosen row is the answer */
    int a = pick_sel;
    if (diary_n || dy_confirm) {  /* the Diary's: kept on screen until the next one or gamemenu_diary_end */
        if (dy_hold) return;
        dy_hold = 1;
        changed();
        __atomic_store_n(&pick_answer, a, __ATOMIC_RELEASE);
        fprintf(stderr, "yw4: overlay: list row %d chosen\n", a);
        return;
    }
    __atomic_store_n(&pick_n, 0, __ATOMIC_RELEASE);
    diary_n = 0, dy_confirm = 0;
    prompt_body[0] = 0;
    prompt_close(1);
    __atomic_store_n(&pick_answer, a, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: overlay: list row %d chosen\n", a);
}
static void pick_move(int d) {
    int n = __atomic_load_n(&pick_n, __ATOMIC_ACQUIRE);
    if (n) pick_sel = (pick_sel + d + n) % n, changed();  /* round, as the Diary's slots */
}

int gamemenu_choice_take(void) { return __atomic_exchange_n(&pick_answer, -1, __ATOMIC_ACQ_REL); }

static int live_popup;  /* gamemenu_live_popup: the popup over the running game */
static float live_rect[4];  /* the live popup's area in design units, its shadow included (gamemenu_live_rect) */
void gamemenu_live_rect(float r[4]) { memcpy(r, live_rect, sizeof live_rect); }
void gamemenu_live_popup(void) { __atomic_store_n(&live_popup, 1, __ATOMIC_RELEASE), changed(); }
int gamemenu_live(void) { return gamemenu_open() && __atomic_load_n(&live_popup, __ATOMIC_ACQUIRE); }

static void prompt_close(int how) {
    __atomic_store_n(&live_popup, 0, __ATOMIC_RELEASE);
    if (osk_switched && !strcmp(prompt_label, "Name")) {  /* the next name's keyboard opens on this language */
#ifdef _WIN32
        _putenv_s("YW4_KEYBOARD", osk_lang_ids[osk_page]);
#else
        setenv("YW4_KEYBOARD", osk_lang_ids[osk_page], 1);
#endif
        settings_save();
        fprintf(stderr, "yw4: overlay: keyboard language %s kept\n", osk_lang_ids[osk_page]);
    }
    osk_switched = 0;
    textin_focus(NULL);
    if (prompt_done) {  /* the overlay's own (a settings row): the answer goes to the row, the overlay stays open */
        void (*done)(const char *) = prompt_done;
        prompt_done = NULL;
        __atomic_store_n(&prompting, 0, __ATOMIC_RELEASE);
        if (how == 1) done(prompt_answer);
        changed();
        return;
    }
    __atomic_store_n(&prompting, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&open_, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&prompt_finished, how, __ATOMIC_RELEASE);
    changed();
}

static void prompt_finish(void) {
    if (!gamemenu_prompting()) return;
    if (__atomic_load_n(&pick_n, __ATOMIC_ACQUIRE)) {
        pick_finish();
        return;
    }
    if (!prompt_enough()) {  /* too short for the game (a password needs a letter): keep typing */
        textin_focus(&prompt_text);
        changed();
        return;
    }
    textin_read(&prompt_text, prompt_answer, sizeof prompt_answer, NULL, NULL);
    if (!prompt_answer[0]) snprintf(prompt_answer, sizeof prompt_answer, "%s", prompt_fallback);
    prompt_close(1);
}

void gamemenu_prompt_cancel(void) {
    if (__atomic_load_n(&pick_n, __ATOMIC_ACQUIRE)) {
        if (dy_hold) return;
        /* a list has no Cancel, one row must be chosen; the Diary's has its back row: Back chooses it */
        if (diary_n) pick_sel = diary_n, pick_finish();
        else if (dy_confirm) pick_sel = 0, pick_finish();  /* the Diary's question: Back is its No */
        return;
    }
    if (gamemenu_prompting() && (prompt_cancel || prompt_body[0])) prompt_close(prompt_body[0] ? 1 : 2);
}

int gamemenu_prompt_take(char *out, size_t n) {
    /* Enter or Tab end the typing (window.c): that is OK here, as is the pad's A or the OK button */
    if (gamemenu_prompting() && !pick_n && !prompt_body[0] && !textin_has(&prompt_text)) prompt_finish();
    int how = __atomic_exchange_n(&prompt_finished, 0, __ATOMIC_ACQ_REL);
    if (how == 1) snprintf(out, n, "%s", prompt_answer);
    return how;
}

int gamemenu_open(void) { return __atomic_load_n(&open_, __ATOMIC_ACQUIRE); }
int gamemenu_previewing(void) { return __atomic_load_n(&preview, __ATOMIC_ACQUIRE); }
int gamemenu_preview_bar_hidden(void) {
    return __atomic_load_n(&preview, __ATOMIC_ACQUIRE) && __atomic_load_n(&preview_hidden, __ATOMIC_ACQUIRE);
}

/* F2 with a keyboard (owner, 2026-11-11: "no need to pause, a quick toggle"): the Controls page's keyboard Overview
 * alone over the running game, not paused, the player's input still going to the game; its mode follows the game
 * (a battle's keys during one). F2 again hides it; opening the overlay hides it too. */
static int peek_;
/* the quick look's Exploring / Battle / Cutscene switch: -1 follows the game; the player's choice (a click on the
 * switch; on a pad a short press of View / Create / Minus, round and round; no keys, keyboard players use the
 * mouse) holds until the card hides (owner, 2026-11-13) */
static int peek_mode = -1;
int gamemenu_peeking(void) { return __atomic_load_n(&peek_, __ATOMIC_ACQUIRE) && !gamemenu_open(); }
/* In a battle the card also pauses the fight (owner, 2026-11-18): opening it presses Plus, which opens the
 * Stopwatch (the battle's pause; it may show), unless the Stopwatch is open already; closing it closes the Stopwatch
 * again if this card opened it and it is still open. Exploring, the card stays a quick look over the running game. */
enum { PEEK_PLUS = 1 << 10, PEEK_CLOSE = 1 << 1 };  /* svc_hid bits: Plus; B closes the Stopwatch */
static int peek_paused;  /* this card opened the Stopwatch */
void gamemenu_peek_toggle(void) {
    int on = !__atomic_load_n(&peek_, __ATOMIC_ACQUIRE);
    if (on) __atomic_store_n(&peek_mode, -1, __ATOMIC_RELEASE);  /* each time it opens: the game's mode */
    __atomic_store_n(&peek_, on, __ATOMIC_RELEASE);
    __atomic_add_fetch(&changes, 1, __ATOMIC_ACQ_REL);
    if (on && battle_active() && !mousemenu_stopwatch()) {
        window_press_for_player(PEEK_PLUS), peek_paused = 1;
        fprintf(stderr, "yw4: overlay: F2 card in a battle: Plus (the Stopwatch)\n");
    } else if (!on && peek_paused) {
        peek_paused = 0;
        if (battle_active() && mousemenu_stopwatch()) {
            window_press_for_player(PEEK_CLOSE);
            fprintf(stderr, "yw4: overlay: F2 card closed: B (the Stopwatch closed)\n");
        }
    }
}
static int peek_shown_mode(void) {
    int m = __atomic_load_n(&peek_mode, __ATOMIC_ACQUIRE);
    return m >= 0 ? m : battle_active() ? 1 : 0;
}
void gamemenu_peek_mode_step(int d) {
    if (!gamemenu_peeking()) return;
    int m = (peek_shown_mode() + d + 3) % 3;  /* round again after the last (one button cycles it) */
    __atomic_store_n(&peek_mode, m, __ATOMIC_RELEASE);
    __atomic_add_fetch(&changes, 1, __ATOMIC_ACQ_REL);
}
void gamemenu_preview_rect(float r[4]) { memcpy(r, pv_rect, sizeof pv_rect); }

static float focused_text_box[4];
static float focused_caret_x;
static int focused_text_valid;
static float draw_text_box[4];
static float draw_text_caret;
static int draw_text_has;

int gamemenu_text_rect(float r[4], float *caret_x) {
    if (!__atomic_load_n(&focused_text_valid, __ATOMIC_ACQUIRE) || !textin_active()) return 0;
    if (r) memcpy(r, focused_text_box, 4 * sizeof(float));
    if (caret_x) *caret_x = focused_caret_x;
    return 1;
}
int gamemenu_selected(void) { return sel; }
int gamemenu_confirming(void) { return confirm; }

static void changed(void) { __atomic_add_fetch(&changes, 1, __ATOMIC_RELEASE); }
static void toast(const char *msg);  /* below: a short note, top right */

/* The popups' second line: how long ago the game saved (savefs.c: its last Commit, else its save's newest file) */
static void save_age(char *out, size_t n) {
    int64_t t = savefs_last_save(), s = (int64_t)time(NULL) - t;
    if (!t) {
        snprintf(out, n, "No save found yet.");
        return;
    }
    if (s < 0) s = 0;
    if (s < 60) snprintf(out, n, "Last saved %lld s ago.", (long long)s);
    else if (s < 3600) snprintf(out, n, "Last saved %lld min %lld s ago.", (long long)(s / 60), (long long)(s % 60));
    else snprintf(out, n, "Last saved %lld h %lld min ago.", (long long)(s / 3600), (long long)(s / 60 % 60));
}

uint32_t gamemenu_look(uint64_t now) {
    if (__atomic_exchange_n(&toast_wanted, 0, __ATOMIC_ACQ_REL)) toast_until = now + 1600;
    /* also when a key capture ends by itself (five seconds) */
    /* a popup's "Last saved ... ago" counts up: drawn again each second while it shows */
    uint32_t tick = confirm ? (uint32_t)(now / 1000) * 0x10000u : 0;
    tick += (uint32_t)(window_pause_title_state() + 1) * 0x01000000u;  /* Return to title greyed or not: drawn again */
    return tick + (__atomic_load_n(&changes, __ATOMIC_ACQUIRE) + textin_changes()) * 4u + (now < toast_until) +
           2u * (uint32_t)bindings_capturing() + (gamemenu_peeking() ? 0x40000000u + 0x20000000u * (uint32_t)battle_active() : 0);
}

static void close_and_resume(void) {
    if (settings_open()) settings_toggle();
    bindings_cancel();
    textin_focus(NULL);  /* no typing into a closed overlay (what was typed stays for next time) */
    __atomic_store_n(&focused_text_valid, 0, __ATOMIC_RELEASE);
    confirm = 0, focus = 0;
    __atomic_store_n(&preview, 0, __ATOMIC_RELEASE);  /* the overlay button in the preview: the game runs on */
    __atomic_store_n(&open_, 0, __ATOMIC_RELEASE);
    if (window_paused()) window_pause_toggle();
    changed();
    fprintf(stderr, "yw4: overlay: closed\n");
}

/* Return to title: closed as for Resume, but the game's pause screen is answered with Return to Title (window.c).
 * Where the game didn't pause (a cutscene) there is no pause screen to answer: a note, and the overlay stays. */
static void close_and_resume_to_title(void) {
    if (!window_pause_to_title()) {
        confirm = 0;
        toast("You can't return to the title screen here");
        changed();
        fprintf(stderr, "yw4: overlay: return to title: the game isn't in its pause screen here\n");
        return;
    }
    if (settings_open()) settings_toggle();
    bindings_cancel();
    textin_focus(NULL);
    __atomic_store_n(&focused_text_valid, 0, __ATOMIC_RELEASE);
    confirm = 0, focus = 0;
    __atomic_store_n(&preview, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&open_, 0, __ATOMIC_RELEASE);
    changed();
    fprintf(stderr, "yw4: overlay: closed for the title screen\n");
}

void gamemenu_toggle(void) {
    if (gamemenu_prompting()) return;  /* the name popup stays until it is answered */
    if (gamemenu_open()) {
        close_and_resume();
        return;
    }
    sel = GM_RESUME, confirm = 0, focus = 0;
    __atomic_store_n(&peek_, 0, __ATOMIC_RELEASE);
    if (!window_paused()) window_pause_toggle();  /* F4 may have paused it already */
    __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    changed();
    fprintf(stderr, "yw4: overlay: open\n");
}

static void accept(void) {
    switch (sel) {
    case GM_RESUME: close_and_resume(); break;
    case GM_SETTINGS: focus = 1, in_rows = 0; break;  /* into its page: its sections first */
    case GM_CONTROLS: focus = 1, mb_open = 0; break;
    case GM_ONLINE: focus = 1; break;
    case GM_PERF: focus = 1; break;
    case GM_CHEATS: focus = 1; break;
    case GM_BUG: focus = 1, bug_page = 0, brow = 0; break;  /* the report's first page (below) */
    case GM_TITLE:  /* "keep playing" chosen first; greyed out (window_pause_title_state): nothing */
        if (window_pause_title_state() > 0) confirm = GM_TITLE, choice = 0;
        break;
    case GM_EXIT: confirm = GM_EXIT, choice = 0; break;
    }
}

/* Preview: the rows that change the picture (SettingRow.look). In a section, from its first such row to its last:
 * Advanced's own row (Legacy menus) comes before its renderer switches and speed-ups. 0: none. */
static int look_span(const SettingRow *rows, int n, int i, int *first, int *last) {
    int a = i, z = i;
    while (a > 0 && !strcmp(rows[a - 1].category, rows[i].category)) a--;
    while (z < n - 1 && !strcmp(rows[z + 1].category, rows[i].category)) z++;
    while (a <= z && !rows[a].look) a++;
    while (z >= a && !rows[z].look) z--;
    if (a > z) return 0;
    *first = a, *last = z;
    return 1;
}

static int can_preview(void) {  /* the Settings page's chosen section has something to see */
    SettingRow rows[48];
    int n = settings_player_rows(rows, 48), a, z;
    return n > 0 && look_span(rows, n, srow < n ? srow : n - 1, &a, &z);
}

static void preview_start(void) {
    SettingRow rows[48];
    int n = settings_player_rows(rows, 48), first, last;
    if (n <= 0) return;
    if (srow >= n) srow = n - 1;
    if (!look_span(rows, n, srow, &first, &last)) return;
    if (srow < first || srow > last) srow = first;  /* on Discord status: the section's first row to see */
    preview_hidden = 0, focus = 1, in_rows = 1;
    __atomic_store_n(&preview, 1, __ATOMIC_RELEASE);
    if (window_paused()) window_pause_toggle();  /* the game runs behind the bar (the player's input held back) */
    fprintf(stderr, "yw4: overlay: preview\n");
}

static void preview_stop(void) {
    __atomic_store_n(&preview, 0, __ATOMIC_RELEASE);
    if (!window_paused()) window_pause_toggle();  /* paused again: a new picture kept for the overlay */
    fprintf(stderr, "yw4: overlay: preview ends\n");
}

void gamemenu_open_page(int item) {
    if (gamemenu_prompting() || item <= GM_RESUME || item >= GM_TITLE) return;  /* the items with a page */
    if (item == GM_CHEATS && !cheats_allowed()) return;
    if (gamemenu_open() && sel == item && focus && !preview && !confirm) {  /* the same key again: back to the game */
        close_and_resume();
        return;
    }
    if (preview) preview_stop();
    if (settings_open()) settings_toggle();  /* a replay's old panel: the overlay's page instead */
    bindings_cancel();
    textin_focus(NULL);
    __atomic_store_n(&peek_, 0, __ATOMIC_RELEASE);
    if (!gamemenu_open()) {
        if (!window_paused()) window_pause_toggle();
        __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    }
    hover_kind = -1, confirm = 0, sel = item;
    accept();  /* into the page, as Enter on its item */
    changed();
    fprintf(stderr, "yw4: overlay: open on item %d\n", item);
}

/* in the preview: Up / Down the section's rows, Left / Right (Enter) change, PREV / NEXT the graphics sections,
 * ALT hides or shows the bar, Back returns to the Settings page */
static void preview_action(int a) {
    SettingRow rows[48];
    int n = settings_player_rows(rows, 48);
    if (n <= 0) return;
    if (srow >= n) srow = n - 1;
    int first = srow, last = srow;
    if (!look_span(rows, n, srow, &first, &last)) return;
    if (srow < first || srow > last) srow = first;
    if (a == GM_UP && srow > first) srow--;
    else if (a == GM_DOWN && srow < last) srow++;
    else if (a == GM_LEFT || a == GM_RIGHT) settings_change(srow, a == GM_RIGHT ? 1 : -1);
    else if (a == GM_ACCEPT) settings_change(srow, 1);
    else if (a == GM_ALT) preview_hidden ^= 1;
    else if (a == GM_BACK || a == GM_LEAVE) preview_stop();
    else if (a == GM_PREV) {  /* the section before with something to see, its first such row */
        for (int r = first - 1, f, l; r >= 0; r--) {
            if (strcmp(rows[r].category, rows[srow].category) && look_span(rows, n, r, &f, &l)) {
                srow = f;
                return;
            }
        }
    } else if (a == GM_NEXT) {
        for (int r = last + 1, f, l; r < n; r++) {
            if (strcmp(rows[r].category, rows[srow].category) && look_span(rows, n, r, &f, &l)) {
                srow = f;
                return;
            }
        }
    }
}

/* the settings page: rows run on from one section into the next; PREV / NEXT jump to a section's first row */
static void settings_page_action(int a) {
    SettingRow rows[48];
    int n = settings_player_rows(rows, 48);
    if (n <= 0) return;
    if (srow >= n) srow = n - 1;
    int first = srow;  /* this section's first row */
    while (first > 0 && !strcmp(rows[first - 1].category, rows[srow].category)) first--;
    if (a == GM_ALT) {  /* Preview, from the sections or the rows */
        preview_start();
        return;
    }
    if (!in_rows) {  /* the sections: Up / Down choose one, Right / Enter go into its rows, Back to the side panel */
        if (a == GM_UP || a == GM_PREV) a = GM_PREV;
        else if (a == GM_DOWN || a == GM_NEXT) a = GM_NEXT;
        else if (a == GM_RIGHT || a == GM_ACCEPT) {
            in_rows = 1;
            return;
        } else if (a == GM_BACK || a == GM_LEAVE) {
            focus = 0, sel = GM_RESUME;  /* back on the side panel: Resume chosen (owner) */
            return;
        } else return;
    }
    /* the rows: the ends stop the choice (no wrapping round), so holding a direction stops at the last row */
    if (a == GM_UP) srow = srow > 0 ? srow - 1 : 0;
    else if (a == GM_DOWN) srow = srow < n - 1 ? srow + 1 : n - 1;
    else if (a == GM_LEFT || a == GM_RIGHT) settings_change(srow, a == GM_RIGHT ? 1 : -1);
    else if (a == GM_ACCEPT) settings_change(srow, 1);  /* Enter / A: the next option, as Right */
    else if (a == GM_BACK || a == GM_LEAVE) in_rows = 0, srow = first;  /* back to the sections */
    else if (a == GM_PREV || a == GM_NEXT) {
        if (a == GM_PREV) {
            if (first == 0) return;  /* the first section: stays */
            int r = first - 1;  /* a row of the section before: to its first */
            while (r > 0 && !strcmp(rows[r - 1].category, rows[r].category)) r--;
            srow = r;
        } else {
            int r = srow;
            while (r < n && !strcmp(rows[r].category, rows[srow].category)) r++;
            if (r < n) srow = r;  /* the last section: stays */
        }
    }
}

/* ---- the Controls page's Mouse buttons page (owner's pick B, 2026-11-13): the mouse drawn, an Exploring / Battle
 * switch, a row per part (left, right, the wheel press, wheel up, wheel down) with ‹ › through what it can press,
 * and a list of them all (Enter). The bindings are the mouse's own (bindings_mouse_*). */
enum { MB_ROWS = BMOUSE_PARTS, MB_PER_COL = 9 };
static int mb_row, mb_set, mb_list, mb_list_sel;
static const char *const mb_part_name[MB_ROWS] = {"Left click", "Right click", "Wheel press", "Wheel up", "Wheel down"};

/* the list's entries: 0 none, then the inputs a mouse part can press; *cur: the entry bound now */
static int mb_entries(int *ids, int max, int *cur) {
    int ch[48], n = bindings_mouse_choices(ch, 48), now = bindings_mouse_get(mb_set, mb_row);
    ids[0] = -1, *cur = 0;
    for (int k = 0; k < n && k + 1 < max; k++) {
        ids[k + 1] = ch[k];
        if (ch[k] == now) *cur = k + 1;
    }
    return n + 1 < max ? n + 1 : max;
}
static void mb_page_open(void) { mb_open = 1, mb_row = 0, mb_list = 0, mb_set = bindings_set_shown(); }
static void mb_step(int part, int d) {  /* ‹ ›: the next or the previous choice, round */
    int ids[48], cur, n;
    mb_row = part;
    n = mb_entries(ids, 48, &cur);
    bindings_mouse_set(mb_set, part, ids[(cur + d + n) % n]);
}
static void mouse_page_action(int a) {
    int ids[48], cur, n = mb_entries(ids, 48, &cur);
    if (mb_list) {  /* the list: two columns, Up / Down through all, Left / Right a column across */
        if (a == GM_UP) mb_list_sel = mb_list_sel > 0 ? mb_list_sel - 1 : 0;
        else if (a == GM_DOWN) mb_list_sel = mb_list_sel < n - 1 ? mb_list_sel + 1 : n - 1;
        else if (a == GM_LEFT && mb_list_sel >= MB_PER_COL) mb_list_sel -= MB_PER_COL;
        else if (a == GM_RIGHT && mb_list_sel + MB_PER_COL < n) mb_list_sel += MB_PER_COL;
        else if (a == GM_ACCEPT) bindings_mouse_set(mb_set, mb_row, ids[mb_list_sel]), mb_list = 0;
        else if (a == GM_BACK || a == GM_LEAVE) mb_list = 0;
        return;
    }
    if (a == GM_UP) mb_row = mb_row > 0 ? mb_row - 1 : 0;
    else if (a == GM_DOWN) mb_row = mb_row < MB_ROWS - 1 ? mb_row + 1 : MB_ROWS - 1;
    else if (a == GM_LEFT || a == GM_RIGHT) mb_step(mb_row, a == GM_RIGHT ? 1 : -1);
    else if (a == GM_ACCEPT) mb_list = 1, mb_list_sel = cur;
    else if (a == GM_CLEAR) bindings_mouse_set(mb_set, mb_row, -1);
    else if (a == GM_PREV || a == GM_NEXT) mb_set ^= 1;
    else if (a == GM_BACK || a == GM_LEAVE) mb_open = 0;  /* back to the Controls table */
}

/* the controls page: the controls' own selection and key capture (bindings.h), as the F2 page's */
static void controls_page_action(int a) {
    static const int map[] = {[GM_UP] = SETTINGS_UP, [GM_DOWN] = SETTINGS_DOWN, [GM_LEFT] = SETTINGS_LEFT,
                              [GM_RIGHT] = SETTINGS_RIGHT, [GM_ACCEPT] = SETTINGS_ACCEPT, [GM_CLEAR] = SETTINGS_CLEAR};
    if (mb_open) {
        mouse_page_action(a);
        return;
    }
    if (ctrl_view && (a == GM_ACCEPT || a == GM_RIGHT) && bindings_on_mouse_row() && !bindings_capturing()) {
        mb_page_open();  /* the Mouse buttons row: its page */
        return;
    }
    if (a == GM_BACK || a == GM_LEAVE) {
        bindings_cancel();
        focus = 0, sel = GM_RESUME;
    } else if (a == GM_ALT && !bindings_capturing()) {  /* P, Y / Triangle: Overview <-> Bindings */
        ctrl_view = !ctrl_view;
    } else if (!ctrl_view) {  /* the Overview: Q / E (LB / RB) step the mode; nothing else to choose */
        if (a == GM_PREV || a == GM_NEXT) kv_mode = (kv_mode + (a == GM_NEXT ? 1 : 2)) % 3;
    } else if (a == GM_UP || a == GM_DOWN || a == GM_LEFT || a == GM_RIGHT || a == GM_ACCEPT || a == GM_CLEAR) {
        bindings_action(map[a]);
    }
}

/* the Cheats page (canvas https://claude.ai/artifact/XcwzdE1gju4aNQJgxgE2e5 option B): Up / Down choose a cheat, Left /
 * Right (or Enter) change it, Back to the side panel */
static void cheats_page_action(int a) {
    SettingRow rows[8];
    int n = settings_cheat_rows(rows, 8);
    if (a == GM_BACK || a == GM_LEAVE || n <= 0) {
        focus = 0;
        return;
    }
    if (crow >= n) crow = n - 1;
    if (a == GM_UP) crow = crow > 0 ? crow - 1 : 0;
    else if (a == GM_DOWN) crow = crow < n - 1 ? crow + 1 : n - 1;
    else if (a == GM_LEFT || a == GM_RIGHT) settings_cheat_change(crow, a == GM_RIGHT ? 1 : -1);
    else if (a == GM_ACCEPT) settings_cheat_change(crow, 1);
}

/* the performance page: its rows are the mode (Off / Compact / Detailed), the corner, the detailed HUD's four parts,
 * then the scale and opacity sliders; Left / Right change them, A / Enter or Left / Right turn a part on or off */
static const int perf_modes[3] = {0, 2, 1};  /* the chips' order: Off, Compact, Detailed (window_hud_mode) */

static void perf_page_action(int a) {
    if (a == GM_BACK || a == GM_LEAVE) {
        focus = 0, sel = GM_RESUME;
        return;
    }
    if (a == GM_PREV || a == GM_NEXT) return;
    if (a == GM_UP) prow = prow > 0 ? prow - 1 : 0;
    else if (a == GM_DOWN) prow = prow < 7 ? prow + 1 : 7;
    else if (prow >= 6) {
        if (a == GM_LEFT || a == GM_RIGHT) settings_hud_step(prow == 7, a == GM_RIGHT ? 1 : -1);
    } else if (a == GM_LEFT || a == GM_RIGHT || a == GM_ACCEPT) {
        int d = a == GM_LEFT ? -1 : 1;
        if (prow == 0) {
            int m = window_hud_mode(), i = 0;
            while (i < 3 && perf_modes[i] != m) i++;
            window_hud_set_mode(perf_modes[((i < 3 ? i : 0) + d + 3) % 3]);
        } else if (prow == 1) {
            window_hud_set_corner((window_hud_corner() + d + 4) % 4);
        } else {
            window_hud_set_parts(window_hud_parts() ^ (1u << (prow - 2)));
        }
        settings_save();
    }
}

/* ---- the Online tab (owner, 2026-11-19, the "Cards" layout): our servers with their ping, Ark Swap's and Blasters'
 * Public / Private switch and code, the profile (player name, Discord status). Its controls in order (orow); a code
 * is passed over while its switch says Public, Discord status while it can't be used. */
enum { ON_SERVER0, ON_SERVER1, ON_SWAP, ON_SWAP_CODE, ON_BLAST, ON_BLAST_CODE, ON_NAME, ON_DISCORD, ON_ITEMS };

static int online_enabled(int i) {
    if (i == ON_SWAP_CODE) return settings_online_private(0);
    if (i == ON_BLAST_CODE) return settings_online_private(1);
    if (i == ON_DISCORD) return discord_disabled() == NULL;
    return 1;
}

/* use control i: dir -1 Left (Public, Off), 1 Right (Private, On), 0 Enter / a click (the other one, or edit) */
static void online_use(int i, int dir) {
    if (!online_enabled(i)) return;
    switch (i) {
    case ON_SERVER0: case ON_SERVER1:
        if (dir == 0) settings_online_server_set(i - ON_SERVER0);
        break;
    case ON_SWAP: case ON_BLAST: {
        int b = i == ON_BLAST, on = settings_online_private(b);
        settings_online_private_set(b, dir ? dir > 0 : !on);
        break;
    }
    case ON_SWAP_CODE: case ON_BLAST_CODE:
        if (dir == 0) settings_online_code_edit(i == ON_BLAST_CODE);
        break;
    case ON_NAME:
        if (dir == 0) settings_player_name_edit();
        break;
    case ON_DISCORD:
        if (dir == 0 || (dir > 0) != discord_enabled()) discord_toggle(), settings_save();
        break;
    }
    changed();
}

static void online_page_action(int a) {
    if (a == GM_BACK || a == GM_LEAVE) {
        focus = 0;
        return;
    }
    if (orow < 0 || orow >= ON_ITEMS) orow = 0;
    if (a == GM_UP || a == GM_DOWN) {  /* the next usable control that way; the ends stop */
        int d = a == GM_UP ? -1 : 1;
        for (int i = orow + d; i >= 0 && i < ON_ITEMS; i += d)
            if (online_enabled(i)) {
                orow = i;
                break;
            }
    } else if (a == GM_LEFT || a == GM_RIGHT) {
        if (orow <= ON_SERVER1) orow = a == GM_LEFT ? ON_SERVER0 : ON_SERVER1, online_use(orow, 0);  /* across */
        else online_use(orow, a == GM_LEFT ? -1 : 1);
    } else if (a == GM_ACCEPT) {
        online_use(orow, 0);
    }
}

/* ---- Report a bug (owner, 2026-11-07, canvas https://claude.ai/artifact/FiVFwHh4mEGWk3dCcYcZ85 board D): two short
 * pages the player fills in. Page 1: what's wrong (chips), what happened (typed, textin.h), how often (chips), Next.
 * Page 2: the paused picture, what to attach (ticks), Back, Save only, Send to GitHub. */
static const char *const bug_kinds[] = {"Graphics", "Slow or stutters", "Sound", "Controls", "Game got stuck", "Other"};
static const char *const bug_oftens[] = {"Every time", "Sometimes", "Once"};
enum { BUG_KINDS = 6, BUG_OFTENS = 3, BUG_ROWS = 4 };
static int bug_page;            /* 0 the first page, 1 the second */
static int brow;                /* the chosen row: page 1 kind, text, how often, Next; page 2 three ticks, the buttons */
static int bug_kind = -1, bug_often = -1;  /* -1: none picked */
static int bug_btn = 2;         /* page 2's buttons: 0 Back, 1 Save only, 2 Send to GitHub */
static unsigned bug_attach = 7; /* bits: 1 the screenshot, 2 the log and settings, 4 the save */
static TextField bug_text;
static const char *toast_msg = "";

static void toast(const char *msg) {
    toast_msg = msg;
    __atomic_store_n(&toast_wanted, 1, __ATOMIC_RELEASE);
}

/* Save only / Send: the zip is written at the next drawing (on the present's thread, which has the paused picture);
 * then the page "Report saved" (bug_page 2) shows, with its own three buttons */
static int bug_request;               /* 1 save, 2 save and open GitHub */
static char bug_zip[1024];            /* the last report's zip */
static char bug_sent_text[TEXTIN_MAX];
static int bug_sent_kind = -1, bug_sent_often = -1;
static int bug_done_btn;              /* the saved page: 0 Show the zip, 1 Open GitHub again, 2 Back to the game */
static int bug_sent_github;           /* the saved page: GitHub was opened (Send), not only saved */

static void bug_finish(int send) { __atomic_store_n(&bug_request, send ? 2 : 1, __ATOMIC_RELEASE); }

static BugReport bug_report_of(const char *text, int kind, int often) {
    return (BugReport){.kind = kind >= 0 ? bug_kinds[kind] : "", .often = often >= 0 ? bug_oftens[often] : "",
                       .text = text, .attach = bug_attach};
}

static void bug_reset(void) {  /* a new report starts empty */
    bug_page = 0, brow = 0, bug_kind = -1, bug_often = -1, bug_btn = 2, bug_attach = 7;
    textin_clear(&bug_text);
}

static void bug_done_press(int b) {
    if (b == 0) bugreport_show_folder(bug_zip);
    else if (b == 1) {
        BugReport r = bug_report_of(bug_sent_text, bug_sent_kind, bug_sent_often);
        bugreport_open_github(&r);
        bug_sent_github = 1;
    } else {
        bug_reset();
        close_and_resume();
    }
}

static void bug_page_action(int a) {
    if (textin_has(&bug_text)) {  /* typing: a pad's A or B ends it (the keyboard's keys are window.c's) */
        if (a == GM_BACK || a == GM_LEAVE || a == GM_ACCEPT) textin_edit(TI_DONE);
        return;
    }
    if (a == GM_PREV || a == GM_NEXT || a == GM_ALT) return;
    if (bug_page == 2) {  /* saved: Left / Right (or Up / Down) choose a button; Back leaves, the form emptied */
        if (a == GM_UP || a == GM_LEFT) bug_done_btn = bug_done_btn > 0 ? bug_done_btn - 1 : 0;
        else if (a == GM_DOWN || a == GM_RIGHT) bug_done_btn = bug_done_btn < 2 ? bug_done_btn + 1 : 2;
        else if (a == GM_ACCEPT) bug_done_press(bug_done_btn);
        else if (a == GM_BACK || a == GM_LEAVE) bug_reset(), focus = 0, sel = GM_RESUME;
        return;
    }
    if (a == GM_UP) {
        brow = brow > 0 ? brow - 1 : 0;
        return;
    }
    if (a == GM_DOWN) {
        brow = brow < BUG_ROWS - 1 ? brow + 1 : BUG_ROWS - 1;
        return;
    }
    int d = a == GM_LEFT ? -1 : a == GM_RIGHT ? 1 : 0;
    if (bug_page == 0) {
        if (a == GM_BACK || a == GM_LEAVE) {
            focus = 0, sel = GM_RESUME;
        } else if (brow == 0 && d) {
            bug_kind = bug_kind < 0 ? 0 : (bug_kind + d + BUG_KINDS) % BUG_KINDS;
        } else if (brow == 0 && a == GM_ACCEPT) {
            if (bug_kind < 0) bug_kind = 0;
            brow = 1;
        } else if (brow == 1 && a == GM_ACCEPT) {
            textin_focus(&bug_text);
        } else if (brow == 1 && a == GM_CLEAR) {
            textin_clear(&bug_text);
        } else if (brow == 2 && d) {
            bug_often = bug_often < 0 ? 0 : (bug_often + d + BUG_OFTENS) % BUG_OFTENS;
        } else if (brow == 2 && a == GM_ACCEPT) {
            if (bug_often < 0) bug_often = 0;
            brow = 3;
        } else if (brow == 3 && a == GM_ACCEPT) {
            bug_page = 1, brow = 0;
        }
        return;
    }
    if (a == GM_BACK || a == GM_LEAVE) {
        bug_page = 0, brow = 3;
    } else if (brow < 3 && (a == GM_ACCEPT || d)) {
        bug_attach ^= 1u << brow;
    } else if (brow == 3 && d) {
        bug_btn = bug_btn + d < 0 ? 0 : bug_btn + d > 2 ? 2 : bug_btn + d;
    } else if (brow == 3 && a == GM_ACCEPT) {
        if (bug_btn == 0) bug_page = 0, brow = 3;
        else bug_finish(bug_btn == 2);
    }
}

void gamemenu_action(int a) {
    if (!gamemenu_open() || settings_open()) return;
    changed();
    if (gamemenu_prompting() && __atomic_load_n(&pick_n, __ATOMIC_ACQUIRE)) {  /* a list: up / down, A picks */
        if (dy_hold) return;  /* answered: waiting for the game */
        if (a == GM_UP || a == GM_LEFT) pick_move(-1);
        else if (a == GM_DOWN || a == GM_RIGHT) pick_move(1);
        else if (a == GM_ACCEPT) pick_finish();
        else if (a == GM_BACK || a == GM_LEAVE) gamemenu_prompt_cancel();  /* the Diary's list: its back row */
        return;
    }
    if (gamemenu_prompting()) {  /* the name popup: A / Enter is OK (typing has ended), the rest nothing */
        if (osk_shown && !prompt_body[0]) {  /* the on-screen keyboard (a controller) */
            char t[8];
            textin_read(&prompt_text, t, sizeof t, NULL, NULL);
            if (a == GM_UP || a == GM_DOWN || a == GM_LEFT || a == GM_RIGHT) osk_move(a);
            else if (a == GM_ACCEPT) osk_press();
            else if (a == GM_ALT && osk_page) osk_change_last(0);  /* Y: ゛゜ on a kana page, else capitals */
            else if (a == GM_ALT) osk_caps ^= 1;
            else if (a == GM_PREV || a == GM_NEXT) osk_step_language();
            else if ((a == GM_BACK || a == GM_LEAVE || a == GM_CLEAR) && t[0]) textin_focus(&prompt_text), textin_edit(TI_BACK);
            else if (a == GM_BACK || a == GM_LEAVE) gamemenu_prompt_cancel();
            return;
        }
        if (a == GM_ACCEPT) prompt_finish();
        else if (a == GM_BACK || a == GM_LEAVE) gamemenu_prompt_cancel();  /* B: Cancel where offered; a message: OK */
        return;
    }
    hover_kind = -1;  /* the keys or the pad took over: no hover look until the mouse moves again */
    if (confirm) {
        if (a == GM_LEFT) choice = 0;
        else if (a == GM_RIGHT) choice = 1;
        else if (a == GM_UP || a == GM_DOWN) choice ^= 1;
        else if (a == GM_BACK || a == GM_LEAVE) confirm = 0;
        else if (a == GM_ACCEPT) {
            if (!choice) {
                confirm = 0;
                return;
            }
            if (confirm == GM_TITLE) {  /* the overlay closes; the window answers the game's own pause screen */
                fprintf(stderr, "yw4: overlay: return to title\n");
                close_and_resume_to_title();
                return;
            }
            fprintf(stderr, "yw4: overlay: exit game\n");
            yw4_exit(0, __FILE__, __LINE__);  /* as closing the window */
        }
        return;
    }
    if (preview) {
        preview_action(a);
        return;
    }
    if (focus) {
        if (sel == GM_CONTROLS) controls_page_action(a);
        else if (sel == GM_ONLINE) online_page_action(a);
        else if (sel == GM_PERF) perf_page_action(a);
        else if (sel == GM_BUG) bug_page_action(a);
        else if (sel == GM_CHEATS) cheats_page_action(a);
        else settings_page_action(a);
        return;
    }
    if (a == GM_UP) sel = sel > 0 ? sel - 1 : 0;
    else if (a == GM_DOWN) sel = sel < GM_ITEMS - 1 ? sel + 1 : GM_ITEMS - 1;
    if ((a == GM_UP || a == GM_DOWN) && sel == GM_CHEATS && !cheats_allowed())
        sel = a == GM_UP ? GM_PERF : GM_BUG;  /* hidden (the launcher's "Cheats" off): passed over */
    if ((a == GM_UP || a == GM_DOWN) && sel == GM_TITLE && window_pause_title_state() <= 0)
        sel = a == GM_UP ? GM_BUG : GM_EXIT;  /* greyed out: passed over */
    else if (a == GM_ACCEPT ||
             (a == GM_RIGHT && (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_ONLINE || sel == GM_PERF || sel == GM_CHEATS ||
                                sel == GM_BUG)))
        accept();
    else if (a == GM_BACK) close_and_resume();  /* Escape, Backspace, B on the side panel: back to the game */
}

void gamemenu_refresh(void) { changed(); }

/* ---- the mouse: where things are, as the last drawing put them (design units). The present draws, the window's
 * thread reads: two lists, the drawing fills the one not being read, then swaps. */

enum { HIT_SIDE, HIT_SECTION, HIT_ROW, HIT_ROW_PREV, HIT_ROW_NEXT, HIT_BIND, HIT_MODE, HIT_CORNER, HIT_CORNER_BOX,
       HIT_PART, HIT_SLIDER, HIT_POPUP, HIT_HINT, HIT_BIND_STEP, HIT_PREVIEW,
       HIT_BUG_KIND, HIT_BUG_TEXT, HIT_BUG_OFTEN, HIT_BUG_NEXT, HIT_BUG_TICK, HIT_BUG_BTN, HIT_BUG_DONE,
       HIT_PROMPT_FIELD, HIT_PROMPT_OK, HIT_PROMPT_CANCEL, HIT_OSK_KEY, HIT_OSK_PAGE,
       HIT_KV_TAB, HIT_KV_MODE, HIT_KV_KEY, HIT_PV_PART,
       HIT_MB_BACK, HIT_MB_SET, HIT_MB_ROW, HIT_MB_STEP, HIT_MB_OPT, HIT_MB_SCRIM, HIT_MB_POP, HIT_ROW_SLIDER, HIT_PICK_ROW,
       HIT_ON_ITEM };
enum { ROW_SLIDER_PAD = 12 };  /* HIT_ROW_SLIDER: the track's hit box reaches this far past each end */
#define HINT_CLOSE (-1)  /* a key hint's action: close the overlay */
typedef struct {
    float x, y, w, h;
    int kind, a, b;
} Hit;
static Hit hits[2][96];
static int hit_n[2], hit_front;  /* hit_front: the list the mouse reads */
static int hit_back(void) { return __atomic_load_n(&hit_front, __ATOMIC_ACQUIRE) ^ 1; }
static void hit_add(float x, float y, float w, float h, int kind, int a, int b) {
    int k = hit_back();
    if (hit_n[k] < (int)(sizeof hits[k] / sizeof *hits[k])) hits[k][hit_n[k]++] = (Hit){x, y, w, h, kind, a, b};
}
/* the topmost thing under (x, y): the last added */
static int hit_find(float x, float y, Hit *out) {
    int k = __atomic_load_n(&hit_front, __ATOMIC_ACQUIRE);
    for (int i = hit_n[k] - 1; i >= 0; i--) {
        const Hit *h = &hits[k][i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) {
            *out = *h;
            return 1;
        }
    }
    return 0;
}

/* The mouse over the quick look (design units): the key under it comes into focus, as on the Controls page, and
 * the mode switch shows its hover. The switch is the only thing a click changes (gamemenu_peek_click). */
void gamemenu_peek_point(float x, float y) {
    if (!gamemenu_peeking()) return;
    Hit h;
    int on = hit_find(x, y, &h) && (h.kind == HIT_KV_KEY || h.kind == HIT_KV_MODE);
    int k = on ? h.kind : -1, ha = on ? h.a : 0, hb = on ? h.b : 0;
    if (k == hover_kind && ha == hover_a && hb == hover_b) return;
    hover_kind = k, hover_a = ha, hover_b = hb;
    changed();
}
void gamemenu_peek_click(float x, float y) {
    if (!gamemenu_peeking()) return;
    Hit h;
    if (hit_find(x, y, &h) && h.kind == HIT_KV_MODE)
        __atomic_store_n(&peek_mode, h.a, __ATOMIC_RELEASE), changed();
}

void gamemenu_mouse(float x, float y, int button) {
    if (!gamemenu_open() || settings_open() || bindings_capturing()) return;
    Hit h;
    int found = hit_find(x, y, &h);
    if (gamemenu_prompting() && __atomic_load_n(&pick_n, __ATOMIC_ACQUIRE)) {  /* a list: hovering chooses, a click picks */
        if (dy_hold) return;  /* answered: waiting for the game */
        if (found && h.kind == HIT_PICK_ROW && (button == 0 || button == 1)) {
            if (pick_sel != h.a) pick_sel = h.a, changed();
            if (button == 1) pick_finish();
        } else if (button == 1 && found && h.kind == HIT_PROMPT_OK) {
            pick_finish();
        }
        return;
    }
    if (gamemenu_prompting()) {  /* the name popup: its field takes the typing again, OK finishes */
        if (found && h.kind == HIT_PROMPT_FIELD) {
            char text[TEXTIN_MAX]; int cur;
            textin_read(&prompt_text, text, sizeof text, &cur, NULL);
            int fh = prompt_body[0] ? 52 : 72;
            int offset = ui_text_offset_at(UI_ROUNDED_800, 32, h.x + 20, h.y + (fh - 32 * 1.3f) / 2, h.w - 40 - 100, 32 * 1.3f, 1, text, cur, x, y);
            if (button == 1) textin_focus(&prompt_text), textin_set_caret(offset), changed();
            else if (button == 2) textin_focus(&prompt_text), textin_select_word(offset), changed();
        }
        if (button == 4 && textin_has(&prompt_text) && draw_text_has) {
            char text[TEXTIN_MAX]; int cur;
            textin_read(&prompt_text, text, sizeof text, &cur, NULL);
            int fh = prompt_body[0] ? 52 : 72;
            int offset = ui_text_offset_at(UI_ROUNDED_800, 32, draw_text_box[0] + 20, draw_text_box[1] + (fh - 32 * 1.3f) / 2,
                                          draw_text_box[2] - 40 - 100, 32 * 1.3f, 1, text, cur, x, y);
            textin_drag(offset), changed();
        }
        if (button == 1 && found && h.kind == HIT_PROMPT_OK) prompt_finish();
        if (button == 1 && found && h.kind == HIT_PROMPT_CANCEL) gamemenu_prompt_cancel();
        if (found && h.kind == HIT_OSK_KEY && (button == 0 || button == 1)) {  /* the on-screen keyboard's keys */
            if (osk_row != h.a || osk_col != h.b) osk_row = h.a, osk_col = h.b, changed();
            if (button == 1) osk_press(), changed();
        }
        if (button == 1 && found && h.kind == HIT_OSK_PAGE) osk_step_language(), changed();  /* the switcher's arrows */
        return;
    }
    if (button == 3) {  /* the right button: back, as Escape */
        gamemenu_action(GM_BACK);
        return;
    }
    if (button == 0 || button == 4) {  /* what is under the mouse now: its hover look */
        int k = found ? h.kind : -1, ha = found ? h.a : 0, hb = found ? h.b : 0;
        if (k != hover_kind || ha != hover_a || hb != hover_b) hover_kind = k, hover_a = ha, hover_b = hb, changed();
    }
    if (button == 4 && textin_has(&bug_text) && draw_text_has) {
        char text[TEXTIN_MAX]; int cur;
        textin_read(&bug_text, text, sizeof text, &cur, NULL);
        float tlh = 18 * 1.5f;
        int offset = ui_text_offset_at(UI_GOTHIC_500, 18, draw_text_box[0] + 20, draw_text_box[1] + 16,
                                      draw_text_box[2] - 40, tlh, (int)((draw_text_box[3] - 32) / tlh), text, cur, x, y);
        textin_drag(offset), changed();
        return;
    }
    if (confirm && (!found || h.kind != HIT_POPUP)) return;  /* the popup covers the rest */
    if (button == 1 && textin_has(&bug_text) && (!found || h.kind != HIT_BUG_TEXT)) textin_edit(TI_DONE), changed();
    if (!found) return;
    int click = button == 1, drag = button == 4;  /* 4: moved with the left button held (a slider's thumb) */
    int before = sel * 1000 + focus * 100 + srow + prow * 37 + choice * 7 + in_rows * 500 + brow * 11 + bug_page * 3 + crow * 13 +
                 orow * 17;
    switch (h.kind) {
    case HIT_SIDE:
        /* hovering chooses on the side panel; on a page, only a click goes to another item */
        if (click) sel = h.a, focus = 0, accept();
        else if (!focus) sel = h.a;
        break;
    case HIT_SECTION:
        if (click) srow = h.a, focus = 1, in_rows = 0;
        break;
    case HIT_ROW: case HIT_ROW_PREV: case HIT_ROW_NEXT:
        if (sel == GM_CHEATS) {  /* the Cheats page's rows */
            crow = h.a, focus = 1;
            if (click) settings_cheat_change(h.a, h.kind == HIT_ROW_PREV ? -1 : 1);
            break;
        }
        srow = h.a, focus = 1, in_rows = 1;
        if (click) settings_change(h.a, h.kind == HIT_ROW_PREV ? -1 : 1);
        break;
    case HIT_ROW_SLIDER:  /* a slider row's track (SHARPENING): a click or a drag sets it where the mouse is */
        srow = h.a, focus = 1, in_rows = 1;
        if (click || drag) settings_slide(h.a, (x - h.x - ROW_SLIDER_PAD) / (h.w - 2 * ROW_SLIDER_PAD)), changed();
        break;
    case HIT_BIND:
        focus = 1, bindings_select(h.a, h.b < 0 ? -1 : h.b);
        if (click && bindings_on_mouse_row()) mb_page_open();  /* the Mouse buttons row: its page */
        else if (click && h.b != -2) bindings_action(SETTINGS_ACCEPT);  /* a key: waits for the new one; a value: the next */
        break;
    case HIT_MB_BACK:
        if (click) mb_open = 0, mb_list = 0;
        break;
    case HIT_MB_SET:
        if (click) mb_set = h.a;
        break;
    case HIT_MB_ROW:  /* a part's row or the part on the drawing: hovering chooses it, a click opens the list */
        if (!mb_list && mb_row != h.a) mb_row = h.a, changed();
        if (click && !mb_list) {
            int ids[48], cur;
            mb_entries(ids, 48, &cur);
            mb_list = 1, mb_list_sel = cur;
        }
        break;
    case HIT_MB_STEP:
        if (click) mb_step(h.a, h.b);
        break;
    case HIT_MB_OPT:
        if (mb_list_sel != h.a) mb_list_sel = h.a, changed();
        if (click) {
            int ids[48], cur, n = mb_entries(ids, 48, &cur);
            if (h.a < n) bindings_mouse_set(mb_set, mb_row, ids[h.a]);
            mb_list = 0;
        }
        break;
    case HIT_MB_SCRIM:  /* a click beside the list closes it */
        if (click) mb_list = 0;
        break;
    case HIT_KV_TAB:
        if (click && !bindings_capturing()) focus = 1, ctrl_view = h.a;
        break;
    case HIT_KV_MODE:
        if (click) focus = 1, kv_mode = h.a;
        break;
    case HIT_KV_KEY: case HIT_PV_PART:  /* pointing is enough: the detail bar follows the hover */
        break;
    case HIT_ON_ITEM:  /* the Online page: a: its control (ON_*); b: a switch's side (0 Public / Off, 1 Private / On), -1 */
        if (!online_enabled(h.a)) break;
        focus = 1, orow = h.a;
        if (click) online_use(h.a, h.b < 0 ? 0 : h.b ? 1 : -1);
        break;
    case HIT_MODE:
        focus = 1, prow = 0;
        if (click) window_hud_set_mode(perf_modes[h.a]), settings_save();
        break;
    case HIT_CORNER:
        focus = 1, prow = 1;
        if (click) window_hud_set_corner(h.a), settings_save();
        break;
    case HIT_CORNER_BOX:
        focus = 1, prow = 1;
        if (click) window_hud_set_corner((window_hud_corner() + 1) % 4), settings_save();
        break;
    case HIT_PART:
        focus = 1, prow = 2 + h.a;
        if (click) window_hud_set_parts(window_hud_parts() ^ (1u << h.a)), settings_save();
        break;
    case HIT_SLIDER:  /* a: 1 opacity; the track from x to x + w */
        focus = 1, prow = 6 + h.a;
        if (click || drag) settings_hud_set(h.a, (x - h.x) / h.w), changed();
        break;
    case HIT_POPUP:
        choice = h.a;
        if (click) gamemenu_action(GM_ACCEPT);
        return;
    case HIT_HINT:  /* a key hint along the bottom: clicked, as its key */
        if (!click) return;
        if (h.a == HINT_CLOSE) close_and_resume();
        else gamemenu_action(h.a);
        return;
    case HIT_PREVIEW:  /* the Settings page's Preview button */
        if (click) preview_start(), changed();
        return;
    case HIT_BIND_STEP:  /* a value's arrows on the controls page: b -1 or 1, as Left / Right */
        focus = 1, bindings_select(h.a, -1);
        if (click) bindings_action(h.b < 0 ? SETTINGS_LEFT : SETTINGS_RIGHT);
        break;
    /* the report's pages: hovering only shows where the mouse is; a click picks, ticks, types or presses */
    case HIT_BUG_KIND:
        if (click) focus = 1, brow = 0, bug_kind = h.a;
        break;
    case HIT_BUG_TEXT:
        {
            char text[TEXTIN_MAX]; int cur;
            textin_read(&bug_text, text, sizeof text, &cur, NULL);
            float tlh = 18 * 1.5f;
            int offset = ui_text_offset_at(UI_GOTHIC_500, 18, h.x + 20, h.y + 16, h.w - 40, tlh, (int)((h.h - 32) / tlh), text, cur, x, y);
            if (click) focus = 1, brow = 1, textin_focus(&bug_text), textin_set_caret(offset);
            else if (button == 2) focus = 1, brow = 1, textin_focus(&bug_text), textin_select_word(offset);
        }
        break;
    case HIT_BUG_OFTEN:
        if (click) focus = 1, brow = 2, bug_often = h.a;
        break;
    case HIT_BUG_NEXT:
        if (click) focus = 1, bug_page = 1, brow = 0;
        break;
    case HIT_BUG_TICK:
        if (click) focus = 1, brow = h.a, bug_attach ^= 1u << h.a;
        break;
    case HIT_BUG_BTN:
        if (click) {
            focus = 1, brow = 3, bug_btn = h.a;
            if (h.a == 0) bug_page = 0, brow = 3;
            else bug_finish(h.a == 2);
        }
        break;
    case HIT_BUG_DONE:
        if (click) focus = 1, bug_done_btn = h.a, bug_done_press(h.a);
        break;
    }
    if (click || sel * 1000 + focus * 100 + srow + prow * 37 + choice * 7 + in_rows * 500 + brow * 11 + bug_page * 3 + crow * 13 != before)
        changed();
}

void gamemenu_wheel(int dy) {
    if (gamemenu_prompting() && __atomic_load_n(&pick_n, __ATOMIC_ACQUIRE)) {  /* a list popup: a row a notch */
        pick_move(dy > 0 ? -1 : 1);
        return;
    }
    /* the wheel moves the selection, one row a notch, as Up / Down do (owner, 2026-11-15: like the game's own menus,
     * mousemenu.c; before, 2026-11-06, it only scrolled the long lists). A list longer than its space scrolls to keep
     * the chosen row in view, as with the keys. Not in a popup, while a key is being bound or text is being typed, or
     * on the Controls page's keyboard view (nothing there to choose). */
    if (!gamemenu_open() || settings_open() || bindings_capturing() || confirm || preview || gamemenu_prompting()) return;
    if (sel == GM_BUG && textin_has(&bug_text)) return;
    if (focus && sel == GM_CONTROLS && !ctrl_view) return;
    gamemenu_action(dy > 0 ? GM_UP : GM_DOWN);
}

/* ---- the look (Main.dc.html) */

/* the colours: the chosen theme's (theme.h; the built-in one is the design's), read at each drawing */
static const Theme *TH;
#define INK (TH->ink)
#define NIGHT (TH->night)
#define PLUM (TH->plum)
#define PAPER (TH->paper)
#define GOLD (TH->gold)
#define RING (TH->ring)
#define LINE (TH->line)
#define WHITE (TH->white)
#define SNOW (TH->snow)
#define LILAC (TH->lilac)
#define ON_DARK (TH->on_dark)
#define MUTED (TH->muted)
#define WINE (TH->wine)
#define SHADE (TH->shade)
#define SOFT (TH->soft)
#define EDGE (TH->edge)
#define TAGFILL (TH->tag)
#define VSOFT (TH->vsoft)
#define ON_GOLD (TH->on_gold)
/* the launcher's popup colours (style.css) */
#define POP_DEEP (TH->pop_deep)
#define POP_GOLD (TH->pop_gold)
#define POP_WHITE (TH->pop_white)
#define POP_INK (TH->pop_ink)
#define POP_LILAC (TH->pop_lilac)
#define POP_LINE (TH->pop_line)
#define POP_BORDER (TH->pop_border)
#define POP_SHADOW (TH->pop_shadow)
#define POP_BACKDROP (TH->pop_backdrop)

#define GAME_VERSION "2.2.0"  /* the one build this port runs (CLAUDE.md: the final 4++ update) */

/* the icons' SVG path data, from the design */
static const char *const ICON_RESUME = "<path d=\"M8 5v14l11-7z\"/>";
static const char *const ICON_SETTINGS =
    "<path d=\"M4 7h9m4 0h3M4 17h3m4 0h9M14 7a2 2 0 1 0 4 0a2 2 0 1 0-4 0M7 17a2 2 0 1 0 4 0a2 2 0 1 0-4 0\"/>";
/* a rounder, fuller pad than the design's (owner, 2026-11-05: the design's looked "malnourished") */
static const char *const ICON_CONTROLS =
    "<path d=\"M8 6h8c3.3 0 5.6 2.3 6 5.5l.8 5.5c.3 2.2-2.4 3.5-4 2L16 16H8l-2.8 3c-1.6 1.5-4.3.2-4-2L2 11.5"
    "C2.4 8.3 4.7 6 8 6zM8 9.5v4M6 11.5h4\"/><circle cx=\"15.4\" cy=\"10.2\" r=\"0.7\"/>"
    "<circle cx=\"18.4\" cy=\"13\" r=\"0.7\"/>";
static const char *const ICON_PERF = "<path d=\"M3 20h18M5 16l4-5 4 3 6-8\"/>";
/* a globe: the Online tab */
static const char *const ICON_ONLINE =
    "<circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18\"/>";
/* a bug: its body, a line down its back, legs and feelers */
static const char *const ICON_BUG =
    "<path d=\"M8 10a4 4 0 0 1 8 0v4a4 4 0 0 1-8 0zM12 10v8M4 12h4M16 12h4M5 7.5l3 2M19 7.5l-3 2M5 18.5l3-2M19 18.5"
    "l-3-2M10 6.5L8.5 4M14 6.5L15.5 4\"/>";
/* a five-pointed star: the Cheats page */
static const char *const ICON_CHEATS =
    "<path d=\"M12 3l2.6 5.6 6 .7-4.5 4.1 1.2 6L12 16.4l-5.3 3 1.2-6-4.5-4.1 6-.7z\"/>";
static const char *const ICON_EXIT = "<path d=\"M12 3v9M6.3 6.3a8 8 0 1 0 11.4 0\"/>";
static const char *const ICON_TITLE = "<path d=\"M3 11l9-7 9 7M5 10v10h14V10\"/>";  /* a house: back to the start */
/* the value arrows (‹ ›): drawn as icons, not the font's characters, which sit low in their line */
static const char *const ICON_PREV = "<path d=\"M14.5 6l-6 6 6 6\"/>";
static const char *const ICON_NEXT = "<path d=\"M9.5 6l6 6-6 6\"/>";
/* a pencil: a text row's Edit button (General > Player name) */
static const char *const ICON_EDIT = "<path d=\"M16 3l5 5L8 21H3v-5z\"/><path d=\"M13 6l5 5\"/>";
static int hovered(int kind, int a, int b) { return hover_kind == kind && hover_a == a && hover_b == b; }
static const char *const ICON_EYE =
    "<path d=\"M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>";
/* the pad's button for GM_ALT (Preview, hide the bar): the top face button, Y on Xbox and Switch layouts alike
 * (window.c maps it), Triangle on PlayStation */
static const char *alt_button(const GameMenuInfo *in) { return in->pad == 2 ? "Triangle" : "Y"; }
static const char *const ICON_CLOCK = "<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M12 7v5l3 2\"/>";

/* A side panel button (tabStyle): 56 high, radius 12, a 2 px border; the chosen one gold. */
static void side_button(const UiCanvas *c, float x, float y, float w, const char *icon, const char *label, int on,
                        uint32_t ink) {
    ui_box(c, x, y, w, 56, 12, on ? GOLD : WHITE, 2, on ? GOLD : LINE);
    uint32_t col = on && ink == PLUM ? ON_GOLD : ink;
    ui_icon(c, icon, x + 20, y + 16, 24, 2, col);
    ui_text_in(c, UI_ROUNDED_700, 18, x + 20 + 24 + 14, y, 0, 56, 0, label, col);
}

/* A side panel button that can't be used here: muted, never gold-filled (chosen: a gold outline only), and a short
 * note at its right (none while it may still become usable) */
static void side_button_off(const UiCanvas *c, float x, float y, float w, const char *icon, const char *label, int on,
                            const char *note) {
    ui_box(c, x, y, w, 56, 12, WHITE, 2, on ? GOLD : LINE);
    ui_icon(c, icon, x + 20, y + 16, 24, 2, MUTED);
    ui_text_in(c, UI_ROUNDED_700, 18, x + 20 + 24 + 14, y, 0, 56, 0, label, MUTED);
    if (note) {
        float nw = ui_text_w(UI_GOTHIC_500, 13, note);
        ui_text_in(c, UI_GOTHIC_500, 13, x + w - 20 - nw, y, nw, 56, 0, note, MUTED);
    }
}

/* A key hint: the key on a round chip, then what it does (the bottom bar). Returns its width. */
/* Buttons shown as their symbol in a key hint, not their name: PlayStation's shapes, the D-pad, and the overlay
 * buttons (24 x 24 view boxes, stroked). Back + Start shows both: View's two windows, Menu's three lines. */
static const struct {
    const char *name, *icon[2];
} hint_icons[] = {
    {"Cross", {"<path d=\"M7 7l10 10M17 7L7 17\"/>"}},
    {"Circle", {"<circle cx=\"12\" cy=\"12\" r=\"6.5\"/>"}},
    {"Square", {"<rect x=\"6\" y=\"6\" width=\"12\" height=\"12\" rx=\"1\"/>"}},
    {"Triangle", {"<path d=\"M12 5l7.5 12.5h-15z\"/>"}},
    {"D-pad", {"<path d=\"M9 3h6v6h6v6h-6v6H9v-6H3V9h6z\"/>"}},
    /* the DualSense touchpad's outline, wider at the top (owner's pick A, 2026-11-13) */
    {"Touchpad", {"<path d=\"M3.5 6.5h17l-1.5 9.6a2.4 2.4 0 0 1-2.4 2H7.4a2.4 2.4 0 0 1-2.4-2z\"/>"}},
    {"Share", {"<path d=\"M12 3v11M8 7l4-4 4 4M5 11v9h14v-9\"/>"}},
    {"Capture", {"<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\"/><circle cx=\"12\" cy=\"12\" r=\"4\"/>"}},
    {"Back + Start", {"<rect x=\"4\" y=\"5\" width=\"9\" height=\"9\" rx=\"1\"/><rect x=\"11\" y=\"10\" width=\"9\" "
                      "height=\"9\" rx=\"1\"/>",
                      "<path d=\"M5 7h14M5 12h14M5 17h14\"/>"}},
    /* an Xbox 360 pad's: Back's left triangle, Start's right one */
    {"Back + Start (360)", {"<path d=\"M7 12l9-6v12z\"/>", "<path d=\"M17 12l-9-6v12z\"/>"}},
};

static float hint(const UiCanvas *c, float x, float y, const char *key, const char *label) {
    int sym = -1;
    for (size_t i = 0; i < sizeof hint_icons / sizeof *hint_icons; i++)
        if (!strcmp(hint_icons[i].name, key)) sym = (int)i;
    float kw;
    if (sym >= 0) {  /* the button's symbol on the chip, 18 units, one per button */
        int n = hint_icons[sym].icon[1] ? 2 : 1;
        kw = 30 + (n - 1) * 24;
        ui_box(c, x, y + 13, kw, 30, 15, SNOW, 0, 0);
        for (int k = 0; k < n; k++) ui_icon(c, hint_icons[sym].icon[k], x + 6 + k * 24, y + 19, 18, 2.4f, NIGHT);
    } else {
        kw = ui_text_w(UI_GOTHIC_700, 14, key) + 16;
        if (kw < 30) kw = 30;
        ui_box(c, x, y + 13, kw, 30, 15, SNOW, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 14, x, y + 13, kw, 30, 1, key, NIGHT);
    }
    float lw = ui_text_w(UI_GOTHIC_400, 16, label);
    ui_text_in(c, UI_GOTHIC_400, 16, x + kw + 10, y, 0, 56, 0, label, SNOW);
    /* the mouse: a hint of an action is a button too (Move and Choose aren't: they need a direction) */
    static const struct {
        const char *label;
        int action;
    } acts[] = {{"Back", GM_BACK}, {"Close overlay", HINT_CLOSE}, {"Select", GM_ACCEPT}, {"Open", GM_ACCEPT},
                {"Change", GM_ACCEPT}, {"Clear", GM_CLEAR}, {"Section", GM_NEXT}, {"Preview", GM_ALT},
                {"Hide bar", GM_ALT}, {"Show bar", GM_ALT}, {"Back to settings", GM_BACK}};
    for (size_t i = 0; i < sizeof acts / sizeof *acts; i++)
        if (!strcmp(label, acts[i].label)) hit_add(x - 6, y + 6, kw + 10 + lw + 12, 44, HIT_HINT, acts[i].action, 0);
    return kw + 10 + lw;
}

/* A popup button (Keep playing / Quit game): 56 high, radius 14; the chosen one ringed in gold. */
/* A popup button, as the launcher's (style.css dialog): 40 high, pill-shaped, 20 at the sides; the chosen one
 * ringed in gold. */
/* The exit popup's size against the launcher's popup it copies: bigger, since it is read from the couch
 * (owner, 2026-11-11: "the current one is small"). */
#define POP_K 1.4f

static float popup_button(const UiCanvas *c, float right, float y, const char *label, int quit, int on) {
    int f = quit ? UI_ROUNDED_800 : UI_ROUNDED_700;
    float fs = 14 * POP_K, bh = 40 * POP_K, ring = 4 * POP_K;
    float w = ui_text_w(f, fs, label) + 40 * POP_K;
    float x = right - w;
    hit_add(x, y, w, bh, HIT_POPUP, quit, 0);
    if (on) ui_box(c, x - ring, y - ring, w + 2 * ring, bh + 2 * ring, bh / 2 + ring, RING, 0, 0);
    if (quit) ui_box(c, x, y, w, bh, bh / 2, WINE, 0, 0);
    else ui_box(c, x, y, w, bh, bh / 2, WHITE, 1, POP_LINE);
    ui_text_in(c, f, fs, x, y, w, bh, 1, label, quit ? SNOW : PLUM);
    return w;
}

/* A greyed-out row's reason that keeps the setting on (Reflex with DLSS frame generation), not off. */
static int locked_on(const char *why) { return !strncmp(why, "ALWAYS ON", 9); }

/* The settings' texts are upper case (the F1 panel's pixel font has no lower case); the design writes names,
 * options and sections in sentence case and keeps acronyms: "WINDOW MODE" -> "Window mode", "X16" -> "x16". */
static void sentence_case(const char *in, char *out, size_t n) {
    static const char *const keep[] = {"HUD", "FXAA", "SMAA", "TAA", "FSR", "DLSS", "GPU", "GPUS", "CPU", "AMD",
                                       "NVIDIA", "RTX", "HD", "MS", "F3", "F11", "AI", "ZL", "ZR", "LB", "RB", "LT",
                                       "RT", "L1", "R1", "L2", "R2", "L3", "R3", "PS", "NINTENDO"};
    /* the F1 panel font's own symbols (bindings.c): arrows, and PlayStation's shapes by name */
    static const char *const sym[9] = {NULL, "\xe2\x86\x91", "\xe2\x86\x93", "\xe2\x86\x90", "\xe2\x86\x92",
                                       "Triangle", "Circle", "Square", "Cross"};
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n;) {
        if (*p >= 1 && *p <= 8) {
            for (const char *q = sym[(int)*p]; *q && o + 1 < n; q++) out[o++] = *q;
            p++;
            continue;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {  /* a word */
            const char *e = p;
            while ((*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9')) e++;
            size_t len = (size_t)(e - p);
            int acr = 0;
            for (size_t k = 0; k < sizeof keep / sizeof *keep; k++)
                if (strlen(keep[k]) == len && !strncmp(keep[k], p, len)) acr = 1;
            int times = *p == 'X' && p[1] >= '0' && p[1] <= '9';  /* "X16": a multiplier, lower case */
            for (const char *q = p; q < e && o + 1 < n; q++)
                out[o++] = acr || (q == in && !times) ? *q : (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
            p = e;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

/* The settings' notes and descriptions as sentences: lower case, a capital at each sentence's start, names and
 * acronyms kept ("THE SWITCH'S DEFAULT: JAGGED EDGES" -> "The Switch's default: jagged edges", "ABOUT 2 MS" -> "about
 * 2 ms", "ENTER OR A: CHANGE" -> "Enter or A: change"). */
static void prose_case(const char *in, char *out, size_t n) {
    static const struct {
        const char *word, *as;
    } names[] = {
        {"SWITCH", "Switch"}, {"SWITCH'S", "Switch's"}, {"ALT", "Alt"}, {"TAB", "Tab"}, {"ENTER", "Enter"},
        {"DELETE", "Delete"}, {"ESCAPE", "Escape"}, {"BACKSPACE", "Backspace"}, {"SHIFT", "Shift"}, {"MS", "ms"},
        {"HZ", "Hz"}, {"NVIDIA", "NVIDIA"}, {"AMD", "AMD"}, {"GPU", "GPU"}, {"GPUS", "GPUs"}, {"CPU", "CPU"},
        {"HUD", "HUD"}, {"FXAA", "FXAA"}, {"SMAA", "SMAA"}, {"TAA", "TAA"}, {"FSR", "FSR"}, {"DLSS", "DLSS"},
        {"RTX", "RTX"}, {"AI", "AI"}, {"HD", "HD"}, {"PCSS", "PCSS"}, {"VSYNC", "VSync"}, {"F1", "F1"}, {"F2", "F2"},
        {"F3", "F3"}, {"F11", "F11"}, {"ZL", "ZL"}, {"ZR", "ZR"}, {"LB", "LB"}, {"RB", "RB"}, {"OK", "OK"},
        /* names in Discord status's text: "Yo-kai Watch 4" ("KAI" stays lower case), "Watch rank", "this PC" */
        {"DISCORD", "Discord"}, {"PC", "PC"}, {"YO", "Yo"}, {"WATCH", "Watch"},
        {"BLASTERS", "Blasters"},  /* the game mode, in Player name's text */
    };
    size_t o = 0;
    int start = 1;     /* at a sentence's start */
    int after_or = 0;  /* the word before was OR / AND: a single letter is then a button ("ENTER OR A") */
    int after_the = 0; /* the word before was THE: "THE SWITCH" is the console, "LEFT/RIGHT: SWITCH" the verb */
    for (const char *p = in; *p && o + 1 < n;) {
        int alnum = (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (!alnum) {
            if (*p == '.' || *p == '?' || *p == '!') start = 1;
            out[o++] = *p++;
            continue;
        }
        const char *e = p;
        while ((*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9') || (*e == '\'' && e[1] >= 'A' && e[1] <= 'Z')) e++;
        size_t len = (size_t)(e - p);
        const char *as = NULL;
        for (size_t k = 0; k < sizeof names / sizeof *names; k++)
            if (strlen(names[k].word) == len && !strncmp(names[k].word, p, len)) as = names[k].as;
        int digit = *p >= '0' && *p <= '9';
        if (as && len == 6 && !strncmp(p, "SWITCH", 6) && !after_the) as = NULL;  /* the verb */
        if (as) {
            for (const char *q = as; *q && o + 1 < n; q++) out[o++] = *q;
        } else if (len == 1 && (after_or || *e == ':' || *e == '/') && *p != 'I') {
            out[o++] = *p;  /* a button: A, B, X, Y */
        } else {
            for (size_t k = 0; k < len && o + 1 < n; k++) {
                char ch = p[k];
                int cap = start && k == 0 && !digit;
                out[o++] = cap || ch < 'A' || ch > 'Z' ? ch : (char)(ch + 32);  /* "1.5X" -> "1.5x" too */
            }
        }
        after_or = (len == 2 && !strncmp(p, "OR", 2)) || (len == 3 && !strncmp(p, "AND", 3));
        after_the = len == 3 && !strncmp(p, "THE", 3);
        if (!digit || as) start = 0;
        p = e;
    }
    out[o] = 0;
}

/* A note's highlights: "THE SWITCH'S DEFAULT" and "(RECOMMENDED)" become tags; *rest is what remains of it. */
enum { TAG_DEFAULT = 1, TAG_RECOMMENDED = 2 };
static int note_tags(const char *note, char *rest, size_t n) {
    int tags = 0;
    const char *def = "THE SWITCH'S DEFAULT";
    if (!strncmp(note, def, strlen(def))) {
        tags |= TAG_DEFAULT;
        note += strlen(def);
        while (*note == ':' || *note == ' ') note++;
    }
    snprintf(rest, n, "%s", note);
    char *r = strstr(rest, "(RECOMMENDED)");
    if (r) {
        tags |= TAG_RECOMMENDED;
        memmove(r, r + 13, strlen(r + 13) + 1);
        size_t l = strlen(rest);
        while (l && rest[l - 1] == ' ') rest[--l] = 0;
    }
    return tags;
}

/* a small tag (a note's highlight), centred at cx; returns its width */
static float tag(const UiCanvas *c, float x, float y, const char *label, uint32_t fill, uint32_t ink, int measure) {
    float w = ui_text_w(UI_GOTHIC_700, 12, label) + 16;
    if (!measure) {
        ui_box(c, x, y, w, 18, 9, fill, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 12, x, y, w, 18, 1, label, ink);
    }
    return w;
}

/* A slider row's value (SHARPENING, issue #11; the owner's pick of the mockup, style B, 2026-11-16, then "only the
 * level", no number) in the box x..x+w of a row y..y+h: the track with its named points marked (lit up to the thumb),
 * the thumb, and the value: the point's name ("Medium"), or the slider's own text with no names. The thumb stops at
 * the points; Left / Right step it as any row; the mouse clicks or drags on the track. */
static void row_slider(const UiCanvas *c, const SettingRow *r, int row, float x, float y, float w, float h, float fs,
                       int off) {
    char t[40];
    float vw = ui_text_w(UI_ROUNDED_700, fs, "100%"), gap = 14;
    for (int k = 0; k < r->nmarks; k++) {  /* room for the widest name, so the track doesn't jump */
        sentence_case(r->marks[k], t, sizeof t);
        float tw = ui_text_w(UI_ROUNDED_700, fs, t);
        if (tw > vw) vw = tw;
    }
    int named = r->nmarks >= 2;
    float tx = x + ROW_SLIDER_PAD, tw = w - ROW_SLIDER_PAD - gap - vw - 10, cy = y + h / 2;
    if (tw < 40) tw = 40;
    uint32_t ink = off ? MUTED : PLUM;
    float f = r->frac < 0 ? 0 : r->frac > 1 ? 1 : r->frac;
    ui_box(c, tx, cy - 3, tw, 6, 3, LINE, 0, 0);
    if (f > 0) ui_box(c, tx, cy - 3, tw * f, 6, 3, ink, 0, 0);
    for (int k = 0; named && k < r->nmarks; k++) {
        float at = (float)k / (float)(r->nmarks - 1);
        ui_box(c, tx + tw * at - 2, cy - 7, 4, 14, 2, f >= at - 0.001f ? ink : EDGE, 0, 0);
    }
    int hv = !off && hovered(HIT_ROW_SLIDER, row, 0);
    ui_box(c, tx + tw * f - 11, cy - 11, 22, 22, 11, hv ? NIGHT : ink, 2, WHITE);
    if (!off) hit_add(tx - ROW_SLIDER_PAD, y, tw + 2 * ROW_SLIDER_PAD, h, HIT_ROW_SLIDER, row, 0);
    sentence_case(r->value, t, sizeof t);  /* the level's name, as the settings give it ("MEDIUM") */
    ui_text_in(c, UI_ROUNDED_700, fs, tx + tw + gap, y, vw, h, 1, t, off ? MUTED : NIGHT);
}

/* The settings page (the design's isSettings section): the sections on the left (the chosen row's lit), its
 * section's rows with the value, its note and arrows, the chosen row's description at the bottom. */
static void draw_settings_page(const UiCanvas *c, const GameMenuInfo *in) {
    SettingRow rows[48];
    int n = settings_player_rows(rows, 48);
    if (n <= 0) return;
    int cur = srow < n ? srow : n - 1;
    const char *sec = rows[cur].category;
    char t[160];
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Settings", NIGHT);
    {  /* Preview, at the right of the title (canvas board "Settings page: the Preview button"): an eye, its name, its
        * key; greyed where there is nothing to see */
        int pf, pl, can = look_span(rows, n, cur, &pf, &pl);
        const char *key = in->pad ? alt_button(in) : "P";
        int tri = !strcmp(key, "Triangle");  /* drawn as its shape */
        float kw = tri ? 30 : ui_text_w(UI_GOTHIC_700, 13, key) + 18, tw = ui_text_w(UI_ROUNDED_700, 17, "Preview");
        float bw = 14 + 22 + 10 + tw + 10 + kw + 14, bx = 448 + 1104 - 40 - bw, by = 76 + (34 * 1.25f - 48) / 2;
        int hv = can && hovered(HIT_PREVIEW, 0, 0);
        uint32_t ink = can ? PLUM : MUTED;
        ui_box(c, bx, by, bw, 48, 24, hv ? LILAC : WHITE, 2, can ? PLUM : LINE);
        ui_icon(c, ICON_EYE, bx + 14, by + 13, 22, 2, ink);
        ui_text_in(c, UI_ROUNDED_700, 17, bx + 14 + 22 + 10, by, 0, 48, 0, "Preview", ink);
        float kx = bx + 14 + 22 + 10 + tw + 10;
        ui_box(c, kx, by + 11, kw, 26, 13, SOFT, 0, 0);
        if (tri) ui_icon(c, hint_icons[3].icon[0], kx + 7, by + 13, 16, 2.4f, can ? NIGHT : MUTED);
        else ui_text_in(c, UI_GOTHIC_700, 13, kx, by + 11, kw, 26, 1, key, can ? NIGHT : MUTED);
        if (can) hit_add(bx, by, bw, 48, HIT_PREVIEW, 0, 0);
    }
    float top = 76 + 34 * 1.25f + 22, bottom = 40 + 764 - 36;
    /* the sections */
    float y = top;
    for (int i = 0; i < n; i++) {
        if (i && !strcmp(rows[i].category, rows[i - 1].category)) continue;
        int on = !strcmp(rows[i].category, sec);
        if (on && focus && !in_rows) ui_box(c, 488 - 4, y - 4, 300 + 8, 52 + 8, 15, RING, 0, 0);  /* the focus */
        int hv = !on && hovered(HIT_SECTION, i, 0);  /* under the mouse: lighter lilac, a darker edge */
        ui_box(c, 488, y, 300, 52, 12, on ? PLUM : hv ? SOFT : WHITE, 2, on ? PLUM : hv ? EDGE : LINE);
        hit_add(488, y, 300, 52, HIT_SECTION, i, 0);
        sentence_case(rows[i].category, t, sizeof t);
        ui_text_wrap(c, UI_GOTHIC_700, 17, 0, 488 + 16, y + 26 - 17 * 1.45f / 2, 300 - 32, 17 * 1.45f, 0, t,
                     on ? SNOW : PLUM);
        y += 52 + 8;
    }
    /* the chosen row's description (drawn last): its height first, so the rows stop above it */
    float x = 812, w = 700;
    const SettingRow *dr = &rows[cur];
    char desc[600], dprose[600], dname[160];
    if (dr->disabled) snprintf(desc, sizeof desc, "%s %s%s", dr->help, locked_on(dr->disabled) ? "" : "OFF FOR NOW: ",
                               dr->disabled);
    else snprintf(desc, sizeof desc, "%s", dr->help);
    prose_case(desc, dprose, sizeof dprose);
    sentence_case(dr->name, dname, sizeof dname);
    float lh = 15 * 1.5f, dh = lh + 4 + ui_text_wrap(NULL, UI_GOTHIC_500, 15, 0, 0, 0, w - 40, lh, 0, dprose, 0) + 32;
    /* its rows: as many as fit above the description; a longer section (Advanced, with the developer options)
     * scrolls to keep the chosen row in view, and the mouse wheel scrolls it (gamemenu_wheel) */
    int idx[48], cnt = 0, pos = 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(rows[i].category, sec)) {
            if (i == cur) pos = cnt;
            idx[cnt++] = i;
        }
    int visible = (int)((bottom - dh - 12 - top + 10) / (72 + 10));
    if (visible < 1) visible = 1;
    static const char *shown_sec;
    if (shown_sec != sec) set_first = 0, shown_sec = sec;  /* another section: from its top */
    if (in_rows && pos < set_first) set_first = pos;
    if (in_rows && pos >= set_first + visible) set_first = pos - visible + 1;
    if (set_first > cnt - visible) set_first = cnt - visible;
    if (set_first < 0) set_first = 0;
    y = top;
    for (int k = set_first; k < cnt && k < set_first + visible; k++) {
        int i = idx[k];
        const SettingRow *r = &rows[i];
        int on = focus && in_rows && i == cur, off = r->disabled != NULL;
        ui_box(c, x, y, w, 72, 14, WHITE, on ? 3 : 2, on ? RING : LINE);
        hit_add(x, y, w, 72, HIT_ROW, i, 0);
        sentence_case(r->name, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_500, 18, x + 20, y, 0, 72, 0, t, off ? MUTED : INK);
        float nx = x + w - 12 - 44, vx = nx - 10 - 300, px = vx - 10 - 44, by = y + 14;
        if (r->slider) {  /* SHARPENING: a slider where the arrows and the value are on the other rows */
            row_slider(c, r, i, px, y, nx + 44 - px, 72, 18, off);
            y += 72 + 10;
            continue;
        }
        if (r->textbox) {  /* Player name (owner's pick, 2026-11-19): the text in a box, an Edit button for the popup */
            const char *value = r->options[0] ? r->options[0] : "";
            float ew = 22 + 8 + ui_text_w(UI_ROUNDED_800, 16, "Edit") + 2 * 16, ex = nx + 44 - ew;
            float fx = px, fw = ex - 10 - px, fh = 44, fy = y + (72 - fh) / 2;
            ui_box(c, fx, fy, fw, fh, 10, WHITE, 2, LINE);
            ui_text_in(c, UI_ROUNDED_700, 18, fx + 16, fy, fw - 32, fh, 0, value, off ? MUTED : NIGHT);
            int hv = !off && hovered(HIT_ROW_NEXT, i, 0);
            ui_box(c, ex, fy, ew, fh, 10, hv ? LILAC : SOFT, 2, hv ? PLUM : LINE);
            ui_icon(c, ICON_EDIT, ex + 16, fy + (fh - 22) / 2, 22, 2.2f, off ? MUTED : PLUM);
            ui_text_in(c, UI_ROUNDED_800, 16, ex + 16 + 22 + 8, fy, 0, fh, 0, "Edit", off ? MUTED : PLUM);
            hit_add(fx, fy, fw, fh, HIT_ROW_NEXT, i, 0), hit_add(ex, fy, ew, fh, HIT_ROW_NEXT, i, 0);
            y += 72 + 10;
            continue;
        }
        hit_add(px, by, 44, 44, HIT_ROW_PREV, i, 0), hit_add(nx, by, 44, 44, HIT_ROW_NEXT, i, 0);
        for (int k = 0; k < 2; k++) {  /* ‹ and ›; darker under the mouse */
            float bx = k ? nx : px;
            int hv = !off && hovered(k ? HIT_ROW_NEXT : HIT_ROW_PREV, i, 0);
            ui_box(c, bx, by, 44, 44, 10, hv ? LILAC : SOFT, 2, hv ? PLUM : LINE);
            ui_icon(c, k ? ICON_NEXT : ICON_PREV, bx + 11, by + 11, 22, 2.4f, off ? MUTED : PLUM);
        }
        const char *value = r->options[r->current] ? r->options[r->current] : "";
        const char *note = r->notes[r->current] ? r->notes[r->current] : "";
        if (r->verbatim) snprintf(t, sizeof t, "%s", value);  /* a theme's name, as written */
        else sentence_case(value, t, sizeof t);
        /* under the value: its tags (Switch default, Recommended), then the rest of its note as a sentence */
        char rest[200], prose[200];
        int tags = note_tags(note, rest, sizeof rest);
        prose_case(rest, prose, sizeof prose);
        float tw = 0;
        if (tags & TAG_DEFAULT) tw += tag(c, 0, 0, "Switch default", 0, 0, 1);
        if (tags & TAG_RECOMMENDED) tw += (tw ? 6 : 0) + tag(c, 0, 0, "Recommended", 0, 0, 1);
        float th = tags ? 18 : 0, gap = tags && *prose ? 3 : 0;
        float vh = 18 * 1.4f, nh = *prose ? ui_text_wrap(NULL, UI_GOTHIC_500, 13, 0, 0, 0, 280, 13 * 1.3f, 1, prose, 0) : 0;
        float bh2 = vh + (th || nh ? 2 + th + gap + nh : 0);
        float vy = y + (72 - bh2) / 2;
        ui_text_in(c, UI_ROUNDED_700, 18, vx, vy, 300, vh, 1, t, off ? MUTED : NIGHT);
        float ny = vy + vh + 2;
        if (tags) {
            float tx = vx + (300 - tw) / 2;
            if (tags & TAG_DEFAULT) tx += tag(c, tx, ny, "Switch default", TAGFILL, PLUM, 0) + 6;
            if (tags & TAG_RECOMMENDED) tag(c, tx, ny, "Recommended", GOLD, ON_GOLD, 0);
            ny += th + gap;
        }
        if (nh) ui_text_wrap(c, UI_GOTHIC_500, 13, 0, vx + 10, ny, 280, 13 * 1.3f, 1, prose, MUTED);  /* clear of the arrows */
        y += 72 + 10;
    }
    /* more rows above or below the ones shown: a short line in the gap under the rows */
    if (cnt > visible) {
        snprintf(t, sizeof t, "%d-%d of %d. %s", set_first + 1, set_first + visible < cnt ? set_first + visible : cnt, cnt,
                 in->pad ? "Up and down to see the rest" : "Scroll or use W and S to see the rest");
        ui_text_in(c, UI_GOTHIC_500, 13, x, y - 6, w, 18, 1, t, MUTED);
    }
    /* the setting's name in bold white, then what it does as sentences (no gold text: the owner's rule) */
    ui_box(c, x, bottom - dh, w, dh, 14, NIGHT, 0, 0);
    ui_text(c, UI_ROUNDED_800, 15, x + 20, bottom - dh + 16, lh, dname, SNOW);
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x + 20, bottom - dh + 16 + lh + 4, w - 40, lh, 0, dprose, ON_DARK);
}

/* The Cheats page (canvas https://claude.ai/artifact/XcwzdE1gju4aNQJgxgE2e5 option B): the title and a line under
 * it, the cheats as the Settings page's rows (full width, no sections), the chosen one's description at the bottom. */
static void draw_cheats_page(const UiCanvas *c, const GameMenuInfo *in) {
    (void)in;
    SettingRow rows[8];
    int n = settings_cheat_rows(rows, 8);
    char t[160];
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Cheats", NIGHT);
    float x = 488, w = 1104 - 80, lead = 15 * 1.5f, top = 76 + 34 * 1.25f + 6;
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x, top, w, lead, 0,
                 "All off by default. They change the game while it runs; turn one off and the game goes back to its own "
                 "rules.", MUTED);
    if (n <= 0) return;
    int cur = crow < n ? crow : n - 1;
    float y = top + lead + 22, bottom = 40 + 764 - 36;
    for (int i = 0; i < n; i++) {
        const SettingRow *r = &rows[i];
        int on = focus && i == cur;
        ui_box(c, x, y, w, 72, 14, WHITE, on ? 3 : 2, on ? RING : LINE);
        hit_add(x, y, w, 72, HIT_ROW, i, 0);
        sentence_case(r->name, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_500, 18, x + 20, y, 0, 72, 0, t, INK);
        float nx = x + w - 12 - 44, vx = nx - 10 - 340, px = vx - 10 - 44, by = y + 14;
        hit_add(px, by, 44, 44, HIT_ROW_PREV, i, 0), hit_add(nx, by, 44, 44, HIT_ROW_NEXT, i, 0);
        for (int k = 0; k < 2; k++) {  /* ‹ and ›; darker under the mouse */
            float bx = k ? nx : px;
            int hv = hovered(k ? HIT_ROW_NEXT : HIT_ROW_PREV, i, 0);
            ui_box(c, bx, by, 44, 44, 10, hv ? LILAC : SOFT, 2, hv ? PLUM : LINE);
            ui_icon(c, k ? ICON_NEXT : ICON_PREV, bx + 11, by + 11, 22, 2.4f, PLUM);
        }
        char prose[200];
        sentence_case(r->options[r->current] ? r->options[r->current] : "", t, sizeof t);
        prose_case(r->notes[r->current] ? r->notes[r->current] : "", prose, sizeof prose);
        float vh = 18 * 1.4f, nh = *prose ? 13 * 1.3f : 0, vy = y + (72 - vh - (nh ? 2 + nh : 0)) / 2;
        ui_text_in(c, UI_ROUNDED_700, 18, vx, vy, 340, vh, 1, t, NIGHT);
        if (nh) ui_text_in(c, UI_GOTHIC_500, 13, vx, vy + vh + 2, 340, nh, 1, prose, MUTED);
        y += 72 + 10;
    }
    /* the chosen cheat's name in bold white, then what it does */
    const SettingRow *dr = &rows[cur];
    char dprose[600], dname[160];
    prose_case(dr->help, dprose, sizeof dprose);
    sentence_case(dr->name, dname, sizeof dname);
    float dh = lead + 4 + ui_text_wrap(NULL, UI_GOTHIC_500, 15, 0, 0, 0, w - 40, lead, 0, dprose, 0) + 32;
    ui_box(c, x, bottom - dh, w, dh, 14, NIGHT, 0, 0);
    ui_text(c, UI_ROUNDED_800, 15, x + 20, bottom - dh + 16, lead, dname, SNOW);
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x + 20, bottom - dh + 16 + lead + 4, w - 40, lead, 0, dprose, ON_DARK);
}

/* A binding's name (bindings.c pad_label, key names) with its symbols drawn as icons, as the old F2 page's pixel
 * font drew them: \1..\4 the arrows (up, down, left, right), \5..\8 PlayStation's triangle, circle, square, cross.
 * The rest in sentence case. Drawn from x, centred in [y, y + h]; returns its width. c NULL: only measures. */
static const char *const bind_sym[9] = {
    NULL,
    "<path d=\"M12 19V5M6 11l6-6 6 6\"/>",   /* up */
    "<path d=\"M12 5v14M6 13l6 6 6-6\"/>",   /* down */
    "<path d=\"M19 12H5M11 6l-6 6 6 6\"/>",  /* left */
    "<path d=\"M5 12h14M13 6l6 6-6 6\"/>",   /* right */
    "<path d=\"M12 5l7.5 12.5h-15z\"/>",     /* triangle */
    "<circle cx=\"12\" cy=\"12\" r=\"6.5\"/>",  /* circle */
    "<rect x=\"6\" y=\"6\" width=\"12\" height=\"12\" rx=\"1\"/>",  /* square */
    "<path d=\"M7 7l10 10M17 7L7 17\"/>",    /* cross */
};
static float cell_text(const UiCanvas *c, float x, float y, float h, const char *cell, uint32_t rgb) {
    float w = 0;
    for (const char *p = cell; *p;) {
        if (*p >= 1 && *p <= 8) {  /* a symbol: 20 units, a little space after when text follows */
            if (c) ui_icon(c, bind_sym[(int)*p], x + w, y + (h - 20) / 2, 20, 2.4f, rgb);
            w += 20;
            p++;
            if (*p) w += 4;
            continue;
        }
        char run[96], t[160];
        size_t n = 0;
        while (*p && !(*p >= 1 && *p <= 8) && n + 1 < sizeof run) run[n++] = *p++;
        run[n] = 0;
        sentence_case(run, t, sizeof t);
        if (c) ui_text_in(c, UI_GOTHIC_500, 17, x + w, y, 0, h, 0, t, rgb);
        w += ui_text_w(UI_GOTHIC_500, 17, t);
    }
    return w;
}

/* ---- the Controls page's Overview: the keyboard and the mouse with the bound keys lit and what each does, for
 * exploring, battles or cutscenes (the owner's mockup, 2026-11-11). What a Switch input does is the owner's names
 * for his binds; an input without a name isn't lit. */

/* the keyboard: rows in key units (a key = 1), the main block 15 wide, the arrows 0.5 to its right as an inverted T.
 * code 0: a gap of that width */
typedef struct {
    int code;
    float w;
    const char *cap;
} KvKey;
static const KvKey kv_rows[6][17] = {
    {{SDL_SCANCODE_ESCAPE, 1, "Esc"}, {0, 1, 0}, {SDL_SCANCODE_F1, 1, "F1"}, {SDL_SCANCODE_F2, 1, "F2"},
     {SDL_SCANCODE_F3, 1, "F3"}, {SDL_SCANCODE_F4, 1, "F4"}, {0, .5f, 0}, {SDL_SCANCODE_F5, 1, "F5"},
     {SDL_SCANCODE_F6, 1, "F6"}, {SDL_SCANCODE_F7, 1, "F7"}, {SDL_SCANCODE_F8, 1, "F8"}, {0, .5f, 0},
     {SDL_SCANCODE_F9, 1, "F9"}, {SDL_SCANCODE_F10, 1, "F10"}, {SDL_SCANCODE_F11, 1, "F11"}, {SDL_SCANCODE_F12, 1, "F12"}},
    {{SDL_SCANCODE_GRAVE, 1, "`"}, {SDL_SCANCODE_1, 1, "1"}, {SDL_SCANCODE_2, 1, "2"}, {SDL_SCANCODE_3, 1, "3"},
     {SDL_SCANCODE_4, 1, "4"}, {SDL_SCANCODE_5, 1, "5"}, {SDL_SCANCODE_6, 1, "6"}, {SDL_SCANCODE_7, 1, "7"},
     {SDL_SCANCODE_8, 1, "8"}, {SDL_SCANCODE_9, 1, "9"}, {SDL_SCANCODE_0, 1, "0"}, {SDL_SCANCODE_MINUS, 1, "-"},
     {SDL_SCANCODE_EQUALS, 1, "="}, {SDL_SCANCODE_BACKSPACE, 2, "Backspace"}},
    {{SDL_SCANCODE_TAB, 1.5f, "Tab"}, {SDL_SCANCODE_Q, 1, "Q"}, {SDL_SCANCODE_W, 1, "W"}, {SDL_SCANCODE_E, 1, "E"},
     {SDL_SCANCODE_R, 1, "R"}, {SDL_SCANCODE_T, 1, "T"}, {SDL_SCANCODE_Y, 1, "Y"}, {SDL_SCANCODE_U, 1, "U"},
     {SDL_SCANCODE_I, 1, "I"}, {SDL_SCANCODE_O, 1, "O"}, {SDL_SCANCODE_P, 1, "P"}, {SDL_SCANCODE_LEFTBRACKET, 1, "["},
     {SDL_SCANCODE_RIGHTBRACKET, 1, "]"}, {SDL_SCANCODE_BACKSLASH, 1.5f, "\\"}},
    {{SDL_SCANCODE_CAPSLOCK, 1.75f, "Caps"}, {SDL_SCANCODE_A, 1, "A"}, {SDL_SCANCODE_S, 1, "S"}, {SDL_SCANCODE_D, 1, "D"},
     {SDL_SCANCODE_F, 1, "F"}, {SDL_SCANCODE_G, 1, "G"}, {SDL_SCANCODE_H, 1, "H"}, {SDL_SCANCODE_J, 1, "J"},
     {SDL_SCANCODE_K, 1, "K"}, {SDL_SCANCODE_L, 1, "L"}, {SDL_SCANCODE_SEMICOLON, 1, ";"},
     {SDL_SCANCODE_APOSTROPHE, 1, "'"}, {SDL_SCANCODE_RETURN, 2.25f, "Enter"}},
    {{SDL_SCANCODE_LSHIFT, 2.25f, "Shift"}, {SDL_SCANCODE_Z, 1, "Z"}, {SDL_SCANCODE_X, 1, "X"}, {SDL_SCANCODE_C, 1, "C"},
     {SDL_SCANCODE_V, 1, "V"}, {SDL_SCANCODE_B, 1, "B"}, {SDL_SCANCODE_N, 1, "N"}, {SDL_SCANCODE_M, 1, "M"},
     {SDL_SCANCODE_COMMA, 1, ","}, {SDL_SCANCODE_PERIOD, 1, "."}, {SDL_SCANCODE_SLASH, 1, "/"},
     /* Up over Down, as on a keyboard: 15 units of keys + 0.5 = 15.5, Down's place in the row below (owner, 2026-11-13) */
     {SDL_SCANCODE_RSHIFT, 2.75f, "Shift"}, {0, .5f, 0}, {SDL_SCANCODE_UP, 1, "\xe2\x86\x91"}},
    {{SDL_SCANCODE_LCTRL, 1.25f, "Ctrl"}, {SDL_SCANCODE_LGUI, 1.25f, "Win"}, {SDL_SCANCODE_LALT, 1.25f, "Alt"},
     {SDL_SCANCODE_SPACE, 6.25f, "Space"}, {SDL_SCANCODE_RALT, 1.25f, "Alt"}, {SDL_SCANCODE_APPLICATION, 1.25f, "Menu"},
     {SDL_SCANCODE_RCTRL, 1.5f, "Ctrl"}, {0, .5f, 0}, {SDL_SCANCODE_LEFT, 1, "\xe2\x86\x90"},
     {SDL_SCANCODE_DOWN, 1, "\xe2\x86\x93"}, {SDL_SCANCODE_RIGHT, 1, "\xe2\x86\x92"}}};
enum { KV_MOUSE_LEFT = 1000, KV_MOUSE_RIGHT, KV_MOUSE_MID, KV_WHEEL };  /* hit numbers of the mouse's parts */

/* the port's own keys (gold): the same in every mode */
static const char *kv_port(int code) {
    switch (code) {
    case SDL_SCANCODE_ESCAPE: return "Overlay";
    case SDL_SCANCODE_F1: return "Settings";
    case SDL_SCANCODE_F2: return "Controls";
    case SDL_SCANCODE_F3: return "HUD";
    case SDL_SCANCODE_F11: return "Full screen";
    case SDL_SCANCODE_F12: return "Screenshot";
    default: return NULL;
    }
}

/* What a Switch input (its settings-file name) does in a mode; NULL: nothing known (not lit). The owner's names. */
static const char *kv_action(int mode, const char *id) {
    static const char *const field[][2] = {
        {"A", "Interact / confirm"}, {"B", "Jump / back, hold to run"}, {"X", "Yo-kai Pad"}, {"Y", "Map"},
        {"L", "Hold: more shortcuts"}, {"R", "Minimap size"}, {"ZL", "Watch scan"}, {"ZR", "Yo-kai Beam"},
        {"PLUS", "Pause"}, {"LS_PRESS", "Close hints"}, {"RS_PRESS", "Scan"},
        {"DPAD_UP", "Shortcut"}, {"DPAD_RIGHT", "Shortcut"}, {"DPAD_DOWN", "Shortcut"}, {"DPAD_LEFT", "Shortcut"},
        {"RUN", "Run (by Running mode)"}};
    static const char *const battle[][2] = {
        {"A", "Confirm"}, {"B", "Roll / dodge"}, {"X", "Character skill"}, {"Y", "Attack"},
        {"R", "Skill wheel / Encourage / Soul Steal"}, {"ZL", "Aim"}, {"ZR", "Yo-kai Drain"}, {"PLUS", "Stop watching"},
        {"MINUS", "Auto battle"}, {"LS_PRESS", "Cycle allies"}, {"RS_PRESS", "Lock on / off"},
        {"DPAD_UP", "Switch character"},
        {"DPAD_DOWN", "Switch character"}, {"DPAD_LEFT", "Cycle targets (allies and enemies)"},
        {"DPAD_RIGHT", "Cycle targets (allies and enemies)"}};  /* both ways, friend and foe (owner, 2026-11-18) */
    static const char *const cutscene[][2] = {{"A", "Next line"}, {"B", "Next line"}, {"R", "Fast forward"},
                                              {"PLUS", "Pause"}};  /* B too (owner, 2026-11-13) */
    if (!strncmp(id, "LS_", 3) && strcmp(id, "LS_PRESS")) return mode == 2 ? NULL : "Move";
    if (!strncmp(id, "RS_", 3) && strcmp(id, "RS_PRESS")) return mode == 2 ? NULL : "Camera";
    const char *const(*t)[2] = mode == 1 ? battle : mode == 2 ? cutscene : field;
    int n = mode == 1 ? (int)(sizeof battle / sizeof *battle) : mode == 2 ? (int)(sizeof cutscene / sizeof *cutscene)
                                                                             : (int)(sizeof field / sizeof *field);
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i][0], id)) return t[i][1];
    return NULL;
}

/* A key's or mouse part's actions in the Overview's mode: their names joined (each once) into out; 0 if none.
 * *move: only moving or the camera (teal); *shortcut: exploring's D-pad shortcut number (1-4, +4 while 1 is held). */
static int kv_mask_names(uint32_t m, char *out, size_t n, int *move, int *shortcut);
static int kv_names(int key, char *out, size_t n, int *move, int *shortcut) {
    return kv_mask_names(bindings_key_inputs(kv_mode == 1 ? 1 : 0, key), out, n, move, shortcut);
}
/* the same for a set of inputs (a mask of input numbers): a key's, or a part of the controller's */
static int kv_mask_names(uint32_t m, char *out, size_t n, int *move, int *shortcut) {
    *out = 0, *move = 1, *shortcut = 0;
    int found = 0;
    for (int i = 0; i < 32; i++) {
        if (!(m >> i & 1)) continue;
        const char *id = bindings_input_id(i), *a = kv_action(kv_mode, id);
        if (!a) continue;
        if (kv_mode == 0 && !strncmp(id, "DPAD_", 5))
            *shortcut = !strcmp(id, "DPAD_UP") ? 1 : !strcmp(id, "DPAD_RIGHT") ? 2 : !strcmp(id, "DPAD_DOWN") ? 3 : 4;
        if (strcmp(a, "Move") && strcmp(a, "Camera")) *move = 0;
        if (strstr(out, a)) continue;  /* the same action twice (a stick's directions) */
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%s", l ? " / " : "", a);
        found = 1;
    }
    return found;
}

/* text cut with "..." to fit w */
static void kv_fit(int f, float px, const char *s, float w, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    if (ui_text_w(f, px, out) <= w) return;
    for (size_t l = strlen(out); l > 1; l--) {
        out[l - 1] = 0;
        char t[200];
        snprintf(t, sizeof t, "%s...", out);
        if (ui_text_w(f, px, t) <= w) {
            snprintf(out, n, "%s", t);
            return;
        }
    }
}

/* a key's label in up to two lines of width w: split at the last space that lets the first line fit, the rest
 * cut with "..." if it still doesn't; returns the lines (1 or 2) */
static int kv_lines(int f, float px, const char *s, float w, char *l1, char *l2, size_t n) {
    *l2 = 0;
    if (ui_text_w(f, px, s) <= w) {
        snprintf(l1, n, "%s", s);
        return 1;
    }
    char t[200];
    snprintf(t, sizeof t, "%s", s);
    for (char *sp = strrchr(t, ' '); sp; sp = strrchr(t, ' ')) {
        *sp = 0;
        if (ui_text_w(f, px, t) <= w) {
            snprintf(l1, n, "%s", t);
            const char *rest = s + (sp - t) + 1;
            while (*rest == '/' || *rest == ' ') rest++;  /* "A / B": the second line starts at B */
            kv_fit(f, px, rest, w, l2, n);
            if (l1[0] && l1[strlen(l1) - 1] == '/') l1[strlen(l1) - 1] = 0;  /* no "/" left hanging */
            while (l1[0] && l1[strlen(l1) - 1] == ' ') l1[strlen(l1) - 1] = 0;
            return 2;
        }
    }
    kv_fit(f, px, s, w, l1, n);  /* one long word */
    return 1;
}

/* a segmented switch of n labels at (x, y), the chosen one filled; returns its width (c NULL: measures) */
static float kv_seg(const UiCanvas *c, float x, float y, const char *const *labels, int n, int chosen, int kind) {
    float pad = 16, h = 40, w = 4;
    for (int i = 0; i < n; i++) w += ui_text_w(UI_ROUNDED_700, 15, labels[i]) + 2 * pad + 4;
    if (!c) return w;
    ui_box(c, x, y, w, h + 8, 12, SOFT, 0, 0);
    float bx = x + 4;
    for (int i = 0; i < n; i++) {
        float bw = ui_text_w(UI_ROUNDED_700, 15, labels[i]) + 2 * pad;
        int hv = i != chosen && hovered(kind, i, 0);
        if (i == chosen || hv) ui_box(c, bx, y + 4, bw, h, 9, i == chosen ? PLUM : LILAC, 0, 0);
        ui_text_in(c, UI_ROUNDED_700, 15, bx, y + 4, bw, h, 1, labels[i], i == chosen ? SNOW : PLUM);
        hit_add(bx, y + 4, bw, h, kind, i, 0);
        bx += bw + 4;
    }
    return w;
}

/* drawn at x0, top, W design units wide (the Controls page: 488, 1024; F2's quick look: bigger, centred) */
static void draw_controls_overview(const UiCanvas *c, float x0, float top, float W, int peek) {
    static const char *const modes[3] = {"Exploring", "Battle", "Cutscene"};
    const uint32_t TEAL = 0x1e9aa6;
    kv_seg(c, x0, top, modes, 3, kv_mode, HIT_KV_MODE);
    /* the keyboard: 18.5 units wide with 18 gaps, beside the mouse */
    float mw = 120, g = 5, kbw = W - mw - 36, k = (kbw - g * 18) / 18.5f;
    float ky = top + 64, hover_key = -1;
    char names[200], t[200];
    const char *hcap = NULL;
    int hcode = 0;
    /* the key in focus: under the mouse, else the last one pressed on the real keyboard */
    int focus_key = hover_kind == HIT_KV_KEY ? 0 : kv_focus;
    for (int r = 0; r < 6; r++) {
        float kx = x0, y = ky + r * (k + g) + (r ? 10 : 0);
        for (int i = 0; i < 17 && kv_rows[r][i].w > 0; i++) {
            const KvKey *key = &kv_rows[r][i];
            float w = k * key->w + g * (key->w - 1);
            if (key->code) {
                int move, sc, lit = kv_names(key->code, names, sizeof names, &move, &sc);
                const char *port = kv_port(key->code);
                if (port) lit = 1, move = 0, snprintf(names, sizeof names, "%s", port);
                int hv = lit && (hovered(HIT_KV_KEY, key->code, 0) || focus_key == key->code);
                int down = kv_held[key->code >> 3 & 63] >> (key->code & 7) & 1;  /* held: the ring, lit or not */
                uint32_t fill = !lit ? WHITE : port ? GOLD : move ? TEAL : PLUM;
                uint32_t ink = !lit ? MUTED : port ? ON_GOLD : SNOW;
                if (hv || down) ui_box(c, kx - 3, y - 3, w + 6, k + 6, 10, port ? PLUM : RING, 0, 0);
                ui_box(c, kx, y, w, k, 8, fill, lit ? 0 : 1.5f, LINE);
                char l1[200], l2[200];
                int lines = 1;
                float fs = k * 0.19f;
                /* the D-pad's keys read "Shortcut" (owner, 2026-11-13); their numbers are in the detail bar */
                if (lit) lines = kv_lines(UI_GOTHIC_700, fs, names, w - 6, l1, l2, sizeof l1);
                /* the key's name at the top, its action under it in one or two lines */
                float capy = !lit ? y : lines == 2 ? y + k * 0.04f : y + k * 0.12f, caph = lit ? k * 0.34f : k;
                int sc_ = key->code;
                if (sc_ == SDL_SCANCODE_UP || sc_ == SDL_SCANCODE_DOWN || sc_ == SDL_SCANCODE_LEFT || sc_ == SDL_SCANCODE_RIGHT) {
                    /* the arrows as a D-pad's marks: a filled triangle (owner's pick A, 2026-11-13), 18 x 13 in a
                     * 70-unit key, drawn in a 24-unit view box */
                    static const char *const tri[4] = {
                        "<path d=\"M12 5.5l9 13H3z\" fill=\"#000\"/>", "<path d=\"M12 18.5l9-13H3z\" fill=\"#000\"/>",
                        "<path d=\"M5.5 12l13-9v18z\" fill=\"#000\"/>", "<path d=\"M18.5 12l-13-9v18z\" fill=\"#000\"/>"};
                    int ti = sc_ == SDL_SCANCODE_UP ? 0 : sc_ == SDL_SCANCODE_DOWN ? 1 : sc_ == SDL_SCANCODE_LEFT ? 2 : 3;
                    float ts = k * 0.32f;
                    ui_shape(c, tri[ti], kx + (w - ts) / 2, capy + (caph - ts) / 2, ts, ts, 24, ink);
                } else
                    ui_text_in(c, UI_GOTHIC_700, k * 0.25f, kx, capy, w, caph, 1, key->cap, ink);
                if (lit) {
                    float ly = lines == 2 ? y + k * 0.40f : y + k * 0.50f, lh = k * 0.27f;
                    ui_text_in(c, UI_GOTHIC_700, fs, kx + 3, ly, w - 6, lh, 1, l1, ink);
                    if (lines == 2) ui_text_in(c, UI_GOTHIC_700, fs, kx + 3, ly + lh * 0.95f, w - 6, lh, 1, l2, ink);
                    hit_add(kx, y, w, k, HIT_KV_KEY, key->code, 0);
                    if (hv) hover_key = 1, hcap = key->cap, hcode = key->code;
                }
            }
            kx += w + g;
        }
    }
    /* the mouse, drawn as a real one (owner, 2026-11-13): a 120 x 190 body; the two buttons are its top 86 units,
     * each following the body's curve and wrapping round the wheel's slot (the wheel 22 x 48 at (49, 16), the slot
     * 4 units wider all round); the action inside, L / R under it. Unbound buttons are outlined only. */
    float mx = x0 + W - mw, my = ky + k + 10, mh = 190;
    ui_box(c, mx, my, mw, mh, mw / 2, WHITE, 1.5f, LINE);
    static const struct { int part, key; const char *cap, *side, *path; } mparts[2] = {
        {KV_MOUSE_LEFT, MOUSE_BIND_LEFT, "Left click", "L",
         "M3 86V60A57 57 0 0 1 59 3V12A15 15 0 0 0 45 27V53A15 15 0 0 0 59 68V86Z"},
        {KV_MOUSE_RIGHT, MOUSE_BIND_RIGHT, "Right click", "R",
         "M117 86V60A57 57 0 0 0 61 3V12A15 15 0 0 1 75 27V53A15 15 0 0 1 61 68V86Z"}};
    for (int p = 0; p < 2; p++) {
        int move, sc, lit = kv_names(mparts[p].key, names, sizeof names, &move, &sc);
        int hv = lit && (hovered(HIT_KV_KEY, mparts[p].part, 0) || focus_key == mparts[p].key || kv_mouse_held >> p & 1);
        char el[256];
        if (lit) snprintf(el, sizeof el, "<path d=\"%s\" fill=\"#000\"/>", mparts[p].path);
        else snprintf(el, sizeof el, "<path d=\"%s\" stroke=\"#000\" stroke-width=\"1.5\"/>", mparts[p].path);
        ui_shape(c, el, mx, my, mw, 90, mw, lit ? (hv ? NIGHT : PLUM) : LINE);
        /* the text between the body's edge and the wheel's slot: x 6..42 (left), 78..114 (right), in the keys' style
         * (their sizes from the key size k, their ink): the action in the middle, L / R under it (owner's pick A) */
        /* below y 36 the body's curve cuts into that strip (owner's screenshot: "Attack" ran past the edge), so the
         * action sits from y 36 (room for two lines), L / R from y 66; a word wider than the strip gets a smaller size
         * rather than being cut */
        float tx = mx + (p ? 77 : 7), tw = 36, fs = k * 0.19f;
        ui_text_in(c, UI_GOTHIC_700, k * 0.25f, tx, my + 66, tw, 18, 1, mparts[p].side, lit ? SNOW : MUTED);
        if (lit) {
            char word[200];
            for (const char *s = names; *s;) {
                while (*s == ' ') s++;
                size_t l = strcspn(s, " ");
                if (!l) break;
                snprintf(word, sizeof word, "%.*s", (int)l, s);
                while (fs > 7 && ui_text_w(UI_GOTHIC_700, fs, word) > tw) fs -= 0.5f;
                s += l;
            }
            ui_text_wrap(c, UI_GOTHIC_700, fs, 0, tx, my + 36, tw, fs * 1.35f, 1, names, SNOW);
            hit_add(mx + (p ? mw / 2 : 0), my, mw / 2, 86, HIT_KV_KEY, mparts[p].part, 0);
            if (hv) hover_key = 1, hcap = mparts[p].cap, hcode = mparts[p].key;
        }
    }
    {  /* the wheel: pressed, and turned (exploring's shortcuts / battle's character switch) */
        int move, move2, sc, lit = kv_names(MOUSE_BIND_MIDDLE, names, sizeof names, &move, &sc);
        char turn[200];
        int lit2 = kv_names(MOUSE_BIND_WHEEL_UP, turn, sizeof turn, &move2, &sc);
        int hv = (lit || lit2) && (hovered(HIT_KV_KEY, KV_WHEEL, 0) || focus_key == MOUSE_BIND_MIDDLE || kv_mouse_held >> 2 & 1);
        /* coloured as the keys are (the legend below): what it does in the game is a game button (Scan, the
         * shortcuts), teal only if it all moves or turns the camera; never gold, the wheel has no window action */
        uint32_t wfill = (!lit || move) && (!lit2 || move2) ? TEAL : PLUM;
        ui_box(c, mx + mw / 2 - 11, my + 16, 22, 48, 11, lit || lit2 ? (hv ? NIGHT : wfill) : SOFT, 1.5f, LINE);
        if (lit || lit2) {
            hit_add(mx + mw / 2 - 14, my + 12, 28, 56, HIT_KV_KEY, KV_WHEEL, 0);
            if (hv) hover_key = 1, hcap = "Wheel", hcode = -1;
        }
        float ly = my + mh + 10;
        if (lit) snprintf(t, sizeof t, "Wheel press: %s", names), ui_text_wrap(c, UI_GOTHIC_500, 13, 0, mx - 20, ly, mw + 40, 17, 1, t, MUTED), ly += 36;
        if (lit2) snprintf(t, sizeof t, "Wheel: %s", turn), ui_text_wrap(c, UI_GOTHIC_500, 13, 0, mx - 20, ly, mw + 40, 17, 1, t, MUTED);
    }
    /* the detail bar: the key under the mouse, in a full sentence */
    float by = ky + 6 * (k + g) + 10 + 18, bh = 58;
    (void)peek;  /* the quick look has the bar too (owner, 2026-11-12) */
    ui_box(c, x0, by, W, bh, 14, NIGHT, 0, 0);
    if (hover_key < 0) {
        ui_text_in(c, UI_GOTHIC_500, 16, x0 + 22, by, W - 44, bh, 0, "Point at a key, or press one, to see what it does.", ON_DARK);
    } else {
        float cw = ui_text_w(UI_GOTHIC_700, 16, hcap) + 24;
        ui_box(c, x0 + 18, by + 13, cw, bh - 26, 8, VSOFT, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 16, x0 + 18, by + 13, cw, bh - 26, 1, hcap, SNOW);
        int move, sc = 0;
        const char *port = hcode > 0 ? kv_port(hcode) : NULL;
        if (port) snprintf(t, sizeof t, "%s.", port);
        else if (hcode == -1) {
            char turn[200];
            int a = kv_names(MOUSE_BIND_MIDDLE, names, sizeof names, &move, &sc);
            int b = kv_names(MOUSE_BIND_WHEEL_UP, turn, sizeof turn, &move, &sc);
            snprintf(t, sizeof t, "%s%s%s%s%s", a ? "Press: " : "", a ? names : "", a && b ? ". " : "", b ? "Turn: " : "",
                     b ? turn : "");
        } else {
            kv_names(hcode, names, sizeof names, &move, &sc);
            if (sc) snprintf(t, sizeof t, "Shortcut %d, or shortcut %d while you hold 1.", sc, sc + 4);
            else snprintf(t, sizeof t, "%s.", names);
        }
        ui_text_in(c, UI_GOTHIC_500, 17, x0 + 18 + cw + 16, by, W - cw - 60, bh, 0, t, SNOW);
    }
    /* the colours */
    float lx = x0, ly = by + bh + 16;
    static const char *const leg[3] = {"Game buttons", "Moving and the camera", "Menus and screenshots"};
    uint32_t lc[3] = {PLUM, TEAL, GOLD};
    for (int i = 0; i < 3; i++) {
        ui_box(c, lx, ly + 3, 16, 16, 4, lc[i], 0, 0);
        lx += 24 + ui_text(c, UI_GOTHIC_500, 15, lx + 24, ly, 22, leg[i], MUTED) + 26;
    }
}

/* ---- the Controls page's controller view: the pad in the middle, its bound buttons lit, a thin line from each to
 * its name, the names stacked down both sides in the buttons' top-to-bottom order so no two lines cross (the
 * owner's mockup, 2026-11-12). The pad follows the connected controller, as the game's button icons do: Xbox,
 * PlayStation (D-pad top left, both sticks low) or Switch labels. A part is lit by what it is bound to in the
 * chosen mode (the real pad bindings, bindings.c), named as the keyboard view names it (kv_action). Gold parts
 * are the port's own: the overlay button, the DualSense's mic (the HUD), and View / Create / Minus, which held
 * shows the quick look (a short press still goes to the game: auto battle in battles). */
enum { PV_ZL = GM_PAD_ZL, PV_L, PV_ZR, PV_R, PV_LS, PV_RS, PV_DPAD, PV_S, PV_E, PV_W, PV_N, PV_MINUS, PV_PLUS, PV_SHARE,
       PV_MIC, PV_PARTS };  /* gamemenu.h's parts; PV_S..PV_N: the face buttons by their place */
/* Pressing a real button lights it on the drawing (gamemenu_held): the parts held now (a gold ring round each), and
 * the last one pressed, which stays in focus as if the mouse pointed at it when it is lit (-1 none) */
static uint32_t pv_down;
static int pv_focus = -1;

/* where each part sits in the pad's 400 x 260 box, per style; every layout is mirrored around x = 200 */
static void pv_layout(int style, float pos[PV_PARTS][2]) {
    static const float base[PV_PARTS][2] = {
        [PV_ZL] = {97, 14}, [PV_L] = {101, 37}, [PV_ZR] = {303, 14}, [PV_R] = {299, 37}, [PV_LS] = {108, 104},
        [PV_RS] = {250, 170}, [PV_DPAD] = {150, 170}, [PV_S] = {292, 132}, [PV_E] = {320, 104}, [PV_W] = {264, 104},
        [PV_N] = {292, 76}, [PV_MINUS] = {175, 92}, [PV_PLUS] = {225, 92}, [PV_SHARE] = {200, 116}, [PV_MIC] = {200, 122}};
    memcpy(pos, base, sizeof base);
#define AT(p, x, y) (pos[p][0] = (x), pos[p][1] = (y))
    if (style == 1)
        AT(PV_DPAD, 108, 104), AT(PV_LS, 150, 172), AT(PV_RS, 250, 172), AT(PV_MINUS, 140, 66), AT(PV_PLUS, 260, 66),
            AT(PV_SHARE, 200, 74);
    if (style == 2) AT(PV_MINUS, 168, 78), AT(PV_PLUS, 232, 78), AT(PV_SHARE, 172, 108);
#undef AT
}

/* what the controller prints on a part; \5..\8: PlayStation's shapes, drawn as symbols (bind_sym) */
static const char *pv_chip(int part, int style) {
    static const char *const names[3][PV_PARTS] = {
        {"LT", "LB", "RT", "RB", "L stick", "R stick", "D-pad", "A", "B", "X", "Y", "View", "Menu", "Share", ""},
        {"L2", "L1", "R2", "R1", "L stick", "R stick", "D-pad", "\x08", "\x06", "\x07", "\x05", "Create", "Options",
         "Touchpad", "Mic"},
        {"ZL", "L", "ZR", "R", "L stick", "R stick", "D-pad", "B", "A", "Y", "X", "-", "+", "Capture", ""}};
    if (style == 1 && (part == PV_MINUS || part == PV_PLUS)) {  /* the PlayStation's middle buttons, by model */
        int m = bindings_ps_model();
        if (part == PV_MINUS) return m == 3 ? "Select" : m == 4 ? "Share" : "Create";
        return m == 3 ? "Start" : "Options";
    }
    if (style == 0 && (part == PV_MINUS || part == PV_PLUS) && bindings_xbox360())  /* an Xbox 360 pad's */
        return part == PV_MINUS ? "Back" : "Start";
    return names[style < 0 || style > 2 ? 0 : style][part];
}
static float pv_chip_w(const char *s, float px) {
    return *s >= 5 && *s <= 8 ? px * 1.2f : ui_text_w(UI_ROUNDED_800, px, s);
}
static void pv_chip_text(const UiCanvas *c, float x, float y, float w, float h, const char *s, float px, uint32_t rgb) {
    if (*s >= 5 && *s <= 8) ui_icon(c, bind_sym[(int)*s], x + (w - px * 1.5f) / 2, y + (h - px * 1.5f) / 2, px * 1.5f, 2.6f, rgb);
    else ui_text_in(c, UI_ROUNDED_800, px, x, y, w, h, 1, s, rgb);
}

/* the bindings' name (BPAD_*) of a part; -1: not something the game's inputs are bound to */
static int pv_bpad(int part) {
    static const int map[PV_PARTS] = {BPAD_L2, BPAD_L1, BPAD_R2, BPAD_R1, BPAD_LSTICK, BPAD_RSTICK, BPAD_DPAD, BPAD_SOUTH,
                                      BPAD_EAST, BPAD_WEST, BPAD_NORTH, BPAD_BACK, BPAD_START, -1, -1};
    return map[part];
}

/* What a part shows in the chosen mode: its name, a smaller second line, and its kind: 0 not lit, 1 a game
 * button (purple), 2 moving and the camera (teal), 3 the port's own (gold). */
typedef struct {
    char n1[100], n2[100];
    int kind;
} PvLabel;
static void pv_label(int part, const GameMenuInfo *in, int style, PvLabel *l) {
    char a[200], b[200];
    int move, sc, set = kv_mode == 1 ? 1 : 0;
    memset(l, 0, sizeof *l);
    if (part == PV_SHARE) {  /* the overlay button; a pad without one opens it with its two middle buttons */
        snprintf(l->n1, sizeof l->n1, "Overlay");
        if (!strncmp(in->close, "Back + Start", 12) || !strcmp(in->close, "Select + Start"))
            snprintf(l->n2, sizeof l->n2, "No %s button: %s + %s", pv_chip(PV_SHARE, style), pv_chip(PV_MINUS, style),
                     pv_chip(PV_PLUS, style));
        else if (style == 1)  /* the touchpad: a menu pointer while a menu is open (window.c tp_touch) */
            snprintf(l->n2, sizeof l->n2, "In menus: drag to point, click to choose");
        l->kind = 3;
        return;
    }
    if (part == PV_MIC) {  /* only a DualSense has it */
        if (style == 1 && in->pad_mic) snprintf(l->n1, sizeof l->n1, "Performance HUD"), l->kind = 3;
        return;
    }
    int has = kv_mask_names(bindings_pad_inputs(set, pv_bpad(part)), a, sizeof a, &move, &sc);
    if (part == PV_MINUS && !settings_legacy_keys()) {  /* held: the quick look; the game's own name first */
        if (has) snprintf(l->n1, sizeof l->n1, "%s", a), snprintf(l->n2, sizeof l->n2, "Hold: Controls");
        else snprintf(l->n1, sizeof l->n1, "Hold: Controls");
        l->kind = 3;
        return;
    }
    if (part == PV_LS || part == PV_RS) {  /* a stick: what moving it does, and under it what pressing it does */
        int m2, s2, press = kv_mask_names(bindings_pad_inputs(set, part == PV_LS ? BPAD_L3 : BPAD_R3), b, sizeof b, &m2, &s2);
        if (!has && !press) return;
        if (has) snprintf(l->n1, sizeof l->n1, "%s", a);
        snprintf(has ? l->n2 : l->n1, sizeof l->n1, "%s%.80s", press ? "Press: " : "", press ? b : "");
        l->kind = 2;
        return;
    }
    if (!has) return;
    l->kind = 1;
    if (part == PV_DPAD && kv_mode == 0 && sc) {  /* exploring: the shortcuts, four more while L is held */
        const char *hold = "L";
        for (int i = 0; i < 32; i++)
            if (!strcmp(bindings_input_id(i), "L"))
                for (int p = PV_ZL; p <= PV_N; p++)
                    if (bindings_pad_inputs(set, pv_bpad(p)) >> i & 1 && *pv_chip(p, style) > 8) hold = pv_chip(p, style);
        snprintf(l->n1, sizeof l->n1, "Shortcuts 1-4");
        snprintf(l->n2, sizeof l->n2, "Hold %s: shortcuts 5-8", hold);
        return;
    }
    if (part == PV_DPAD && kv_mode == 1 && strstr(a, "Switch character") && strstr(a, "Target ally")) {
        snprintf(l->n1, sizeof l->n1, "Up / down: switch character");
        snprintf(l->n2, sizeof l->n2, "Left / right: target ally");
        return;
    }
    char *cut = strlen(a) > 24 ? strstr(a, " / ") : NULL;  /* a long list of names: the first above the rest */
    if (cut) *cut = 0, snprintf(l->n2, sizeof l->n2, "%s", cut + 3);
    snprintf(l->n1, sizeof l->n1, "%s", a);
}

/* the keyboard keys that do the same as a part in the chosen mode, for the detail bar ("" none) */
static void pv_keys(int part, char *out, size_t n) {
    static const char *const pretty[][2] = {
        {"MOUSE LEFT", "Left mouse"}, {"MOUSE RIGHT", "Right mouse"}, {"MOUSE MID", "Wheel press"}, {"MOUSE 4", "Mouse 4"},
        {"MOUSE 5", "Mouse 5"}, {"Return", "Enter"}, {"Up", "\xe2\x86\x91"}, {"Down", "\xe2\x86\x93"},
        {"Left", "\xe2\x86\x90"}, {"Right", "\xe2\x86\x92"}};
    *out = 0;
    if (part == PV_SHARE || part == PV_MIC) {
        snprintf(out, n, "%s", part == PV_SHARE ? "Esc" : "F3");
        return;
    }
    int set = kv_mode == 1 ? 1 : 0;
    uint32_t m = bindings_pad_inputs(set, pv_bpad(part));
    const char *sep = part == PV_LS || part == PV_RS || part == PV_DPAD ? "  " : " or ";  /* directions: side by side */
    for (int i = 0; i < 32; i++) {
        if (!(m >> i & 1) || !kv_action(kv_mode, bindings_input_id(i))) continue;
        for (int k = 0; k < BIND_KEYS; k++) {
            const char *name = bindings_input_key(set, i, k);
            if (!name || !strncmp(name, "WHEEL", 5)) continue;
            for (size_t j = 0; j < sizeof pretty / sizeof *pretty; j++)
                if (!strcmp(name, pretty[j][0])) name = pretty[j][1];
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%s", l ? sep : "", name);
        }
    }
    if (part == PV_MINUS && !settings_legacy_keys()) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%sF2 for Controls", l ? " \xc2\xb7 " : "");
    }
}

/* a colour a part of the way (t) to another: a part stepping back while another is in focus, a line's lighter tone */
static uint32_t pv_mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int s = 16; s >= 0; s -= 8) {
        float x = (float)(a >> s & 255), y = (float)(b >> s & 255);
        out |= (uint32_t)(x + (y - x) * t + 0.5f) << s;
    }
    return out;
}

/* a part's name beside its chip (the controller's label for it) at height ly: dir 0 the chip ends at ex and the
 * name runs left of it as far as lim, 1 the chip starts at ex and the name runs right to lim, 2 the chip centred
 * on ex with the name under it. k: the drawing's scale. */
static void pv_name(const UiCanvas *c, int part, const PvLabel *l, const char *chip, float ex, float ly, int dir, float k,
                    float lim, int dim) {
    uint32_t fill = l->kind == 3 ? GOLD : l->kind == 2 ? 0x1e9aa6 : PLUM, ink = l->kind == 3 ? ON_GOLD : SNOW;
    uint32_t nc = INK, sc = MUTED;
    if (dim) fill = pv_mix(fill, PAPER, 0.7f), ink = pv_mix(ink, PAPER, 0.5f), nc = pv_mix(nc, PAPER, 0.7f), sc = pv_mix(sc, PAPER, 0.7f);
    float cpx = 12 * k, cw = pv_chip_w(chip, cpx) + 14 * k, ch = 22 * k;
    if (cw < 28 * k) cw = 28 * k;
    float cx = dir == 2 ? ex - cw / 2 : dir == 0 ? ex - cw : ex;
    ui_box(c, cx, ly - ch / 2, cw, ch, 7 * k, fill, 0, 0);
    pv_chip_text(c, cx, ly - ch / 2, cw, ch, chip, cpx, ink);
    float npx = 15 * k, spx = 12.5f * k, nh = 18 * k, sh = 16 * k;
    if (dir == 2) {
        ui_text_in(c, UI_GOTHIC_700, npx, ex - 200 * k, ly + 22 * k - nh / 2, 400 * k, nh, 1, l->n1, nc);
        if (l->n2[0]) ui_text_in(c, UI_GOTHIC_500, spx, ex - 200 * k, ly + 39 * k - sh / 2, 400 * k, sh, 1, l->n2, sc);
        hit_add(ex - 100 * k, ly - ch / 2, 200 * k, 58 * k, HIT_PV_PART, part, 0);
        return;
    }
    char t1[100], t2[100];
    float tx = dir == 0 ? cx - 8 * k : cx + cw + 8 * k, tw = dir == 0 ? tx - lim : lim - tx, bx = dir == 0 ? lim : tx;
    kv_fit(UI_GOTHIC_700, npx, l->n1, tw, t1, sizeof t1);
    kv_fit(UI_GOTHIC_500, spx, l->n2, tw, t2, sizeof t2);
    ui_text_in(c, UI_GOTHIC_700, npx, bx, (t2[0] ? ly - 8 * k : ly) - nh / 2, tw, nh, dir == 0 ? 2 : 0, t1, nc);
    if (t2[0]) ui_text_in(c, UI_GOTHIC_500, spx, bx, ly + 9 * k - sh / 2, tw, sh, dir == 0 ? 2 : 0, t2, sc);
    float nw = ui_text_w(UI_GOTHIC_700, npx, t1), sw = ui_text_w(UI_GOTHIC_500, spx, t2);
    if (sw > nw) nw = sw;
    hit_add(dir == 0 ? tx - nw : cx, ly - 20 * k, cw + 8 * k + nw, 40 * k, HIT_PV_PART, part, 0);
}

/* a part of the pad: a rounded box centred on (cx, cy) with what is printed on it; bg: what it lies on (the body or
 * the page), for the lighter tones. Returns its fill. */
static uint32_t pv_button(const UiCanvas *c, int part, float cx, float cy, float w, float h, float r, int kind, int dim,
                          const char *text, float k, uint32_t bg) {
    uint32_t fill, ink, border = 0;
    float bw = 0, ring = 4 * k;
    /* held on the real controller: a gold ring round it (dark round a gold one), lit or not */
    if (pv_down >> part & 1)
        ui_box(c, cx - w / 2 - ring, cy - h / 2 - ring, w + 2 * ring, h + 2 * ring, r + ring, kind == 3 ? NIGHT : RING, 0, 0);
    if (!kind) fill = pv_mix(SOFT, bg, 0.45f), border = pv_mix(LINE, bg, 0.3f), bw = 1.5f * k, ink = pv_mix(MUTED, fill, 0.5f);
    else fill = kind == 3 ? GOLD : kind == 2 ? 0x1e9aa6 : PLUM, ink = kind == 3 ? ON_GOLD : SNOW;
    if (kind && dim) fill = pv_mix(fill, bg, 0.7f), ink = pv_mix(ink, bg, 0.5f);
    ui_box(c, cx - w / 2, cy - h / 2, w, h, r, fill, bw, border);
    if (text && *text) pv_chip_text(c, cx - w / 2, cy - h / 2, w, h, text, 15 * k, ink);
    if (kind) hit_add(cx - w / 2, cy - h / 2, w, h, HIT_PV_PART, part, 0);
    return fill;
}

/* drawn at x0, top, W design units wide (the Controls page: 488, 1024; the quick look: bigger, centred). The
 * drawing's own units are the mockup's (its picture is 1280 wide from x = -40, the pad a 400 x 260 box drawn 1.4
 * times at (320, 120)); k turns them into the design's. */
static void draw_controls_pad(const UiCanvas *c, const GameMenuInfo *in, float x0, float top, float W, int peek) {
    static const char *const modes[3] = {"Exploring", "Battle", "Cutscene"};
    const uint32_t TEAL = 0x1e9aa6, DGOLD = 0xb88a2a;
    const float S = 1.4f, PX = 320, PY = 120;
    int style = bindings_pad_style();
    float k = W / 1280, oy = top;
    if (!peek) kv_seg(c, x0, top, modes, 3, kv_mode, HIT_KV_MODE), oy = top + 56;
#define MX(v) (x0 + ((v) + 40) * k)
#define MY(v) (oy + ((v) - 50) * k)
    float pos[PV_PARTS][2];
    PvLabel lab[PV_PARTS];
    pv_layout(style, pos);
    for (int p = 0; p < PV_PARTS; p++) pv_label(p, in, style, &lab[p]);
    /* the part in focus: under the mouse, or the last one pressed on the controller; the others step back */
    int hot = -1;
    for (int p = 0; p < PV_PARTS && !peek; p++)
        if (lab[p].kind && hovered(HIT_PV_PART, p, 0)) hot = p;
    if (hot < 0 && pv_focus >= 0 && lab[pv_focus].kind) hot = pv_focus;
#define DIMMED(p) (hot >= 0 && hot != (p))
    /* the body: drawn 1.18 times around the buttons' middle (200, 140), so it has room around them; its picture
     * is the pad's box with that margin (x -40..440, y 10..280) */
    {
        static const char path[] = "M110 40h180c40 0 62 20 74 58l26 92c10 36-14 62-42 54c-18-5-30-22-44-44l-14-22h-180"
                                   "l-14 22c-14 22-26 39-44 44c-28 8-52-18-42-54l26-92c12-38 34-58 74-58z";
        static const char tf[] = "translate(40 -10) translate(200 140) scale(1.18) translate(-200 -140)";
        char el[512];
        float bx = MX(PX - 40 * S), by = MY(PY + 10 * S), bw = 480 * S * k, bh = 270 * S * k;
        snprintf(el, sizeof el, "<g transform=\"%s\"><path fill=\"#000000\" d=\"%s\"/></g>", tf, path);
        ui_shape(c, el, bx, by, bw, bh, 480, WHITE);
        snprintf(el, sizeof el, "<g transform=\"%s\"><path stroke=\"#000000\" stroke-width=\"1.6\" d=\"%s\"/></g>", tf, path);
        ui_shape(c, el, bx, by, bw, bh, 480, LINE);
    }
    /* the lines and the names. A line is its colour half way to the page's (thin lines at full strength look
     * heavy); in focus at full strength and thicker */
    uint32_t leadc[4] = {0, PLUM, TEAL, DGOLD};
#define LEAD(p) (hot == (p) ? leadc[lab[p].kind] : pv_mix(leadc[lab[p].kind], PAPER, DIMMED(p) ? 0.85f : 0.5f))
#define LEADW(p) ((hot == (p) ? 2.0f : 1.5f) * k)
    /* the sides: each line runs sideways from its button at its own height, then bends to its name; the names keep
     * the buttons' top-to-bottom order so nothing crosses (the left face button's line first goes up, between
     * the top one and the shoulder) */
    int left[4] = {PV_ZL, PV_L, PV_LS, PV_DPAD}, right[7] = {PV_ZR, PV_R, PV_W, PV_N, PV_E, PV_S, PV_RS};
    if (pos[PV_DPAD][1] < pos[PV_LS][1]) left[2] = PV_DPAD, left[3] = PV_LS;
    float out = 400 * S * (1.18f - 1) / 2;  /* the name columns stay clear of the bigger body */
    for (int side = 0; side < 2; side++) {
        int list[7], n = 0;
        for (int i = 0; i < (side ? 7 : 4); i++)
            if (lab[(side ? right : left)[i]].kind) list[n++] = (side ? right : left)[i];
        float t0 = 70, b0 = 440, step = n > 1 ? (b0 - t0) / (float)(n - 1) : 0;
        if (step > 58) step = 58;
        float y0 = t0 + ((b0 - t0) - step * (float)(n - 1)) / 2;
        float col = side ? PX + 400 * S + 18 + out : PX - 18 - out, ex = side ? col + 28 : col - 28;
        float hx = side ? col - 40 : col + 40;
        for (int i = 0; i < n; i++) {
            int p = list[i], up = p == PV_W;
            float bx = PX + pos[p][0] * S, by = PY + pos[p][1] * S, ly = y0 + (float)i * step, run = up ? PY + 54 * S : by;
            uint32_t lc = LEAD(p);
            if (up) ui_line(c, MX(bx), MY(by), MX(bx), MY(run), LEADW(p), lc, 1);
            ui_line(c, MX(bx), MY(run), MX(hx), MY(run), LEADW(p), lc, 1);
            ui_line(c, MX(hx), MY(run), MX(col), MY(ly), LEADW(p), lc, 1);
            ui_line(c, MX(col), MY(ly), MX(ex), MY(ly), LEADW(p), lc, 1);
            pv_name(c, p, &lab[p], pv_chip(p, style), MX(side ? ex + 6 : ex - 6), MY(ly), side, k, side ? MX(1240) : MX(-40),
                    DIMMED(p));
        }
    }
    for (int i = 0; i < 2; i++) {  /* the two small middle buttons: straight up, their names above the pad */
        int p = i ? PV_PLUS : PV_MINUS;
        if (!lab[p].kind) continue;
        float bx = PX + pos[p][0] * S, by = PY + pos[p][1] * S, ly = PY - 40;
        ui_line(c, MX(bx), MY(by), MX(bx), MY(ly), LEADW(p), LEAD(p), 1);
        pv_name(c, p, &lab[p], pv_chip(p, style), MX(bx), MY(ly), i, k, i ? MX(bx + 330) : MX(bx - 330), DIMMED(p));
    }
    {  /* the overlay button (and the mic): straight down, their names under the pad, side by side when both */
        int two = lab[PV_MIC].kind != 0;
        for (int i = 0; i <= two; i++) {
            int p = i ? PV_MIC : PV_SHARE;
            float bx = PX + pos[p][0] * S + (two ? (i ? 4 : -4) : 0), by = PY + pos[p][1] * S;
            float lx = two ? PX + pos[p][0] * S + (i ? 110 : -110) : bx, ly = 488, mid = ly - 30;
            uint32_t lc = LEAD(p);
            ui_line(c, MX(bx), MY(by), MX(bx), MY(mid), LEADW(p), lc, 1);
            ui_line(c, MX(bx), MY(mid), MX(lx), MY(mid), LEADW(p), lc, 1);
            ui_line(c, MX(lx), MY(mid), MX(lx), MY(ly - 11), LEADW(p), lc, 1);
            pv_name(c, p, &lab[p], pv_chip(p, style), MX(lx), MY(ly), 2, k, 0, DIMMED(p));
        }
    }
    /* the buttons, over where the lines start */
#define BTN(p, w, h, r, text, bg) \
    pv_button(c, p, MX(PX + pos[p][0] * S), MY(PY + pos[p][1] * S), (w) * S * k, (h) * S * k, (r) * S * k, lab[p].kind, \
              DIMMED(p), text, k, bg)
    BTN(PV_ZL, 72, 24, 10, pv_chip(PV_ZL, style), PAPER), BTN(PV_ZR, 72, 24, 10, pv_chip(PV_ZR, style), PAPER);
    BTN(PV_L, 66, 18, 9, pv_chip(PV_L, style), WHITE), BTN(PV_R, 66, 18, 9, pv_chip(PV_R, style), WHITE);
    BTN(PV_MINUS, 26, 14, 7, "", WHITE), BTN(PV_PLUS, 26, 14, 7, "", WHITE);
    if (style == 1) {  /* the touchpad, and the mic button under it */
        BTN(PV_SHARE, 74, 40, 8, "", WHITE), BTN(PV_MIC, 16, 6, 3, "", WHITE);
    } else if (style == 2) {  /* Capture, and Home mirroring it (never lit: Steam and the console take that one) */
        BTN(PV_SHARE, 13, 13, 3, "", WHITE);
        float hx = MX(PX + (400 - pos[PV_SHARE][0]) * S), hy = MY(PY + pos[PV_SHARE][1] * S), hr = 7.5f * S * k;
        ui_box(c, hx - hr, hy - hr, 2 * hr, 2 * hr, hr, pv_mix(SOFT, WHITE, 0.45f), 1.5f * k, pv_mix(LINE, WHITE, 0.3f));
    } else {
        BTN(PV_SHARE, 20, 12, 6, "", WHITE);
    }
    for (int i = 0; i < 2; i++) {  /* the sticks: a disc with a ring on it */
        int p = i ? PV_RS : PV_LS;
        uint32_t fill = BTN(p, 54, 54, 27, "", WHITE);
        float cx = MX(PX + pos[p][0] * S), cy = MY(PY + pos[p][1] * S), r = 17 * S * k;
        ui_box(c, cx - r, cy - r, 2 * r, 2 * r, r, fill, 2 * k, lab[p].kind ? pv_mix(fill, SNOW, 0.5f) : pv_mix(LINE, WHITE, 0.3f));
    }
    {  /* the D-pad: a cross of two bars; not lit, its outline is one cross (no lines through its middle) */
        float cx = MX(PX + pos[PV_DPAD][0] * S), cy = MY(PY + pos[PV_DPAD][1] * S), u = S * k;
        uint32_t held = pv_down;  /* held: one ring round the whole cross, under both bars */
        if (held >> PV_DPAD & 1) {
            float ring = 4 * k;
            ui_box(c, cx - 10 * u - ring, cy - 28 * u - ring, 20 * u + 2 * ring, 56 * u + 2 * ring, 4 * u + ring, RING, 0, 0);
            ui_box(c, cx - 28 * u - ring, cy - 10 * u - ring, 56 * u + 2 * ring, 20 * u + 2 * ring, 4 * u + ring, RING, 0, 0);
        }
        pv_down &= ~(1u << PV_DPAD);
        BTN(PV_DPAD, 20, 56, 4, "", WHITE);
        uint32_t fill = BTN(PV_DPAD, 56, 20, 4, "", WHITE);
        pv_down = held;
        if (!lab[PV_DPAD].kind) ui_rect(c, cx - 10 * u + 1.5f * k, cy - 11 * u, 20 * u - 3 * k, 22 * u, fill, 1);
    }
    for (int p = PV_S; p <= PV_N; p++) BTN(p, 30, 30, 15, pv_chip(p, style), WHITE);
#undef BTN
#undef LEAD
#undef LEADW
    float by = MY(545) + 8, bh = 58;
#undef MX
#undef MY
#undef DIMMED
    if (peek) return;
    /* the detail bar: the part in focus, what it does, and the keys that do the same */
    ui_box(c, x0, by, W, bh, 14, NIGHT, 0, 0);
    if (hot < 0) {
        ui_text_in(c, UI_GOTHIC_500, 16, x0 + 22, by, W - 44, bh, 0,
                   "Point at a button, or press one, to see what it does and its keys.", ON_DARK);
    } else {
        const char *chip = pv_chip(hot, style);
        char keys[200];
        float cw = pv_chip_w(chip, 16) + 24, tx = x0 + 18 + cw + 16;
        ui_box(c, x0 + 18, by + 13, cw, bh - 26, 8, VSOFT, 0, 0);
        pv_chip_text(c, x0 + 18, by + 13, cw, bh - 26, chip, 16, SNOW);
        ui_text_in(c, UI_GOTHIC_500, 17, tx, by, 0, bh, 0, lab[hot].n1, SNOW);
        tx += ui_text_w(UI_GOTHIC_500, 17, lab[hot].n1);
        if (lab[hot].n2[0]) {
            char t[120];
            snprintf(t, sizeof t, " \xc2\xb7 %s", lab[hot].n2);
            ui_text_in(c, UI_GOTHIC_500, 17, tx, by, 0, bh, 0, t, ON_DARK);
        }
        pv_keys(hot, keys, sizeof keys);
        if (keys[0]) {
            float kw = ui_text_w(UI_GOTHIC_700, 15, keys), rx = x0 + W - 22 - kw;
            ui_text_in(c, UI_GOTHIC_700, 15, rx, by, kw, bh, 0, keys, SNOW);
            ui_text_in(c, UI_GOTHIC_500, 15, rx - 8 - 200, by, 200, bh, 2, "Keyboard:", ON_DARK);
        }
    }
    /* the colours */
    float lx = x0, ly = by + bh + 16;
    static const char *const leg[3] = {"Game buttons", "Moving and the camera", "The port's own: overlay, Controls, HUD"};
    uint32_t lc[3] = {PLUM, TEAL, GOLD};
    for (int i = 0; i < 3; i++) {
        ui_box(c, lx, ly + 3, 16, 16, 4, lc[i], 0, 0);
        lx += 24 + ui_text(c, UI_GOTHIC_500, 15, lx + 24, ly, 22, leg[i], MUTED) + 26;
    }
}

/* What is held on the real keyboard, mouse and controller (gamemenu.h), each time the window polls: on the Controls
 * page's keyboard or controller view, and in the quick look, what is pressed lights up on the drawing and, when it
 * does something, comes into focus. Elsewhere nothing is kept. */
void gamemenu_held(const bool *keys, uint32_t mouse, uint32_t parts) {
    int peek = gamemenu_peeking();
    int page = gamemenu_open() && !settings_open() && focus && sel == GM_CONTROLS && !ctrl_view && !confirm &&
               !preview && !gamemenu_prompting() && !bindings_capturing();
    uint8_t now[64] = {0};
    int fresh = 0, m = 0;
    if (!peek && !page) parts = 0;
    for (int r = 0; r < 6 && keys && (peek || page); r++)
        for (int i = 0; i < 17 && kv_rows[r][i].w > 0; i++) {
            int code = kv_rows[r][i].code;
            if (!code || !keys[code]) continue;
            if (!(kv_held[code >> 3 & 63] >> (code & 7) & 1)) fresh = code;
            now[code >> 3 & 63] |= (uint8_t)(1 << (code & 7));
        }
    /* the mouse's buttons only in the quick look: on the page they click its own buttons. SDL's mask: 1 the left
     * button, 2 the middle one, 4 the right one */
    if (peek) m = (mouse & 1 ? 1 : 0) | (mouse & 4 ? 2 : 0) | (mouse & 2 ? 4 : 0);
    if (!memcmp(now, kv_held, sizeof now) && m == kv_mouse_held && parts == pv_down) return;
    for (int b = 0; b < 3; b++)
        if ((m & ~kv_mouse_held) >> b & 1) fresh = b == 0 ? MOUSE_BIND_LEFT : b == 1 ? MOUSE_BIND_RIGHT : MOUSE_BIND_MIDDLE;
    if (fresh) {  /* into focus if it does something in the view's mode (the quick look: the game's) */
        char names[200];
        int move, sc, saved = kv_mode;
        if (peek) kv_mode = battle_active() ? 1 : 0;
        if ((fresh < 1000 && kv_port(fresh)) || kv_names(fresh, names, sizeof names, &move, &sc)) kv_focus = fresh, hover_kind = -1;
        kv_mode = saved;
    }
    for (int p = 0; p < PV_PARTS; p++)
        if ((parts & ~pv_down) >> p & 1) pv_focus = p, hover_kind = -1;
    memcpy(kv_held, now, sizeof now);
    kv_mouse_held = m, pv_down = parts;
    changed();
}

/* The controls page (the design's isControls section): a table of each Switch input with its two keys and its
 * controller input, the chosen row ringed in gold and the chosen key or input on a gold chip; the rows scroll.
 * Under it, what the chosen row does (the design's note line). */
/* What an input does in a set, for the Mouse buttons page: its action (the keyboard view's names; the D-pad's
 * shortcuts numbered) and its Switch name; none: "None" and "". */
static void mb_value(int set, int input, char *act, size_t na, char *sw, size_t ns) {
    if (input < 0) {
        snprintf(act, na, "None"), *sw = 0;
        return;
    }
    char t[48];
    bindings_input_title(input, t, sizeof t);
    sentence_case(t, sw, ns);
    const char *id = bindings_input_id(input), *a = kv_action(set, id);
    static const char *const dpad[4] = {"DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT"};
    for (int k = 0; set == 0 && k < 4; k++)
        if (!strcmp(id, dpad[k])) {
            snprintf(act, na, "Shortcut %d", k + 1);
            return;
        }
    snprintf(act, na, "%s", a ? a : set ? "Nothing in battles" : "Nothing while exploring");
}

/* The Mouse buttons page (owner's pick B, 2026-11-13): the mouse drawn at the left, lit by what its parts press in
 * the set shown, the rows at the right with ‹ › (the part in focus ringed in gold on both), a box explaining the row,
 * and the list over it all when open. */
static void draw_mouse_page(const UiCanvas *c, const GameMenuInfo *in) {
    char act[96], sw[48], t[200];
    /* the title: ‹ Controls (back), Mouse buttons, the Exploring / Battle switch at the right */
    float bw = ui_text_w(UI_ROUNDED_700, 16, "Controls") + 58, by = 76 + (34 * 1.25f - 48) / 2;
    int bh = hovered(HIT_MB_BACK, 0, 0);
    ui_box(c, 488, by, bw, 48, 24, bh ? LILAC : WHITE, 2, PLUM);
    ui_icon(c, ICON_PREV, 488 + 14, by + 13, 22, 2.4f, PLUM);
    ui_text_in(c, UI_ROUNDED_700, 16, 488 + 38, by, bw - 50, 48, 0, "Controls", PLUM);
    hit_add(488, by, bw, 48, HIT_MB_BACK, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488 + bw + 18, 76, 34 * 1.25f, "Mouse buttons", NIGHT);
    static const char *const sets[2] = {"Exploring", "Battle"};
    float sw2 = kv_seg(NULL, 0, 0, sets, 2, mb_set, HIT_MB_SET);
    kv_seg(c, 1512 - sw2, 76 + (34 * 1.25f - 48) / 2, sets, 2, mb_set, HIT_MB_SET);
    float top = 76 + 34 * 1.25f + 30;
    /* the mouse: the F2 card's shapes (120 x 190 units), S times as large */
    const float S = 2.1f, mx = 488 + 18, my = top + 6;
    static const char *const paths[2] = {"M3 86V60A57 57 0 0 1 59 3V12A15 15 0 0 0 45 27V53A15 15 0 0 0 59 68V86Z",
                                         "M117 86V60A57 57 0 0 0 61 3V12A15 15 0 0 1 75 27V53A15 15 0 0 1 61 68V86Z"};
    ui_box(c, mx, my, 120 * S, 190 * S, 60 * S, WHITE, 2, LINE);
    for (int p = 0; p < 2; p++) {
        int input = bindings_mouse_get(mb_set, p), on = mb_row == p;
        char el[200];
        snprintf(el, sizeof el, input >= 0 || on ? "<path d=\"%s\" fill=\"#000\"/>" : "<path d=\"%s\" stroke=\"#000\" stroke-width=\"1.5\"/>",
                 paths[p]);
        ui_shape(c, el, mx, my, 120 * S, 90 * S, 120, on ? (input >= 0 ? NIGHT : GOLD) : input >= 0 ? PLUM : LINE);
        float tx = mx + (p ? 77 : 7) * S, tw = 36 * S;
        uint32_t ink = input >= 0 ? SNOW : on ? ON_GOLD : MUTED;
        if (input >= 0) {
            mb_value(mb_set, input, act, sizeof act, sw, sizeof sw);
            ui_text_wrap(c, UI_GOTHIC_700, 15, 0, tx, my + 34 * S, tw, 19, 1, act, ink);
        }
        ui_text_in(c, UI_GOTHIC_700, 20, tx, my + 64 * S, tw, 26, 1, p ? "R" : "L", ink);
        hit_add(mx + (p ? 60 * S : 0), my, 60 * S, 86 * S, HIT_MB_ROW, p, 0);
    }
    {  /* the wheel: pressed or turned; lit if any of the three is bound, darker when one of them is in focus */
        int any = 0, on = mb_row >= BMOUSE_MID;
        for (int p = BMOUSE_MID; p < MB_ROWS; p++) any |= bindings_mouse_get(mb_set, p) >= 0;
        ui_box(c, mx + 49 * S, my + 16 * S, 22 * S, 48 * S, 11 * S, on ? (any ? NIGHT : GOLD) : any ? PLUM : SOFT, 2, LINE);
        ui_icon(c, "<path d=\"M12 5l5 6H7z\" fill=\"#000\" stroke=\"none\"/>", mx + 49 * S + 11 * S - 9, my + 16 * S + 8, 18, 1,
                any || on ? SNOW : MUTED);
        ui_icon(c, "<path d=\"M12 19l5-6H7z\" fill=\"#000\" stroke=\"none\"/>", mx + 49 * S + 11 * S - 9, my + 64 * S - 26, 18, 1,
                any || on ? SNOW : MUTED);
        hit_add(mx + 45 * S, my + 12 * S, 30 * S, 56 * S, HIT_MB_ROW, mb_row >= BMOUSE_MID ? mb_row : BMOUSE_MID, 0);
    }
    /* the rows: the part, then ‹ what it does (its Switch name under) › */
    float rx = 488 + 330, rw = 1024 - 330, rh = 66, gap = 12;
    for (int p = 0; p < MB_ROWS; p++) {
        float y = top + p * (rh + gap);
        int on = mb_row == p, input = bindings_mouse_get(mb_set, p);
        if (on) ui_box(c, rx - 4, y - 4, rw + 8, rh + 8, 16, RING, 0, 0);
        ui_box(c, rx, y, rw, rh, 12, WHITE, on ? 0 : 2, LINE);
        hit_add(rx, y, rw, rh, HIT_MB_ROW, p, 0);
        ui_text_in(c, UI_GOTHIC_400, 18, rx + 22, y, 0, rh, 0, mb_part_name[p], INK);
        mb_value(mb_set, input, act, sizeof act, sw, sizeof sw);
        float vx = rx + 250, vw = rw - 250 - 16;
        for (int k = 0; k < 2; k++) {  /* ‹ and ›, a box under the mouse */
            float ax = k ? vx + vw - 34 : vx, ay = y + (rh - 34) / 2;
            int hv = hovered(HIT_MB_STEP, p, k ? 1 : -1);
            if (hv) ui_box(c, ax, ay, 34, 34, 9, LILAC, 0, 0);
            ui_icon(c, k ? ICON_NEXT : ICON_PREV, ax + 6, ay + 6, 22, 2.4f, PLUM);
            hit_add(ax - 4, y, 42, rh, HIT_MB_STEP, p, k ? 1 : -1);
        }
        if (*sw) {
            ui_text_in(c, UI_GOTHIC_700, 17, vx + 40, y + 9, vw - 80, 26, 1, act, NIGHT);
            ui_text_in(c, UI_GOTHIC_500, 13, vx + 40, y + 36, vw - 80, 20, 1, sw, MUTED);
        } else {
            ui_text_in(c, UI_GOTHIC_500, 17, vx + 40, y, vw - 80, rh, 1, act, MUTED);
        }
    }
    /* the box: the row in focus, in a sentence */
    {
        float y = top + MB_ROWS * (rh + gap) + 8, h = 118;
        int input = bindings_mouse_get(mb_set, mb_row);
        mb_value(mb_set, input, act, sizeof act, sw, sizeof sw);
        ui_box(c, rx, y, rw, h, 14, NIGHT, 0, 0);
        snprintf(t, sizeof t, "%s \xc2\xb7 %s", mb_part_name[mb_row], mb_set ? "Battle" : "Exploring");
        ui_text_in(c, UI_GOTHIC_700, 17, rx + 22, y + 14, rw - 44, 26, 0, t, SNOW);
        if (input >= 0) snprintf(t, sizeof t, "%s (%s). %s steps through what it can do, %s opens the list, %s sets none.", act, sw,
                                 in->pad ? "Left / Right" : "A / D or the arrows", in->pad ? "A" : "Enter or a click",
                                 in->pad ? "X" : "Delete");
        else snprintf(t, sizeof t, "Does nothing now. %s steps through what it can do, %s opens the list.",
                      in->pad ? "Left / Right" : "A / D or the arrows", in->pad ? "A" : "Enter or a click");
        ui_text_wrap(c, UI_GOTHIC_500, 15, 0, rx + 22, y + 46, rw - 44, 15 * 1.5f, 0, t, ON_DARK);
    }
    if (!mb_list) return;
    /* the list: over the page, dimmed behind; two columns, the one bound now marked, the one in focus ringed */
    int ids[48], cur, n = mb_entries(ids, 48, &cur);
    ui_card(c, 448, 40, 1104, 764, 20, SHADE, 0.55f, 0, 0, 0);
    hit_add(448, 40, 1104, 764, HIT_MB_SCRIM, 0, 0);
    int cols = n > MB_PER_COL ? 2 : 1, per = (n + cols - 1) / cols;
    float ow = 360, oh = 44, pw = cols * ow + 48 + (cols - 1) * 16, ph = 96 + per * oh + 20, px = 1000 - pw / 2, py = 422 - ph / 2;
    ui_box(c, px, py, pw, ph, 20, PAPER, 0, 0);
    hit_add(px, py, pw, ph, HIT_MB_POP, 0, 0);  /* the list itself: not a click beside it */
    snprintf(t, sizeof t, "%s in %s", mb_part_name[mb_row], mb_set ? "battles" : "exploring");
    ui_text(c, UI_ROUNDED_800, 22, px + 24, py + 22, 22 * 1.3f, t, NIGHT);
    snprintf(t, sizeof t, "The Switch button it presses, named by what it does %s.", mb_set ? "in battles" : "while exploring");
    ui_text_in(c, UI_GOTHIC_500, 14, px + 24, py + 54, pw - 48, 22, 0, t, MUTED);
    for (int i = 0; i < n; i++) {
        float ox = px + 24 + (i / per) * (ow + 16), oy = py + 92 + (i % per) * oh;
        int on = mb_list_sel == i;
        if (on) ui_box(c, ox, oy + 2, ow, oh - 4, 10, RING, 0, 0), ui_box(c, ox + 3, oy + 5, ow - 6, oh - 10, 8, WHITE, 0, 0);
        hit_add(ox, oy, ow, oh, HIT_MB_OPT, i, 0);
        mb_value(mb_set, ids[i], act, sizeof act, sw, sizeof sw);
        if (*sw) {
            ui_text_in(c, UI_GOTHIC_700, 15, ox + 14, oy, 128, oh, 0, sw, PLUM);
            ui_text_in(c, UI_GOTHIC_400, 15, ox + 148, oy, ow - 148 - 58, oh, 0, act, INK);
        } else {
            ui_text_in(c, UI_GOTHIC_500, 15, ox + 14, oy, ow - 70, oh, 0, act, MUTED);
        }
        if (i == cur) {  /* the one bound now */
            float tw = ui_text_w(UI_GOTHIC_700, 12, "Now") + 14;
            ui_box(c, ox + ow - tw - 10, oy + (oh - 22) / 2, tw, 22, 6, PLUM, 0, 0);
            ui_text_in(c, UI_GOTHIC_700, 12, ox + ow - tw - 10, oy + (oh - 22) / 2, tw, 22, 1, "Now", SNOW);
        }
    }
}

static void draw_controls_page(const UiCanvas *c, const GameMenuInfo *in) {
    static BindRow rows[64];
    char help[512], t[160], u[160];
    int row, col, n = bindings_rows(rows, 64, &row, &col, help, sizeof help);
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    if (mb_open && ctrl_view) {
        draw_mouse_page(c, in);
        return;
    }
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Controls", NIGHT);
    /* one button at the right of the title: to the keyboard view, or back to the bindings (P, Y / Triangle) */
    const char *other = !ctrl_view ? "Bindings" : in->pad ? "Controller view" : "Keyboard view";
    float vw = ui_text_w(UI_ROUNDED_700, 17, other) + 40, vh = 48, vx = 488 + 1024 - vw, vy = 76 + (34 * 1.25f - vh) / 2;
    int vhov = hovered(HIT_KV_TAB, !ctrl_view, 0);
    ui_box(c, vx, vy, vw, vh, 24, vhov ? LILAC : WHITE, 2, PLUM);
    ui_text_in(c, UI_ROUNDED_700, 17, vx, vy, vw, vh, 1, other, PLUM);
    hit_add(vx, vy, vw, vh, HIT_KV_TAB, !ctrl_view, 0);
    float top = 76 + 34 * 1.25f + 22, bottom = 40 + 764 - 36;
    if (!ctrl_view) {  /* the controller when it was used last, else the keyboard */
        if (in->pad) draw_controls_pad(c, in, 488, top, 1024, 0);
        else draw_controls_overview(c, 488, top, 1024, 0);
        return;
    }
    if (in->pad_name) snprintf(t, sizeof t, "%s connected", in->pad_name);
    else snprintf(t, sizeof t, "No controller connected");
    ui_text_in(c, UI_GOTHIC_400, 16, 488, 76 + 12, 1024 - vw - 24, 34 * 1.25f - 12, 2, t, MUTED);
    /* the help: a fixed height (3 lines for what the row is, then its controls on a line of their own), so the table
     * keeps its size and the controls their place from row to row (owner, 2026-11-18) */
    const float hlh = 15 * 1.45f, hh = 4 * hlh + 4;
    float x = 488, w = 1024, cw[3] = {379, 379, 266}, hdr = 16 * 1.45f + 28, rh = 17 * 1.45f + 24;
    int visible = (int)((bottom - hh - 22 - top - hdr - 4) / rh);
    if (visible < 1) visible = 1;
    float th = hdr + visible * rh + 4;
    /* the rows shown: they move only when the chosen row leaves them (keys, the pad, the wheel), never because the
     * mouse hovers a row, so the table doesn't run away under the cursor */
    if (row < ctrl_first) ctrl_first = row;
    if (row >= ctrl_first + visible) ctrl_first = row - visible + 1;
    if (ctrl_first > n - visible) ctrl_first = n - visible;
    if (ctrl_first < 0) ctrl_first = 0;
    int first = ctrl_first;
    /* the table: a white box, its header row lilac */
    ui_box(c, x, top, w, th, 16, WHITE, 2, LINE);
    ui_box(c, x + 2, top + 2, w - 4, hdr, 14, SOFT, 0, 0);
    ui_rect(c, x + 2, top + 2 + hdr - 14, w - 4, 14, SOFT, 1);
    static const char *const heads[3] = {"Switch button", "Keyboard", "Controller"};
    float cx = x;
    for (int k = 0; k < 3; k++) ui_text_in(c, UI_GOTHIC_700, 16, cx + 22, top + 2, 0, hdr, 0, heads[k], PLUM), cx += cw[k];
    float y = top + 2 + hdr;
    for (int i = first; i < n && i < first + visible; i++, y += rh) {
        const BindRow *r = &rows[i];
        int on = focus && i == row;
        /* the mouse: a setting row or the reset row as a whole (a click: as Enter); an input's row chooses it, its
         * key or input cells below take the click */
        hit_add(x, y, w, rh, HIT_BIND, i, r->group && !r->value ? -2 : -1);
        ui_rect(c, x + 2, y, w - 4, 2, SOFT, 1);  /* the line between rows */
        if (on) {  /* a gold ring */
            ui_box(c, x + 4, y + 2, w - 8, rh - 2, 10, RING, 0, 0);
            ui_box(c, x + 7, y + 5, w - 14, rh - 8, 7, WHITE, 0, 0);
        }
        /* the Switch input's name */
        if (!r->group) snprintf(u, sizeof u, "%s", r->name);  /* the reset row */
        else if (!strcmp(r->group, "BUTTONS")) snprintf(u, sizeof u, "%s", r->name);
        else if (!strcmp(r->group, "KEYS FOR")) snprintf(u, sizeof u, "KEYS FOR");
        else snprintf(u, sizeof u, "%s %s", r->group, r->name);
        sentence_case(u, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_400, 17, x + 22, y, 0, rh, 0, t, r->disabled ? MUTED : INK);  /* greyed: does nothing */
        if (!r->group) continue;
        if (r->value && !strcmp(r->group, "MOUSE") && !strcmp(r->name, "BUTTONS")) {
            /* the way into the Mouse buttons page: what each part does in the set shown, as chips, and a › */
            static const char *const part[MB_ROWS] = {"Left", "Right", "Wheel press", "Wheel up", "Wheel down"};
            int set = bindings_set_shown(), any = 0;
            float sx = x + cw[0] + 22, lim = x + w - 64, chh = 28;
            for (int p = 0; p < MB_ROWS; p++) {
                int input = bindings_mouse_get(set, p);
                if (input < 0) continue;
                char act[96], sw[48], chipt[160];
                mb_value(set, input, act, sizeof act, sw, sizeof sw);
                snprintf(chipt, sizeof chipt, "%s: %s", part[p], act);
                float chw = ui_text_w(UI_GOTHIC_700, 13, chipt) + 20;
                if (sx + chw > lim) break;
                ui_box(c, sx, y + (rh - chh) / 2, chw, chh, 8, SOFT, 0, 0);
                ui_text_in(c, UI_GOTHIC_700, 13, sx, y + (rh - chh) / 2, chw, chh, 1, chipt, PLUM);
                sx += chw + 8, any = 1;
            }
            if (!any) ui_text_in(c, UI_GOTHIC_500, 16, sx, y, 0, rh, 0, "Nothing bound", MUTED);
            ui_icon(c, ICON_NEXT, x + w - 46, y + (rh - 24) / 2, 24, 2.6f, PLUM);
            continue;
        }
        /* VIBRATION, NINTENDO LAYOUT, the mouse's SHORTCUT WHEEL: an On/Off pill */
        if (r->value && (!strcmp(r->group, "CONTROLLER") || !strcmp(r->name, "SHORTCUT WHEEL"))) {
            int on_state = !strcmp(r->cells[0], "ON");
            const char *label = on_state ? "On" : "Off";
            float sx = x + cw[0] + 22, sh = rh - 16, sy = y + 8, aw = 24;
            float nw = 48, sw = 16 + aw + nw + aw + 16;
            int hv_pill = hovered(HIT_BIND, i, 0);
            if (on_state) {
                ui_box(c, sx, sy, sw, sh, sh / 2, PLUM, 0, 0);
                ui_text_in(c, UI_GOTHIC_700, 15, sx + 16 + aw, sy, nw, sh, 1, label, SNOW);
            } else {
                ui_box(c, sx, sy, sw, sh, sh / 2, hv_pill ? LILAC : WHITE, 2, PLUM);
                ui_text_in(c, UI_GOTHIC_700, 15, sx + 16 + aw, sy, nw, sh, 1, label, PLUM);
            }
            hit_add(sx, sy, sw, sh, HIT_BIND, i, 0);
            for (int k = 0; k < 2; k++) {
                float ix = k ? sx + sw - 12 - aw : sx + 12, iy = sy + (sh - aw) / 2;
                int hv = hovered(HIT_BIND_STEP, i, k ? 1 : -1);
                if (hv) ui_box(c, ix, iy, aw, aw, aw / 2, on_state ? VSOFT : LILAC, 0, 0);
                ui_icon(c, k ? ICON_NEXT : ICON_PREV, ix + 4, iy + 4, aw - 8, 2.6f,
                        on_state ? (hv ? SNOW : ON_DARK) : (hv ? PLUM : MUTED));
                hit_add(k ? sx + sw - 16 - aw : sx, sy, 16 + aw, sh, HIT_BIND_STEP, i, k ? 1 : -1);
            }
            continue;
        }
        if (r->value && !strcmp(r->group, "KEYS FOR")) {  /* which set the page shows: one coloured pill, its name */
            const char *set = !strcmp(r->cells[0], "BATTLES") ? "Battles" : "Exploring";
            /* ‹ name ›: the arrows inside the pill, as the value Left / Right change */
            float sx = x + cw[0] + 22, sh = rh - 16, sy = y + 8, aw = 24;
            float nw = ui_text_w(UI_GOTHIC_700, 15, "Exploring"), sw = 16 + aw + nw + aw + 16;  /* as wide for both */
            ui_box(c, sx, sy, sw, sh, sh / 2, PLUM, 0, 0);
            ui_text_in(c, UI_GOTHIC_700, 15, sx + 16 + aw, sy, nw, sh, 1, set, SNOW);
            for (int k = 0; k < 2; k++) {  /* ‹ and ›, a lighter circle behind under the mouse */
                float ix = k ? sx + sw - 12 - aw : sx + 12, iy = sy + (sh - aw) / 2;
                int hv = hovered(HIT_BIND_STEP, i, k ? 1 : -1);
                if (hv) ui_box(c, ix, iy, aw, aw, aw / 2, VSOFT, 0, 0);
                ui_icon(c, k ? ICON_NEXT : ICON_PREV, ix + 4, iy + 4, aw - 8, 2.6f, hv ? SNOW : ON_DARK);
                hit_add(k ? sx + sw - 16 - aw : sx, sy, 16 + aw, sh, HIT_BIND_STEP, i, k ? 1 : -1);
            }
            continue;
        }
        if (r->value) {  /* a setting: its value in the keyboard column */
            /* ‹ value ›: the arrows either side of the value (as the set's pill), Left / Right; a box under the mouse */
            sentence_case(r->cells[0], t, sizeof t);
            float lx = x + cw[0] + 14, vw = 110, by = y + (rh - 26) / 2;
            ui_text_in(c, UI_GOTHIC_500, 17, lx + 30, y, vw, rh, 1, t, NIGHT);
            for (int k = 0; k < 2; k++) {
                float bx = k ? lx + 30 + vw + 4 : lx;
                int hv = hovered(HIT_BIND_STEP, i, k ? 1 : -1);
                if (hv) ui_box(c, bx, by, 26, 26, 7, LILAC, 0, 0);
                ui_icon(c, k ? ICON_NEXT : ICON_PREV, bx + 4, by + 4, 18, 2.4f, PLUM);
                hit_add(bx - 2, y, 30, rh, HIT_BIND_STEP, i, k ? 1 : -1);
            }
            continue;
        }
        /* the two keys, then the controller input: each a chip when chosen */
        for (int k = 0; k < 3; k++) {
            float kx = k < 2 ? x + cw[0] + 22 : x + cw[0] + cw[1] + 22;
            if (k == 1) {  /* after the first key: "E, Enter" */
                kx += cell_text(NULL, kx, y, rh, r->cells[0], 0);
                ui_text_in(c, UI_GOTHIC_500, 17, kx, y, 0, rh, 0, ",", MUTED);
                kx += ui_text_w(UI_GOTHIC_500, 17, ", ") + 6;
            }
            const char *cell = r->cells[k];
            if (on && col == k && bindings_capturing()) cell = "PRESS...";
            float tw = cell_text(NULL, kx, y, rh, cell, 0);
            hit_add(kx - 10, y, tw + 20, rh, HIT_BIND, i, k);
            if (on && col == k && !r->disabled) ui_box(c, kx - 10, y + 8, tw + 20, rh - 16, 8, GOLD, 0, 0);
            cell_text(c, kx, y, rh, cell,
                      r->disabled ? MUTED : r->conflict[k] ? WINE : !strcmp(r->cells[k], "-") ? MUTED : INK);
        }
    }
    /* "WHAT IT IS|HOW TO CHANGE IT" (bindings.h BIND_HELP_SPLIT); a game input's first part is empty: its actions,
     * from the F2 sheet's names (kv_action), the set this page shows first */
    char *split = strchr(help, BIND_HELP_SPLIT), what[400], how[400];
    if (split) *split = 0;
    prose_case(help, what, sizeof what);
    prose_case(split ? split + 1 : "", how, sizeof how);
    const BindRow *sel = row >= 0 && row < n ? &rows[row] : NULL;
    if (!*what && sel && sel->id) {
        int first = bindings_set_shown() ? 1 : 0;
        const char *f = kv_action(0, sel->id), *b = kv_action(1, sel->id), *s = kv_action(2, sel->id);
        size_t o = 0;
        if (f && b && !strcmp(f, b)) {
            o += (size_t)snprintf(what + o, sizeof what - o, "Exploring and battles: %s.", f);
        } else {
            const char *label[2] = {"Exploring", "Battles"}, *act[2] = {f, b};
            for (int k = 0; k < 2; k++) {
                int m = k ? !first : first;
                if (act[m] && o < sizeof what)
                    o += (size_t)snprintf(what + o, sizeof what - o, "%s%s: %s.", o ? " " : "", label[m], act[m]);
            }
        }
        if (s && o < sizeof what) snprintf(what + o, sizeof what - o, "%sCutscenes: %s.", o ? " " : "", s);
    }
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x, top + th + 22, w, hlh, 0, what, MUTED);
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x, top + th + 22 + 3 * hlh + 4, w, hlh, 0, how, MUTED);
}

/* A choice chip (the design's chipStyle): the chosen one filled; ringed in gold where the focus is. */
static void chip(const UiCanvas *c, float x, float y, float w, const char *label, int on, int ring) {
    if (ring) ui_box(c, x - 4, y - 4, w + 8, 60, 15, RING, 0, 0);
    ui_box(c, x, y, w, 52, 12, on ? PLUM : WHITE, 2, on ? PLUM : LINE);
    ui_text_in(c, UI_GOTHIC_700, 17, x, y, w, 52, 1, label, on ? SNOW : PLUM);
}

/* A slider of the Overlay page: the track, its filled part, the thumb, the value at the right. */
static void perf_slider(const UiCanvas *c, float x, float y, float w, const char *label, float lh, int opacity, int ring) {
    float frac;
    char text[16];
    settings_hud_slider(opacity, &frac, text, sizeof text);
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, label, PLUM);
    y += lh + 10;
    ui_box(c, x, y, w, 52, 12, WHITE, ring ? 3 : 2, ring ? RING : LINE);
    float tx = x + 18, tw = w - 18 - 16 - 54 - 14;
    hit_add(tx, y, tw, 52, HIT_SLIDER, opacity, 0);  /* the track, the box's height */
    ui_box(c, tx, y + 23, tw, 6, 3, LINE, 0, 0);
    if (frac > 0) ui_box(c, tx, y + 23, tw * frac, 6, 3, PLUM, 0, 0);
    ui_box(c, tx + tw * frac - 10, y + 16, 20, 20, 10, PLUM, 2, WHITE);
    ui_text_in(c, UI_ROUNDED_700, 16, x + w - 16 - 54, y, 54, 52, 2, text, INK);
}

/* The preview's picture: the paused game at its true size, the part where the HUD sits (the chosen corner), so the
 * opacity slider shows what the card will really be over. */
static void perf_preview_picture(const UiCanvas *c, const GameMenuInfo *in, float x, float y, float w, float h, int corner) {
    int pw = (int)(w * c->s), ph = (int)(h * c->s);
    if (!in->picture || pw > in->picture_w || ph > in->picture_h) {
        ui_box(c, x, y, w, h, 16, NIGHT, 0, 0);
        return;
    }
    uint8_t *crop = malloc((size_t)pw * ph * 4);
    if (!crop) {
        ui_box(c, x, y, w, h, 16, NIGHT, 0, 0);
        return;
    }
    int ox = corner & 1 ? in->picture_w - pw : 0, oy = corner >> 1 ? in->picture_h - ph : 0;
    for (int j = 0; j < ph; j++)
        memcpy(crop + (size_t)j * pw * 4, in->picture + ((size_t)(oy + j) * in->picture_w + ox) * 4, (size_t)pw * 4);
    ui_image(c, crop, pw, ph, x, y, w, h, 16);
    free(crop);
}

/* The performance page (design canvas: Overlay v2, the corner on a small screen): how the HUD shows while playing, its
 * corner, the detailed one's parts, a preview of the HUD over the paused game, its scale and opacity. */
static void draw_perf_page(const UiCanvas *c, const GameMenuInfo *in) {
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Overlay", NIGHT);
    float y = 76 + 34 * 1.25f + 24, x = 488, lh = 16 * 1.45f;
    int mode = window_hud_mode(), corner = window_hud_corner();
    unsigned parts = window_hud_parts();
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "Show while playing", PLUM);
    y += lh + 10;
    static const char *const modes[3] = {"Off", "Compact", "Detailed"};
    float lw = 1024 - 360 - 32;
    float mw = (lw - 2 * 10) / 3;
    for (int i = 0; i < 3; i++) hit_add(x + (float)i * (mw + 10), y, mw, 52, HIT_MODE, i, 0);
    for (int i = 0; i < 3; i++)
        chip(c, x + (float)i * (mw + 10), y, mw, modes[i], perf_modes[i] == mode, focus && prow == 0 && perf_modes[i] == mode);
    y += 52 + 22;
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "Corner", PLUM);
    y += lh + 10;
    static const char *const corners[4] = {"Top left", "Top right", "Bottom left", "Bottom right"};
    {  /* a small screen with the four corners on it, and the chosen one's name */
        int ring = focus && prow == 1;
        ui_box(c, x, y, lw, 124, 12, WHITE, ring ? 3 : 2, ring ? RING : LINE);
        float sx = x + 18, sy = y + 12;
        ui_box(c, sx, sy, 176, 100, 10, LINE, 0, 0);
        hit_add(x, y, lw, 124, HIT_CORNER_BOX, 0, 0);
        for (int i = 0; i < 4; i++)  /* each half of the small screen picks its corner */
            hit_add(sx + (i & 1 ? 88 : 0), sy + (i >> 1 ? 50 : 0), 88, 50, HIT_CORNER, i, 0);
        for (int i = 0; i < 4; i++)
            ui_box(c, sx + (i & 1 ? 176 - 6 - 58 : 6), sy + (i >> 1 ? 100 - 6 - 36 : 6), 58, 36, 7, i == corner ? PLUM : WHITE, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 17, sx + 176 + 24, y, 0, 124, 0, corners[corner & 3], PLUM);
    }
    y += 124 + 22;
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "In the detailed overlay", PLUM);
    y += lh + 10;
    static const char *const names[4] = {"Frame rate", "Frame time graph", "GPU, CPU and RAM", "Frame generation"};
    for (int i = 0; i < 4; i++, y += 52 + 10) {
        int on = parts >> i & 1, ring = focus && prow == 2 + i, off = mode != 1;
        ui_box(c, x, y, lw, 52, 12, WHITE, ring ? 3 : 2, ring ? RING : LINE);
        hit_add(x, y, lw, 52, HIT_PART, i, 0);
        ui_box(c, x + 18, y + 13, 26, 26, 7, on ? PLUM : WHITE, 2, on ? PLUM : EDGE);
        if (on) ui_text_in(c, UI_ROUNDED_800, 16, x + 18, y + 13, 26, 26, 1, "\xe2\x9c\x93", SNOW);  /* ✓ */
        ui_text_in(c, UI_GOTHIC_700, 17, x + 18 + 26 + 14, y, 0, 52, 0, names[i], off ? MUTED : PLUM);
    }
    /* the preview: the HUD itself, drawn now, over the paused game; one height for every form, so the sliders below
     * stay put and the two columns end level */
    float px = 488 + lw + 32, py = 76 + 34 * 1.25f + 24, bh = 310;
    ui_text(c, UI_GOTHIC_700, 16, px, py, lh, "Preview", PLUM);
    py += lh + 10;
    static uint8_t img[1024 * 1024 * 4], tight[1024 * 1024 * 4];
    int iw = 0, ih = 0;
    if (mode) window_hud_preview(img, 1024, 1024, c->s, &iw, &ih);
    perf_preview_picture(c, in, px, py, 360, bh, corner);
    if (iw > 0 && ih > 0) {
        for (int j = 0; j < ih; j++) memcpy(tight + (size_t)j * iw * 4, img + (size_t)j * 1024 * 4, (size_t)iw * 4);
        /* pixel for pixel, so it is sharp and as big as it will be; centred in the box */
        float sw = (float)iw / c->s, sh = (float)ih / c->s;
        ui_image(c, tight, iw, ih, px + (360 - sw) * 0.5f, py + (bh - sh) * 0.5f, sw, sh, 0);
    } else {
        ui_box_alpha(c, px, py, 360, bh, 16, NIGHT, 0.6f);
        ui_text_in(c, UI_GOTHIC_700, 16, px, py, 360, bh, 1, "Off while playing", SNOW);
    }
    py += bh + 22;
    perf_slider(c, px, py, 360, "Scale", lh, 0, focus && prow == 6);
    perf_slider(c, px, py + lh + 10 + 52 + 22, 360, "Opacity", lh, 1, focus && prow == 7);
}

/* ---- the Online page (the "Cards" layout): its cards are outlined on the page's paper, their rows white */
static void online_card(const UiCanvas *c, float x, float y, float w, float h, const char *title, float lh) {
    ui_box(c, x, y, w, h, 16, PAPER, 2, LINE);
    ui_text(c, UI_GOTHIC_700, 16, x + 24, y + 18, lh, title, PLUM);
}

/* a typed value in a box with an Edit button (a code, the player's name); greyed out when off */
static void online_edit_box(const UiCanvas *c, float x, float y, float w, float h, const char *text, int off, int item) {
    int ring = focus && orow == item && !off;
    float ew = 22 + 8 + ui_text_w(UI_ROUNDED_800, 16, "Edit") + 2 * 16, ex = x + w - ew;
    ui_box(c, x, y, ex - 10 - x, h, 10, WHITE, ring ? 3 : 2, ring ? RING : LINE);
    ui_text_in(c, UI_ROUNDED_700, 18, x + 16, y, ex - 10 - x - 32, h, 0, text, off ? MUTED : NIGHT);
    int hv = !off && hovered(HIT_ON_ITEM, item, -1);
    ui_box(c, ex, y, ew, h, 10, hv ? LILAC : SOFT, 2, hv ? PLUM : LINE);
    ui_icon(c, ICON_EDIT, ex + 16, y + (h - 22) / 2, 22, 2.2f, off ? MUTED : PLUM);
    ui_text_in(c, UI_ROUNDED_800, 16, ex + 16 + 22 + 8, y, 0, h, 0, "Edit", off ? MUTED : PLUM);
    hit_add(x, y, w, h, HIT_ON_ITEM, item, -1);
}

/* two chips side by side, the chosen one filled; the ring on it while the control has the focus */
static void online_switch(const UiCanvas *c, float x, float y, float w, const char *a, const char *b, int on, int item,
                          int off) {
    float cw = (w - 10) / 2;
    for (int k = 0; k < 2; k++) {
        float cx = x + (float)k * (cw + 10);
        if (off) {
            ui_box(c, cx, y, cw, 52, 12, WHITE, 2, LINE);
            ui_text_in(c, UI_GOTHIC_700, 17, cx, y, cw, 52, 1, k ? b : a, MUTED);
            continue;
        }
        chip(c, cx, y, cw, k ? b : a, on == k, focus && orow == item && on == k);
        hit_add(cx, y, cw, 52, HIT_ON_ITEM, item, k);
    }
}

static void draw_online_page(const UiCanvas *c, const GameMenuInfo *in) {
    (void)in;
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Online", NIGHT);
    float lh = 16 * 1.45f, x = 488, w = 1024, y = 76 + 34 * 1.25f + 6;
    ui_text(c, UI_GOTHIC_500, 15, x, y, 15 * 1.45f,
            "The game's Online button plays through our servers. Local play stays local.", MUTED);
    y += 15 * 1.45f + 18;

    /* the servers, with their ping (measured when the page opens, again every 20 s while it shows) */
    int n = settings_online_servers(), chosen = settings_online_server();
    static int64_t pinged;
    int64_t now = host_now_ns();
    if (!pinged || now - pinged > 20000000000LL) {
        const char *addrs[4];
        for (int i = 0; i < n && i < 4; i++) addrs[i] = settings_online_server_address(i);
        relay_ping_start(addrs, n);
        pinged = now;
    }
    int best = -1;
    for (int i = 0; i < n; i++) {
        int ms = relay_ping_ms(i);
        if (ms >= 0 && (best < 0 || ms < relay_ping_ms(best))) best = i;
        if (ms == -1) changed();  /* still measuring: draw again until the numbers are in */
    }
    float hs = 18 + lh + 10 + (float)n * 52 + (float)(n - 1) * 10 + 20;
    online_card(c, x, y, w, hs, "Server", lh);
    float ry = y + 18 + lh + 10;
    for (int i = 0; i < n; i++, ry += 52 + 10) {
        int item = ON_SERVER0 + i, on = i == chosen, ring = focus && orow == item, ms = relay_ping_ms(i);
        float rx = x + 24, rw = w - 48;
        ui_box(c, rx, ry, rw, 52, 12, WHITE, ring ? 3 : 2, ring ? RING : LINE);
        ui_box(c, rx + 18, ry + 15, 22, 22, 11, WHITE, 2, on ? PLUM : EDGE);
        if (on) ui_box(c, rx + 23, ry + 20, 12, 12, 6, PLUM, 0, 0);
        const char *name = settings_online_server_name(i);
        ui_text_in(c, UI_GOTHIC_700, 17, rx + 54, ry, 0, 52, 0, name, PLUM);
        ui_text_in(c, UI_GOTHIC_500, 15, rx + 54 + ui_text_w(UI_GOTHIC_700, 17, name) + 14, ry, 0, 52, 0,
                   settings_online_server_region(i), MUTED);
        char p[32];
        if (ms >= 0) snprintf(p, sizeof p, "%d ms", ms);
        else snprintf(p, sizeof p, "%s", ms == -1 ? "Measuring..." : "No answer");
        float pw = ui_text_w(UI_GOTHIC_700, 16, p);
        ui_text_in(c, UI_GOTHIC_700, 16, rx + rw - 20 - pw, ry, pw, 52, 0, p, ms == -2 ? WINE : NIGHT);
        if (i == best && n > 1) {
            float tw = tag(c, 0, 0, "Nearest", 0, 0, 1);
            tag(c, rx + rw - 20 - pw - 12 - tw, ry + 17, "Nearest", GOLD, ON_GOLD, 0);
        }
        hit_add(rx, ry, rw, 52, HIT_ON_ITEM, item, -1);
    }
    y += hs + 20;

    /* Ark Swap and Blasters: who sees your room, and the code for Private */
    float cw = (w - 24) / 2, ha = 18 + lh + 10 + 52 + 14 + 48 + 12 + 15 * 1.45f + 18;
    for (int b = 0; b < 2; b++) {
        float cx = x + (float)b * (cw + 24), ix = cx + 24, iw = cw - 48;
        int priv = settings_online_private(b);
        online_card(c, cx, y, cw, ha, b ? "Blasters" : "Ark Swap", lh);
        float iy = y + 18 + lh + 10;
        online_switch(c, ix, iy, iw, "Public", "Private", priv, b ? ON_BLAST : ON_SWAP, 0);
        iy += 52 + 14;
        const char *code = settings_online_code(b);
        ui_text_in(c, UI_GOTHIC_700, 15, ix, iy, 0, 48, 0, "Code", priv ? PLUM : MUTED);
        online_edit_box(c, ix + 56, iy, iw - 56, 48, !priv ? "Not needed" : *code ? code : "Not set", !priv,
                        b ? ON_BLAST_CODE : ON_SWAP_CODE);
        iy += 48 + 12;
        const char *note = b ? "Coming soon: Blasters' Online button isn't connected yet."
                         : priv ? "Only players with the same code see your room."
                                : "Everyone on the server sees your room.";
        ui_text(c, UI_GOTHIC_500, 14, ix, iy, 15 * 1.45f, note, MUTED);
    }
    y += ha + 20;

    /* the profile: the name the others see, and Discord status */
    float hp = 18 + lh + 10 + 52 + 20;
    online_card(c, x, y, w, hp, "Profile", lh);
    float py = y + 18 + lh + 10, px = x + 24;
    ui_text_in(c, UI_GOTHIC_700, 15, px, py, 0, 52, 0, "Player name", PLUM);
    online_edit_box(c, px + 120, py, 440, 52, settings_player_name(), 0, ON_NAME);
    int doff = discord_disabled() != NULL;
    float dx = x + w - 24 - 250;
    ui_text_in(c, UI_GOTHIC_700, 15, dx - 140, py, 0, 52, 0, "Discord status", doff ? MUTED : PLUM);
    online_switch(c, dx, py, 250, "Off", "On", discord_enabled(), ON_DISCORD, doff);
}

/* A gold button (page 2's Send, page 1's Next) or a white one; ringed where the focus is. Returns its width. */
static float bug_button(const UiCanvas *c, float x, float y, const char *label, int gold, int ring, int measure) {
    float w = ui_text_w(UI_ROUNDED_800, 18, label) + 2 * 28;
    if (measure) return w;
    if (ring) ui_box(c, x - 4, y - 4, w + 8, 64, 16, RING, 0, 0);
    ui_box(c, x, y, w, 56, 12, gold ? GOLD : WHITE, gold ? 0 : 2, LINE);
    ui_text_in(c, UI_ROUNDED_800, 18, x, y, w, 56, 1, label, gold ? ON_GOLD : PLUM);
    return w;
}

/* Report a bug (canvas board D): page 1 what's wrong, what happened, how often; page 2 the picture and the ticks */
static void draw_bug_page(const UiCanvas *c, const GameMenuInfo *in) {
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    float th = 34 * 1.25f, x = 488, w = 1024, lh = 16 * 1.45f;
    ui_text(c, UI_ROUNDED_800, 34, x, 76, th, "Report a bug", NIGHT);
    if (bug_page < 2) ui_text_in(c, UI_ROUNDED_700, 16, x, 76, w, th, 2, bug_page ? "2 of 2" : "1 of 2", MUTED);
    float y = 76 + th + 24;
    int on = focus;  /* rings only while the page has the focus */
    if (bug_page == 2) {  /* saved: where the zip is, what comes next, three buttons */
        ui_box(c, x, y, w, 96, 14, TAGFILL, 0, 0);
        ui_box(c, x + 24, y + 24, 48, 48, 24, PLUM, 0, 0);
        ui_text_in(c, UI_ROUNDED_800, 26, x + 24, y + 24, 48, 48, 1, "\xe2\x9c\x93", SNOW);  /* ✓ */
        ui_text(c, UI_ROUNDED_800, 22, x + 96, y + 16, 22 * 1.3f, "Report saved", NIGHT);
        const char *name = strrchr(bug_zip, '\\') ? strrchr(bug_zip, '\\') + 1 : strrchr(bug_zip, '/') ? strrchr(bug_zip, '/') + 1 : bug_zip;
        char where[400];
        snprintf(where, sizeof where, "reports\\%s", name);
        ui_text(c, UI_GOTHIC_500, 16, x + 96, y + 16 + 22 * 1.3f + 4, lh, where, INK);
        y += 96 + 30;
        float blh = 18 * 1.6f;
        ui_text(c, UI_GOTHIC_500, 18, x, y, blh,
                bug_sent_github ? "GitHub opened in your browser with your answers filled in."
                    : "Open GitHub to post it, or share the zip another way.", INK);
        ui_text(c, UI_GOTHIC_500, 18, x, y + blh, blh, "Drag the zip into the last box and post it.", INK);
        float by = y + 2 * blh + 30, bx = x;
        static const char *const labels[3] = {"Show the zip", "Open GitHub again", "Back to the game"};
        for (int i = 0; i < 3; i++) {
            const char *l = i == 1 && !bug_sent_github ? "Open GitHub" : labels[i];
            float bw = bug_button(c, 0, 0, l, i == 0, 0, 1);
            hit_add(bx, by, bw, 56, HIT_BUG_DONE, i, 0);
            bug_button(c, bx, by, l, i == 0, on && bug_done_btn == i, 0);
            bx += bw + 12;
        }
        return;
    }
    if (bug_page == 0) {
        ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "What's wrong?", PLUM);
        y += lh + 10;
        float cw = (w - (BUG_KINDS - 1) * 10) / BUG_KINDS;
        for (int i = 0; i < BUG_KINDS; i++) {
            float cx = x + (float)i * (cw + 10);
            hit_add(cx, y, cw, 52, HIT_BUG_KIND, i, 0);
            chip(c, cx, y, cw, bug_kinds[i], bug_kind == i, on && brow == 0 && (bug_kind == i || (bug_kind < 0 && i == 0)));
        }
        y += 52 + 26;
        ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "What happened?", PLUM);
        ui_text(c, UI_GOTHIC_500, 15, x + ui_text_w(UI_GOTHIC_700, 16, "What happened?") + 12, y, lh,
                "You can also write it on GitHub", MUTED);
        y += lh + 10;
        float bh = 200;
        int typing = textin_has(&bug_text), ring = on && brow == 1;
        if (ring) ui_box(c, x - 4, y - 4, w + 8, bh + 8, 16, RING, 0, 0);
        ui_box(c, x, y, w, bh, 12, WHITE, typing ? 3 : 2, typing ? PLUM : LINE);
        hit_add(x, y, w, bh, HIT_BUG_TEXT, 0, 0);
        char text[TEXTIN_MAX];
        int cur, sel;
        textin_read(&bug_text, text, sizeof text, &cur, &sel);
        float tlh = 18 * 1.5f;
        if (!text[0] && !typing) {
            ui_text(c, UI_GOTHIC_500, 18, x + 20, y + 16, tlh, "For example: Whisper has no shadow on the school field.",
                    MUTED);
            if (ring)
                ui_text(c, UI_GOTHIC_700, 15, x + 20, y + bh - 16 - lh, lh,
                        in->pad ? "Press A to type with a keyboard" : "Press Enter to type", PLUM);
        } else {
            ui_text_edit(c, UI_GOTHIC_500, 18, x + 20, y + 16, w - 40, tlh, (int)((bh - 32) / tlh), text,
                         typing ? cur : -1, typing ? sel : -1, INK, PLUM, RING);
        }
        if (typing) {
            draw_text_box[0] = x, draw_text_box[1] = y, draw_text_box[2] = w, draw_text_box[3] = bh;
            draw_text_caret = ui_text_caret_x();
            draw_text_has = 1;
        }
        y += bh + 26;
        ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "How often?", PLUM);
        y += lh + 10;
        float ow = 200;
        for (int i = 0; i < BUG_OFTENS; i++) {
            float ox = x + (float)i * (ow + 10);
            hit_add(ox, y, ow, 52, HIT_BUG_OFTEN, i, 0);
            chip(c, ox, y, ow, bug_oftens[i], bug_often == i, on && brow == 2 && (bug_often == i || (bug_often < 0 && i == 0)));
        }
        float nw = bug_button(c, 0, 0, "Next", 1, 0, 1), ny = 804 - 40 - 56;
        hit_add(x + w - nw, ny, nw, 56, HIT_BUG_NEXT, 0, 0);
        bug_button(c, x + w - nw, ny, "Next", 1, on && brow == 3, 0);
        return;
    }
    /* page 2: the paused picture on the left, the ticks on the right */
    float pw = 480, ph = 270;
    if (in->picture) ui_image(c, in->picture, in->picture_w, in->picture_h, x, y, pw, ph, 16);
    else ui_box(c, x, y, pw, ph, 16, NIGHT, 0, 0);
    if (!(bug_attach & 1)) {
        ui_box_alpha(c, x, y, pw, ph, 16, NIGHT, 0.6f);
        ui_text_in(c, UI_GOTHIC_700, 16, x, y, pw, ph, 1, "Not attached", SNOW);
    }
    float rx = x + pw + 32, rw = w - pw - 32, ry = y;
    ui_text(c, UI_GOTHIC_700, 16, rx, ry, lh, "Attach", PLUM);
    ry += lh + 10;
    static const char *const ticks[3] = {"Screenshot", "Game log and settings", "My save"};
    for (int i = 0; i < 3; i++, ry += 56 + 10) {
        int tick = bug_attach >> i & 1, ring = on && brow == i;
        if (ring) ui_box(c, rx - 4, ry - 4, rw + 8, 64, 16, RING, 0, 0);
        ui_box(c, rx, ry, rw, 56, 12, WHITE, 2, LINE);
        hit_add(rx, ry, rw, 56, HIT_BUG_TICK, i, 0);
        ui_box(c, rx + 18, ry + 15, 26, 26, 7, tick ? PLUM : WHITE, 2, tick ? PLUM : EDGE);
        if (tick) ui_text_in(c, UI_ROUNDED_800, 16, rx + 18, ry + 15, 26, 26, 1, "\xe2\x9c\x93", SNOW);  /* ✓ */
        float lx = rx + 18 + 26 + 14;
        lx += ui_text(c, UI_GOTHIC_700, 17, lx, ry, 56, ticks[i], PLUM);
        if (i == 2) ui_text(c, UI_GOTHIC_500, 15, lx + 8, ry, 56, "so we can load the same place", MUTED);
    }
    y += ph + 30;
    ui_text_wrap(c, UI_GOTHIC_500, 16, 0, x, y, w, lh, 0,
                 "Send to GitHub saves a zip and opens GitHub with your answers filled in. Drag the zip in and post it.",
                 MUTED);
    float by = 804 - 40 - 56;
    hit_add(x, by, bug_button(c, 0, 0, "Back", 0, 0, 1), 56, HIT_BUG_BTN, 0, 0);
    bug_button(c, x, by, "Back", 0, on && brow == 3 && bug_btn == 0, 0);
    float sw = bug_button(c, 0, 0, "Send to GitHub", 1, 0, 1), ow2 = bug_button(c, 0, 0, "Save only", 0, 0, 1);
    float sx = x + w - sw, ox2 = sx - 12 - ow2;
    hit_add(ox2, by, ow2, 56, HIT_BUG_BTN, 1, 0);
    hit_add(sx, by, sw, 56, HIT_BUG_BTN, 2, 0);
    bug_button(c, ox2, by, "Save only", 0, on && brow == 3 && bug_btn == 1, 0);
    bug_button(c, sx, by, "Send to GitHub", 1, on && brow == 3 && bug_btn == 2, 0);
}

/* The preview's bar (canvas board B): over the running game at the bottom, the section's name and the frame rate,
 * its rows (the chosen one ringed in gold, the value between arrows), the chosen row's description in a dark box,
 * the key hints. Hidden (GM_ALT): only a chip saying how to show it again. Sets pv_rect, the part the window shows. */
static float edge;  /* gamemenu_draw's E: the strip above and below the design in a picture narrower than 16:9 */

static void draw_preview(const UiCanvas *c, const GameMenuInfo *in) {
    float W = (float)c->w / c->s, H = 900 + edge;  /* the bar sits at the picture's bottom */
    SettingRow rows[48];
    int n = settings_player_rows(rows, 48);
    if (n <= 0) return;
    int cur = srow < n ? srow : n - 1;
    int first = cur, last = cur;
    if (!look_span(rows, n, cur, &first, &last)) return;  /* the rows to see (Advanced: not Discord status) */
    char t[200];
    const char *alt = in->pad ? alt_button(in) : "H";
    if (preview_hidden) {
        float hw = 12 + 30 + 10 + ui_text_w(UI_GOTHIC_400, 16, "Show bar") + 16;
        ui_box(c, 24, H - 24 - 56, hw, 56, 16, NIGHT, 0, 0);
        hint(c, 36, H - 24 - 56, alt, "Show bar");
        pv_rect[0] = 24, pv_rect[1] = H - 24 - 56, pv_rect[2] = hw, pv_rect[3] = 56;
        return;
    }
    float paper = 214, top = H - 56 - paper - 4, py = top + 4;
    pv_rect[0] = 0, pv_rect[1] = top, pv_rect[2] = W, pv_rect[3] = H - top;
    ui_rect(c, 0, top, W, 4, GOLD, 1);
    ui_rect(c, 0, py, W, paper, PAPER, 1);
    /* the left: one card, level with the rows and the description (owner: nothing floating): Preview and the
     * section at its top, a strip at its bottom with the frame rate and which graphics section this is */
    float cx = 40, cy = py + 14, cw = 240, ch = paper - 28, sh = 40;
    ui_box(c, cx, cy, cw, ch, 14, WHITE, 2, LINE);
    tag(c, cx + 16, cy + 14, "Preview", GOLD, ON_GOLD, 0);
    sentence_case(rows[cur].category, t, sizeof t);
    ui_text_wrap(c, UI_ROUNDED_800, 20, 0, cx + 16, cy + 14 + 18 + 8, cw - 32, 20 * 1.25f, 0, t, NIGHT);
    int nsec = 0, isec = 0;
    for (int i = 0; i < n; i++) {
        int f, l;
        if ((i && !strcmp(rows[i].category, rows[i - 1].category)) || !look_span(rows, n, i, &f, &l)) continue;
        nsec++;
        if (i <= cur) isec = nsec;
    }
    ui_box(c, cx + 2, cy + ch - 2 - sh, cw - 4, sh, 12, SOFT, 0, 0);
    ui_rect(c, cx + 2, cy + ch - 2 - sh, cw - 4, 12, SOFT, 1);  /* square top corners: a strip, not a pill */
    if (in->shown_fps > 0) snprintf(t, sizeof t, "%.0f FPS (%.0f)", in->shown_fps, in->fps);
    else snprintf(t, sizeof t, "%.0f FPS", in->fps);
    ui_text_in(c, UI_ROUNDED_800, 17, cx + 16, cy + ch - 2 - sh, 0, sh, 0, t, PLUM);
    snprintf(t, sizeof t, "%d / %d", isec, nsec);
    ui_text_in(c, UI_GOTHIC_700, 14, cx + 16, cy + ch - 2 - sh, cw - 32, sh, 2, t, MUTED);
    /* the rows: up to four, the chosen one among them */
    float rx = 304, dw = 440, rw = W - 40 - dw - 24 - rx, rh = 42, gap = 6;
    int shown = 4, from = first;
    if (last - first + 1 > shown) {
        from = cur - 1 < first ? first : cur - 1;
        if (from > last - shown + 1) from = last - shown + 1;
    }
    float ry = py + 14;
    for (int i = from; i <= last && i < from + shown; i++, ry += rh + gap) {
        const SettingRow *r = &rows[i];
        int on = i == cur, off = r->disabled != NULL;
        ui_box(c, rx, ry, rw, rh, 12, WHITE, on ? 3 : 2, on ? RING : LINE);
        hit_add(rx, ry, rw, rh, HIT_ROW, i, 0);
        sentence_case(r->name, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_500, 17, rx + 18, ry, 0, rh, 0, t, off ? MUTED : INK);
        float nx = rx + rw - 10 - 30, vx = nx - 6 - 170, px = vx - 6 - 30, by = ry + (rh - 30) / 2;
        if (r->slider) {  /* SHARPENING: a slider in the arrows' and the value's place, reaching left into the row's
                           * empty middle (the arrows' 242 alone left the track a stub) */
            float sw = 400, sx = nx + 30 - sw, lx = rx + 18 + ui_text_w(UI_GOTHIC_500, 17, t) + 24;
            if (sx < lx) sx = lx, sw = nx + 30 - sx;
            row_slider(c, r, i, sx, ry, sw, rh, 17, off);
            continue;
        }
        for (int k = 0; k < 2; k++) {  /* ‹ and ›; a box under the mouse */
            float bx = k ? nx : px;
            int hv = !off && hovered(k ? HIT_ROW_NEXT : HIT_ROW_PREV, i, 0);
            if (hv) ui_box(c, bx, by, 30, 30, 8, LILAC, 0, 0);
            ui_icon(c, k ? ICON_NEXT : ICON_PREV, bx + 5, by + 5, 20, 2.4f, off ? MUTED : PLUM);
            hit_add(bx, by, 30, 30, k ? HIT_ROW_NEXT : HIT_ROW_PREV, i, 0);
        }
        if (r->verbatim) snprintf(t, sizeof t, "%s", r->options[r->current] ? r->options[r->current] : "");
        else sentence_case(r->options[r->current] ? r->options[r->current] : "", t, sizeof t);
        ui_text_in(c, UI_ROUNDED_700, 17, vx, ry, 170, rh, 1, t, off ? MUTED : NIGHT);
    }
    /* the chosen row: name and value, its tags, then its note and what it does */
    const SettingRow *r = &rows[cur];
    float dx = W - 40 - dw, dy = py + 14, dh = paper - 28;
    ui_box(c, dx, dy, dw, dh, 14, NIGHT, 0, 0);
    char name[96], value[96], rest[200], desc[700], prose[700];
    sentence_case(r->name, name, sizeof name);
    if (r->verbatim) snprintf(value, sizeof value, "%s", r->options[r->current] ? r->options[r->current] : "");
    else sentence_case(r->options[r->current] ? r->options[r->current] : "", value, sizeof value);
    snprintf(t, sizeof t, "%s: %s", name, value);
    ui_text(c, UI_ROUNDED_800, 15, dx + 20, dy + 14, 15 * 1.5f, t, SNOW);
    int tags = note_tags(r->notes[r->current] ? r->notes[r->current] : "", rest, sizeof rest);
    float ty = dy + 14 + 15 * 1.5f + 4;
    if (tags) {
        float tx = dx + 20;
        if (tags & TAG_DEFAULT) tx += tag(c, tx, ty, "Switch default", TAGFILL, PLUM, 0) + 6;
        if (tags & TAG_RECOMMENDED) tag(c, tx, ty, "Recommended", GOLD, ON_GOLD, 0);
        ty += 18 + 6;
    }
    if (r->disabled) snprintf(desc, sizeof desc, "%s%s.", locked_on(r->disabled) ? "" : "OFF FOR NOW: ", r->disabled);
    else if (*rest) snprintf(desc, sizeof desc, "%s. %s", rest, r->help);
    else snprintf(desc, sizeof desc, "%s", r->help);
    prose_case(desc, prose, sizeof prose);
    ui_text_wrap(c, UI_GOTHIC_500, 14, 0, dx + 20, ty, dw - 40, 14 * 1.45f, 0, prose, ON_DARK);
    /* the hints */
    ui_rect(c, 0, H - 56, W, 56, NIGHT, 1);
    float hx = 40;
    if (in->pad) {
        hx += hint(c, hx, H - 56, "D-pad", "Setting and change") + 28;
        hx += hint(c, hx, H - 56, in->pad == 2 ? "L1 R1" : "LB RB", "Section") + 28;
        hx += hint(c, hx, H - 56, alt, "Hide bar") + 28;
        hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Back to settings");
    } else {
        hx += hint(c, hx, H - 56, "W S", "Setting") + 28;
        hx += hint(c, hx, H - 56, "A D", "Change") + 28;
        hx += hint(c, hx, H - 56, "Q E", "Section") + 28;
        hx += hint(c, hx, H - 56, "H", "Hide bar") + 28;
        hint(c, hx, H - 56, "Esc", "Back to settings");
    }
}

/* The New Game slot list in the Diary's look (gamemenu_diary_begin; owner, 2026-11-17: "make it actually match the
 * game's", the reference the game's own Diary, title > Continue, work/screenshots/frame_032363.bmp). The whole
 * screen, as the game's: a lilac page with a white header band ("Diary" and its icon), the slots as the Diary's rows
 * (purple plates fading out to the right, a square at their left, the name in white over a thin line, the date in
 * navy under it; the chosen one green with a pale edge and a dot beside it), and the chosen slot's page on the right
 * as the Diary's notebook (cream, a maroon cover showing at its left and bottom, rows of diamonds along its top and
 * bottom, the spiral's rings at the right, the labels in coloured pills with a line out to a dot). The keys along a
 * dark bar at the bottom, on the right. The game's portrait and party have no counterpart here: the slot's number
 * sits where the portrait is, and a note where the party is. Laid out from the 1920 x 1080 capture at 900 / 1080. */
enum { DY_BG = 0xCBB9DF, DY_BG_LIGHT = 0xDACCE9, DY_BAND = 0xB79FD3, DY_STRIP = 0xE4D9EF, DY_HEAD = 0x5E3E8E,
       DY_SUB = 0x6B5A85, DY_BAR = 0x4D3566, DY_BAR_LINE = 0xA88FC6, DY_PLATE = 0x9682C3, DY_SQUARE = 0x7A66A6,
       DY_CHOSEN = 0x5CCF55, DY_CHOSEN_HI = 0x9BE57F, DY_CHOSEN_EDGE = 0xD9F7CB, DY_SQUARE_ON = 0x3E9E3A,
       DY_INK = 0x4A3570, DY_INK_ON = 0x2F7A2A, DY_NAVY = 0x26245E, DY_NAVY_ON = 0x173E17, DY_RULE = 0xDCD2EC,
       DY_RULE_ON = 0xE6FAD9, DY_COVER = 0x7A2F3B, DY_PAPER = 0xFFF8F5, DY_PAPER_EDGE = 0xE9D7DB,
       DY_DIAMOND = 0xC6B3DF, DY_RING = 0xF7F3FA, DY_RING_SHADE = 0x6F5C84, DY_HOLE = 0x9C8AAE, DY_TITLE = 0x7B2D6F,
       DY_UNDER = 0x7B5AA6, DY_NOTE_CARD = 0xF5EDF7 };
#define DY_BAR_H 79.0f
#define DY_ARROW "<path d=\"M10 6L4 12l6 6M4 12h16\"/>"
#define DY_PENCIL "<path d=\"M4 20l1.2-4.8L15.5 4.9l3.6 3.6L8.8 18.8zM13.5 6.9l3.6 3.6\"/>"

/* The Diary drawn as a popup: the game's full-screen layout (1600 x 900 design units, laid out from the capture)
 * scaled down into a rounded panel in the middle, over the running game (owner, 2026-11-17: "dont need to be full
 * screen but the snippet needs to look like the real thing"). dy_canvas is the canvas with the layout's units. */
#define DY_K 0.9f
static UiCanvas dy_canvas(const UiCanvas *c, float W, float *offx, float *offy) {
    *offx = (W - 1600 * DY_K) / 2, *offy = (900 - 900 * DY_K) / 2;
    UiCanvas d = {c->px, c->w, c->h, c->s * DY_K, c->ox + *offx * c->s, c->oy + *offy * c->s};
    return d;
}

/* The page's parts that stay the same while it shows (on the layout's canvas): the background and its pattern, the
 * header band and its words, the bar at the bottom, the notebook with its cover, diamonds and spiral. */
static void diary_page_static(const UiCanvas *c) {
    float nx = 621, bt = 900 - DY_BAR_H;
    ui_box(c, 0, 0, 1600, 900, 24, DY_BG, 0, 0);
    /* the header: a band along the top with a lighter strip, the white band round at its right end */
    ui_box(c, 0, 0, 1600, 60, 24, DY_BAND, 0, 0);
    ui_rect(c, 0, 30, 1600, 57, DY_BAND, 1);
    ui_box(c, 300, 6, 1276, 22, 11, DY_STRIP, 0, 0);
    ui_box(c, 0, 0, 790, 79, 24, 0xFFFFFF, 0, 0);
    ui_rect(c, 0, 40, 60, 39, 0xFFFFFF, 1);
    ui_shape(c, "<path fill=\"#000\" d=\"M12 1L23 11V23H1V11Z\"/>", 52, 18, 52, 52, 24, 0x7FCB6E);  /* the icon: a green tag */
    ui_shape(c, "<path fill=\"#000\" d=\"M7 13H17V16H7Z\"/>", 52, 18, 52, 52, 24, 0xFFFFFF);
    float tw = ui_text(c, UI_ROUNDED_800, 40, 114, 24, 40 * 1.2f, "Diary", DY_HEAD);
    ui_text(c, UI_GOTHIC_700, 20, 114 + tw + 24, 40, 20 * 1.3f, dy_title, DY_SUB);
    /* the pattern: lighter checks right of the rows (the rows fade out into the plain colour), inside the panel */
    for (int j = 0; 87 + (float)j * 96 < bt - 4; j++)
        for (int i = 0; (float)i * 96 < 1600; i++)
            if ((i + j) % 2 && (float)i * 96 >= 620) {
                float y = 87 + (float)j * 96, h = y + 96 > bt - 4 ? bt - 4 - y : 96;
                float w = (float)i * 96 + 96 > 1600 ? 1600 - (float)i * 96 : 96;
                ui_rect(c, (float)i * 96, y, w, h, DY_BG_LIGHT, 0.5f);
            }
    /* the bar along the bottom, its corners round as the panel's */
    ui_rect(c, 0, bt - 4, 1600, 4, DY_BAR_LINE, 1);
    ui_rect(c, 0, bt, 1600, 40, DY_BAR, 1);
    ui_box(c, 0, bt, 1600, DY_BAR_H, 24, DY_BAR, 0, 0);
    /* the notebook: its cover under the page, out at the left and bottom; the diamonds; the spiral */
    ui_box(c, nx - 8, 129, 880, 630, 12, DY_COVER, 0, 0);
    ui_box(c, nx, 121, 878, 628, 8, DY_PAPER, 2, DY_PAPER_EDGE);
    for (float dx = nx + 18; dx < nx + 830; dx += 16)
        for (int k = 0; k < 2; k++) {
            float dy = k ? 727 : 129;
            ui_shape(c, "<path fill=\"#000\" d=\"M5 0L10 5L5 10L0 5Z\"/>", dx, dy, 10, 10, 10, DY_DIAMOND);
            ui_shape(c, "<path fill=\"#000\" d=\"M5 2.4L7.6 5L5 7.6L2.4 5Z\"/>", dx, dy, 10, 10, 10, DY_PAPER);
        }
    for (float ry = 150; ry < 700; ry += 47) {  /* a hole in the page, the ring through it and over the edge */
        ui_box(c, nx + 836, ry + 9, 9, 9, 4.5f, DY_HOLE, 0, 0);
        static const char ring[] = "<path stroke=\"#000\" stroke-width=\"4.5\" d=\"M6 5.5C17 1 27 6 27 15S17 29 6 24.5\"/>";
        ui_shape(c, ring, nx + 838, ry - 1, 32, 32, 32, DY_RING_SHADE);
        ui_shape(c, ring, nx + 836, ry - 3, 32, 32, 32, DY_RING);
    }
}

/* Those parts and the panel's shadow over the running game, drawn once and kept, then copied in. Drawing them took
 * about 230 ms at 4K, and the present thread draws the overlay while the game waits on its present: each move in the
 * list froze the game for a quarter of a second (owner, 2026-11-17; the log's "stall: window present 230 ms" at each
 * key). Only over the running game: there window.c clears the canvas first, so the kept pixels are what drawing them
 * again would give (over the paused picture they would carry that picture). The rows and the chosen slot's details
 * are drawn on top each time. Only the present thread draws, so no lock. */
static struct {
    uint8_t *px;
    int x, y, w, h;  /* the part of the canvas kept (live_rect, in image pixels) */
    int cw, ch;      /* the canvas it was drawn on */
    float s, ox, oy;
    char title[sizeof dy_title];
} dp_keep;

static void diary_page_kept(const UiCanvas *c, const UiCanvas *d, float offx, float offy) {
    int x0 = (int)(c->ox + live_rect[0] * c->s), y0 = (int)(c->oy + live_rect[1] * c->s);
    int x1 = (int)(c->ox + (live_rect[0] + live_rect[2]) * c->s) + 1, y1 = (int)(c->oy + (live_rect[1] + live_rect[3]) * c->s) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    if (x1 <= x0 || y1 <= y0) return;
    int w = x1 - x0, h = y1 - y0;
    if (dp_keep.px && dp_keep.cw == c->w && dp_keep.ch == c->h && dp_keep.s == c->s && dp_keep.ox == c->ox &&
        dp_keep.oy == c->oy && dp_keep.x == x0 && dp_keep.y == y0 && dp_keep.w == w && dp_keep.h == h &&
        !strcmp(dp_keep.title, dy_title)) {
        for (int j = 0; j < h; j++)
            memcpy(c->px + ((size_t)(y0 + j) * (size_t)c->w + (size_t)x0) * 4, dp_keep.px + (size_t)j * (size_t)w * 4,
                   (size_t)w * 4);
        return;
    }
    ui_shadow(c, offx, offy + 14, 1600 * DY_K, 900 * DY_K, 24 * DY_K, 40, 0x241040, 0.45f);
    diary_page_static(d);  /* drawn on the canvas, then kept from it */
    uint8_t *k = realloc(dp_keep.px, (size_t)w * (size_t)h * 4);
    if (!k) return;  /* not kept: drawn again next time */
    for (int j = 0; j < h; j++)
        memcpy(k + (size_t)j * (size_t)w * 4, c->px + ((size_t)(y0 + j) * (size_t)c->w + (size_t)x0) * 4, (size_t)w * 4);
    dp_keep.px = k, dp_keep.x = x0, dp_keep.y = y0, dp_keep.w = w, dp_keep.h = h;
    dp_keep.cw = c->w, dp_keep.ch = c->h, dp_keep.s = c->s, dp_keep.ox = c->ox, dp_keep.oy = c->oy;
    snprintf(dp_keep.title, sizeof dp_keep.title, "%s", dy_title);
}

/* A row's plate: purple (the chosen one green, lighter to the right, with a pale edge), fading into the page at its
 * right end as the game's rows do */
static void diary_plate(const UiCanvas *c, float x, float y, float w, float h, int on) {
    if (on) ui_box(c, x - 3, y - 3, w + 6, h + 6, 15, DY_CHOSEN_EDGE, 0, 0);
    ui_box_alpha(c, x, y, w, h, 12, on ? DY_CHOSEN : DY_PLATE, on ? 1.0f : 0.85f);
    if (on)
        for (int k = 1; k <= 12; k++) ui_rect(c, x + w * 0.35f + (float)(k - 1) * w * 0.04f, y, w * 0.04f, h, DY_CHOSEN_HI, 0.04f * (float)k);
    for (int k = 1; k <= 16; k++) ui_rect(c, x + w - 160 + (float)(k - 1) * 10, y - 4, 10, h + 8, DY_BG, (float)k / 16);
    ui_rect(c, x + w, y - 4, 8, h + 8, DY_BG, 1);  /* the edge's last pixels past the plate */
}

/* A label in a coloured pill with the game's swirls at its ends, a line out from it to a dot */
static void diary_pill(const UiCanvas *c, float x, float y, float w, float line_to, const char *label, uint32_t fill,
                       uint32_t ink, uint32_t line) {
    if (line_to > x + w) {
        ui_rect(c, x + w - 4, y + 26, line_to - (x + w) + 4, 1.5f, line, 1);
        ui_box(c, line_to - 4, y + 22.5f, 8, 8, 4, line, 0, 0);
    }
    ui_box(c, x, y, w, 28, 14, fill, 0, 0);
    ui_box_alpha(c, x + 6, y + 9, 10, 10, 5, ink, 0.18f);
    ui_box_alpha(c, x + w - 16, y + 9, 10, 10, 5, ink, 0.18f);
    ui_text_in(c, UI_ROUNDED_700, 19, x, y, w, 28, 1, label, ink);
}

/* a key hint's width (hint(), below the same sizes): to set the bar's hints at its right end */
static float hint_w(const char *key, const char *label) {
    float kw = -1;
    for (size_t i = 0; i < sizeof hint_icons / sizeof *hint_icons; i++)
        if (!strcmp(hint_icons[i].name, key)) kw = 30 + (hint_icons[i].icon[1] ? 24 : 0);
    if (kw < 0) {
        kw = ui_text_w(UI_GOTHIC_700, 14, key) + 16;
        if (kw < 30) kw = 30;
    }
    return kw + 10 + ui_text_w(UI_GOTHIC_400, 16, label);
}

static void draw_diary_pick(const UiCanvas *real, const GameMenuInfo *in, float W, float H, float T, float E,
                            float by) {
    (void)by, (void)H;
    int conf = dy_confirm, slots = conf ? dy_slots : diary_n, live = gamemenu_live();
    int n = conf ? slots + 1 : __atomic_load_n(&pick_n, __ATOMIC_ACQUIRE);
    int sel = conf ? dy_list_sel : pick_sel;  /* the page's chosen row (under the question: the slot asked about) */
    float offx, offy;
    UiCanvas dc = dy_canvas(real, W, &offx, &offy);
    const UiCanvas *c = &dc;  /* below, the layout's units */
    live_rect[0] = offx - 30, live_rect[1] = offy - 30, live_rect[2] = 1600 * DY_K + 60, live_rect[3] = 900 * DY_K + 70;
    if (live) {
        diary_page_kept(real, c, offx, offy);
    } else {  /* over the paused picture: dimmed, as the other popups */
        ui_rect(real, 0, T - 1, W, 900 + 2 * E + 2, POP_BACKDROP, 0.30f);
        ui_shadow(real, offx, offy + 14, 1600 * DY_K, 900 * DY_K, 24 * DY_K, 40, 0x241040, 0.45f);
        diary_page_static(c);
    }
    float X = 0, nx = 621;
    /* the rows: the slots, then (apart, where the game's Autosave row is) the way back */
    float lx = X + 79, lw = 520;
    for (int i = 0; i < n && i <= slots; i++) {
        int back = i == slots, on = i == sel;
        const GameMenuDiaryRow *r = back ? NULL : &diary_rows[i];
        float h = back ? 58 : 67, y = back ? 605 : 175 + (float)i * 83, sq = h - 16;
        diary_plate(c, lx, y, lw, h, on);
        if (on) ui_box(c, lx - 16, y + h / 2 - 9, 18, 18, 9, 0x7BD86A, 2, 0xFFFFFF);  /* the game's green pointer */
        /* the square at the left: the slot's number (the game's sprite), an empty slot's pencil, the back arrow */
        ui_box(c, lx + 9, y + 8, sq, sq, 8, on ? DY_SQUARE_ON : DY_SQUARE, 0, 0);
        if (back || !r->name[0]) {
            float is = sq * 0.6f;
            ui_icon(c, back ? DY_ARROW : DY_PENCIL, lx + 9 + (sq - is) / 2, y + 8 + (sq - is) / 2, is, 2.6f, 0xFFFFFF);
        } else {
            char num[4];
            snprintf(num, sizeof num, "%d", i + 1);
            ui_text_in(c, UI_ROUNDED_800, 28, lx + 9, y + 8, sq, sq, 1, num, 0xFFFFFF);
        }
        float tx = lx + 9 + sq + 16, tw = lx + 380 - tx;
        const char *name = back ? diary_back : r->name[0] ? r->name : "Empty Diary";
        if (back) {  /* one line, in the middle */
            ui_text_in(c, UI_ROUNDED_800, 25, tx + 1.5f, y + 1.5f, tw, h, 0, name, on ? DY_INK_ON : DY_INK);
            ui_text_in(c, UI_ROUNDED_800, 25, tx, y, tw, h, 0, name, 0xFFFFFF);
        } else {  /* the name over a thin line out to a dot, the date under it at the right */
            ui_rect(c, tx - 4, y + 39, lx + lw - 150 - tx, 1.5f, on ? DY_RULE_ON : DY_RULE, 0.9f);
            ui_text(c, UI_ROUNDED_800, 26, tx + 1.5f, y + 5 + 1.5f, 26 * 1.25f, name, on ? DY_INK_ON : DY_INK);
            ui_text(c, UI_ROUNDED_800, 26, tx, y + 5, 26 * 1.25f, name, 0xFFFFFF);
            if (r->date[0]) ui_text_in(c, UI_GOTHIC_700, 22, tx, y + 39, tw, 28, 2, r->date, on ? DY_NAVY_ON : DY_NAVY);
        }
        if (!conf) hit_add(offx + lx * DY_K, offy + y * DY_K, lw * DY_K, h * DY_K, HIT_PICK_ROW, i, 0);  /* the mouse: the screen's units */
    }
    /* the chosen slot's page */
    int sel_back = sel >= slots;
    const GameMenuDiaryRow *r = sel_back ? NULL : &diary_rows[sel];
    ui_box(c, nx + 66, 140, 78, 92, 4, 0xFFFFFF, 2, 0xC9BAD9);  /* where the game's portrait is */
    ui_box(c, nx + 71, 145, 68, 82, 2, sel_back ? 0xEDE6F3 : 0xE3EEF8, 0, 0);
    if (sel_back) {
        ui_icon(c, DY_ARROW, nx + 85, 166, 40, 3.2f, DY_HEAD);
    } else {
        char num[4];
        snprintf(num, sizeof num, "%d", sel + 1);
        ui_text_in(c, UI_ROUNDED_800, 48, nx + 71, 145, 68, 82, 1, num, DY_HEAD);
    }
    if (!sel_back) diary_pill(c, nx + 150, 150, 150, 0, "Recorder", 0xE8CBEA, 0x7A4A9A, 0);
    ui_text_in(c, UI_ROUNDED_800, 38, nx + 300, 136, 520, 52, 1,
               sel_back ? diary_back : r->name[0] ? r->name : "Empty Diary", DY_TITLE);
    ui_rect(c, nx + 150, 219, 670, 2.5f, DY_UNDER, 1);
    ui_box(c, nx + 816, 215.5f, 9, 9, 4.5f, DY_UNDER, 0, 0);
    const char *note;
    float note_y = 254;
    if (!sel_back) {
        char slot[24];
        snprintf(slot, sizeof slot, "%d of %d", sel + 1, slots);
        const struct {
            const char *label, *value;
            uint32_t fill, ink, line;
        } line[4] = {
            {"Diary Date", r->date[0] ? r->date : "-", 0xEBCFEC, 0x7A4A9A, 0xCDB3DA},
            {"Slot", slot, 0xE7EEB3, 0x6B7A2A, 0xD3DC98},
            {"Playtime", r->play[0] ? r->play : "-", 0xF4D4E4, 0x8A3F6A, 0xE6BBD0},
            {"Status", r->name[0] ? "Saved" : "Empty", 0xD0DEE8, 0x3F6680, 0xB5C9D8},
        };
        for (int k = 0; k < 4; k++) {
            float ly = 254 + (float)k * 47;
            diary_pill(c, nx + 46, ly, 233, nx + 822, line[k].label, line[k].fill, line[k].ink, line[k].line);
            ui_text_in(c, UI_GOTHIC_700, 27, nx + 300, ly - 15, 500, 36, 1, line[k].value, DY_NAVY);
        }
        note = r->name[0] ? "Your new game takes this slot. This diary is only replaced when you save over it in the Diary."
                          : "Your new game starts in this empty slot.";
        note_y = 254 + 4 * 47 + 20;
    } else {
        note = "No new game: back to the title screen. Your diaries stay as they are.";
    }
    ui_box(c, nx + 46, note_y, 778, 96, 14, DY_NOTE_CARD, 0, 0);  /* where the game's party is */
    ui_text_wrap(c, UI_GOTHIC_500, 21, 0, nx + 72, note_y + 18, 726, 21 * 1.5f, 0, note, DY_SUB);
    if (conf) {  /* the question over the page (owner, 2026-11-17: not the generic popup): a page of the notebook,
                  * its two answers as the Diary's rows side by side */
        ui_box_alpha(c, 0, 0, 1600, 900, 24, 0x2A1A40, 0.38f);
        float dw = 820, dh = 330, dx = (1600 - dw) / 2, dyy = (900 - DY_BAR_H - dh) / 2;
        ui_shadow(c, dx, dyy + 12, dw, dh, 12, 36, 0x241040, 0.5f);
        ui_box(c, dx - 8, dyy + 8, dw, dh, 12, DY_COVER, 0, 0);
        ui_box(c, dx, dyy, dw, dh, 10, DY_PAPER, 2, DY_PAPER_EDGE);
        for (float ddx = dx + 18; ddx < dx + dw - 24; ddx += 16)
            for (int k = 0; k < 2; k++) {
                float ddy = k ? dyy + dh - 18 : dyy + 8;
                ui_shape(c, "<path fill=\"#000\" d=\"M5 0L10 5L5 10L0 5Z\"/>", ddx, ddy, 10, 10, 10, DY_DIAMOND);
                ui_shape(c, "<path fill=\"#000\" d=\"M5 2.4L7.6 5L5 7.6L2.4 5Z\"/>", ddx, ddy, 10, 10, 10, DY_PAPER);
            }
        ui_text_in(c, UI_ROUNDED_800, 34, dx + 30, dyy + 30, dw - 60, 50, 1, prompt_title, DY_TITLE);
        ui_rect(c, dx + 60, dyy + 88, dw - 120, 2.5f, DY_UNDER, 1);
        ui_box(c, dx + dw - 64, dyy + 84.5f, 9, 9, 4.5f, DY_UNDER, 0, 0);
        ui_text_wrap(c, UI_GOTHIC_500, 21, 0, dx + 60, dyy + 106, dw - 120, 21 * 1.5f, 1, prompt_body, DY_SUB);
        float bw = 340, bh = 62, gap = 40, bx = dx + (dw - 2 * bw - gap) / 2, byy = dyy + dh - 46 - bh;
        for (int k = 0; k < 2; k++) {
            int on = k == pick_sel;
            float x = bx + (float)k * (bw + gap);
            if (on) ui_box(c, x - 3, byy - 3, bw + 6, bh + 6, 15, DY_CHOSEN_EDGE, 0, 0);
            ui_box(c, x, byy, bw, bh, 12, on ? DY_CHOSEN : DY_PLATE, 0, 0);
            if (on) ui_box(c, x - 16, byy + bh / 2 - 9, 18, 18, 9, 0x7BD86A, 2, 0xFFFFFF);  /* the green pointer */
            ui_text_in(c, UI_ROUNDED_800, 23, x + 1.5f, byy + 1.5f, bw, bh, 1, pick_rows[k], on ? DY_INK_ON : DY_INK);
            ui_text_in(c, UI_ROUNDED_800, 23, x, byy, bw, bh, 1, pick_rows[k], 0xFFFFFF);
            hit_add(offx + x * DY_K, offy + byy * DY_K, bw * DY_K, bh * DY_K, HIT_PICK_ROW, k, 0);
        }
    }
    /* the keys, at the bar's right end as the game's (the keys bound to the game's A and B work here: window.c) */
    const char *choose = "W S", *ok = "Enter", *back = "Esc";
    if (in->pad) {
        choose = "D-pad", ok = in->pad == 2 ? "Cross" : "A", back = in->pad == 2 ? "Circle" : "B";
    } else {
        for (int i = 0; i < 32; i++) {
            const char *id = bindings_input_id(i), *key = bindings_input_key(0, i, 0);
            if (key && key[0] && !strcmp(id, "A")) ok = key;
            if (key && key[0] && !strcmp(id, "B")) back = key;
        }
    }
    float hy = 900 - DY_BAR_H + (DY_BAR_H - 56) / 2;
    float hx = X + 1600 - 48 - hint_w(choose, "Choose") - 36 - hint_w(ok, "Select") - 36 - hint_w(back, "Back");
    hx += hint(c, hx, hy, choose, "Choose") + 36;
    hx += hint(c, hx, hy, ok, "Select") + 36;
    hint(c, hx, hy, back, "Back");
}

/* The list popup (gamemenu_choice_begin): the name popup's frame (board N1), the question under the title, the rows
 * as buttons, the chosen one gold (as the on-screen keyboard's key); OK picks it. No Cancel: one row is the answer. */
static void draw_pick(const UiCanvas *c, const GameMenuInfo *in, float W, float H, float T, float E, float by) {
    int live = gamemenu_live();
    if (!live) ui_rect(c, 0, T - 1, W, H + 2 * E + 2, POP_BACKDROP, 0.45f);
    int n = __atomic_load_n(&pick_n, __ATOMIC_ACQUIRE);
    float pw = 720, bar = 18 + 22 * 1.3f + 16, blh = 17 * 1.5f, rh = 44, rg = 4;
    float msg_h = prompt_body[0] ? ui_text_wrap(NULL, UI_GOTHIC_500, 17, 0, 0, 0, pw - 56, blh, 0, prompt_body, 0) : 0;
    float body = 22 + (msg_h > 0 ? msg_h + 14 : 0) + (float)n * (rh + rg) - rg + 18;
    float tray = 16 + 52 + 16, ph = bar + 3 + body + tray;
    float px = (W - pw) / 2, py = (H - 56 - ph) / 2;
    live_rect[0] = px - 40, live_rect[1] = py - 30, live_rect[2] = pw + 80, live_rect[3] = ph + 90;
    ui_shadow(c, px, py + 14, pw, ph, 16, 40, POP_SHADOW, 0.45f);
    ui_box_alpha(c, px - 1, py - 1, pw + 2, ph + 2, 17, POP_BORDER, 0.5f);
    ui_box(c, px, py, pw, ph, 16, POP_WHITE, 0, 0);
    ui_box(c, px, py, pw, bar + 16, 16, POP_DEEP, 0, 0);
    ui_rect(c, px, py + bar, pw, 3, POP_GOLD, 1);
    float ty = py + ph - tray;
    ui_box(c, px, ty - 16, pw, tray + 16, 16, POP_LILAC, 0, 0);
    ui_rect(c, px, py + bar + 3, pw, ty - (py + bar + 3), POP_WHITE, 1);
    ui_rect(c, px, ty, pw, 1, POP_LINE, 1);
    ui_text(c, UI_ROUNDED_800, 22, px + 28, py + 18, 22 * 1.3f, prompt_title, POP_WHITE);
    float bx = px + 28, bw = pw - 56, y = py + bar + 3 + 22;
    if (msg_h > 0) {
        ui_text_wrap(c, UI_GOTHIC_500, 17, 0, bx, y, bw, blh, 0, prompt_body, POP_INK);
        y += msg_h + 14;
    }
    for (int i = 0; i < n; i++, y += rh + rg) {
        int on = i == pick_sel;
        ui_box(c, bx, y, bw, rh, 12, on ? GOLD : WHITE, on ? 0 : 2, LINE);
        ui_text_in(c, UI_ROUNDED_800, 18, bx + 18, y, bw - 36, rh, 0, pick_rows[i], on ? ON_GOLD : PLUM);
        hit_add(bx, y, bw, rh, HIT_PICK_ROW, i, 0);
    }
    float okw = ui_text_w(UI_ROUNDED_800, 18, "OK") + 2 * 32, okx = px + pw - 16 - okw, oky = ty + 16;
    ui_box(c, okx - 4, oky - 4, okw + 8, 60, 15, RING, 0, 0);
    ui_box(c, okx, oky, okw, 52, 12, GOLD, 0, 0);
    ui_text_in(c, UI_ROUNDED_800, 18, okx, oky, okw, 52, 1, "OK", ON_GOLD);
    hit_add(okx, oky, okw, 52, HIT_PROMPT_OK, 0, 0);
    if (live) return;  /* over the running game: the popup alone */
    ui_rect(c, 0, by, W, 56 + 1, NIGHT, 1);  /* the key hints, the whole width */
    float hx = 48;
    if (in->pad) {
        hx += hint(c, hx, by, "D-pad", "Choose") + 28;
        hint(c, hx, by, in->pad == 2 ? "Cross" : "A", "OK");
    } else {
        hx += hint(c, hx, by, "W S", "Choose") + 28;
        hint(c, hx, by, "Enter", "OK");
    }
}

void gamemenu_draw(const UiCanvas *c, const GameMenuInfo *in) {
    TH = theme_colors();
    draw_text_has = 0;
    /* the overlay's own name entry has no thread waiting in gamemenu_prompt_take: Enter (the field let go) is OK here */
    if (prompt_done && gamemenu_prompting() && !textin_has(&prompt_text)) prompt_finish();
    if (gamemenu_peeking()) {  /* F2's quick look: the keyboard Overview alone, in the game's current mode */
        int hk = hit_back();
        hit_n[hk] = 0;  /* only the keys and the mode switch can be pointed at (gamemenu_peek_point) */
        static const char *const ptitle[3] = {"Controls \xc2\xb7 Exploring", "Controls \xc2\xb7 Battle", "Controls \xc2\xb7 Cutscene"};
        int saved = kv_mode;
        kv_mode = peek_shown_mode();
        /* centred on the 1600 x 900 design (owner: in the middle, bigger, the picture behind it blurred); the
         * keyboard's card is taller by its detail bar */
        float pw = 1360, ph = in->pad ? 640 : 714, px = (1600 - pw) / 2, py = (900 - ph) / 2;
        ui_box(c, px, py, pw, ph, 22, PAPER, 0, 0);
        /* the key hints along the card's bottom, as the other pages' bar (a dark band, the keys on light chips) */
        float hb = py + ph - 56, hx = px + 40;
        ui_box(c, px, hb, pw, 56, 22, NIGHT, 0, 0);
        ui_rect(c, px, hb, pw, 24, NIGHT, 1);  /* its top corners square */
        if (in->pad) {  /* the controller was used last (the quick look by holding View / Create / Minus): its view */
            hx += hint(c, hx, hb, pv_chip(PV_MINUS, bindings_pad_style()), "Next page") + 28;
            hx += hint(c, hx, hb, pv_chip(PV_MINUS, bindings_pad_style()), "Hold: hide") + 28;
            hint(c, hx, hb, in->close, "Menu");
            ui_text(c, UI_ROUNDED_800, 30, px + 40, py + 30, 30 * 1.25f, ptitle[kv_mode], NIGHT);
            draw_controls_pad(c, in, px + 40, py + 30 + 30 * 1.25f + 6, pw - 80, 1);
        } else {
            hx += hint(c, hx, hb, "F2", "Hide") + 28;
            hint(c, hx, hb, "Esc", "Menu, to change the keys");
            ui_text(c, UI_ROUNDED_800, 30, px + 40, py + 30, 30 * 1.25f, "Controls", NIGHT);
            draw_controls_overview(c, px + 40, py + 30 + 30 * 1.25f + 18, pw - 80, 1);
        }
        kv_mode = saved;
        pv_rect[0] = px, pv_rect[1] = py, pv_rect[2] = pw, pv_rect[3] = ph;  /* the part the window shows */
        __atomic_store_n(&hit_front, hk, __ATOMIC_RELEASE);
        return;
    }
    float W = (float)c->w / c->s, H = 900;
    /* a picture narrower than 16:9 (the 16:10 aspect ratio): the design is fitted to its width and centred, with
     * a strip of E design units above and below it (window.c, overlay_fit). The frame reaches the picture's
     * edges: the tint and the side panel cover the strips, the header and the side buttons sit at the top (T),
     * Exit game, the version and the key hints at the bottom (by). The pages and popups stay centred */
    float E = c->oy > 0 ? c->oy / c->s : 0, T = -E, by = H - 56 + E;
    edge = E;
    int hk = hit_back();
    hit_n[hk] = 0;  /* the mouse's list, filled as things are drawn */
    int req = __atomic_exchange_n(&bug_request, 0, __ATOMIC_ACQ_REL);
    if (req) {  /* Save only / Send (bug_finish): here, where the paused picture is */
        textin_read(&bug_text, bug_sent_text, sizeof bug_sent_text, NULL, NULL);
        bug_sent_kind = bug_kind, bug_sent_often = bug_often;
        BugReport r = bug_report_of(bug_sent_text, bug_kind, bug_often);
        r.picture = in->picture, r.picture_w = in->picture_w, r.picture_h = in->picture_h;
        r.window_w = c->w, r.window_h = c->h;
        if (bugreport_save(&r, bug_zip, sizeof bug_zip) == 0) {
            if (req == 2) bugreport_open_github(&r);
            bug_sent_github = req == 2;
            bug_page = 2, bug_done_btn = req == 2 ? 2 : 0;
        } else {
            toast("Couldn't save the report");
        }
        changed();
    }
    if (gamemenu_prompting() && __atomic_load_n(&pick_n, __ATOMIC_ACQUIRE)) {  /* a list (the New Game slots) */
        if (diary_n || dy_confirm) draw_diary_pick(c, in, W, H, T, E, by);
        else draw_pick(c, in, W, H, T, E, by);
        __atomic_store_n(&hit_front, hk, __ATOMIC_RELEASE);
        return;
    }
    if (gamemenu_prompting()) {  /* name entry (board N1): only the popup, over the game's waiting picture */
        ui_rect(c, 0, T - 1, W, H + 2 * E + 2, POP_BACKDROP, 0.45f);
        float pw = 650, bar = 18 + 22 * 1.3f + 16, fh = 72, llh = 16 * 1.45f;
        float blh = 17 * 1.5f, msg_h = prompt_body[0] ? ui_text_wrap(NULL, UI_GOTHIC_500, 17, 0, 0, 0, pw - 56, blh, 0,
                                                                      prompt_body, 0) : 0;
        static int osk_force = -1;  /* YW4_OSK=1: the keyboard with any device (tests, touch screens) */
        if (osk_force < 0) osk_force = getenv("YW4_OSK") && *getenv("YW4_OSK") == '1';
        osk_shown = (in->pad || osk_force) && !prompt_body[0];
        float kh = 50, kgap = 8, tab_h = 40, osk_h = osk_shown ? 14 + tab_h + 12 + OSK_ROWS * (kh + kgap) : 0;
        float body = prompt_body[0] ? 24 + msg_h + 24 : 24 + llh + 8 + fh + 12 + llh + osk_h + 22;
        float tray = 16 + 52 + 16, ph = bar + 3 + body + tray;
        float px = (W - pw) / 2, py = (H - 56 - ph) / 2;
        ui_shadow(c, px, py + 14, pw, ph, 16, 40, POP_SHADOW, 0.45f);
        ui_box_alpha(c, px - 1, py - 1, pw + 2, ph + 2, 17, POP_BORDER, 0.5f);
        ui_box(c, px, py, pw, ph, 16, POP_WHITE, 0, 0);
        ui_box(c, px, py, pw, bar + 16, 16, POP_DEEP, 0, 0);
        ui_rect(c, px, py + bar, pw, 3, POP_GOLD, 1);
        float ty = py + ph - tray;
        ui_box(c, px, ty - 16, pw, tray + 16, 16, POP_LILAC, 0, 0);
        ui_rect(c, px, py + bar + 3, pw, ty - (py + bar + 3), POP_WHITE, 1);
        ui_rect(c, px, ty, pw, 1, POP_LINE, 1);
        ui_text(c, UI_ROUNDED_800, 22, px + 28, py + 18, 22 * 1.3f, prompt_title, POP_WHITE);
        float bx = px + 28, bw = pw - 56, y = py + bar + 3 + 24;
        int typing = textin_has(&prompt_text);
        if (prompt_body[0]) {  /* a message (download codes): the text, then OK */
            ui_text_wrap(c, UI_GOTHIC_500, 17, 0, bx, y, bw, blh, 0, prompt_body, POP_INK);
            goto prompt_ok;
        }
        ui_text(c, UI_GOTHIC_700, 16, bx, y, llh, prompt_label, MUTED);
        y += llh + 8;
        char text[TEXTIN_MAX];
        int cur, sel, chars = 0;
        textin_read(&prompt_text, text, sizeof text, &cur, &sel);
        for (const char *p = text; *p; p++) chars += ((unsigned char)*p & 0xC0) != 0x80;
        ui_box(c, bx, y, bw, fh, 14, WHITE, typing ? 3 : 2, typing ? PLUM : LINE);
        hit_add(bx, y, bw, fh, HIT_PROMPT_FIELD, 0, 0);
        char count[24] = "";
        if (prompt_text.max_chars > 0) snprintf(count, sizeof count, "%d / %d", chars, prompt_text.max_chars);
        float cw = ui_text_w(UI_GOTHIC_700, 17, count);
        ui_text_in(c, UI_GOTHIC_700, 17, bx + bw - 20 - cw, y, cw, fh, 2, count, MUTED);
        if (!text[0]) ui_text_in(c, UI_ROUNDED_800, 32, bx + 20 + 6, y, 0, fh, 0, prompt_fallback, LILAC);  /* the default */
        ui_text_edit(c, UI_ROUNDED_800, 32, bx + 20, y + (fh - 32 * 1.3f) / 2, bw - 40 - cw - 16, 32 * 1.3f, 1, text,
                     typing ? cur : -1, typing ? sel : -1, INK, PLUM, RING);
        if (typing) {
            draw_text_box[0] = bx, draw_text_box[1] = y, draw_text_box[2] = bw, draw_text_box[3] = fh;
            draw_text_caret = ui_text_caret_x();
            draw_text_has = 1;
        }
        y += fh + 12;
        char help[128], limit[32] = "";
        if (prompt_text.max_chars > 0) snprintf(limit, sizeof limit, " Up to %d letters.", prompt_text.max_chars);
        snprintf(help, sizeof help, "%s%s%s%s%s", osk_shown ? "Pick the letters below." : "Type with your keyboard.", limit,
                 prompt_fallback[0] ? " Leave it empty for " : "", prompt_fallback, prompt_fallback[0] ? "." : "");
        ui_text(c, UI_GOTHIC_500, 16, bx, y, llh, help, MUTED);
        if (osk_shown) {  /* the on-screen keyboard: the language switcher, then ten columns, the chosen key gold */
            float ky = y + llh + 14, kw = (bw - (OSK_COLS - 1) * kgap) / OSK_COLS;
            {  /* LB [‹] [ English ] [›] RB: the language's name in its own script, on plum */
                const char *lb = in->pad == 2 ? "L1" : "LB", *rb = in->pad == 2 ? "R1" : "RB";
                float sw = ui_text_w(UI_ROUNDED_800, 13, "L1") + 18;
                if (ui_text_w(UI_ROUNDED_800, 13, "LB") + 18 > sw) sw = ui_text_w(UI_ROUNDED_800, 13, "LB") + 18;
                float sh = 26, sy = ky + (tab_h - sh) / 2, aw = 52;
                float ax0 = bx + sw + kgap, ax1 = bx + bw - sw - kgap - aw;  /* the two arrows */
                float nx = ax0 + aw + kgap, nw = ax1 - kgap - nx;                /* the name between them */
                ui_box(c, bx, sy, sw, sh, 8, PLUM, 0, 0);
                ui_text_in(c, UI_ROUNDED_800, 13, bx, sy, sw, sh, 1, lb, SNOW);
                ui_box(c, bx + bw - sw, sy, sw, sh, 8, PLUM, 0, 0);
                ui_text_in(c, UI_ROUNDED_800, 13, bx + bw - sw, sy, sw, sh, 1, rb, SNOW);
                ui_box(c, ax0, ky, aw, tab_h, 10, WHITE, 2, LINE);
                ui_text_in(c, UI_ROUNDED_800, 24, ax0, ky, aw, tab_h, 1, "‹", PLUM);
                hit_add(ax0, ky, aw, tab_h, HIT_OSK_PAGE, -1, 0);
                ui_box(c, ax1, ky, aw, tab_h, 10, WHITE, 2, LINE);
                ui_text_in(c, UI_ROUNDED_800, 24, ax1, ky, aw, tab_h, 1, "›", PLUM);
                hit_add(ax1, ky, aw, tab_h, HIT_OSK_PAGE, 1, 0);
                ui_box(c, nx, ky, nw, tab_h, 10, PLUM, 0, 0);
                ui_text_in(c, UI_ROUNDED_800, 18, nx, ky, nw, tab_h, 1, osk_page ? "日本語" : "English", SNOW);
                ky += tab_h + 12;
            }
            int nlast;
            const OskKey *L = osk_last(&nlast);
            for (int r = 0; r < OSK_ROWS; r++, ky += kh + kgap) {
                int last = r == OSK_ROWS - 1, nkeys = last ? nlast : r < osk_letter_rows() ? OSK_COLS : 0;
                for (int k = 0; k < nkeys; k++) {
                    int c0 = last ? L[k].c0 : k, c1 = last ? L[k].c1 : k + 1;
                    char label[16];
                    if (last) snprintf(label, sizeof label, "%s", L[k].act == OSK_SCRIPT ? osk_page == 1 ? "ア" : "あ" : L[k].label);
                    else osk_letter(r, k, label);
                    if (!label[0]) continue;  /* や's column: no key in the gaps */
                    float kx = bx + (float)c0 * (kw + kgap), w2 = (float)(c1 - c0) * (kw + kgap) - kgap;
                    int on = osk_row == r && osk_col >= c0 && osk_col < c1;
                    int shift_on = last && L[k].act == OSK_SHIFT && osk_caps, done = last && L[k].act == OSK_DONE;
                    int word = last && label[0] >= 'A' && label[0] <= 'Z';  /* Shift, Space...: smaller than letters */
                    uint32_t fill = on ? GOLD : done ? PLUM : shift_on ? LILAC : WHITE;
                    ui_box(c, kx, ky, w2, kh, 10, fill, on ? 0 : 2, LINE);
                    ui_text_in(c, UI_ROUNDED_800, word ? 17 : 22, kx, ky, w2, kh, 1, label,
                               on ? ON_GOLD : done ? SNOW : PLUM);
                    hit_add(kx, ky, w2, kh, HIT_OSK_KEY, r, c0);
                }
            }
        }
    prompt_ok:;
        float okw = ui_text_w(UI_ROUNDED_800, 18, "OK") + 2 * 32, okx = px + pw - 16 - okw, oky = ty + 16;
        int ok_on = prompt_enough();  /* a password with nothing typed: OK greyed out until there is a letter */
        if (!typing && ok_on) ui_box(c, okx - 4, oky - 4, okw + 8, 60, 15, RING, 0, 0);
        ui_box(c, okx, oky, okw, 52, 12, ok_on ? GOLD : LINE, 0, 0);
        ui_text_in(c, UI_ROUNDED_800, 18, okx, oky, okw, 52, 1, "OK", ok_on ? ON_GOLD : MUTED);
        hit_add(okx, oky, okw, 52, HIT_PROMPT_OK, 0, 0);
        int can_cancel = prompt_cancel && !prompt_body[0];
        if (can_cancel) {  /* Cancel (a password, a nickname): the game is told the keyboard was closed */
            float cw2 = ui_text_w(UI_ROUNDED_800, 18, "Cancel") + 2 * 28, cx2 = okx - 10 - cw2;
            ui_box(c, cx2, oky, cw2, 52, 12, POP_WHITE, 2, POP_LINE);
            ui_text_in(c, UI_ROUNDED_800, 18, cx2, oky, cw2, 52, 1, "Cancel", POP_INK);
            hit_add(cx2, oky, cw2, 52, HIT_PROMPT_CANCEL, 0, 0);
        }
        ui_rect(c, 0, by, W, 56 + 1, NIGHT, 1);  /* the key hints, the whole width */
        float hx = 48;
        if (in->pad && osk_shown) {  /* the on-screen keyboard */
            int ps = in->pad == 2;
            hx += hint(c, hx, by, "D-pad", "Move") + 28;
            hx += hint(c, hx, by, ps ? "Cross" : "A", "Type") + 28;
            hx += hint(c, hx, by, ps ? "Circle" : "B", can_cancel ? "Delete / Cancel" : "Delete") + 28;
            hx += hint(c, hx, by, ps ? "Triangle" : "Y", osk_page ? "゛゜" : "Capitals") + 28;
            hint(c, hx, by, ps ? "L1 R1" : "LB RB", "Language");
        } else if (in->pad) {
            hx += hint(c, hx, by, in->pad == 2 ? "Cross" : "A", "OK") + 28;
            if (can_cancel) hint(c, hx, by, in->pad == 2 ? "Circle" : "B", "Cancel");
        } else {
            hx += hint(c, hx, by, "Enter", "OK") + 28;
            if (can_cancel) hx += hint(c, hx, by, "Esc", "Cancel") + 28;
            if (!prompt_body[0]) {
                hx += hint(c, hx, by, "Backspace", "Delete") + 28;
                hint(c, hx, by, "Ctrl V", "Paste");
            }
        }
        __atomic_store_n(&hit_front, hk, __ATOMIC_RELEASE);
        return;
    }
    if (preview) {  /* only the bar, over the running game */
        draw_preview(c, in);
        __atomic_store_n(&hit_front, hk, __ATOMIC_RELEASE);
        return;
    }
    int cheats = cheats_allowed();
    int page = (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_ONLINE || sel == GM_PERF || (sel == GM_CHEATS && cheats) ||
                sel == GM_BUG) && !confirm;  /* these show a page */
    ui_rect(c, 0, T - 1, W, H + 2 * E + 2, SHADE, page ? 0.62f : 0.35f);  /* the tint over the game, darker behind a page */
    if (page && sel == GM_SETTINGS) draw_settings_page(c, in);
    if (page && sel == GM_CONTROLS) draw_controls_page(c, in);
    if (page && sel == GM_ONLINE) draw_online_page(c, in);
    if (page && sel == GM_PERF) draw_perf_page(c, in);
    if (page && sel == GM_CHEATS) draw_cheats_page(c, in);
    if (page && sel == GM_BUG) draw_bug_page(c, in);

    /* the side panel: 400 wide with a 4 px gold edge towards the game */
    ui_rect(c, 0, T - 1, 400, H + 2 * E + 2, PAPER, 1);
    ui_rect(c, 396, T - 1, 4, H + 2 * E + 2, GOLD, 1);
    ui_rect(c, 0, T - 1, 396, 108 + 1, NIGHT, 1);  /* the header: padding 28 / 24 around the 56 px logo */
    if (in->logo) {  /* the game's own icon (gamelogo.c), else the design's clock */
        ui_image(c, in->logo, in->logo_w, in->logo_h, 28, T + 28, 56, 56, 14);
    } else {
        ui_box(c, 28, T + 28, 56, 56, 14, GOLD, 0, 0);
        ui_icon(c, ICON_CLOCK, 28 + 13, T + 28 + 13, 30, 2.2f, NIGHT);
    }
    float th = 21 * 1.2f, sh = 14 * 1.45f, ty = T + 28 + (56 - (th + 4 + sh)) / 2;
    ui_text(c, UI_ROUNDED_800, 21, 100, ty, th, "Yo-kai Watch 4 Recomp", SNOW);
    ui_text(c, UI_GOTHIC_400, 14, 100, ty + th + 4, sh, "Game version " GAME_VERSION, ON_DARK);

    /* the items with a page, GM_RESUME..GM_BUG; Cheats only while the launcher allows them (cheats.h) */
    static const char *const labels[] = {"Resume", "Settings", "Controls", "Online", "Overlay", "Cheats", "Report a bug"};
    const char *icons[] = {ICON_RESUME, ICON_SETTINGS, ICON_CONTROLS, ICON_ONLINE, ICON_PERF, ICON_CHEATS, ICON_BUG};
    for (int i = 0, k = 0; i <= GM_BUG; i++) {
        if (i == GM_CHEATS && !cheats) continue;
        side_button(c, 20, T + 128 + k * 66, 356, icons[i], labels[i], sel == i, PLUM);
        hit_add(20, T + 128 + k * 66, 356, 56, HIT_SIDE, i, 0);
        k++;
    }
    /* Return to title and Exit game at the bottom, under a line (the ways out of the game); the version under them */
    float vy = H + E - 20 - 19, ey = vy - 14 - 56, ry = ey - 10 - 56;
    ui_rect(c, 20, ry - 14 - 2, 356, 2, LINE, 1);
    int title_state = window_pause_title_state();  /* 1 usable, 0 the game is still pausing, -1 not here */
    if (title_state > 0) {
        side_button(c, 20, ry, 356, ICON_TITLE, "Return to title", sel == GM_TITLE, PLUM);
        hit_add(20, ry, 356, 56, HIT_SIDE, GM_TITLE, 0);
    } else {  /* greyed out: no hover, no click, the keys pass over it */
        side_button_off(c, 20, ry, 356, ICON_TITLE, "Return to title", 0, NULL);
    }
    side_button(c, 20, ey, 356, ICON_EXIT, "Exit game", sel == GM_EXIT, WINE);
    hit_add(20, ey, 356, 56, HIT_SIDE, GM_EXIT, 0);
    char ver[48];
    snprintf(ver, sizeof ver, "YW4 Recomp %s", in->version);
    ui_text(c, UI_GOTHIC_400, 13, 20, vy, 19, ver, MUTED);

    /* the key hints along the bottom */
    ui_rect(c, 400, by, W - 400, 56 + 1, NIGHT, 1);
    float hx = 448;
    if (focus && sel == GM_BUG && textin_has(&bug_text)) {  /* typing in the report */
        if (in->pad) {
            hint(c, hx, by, in->pad == 2 ? "Circle" : "B", "Done typing");
        } else {
            hx += hint(c, hx, by, "Enter", "Done") + 28;
            hint(c, hx, by, "Ctrl V", "Paste");
        }
    } else if (focus && sel == GM_CONTROLS && ctrl_view && mb_open) {  /* the Mouse buttons page, or its list */
        static const char *const shoulders[3] = {"LB RB", "L1 R1", "L R"};
        int ps = in->pad == 2;
        const char *ok = in->pad ? (ps ? "Cross" : "A") : "Enter", *back = in->pad ? (ps ? "Circle" : "B") : "Esc";
        if (mb_list) {
            hx += hint(c, hx, by, in->pad ? "D-pad" : "W S", "Choose") + 28;
            hx += hint(c, hx, by, ok, "Bind") + 28;
            hint(c, hx, by, back, "Cancel");
        } else {
            hx += hint(c, hx, by, in->pad ? "D-pad" : "W S", "Choose") + 28;
            hx += hint(c, hx, by, in->pad ? "D-pad" : "A D", "Change") + 28;
            hx += hint(c, hx, by, ok, "List") + 28;
            hx += hint(c, hx, by, in->pad ? (ps ? "Square" : "X") : "Del", "None") + 28;
            hx += hint(c, hx, by, in->pad ? shoulders[bindings_pad_style() % 3] : "Q E", "Exploring / Battle") + 28;
            hint(c, hx, by, back, "Back");
        }
    } else if (focus && sel == GM_CONTROLS && !ctrl_view) {  /* the controls page's keyboard or controller view */
        static const char *const shoulders[3] = {"LB RB", "L1 R1", "L R"};  /* as the controller names them */
        int ps = in->pad == 2;
        hx += hint(c, hx, by, in->pad ? shoulders[bindings_pad_style() % 3] : "Q E", "Exploring / Battle / Cutscene") + 28;
        hx += hint(c, hx, by, in->pad ? alt_button(in) : "P", "Bindings") + 28;
        hx += hint(c, hx, by, in->pad ? (ps ? "Circle" : "B") : "Esc", "Back") + 28;
        if (in->pad) hint(c, hx, by, in->close, "Close overlay");
    } else if (focus && sel == GM_CONTROLS) {  /* the controls page */
        int ps = in->pad == 2;
        hx += hint(c, hx, by, in->pad ? alt_button(in) : "P", in->pad ? "Controller view" : "Keyboard view") + 28;
        hx += hint(c, hx, by, in->pad ? "D-pad" : "W S", "Choose") + 28;
        if (!in->pad) hx += hint(c, hx, by, "A D", "Navigate") + 28;  /* across: a key, the other key, the pad */
        hx += hint(c, hx, by, in->pad ? (ps ? "Cross" : "A") : "Enter", "Change") + 28;
        hx += hint(c, hx, by, in->pad ? (ps ? "Square" : "X") : "Del", "Clear") + 28;
        hx += hint(c, hx, by, in->pad ? (ps ? "Circle" : "B") : "Esc", "Back") + 28;
        if (in->pad) hint(c, hx, by, in->close, "Close overlay");
    } else if (focus && sel == GM_SETTINGS && !in_rows) {  /* the settings page's sections */
        hx += hint(c, hx, by, in->pad ? "D-pad" : "W S", "Choose section") + 28;
        hx += hint(c, hx, by, in->pad ? (in->pad == 2 ? "Cross" : "A") : "Enter", "Open") + 28;
        if (can_preview()) hx += hint(c, hx, by, in->pad ? alt_button(in) : "P", "Preview") + 28;
        hx += hint(c, hx, by, in->pad ? (in->pad == 2 ? "Circle" : "B") : "Esc", "Back") + 28;
        if (in->pad) hint(c, hx, by, in->close, "Close overlay");
    } else if (focus && in->pad) {  /* on a page */
        hx += hint(c, hx, by, "D-pad", "Choose and change") + 28;
        hx += hint(c, hx, by, in->pad == 2 ? "Cross" : "A", "Select") + 28;
        if (sel == GM_SETTINGS) hx += hint(c, hx, by, in->pad == 2 ? "L1 R1" : "LB RB", "Section") + 28;
        if (sel == GM_SETTINGS && can_preview()) hx += hint(c, hx, by, alt_button(in), "Preview") + 28;
        hx += hint(c, hx, by, in->pad == 2 ? "Circle" : "B", "Back") + 28;
        hint(c, hx, by, in->close, "Close overlay");
    } else if (focus) {  /* on a page, with the keyboard: Escape goes back to the side panel, again closes */
        hx += hint(c, hx, by, "W S", "Move") + 28;
        hx += hint(c, hx, by, "A D", "Change") + 28;
        hx += hint(c, hx, by, "Enter", "Select") + 28;
        if (sel == GM_SETTINGS) hx += hint(c, hx, by, "Q E", "Section") + 28;
        if (sel == GM_SETTINGS && can_preview()) hx += hint(c, hx, by, "P", "Preview") + 28;
        hint(c, hx, by, "Esc", "Back");
    } else if (in->pad) {
        hx += hint(c, hx, by, in->pad == 2 ? "Cross" : "A", "Select") + 28;
        hint(c, hx, by, in->pad == 2 ? "Circle" : "B", "Close overlay");
    } else {
        hx += hint(c, hx, by, "W S", "Move") + 28;
        hx += hint(c, hx, by, "Enter", "Select") + 28;
        hint(c, hx, by, "Esc", "Close overlay");
    }

    /* a short note (toast_msg): top right */
    if (in->now < toast_until) {
        const char *t = toast_msg;
        float tw = ui_text_w(UI_GOTHIC_700, 17, t), bw = tw + 44 + 6, bh = 17 * 1.45f + 28 + 6;
        ui_box(c, W - 48 - bw, T + 40, bw, bh, 14, SNOW, 3, RING);
        ui_text_in(c, UI_GOTHIC_700, 17, W - 48 - bw, T + 40, bw, bh, 1, t, NIGHT);
    }

    /* the exit popup: a small window as the launcher's popups (launcher/ui/style.css "dialog", 2026-11-05): a
     * violet title bar over a gold rule, the text on white, the buttons in a lilac tray at the bottom */
    if (confirm) {
        int title = confirm == GM_TITLE;
        char age[80];
        save_age(age, sizeof age);
        ui_rect(c, 0, T - 1, W, H + 2 * E + 2, POP_BACKDROP, 0.6f);
        const float k = POP_K, r = 14 * k, gold = 3 * k;
        float pw = 460 * k, lh_title = 17 * k * 1.3f, bar = (15 + 13) * k + lh_title, lh = 14 * k * 1.5f;
        float tray = (12 + 40 + 12) * k, gap = 18 * k, pad = 22 * k;
        float ph = bar + gold + gap + 2 * lh + gap + tray;  /* two lines: what is lost, and when the game saved */
        float px = (W - pw) / 2, py = (H - ph) / 2;
        ui_shadow(c, px, py + r, pw, ph, r, 36 * k, POP_SHADOW, 0.45f);
        ui_box_alpha(c, px - 1, py - 1, pw + 2, ph + 2, r + 1, POP_BORDER, 0.5f);  /* the 1 px border, half seen */
        ui_box(c, px, py, pw, ph, r, POP_WHITE, 0, 0);
        ui_box(c, px, py, pw, bar + r, r, POP_DEEP, 0, 0);  /* the title bar (its lower corners covered below) */
        ui_rect(c, px, py + bar, pw, gold, POP_GOLD, 1);
        float ty = py + ph - tray;
        ui_box(c, px, ty - r, pw, tray + r, r, POP_LILAC, 0, 0);  /* the tray (its upper corners covered) */
        ui_rect(c, px, py + bar + gold, pw, ty - (py + bar + gold), POP_WHITE, 1);
        ui_rect(c, px, ty, pw, 1, POP_LINE, 1);
        /* the title bar: the side button's icon (house / power), then the question */
        float is = 20 * k;
        ui_icon(c, title ? ICON_TITLE : ICON_EXIT, px + pad, py + 15 * k + (lh_title - is) / 2, is, 2.2f, POP_WHITE);
        ui_text(c, UI_ROUNDED_800, 17 * k, px + pad + is + 10 * k, py + 15 * k, lh_title,
                title ? "Return to the title screen?" : "Quit the game?", POP_WHITE);
        ui_text(c, UI_GOTHIC_400, 14 * k, px + pad, py + bar + gold + gap, lh, "Progress since your last save is lost.",
                POP_INK);
        ui_text(c, UI_GOTHIC_400, 14 * k, px + pad, py + bar + gold + gap + lh, lh, age, POP_INK);
        float right = px + pw - 16 * k;
        right -= popup_button(c, right, ty + 12 * k, title ? "Return to title" : "Quit game", 1, choice == 1) + 10 * k;
        popup_button(c, right, ty + 12 * k, "Keep playing", 0, choice == 0);
    }
    if (draw_text_has) {
        memcpy(focused_text_box, draw_text_box, sizeof focused_text_box);
        focused_caret_x = draw_text_caret;
        __atomic_store_n(&focused_text_valid, 1, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&focused_text_valid, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&hit_front, hk, __ATOMIC_RELEASE);  /* the mouse reads this drawing's list now */
}
