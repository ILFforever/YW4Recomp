/* The in-game overlay's side panel (gamemenu.h): which item is chosen, what each does, and how it looks. Runs on
 * the window's thread (key and pad events); the present reads the state to draw it. The layout's numbers are the
 * design's (Main.dc.html on the overlay's design canvas), in its CSS pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamemenu.h"
#include "bindings.h"
#include "bugreport.h"
#include "settings.h"
#include "textin.h"
#include "theme.h"
#include "window.h"

static int open_, sel, confirm, choice;
static int focus;             /* 0 the side panel, 1 the page it shows (Settings) */
static int srow;              /* the chosen row on the settings page (settings_player_rows) */
static int in_rows;           /* the settings page: 0 the sections list has the focus, 1 the chosen section's rows */
static int prow;              /* the chosen row on the performance page: mode, corner, the four parts */
static uint32_t changes;      /* counts every change of what shows */
static int toast_wanted;      /* the note (toast_msg, top right) starts at the next look */
static uint64_t toast_until;  /* ms */
static int hover_kind = -1, hover_a, hover_b;  /* what the mouse is over (HIT_*, -1 nothing), for the hover looks */
static int ctrl_first, ctrl_rows, ctrl_visible;  /* the Controls table: its first row shown, rows, rows that fit */
static int preview, preview_hidden;  /* Preview: the game runs under the bar; the bar hidden (only its chip shows) */
static float pv_rect[4];             /* the part of the overlay the window shows while previewing (design units) */
static int bug_page, brow;           /* Report a bug: its page and chosen row (below, with the rest of it) */

/* name entry (gamemenu.h): the popup's state; the game's thread waits in swkbd.c meanwhile */
static int prompting, prompt_finished;  /* prompt_finished: 1 answered, 2 cancelled */
static TextField prompt_text;
static char prompt_title[256], prompt_fallback[64], prompt_answer[TEXTIN_MAX];
static char prompt_body[512];  /* a message instead of a field ("" for a field) */
static const char *prompt_label = "Name";
static int prompt_min, prompt_cancel;  /* letters the game needs at least; Cancel offered */
static void changed(void);

int gamemenu_prompting(void) { return __atomic_load_n(&prompting, __ATOMIC_ACQUIRE); }

/* letters in the field, or in the default when it is empty */
static int prompt_chars(void) {
    char t[TEXTIN_MAX];
    textin_read(&prompt_text, t, sizeof t, NULL);
    const char *s = t[0] ? t : prompt_fallback;
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}
static int prompt_enough(void) { return prompt_body[0] || prompt_chars() >= (prompt_min > 0 ? prompt_min : 1); }

void gamemenu_prompt_options(int min_chars, int cancel) { prompt_min = min_chars, prompt_cancel = cancel; }

/* The on-screen keyboard (owner, 2026-11-07: "make it simple"): a letter grid in the popup while a controller is in
 * use (the hints follow the last device), as the Switch's own keyboard. D-pad moves, A types, B deletes (on an
 * empty field: Cancel where offered), Y capitals on or off, Done answers. The first letter is a
 * capital, the next ones small, as names are written. The mouse can press its keys too. */
enum { OSK_COLS = 10, OSK_ROWS = 5 };
static const char *const osk_keys[OSK_ROWS - 1] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.'!", "1234567890"};
enum { OSK_SHIFT, OSK_SPACE, OSK_DELETE, OSK_DONE };  /* the last row: its keys and the columns they cover */
static const int osk_last[4][2] = {{0, 2}, {2, 6}, {6, 8}, {8, 10}};
static int osk_row, osk_col, osk_caps = 1, osk_shown;

static int osk_last_key(int col) {
    for (int k = 0; k < 4; k++)
        if (col >= osk_last[k][0] && col < osk_last[k][1]) return k;
    return OSK_DONE;
}

static void prompt_finish(void);
static void osk_press(void) {
    if (osk_row < OSK_ROWS - 1) {
        char ch[2] = {osk_keys[osk_row][osk_col], 0};
        if (!osk_caps && ch[0] >= 'A' && ch[0] <= 'Z') ch[0] = (char)(ch[0] + 32);
        textin_focus(&prompt_text);
        textin_type(ch);
        if (ch[0] != ' ') osk_caps = 0;  /* a capital first, then small letters */
        return;
    }
    switch (osk_last_key(osk_col)) {
    case OSK_SHIFT: osk_caps ^= 1; break;
    case OSK_SPACE: textin_focus(&prompt_text), textin_type(" "); break;
    case OSK_DELETE: textin_focus(&prompt_text), textin_edit(TI_BACK); break;
    case OSK_DONE: prompt_finish(); break;
    }
}

