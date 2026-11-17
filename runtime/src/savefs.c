/* Save data on the host: IFileSystem and IFile over a folder (Switchbrew "Filesystem_services").
 *
 * Each save is a folder <save dir>/<program id>/<user id>/. Paths the game sends are
 * Switch paths ("/data/save.bin", at most 0x300 bytes); ".." and backslashes are refused,
 * so nothing outside the save folder can be reached.
 *
 * Simplification: the Switch journals save data, so writes only become permanent at
 * Commit; here they go to the file at once and Commit does nothing. A crash in the middle
 * of a save can therefore leave a half-written file that the Switch would have discarded.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_one(p) _mkdir(p)
#define rmdir_one(p) _rmdir(p)
#define truncate_file(f, n) _chsize_s(_fileno(f), (long long)(n))
#else
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0755)
#define rmdir_one(p) rmdir(p)
#define truncate_file(f, n) ftruncate(fileno(f), (off_t)(n))
#endif

#include "gamemenu.h"
#include "ipc_simple.h"
#include "kernel.h"
#include "pause.h"
#include "savefs.h"
#include "host.h"
#include "window.h"
#include "yw4rt.h"

/* PoC switch for save-slot research (when YW4_DIARY_SLOTS=0). */
static int save_slot_test(void) {
    static int cached = -1;
    int c = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (c < 0) {
        const char *v = getenv("YW4_SAVE_SLOT_TEST");
        c = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
        __atomic_store_n(&cached, c, __ATOMIC_RELAXED);
    }
    return c;
}

/* 10-slot Diary & Autosave system: enabled by default; set YW4_DIARY_SLOTS=0 for vanilla 1-slot behavior. */
static int diary_slots_enabled(void) {
    static int cached = -1;
    int c = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (c < 0) {
        const char *v = getenv("YW4_DIARY_SLOTS");
        c = (v && strcmp(v, "0") == 0) ? 0 : 1;
        __atomic_store_n(&cached, c, __ATOMIC_RELAXED);
    }
    return c;
}

#define DIARY_SLOT_COUNT 5  /* the Diary's slots (owner, 2026-11-17: five, then one Autosave row under them) */
#define DIARY_OF "/5"      /* the labels' "<n>/5" */
#define HDR_REC_SIZE 0xA0u

typedef struct {
    uint8_t raw[HDR_REC_SIZE];
    uint8_t valid;
} DiarySlotRow;

static HostLock slot_lock = HOST_LOCK_INIT;
static char user_save_dir[1024];
static int active_slot;            /* slot (0..9) backing live load/save/autosave I/O */

static void set_active_slot(int val) {
    __atomic_store_n(&active_slot, val, __ATOMIC_RELEASE);
}
static int diary_slot;             /* slot (0..9) currently shown in the Diary menu */
static int diary_opened_once;
/* the armed row's label: no button named, it is X, Tab, the X key or Square by device (owner, 2026-11-17) */
#define DELETE_LABEL "Press again to delete"
static int delete_confirm_slot = -1;
static int delete_confirm_frames;
static int delete_blocked;         /* X on the slot being played (in the game): "In use" shows instead of DELETE? */
static uint64_t diary_seen_ms;     /* host ms of the Diary's last update: savefs_diary_open, for the mouse */
/* A slot a New Game was started over (yw4_hook_save_clear): its old files stay on disk until the new game's first
 * save, but it reads as empty until then, so no save writes the old game's rows back into its HEADERSAVE. -1 none. */
static int fresh_slot = -1;
static int new_game_slot_set;  /* this new game's slot is chosen (savefs_new_game_naming) */
static int title_loaded_save;  /* since the title last opened, a save was loaded (so no new game is starting) */
static int input_cooldown;
static uint64_t prev_buttons;
static int repeat_dir;
static int repeat_hold;
static DiarySlotRow slot_cache[DIARY_SLOT_COUNT][2];

/* Stacked rows (owner, 2026-11-17: "the save slots stack, not Left / Right"): when the menu's data has six copies of
 * the row layer (diary_load_slot; the menu setting's count, a row bone for each in save00_06), copy k shows slot k+1
 * and the last copy the Autosave. The game's own row code fills each copy: it reads its row data at
 * menu + 0x210 + j * 0x288 (j 0 a save row, 1 the Autosave row) and finds the copy by f_9f3e90(menu, layer, j); for a
 * paint pass that lookup is sent to copy k (yw4_hook_menu_layer). Without the six copies the Diary pages as before. */
#define DIARY_ROWS (DIARY_SLOT_COUNT + 1)
#define SLOT_LAYER 0xd14f93d6u  /* crc32("diary_load_slot") */
static int rows_on;             /* (declared above) this Diary has the stacked rows (diary_paint_rows found copy DIARY_ROWS - 1) */
static int paint_pending;       /* paint the rows at the next Diary frame */
static int paint_slot = -1;     /* during a paint pass: the slot the populate hook puts in row 0 */
static int paint_copy = -1;     /* during a paint pass: the copy row j's lookups go to */
static int paint_j;             /* during a paint pass: the game's row (0 or 1) being drawn */
static int focus_auto;          /* stacked rows: the Autosave row has the focus (X deletes nothing) */
static int applying_focus;      /* diary_paint_rows is putting the focus look on its row */
static int64_t open_ns, paint_ns_total;  /* the Diary's opening: when, and the time our row painting took in it */
static int open_updates, open_logged;
static int focus_empty;         /* stacked rows: an empty save row has the focus (A is held back: savefs_filter_buttons) */

static int path_is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

static void slot_base_dir_locked(char out[1024], int slot) {
    if (!user_save_dir[0]) {
        out[0] = '\0';
        return;
    }
    if (slot <= 0) snprintf(out, 1024, "%s", user_save_dir);
    else snprintf(out, 1024, "%s/SLOT%02d", user_save_dir, slot + 1);  /* the number the Diary shows: SLOT02..SLOT10 */
}

static int newest_auto_slot_locked(void);

/* A slot is used when it has its save, or its game made the (one, shared) Autosave. */
static int slot_any_data_locked(int slot) {
    if (slot < 0 || slot >= DIARY_SLOT_COUNT) return 0;
    return slot_cache[slot][0].valid || newest_auto_slot_locked() == slot;
}

static uint32_t read_le32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static uint16_t read_le16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* Strip any UI-only "<n>/<count> " slot prefix (any one- or two-digit numbers: the count was 10 before it was 5) or
 * placeholder label from a 36-byte player name field. */
static void strip_slot_prefix_inplace(uint8_t name_field[36]) {
    name_field[35] = '\0';
    for (;;) {
        const char *s = (const char *)name_field;
        int p = 0, d1 = 0, d2 = 0;
        while (d1 < 2 && s[d1] >= '0' && s[d1] <= '9') d1++;
        if (d1 && s[d1] == '/') {
            while (d2 < 2 && s[d1 + 1 + d2] >= '0' && s[d1 + 1 + d2] <= '9') d2++;
            if (d2 && s[d1 + 1 + d2] == ' ') p = d1 + 1 + d2 + 1;
        }
        if (!p) break;
        size_t rem = strlen(s + p);
        memmove(name_field, s + p, rem + 1);
        memset(name_field + rem + 1, 0, (size_t)(36 - (rem + 1)));
    }
    if (!strcmp((const char *)name_field, "Empty Diary") ||
        !strcmp((const char *)name_field, "DELETE? (Press X)") || !strcmp((const char *)name_field, DELETE_LABEL) ||
        !strcmp((const char *)name_field, "In use: can't delete")) {
        memset(name_field, 0, 36);
    }
}

static void parse_slot_from_disk_locked(int slot) {
    memset(slot_cache[slot], 0, sizeof slot_cache[slot]);
    if (slot == fresh_slot) return;
    char sdir[1024];
    slot_base_dir_locked(sdir, slot);
    if (!sdir[0]) return;

    char p_manual[1088], p_auto[1088], p_hdr[1088];
    snprintf(p_manual, sizeof p_manual, "%s/USERDATA00/data.bin", sdir);
    /* one Autosave for every slot, in the main folder (owner, 2026-11-17: "autosave is autosave"): a slot's summary
     * row 1 is the autosave its game last made; the newest of them is the one on disk */
    snprintf(p_auto, sizeof p_auto, "%s/AUTOSAVE/data.bin", user_save_dir);
    snprintf(p_hdr, sizeof p_hdr, "%s/HEADERSAVE/data.bin", sdir);

    int has_manual = path_is_file(p_manual);
    int has_auto = path_is_file(p_auto);

    FILE *f = fopen(p_hdr, "rb");
    if (!f) return;
    uint8_t buf[2048];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 16 || read_le32(buf) != 0x10f2ffeeu) return;

    /* Walk Level-5 binary TLV stream (0x28677e98 delimits records 0=Manual, 1=Autosave;
     * 0x1774a7c8 delimits up to 12 party entries before summary leaves). */
    int rec = -1;
    int party = -1;
    size_t pos = 8;
    while (pos + 8 <= n) {
        uint32_t tag = read_le32(buf + pos);
        uint16_t lo = read_le16(buf + pos + 4);
        uint16_t hi = read_le16(buf + pos + 6);
        if (hi != 0) {
            if (tag == 0x28677e98u) {
                if (hi == 0x0300u) {
                    rec = 0;
                    party = -1;
                } else if (hi == 0x0400u) {
                    rec++;
                    party = -1;
                    if (rec >= 2) break;
                }
            } else if (tag == 0x1774a7c8u && rec >= 0 && rec < 2) {
                if (hi == 0x0300u) party = 0;
                else if (hi == 0x0400u) party++;
            }
            pos += 8;
            continue;
        }
        if (pos + 8 + (size_t)lo > n) break;
        const uint8_t *val = buf + pos + 8;
        if (rec >= 0 && rec < 2) {
            uint8_t *dst = slot_cache[slot][rec].raw;
            if (party >= 0 && party < 12) {
                if (tag == 0x8480c2c1u && lo >= 4) memcpy(dst + party * 8, val, 4);
                else if (tag == 0x07f65d6bu && lo >= 4) memcpy(dst + party * 8 + 4, val, 4);
            }
            switch (tag) {
            case 0xce5b89e1u: {
                size_t copy = lo < 36 ? lo : 36;
                memcpy(dst + 0x60, val, copy);
                dst[0x60 + 35] = '\0';
                strip_slot_prefix_inplace(dst + 0x60);
                break;
            }
            case 0x20e1cd12u:
                if (lo >= 4) memcpy(dst + 0x84, val, 4);
                break;
            case 0xebd6a97du:
                if (lo >= 4) memcpy(dst + 0x88, val, 4);
                break;
            case 0xb590c464u:
                if (lo >= 4) memcpy(dst + 0x8c, val, 4);
                break;
            case 0x0877f0cdu:
                if (lo >= 8) memcpy(dst + 0x90, val, 8);
                break;
            case 0x64916694u:
                if (lo >= 4) memcpy(dst + 0x98, val, 4);
                break;
            case 0x58803710u:
                if (lo >= 1) dst[0x9c] = val[0];
                break;
            case 0xecb44d39u:
                if (lo >= 1) dst[0x9d] = val[0];
                break;
            case 0x0a880c2du:
                if (lo >= 1) dst[0x9e] = val[0];
                break;
            default:
                break;
            }
        }
        pos += 8 + (size_t)lo;
    }

    if (!has_manual) memset(slot_cache[slot][0].raw, 0, HDR_REC_SIZE);
    if (!has_auto) memset(slot_cache[slot][1].raw, 0, HDR_REC_SIZE);
    slot_cache[slot][0].valid = has_manual && slot_cache[slot][0].raw[0x9d] != 0;
    slot_cache[slot][1].valid = has_auto && slot_cache[slot][1].raw[0x9d] != 0;
    slot_cache[slot][0].raw[0x9d] = slot_cache[slot][0].valid;
    slot_cache[slot][1].raw[0x9d] = slot_cache[slot][1].valid;
}

