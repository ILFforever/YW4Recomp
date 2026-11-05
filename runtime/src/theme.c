/* Themes (theme.h): the built-in look, and the launcher's theme files read into the same roles. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "theme.h"

/* the built-in look: the overlay's design colours (gamemenu.c's before themes) and the HUD's (hud_style.c) */
static const Theme recomp = {
    .ink = 0x2b2338, .night = 0x2a1b4c, .plum = 0x3e2a6e, .paper = 0xf8f4fc, .gold = 0xe3be5c, .ring = 0xe3be5c, .line = 0xe2d5ef,
    .white = 0xffffff, .snow = 0xfdfcfe, .lilac = 0xd9cdea, .on_dark = 0xd9cdea, .muted = 0x605673, .wine = 0x8c1f3f,
    .shade = 0x160e26, .soft = 0xefe7f7, .edge = 0xc6b2da, .tag = 0xe8dff4, .vsoft = 0x5b4691, .on_gold = 0x2a1b4c,
    .pop_deep = 0x2b1a4d, .pop_gold = 0xe1b75c, .pop_white = 0xfdfcff, .pop_ink = 0x231e33, .pop_lilac = 0xf8f5fd,
    .pop_line = 0xe1d5f1, .pop_border = 0x312451, .pop_shadow = 0x0f0028, .pop_backdrop = 0x1a0a35,
    .hud_text = 0xfdfcfe, .hud_label = 0xd9cdea, .hud_card = 0x2a1b4c, .hud_plot = 0x3e2a6e, .hud_target = 0xc6b2da,
};

/* Which CSS token each role takes (launcher/ui/style.css's names). A theme without the token keeps the built-in
 * colour for that role. */
#define ROLE(field, token) {offsetof(Theme, field), token}
static const struct {
    size_t at;
    const char *token;
} roles[] = {
    ROLE(ink, "--ink"), ROLE(night, "--violet-deep"), ROLE(plum, "--violet"), ROLE(paper, "--lilac-0"),
    ROLE(gold, "--gold"), ROLE(ring, "--select"), ROLE(line, "--lilac-2"), ROLE(white, "--white"), ROLE(snow, "--white"),
    ROLE(lilac, "--lilac-2"), ROLE(on_dark, "--bar-ink"), ROLE(muted, "--ink-2"), ROLE(wine, "--danger"),
    ROLE(soft, "--lilac-1"), ROLE(edge, "--lilac-3"), ROLE(tag, "--lilac-1"), ROLE(vsoft, "--violet-soft"),
    ROLE(on_gold, "--ink"), ROLE(pop_deep, "--violet-deep"), ROLE(pop_gold, "--gold"),
    ROLE(pop_white, "--white"), ROLE(pop_ink, "--ink"), ROLE(pop_lilac, "--lilac-0"), ROLE(pop_line, "--lilac-2"),
    ROLE(pop_border, "--violet"), ROLE(hud_text, "--white"), ROLE(hud_label, "--bar-ink"),
    ROLE(hud_card, "--violet-deep"), ROLE(hud_plot, "--violet"), ROLE(hud_target, "--lilac-3"),
};

/* ---- CSS colours: #rgb, #rrggbb, rgb(r g b / a) or rgb(r, g, b), oklch(L% C H / a); the alpha is dropped */

static uint8_t to8(double v) { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0 + 0.5); }
static double encode(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055; }

/* OKLCH -> sRGB (Björn Ottosson's OKLab matrices) */
static uint32_t oklch(double L, double C, double H) {
    double a = C * cos(H * 3.14159265358979 / 180), b = C * sin(H * 3.14159265358979 / 180);
    double l = L + 0.3963377774 * a + 0.2158037573 * b, m = L - 0.1055613458 * a - 0.0638541728 * b,
           s = L - 0.0894841775 * a - 1.2914855480 * b;
    l = l * l * l, m = m * m * m, s = s * s * s;
    double r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    double bl = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    return (uint32_t)to8(encode(r)) << 16 | (uint32_t)to8(encode(g)) << 8 | to8(encode(bl));
}

/* the numbers in "( ... )": up to 3 before any '/', a percent sign marks one (as 0..1) */
static int numbers(const char *p, double v[3], int pct[3]) {
    int n = 0;
    while (*p && *p != ')' && *p != '/' && n < 3) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        char *e;
        double d = strtod(p, &e);
        if (e == p) break;
        p = e;
        pct[n] = *p == '%';
        if (pct[n]) p++;
        v[n++] = d;
    }
    return n;
}

static int parse_colour(const char *p, uint32_t *out) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#') {
        unsigned x;
        int len = 0;
        while ((p[1 + len] >= '0' && p[1 + len] <= '9') || ((p[1 + len] | 32) >= 'a' && (p[1 + len] | 32) <= 'f')) len++;
        if (len != 3 && len != 6 && len != 8) return 0;
        char hex[7];
        if (len == 3) {
            for (int k = 0; k < 3; k++) hex[2 * k] = hex[2 * k + 1] = p[1 + k];
        } else {
            memcpy(hex, p + 1, 6);
        }
        hex[6] = 0;
        if (sscanf(hex, "%x", &x) != 1) return 0;
        *out = x;
        return 1;
    }
    double v[3];
    int pct[3];
    if (!strncmp(p, "rgb(", 4) || !strncmp(p, "rgba(", 5)) {
        if (numbers(strchr(p, '(') + 1, v, pct) != 3) return 0;
        uint32_t c = 0;
        for (int k = 0; k < 3; k++) c = c << 8 | to8(pct[k] ? v[k] / 100 : v[k] / 255);
        *out = c;
        return 1;
    }
    if (!strncmp(p, "oklch(", 6)) {
        if (numbers(p + 6, v, pct) != 3) return 0;
        *out = oklch(pct[0] ? v[0] / 100 : v[0], pct[1] ? v[1] * 0.004 : v[1], v[2]);  /* C: 100% = 0.4 */
        return 1;
    }
    return 0;  /* var(...) and the rest: the role keeps its built-in colour */
}