static void osk_move(int a) {
    if (a == GM_UP) osk_row = (osk_row + OSK_ROWS - 1) % OSK_ROWS;
    else if (a == GM_DOWN) osk_row = (osk_row + 1) % OSK_ROWS;
    else if (osk_row == OSK_ROWS - 1) {  /* the last row moves key by key */
        int k = osk_last_key(osk_col);
        k = a == GM_LEFT ? (k + 3) % 4 : (k + 1) % 4;
        osk_col = osk_last[k][0];
    } else osk_col = a == GM_LEFT ? (osk_col + OSK_COLS - 1) % OSK_COLS : (osk_col + 1) % OSK_COLS;
}

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
    osk_row = 0, osk_col = 0, osk_caps = 1;
    __atomic_store_n(&prompt_finished, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&prompting, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    textin_focus(&prompt_text);  /* typing starts at once */
    changed();
    fprintf(stderr, "yw4: overlay: name entry \"%s\" (at most %d letters)\n", prompt_title, max_chars);
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

static void prompt_close(int how) {
    textin_focus(NULL);
    __atomic_store_n(&prompting, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&open_, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&prompt_finished, how, __ATOMIC_RELEASE);
    changed();
}

static void prompt_finish(void) {
    if (!gamemenu_prompting()) return;
    if (!prompt_enough()) {  /* too short for the game (a password needs a letter): keep typing */
        textin_focus(&prompt_text);
        changed();
        return;
    }
    textin_read(&prompt_text, prompt_answer, sizeof prompt_answer, NULL);
    if (!prompt_answer[0]) snprintf(prompt_answer, sizeof prompt_answer, "%s", prompt_fallback);
    prompt_close(1);
}

void gamemenu_prompt_cancel(void) {
    if (gamemenu_prompting() && (prompt_cancel || prompt_body[0])) prompt_close(prompt_body[0] ? 1 : 2);
}

int gamemenu_prompt_take(char *out, size_t n) {
    /* Enter or Tab end the typing (window.c): that is OK here, as is the pad's A or the OK button */
    if (gamemenu_prompting() && !prompt_body[0] && !textin_has(&prompt_text)) prompt_finish();
    int how = __atomic_exchange_n(&prompt_finished, 0, __ATOMIC_ACQ_REL);
    if (how == 1) snprintf(out, n, "%s", prompt_answer);
    return how;
}

int gamemenu_open(void) { return __atomic_load_n(&open_, __ATOMIC_ACQUIRE); }
int gamemenu_previewing(void) { return __atomic_load_n(&preview, __ATOMIC_ACQUIRE); }
void gamemenu_preview_rect(float r[4]) { memcpy(r, pv_rect, sizeof pv_rect); }
int gamemenu_selected(void) { return sel; }
int gamemenu_confirming(void) { return confirm; }

static void changed(void) { __atomic_add_fetch(&changes, 1, __ATOMIC_RELEASE); }

uint32_t gamemenu_look(uint64_t now) {
    if (__atomic_exchange_n(&toast_wanted, 0, __ATOMIC_ACQ_REL)) toast_until = now + 1600;
    /* also when a key capture ends by itself (five seconds) */
    return (__atomic_load_n(&changes, __ATOMIC_ACQUIRE) + textin_changes()) * 4u + (now < toast_until) +
           2u * (uint32_t)bindings_capturing();
}

static void close_and_resume(void) {
    if (settings_open()) settings_toggle();
    bindings_cancel();
    textin_focus(NULL);  /* no typing into a closed overlay (what was typed stays for next time) */
    confirm = 0, focus = 0;
    __atomic_store_n(&preview, 0, __ATOMIC_RELEASE);  /* the overlay button in the preview: the game runs on */
    __atomic_store_n(&open_, 0, __ATOMIC_RELEASE);
    if (window_paused()) window_pause_toggle();
    changed();
    fprintf(stderr, "yw4: overlay: closed\n");
}

void gamemenu_toggle(void) {
    if (gamemenu_prompting()) return;  /* the name popup stays until it is answered */
    if (gamemenu_open()) {
        close_and_resume();
        return;
    }
    sel = GM_RESUME, confirm = 0, focus = 0;
    if (!window_paused()) window_pause_toggle();  /* F4 may have paused it already */
    __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    changed();
    fprintf(stderr, "yw4: overlay: open\n");
}

static void accept(void) {
    switch (sel) {
    case GM_RESUME: close_and_resume(); break;
    case GM_SETTINGS: focus = 1, in_rows = 0; break;  /* into its page: its sections first */
    case GM_CONTROLS: focus = 1; break;
    case GM_PERF: focus = 1; break;
    case GM_BUG: focus = 1, bug_page = 0, brow = 0; break;  /* the report's first page (below) */
    case GM_EXIT: confirm = 1, choice = 0; break;  /* "keep playing" chosen first */
    }
}

/* Preview: the graphics sections only (Advanced has nothing to see) */
static int previewable(const char *category) { return strcmp(category, "ADVANCED") != 0; }

static int can_preview(void) {  /* the Settings page's chosen section has something to see */
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    return n > 0 && previewable(rows[srow < n ? srow : n - 1].category);
}

static void preview_start(void) {
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    if (n <= 0) return;
    if (srow >= n) srow = n - 1;
    if (!previewable(rows[srow].category)) return;
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

/* in the preview: Up / Down the section's rows, Left / Right (Enter) change, PREV / NEXT the graphics sections,
 * ALT hides or shows the bar, Back returns to the Settings page */
static void preview_action(int a) {
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    if (n <= 0) return;
    if (srow >= n) srow = n - 1;
    int first = srow, last = srow;
    while (first > 0 && !strcmp(rows[first - 1].category, rows[srow].category)) first--;
    while (last < n - 1 && !strcmp(rows[last + 1].category, rows[srow].category)) last++;
    if (a == GM_UP && srow > first) srow--;
    else if (a == GM_DOWN && srow < last) srow++;
    else if (a == GM_LEFT || a == GM_RIGHT) settings_change(srow, a == GM_RIGHT ? 1 : -1);
    else if (a == GM_ACCEPT) settings_change(srow, 1);
    else if (a == GM_ALT) preview_hidden ^= 1;
    else if (a == GM_BACK || a == GM_LEAVE) preview_stop();
    else if (a == GM_PREV) {  /* the graphics section before, its first row */
        int r = first - 1;
        while (r >= 0 && !previewable(rows[r].category)) r--;
        if (r < 0) return;
        while (r > 0 && !strcmp(rows[r - 1].category, rows[r].category)) r--;
        srow = r;
    } else if (a == GM_NEXT) {
        int r = last + 1;
        while (r < n && !previewable(rows[r].category)) r++;
        if (r < n) srow = r;
    }
}

/* the settings page: rows run on from one section into the next; PREV / NEXT jump to a section's first row */
static void settings_page_action(int a) {
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
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

/* the controls page: the controls' own selection and key capture (bindings.h), as the F2 page's */
static void controls_page_action(int a) {
    static const int map[] = {[GM_UP] = SETTINGS_UP, [GM_DOWN] = SETTINGS_DOWN, [GM_LEFT] = SETTINGS_LEFT,
                              [GM_RIGHT] = SETTINGS_RIGHT, [GM_ACCEPT] = SETTINGS_ACCEPT, [GM_CLEAR] = SETTINGS_CLEAR};
    if (a == GM_BACK || a == GM_LEAVE) {
        bindings_cancel();
        focus = 0, sel = GM_RESUME;
    } else if (a == GM_UP || a == GM_DOWN || a == GM_LEFT || a == GM_RIGHT || a == GM_ACCEPT || a == GM_CLEAR) {
        bindings_action(map[a]);
    }
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
    if (gamemenu_prompting()) {  /* the name popup: A / Enter is OK (typing has ended), the rest nothing */
        if (osk_shown && !prompt_body[0]) {  /* the on-screen keyboard (a controller) */
            char t[8];
            textin_read(&prompt_text, t, sizeof t, NULL);
            if (a == GM_UP || a == GM_DOWN || a == GM_LEFT || a == GM_RIGHT) osk_move(a);
            else if (a == GM_ACCEPT) osk_press();
            else if (a == GM_ALT) osk_caps ^= 1;
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
            fprintf(stderr, "yw4: overlay: exit game\n");
            exit(0);  /* as closing the window */
        }
        return;
    }
    if (preview) {
        preview_action(a);
        return;
    }
    if (focus) {
        if (sel == GM_CONTROLS) controls_page_action(a);
        else if (sel == GM_PERF) perf_page_action(a);
        else if (sel == GM_BUG) bug_page_action(a);
        else settings_page_action(a);
        return;
    }
    if (a == GM_UP) sel = sel > 0 ? sel - 1 : 0;
    else if (a == GM_DOWN) sel = sel < GM_ITEMS - 1 ? sel + 1 : GM_ITEMS - 1;
    else if (a == GM_ACCEPT ||
             (a == GM_RIGHT && (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_PERF || sel == GM_BUG)))
        accept();
    else if (a == GM_BACK) close_and_resume();  /* Escape, Backspace, B on the side panel: back to the game */
}

void gamemenu_refresh(void) { changed(); }

/* ---- the mouse: where things are, as the last drawing put them (design units). The present draws, the window's
 * thread reads: two lists, the drawing fills the one not being read, then swaps. */

enum { HIT_SIDE, HIT_SECTION, HIT_ROW, HIT_ROW_PREV, HIT_ROW_NEXT, HIT_BIND, HIT_MODE, HIT_CORNER, HIT_CORNER_BOX,
       HIT_PART, HIT_SLIDER, HIT_POPUP, HIT_HINT, HIT_BIND_STEP, HIT_PREVIEW,
       HIT_BUG_KIND, HIT_BUG_TEXT, HIT_BUG_OFTEN, HIT_BUG_NEXT, HIT_BUG_TICK, HIT_BUG_BTN, HIT_BUG_DONE,
       HIT_PROMPT_FIELD, HIT_PROMPT_OK, HIT_PROMPT_CANCEL, HIT_OSK_KEY };
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

void gamemenu_mouse(float x, float y, int button) {
    if (!gamemenu_open() || settings_open() || bindings_capturing()) return;
    Hit h;
    int found = hit_find(x, y, &h);
    if (gamemenu_prompting()) {  /* the name popup: its field takes the typing again, OK finishes */
        if (button == 1 && found && h.kind == HIT_PROMPT_FIELD) textin_focus(&prompt_text), changed();
        if (button == 1 && found && h.kind == HIT_PROMPT_OK) prompt_finish();
        if (button == 1 && found && h.kind == HIT_PROMPT_CANCEL) gamemenu_prompt_cancel();
        if (found && h.kind == HIT_OSK_KEY && (button == 0 || button == 1)) {  /* the on-screen keyboard's keys */
            if (osk_row != h.a || osk_col != h.b) osk_row = h.a, osk_col = h.b, changed();
            if (button == 1) osk_press(), changed();
        }
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
    if (confirm && (!found || h.kind != HIT_POPUP)) return;  /* the popup covers the rest */
    if (button == 1 && textin_has(&bug_text) && (!found || h.kind != HIT_BUG_TEXT)) textin_edit(TI_DONE), changed();
    if (!found) return;
    int click = button == 1, drag = button == 4;  /* 4: moved with the left button held (a slider's thumb) */
    int before = sel * 1000 + focus * 100 + srow + prow * 37 + choice * 7 + in_rows * 500 + brow * 11 + bug_page * 3;
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
        srow = h.a, focus = 1, in_rows = 1;
        if (click) settings_change(h.a, h.kind == HIT_ROW_PREV ? -1 : 1);
        break;
    case HIT_BIND:
        focus = 1, bindings_select(h.a, h.b < 0 ? -1 : h.b);
        if (click && h.b != -2) bindings_action(SETTINGS_ACCEPT);  /* a key: waits for the new one; a value: the next */        break;
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
        if (click) focus = 1, brow = 1, textin_focus(&bug_text);
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
    if (click || sel * 1000 + focus * 100 + srow + prow * 37 + choice * 7 + in_rows * 500 + brow * 11 + bug_page * 3 != before)
        changed();
}

void gamemenu_wheel(int dy) {
    /* only a page that scrolls takes the wheel (the Controls table): its rows shown, one a notch; elsewhere it does
     * nothing (owner, 2026-11-06: the wheel scrolls, it doesn't choose) */
    if (!gamemenu_open() || settings_open() || bindings_capturing() || confirm || sel != GM_CONTROLS) return;
    if (ctrl_visible <= 0 || ctrl_rows <= ctrl_visible) return;  /* not drawn yet, or it all fits */
    int f = ctrl_first + (dy > 0 ? -1 : 1);  /* one row a notch (owner: no fast scroll) */
    if (f > ctrl_rows - ctrl_visible) f = ctrl_rows - ctrl_visible;
    if (f < 0) f = 0;
    ctrl_first = f;
    /* the chosen row stays among the ones shown (else the drawing would scroll back to it) */
    static BindRow rows[64];
    char help[8];
    int row, col;
    bindings_rows(rows, 64, &row, &col, help, sizeof help);
    if (row < f) bindings_select(f, -1);
    else if (row >= f + ctrl_visible) bindings_select(f + ctrl_visible - 1, -1);
    hover_kind = -1;
    changed();
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
/* a bug: its body, a line down its back, legs and feelers */
static const char *const ICON_BUG =
    "<path d=\"M8 10a4 4 0 0 1 8 0v4a4 4 0 0 1-8 0zM12 10v8M4 12h4M16 12h4M5 7.5l3 2M19 7.5l-3 2M5 18.5l3-2M19 18.5"
    "l-3-2M10 6.5L8.5 4M14 6.5L15.5 4\"/>";
static const char *const ICON_EXIT = "<path d=\"M12 3v9M6.3 6.3a8 8 0 1 0 11.4 0\"/>";
/* the value arrows (‹ ›): drawn as icons, not the font's characters, which sit low in their line */
static const char *const ICON_PREV = "<path d=\"M14.5 6l-6 6 6 6\"/>";
static const char *const ICON_NEXT = "<path d=\"M9.5 6l6 6-6 6\"/>";
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
    {"Touchpad", {"<rect x=\"3\" y=\"6\" width=\"18\" height=\"12\" rx=\"3\"/><path d=\"M12 6v5\"/>"}},
    {"Share", {"<path d=\"M12 3v11M8 7l4-4 4 4M5 11v9h14v-9\"/>"}},
    {"Capture", {"<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\"/><circle cx=\"12\" cy=\"12\" r=\"4\"/>"}},
    {"Back + Start", {"<rect x=\"4\" y=\"5\" width=\"9\" height=\"9\" rx=\"1\"/><rect x=\"11\" y=\"10\" width=\"9\" "
                      "height=\"9\" rx=\"1\"/>",
                      "<path d=\"M5 7h14M5 12h14M5 17h14\"/>"}},
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
static float popup_button(const UiCanvas *c, float right, float y, const char *label, int quit, int on) {
    int f = quit ? UI_ROUNDED_800 : UI_ROUNDED_700;
    float w = ui_text_w(f, 14, label) + 40;
    float x = right - w;
    hit_add(x, y, w, 40, HIT_POPUP, quit, 0);
    if (on) ui_box(c, x - 4, y - 4, w + 8, 48, 24, RING, 0, 0);
    if (quit) ui_box(c, x, y, w, 40, 20, WINE, 0, 0);
    else ui_box(c, x, y, w, 40, 20, WHITE, 1, POP_LINE);
    ui_text_in(c, f, 14, x, y, w, 40, 1, label, quit ? SNOW : PLUM);
    return w;
}

/* The settings' texts are upper case (the F1 panel's pixel font has no lower case); the design writes names,
 * options and sections in sentence case and keeps acronyms: "WINDOW MODE" -> "Window mode", "X16" -> "x16". */
static void sentence_case(const char *in, char *out, size_t n) {
    static const char *const keep[] = {"HUD", "FXAA", "SMAA", "TAA", "FSR", "DLSS", "GPU", "GPUS", "CPU", "AMD",
                                       "NVIDIA", "RTX", "HD", "MS", "F3", "F11", "AI", "ZL", "ZR", "LB", "RB", "LT",
                                       "RT", "L1", "R1", "L2", "R2", "L3", "R3", "PS"};
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

/* The settings page (the design's isSettings section): the sections on the left (the chosen row's lit), its
 * section's rows with the value, its note and arrows, the chosen row's description at the bottom. */
static void draw_settings_page(const UiCanvas *c, const GameMenuInfo *in) {
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    if (n <= 0) return;
    int cur = srow < n ? srow : n - 1;
    const char *sec = rows[cur].category;
    char t[160];
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Settings", NIGHT);
    {  /* Preview, at the right of the title (canvas board "Settings page: the Preview button"): an eye, its name, its
        * key; greyed where there is nothing to see (Advanced) */
        int can = previewable(sec);
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
    /* its rows */
    float x = 812, w = 700;
    y = top;
    for (int i = 0; i < n; i++) {
        if (strcmp(rows[i].category, sec)) continue;
        const SettingRow *r = &rows[i];
        int on = focus && in_rows && i == cur, off = r->disabled != NULL;
        ui_box(c, x, y, w, 72, 14, WHITE, on ? 3 : 2, on ? RING : LINE);
        hit_add(x, y, w, 72, HIT_ROW, i, 0);
        sentence_case(r->name, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_500, 18, x + 20, y, 0, 72, 0, t, off ? MUTED : INK);
        float nx = x + w - 12 - 44, vx = nx - 10 - 300, px = vx - 10 - 44, by = y + 14;
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
    /* the chosen row's description */
    const SettingRow *r = &rows[cur];
    char desc[600], dprose[600], dname[160];
    if (r->disabled) snprintf(desc, sizeof desc, "%s OFF FOR NOW: %s", r->help, r->disabled);
    else snprintf(desc, sizeof desc, "%s", r->help);
    prose_case(desc, dprose, sizeof dprose);
    sentence_case(r->name, dname, sizeof dname);
    /* the setting's name in bold white, then what it does as sentences (no gold text: the owner's rule) */
    float lh = 15 * 1.5f, dh = lh + 4 + ui_text_wrap(NULL, UI_GOTHIC_500, 15, 0, 0, 0, w - 40, lh, 0, dprose, 0) + 32;
    ui_box(c, x, bottom - dh, w, dh, 14, NIGHT, 0, 0);
    ui_text(c, UI_ROUNDED_800, 15, x + 20, bottom - dh + 16, lh, dname, SNOW);
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x + 20, bottom - dh + 16 + lh + 4, w - 40, lh, 0, dprose, ON_DARK);
}

/* A binding's name (bindings.c pad_label, key names) with its symbols drawn as icons, as the old F2 page's pixel
 * font drew them: \1..\4 the arrows (up, down, left, right), \5..\8 PlayStation's triangle, circle, square, cross.
 * The rest in sentence case. Drawn from x, centred in [y, y + h]; returns its width. c NULL: only measures. */
static float cell_text(const UiCanvas *c, float x, float y, float h, const char *cell, uint32_t rgb) {
    static const char *const sym[9] = {
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
    float w = 0;
    for (const char *p = cell; *p;) {
        if (*p >= 1 && *p <= 8) {  /* a symbol: 20 units, a little space after when text follows */
            if (c) ui_icon(c, sym[(int)*p], x + w, y + (h - 20) / 2, 20, 2.4f, rgb);
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

/* The controls page (the design's isControls section): a table of each Switch input with its two keys and its
 * controller input, the chosen row ringed in gold and the chosen key or input on a gold chip; the rows scroll.
 * Under it, what the chosen row does (the design's note line). */
static void draw_controls_page(const UiCanvas *c, const GameMenuInfo *in) {
    static BindRow rows[64];
    char help[512], t[160], u[160];
    int row, col, n = bindings_rows(rows, 64, &row, &col, help, sizeof help);
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Controls", NIGHT);
    if (in->pad_name) snprintf(t, sizeof t, "%s connected", in->pad_name);
    else snprintf(t, sizeof t, "No controller connected");
    ui_text_in(c, UI_GOTHIC_400, 16, 488, 76 + 12, 1024, 34 * 1.25f - 12, 2, t, MUTED);
    float top = 76 + 34 * 1.25f + 22, bottom = 40 + 764 - 36;
    float hh = ui_text_wrap(NULL, UI_GOTHIC_500, 15, 0, 0, 0, 1024, 15 * 1.45f, 0, help, 0);
    float x = 488, w = 1024, cw[3] = {379, 379, 266}, hdr = 16 * 1.45f + 28, rh = 17 * 1.45f + 24;
    int visible = (int)((bottom - hh - 22 - top - hdr - 4) / rh);
    if (visible < 1) visible = 1;
    float th = hdr + visible * rh + 4;
    /* the rows shown: they move only when the chosen row leaves them (keys, the pad), or by the wheel
     * (gamemenu_wheel), never because the mouse hovers a row, so the table doesn't run away under the cursor */
    if (row < ctrl_first) ctrl_first = row;
    if (row >= ctrl_first + visible) ctrl_first = row - visible + 1;
    if (ctrl_first > n - visible) ctrl_first = n - visible;
    if (ctrl_first < 0) ctrl_first = 0;
    ctrl_rows = n, ctrl_visible = visible;
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
        ui_text_in(c, UI_GOTHIC_400, 17, x + 22, y, 0, rh, 0, t, INK);
        if (!r->group) continue;
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
            if (on && col == k) ui_box(c, kx - 10, y + 8, tw + 20, rh - 16, 8, GOLD, 0, 0);
            cell_text(c, kx, y, rh, cell, r->conflict[k] ? WINE : !strcmp(r->cells[k], "-") ? MUTED : INK);
        }
    }
    char hprose[512];
    prose_case(help, hprose, sizeof hprose);
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x, top + th + 22, w, 15 * 1.45f, 0, hprose, MUTED);
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
        int cur;
        textin_read(&bug_text, text, sizeof text, &cur);
        float tlh = 18 * 1.5f;
        if (!text[0] && !typing) {
            ui_text(c, UI_GOTHIC_500, 18, x + 20, y + 16, tlh, "For example: Whisper has no shadow on the school field.",
                    MUTED);
            if (ring)
                ui_text(c, UI_GOTHIC_700, 15, x + 20, y + bh - 16 - lh, lh,
                        in->pad ? "Press A to type with a keyboard" : "Press Enter to type", PLUM);
        } else {
            ui_text_edit(c, UI_GOTHIC_500, 18, x + 20, y + 16, w - 40, tlh, (int)((bh - 32) / tlh), text,
                         typing ? cur : -1, INK, PLUM);
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
static void draw_preview(const UiCanvas *c, const GameMenuInfo *in) {
    float W = (float)c->w / c->s, H = 900;
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    if (n <= 0) return;
    int cur = srow < n ? srow : n - 1;
    int first = cur, last = cur;
    while (first > 0 && !strcmp(rows[first - 1].category, rows[cur].category)) first--;
    while (last < n - 1 && !strcmp(rows[last + 1].category, rows[cur].category)) last++;
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
        if (!previewable(rows[i].category) || (i && !strcmp(rows[i].category, rows[i - 1].category))) continue;
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
    if (r->disabled) snprintf(desc, sizeof desc, "OFF FOR NOW: %s.", r->disabled);
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

void gamemenu_draw(const UiCanvas *c, const GameMenuInfo *in) {
    TH = theme_colors();
    float W = (float)c->w / c->s, H = 900;
    int hk = hit_back();
    hit_n[hk] = 0;  /* the mouse's list, filled as things are drawn */
    int req = __atomic_exchange_n(&bug_request, 0, __ATOMIC_ACQ_REL);
    if (req) {  /* Save only / Send (bug_finish): here, where the paused picture is */
        textin_read(&bug_text, bug_sent_text, sizeof bug_sent_text, NULL);
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
    if (gamemenu_prompting()) {  /* name entry (board N1): only the popup, over the game's waiting picture */
        ui_rect(c, 0, 0, W, H, POP_BACKDROP, 0.45f);
        float pw = 650, bar = 18 + 22 * 1.3f + 16, fh = 72, llh = 16 * 1.45f;
        float blh = 17 * 1.5f, msg_h = prompt_body[0] ? ui_text_wrap(NULL, UI_GOTHIC_500, 17, 0, 0, 0, pw - 56, blh, 0,
                                                                      prompt_body, 0) : 0;
        static int osk_force = -1;  /* YW4_OSK=1: the keyboard with any device (tests, touch screens) */
        if (osk_force < 0) osk_force = getenv("YW4_OSK") && *getenv("YW4_OSK") == '1';
        osk_shown = (in->pad || osk_force) && !prompt_body[0];
        float kh = 50, kgap = 8, osk_h = osk_shown ? 14 + OSK_ROWS * (kh + kgap) : 0;
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
        int cur, chars = 0;
        textin_read(&prompt_text, text, sizeof text, &cur);
        for (const char *p = text; *p; p++) chars += ((unsigned char)*p & 0xC0) != 0x80;
        ui_box(c, bx, y, bw, fh, 14, WHITE, typing ? 3 : 2, typing ? PLUM : LINE);
        hit_add(bx, y, bw, fh, HIT_PROMPT_FIELD, 0, 0);
        char count[24] = "";
        if (prompt_text.max_chars > 0) snprintf(count, sizeof count, "%d / %d", chars, prompt_text.max_chars);
        float cw = ui_text_w(UI_GOTHIC_700, 17, count);
        ui_text_in(c, UI_GOTHIC_700, 17, bx + bw - 20 - cw, y, cw, fh, 2, count, MUTED);
        if (!text[0]) ui_text_in(c, UI_ROUNDED_800, 32, bx + 20 + 6, y, 0, fh, 0, prompt_fallback, LILAC);  /* the default */
        ui_text_edit(c, UI_ROUNDED_800, 32, bx + 20, y + (fh - 32 * 1.3f) / 2, bw - 40 - cw - 16, 32 * 1.3f, 1, text,
                     typing ? cur : -1, INK, PLUM);
        y += fh + 12;
        char help[128], limit[32] = "";
        if (prompt_text.max_chars > 0) snprintf(limit, sizeof limit, " Up to %d letters.", prompt_text.max_chars);
        snprintf(help, sizeof help, "%s%s%s%s%s", osk_shown ? "Pick the letters below." : "Type with your keyboard.", limit,
                 prompt_fallback[0] ? " Leave it empty for " : "", prompt_fallback, prompt_fallback[0] ? "." : "");
        ui_text(c, UI_GOTHIC_500, 16, bx, y, llh, help, MUTED);
        if (osk_shown) {  /* the on-screen keyboard: ten columns, the chosen key gold */
            float ky = y + llh + 14, kw = (bw - (OSK_COLS - 1) * kgap) / OSK_COLS;
            for (int r = 0; r < OSK_ROWS; r++, ky += kh + kgap) {
                int nkeys = r < OSK_ROWS - 1 ? OSK_COLS : 4;
                for (int k = 0; k < nkeys; k++) {
                    int c0 = r < OSK_ROWS - 1 ? k : osk_last[k][0], c1 = r < OSK_ROWS - 1 ? k + 1 : osk_last[k][1];
                    float kx = bx + (float)c0 * (kw + kgap), w2 = (float)(c1 - c0) * (kw + kgap) - kgap;
                    int on = osk_row == r && osk_col >= c0 && osk_col < c1;
                    char label[16];
                    if (r < OSK_ROWS - 1) {
                        label[0] = osk_keys[r][k], label[1] = 0;
                        if (!osk_caps && label[0] >= 'A' && label[0] <= 'Z') label[0] = (char)(label[0] + 32);
                    } else {
                        static const char *const names[4] = {"Shift", "Space", "Delete", "Done"};
                        snprintf(label, sizeof label, "%s", names[k]);
                    }
                    int shift_on = r == OSK_ROWS - 1 && k == OSK_SHIFT && osk_caps, done = r == OSK_ROWS - 1 && k == OSK_DONE;
                    uint32_t fill = on ? GOLD : done ? PLUM : shift_on ? LILAC : WHITE;
                    ui_box(c, kx, ky, w2, kh, 10, fill, on ? 0 : 2, LINE);
                    ui_text_in(c, UI_ROUNDED_800, r < OSK_ROWS - 1 ? 22 : 17, kx, ky, w2, kh, 1, label,
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
        ui_rect(c, 0, H - 56, W, 56, NIGHT, 1);  /* the key hints, the whole width */
        float hx = 48;
        if (in->pad && osk_shown) {  /* the on-screen keyboard */
            int ps = in->pad == 2;
            hx += hint(c, hx, H - 56, "D-pad", "Move") + 28;
            hx += hint(c, hx, H - 56, ps ? "Cross" : "A", "Type") + 28;
            hx += hint(c, hx, H - 56, ps ? "Circle" : "B", can_cancel ? "Delete / Cancel" : "Delete") + 28;
            hint(c, hx, H - 56, ps ? "Triangle" : "Y", "Capitals");
        } else if (in->pad) {
            hx += hint(c, hx, H - 56, in->pad == 2 ? "Cross" : "A", "OK") + 28;
            if (can_cancel) hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Cancel");
        } else {
            hx += hint(c, hx, H - 56, "Enter", "OK") + 28;
            if (can_cancel) hx += hint(c, hx, H - 56, "Esc", "Cancel") + 28;
            if (!prompt_body[0]) {
                hx += hint(c, hx, H - 56, "Backspace", "Delete") + 28;
                hint(c, hx, H - 56, "Ctrl V", "Paste");
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
    int page = (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_PERF || sel == GM_BUG) && !confirm;  /* these show a page */
    ui_rect(c, 0, 0, W, H, SHADE, page ? 0.62f : 0.35f);  /* the tint over the game, darker behind a page */
    if (page && sel == GM_SETTINGS) draw_settings_page(c, in);
    if (page && sel == GM_CONTROLS) draw_controls_page(c, in);
    if (page && sel == GM_PERF) draw_perf_page(c, in);
    if (page && sel == GM_BUG) draw_bug_page(c, in);

    /* the side panel: 400 wide with a 4 px gold edge towards the game */
    ui_rect(c, 0, 0, 400, H, PAPER, 1);
    ui_rect(c, 396, 0, 4, H, GOLD, 1);
    ui_rect(c, 0, 0, 396, 108, NIGHT, 1);  /* the header: padding 28 / 24 around the 56 px logo */
    if (in->logo) {  /* the game's own icon (gamelogo.c), else the design's clock */
        ui_image(c, in->logo, in->logo_w, in->logo_h, 28, 28, 56, 56, 14);
    } else {
        ui_box(c, 28, 28, 56, 56, 14, GOLD, 0, 0);
        ui_icon(c, ICON_CLOCK, 28 + 13, 28 + 13, 30, 2.2f, NIGHT);
    }
    float th = 21 * 1.2f, sh = 14 * 1.45f, ty = 28 + (56 - (th + 4 + sh)) / 2;
    ui_text(c, UI_ROUNDED_800, 21, 100, ty, th, "Yo-kai Watch 4 Recomp", SNOW);
    ui_text(c, UI_GOTHIC_400, 14, 100, ty + th + 4, sh, "Game version " GAME_VERSION, ON_DARK);

    static const char *const labels[] = {"Resume", "Settings", "Controls", "Overlay", "Report a bug"};
    const char *icons[] = {ICON_RESUME, ICON_SETTINGS, ICON_CONTROLS, ICON_PERF, ICON_BUG};
    for (int i = 0; i < 5; i++) {
        const char *l = labels[i];
        side_button(c, 20, 128 + i * 66, 356, icons[i], l, sel == i, PLUM);
        hit_add(20, 128 + i * 66, 356, 56, HIT_SIDE, i, 0);
    }
    /* Exit game at the bottom, under a line; the version under it */
    float vy = H - 20 - 19, ey = vy - 14 - 56;
    ui_rect(c, 20, ey - 14 - 2, 356, 2, LINE, 1);
    side_button(c, 20, ey, 356, ICON_EXIT, "Exit game", sel == GM_EXIT, WINE);
    hit_add(20, ey, 356, 56, HIT_SIDE, GM_EXIT, 0);
    char ver[48];
    snprintf(ver, sizeof ver, "YW4 Recomp %s", in->version);
    ui_text(c, UI_GOTHIC_400, 13, 20, vy, 19, ver, MUTED);

    /* the key hints along the bottom */
    ui_rect(c, 400, H - 56, W - 400, 56, NIGHT, 1);
    float hx = 448;
    if (focus && sel == GM_BUG && textin_has(&bug_text)) {  /* typing in the report */
        if (in->pad) {
            hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Done typing");
        } else {
            hx += hint(c, hx, H - 56, "Enter", "Done") + 28;
            hint(c, hx, H - 56, "Ctrl V", "Paste");
        }
    } else if (focus && sel == GM_CONTROLS) {  /* the controls page */
        int ps = in->pad == 2;
        hx += hint(c, hx, H - 56, in->pad ? "D-pad" : "W S", "Choose") + 28;
        if (!in->pad) hx += hint(c, hx, H - 56, "A D", "Navigate") + 28;  /* across: a key, the other key, the pad */
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Cross" : "A") : "Enter", "Change") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Square" : "X") : "Del", "Clear") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Circle" : "B") : "Esc", "Back") + 28;
        if (in->pad) hint(c, hx, H - 56, in->close, "Close overlay");
    } else if (focus && sel == GM_SETTINGS && !in_rows) {  /* the settings page's sections */
        hx += hint(c, hx, H - 56, in->pad ? "D-pad" : "W S", "Choose section") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (in->pad == 2 ? "Cross" : "A") : "Enter", "Open") + 28;
        if (can_preview()) hx += hint(c, hx, H - 56, in->pad ? alt_button(in) : "P", "Preview") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (in->pad == 2 ? "Circle" : "B") : "Esc", "Back") + 28;
        if (in->pad) hint(c, hx, H - 56, in->close, "Close overlay");
    } else if (focus && in->pad) {  /* on a page */
        hx += hint(c, hx, H - 56, "D-pad", "Choose and change") + 28;
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Cross" : "A", "Select") + 28;
        if (sel == GM_SETTINGS) hx += hint(c, hx, H - 56, in->pad == 2 ? "L1 R1" : "LB RB", "Section") + 28;
        if (sel == GM_SETTINGS && can_preview()) hx += hint(c, hx, H - 56, alt_button(in), "Preview") + 28;
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Back") + 28;
        hint(c, hx, H - 56, in->close, "Close overlay");
    } else if (focus) {  /* on a page, with the keyboard: Escape goes back to the side panel, again closes */
        hx += hint(c, hx, H - 56, "W S", "Move") + 28;
        hx += hint(c, hx, H - 56, "A D", "Change") + 28;
        hx += hint(c, hx, H - 56, "Enter", "Select") + 28;
        if (sel == GM_SETTINGS) hx += hint(c, hx, H - 56, "Q E", "Section") + 28;
        if (sel == GM_SETTINGS && can_preview()) hx += hint(c, hx, H - 56, "P", "Preview") + 28;
        hint(c, hx, H - 56, "Esc", "Back");
    } else if (in->pad) {
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Cross" : "A", "Select") + 28;
        hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Close overlay");
    } else {
        hx += hint(c, hx, H - 56, "W S", "Move") + 28;
        hx += hint(c, hx, H - 56, "Enter", "Select") + 28;
        hint(c, hx, H - 56, "Esc", "Close overlay");
    }

    /* a short note (toast_msg): top right */
    if (in->now < toast_until) {
        const char *t = toast_msg;
        float tw = ui_text_w(UI_GOTHIC_700, 17, t), bw = tw + 44 + 6, bh = 17 * 1.45f + 28 + 6;
        ui_box(c, W - 48 - bw, 40, bw, bh, 14, SNOW, 3, RING);
        ui_text_in(c, UI_GOTHIC_700, 17, W - 48 - bw, 40, bw, bh, 1, t, NIGHT);
    }

    /* the exit popup: a small window as the launcher's popups (launcher/ui/style.css "dialog", 2026-11-05): a
     * violet title bar over a gold rule, the text on white, the buttons in a lilac tray at the bottom */
    if (confirm) {
        ui_rect(c, 0, 0, W, H, POP_BACKDROP, 0.6f);
        float pw = 460, lh_title = 17 * 1.3f, bar = 15 + lh_title + 13, lh = 14 * 1.5f, tray = 12 + 40 + 12;
        float ph = bar + 3 + 18 + lh + 18 + tray;
        float px = (W - pw) / 2, py = (H - ph) / 2;
        ui_shadow(c, px, py + 14, pw, ph, 14, 36, POP_SHADOW, 0.45f);
        ui_box_alpha(c, px - 1, py - 1, pw + 2, ph + 2, 15, POP_BORDER, 0.5f);  /* the 1 px border, half seen */
        ui_box(c, px, py, pw, ph, 14, POP_WHITE, 0, 0);
        ui_box(c, px, py, pw, bar + 14, 14, POP_DEEP, 0, 0);  /* the title bar (its lower corners covered below) */
        ui_rect(c, px, py + bar, pw, 3, POP_GOLD, 1);
        float ty = py + ph - tray;
        ui_box(c, px, ty - 14, pw, tray + 14, 14, POP_LILAC, 0, 0);  /* the tray (its upper corners covered) */
        ui_rect(c, px, py + bar + 3, pw, ty - (py + bar + 3), POP_WHITE, 1);
        ui_rect(c, px, ty, pw, 1, POP_LINE, 1);
        ui_text(c, UI_ROUNDED_800, 17, px + 22, py + 15, lh_title, "Quit the game?", POP_WHITE);
        ui_text(c, UI_GOTHIC_400, 14, px + 22, py + bar + 3 + 18, lh, "Progress since your last save is lost.", POP_INK);
        float right = px + pw - 16;
        right -= popup_button(c, right, ty + 12, "Quit game", 1, choice == 1) + 10;
        popup_button(c, right, ty + 12, "Keep playing", 0, choice == 0);
    }
    __atomic_store_n(&hit_front, hk, __ATOMIC_RELEASE);  /* the mouse reads this drawing's list now */
}