static void scan_all_slots_locked(void) {
    for (int s = 0; s < DIARY_SLOT_COUNT; s++) parse_slot_from_disk_locked(s);
}

static void utf8_copy_bounded(char *dst, size_t cap, const char *src, size_t max_src) {
    if (cap == 0) return;
    size_t slen = strnlen(src, max_src);
    size_t n = slen < cap - 1 ? slen : cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) n--;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

/* delete_mode: 0 the name, 1 delete armed, 2 X on the slot being played (it can't be deleted from the game) */
static int rows_on;
static void format_slot_label(char dst[36], int slot, int delete_mode, const uint8_t raw_name[36], uint8_t valid) {
    if (rows_on) {  /* stacked rows: the five rows show which slot it is, no "<n>/5" (owner, 2026-11-17) */
        const char *t = delete_mode == 2 ? "In use: can't delete" : delete_mode ? DELETE_LABEL
                        : !valid                  ? "Empty Diary"
                                                  : NULL;
        if (t) {
            snprintf(dst, 36, "%s", t);
        } else {
            char clean[37];
            memcpy(clean, raw_name, 36);
            clean[36] = '\0';
            utf8_copy_bounded(dst, 36, clean[0] ? clean : "Diary", 36);
        }
        return;
    }
    if (delete_mode) {
        snprintf(dst, 36, delete_mode == 2 ? "%d" DIARY_OF " In use: can't delete" : "%d" DIARY_OF " " DELETE_LABEL,
                 slot + 1);
        return;
    }
    if (!valid) {
        snprintf(dst, 36, "%d" DIARY_OF " Empty Diary", slot + 1);
        return;
    }
    char clean[37];
    memcpy(clean, raw_name, 36);
    clean[36] = '\0';
    int p = snprintf(dst, 36, "%d" DIARY_OF " ", slot + 1);
    if (p > 0 && p < 36) utf8_copy_bounded(dst + p, (size_t)(36 - p), clean[0] ? clean : "Diary", 36);
}

static uint64_t get_game_mgr(void) {
    uint64_t mgr = 0;
    memcpy(&mgr, (const void *)(uintptr_t)(yw4_image_base + 0x1548fd8), sizeof mgr);
    return mgr;
}

static uint64_t get_headersave_obj(void) {
    uint64_t mgr = get_game_mgr();
    if (!mgr) return 0;
    uint64_t hdr = 0;
    memcpy(&hdr, (const void *)(uintptr_t)(mgr + 0x57f8), sizeof hdr);
    return hdr;
}

/* True when a CMenuGeneralWindow Yes/No or alert dialog is live (instance count at +0x5511a84,
 * f_6bdab0/f_6bf4c0 in main_121.c) or a save/load job is running in save_mgr (+0x154b2b8, f_267eb0). */
static int is_general_win_active(void) {
    uint32_t gwin_count = 0;
    memcpy(&gwin_count, (const void *)(uintptr_t)(yw4_image_base + 0x5511a84), sizeof gwin_count);
    if (gwin_count != 0) return 1;
    uint64_t save_mgr = 0;
    memcpy(&save_mgr, (const void *)(uintptr_t)(yw4_image_base + 0x154b2b8), sizeof save_mgr);
    if (save_mgr) {
        uint32_t j0 = 0, j1 = 0, j2 = 0;
        memcpy(&j0, (const void *)(uintptr_t)(save_mgr + 0x18), sizeof j0);
        memcpy(&j1, (const void *)(uintptr_t)(save_mgr + 0x30), sizeof j1);
        memcpy(&j2, (const void *)(uintptr_t)(save_mgr + 0x48), sizeof j2);
        if (j0 != 0 || j1 != 0 || j2 != 0) return 1;
    }
    return 0;
}

static int ts_cmp(const uint8_t *a, const uint8_t *b);

/* The Diary's one Autosave row (owner, 2026-11-17): the newest autosave of any slot. Each slot keeps its own on disk,
 * so playing another slot never throws one away; only the newest shows. -1: no slot has one. */
static int newest_auto_slot_locked(void) {
    int best = -1;
    for (int s = 0; s < DIARY_SLOT_COUNT; s++)
        if (slot_cache[s][1].valid &&
            (best < 0 || ts_cmp(slot_cache[s][1].raw + 0x90, slot_cache[best][1].raw + 0x90) > 0))
            best = s;
    return best;
}

/* The Diary's two rows for the game's menu: row 0 the slot shown (paged), row 1 the newest autosave of any slot, each
 * name prefixed with its slot ("2/5 Nate"). delete_mode labels row 0 only. */
static void write_slot_to_hdr_locked(uint64_t hdr, int slot, int delete_mode) {
    if (!hdr || slot < 0 || slot >= DIARY_SLOT_COUNT) return;
    int auto_slot = newest_auto_slot_locked();
    for (int i = 0; i < 2; i++) {
        uint8_t *dst = (uint8_t *)(uintptr_t)(hdr + 0x0c + (uint64_t)i * HDR_REC_SIZE);
        int s = i == 0 ? slot : auto_slot;
        char label[36];
        if (s < 0) {  /* no autosave anywhere: the game's empty row */
            memset(dst, 0, HDR_REC_SIZE);
            continue;
        }
        memcpy(dst, slot_cache[s][i].raw, HDR_REC_SIZE);
        format_slot_label(label, s, i == 0 ? delete_mode : 0, slot_cache[s][i].raw + 0x60, slot_cache[s][i].valid);
        memcpy(dst + 0x60, label, 36);
    }
}

/* Restore unprefixed slot records in headersave_obj before f_32f470 runs f_32ec30 + Serialize(),
 * so neither the UI "<n>/5 " prefix nor another browsed slot's record is baked into HEADERSAVE/data.bin. */
static void restore_clean_hdr_locked(uint64_t hdr, int slot, int keep_row) {
    if (!hdr || slot < 0 || slot >= DIARY_SLOT_COUNT) return;
    uint32_t ver = 0x10001u;
    memcpy((void *)(uintptr_t)(hdr + 0x08), &ver, sizeof ver);
    for (int i = 0; i < 2; i++) {
        uint8_t *dst = (uint8_t *)(uintptr_t)(hdr + 0x0c + (uint64_t)i * HDR_REC_SIZE);
        if (i == keep_row) strip_slot_prefix_inplace(dst + 0x60);
        else memcpy(dst, slot_cache[slot][i].raw, HDR_REC_SIZE);
    }
}

static int ts_cmp(const uint8_t *a, const uint8_t *b) {
    uint16_t ya = read_le16(a), yb = read_le16(b);
    if (ya != yb) return ya > yb ? 1 : -1;
    for (int k = 2; k <= 6; k++) {
        if (a[k] != b[k]) return a[k] > b[k] ? 1 : -1;
    }
    return 0;
}

/* Which row gets the game's "Latest!" badge: the slot shown, or the Autosave row (the newest autosave of any slot). */
static uint32_t compute_latest_row_locked(int slot) {
    int a = newest_auto_slot_locked();
    if (a < 0) return 0;
    if (!slot_cache[slot][0].valid) return 1;
    return ts_cmp(slot_cache[a][1].raw + 0x90, slot_cache[slot][0].raw + 0x90) > 0 ? 1u : 0u;
}

static void remove_slot_subdir(const char *sdir, const char *sub) {
    char p_dir[1088], p_file[1104];
    snprintf(p_dir, sizeof p_dir, "%s/%s", sdir, sub);
    snprintf(p_file, sizeof p_file, "%s/data.bin", p_dir);
    remove(p_file);
    rmdir_one(p_dir);
}

static void delete_slot_files_locked(int slot) {
    char sdir[1024];
    slot_base_dir_locked(sdir, slot);
    /* Its save goes. The Autosave is shared and stays; so does the summary when it holds the Autosave's row (this
     * slot's game made it): its save row reads as empty without the save. */
    int keeps_auto = newest_auto_slot_locked() == slot;
    if (sdir[0]) {
        remove_slot_subdir(sdir, "USERDATA00");
        if (!keeps_auto) {
            remove_slot_subdir(sdir, "HEADERSAVE");
            if (slot > 0) rmdir_one(sdir);
        }
    }
    memset(&slot_cache[slot][0], 0, sizeof slot_cache[slot][0]);
    if (!keeps_auto) memset(&slot_cache[slot][1], 0, sizeof slot_cache[slot][1]);
}

static int find_first_empty_slot_locked(void) {
    for (int s = 0; s < DIARY_SLOT_COUNT; s++) {
        if (!slot_any_data_locked(s)) return s;
    }
    return -1;
}

static int find_first_used_slot_locked(void) {
    for (int s = 0; s < DIARY_SLOT_COUNT; s++) {
        if (slot_any_data_locked(s)) return s;
    }
    return 0;
}

#define FS_PATH_NOT_FOUND 0x202u       /* fs (module 2), description 1 */
#define FS_PATH_ALREADY_EXISTS 0x402u  /* description 2 */
#define FS_TARGET_NOT_FOUND 0x7D402u   /* description 1002 */
#define FS_INVALID_PATH 0x2EE602u      /* description 6003 (invalid path format) */
#define FS_OUT_OF_RANGE 0x3EA02u       /* description 3001 */

static char save_root[1024];
const char *savefs_root(void) { return save_root; }

static int64_t last_save;  /* Unix seconds: savefs_last_save */
int64_t savefs_last_save(void) { return __atomic_load_n(&last_save, __ATOMIC_ACQUIRE); }

/* The newest modification time of the files under dir, two folders deep at most (a save is a few files) */
static int64_t newest_file(const char *dir, int depth) {
    int64_t best = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        int64_t t = S_ISDIR(st.st_mode) ? (depth > 0 ? newest_file(p, depth - 1) : 0) : (int64_t)st.st_mtime;
        if (t > best) best = t;
    }
    closedir(d);
    return best;
}

/* mkdir -p */
static void make_dirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir_one(p);
            *c = '/';
        }
    mkdir_one(p);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

/* The save folder is made here when missing (2026-11-13, a Linux tester had to mkdir -p the default
 * ~/.local/share/YW4Recomp/save by hand), so the default folder and a new one named on the command line just work. */
int savefs_init(const char *dir) {
    if (strlen(dir) >= sizeof save_root - 200) return -1;
    strcpy(save_root, dir);
    if (!is_dir(dir)) {
        make_dirs(dir);
        if (!is_dir(dir)) {
            fprintf(stderr, "yw4: cannot create the save folder %s\n", dir);
            return -1;
        }
        fprintf(stderr, "yw4: created the save folder %s\n", dir);
    }
    return 0;
}

static void save_dir(char out[1024], uint64_t program_id, const uint8_t uid[16]) {
    char u[33];
    for (int i = 0; i < 16; i++) snprintf(u + 2 * i, 3, "%02x", uid[i]);
    snprintf(out, 1024, "%s/%016" PRIx64 "/%s", save_root, program_id, u);
}

/* 64-bit seek (offset -1: to the end) and tell: 0 / position, as fseek and ftell */
static int file_seek(FILE *f, int64_t off) {
#ifdef _WIN32
    return off < 0 ? _fseeki64(f, 0, SEEK_END) : _fseeki64(f, off, SEEK_SET);
#else
    return off < 0 ? fseeko(f, 0, SEEK_END) : fseeko(f, (off_t)off, SEEK_SET);
#endif
}
static int64_t file_tell(FILE *f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}