/* ---- the folder */

enum { MAX_THEMES = 32 };
static struct {
    char id[64], name[64];
    Theme colours;
} themes[MAX_THEMES];
static int nthemes = -1, chosen;
static Theme current;  /* what the overlay and the HUD read (a copy: changed on the window's thread, read on the present's) */

/* a theme file into t, over the built-in colours; its name from the "name:" comment */
static int load_file(const char *path, Theme *t, char *name, size_t nn) {
    size_t size = 0;
    char *text = SDL_LoadFile(path, &size);
    if (!text) return 0;
    *t = recomp;
    const char *nm = strstr(text, "name:");
    if (nm) {
        nm += 5;
        while (*nm == ' ') nm++;
        size_t k = 0;
        while (nm[k] && nm[k] != '*' && nm[k] != '\n' && k + 1 < nn) k++;
        while (k && nm[k - 1] == ' ') k--;
        memcpy(name, nm, k), name[k] = 0;
    }
    for (size_t r = 0; r < sizeof roles / sizeof *roles; r++) {
        size_t tl = strlen(roles[r].token);
        for (const char *p = text; (p = strstr(p, roles[r].token)); p += tl) {
            const char *q = p + tl;
            if (*q != ':' && *q != ' ') continue;  /* "--lilac-1" is not "--lilac-10" */
            while (*q == ' ') q++;
            if (*q != ':') continue;
            uint32_t c;
            if (parse_colour(q + 1, &c)) *(uint32_t *)((char *)t + roles[r].at) = c;
            break;
        }
    }
    /* the tint and the popup's shadow: the dark, darker */
    uint32_t n = t->night;
    t->shade = t->pop_shadow = t->pop_backdrop = ((n >> 16 & 255) * 2 / 5) << 16 | ((n >> 8 & 255) * 2 / 5) << 8 | (n & 255) * 2 / 5;
    SDL_free(text);
    return 1;
}

static const char *themes_dir(void) {
    static char dir[1024];
    const char *e = getenv("YW4_THEMES_DIR");
    if (e && *e) {
        snprintf(dir, sizeof dir, "%s", e);
        return dir;
    }
    const char *base = SDL_GetBasePath();
    static const char *const tries[] = {"themes", "../../launcher/ui/themes"};  /* next to yw4.exe; the repo's */
    for (size_t i = 0; base && i < sizeof tries / sizeof *tries; i++) {
        snprintf(dir, sizeof dir, "%s%s", base, tries[i]);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(dir, &info) && info.type == SDL_PATHTYPE_DIRECTORY) return dir;
    }
    return NULL;
}

static int by_id(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void load_all(void) {
    if (nthemes >= 0) return;
    nthemes = 1;
    snprintf(themes[0].id, sizeof themes[0].id, "recomp");
    snprintf(themes[0].name, sizeof themes[0].name, "YW4 Recomp");
    themes[0].colours = recomp;
    const char *dir = themes_dir();
    int count = 0;
    char **files = dir ? SDL_GlobDirectory(dir, "*.css", 0, &count) : NULL;
    for (int i = 0; files && i < count && nthemes < MAX_THEMES; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        const char *slash = strrchr(files[i], '/');
        const char *file = slash ? slash + 1 : files[i];
        size_t len = strlen(file) - 4;  /* without ".css" */
        if (len == 0 || len >= sizeof themes[0].id) continue;
        memcpy(themes[nthemes].id, file, len), themes[nthemes].id[len] = 0;
        if (!strcmp(themes[nthemes].id, "recomp")) continue;  /* the built-in one has no file */
        snprintf(themes[nthemes].name, sizeof themes[nthemes].name, "%s", themes[nthemes].id);
        if (load_file(path, &themes[nthemes].colours, themes[nthemes].name, sizeof themes[nthemes].name)) nthemes++;
    }
    SDL_free(files);
    qsort(themes + 1, (size_t)(nthemes - 1), sizeof themes[0], by_id);  /* id first in the struct: sorts by id */
    const char *want = getenv("YW4_THEME");
    chosen = 0;
    for (int i = 0; want && i < nthemes; i++)
        if (!strcmp(themes[i].id, want)) chosen = i;
    current = themes[chosen].colours;
    fprintf(stderr, "yw4: themes: %d (%s), %s\n", nthemes, dir ? dir : "no themes folder", themes[chosen].id);
}

const Theme *theme_colors(void) {
    load_all();
    return &current;
}
int theme_count(void) { return load_all(), nthemes; }
int theme_index(void) { return load_all(), chosen; }
const char *theme_name(int i) { return load_all(), i >= 0 && i < nthemes ? themes[i].name : ""; }
const char *theme_id(void) { return load_all(), themes[chosen].id; }

void theme_step(int dir) {
    load_all();
    chosen = (chosen + (dir < 0 ? nthemes - 1 : 1)) % nthemes;
    current = themes[chosen].colours;
}