/* ---- IFile */

typedef struct {
    FILE *f;
} SaveFile;

static void save_file(Service *s, IpcRequest *req, IpcResponse *resp) {
    SaveFile *sf = s->state;
    switch (req->cmd) {
    case 0: {  /* Read(u32 option, u64 offset, u64 size, B buffer) -> u64 bytes read */
        uint64_t off = IPC_IN(uint64_t, 8), size = IPC_IN(uint64_t, 16);
        IpcBuffer out = ipc_out_buffer(req, 0);
        if (size > out.size) size = out.size;
        size_t n = 0;
        if (!file_seek(sf->f, off)) n = fread(GPTR(out.addr), 1, (size_t)size, sf->f);
        IPC_OUT(uint64_t, 0) = n;
        ipc_out_size(resp, 8);
        return;
    }
    case 1: {  /* Write(u32 option, u64 offset, u64 size, A buffer) */
        uint64_t off = IPC_IN(uint64_t, 8), size = IPC_IN(uint64_t, 16);
        const IpcBuffer *in = req->na ? &req->a[0] : &req->x[0];
        if (size > in->size || file_seek(sf->f, off) || fwrite(GPTR(in->addr), 1, (size_t)size, sf->f) != size)
            resp->result = FS_OUT_OF_RANGE;
        return;
    }
    case 2:  /* Flush */
        fflush(sf->f);
        return;
    case 3:  /* SetSize(u64) */
        fflush(sf->f);
        if (truncate_file(sf->f, IPC_IN(uint64_t, 0))) resp->result = FS_OUT_OF_RANGE;
        return;
    case 4: {  /* GetSize -> u64 */
        fflush(sf->f);
        file_seek(sf->f, -1);
        IPC_OUT(uint64_t, 0) = (uint64_t)file_tell(sf->f);
        ipc_out_size(resp, 8);
        return;
    }
    }
    ipc_unimplemented(s, req, resp);
}

/* ---- IDirectory: the entries, read once when it is opened */

typedef struct {
    char name[0x301];
    uint8_t type;  /* 0 directory, 1 file */
    int64_t size;
} DirEntry;

typedef struct {
    DirEntry *entries;
    uint64_t n, next;
} SaveDir;

static void save_directory(Service *s, IpcRequest *req, IpcResponse *resp) {
    SaveDir *d = s->state;
    switch (req->cmd) {
    case 0: {  /* Read(B: DirectoryEntry[], 0x310 each) -> u64 count */
        IpcBuffer out = ipc_out_buffer(req, 0);
        uint64_t n = 0;
        uint8_t *p = GPTR(out.addr);
        for (; n < out.size / 0x310 && d->next < d->n; n++, d->next++, p += 0x310) {
            /* DirectoryEntry: name[0x301], pad[3], u8 type, pad[3], s64 size */
            memset(p, 0, 0x310);
            memcpy(p, d->entries[d->next].name, strlen(d->entries[d->next].name));
            p[0x304] = d->entries[d->next].type;
            memcpy(p + 0x308, &d->entries[d->next].size, 8);
        }
        IPC_OUT(uint64_t, 0) = n;
        ipc_out_size(resp, 8);
        return;
    }
    case 1:  /* GetEntryCount -> u64 */
        IPC_OUT(uint64_t, 0) = d->n - d->next;
        ipc_out_size(resp, 8);
        return;
    }
    ipc_unimplemented(s, req, resp);
}

/* mode: 1 directories, 2 files, 3 both. NULL if the path is no directory. */
static SaveDir *read_directory(const char *path, uint32_t mode) {
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    SaveDir *d = calloc(1, sizeof *d);
    if (!d) yw4_exit(3, __FILE__, __LINE__);
    uint64_t cap = 0;
    for (struct dirent *e; (e = readdir(dir));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 0x300) continue;
        char full[2048];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st)) continue;
        int is_directory = (st.st_mode & S_IFMT) == S_IFDIR;
        if (!(mode & (is_directory ? 1 : 2))) continue;
        if (d->n == cap && !(d->entries = realloc(d->entries, (cap = cap ? 2 * cap : 16) * sizeof *d->entries))) yw4_exit(3, __FILE__, __LINE__);
        DirEntry *de = &d->entries[d->n++];
        strcpy(de->name, e->d_name);
        de->type = is_directory ? 0 : 1;
        de->size = is_directory ? 0 : (int64_t)st.st_size;
    }
    closedir(dir);
    return d;
}

/* ---- IFileSystem over one save folder */

typedef struct {
    char root[1024];
} SaveFs;

static int switch_path_is_slot_scoped(const char *p, size_t n) {
    if (n >= 9 && !memcmp(p, "/USERDATA", 9)) return 1;
    if (n >= 11 && !memcmp(p, "/HEADERSAVE", 11)) return 1;
    return 0;
}

/* The host path for the Switch path in the first X buffer; 0 if it is malformed. */
static int host_path(const SaveFs *fs, const IpcRequest *req, int x, char out[1024]) {
    if (req->nx <= x) return 0;
    const char *p = GPTR(req->x[x].addr);
    size_t n = strnlen(p, req->x[x].size < 0x301 ? req->x[x].size : 0x301);
    if (n == 0 || n > 0x300 || p[0] != '/' || memchr(p, '\\', n) || strstr(p, "/../") ||
        (n >= 3 && !memcmp(p + n - 3, "/..", 3)) || strstr(p, ":"))
        return 0;
    int s = diary_slots_enabled() ? __atomic_load_n(&active_slot, __ATOMIC_ACQUIRE) : 0;
    if (s > 0 && switch_path_is_slot_scoped(p, n))
        snprintf(out, 1024, "%s/SLOT%02d%.*s", fs->root, s + 1, (int)n, p);  /* as slot_base_dir_locked */
    else
        snprintf(out, 1024, "%s%.*s", fs->root, (int)n, p);
    size_t len = strlen(out);
    while (len > strlen(fs->root) + 1 && out[len - 1] == '/') out[--len] = 0;  /* "/dir/" -> "/dir" */
    return 1;
}

/* 1 when the Switch path in X buffer x goes to a slot's own folder (SLOT02..SLOT10): the game made its save folders
 * once, at its first start, in the root, so a slot's folder may not exist yet. Only there are missing files and
 * folders made on demand; anywhere else a missing path is still "not found", as before. */
static int in_new_slot_folder(const IpcRequest *req, int x) {
    if (!diary_slots_enabled() || __atomic_load_n(&active_slot, __ATOMIC_ACQUIRE) <= 0 || req->nx <= x) return 0;
    const char *p = GPTR(req->x[x].addr);
    return switch_path_is_slot_scoped(p, strnlen(p, req->x[x].size < 0x301 ? req->x[x].size : 0x301));
}

static void ensure_parent_dir(const char *path) {
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash && slash > parent) {
        *slash = '\0';
        if (!is_dir(parent)) make_dirs(parent);
    }
}

static void save_fs(Service *s, IpcRequest *req, IpcResponse *resp) {
    SaveFs *fs = s->state;
    char path[1024];
    switch (req->cmd) {
    case 0: {  /* CreateFile(u32 option, u64 size, X path) */
        if (!host_path(fs, req, 0, path)) break;
        if (save_slot_test()) fprintf(stderr, "yw4: save_slot_test: CreateFile %s\n", path + strlen(fs->root));
        struct stat st;
        if (stat(path, &st) == 0) { resp->result = FS_PATH_ALREADY_EXISTS; return; }
        if (in_new_slot_folder(req, 0)) ensure_parent_dir(path);
        FILE *f = fopen(path, "wb");
        if (!f) { resp->result = FS_PATH_NOT_FOUND; return; }
        if (truncate_file(f, IPC_IN(uint64_t, 8))) resp->result = FS_OUT_OF_RANGE;
        fclose(f);
        return;
    }
    case 1:  /* DeleteFile(X path) */
        if (!host_path(fs, req, 0, path)) break;
        if (remove(path)) resp->result = FS_PATH_NOT_FOUND;
        return;
    case 2: {  /* CreateDirectory(X path) */
        if (!host_path(fs, req, 0, path)) break;
        if (save_slot_test()) fprintf(stderr, "yw4: save_slot_test: CreateDirectory %s\n", path + strlen(fs->root));
        struct stat st;
        if (stat(path, &st) == 0) { resp->result = FS_PATH_ALREADY_EXISTS; return; }
        if (in_new_slot_folder(req, 0)) ensure_parent_dir(path);
        if (mkdir_one(path)) resp->result = FS_PATH_NOT_FOUND;
        return;
    }
    case 7: {  /* GetEntryType(X path) -> u32: 0 directory, 1 file */
        if (!host_path(fs, req, 0, path)) break;
        struct stat st;
        if (stat(path, &st)) { resp->result = FS_PATH_NOT_FOUND; return; }
        IPC_OUT(uint32_t, 0) = (st.st_mode & S_IFMT) == S_IFDIR ? 0 : 1;
        ipc_out_size(resp, 4);
        return;
    }
    case 8: {  /* OpenFile(u32 mode: 1 read, 2 write, 4 append; X path) -> IFile */
        if (!host_path(fs, req, 0, path)) break;
        uint32_t mode = IPC_IN(uint32_t, 0);
        if (save_slot_test())
            fprintf(stderr, "yw4: save_slot_test: OpenFile %s (mode=%u)\n", path + strlen(fs->root), mode);
        struct stat st;
        int missing = stat(path, &st) != 0;
        if (missing || (st.st_mode & S_IFMT) == S_IFDIR) {
            if (missing && (mode & 6) && in_new_slot_folder(req, 0)) {  /* a write into a slot not saved yet */
                ensure_parent_dir(path);
                FILE *create_f = fopen(path, "wb");
                if (create_f) fclose(create_f);
            } else {
                resp->result = FS_PATH_NOT_FOUND;
                return;
            }
        }
        FILE *f = fopen(path, mode & 6 ? "r+b" : "rb");
        if (!f) { resp->result = FS_PATH_NOT_FOUND; return; }
        SaveFile *sf = calloc(1, sizeof *sf);
        if (!sf) yw4_exit(3, __FILE__, __LINE__);
        sf->f = f;  /* closed never: files of a save stay few, and the Switch's limit is the game's concern */
        ipc_return_object(resp, ipc_service_new("fs:IFile (save)", save_file, sf));
        return;
    }
    case 9: {  /* OpenDirectory(u32 mode, X path) -> IDirectory */
        if (!host_path(fs, req, 0, path)) break;
        if (!is_dir(path) && in_new_slot_folder(req, 0)) make_dirs(path);
        SaveDir *d = read_directory(path, IPC_IN(uint32_t, 0));
        if (!d) { resp->result = FS_PATH_NOT_FOUND; return; }
        ipc_return_object(resp, ipc_service_new("fs:IDirectory (save)", save_directory, d));
        return;
    }
    case 10:  /* Commit: writes are already in the files; it marks the end of a save (the overlay says when) */
        __atomic_store_n(&last_save, (int64_t)time(NULL), __ATOMIC_RELEASE);
        if (diary_slots_enabled()) {
            host_lock(&slot_lock);
            if (!user_save_dir[0]) snprintf(user_save_dir, sizeof user_save_dir, "%s", fs->root);
            if (fresh_slot == active_slot) fresh_slot = -1;  /* the new game has saved: its files are the slot's now */
            parse_slot_from_disk_locked(active_slot);
            host_unlock(&slot_lock);
        }
        fprintf(stderr, "yw4: fs: the game saved (Commit)\n");
        return;
    default:
        ipc_unimplemented(s, req, resp);
        return;
    }
    resp->result = FS_INVALID_PATH;
}

uint32_t savefs_open(uint64_t program_id, const uint8_t uid[16], int create, Service **out) {
    char dir[1024];
    save_dir(dir, program_id, uid);
    if (!is_dir(dir)) {
        if (!create) return FS_TARGET_NOT_FOUND;
        make_dirs(dir);
        if (!is_dir(dir)) {
            fprintf(stderr, "yw4: fs: cannot create the save folder %s\n", dir);
            yw4_exit(3, __FILE__, __LINE__);
        }
        if (diary_slots_enabled()) {
            host_lock(&slot_lock);
            snprintf(user_save_dir, sizeof user_save_dir, "%s", dir);
            scan_all_slots_locked();
            static int initial_boot_done_create;
            if (!initial_boot_done_create) {
                initial_boot_done_create = 1;
                if (!slot_any_data_locked(active_slot)) {
                    int first = find_first_used_slot_locked();
                    diary_slot = first;
                    set_active_slot(first);
                }
            }
            host_unlock(&slot_lock);
        }
        fprintf(stderr, "yw4: fs: created save data in %s\n", dir);
        return 0;
    }
    if (out) {
        SaveFs *fs = calloc(1, sizeof *fs);
        if (!fs) yw4_exit(3, __FILE__, __LINE__);
        snprintf(fs->root, sizeof fs->root, "%s", dir);
        *out = ipc_service_new("fs:IFileSystem (save)", save_fs, fs);
        if (diary_slots_enabled()) {
            host_lock(&slot_lock);
            snprintf(user_save_dir, sizeof user_save_dir, "%s", dir);
            scan_all_slots_locked();
            static int initial_boot_done_mount;
            if (!initial_boot_done_mount) {
                initial_boot_done_mount = 1;
                if (!slot_any_data_locked(active_slot)) {
                    int first = find_first_used_slot_locked();
                    diary_slot = first;
                    set_active_slot(first);
                }
            }
            host_unlock(&slot_lock);
        }
        if (!savefs_last_save()) {  /* until the game saves in this run: when its save was last written */
            int64_t t = newest_file(dir, 2), none = 0;
            if (t) __atomic_compare_exchange_n(&last_save, &none, t, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    }
    return 0;
}

/* ---- 10-slot Diary, In-Game Save selector, Non-destructive New Game & Delete UX */

#define HID_BTN_X            (1ull << 2)
#define HID_BTN_MINUS        (1ull << 11)
#define HID_BTN_L            (1ull << 6)
#define HID_BTN_R            (1ull << 7)
#define HID_BTN_DPAD_LEFT    (1ull << 12)
#define HID_BTN_DPAD_RIGHT   (1ull << 14)
#define HID_BTN_LSTICK_LEFT  (1ull << 16)
#define HID_BTN_LSTICK_RIGHT (1ull << 18)

static void diary_paint_rows(CPUState *ctx, uint64_t menu);

static void diary_refresh_ui(CPUState *ctx, uint64_t menu) {
    if (rows_on) {  /* stacked rows: all of them again (a delete changed one) */
        diary_paint_rows(ctx, menu);
        return;
    }
    GuestFn fn_populate = yw4_lookup(yw4_image_base + 0x761aa0ull);
    GuestFn fn_members = yw4_lookup(yw4_image_base + 0x75fbf0ull);
    GuestFn fn_card = yw4_lookup(yw4_image_base + 0x760750ull);
    GuestFn fn_row = yw4_lookup(yw4_image_base + 0x75ee60ull);
    if (!fn_populate || !fn_members || !fn_card) return;

    uint8_t mode_continue = *(const uint8_t *)(uintptr_t)(menu + 0x721);
    host_lock(&slot_lock);
    int ds = diary_slot;
    if (mode_continue) {
        uint32_t latest = compute_latest_row_locked(ds);
        memcpy((void *)(uintptr_t)(menu + 0x748), &latest, sizeof latest);
    } else {
        uint8_t zero = 0;
        memcpy((void *)(uintptr_t)(menu + 0x720), &zero, sizeof zero);
    }
    host_unlock(&slot_lock);

    /* Clear cached row portrait/location pointers before f_761aa0 repopulates them cleanly. */
    uint64_t zero64 = 0;
    memcpy((void *)(uintptr_t)(menu + 0x458), &zero64, sizeof zero64);
    memcpy((void *)(uintptr_t)(menu + 0x460), &zero64, sizeof zero64);
    memcpy((void *)(uintptr_t)(menu + 0x6e0), &zero64, sizeof zero64);
    memcpy((void *)(uintptr_t)(menu + 0x6e8), &zero64, sizeof zero64);

    CPUState saved = *ctx;
    uint64_t gsp = saved.sp - 0x20;
    uint32_t row_hash = 0xd14f93d6u;

    ctx->sp = gsp;
    ctx->x[0] = menu;
    ctx->x[30] = saved.x[30];
    fn_populate(ctx);

    ctx->sp = gsp;
    ctx->x[0] = menu;
    ctx->x[30] = saved.x[30];
    fn_members(ctx);

    ctx->sp = gsp;
    ctx->x[0] = menu;
    ctx->x[30] = saved.x[30];
    fn_card(ctx);

    if (mode_continue && fn_row) {
        for (uint64_t sub = 0; sub < 2; sub++) {
            memcpy((void *)(uintptr_t)gsp, &row_hash, sizeof row_hash);
            ctx->sp = gsp;
            ctx->x[0] = menu;
            ctx->x[1] = gsp;
            ctx->x[2] = sub;
            ctx->x[30] = saved.x[30];
            fn_row(ctx);
        }
    }

    uint8_t row = *(const uint8_t *)(uintptr_t)(menu + 0x720);
    if (row > 1) row = 0;
    uint64_t portrait = 0, handle = 0;
    memcpy(&portrait, (const void *)(uintptr_t)(menu + (uint64_t)row * 0x288 + 0x460), sizeof portrait);
    memcpy(&handle, (const void *)(uintptr_t)(menu + 0x468), sizeof handle);
    /* f_75e810 passes row0's +0x460 to f_70f150 when handle==0 and +0x751==1; guard against NULL. */
    uint8_t reload = (handle != 0 || *(const uint64_t *)(uintptr_t)(menu + 0x460) != 0) ? 1 : 0;
    if (portrait != 0 && handle == 0 && *(const uint64_t *)(uintptr_t)(menu + 0x460) == 0) reload = 0;
    memcpy((void *)(uintptr_t)(menu + 0x751), &reload, sizeof reload);

    *ctx = saved;
}

int yw4_hook_save_load(CPUState *ctx) {
    uint8_t *req = (uint8_t *)(uintptr_t)ctx->x[0];
    if (!req) return 0;
    if (!diary_slots_enabled()) {
        if (!save_slot_test()) return 0;
        uint16_t slot = 0;
        memcpy(&slot, req + 2, sizeof slot);
        if (req[0] == 0 && slot == 0) {
            uint16_t new_slot = 1;
            memcpy(req + 2, &new_slot, sizeof new_slot);
            fprintf(stderr, "yw4: save_slot_test: f_8a7dc0 load USERDATA slot 0 -> 1\n");
        }
        return 0;
    }
    if (scene_on_title()) {
        if (req[0] == 3 && !rows_on) {  /* stacked rows: the focused slot stays, an empty one says "no save data" */
            host_lock(&slot_lock);
            int cur = __atomic_load_n(&active_slot, __ATOMIC_ACQUIRE);
            if (!slot_any_data_locked(cur)) {
                int first = find_first_used_slot_locked();
                diary_slot = first;
                set_active_slot(first);
            }
            host_unlock(&slot_lock);
        } else if (req[0] == 0 || req[0] == 2) {
            host_lock(&slot_lock);
            /* stacked rows: the game takes the kind from the focused copy (its copy 1 is the Autosave), so it is set
             * from the row: a save row loads its save, the Autosave row the Autosave */
            if (rows_on) req[0] = focus_auto ? 2 : 0;
            /* kind 2, the Autosave row: the newest autosave's slot, whose game goes on (its next saves go there) */
            int a = req[0] == 2 ? newest_auto_slot_locked() : -1;
            int chosen = a >= 0 ? a : diary_slot;
            diary_slot = chosen;
            set_active_slot(chosen);
            title_loaded_save = 1;  /* a saved game, not a new one: its names are not a new game's */
            host_unlock(&slot_lock);
            fprintf(stderr, "yw4: diary: load slot %d" DIARY_OF " (kind=%u)\n", chosen + 1, req[0]);
        }
    }
    return 0;
}

int yw4_hook_save_write(CPUState *ctx) {
    uint8_t *req = (uint8_t *)(uintptr_t)ctx->x[0];
    if (!req) return 0;
    if (!diary_slots_enabled()) {
        if (!save_slot_test()) return 0;
        uint16_t slot = 0;
        memcpy(&slot, req + 4, sizeof slot);
        if (req[0] == 0 && slot == 0) {
            uint16_t new_slot = 1;
            memcpy(req + 4, &new_slot, sizeof new_slot);
            fprintf(stderr, "yw4: save_slot_test: f_8a7b10 write USERDATA slot 0 -> 1\n");
        }
        return 0;
    }
    if (req[0] == 0) {
        host_lock(&slot_lock);
        int chosen = diary_slot;
        set_active_slot(chosen);
        restore_clean_hdr_locked(get_headersave_obj(), chosen, -1);
        host_unlock(&slot_lock);
        fprintf(stderr, "yw4: diary: manual save -> slot %d" DIARY_OF "\n", chosen + 1);
    } else if (req[0] == 2) {
        host_lock(&slot_lock);
        int cur = __atomic_load_n(&active_slot, __ATOMIC_ACQUIRE);
        diary_slot = cur;
        uint64_t hdr = get_headersave_obj();
        if (req[2] != 0) {
            /* Combined save (f_268090 / f_727820): f_32ec30(1) already populated Row 1 before f_32f470. */
            restore_clean_hdr_locked(hdr, cur, 1);
            if (hdr && cur >= 0 && cur < DIARY_SLOT_COUNT) {
                memcpy(slot_cache[cur][1].raw, (const void *)(uintptr_t)(hdr + 0x0c + HDR_REC_SIZE), HDR_REC_SIZE);
                slot_cache[cur][1].valid = slot_cache[cur][1].raw[0x9d] != 0;
            }
        } else {
            restore_clean_hdr_locked(hdr, cur, -1);
        }
        host_unlock(&slot_lock);
        fprintf(stderr, "yw4: diary: autosave -> slot %d" DIARY_OF "\n", cur + 1);
    }
    return 0;
}

int yw4_hook_diary_open(CPUState *ctx) {
    (void)ctx;
    if (!diary_slots_enabled()) return 0;
    host_lock(&slot_lock);
    if (user_save_dir[0]) scan_all_slots_locked();
    delete_confirm_slot = -1;
    delete_confirm_frames = 0;
    delete_blocked = 0;
    int cur_active = __atomic_load_n(&active_slot, __ATOMIC_ACQUIRE);
    if (!diary_opened_once) {
        diary_opened_once = 1;
        if (scene_on_title() && !slot_any_data_locked(0)) {
            int first = find_first_used_slot_locked();
            diary_slot = first;
            set_active_slot(first);
        } else {
            diary_slot = cur_active;
        }
    } else {
        diary_slot = cur_active;
    }
    uint64_t hdr = get_headersave_obj();
    write_slot_to_hdr_locked(hdr, diary_slot, 0);
    int chosen = diary_slot;
    host_unlock(&slot_lock);
    prev_buttons = hid_current_buttons();
    input_cooldown = 10;
    repeat_dir = 0;
    repeat_hold = 0;
    rows_on = 0;
    focus_auto = 0;
    paint_pending = 1;
    open_ns = host_now_ns(), open_updates = 0, open_logged = 0, paint_ns_total = 0;
    fprintf(stderr, "yw4: diary: open slot %d" DIARY_OF " (%s)\n", chosen + 1,
            scene_on_title() ? "Continue" : "InGame");
    return 0;
}

int yw4_hook_diary_populate(CPUState *ctx) {
    (void)ctx;
    if (!diary_slots_enabled()) {
        if (!save_slot_test()) return 0;
        uint64_t hdr = get_headersave_obj();
        if (!hdr) return 0;
        for (int i = 0; i < 2; i++) {
            const uint8_t *rec = (const uint8_t *)(uintptr_t)(hdr + (uint64_t)i * 0xa0);
            char name[37];
            uint32_t map_id = 0, loc_text = 0, chapter = 0, time_s = 0;
            memcpy(name, rec + 0x6c, 36);
            name[36] = 0;
            memcpy(&map_id, rec + 0x90, sizeof map_id);
            memcpy(&loc_text, rec + 0x94, sizeof loc_text);
            memcpy(&chapter, rec + 0x98, sizeof chapter);
            memcpy(&time_s, rec + 0xa4, sizeof time_s);
            fprintf(stderr,
                    "yw4: save_slot_test: diary row=%d valid=%u clear=%u rank=%u name=\"%s\" "
                    "map=0x%08x loc_text=0x%08x chap=%u time=%us\n",
                    i, rec[0xa9], rec[0xaa], rec[0xa8], name, map_id, loc_text, chapter, time_s);
        }
        return 0;
    }
    host_lock(&slot_lock);
    uint64_t hdr = get_headersave_obj();
    int s = paint_slot >= 0 ? paint_slot : diary_slot;
    write_slot_to_hdr_locked(hdr, s, delete_confirm_slot == s ? 1 + delete_blocked : 0);
    host_unlock(&slot_lock);
    return 0;
}

/* ---- Stacked rows (see DIARY_ROWS) */

/* Calls a game function with x0..x2 on a scratch frame below the hook's stack; returns x0. ctx is put back. */
static uint64_t guest_call3(CPUState *ctx, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2) {
    GuestFn fn = yw4_lookup(yw4_image_base + addr);
    if (!fn) return 0;
    CPUState saved = *ctx;
    ctx->sp = saved.sp - 0x40;
    ctx->x[0] = a0, ctx->x[1] = a1, ctx->x[2] = a2;
    ctx->x[30] = saved.x[30];
    fn(ctx);
    uint64_t r = ctx->x[0];
    *ctx = saved;
    return r;
}

/* On the title the focused row's slot is the active one at once, not only when it loads: the game looks at its files
 * before it loads (an empty slot: "There is no save data available."). The Autosave row: the slot whose game made
 * it, which goes on in it. In the game (Save) the slot being played stays. */
static void rows_set_active_locked(int row) {
    __atomic_store_n(&focus_empty, row < DIARY_SLOT_COUNT && !slot_cache[row][0].valid, __ATOMIC_RELEASE);
    if (!scene_on_title()) return;
    int s = row < DIARY_SLOT_COUNT ? row : newest_auto_slot_locked();
    if (s >= 0) set_active_slot(s);
}

/* The row that gets the "Latest!" badge: the newest of the five saves and the Autosave row; -1 none. */
static int newest_row_locked(void) {
    int best = -1;
    const uint8_t *bt = NULL;
    for (int s = 0; s < DIARY_SLOT_COUNT; s++)
        if (slot_cache[s][0].valid && (!bt || ts_cmp(slot_cache[s][0].raw + 0x90, bt) > 0))
            best = s, bt = slot_cache[s][0].raw + 0x90;
    int a = newest_auto_slot_locked();
    if (a >= 0 && (!bt || ts_cmp(slot_cache[a][1].raw + 0x90, bt) > 0)) best = DIARY_SLOT_COUNT;
    return best;
}

/* The game's row data (menu + 0x210 / + 0x498) filled again from headersave_obj (f_761aa0, through the populate
 * hook). Its cached portrait and location pointers are cleared first, as diary_refresh_ui does. */
static void diary_repopulate(CPUState *ctx, uint64_t menu) {
    uint64_t zero64 = 0;
    memcpy((void *)(uintptr_t)(menu + 0x458), &zero64, sizeof zero64);
    memcpy((void *)(uintptr_t)(menu + 0x460), &zero64, sizeof zero64);
    memcpy((void *)(uintptr_t)(menu + 0x6e0), &zero64, sizeof zero64);
    memcpy((void *)(uintptr_t)(menu + 0x6e8), &zero64, sizeof zero64);
    guest_call3(ctx, 0x761aa0, menu, 0, 0);
}

/* The card (members and notebook) for the row in the game's selection (menu + 0x720). */
static void diary_redraw_card(CPUState *ctx, uint64_t menu) {
    guest_call3(ctx, 0x75fbf0, menu, 0, 0);
    guest_call3(ctx, 0x760750, menu, 0, 0);
}

/* A layer copy's draw priority (its CMenuRenderComponent's m_drawPriority, +0x94; a higher one is drawn later,
 * on top). The copy's object (+0x10) lists its components from +0xe8, the next at +0x10; each is known by its type
 * name (vtable[-1] -> type info, +8 its name), as the game finds its locator (f_70c920). */
static void set_draw_priority(CPUState *ctx, uint64_t menu, uint32_t layer, uint64_t copy, uint32_t pr) {
    uint64_t c = guest_call3(ctx, 0x9f3e90, menu, layer, copy), obj = 0, node = 0;
    if (!c) return;
    memcpy(&obj, (const void *)(uintptr_t)(c + 0x10), sizeof obj);
    if (obj) memcpy(&node, (const void *)(uintptr_t)(obj + 0xe8), sizeof node);
    for (int guard = 0; node && guard < 64; guard++) {
        uint64_t vt = 0, ti = 0, name = 0;
        memcpy(&vt, (const void *)(uintptr_t)node, sizeof vt);
        if (vt) memcpy(&ti, (const void *)(uintptr_t)(vt - 8), sizeof ti);
        if (ti) memcpy(&name, (const void *)(uintptr_t)(ti + 8), sizeof name);
        if (name == yw4_image_base + 0x12bfe30ull) {  /* "N5lives20CMenuRenderComponentE" */
            memcpy((void *)(uintptr_t)(node + 0x94), &pr, sizeof pr);
            return;
        }
        memcpy(&node, (const void *)(uintptr_t)(node + 0x10), sizeof node);
    }
}

/* Debug (YW4_DIARY_DEBUG=1): each component of a row copy with its draw priority, to compare the latest row with the
 * others (the name that shows dim on the latest row). */
static void diary_debug_row(CPUState *ctx, uint64_t menu, int k) {
    if (!getenv("YW4_DIARY_DEBUG")) return;
    uint64_t c = guest_call3(ctx, 0x9f3e90, menu, SLOT_LAYER, (uint64_t)k), obj = 0, node = 0;
    if (!c) {
        fprintf(stderr, "yw4: diary dbg: row %d has no copy\n", k);
        return;
    }
    memcpy(&obj, (const void *)(uintptr_t)(c + 0x10), sizeof obj);
    if (obj) memcpy(&node, (const void *)(uintptr_t)(obj + 0xe8), sizeof node);
    fprintf(stderr, "yw4: diary dbg: row %d copy %#llx obj %#llx\n", k, (unsigned long long)c,
            (unsigned long long)obj);
    for (int guard = 0; node && guard < 64; guard++) {
        uint64_t vt = 0, ti = 0, name = 0;
        uint32_t pr = 0;
        memcpy(&vt, (const void *)(uintptr_t)node, sizeof vt);
        if (vt) memcpy(&ti, (const void *)(uintptr_t)(vt - 8), sizeof ti);
        if (ti) memcpy(&name, (const void *)(uintptr_t)(ti + 8), sizeof name);
        memcpy(&pr, (const void *)(uintptr_t)(node + 0x94), sizeof pr);
        fprintf(stderr, "yw4: diary dbg:   %s prio %u\n", name ? (const char *)(uintptr_t)name : "?", pr);
        /* the component's fields 0x60..0x13f as 32-bit words, to find the name's colour and alpha */
        for (uint64_t off = 0x60; off < 0x140; off += 16) {
            uint32_t w[4] = {0, 0, 0, 0};
            memcpy(w, (const void *)(uintptr_t)(node + off), sizeof w);
            fprintf(stderr, "yw4: diary dbg:     +%03llx %08x %08x %08x %08x\n", (unsigned long long)off, w[0], w[1],
                    w[2], w[3]);
        }
        memcpy(&node, (const void *)(uintptr_t)(node + 0x10), sizeof node);
    }
}

/* Draws every row with the game's row code: copy k from slot k (as the game's save row, j 0) and the last copy from
 * the newest autosave (as its Autosave row, j 1). Then the game's two rows of data hold the focused row again and the
 * card shows it. Sets rows_on. */
static void diary_paint_rows(CPUState *ctx, uint64_t menu) {
    if (!*(const uint8_t *)(uintptr_t)(menu + 0x721)) return;  /* the title's Continue only: Save has no rows */
    if (!guest_call3(ctx, 0x9f3e90, menu, SLOT_LAYER, DIARY_ROWS - 1)) {
        rows_on = 0;
        return;
    }
    rows_on = 1;
    host_lock(&slot_lock);
    int latest = newest_row_locked();
    host_unlock(&slot_lock);
    uint32_t keep748;
    memcpy(&keep748, (const void *)(uintptr_t)(menu + 0x748), sizeof keep748);
    /* each row over the one above it, so a row's "Latest!" badge, which stands above the row, isn't covered (owner,
     * 2026-11-17): the rows 15..20 from the top (the game's are all 20). Not above 20: their texts stay at the layer's
     * 20 (set when the menu is built), and a row drawn over 20 covered its own text (grey). Nothing of the Diary is
     * drawn between its background (10) and 20; the cursor keeps its 21. */
    for (int k = 0; k < DIARY_ROWS; k++)
        set_draw_priority(ctx, menu, SLOT_LAYER, (uint64_t)k, 20u - (uint32_t)(DIARY_ROWS - 1 - k));
    set_draw_priority(ctx, menu, 0x6ac96a00u /* diary_load_cursor */, 0, 21u);
    for (int k = 0; k < DIARY_ROWS; k++) {
        int j = k == DIARY_SLOT_COUNT;
        host_lock(&slot_lock);
        paint_slot = j ? diary_slot : k;
        host_unlock(&slot_lock);
        diary_repopulate(ctx, menu);
        uint32_t lat = latest == k ? (uint32_t)j : 7u;  /* the badge shows on row j when menu + 0x748 == j */
        memcpy((void *)(uintptr_t)(menu + 0x748), &lat, sizeof lat);
        uint64_t hash_at = ctx->sp - 0x80;  /* the row code takes the layer hash by address */
        uint32_t h = SLOT_LAYER;
        memcpy((void *)(uintptr_t)hash_at, &h, sizeof h);
        __atomic_store_n(&paint_j, j, __ATOMIC_RELAXED);
        __atomic_store_n(&paint_copy, k, __ATOMIC_RELEASE);
        CPUState saved = *ctx;
        ctx->sp = hash_at - 0x40;
        guest_call3(ctx, 0x75ee60, menu, hash_at, (uint64_t)j);
        *ctx = saved;
        __atomic_store_n(&paint_copy, -1, __ATOMIC_RELEASE);
        diary_debug_row(ctx, menu, k);
        if (!j) {
            /* The pencil (the save row's icon, _space_pencil01 / 02) only on an empty slot: on a save the character
             * stands alone (owner, 2026-11-17). The game's row code never hides it on its save row. f_9eaf10(copy,
             * part, shown). */
            host_lock(&slot_lock);
            uint64_t empty = !slot_cache[k][0].valid;
            host_unlock(&slot_lock);
            uint64_t obj = guest_call3(ctx, 0x9f3e90, menu, SLOT_LAYER, (uint64_t)k);
            if (obj) {
                guest_call3(ctx, 0x9eaf10, obj, 0xd11572feu, empty);
                guest_call3(ctx, 0x9eaf10, obj, 0x481c2344u, empty);
            }
        }
    }
    host_lock(&slot_lock);
    paint_slot = -1;
    host_unlock(&slot_lock);
    memcpy((void *)(uintptr_t)(menu + 0x748), &keep748, sizeof keep748);
    host_lock(&slot_lock);
    int v = focus_auto ? DIARY_SLOT_COUNT : diary_slot;
    host_unlock(&slot_lock);
    uint64_t c0 = guest_call3(ctx, 0x9f3e90, menu, SLOT_LAYER, 0);
    if (c0) {  /* the focus grid (f_9e9340: rows at +0x138, columns +0x13a, the index +0x13c): the game's row setup
                * makes it 2 x 1; six rows here. Then the focus and the cursor on that row. */
        uint16_t rows = DIARY_ROWS;
        memcpy((void *)(uintptr_t)(c0 + 0x138), &rows, sizeof rows);
        int16_t v16 = (int16_t)v;
        memcpy((void *)(uintptr_t)(c0 + 0x13c), &v16, sizeof v16);
        uint32_t id;
        memcpy(&id, (const void *)(uintptr_t)(menu + 0x158), sizeof id);
        GuestFn attach = yw4_lookup(yw4_image_base + 0x70c920ull);
        if (attach) {
            CPUState saved = *ctx;
            ctx->sp = saved.sp - 0x40;
            ctx->x[0] = id, ctx->x[1] = SLOT_LAYER, ctx->x[2] = 0xb1f061c1u, ctx->x[3] = (uint64_t)(int64_t)v16,
            ctx->x[4] = DIARY_ROWS, ctx->x[30] = saved.x[30];
            attach(ctx);
            *ctx = saved;
        }
        /* the focus look (the green row) on it too, as the engine's own moves do: +0x144 set, then f_9f4020 */
        uint8_t one = 1;
        memcpy((void *)(uintptr_t)(c0 + 0x144), &one, sizeof one);
        uint64_t hash_at = ctx->sp - 0x80;
        uint32_t h = SLOT_LAYER;
        memcpy((void *)(uintptr_t)hash_at, &h, sizeof h);
        CPUState saved = *ctx;
        ctx->sp = hash_at - 0x40;
        applying_focus = 1;  /* it calls the rows' focus handler back: not a move (yw4_hook_diary_focus) */
        guest_call3(ctx, 0x9f4020, menu, hash_at, 0);
        applying_focus = 0;
        *ctx = saved;
    }
    if (v >= 0) {
        host_lock(&slot_lock);
        if (v < DIARY_SLOT_COUNT) diary_slot = v;
        focus_auto = v == DIARY_SLOT_COUNT;
        rows_set_active_locked(v);
        host_unlock(&slot_lock);
        uint8_t sel = v == DIARY_SLOT_COUNT;
        memcpy((void *)(uintptr_t)(menu + 0x720), &sel, sizeof sel);
    }
    diary_repopulate(ctx, menu);
    diary_redraw_card(ctx, menu);
    fprintf(stderr, "yw4: diary: rows painted (focus %d, latest %d)\n", v, latest);
}

/* f_9f3e90(menu, layer hash, copy): during a paint pass, the save-row layer's row j is copy paint_copy. */
int yw4_hook_menu_layer(CPUState *ctx) {
    int c = __atomic_load_n(&paint_copy, __ATOMIC_ACQUIRE);
    if (c >= 0 && (uint32_t)ctx->x[1] == SLOT_LAYER && (uint32_t)ctx->x[2] == (uint32_t)__atomic_load_n(&paint_j, __ATOMIC_RELAXED))
        ctx->x[2] = (uint64_t)c;
    return 0;
}

/* f_9e9340 and f_9f4020: what the row code does for row 0 only (its focus look and the menu's focus); skipped in a
 * paint pass, so painting a row doesn't focus it. */
int yw4_hook_menu_row_focus(CPUState *ctx) {
    (void)ctx;
    return __atomic_load_n(&paint_copy, __ATOMIC_ACQUIRE) >= 0;
}

/* f_75ee60(menu, &layer hash, copy), the Diary's per-copy setup: the game's row data has two rows, so copies 2.. of
 * the row layer would read past them. The game's own calls for those copies are skipped (diary_paint_rows draws
 * them). */
int yw4_hook_diary_setup(CPUState *ctx) {
    if (!diary_slots_enabled() || !ctx->x[1]) return 0;
    uint32_t h;
    memcpy(&h, (const void *)(uintptr_t)ctx->x[1], sizeof h);
    if (h != SLOT_LAYER || __atomic_load_n(&paint_copy, __ATOMIC_ACQUIRE) >= 0) return 0;
    paint_pending = 1;  /* the game set up its rows (again): ours are drawn over them at the next frame */
    uint32_t copy = (uint32_t)ctx->x[2];
    if (copy >= 2) return 1;
    /* Copy 1 is the game's Autosave row: its setup gives it the Autosave look (dark text, the pencil hidden), which a
     * save row's paint doesn't undo. With the stacked rows it is a save row (slot 2), so the game's setup is skipped
     * for it too; copy 0's still runs (it sets the rows' focus grid). */
    return copy == 1 && guest_call3(ctx, 0x9f3e90, ctx->x[0], SLOT_LAYER, DIARY_ROWS - 1) != 0;
}

/* f_75fb40(menu, &layer hash), the rows' Up / Down: the engine has moved the focus (copy 0's +0x13c) and the game
 * moves the cursor there, then flips its selection (+0x720) between its two rows and redraws the card. With the
 * stacked rows: the focused copy picks the slot (or the Autosave), the game's row data is filled for it, and +0x720
 * is set to the opposite so the game's flip lands on 0 (a save) or 1 (the Autosave). */
int yw4_hook_diary_focus(CPUState *ctx) {
    if (!diary_slots_enabled() || !rows_on || !ctx->x[1] || applying_focus) return 0;
    uint64_t menu = ctx->x[0];
    uint32_t h;
    memcpy(&h, (const void *)(uintptr_t)ctx->x[1], sizeof h);
    if (h != SLOT_LAYER) return 0;
    /* the engine has moved its focus (copy 0's +0x13c, in the grid diary_paint_rows set to DIARY_ROWS rows) */
    uint64_t c0 = guest_call3(ctx, 0x9f3e90, menu, SLOT_LAYER, 0);
    if (!c0) return 0;
    int16_t v16;
    memcpy(&v16, (const void *)(uintptr_t)(c0 + 0x13c), sizeof v16);
    int v = v16;
    if (v < 0 || v >= DIARY_ROWS) return 0;
    host_lock(&slot_lock);
    int was = focus_auto ? DIARY_SLOT_COUNT : diary_slot;
    if (v < DIARY_SLOT_COUNT) diary_slot = v;
    focus_auto = v == DIARY_SLOT_COUNT;
    rows_set_active_locked(v);
    /* moving off an armed row cancels its delete, and its label goes back to the name: the rows painted again
     * at the next update (owner, 2026-11-17: it kept "Press again to delete" after scrolling away). Only a real
     * move: painting the rows calls this back on the same row, which cancelled every armed delete (the label
     * blinked back to the name and the second press armed it again instead of deleting) */
    if (v != was) {
        if (delete_confirm_slot >= 0) paint_pending = 1;
        delete_confirm_slot = -1, delete_confirm_frames = 0, delete_blocked = 0;
    }
    host_unlock(&slot_lock);
    diary_repopulate(ctx, menu);
    uint8_t flip_from = v == DIARY_SLOT_COUNT ? 0 : 1;
    memcpy((void *)(uintptr_t)(menu + 0x720), &flip_from, sizeof flip_from);
    fprintf(stderr, "yw4: diary: focus row %d\n", v);
    return 0;
}

/* f_70c920(menu id, layer, attach line, copy, n): turns the line off on copies 0..n-1, then on for that copy. The
 * Diary moves its cursor (line 0xb1f061c1 on the row layer, _pos_attach_cur_01 -> diary_load_cursor) with n = 2; with
 * the stacked rows all of them, or rows 2.. keep the cursor. */
int yw4_hook_menu_attach(CPUState *ctx) {
    if (rows_on && (uint32_t)ctx->x[1] == SLOT_LAYER && (uint32_t)ctx->x[2] == 0xb1f061c1u && (uint32_t)ctx->x[4] == 2)
        ctx->x[4] = DIARY_ROWS;
    return 0;
}

/* f_9f2a00(menu, &layer hash, wrap): the engine's focus one up; at the top it wraps to the bottom only with wrap set,
 * and the Diary's rows don't set it (Down wraps). With the stacked rows Up from slot 1 goes to the Autosave. */
int yw4_hook_menu_focus_up(CPUState *ctx) {
    if (!rows_on || !ctx->x[1]) return 0;
    uint32_t h;
    memcpy(&h, (const void *)(uintptr_t)ctx->x[1], sizeof h);
    if (h == SLOT_LAYER) ctx->x[2] |= 1;
    return 0;
}

/* f_75f6b0(menu, &layer hash): a layer opened (the game's base open, f_6c5cb0, then for the rows their first focus on
 * its "Latest!" row of two: menu + 0x748, kept below the grid's row count). With the stacked rows (six copies) that is
 * the newest of the six, as the game does (the save to continue): the grid is made six rows and 0x748 that row before
 * the game's code runs; diary_paint_rows puts the badges back. */
int yw4_hook_diary_first_focus(CPUState *ctx) {
    if (!diary_slots_enabled() || !ctx->x[1]) return 0;
    uint32_t h;
    memcpy(&h, (const void *)(uintptr_t)ctx->x[1], sizeof h);
    uint64_t menu = ctx->x[0];
    /* the title's Continue only (menu + 0x721): the in-game Save has the row layer too, unshown, and opens on the slot
     * being played (yw4_hook_diary_open) */
    if (h != SLOT_LAYER || !*(const uint8_t *)(uintptr_t)(menu + 0x721) ||
        !guest_call3(ctx, 0x9f3e90, menu, SLOT_LAYER, DIARY_ROWS - 1))
        return 0;
    uint64_t c0 = guest_call3(ctx, 0x9f3e90, menu, SLOT_LAYER, 0);
    if (!c0) return 0;
    host_lock(&slot_lock);
    int v = newest_row_locked();
    if (v < 0) v = 0;
    if (v < DIARY_SLOT_COUNT) diary_slot = v;
    focus_auto = v == DIARY_SLOT_COUNT;
    rows_set_active_locked(v);
    host_unlock(&slot_lock);
    uint16_t rows = DIARY_ROWS;
    memcpy((void *)(uintptr_t)(c0 + 0x138), &rows, sizeof rows);
    uint32_t v32 = (uint32_t)v;
    memcpy((void *)(uintptr_t)(menu + 0x748), &v32, sizeof v32);
    paint_pending = 1;
    return 0;
}

/* f_2672d0(args, count): the Lua call the Diary's script asks "has this row a save?" with, before its "load this
 * diary?" box; it answers the summary's valid flag at hdr + row * 0xa0, for the game's two rows only (row = the
 * focused copy). With the stacked rows the answer is the focused row's: its slot's save, or the Autosave. As the game
 * does: the argument read (+0x8 counts them), a boolean pushed (f_d243e0), 1 returned. */
int yw4_hook_diary_has_save(CPUState *ctx) {
    if (!rows_on || !(uint32_t)ctx->x[1] || !savefs_diary_open() || !scene_on_title()) return 0;
    uint64_t args = ctx->x[0];
    host_lock(&slot_lock);
    int ok = focus_auto ? newest_auto_slot_locked() >= 0 : slot_cache[diary_slot][0].valid != 0;
    host_unlock(&slot_lock);
    uint32_t used;
    memcpy(&used, (const void *)(uintptr_t)(args + 8), sizeof used);
    used++;
    memcpy((void *)(uintptr_t)(args + 8), &used, sizeof used);
    uint64_t L;
    memcpy(&L, (const void *)(uintptr_t)args, sizeof L);
    guest_call3(ctx, 0xd243e0, L, (uint64_t)ok, 0);
    ctx->x[0] = 1;
    return 1;
}

/* f_266fc0, the Lua call "load row N", at 0x267030: its kind is in w22 (row 1 -> 2, the Autosave; any other row number
 * kept: copy 5, the stacked Autosave row, would ask for kind 5). With the stacked rows: 0 (the focused slot's save,
 * from its folder: the active slot) or 2 (the Autosave). */
int yw4_hook_diary_load_kind(CPUState *ctx) {
    if (!rows_on || !savefs_diary_open() || !scene_on_title()) return 0;
    ctx->x[22] = focus_auto ? 2 : 0;
    return 0;
}

/* The pad as the game sees it (svc_hid.c): on the title's stacked rows an empty save row takes no A (owner,
 * 2026-11-17: no "load this diary?" box on an empty slot). Not while a box is up (its own A). */
uint64_t savefs_filter_buttons(uint64_t buttons) {
    if (!(buttons & 1) || !__atomic_load_n(&focus_empty, __ATOMIC_ACQUIRE) || !savefs_diary_rows() || !scene_on_title())
        return buttons;
    if (is_general_win_active()) return buttons;
    return buttons & ~(uint64_t)1;
}

int savefs_diary_rows(void) {
    return savefs_diary_open() && rows_on;
}

int yw4_hook_diary_update(CPUState *ctx) {
    if (!diary_slots_enabled()) return 0;
    uint64_t menu = ctx->x[0];
    if (!menu) return 0;
    __atomic_store_n(&diary_seen_ms, (uint64_t)(host_now_ns() / 1000000), __ATOMIC_RELEASE);

    uint64_t cur = hid_current_buttons();
    uint64_t edge = cur & ~prev_buttons;
    prev_buttons = cur;
    /* the keyboard's X key deletes too on the title's Continue, where the hint shows it (owner, 2026-11-17: "use X
     * like actual X on a keyboard"); it is bound to the game's Minus, which does nothing there, so that press is
     * left out (it would cancel the armed delete) */
    static int xkey_was;
    int xkey = scene_on_title() && window_key_x_held();
    if (xkey && !xkey_was) edge |= HID_BTN_X;
    if (xkey) cur &= ~HID_BTN_MINUS, edge &= ~HID_BTN_MINUS;
    xkey_was = xkey;

    if (paint_pending) {
        paint_pending = 0;
        int64_t t = host_now_ns();
        diary_paint_rows(ctx, menu);
        paint_ns_total += host_now_ns() - t;
    }
    open_updates++;
    if (!open_logged && *(const uint8_t *)(uintptr_t)(menu + 0x750)) {  /* opened (OnOpenEndLayer) */
        open_logged = 1;
        fprintf(stderr, "yw4: diary: opened in %.1f ms, %d frames (rows %s; their painting %.2f ms)\n",
                (double)(host_now_ns() - open_ns) / 1e6, open_updates, rows_on ? "stacked" : "the game's",
                (double)paint_ns_total / 1e6);
    }

    /* The menu takes input (its base's +0x1a6 and +0x1ac, both 1): not while its "Would you like to save all data
     * to your diary?" box has it, which is_general_win_active doesn't see (measured 2026-11-17: 0 with the box up,
     * 1 again after it, dev_run_20261117_065209). */
    int menu_busy = !*(const uint8_t *)(uintptr_t)(menu + 0x1a6) || !*(const uint8_t *)(uintptr_t)(menu + 0x1ac);
    if (menu_busy && (edge & (HID_BTN_DPAD_LEFT | HID_BTN_DPAD_RIGHT | HID_BTN_L | HID_BTN_R)))
        fprintf(stderr, "yw4: diary: left / right held back (a box is up)\n");
    /* Wait for OnOpenEndLayer (+0x750) and step back whenever a Yes/No or alert dialog covers Diary. */
    if (*(const uint8_t *)(uintptr_t)(menu + 0x750) == 0 || is_general_win_active() || menu_busy) {
        repeat_dir = 0;
        repeat_hold = 0;
        return 0;
    }

    if (input_cooldown > 0) {
        input_cooldown--;
        return 0;
    }

    if (rows_on) cur &= ~(uint64_t)(HID_BTN_DPAD_LEFT | HID_BTN_DPAD_RIGHT | HID_BTN_L | HID_BTN_R |
                                     HID_BTN_LSTICK_LEFT | HID_BTN_LSTICK_RIGHT),
                 edge &= cur | HID_BTN_X;  /* stacked rows: Up / Down move (the game's focus), no paging */
    int prev_held = (cur & (HID_BTN_DPAD_LEFT | HID_BTN_L | HID_BTN_LSTICK_LEFT)) != 0;
    int next_held = (cur & (HID_BTN_DPAD_RIGHT | HID_BTN_R | HID_BTN_LSTICK_RIGHT)) != 0;
    int delta = 0;
    if (edge & (HID_BTN_DPAD_RIGHT | HID_BTN_R | HID_BTN_LSTICK_RIGHT)) {
        delta = 1;
        repeat_dir = 1;
        repeat_hold = 0;
    } else if (edge & (HID_BTN_DPAD_LEFT | HID_BTN_L | HID_BTN_LSTICK_LEFT)) {
        delta = -1;
        repeat_dir = -1;
        repeat_hold = 0;
    } else if (repeat_dir == 1 && next_held && !prev_held) {
        if (++repeat_hold >= 24 && (repeat_hold - 24) % 10 == 0) delta = 1;
    } else if (repeat_dir == -1 && prev_held && !next_held) {
        if (++repeat_hold >= 24 && (repeat_hold - 24) % 10 == 0) delta = -1;
    } else {
        repeat_dir = 0;
        repeat_hold = 0;
    }

    int need_refresh = 0;
    if (delta != 0) {
        host_lock(&slot_lock);
        diary_slot = (diary_slot + delta + DIARY_SLOT_COUNT) % DIARY_SLOT_COUNT;
        delete_confirm_slot = -1;
        delete_confirm_frames = 0;
        delete_blocked = 0;
        int ns = diary_slot;
        int valid_any = slot_any_data_locked(ns);
        host_unlock(&slot_lock);
        fprintf(stderr, "yw4: diary: selected slot %d" DIARY_OF " (%s)\n", ns + 1, valid_any ? "occupied" : "empty");
        need_refresh = 1;
    } else if ((edge & HID_BTN_X) && scene_on_title() && !(rows_on && focus_auto)) {
        /* X deletes on the title's Continue only, where its hint says so (owner, 2026-11-17): not from the
         * in-game Save (where the slot being played couldn't go anyway: its next autosave would write it back) */
        host_lock(&slot_lock);
        int ds = diary_slot;
        int valid_any = slot_any_data_locked(ds);
        if (valid_any) {
            delete_blocked = 0;
            if (delete_confirm_slot == ds) {
                delete_slot_files_locked(ds);
                if (scene_on_title() && __atomic_load_n(&active_slot, __ATOMIC_ACQUIRE) == ds) {
                    set_active_slot(find_first_used_slot_locked());
                }
                delete_confirm_slot = -1;
                delete_confirm_frames = 0;
                host_unlock(&slot_lock);
                fprintf(stderr, "yw4: diary: deleted slot %d" DIARY_OF "\n", ds + 1);
                need_refresh = 1;
            } else {
                delete_confirm_slot = ds;
                delete_confirm_frames = 300;
                host_unlock(&slot_lock);
                fprintf(stderr, "yw4: diary: armed delete on slot %d" DIARY_OF " (press X again to confirm)\n", ds + 1);
                need_refresh = 1;
            }
        } else {
            host_unlock(&slot_lock);
        }
    } else if (delete_confirm_slot >= 0) {
        if ((edge & ~HID_BTN_X) != 0 || --delete_confirm_frames <= 0) {
            delete_confirm_slot = -1;
            delete_confirm_frames = 0;
            delete_blocked = 0;
            need_refresh = 1;
        }
    }

    if (need_refresh) diary_refresh_ui(ctx, menu);
    return 0;
}

/* A slot's line in the New Game picker: "3/10  Summer  (36:28 played)" or "3/10  Empty". */
static void slot_choice_text(char *out, size_t n, int s) {
    host_lock(&slot_lock);
    const DiarySlotRow *r = slot_cache[s][0].valid   ? &slot_cache[s][0]
                            : slot_cache[s][1].valid ? &slot_cache[s][1]
                                                     : NULL;
    if (!r) {
        host_unlock(&slot_lock);
        snprintf(out, n, "%d" DIARY_OF "  Empty", s + 1);
        return;
    }
    char name[37];
    memcpy(name, r->raw + 0x60, 36);
    name[36] = 0;
    uint32_t secs = 0;
    memcpy(&secs, r->raw + 0x98, sizeof secs);
    host_unlock(&slot_lock);
    snprintf(out, n, "%d" DIARY_OF "  %s  (%u:%02u played)", s + 1, name[0] ? name : "Diary", secs / 3600,
             secs / 60 % 60);
}

/* The replay (YW4_REPLAY) while it still gives the input, else NULL: once it hands over to the player (a replay
 * that only loads a save) New Game shows its slot list again (owner, 2026-11-17: the list stopped opening in a
 * session started from a save copy, after its replay had finished). */
extern int yw4_replay_done;  /* svc_hid.c */
static const char *replay_playing(void) {
    const char *r = getenv("YW4_REPLAY");
    return r && *r && !__atomic_load_n(&yw4_replay_done, __ATOMIC_ACQUIRE) ? r : NULL;
}

/* New Game on the title (owner, 2026-11-17: "choose on beginning which save slot to write to"): the overlay's
 * list popup (gamemenu_choice_begin) over the game's waiting picture, as the name entry does (swkbd.c); the game's
 * thread waits here. An occupied slot asks again before it is used. Replays and YW4_NEWGAME_SLOT=<1..10> answer at
 * once (a replay's clock counts frames, which stop while the game waits): the given slot, else the first empty one,
 * else slot 1. */
static int choose_new_game_slot(int can_go_back) {
    host_lock(&slot_lock);
    int first_empty = find_first_empty_slot_locked();
    host_unlock(&slot_lock);
    const char *given = getenv("YW4_NEWGAME_SLOT"), *replay = replay_playing();
    if (given && atoi(given) >= 1 && atoi(given) <= DIARY_SLOT_COUNT) return atoi(given) - 1;
    if ((replay && *replay) || window_repaint(1) != 0) {
        fprintf(stderr, "yw4: diary: New Game slot list not shown (%s), first empty slot taken\n",
                replay && *replay ? "replay" : "no picture to show it over");
        return first_empty >= 0 ? first_empty : 0;
    }
    fprintf(stderr, "yw4: diary: New Game slot list shown\n");
    char lines[DIARY_SLOT_COUNT][96];
    const char *rows[DIARY_SLOT_COUNT + 1];
    int n = DIARY_SLOT_COUNT + (can_go_back ? 1 : 0), sel = first_empty >= 0 ? first_empty : 0;
    for (;;) {
        for (int s = 0; s < DIARY_SLOT_COUNT; s++) slot_choice_text(lines[s], sizeof lines[s], s), rows[s] = lines[s];
        if (can_go_back) rows[DIARY_SLOT_COUNT] = "Back to the title";
        gamemenu_choice_begin("New Game: choose a Diary slot",
                              "Your new game is saved in this slot when you save it in the Diary.", rows, n, sel);
        int s;
        while ((s = gamemenu_choice_take()) < 0) {
            window_repaint(0);
            host_sleep_ns(16 * 1000000LL);
        }
        if (s >= DIARY_SLOT_COUNT) {  /* back to the title, as the game's own warning's No */
            window_repaint(0);
            return -1;
        }
        sel = s;
        host_lock(&slot_lock);
        int used = slot_any_data_locked(s);
        host_unlock(&slot_lock);
        if (!used) break;
        char title[64], body[200];
        snprintf(title, sizeof title, "Overwrite Diary %d" DIARY_OF "?", s + 1);
        snprintf(body, sizeof body, "%s. It is only replaced when you save your new game over it in the Diary.",
                 lines[s]);
        const char *const yes_no[2] = {"No, choose another slot", "Yes, overwrite it"};
        gamemenu_choice_begin(title, body, yes_no, 2, 0);
        int yes;
        while ((yes = gamemenu_choice_take()) < 0) {
            window_repaint(0);
            host_sleep_ns(16 * 1000000LL);
        }
        if (yes == 1) break;
    }
    window_repaint(0);  /* the popup gone before the game goes on */
    return sel;
}

/* The new game's slot: active from now on, its old save (if any) kept on disk until the new game first saves (New
 * Game writes nothing, research Round 4), so quitting before that keeps it. Its summary is forgotten now: the first
 * save must not carry the old game's other row into HEADERSAVE. */
static void new_game_take_slot(int slot) {
    host_lock(&slot_lock);
    new_game_slot_set = 1;
    int overwrite = slot_any_data_locked(slot);
    if (overwrite) memset(slot_cache[slot], 0, sizeof slot_cache[slot]);
    fresh_slot = overwrite ? slot : -1;
    diary_slot = slot;
    set_active_slot(slot);
    host_unlock(&slot_lock);
    fprintf(stderr, "yw4: diary: New Game in slot %d" DIARY_OF " (%s)\n", slot + 1,
            overwrite ? "replaces its save at the first save" : "empty");
}

/* The title's New Game (owner, 2026-11-17: the slot list right after New Game, and no "erase the current saved data?"
 * warning: with five slots nothing is erased). The title script (title_main) asks that with OpenCheckWindow(text
 * 0x3f4180e1): funcLuaCommand 0xade8af16 opens the box (f_299910: its argument 3 the text), then 0x024a58f2 (f_29c060)
 * is polled until it gives the answer (-1 while open; 1 Yes, 0 No), and Yes starts the new game. For that text the
 * slot list shows instead of the box, and the poll answers from it: a slot (Yes) or Back to the title (No). */
static int newgame_answer = -1;

static double guest_number(CPUState *ctx, uint64_t L, uint32_t idx) {
    GuestFn fn = yw4_lookup(yw4_image_base + 0xd235c0ull);  /* the game's lua_tonumberx(L, idx, NULL) */
    if (!fn) return 0;
    CPUState saved = *ctx;
    ctx->sp = saved.sp - 0x40;
    ctx->x[0] = L, ctx->x[1] = idx, ctx->x[2] = 0, ctx->x[30] = saved.x[30];
    fn(ctx);
    double d;
    uint64_t bits = ctx->v[0].d[0];
    memcpy(&d, &bits, sizeof d);
    *ctx = saved;
    return d;
}

/* The slot list's state while the title waits on it (the game goes on behind it, as behind its own box): 0 none,
 * 1 the list up, 2 its "Overwrite Diary n/5?" up (ng_slot the slot asked about). */
static int ng_state, ng_slot;

static void ng_row(GameMenuDiaryRow *r, int s) {
    memset(r, 0, sizeof *r);
    host_lock(&slot_lock);
    const DiarySlotRow *d = slot_cache[s][0].valid ? &slot_cache[s][0]
                            : newest_auto_slot_locked() == s && slot_cache[s][1].valid ? &slot_cache[s][1]
                                                                                         : NULL;
    if (d) {
        char name[37];
        memcpy(name, d->raw + 0x60, 36);
        name[36] = 0;
        snprintf(r->name, sizeof r->name, "%s", name[0] ? name : "Diary");
        const uint8_t *t = d->raw + 0x90;  /* year (u16), month, day, hour, minute */
        snprintf(r->date, sizeof r->date, "%02u/%02u/%04u  %u:%02u", t[2], t[3], read_le16(t), t[4], t[5]);
        uint32_t secs = read_le32(d->raw + 0x98);
        snprintf(r->play, sizeof r->play, "%03u:%02u:%02u", secs / 3600, secs / 60 % 60, secs % 60);
    }
    host_unlock(&slot_lock);
}

static void ng_open_list(int sel) {
    GameMenuDiaryRow rows[DIARY_SLOT_COUNT];
    for (int s = 0; s < DIARY_SLOT_COUNT; s++) ng_row(&rows[s], s);
    gamemenu_diary_begin("New Game: choose a slot for your new game", rows, DIARY_SLOT_COUNT, sel, "Back to the title");
    gamemenu_live_popup();  /* over the running title */
    ng_state = 1;
}

int yw4_hook_check_window_open(CPUState *ctx) {
    if (!diary_slots_enabled() || !scene_on_title() || !ctx->x[0]) return 0;
    uint64_t args = ctx->x[0], L;
    uint32_t base;
    memcpy(&L, (const void *)(uintptr_t)args, sizeof L);
    memcpy(&base, (const void *)(uintptr_t)(args + 0xc), sizeof base);
    if ((uint32_t)ctx->x[1] < 3 || guest_number(ctx, L, base + 3) != (double)0x3f4180e1u) return 0;
    host_lock(&slot_lock);
    if (user_save_dir[0]) scan_all_slots_locked();
    int first_empty = find_first_empty_slot_locked();
    host_unlock(&slot_lock);
    const char *given = getenv("YW4_NEWGAME_SLOT"), *replay = replay_playing();
    const char *show = getenv("YW4_NEWGAME_LIST");  /* tests: the list even in a replay (to see it; nothing answers) */
    if ((given && atoi(given) >= 1 && atoi(given) <= DIARY_SLOT_COUNT) || (replay && *replay && !(show && *show == '1'))) {
        /* tests answer at once (a replay's clock counts frames; the list would wait for a person) */
        new_game_take_slot(given && atoi(given) >= 1 && atoi(given) <= DIARY_SLOT_COUNT ? atoi(given) - 1
                           : first_empty >= 0                                          ? first_empty
                                                                                       : 0);
        __atomic_store_n(&newgame_answer, 1, __ATOMIC_RELEASE);
    } else {
        ng_open_list(first_empty >= 0 ? first_empty : 0);
        fprintf(stderr, "yw4: diary: New Game slot list shown\n");
    }
    ctx->x[0] = 0;  /* no results, as the box's open */
    return 1;
}

int yw4_hook_check_window_result(CPUState *ctx) {
    if (!ctx->x[0]) return 0;
    int a = -1;
    if (ng_state == 1) {
        int t = gamemenu_choice_take();
        if (t >= DIARY_SLOT_COUNT) {  /* back to the title, as the game's box's No */
            ng_state = 0, a = 0, gamemenu_diary_end();
            fprintf(stderr, "yw4: diary: New Game: back to the title\n");
        } else if (t >= 0) {
            host_lock(&slot_lock);
            int used = slot_any_data_locked(t);
            host_unlock(&slot_lock);
            if (!used) {
                new_game_take_slot(t);
                ng_state = 0, a = 1, gamemenu_diary_end();
            } else {
                GameMenuDiaryRow r;
                ng_row(&r, t);
                char title[64], body[200];
                snprintf(title, sizeof title, "Overwrite Diary %d" DIARY_OF "?", t + 1);
                snprintf(body, sizeof body,
                         "%s's diary is only replaced when you save your new game over it in the Diary.", r.name);
                gamemenu_diary_confirm(title, body, "No, choose another slot", "Yes, overwrite it");
                gamemenu_live_popup();
                ng_slot = t, ng_state = 2;
            }
        }
    } else if (ng_state == 2) {
        int y = gamemenu_choice_take();
        if (y == 1) {
            new_game_take_slot(ng_slot);
            ng_state = 0, a = 1, gamemenu_diary_end();
        } else if (y == 0) {
            ng_open_list(ng_slot);
        }
    } else {
        a = __atomic_exchange_n(&newgame_answer, -1, __ATOMIC_ACQ_REL);
        if (a < 0) return 0;  /* not ours: the game's own box */
    }
    uint64_t L;
    memcpy(&L, (const void *)(uintptr_t)ctx->x[0], sizeof L);
    guest_call3(ctx, 0xd23e30, L, (uint64_t)(int64_t)a, 0);  /* -1 while open, else the answer, as f_29c060 does */
    ctx->x[0] = 1;
    return 1;
}

/* The title opened (pause.c, SceneTitle: at the start and at each Return to Title): the next game is a new one until
 * a save is loaded, and a slot a new game was started over but never saved shows its old save again. */
void savefs_title_opened(void) {
    if (!diary_slots_enabled()) return;
    host_lock(&slot_lock);
    title_loaded_save = 0;
    new_game_slot_set = 0;
    if (fresh_slot >= 0) {
        int f = fresh_slot;
        fresh_slot = -1;
        if (user_save_dir[0]) parse_slot_from_disk_locked(f);
    }
    host_unlock(&slot_lock);
}

/* A new game asks the boy's name (swkbd.c): the first thing it asks, and nothing is saved before it (research,
 * Round 4). The hook first tried, f_32e540, is never called between title > New Game > Yes and the intro (runs
 * dev_run_20261117_023231 / 023319), so the slot is chosen here, before the name popup. A new game is one that left
 * the title without loading a save; the title has already closed when the names are asked (title 0 in
 * dev_run_20261117_024951). Not by the token: with the English patch the game gives "Nate", not "<NAME_KETA_8888>".
 * Once per new game: the later main characters' names, asked in the same run, don't ask again. */
void savefs_new_game_naming(const char *token, int girl) {
    if (!diary_slots_enabled()) return;
    host_lock(&slot_lock);
    int loaded = title_loaded_save, done = new_game_slot_set;
    host_unlock(&slot_lock);
    fprintf(stderr, "yw4: diary: name entry \"%s\"%s (save loaded %d, slot chosen %d)\n", token, girl ? " (girl)" : "",
            loaded, done);
    if (girl || loaded) return;
    host_lock(&slot_lock);
    if (!done && user_save_dir[0]) scan_all_slots_locked();
    host_unlock(&slot_lock);
    if (done) return;
    new_game_take_slot(choose_new_game_slot(0) /* at the name: no way back */);
}

/* 1 while the Diary (title > Continue, or Save in the game) is on screen: its update runs every frame while it
 * shows. The mouse's wheel then steps the slot (bindings.c: the wheel as Left / Right, which the update reads). */
int savefs_diary_open(void) {
    if (!diary_slots_enabled()) return 0;
    uint64_t seen = __atomic_load_n(&diary_seen_ms, __ATOMIC_ACQUIRE);
    return seen && (uint64_t)(host_now_ns() / 1000000) - seen < 200;
}
