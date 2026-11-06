/* The bug report's zip and GitHub link (bugreport.h). Runs on the present's thread while the overlay shows (the
 * game is paused), so taking a moment to read and write a few MB is fine. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#undef GPTR  /* windows.h's; hle.h (through savefs.h) has its own */
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <SDL3/SDL.h>

#include "bugreport.h"
#include "savefs.h"
#include "settings.h"
#include "version.h"
#include "vk.h"

const char *yw4_log_path(void);  /* log.c */

#define REPO_ISSUES "https://github.com/ILFforever/YW4Recomp/issues/new"
#define GAME_VERSION "2.2.0"

/* ---- bytes */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static void put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) return;
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put16(Buf *b, unsigned v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(b, x, 2); }
static void put32(Buf *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void putf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void putf(Buf *b, const char *fmt, ...) {
    char s[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    if (n > 0) put(b, s, (size_t)(n < (int)sizeof s ? n : (int)sizeof s - 1));
}

static int read_file(const char *path, Buf *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) put(out, chunk, got);
    fclose(f);
    return 0;
}

/* ---- a zip that stores its files (method 0), names in UTF-8 */

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct {
    char name[260];
    uint32_t crc, size, offset;
} Entry;

typedef struct {
    Buf out;
    Entry e[256];
    int n;
    unsigned dtime, ddate;  /* MS-DOS time and date of the report */
} Zip;

static void zip_add(Zip *z, const char *name, const uint8_t *data, size_t size) {
    if (z->n >= (int)(sizeof z->e / sizeof *z->e)) return;
    Entry *e = &z->e[z->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->crc = crc32_of(data, size), e->size = (uint32_t)size, e->offset = (uint32_t)z->out.n;
    put32(&z->out, 0x04034b50), put16(&z->out, 20), put16(&z->out, 0x0800), put16(&z->out, 0);
    put16(&z->out, z->dtime), put16(&z->out, z->ddate);
    put32(&z->out, e->crc), put32(&z->out, e->size), put32(&z->out, e->size);
    put16(&z->out, (unsigned)strlen(e->name)), put16(&z->out, 0);
    put(&z->out, e->name, strlen(e->name));
    if (size) put(&z->out, data, size);
}

static void zip_end(Zip *z) {
    uint32_t start = (uint32_t)z->out.n;
    for (int i = 0; i < z->n; i++) {
        const Entry *e = &z->e[i];
        put32(&z->out, 0x02014b50), put16(&z->out, 20), put16(&z->out, 20), put16(&z->out, 0x0800), put16(&z->out, 0);
        put16(&z->out, z->dtime), put16(&z->out, z->ddate);
        put32(&z->out, e->crc), put32(&z->out, e->size), put32(&z->out, e->size);
        put16(&z->out, (unsigned)strlen(e->name)), put16(&z->out, 0), put16(&z->out, 0), put16(&z->out, 0),
            put16(&z->out, 0);
        put32(&z->out, 0), put32(&z->out, e->offset);
        put(&z->out, e->name, strlen(e->name));
    }
    uint32_t size = (uint32_t)z->out.n - start;
    put32(&z->out, 0x06054b50), put16(&z->out, 0), put16(&z->out, 0), put16(&z->out, (unsigned)z->n),
        put16(&z->out, (unsigned)z->n);
    put32(&z->out, size), put32(&z->out, start), put16(&z->out, 0);
}

static void zip_file(Zip *z, const char *name, const char *path) {
    Buf b = {0};
    if (read_file(path, &b) == 0) zip_add(z, name, b.p, b.n);
    free(b.p);
}

/* every file under dir, as prefix/<path inside dir> */
static void zip_dir(Zip *z, const char *dir, const char *prefix, int depth) {
    DIR *d = opendir(dir);
    if (!d || depth > 8) {
        if (d) closedir(d);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char path[1024], name[600];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        snprintf(name, sizeof name, "%s/%s", prefix, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if ((st.st_mode & S_IFMT) == S_IFDIR) zip_dir(z, path, name, depth + 1);
        else zip_file(z, name, path);
    }
    closedir(d);
}

/* ---- what goes in */

/* the paused picture as a 24-bit BMP, made smaller by whole steps until it is at most 1600 wide */
static void picture_bmp(const BugReport *r, Buf *out) {
    int k = 1;
    while (r->picture_w / k > 1600) k++;
    int w = r->picture_w / k, h = r->picture_h / k, row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + (uint32_t)row * (uint32_t)h;
    put16(out, 0x4d42), put32(out, size), put32(out, 0), put32(out, 54);
    put32(out, 40), put32(out, (uint32_t)w), put32(out, (uint32_t)h), put16(out, 1), put16(out, 24), put32(out, 0);
    put32(out, (uint32_t)row * (uint32_t)h), put32(out, 2835), put32(out, 2835), put32(out, 0), put32(out, 0);
    uint8_t *line = calloc((size_t)row, 1);
    if (!line) return;
    for (int y = h - 1; y >= 0; y--) {  /* bottom row first */
        for (int x = 0; x < w; x++) {
            unsigned s[3] = {0, 0, 0};
            for (int j = 0; j < k; j++)
                for (int i = 0; i < k; i++) {
                    const uint8_t *p = r->picture + ((size_t)(y * k + j) * r->picture_w + (size_t)(x * k + i)) * 4;
                    s[0] += p[0], s[1] += p[1], s[2] += p[2];
                }
            line[x * 3 + 0] = (uint8_t)(s[2] / (unsigned)(k * k));  /* B G R */
            line[x * 3 + 1] = (uint8_t)(s[1] / (unsigned)(k * k));
            line[x * 3 + 2] = (uint8_t)(s[0] / (unsigned)(k * k));
        }
        put(out, line, (size_t)row);
    }
    free(line);
}

static void windows_version(char *out, size_t n) {
#ifdef _WIN32
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    RtlGetVersionFn get = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (get && get(&v) == 0) {
        snprintf(out, n, "Windows %s (%lu.%lu.%lu)", v.dwBuildNumber >= 22000 ? "11" : "10", v.dwMajorVersion,
                 v.dwMinorVersion, v.dwBuildNumber);
        return;
    }
    snprintf(out, n, "Windows");
#else
    snprintf(out, n, "%s", SDL_GetPlatform());
#endif
}

static void gpu_line(char *out, size_t n) {
    const VkPhysicalDeviceProperties *p = &vk.props;
    if (!p->deviceName[0]) {
        snprintf(out, n, "unknown");
        return;
    }
    uint32_t v = p->driverVersion;
    if (p->vendorID == 0x10DE)  /* NVIDIA's own numbering: 581.57 */
        snprintf(out, n, "%s (driver %u.%02u)", p->deviceName, v >> 22 & 0x3ff, v >> 14 & 0xff);
    else
        snprintf(out, n, "%s (driver %u.%u.%u)", p->deviceName, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
                 VK_API_VERSION_PATCH(v));
}

static void version_line(char *out, size_t n) {
    snprintf(out, n, "%s (%s, built %s)", yw4_version, yw4_git_commit, yw4_build_date);
}

int bugreport_save(const BugReport *r, char *path, size_t n) {
    time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    const char *dir = getenv("YW4_REPORT_DIR");
    if (!dir || !*dir) dir = "reports";
    MKDIR(dir);
    char stamp[32], rel[600];
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", &t);
    snprintf(rel, sizeof rel, "%s/yw4_report_%s.zip", dir, stamp);
#ifdef _WIN32
    if (!_fullpath(path, rel, n)) snprintf(path, n, "%s", rel);
#else
    if (!realpath(dir, path)) snprintf(path, n, "%s", rel);
    else snprintf(path + strlen(path), n - strlen(path), "/yw4_report_%s.zip", stamp);
#endif
    static Zip z;  /* large: not on the stack */
    memset(&z, 0, sizeof z);
    z.dtime = (unsigned)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
    z.ddate = (unsigned)((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);

    char ver[128], gpu[300], os[96], when[32];
    version_line(ver, sizeof ver), gpu_line(gpu, sizeof gpu), windows_version(os, sizeof os);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &t);
    Buf txt = {0};
    putf(&txt, "YW4 Recomp bug report\r\n\r\nWhat's wrong: %s\r\nHow often: %s\r\nWhat happened:\r\n", r->kind[0] ? r->kind : "-",
         r->often[0] ? r->often : "-");
    put(&txt, r->text, strlen(r->text));
    putf(&txt, "\r\n\r\nVersion: %s\r\nGame: Yo-kai Watch 4++ %s\r\nGPU: %s\r\nSystem: %s\r\nWindow: %dx%d\r\nTime: %s\r\n",
         ver, GAME_VERSION, gpu, os, r->window_w, r->window_h, when);
    putf(&txt, "Attached: %s%s%s\r\n", r->attach & BUG_ATTACH_SHOT ? "screenshot " : "",
         r->attach & BUG_ATTACH_LOG ? "log settings " : "", r->attach & BUG_ATTACH_SAVE ? "save" : "");
    zip_add(&z, "report.txt", txt.p, txt.n);
    free(txt.p);

    if ((r->attach & BUG_ATTACH_SHOT) && r->picture && r->picture_w > 0 && r->picture_h > 0) {
        Buf bmp = {0};
        picture_bmp(r, &bmp);
        zip_add(&z, "screenshot.bmp", bmp.p, bmp.n);
        free(bmp.p);
    }
    if (r->attach & BUG_ATTACH_LOG) {
        if (yw4_log_path()[0]) zip_file(&z, "log.txt", yw4_log_path());
        if (settings_file_name()[0]) zip_file(&z, "settings.ini", settings_file_name());
    }
    if ((r->attach & BUG_ATTACH_SAVE) && savefs_root()[0]) zip_dir(&z, savefs_root(), "save", 0);
    zip_end(&z);

    FILE *f = fopen(rel, "wb");
    int ok = f && fwrite(z.out.p, 1, z.out.n, f) == z.out.n;
    if (f) ok = fclose(f) == 0 && ok;
    fprintf(stderr, "yw4: bug report: %s %s (%d files, %zu bytes)\n", ok ? "saved" : "NOT saved", path, z.n, z.out.n);
    free(z.out.p);
    return ok ? 0 : -1;
}

/* ---- the link */

/* opens url in the browser (or Explorer); YW4_REPORT_OPEN=0 (test runs): only writes it in the log */
static void open_url(const char *url) {
    const char *e = getenv("YW4_REPORT_OPEN");
    if (e && *e == '0') {
        fprintf(stderr, "yw4: bug report: would open %s\n", url);
        return;
    }
    if (!SDL_OpenURL(url)) fprintf(stderr, "yw4: bug report: can't open %s: %s\n", url, SDL_GetError());
}

static void url_add(char *url, size_t n, const char *key, const char *value, size_t max) {
    size_t at = strlen(url);
    at += (size_t)snprintf(url + at, at < n ? n - at : 0, "&%s=", key);
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p && at + 4 < n; p++, used++) {
        unsigned char ch = *p;
        if (used >= max && (ch & 0xC0) != 0x80) break;  /* long: cut, but never inside a character */
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '~') {
            url[at++] = (char)ch;
        } else {
            url[at++] = '%', url[at++] = hex[ch >> 4], url[at++] = hex[ch & 15];
        }
    }
    url[at < n ? at : n - 1] = 0;
}

void bugreport_open_github(const BugReport *r) {
    static char url[12000];
    char title[200], ver[128], gpu[300], os[96], pc[420];
    /* the title: the kind and the start of what happened */
    snprintf(title, sizeof title, "%s%s%.80s", r->kind[0] ? r->kind : "Bug", r->text[0] ? ": " : "", r->text);
    version_line(ver, sizeof ver), gpu_line(gpu, sizeof gpu), windows_version(os, sizeof os);
    snprintf(pc, sizeof pc, "%s, %s", gpu, os);
    snprintf(url, sizeof url, "%s?template=bug.yml", REPO_ISSUES);
    url_add(url, sizeof url, "title", title, 120);
    url_add(url, sizeof url, "what", r->text, 4000);
    url_add(url, sizeof url, "often", r->often, 40);
    url_add(url, sizeof url, "version", ver, 120);
    url_add(url, sizeof url, "pc", pc, 300);
    open_url(url);
}

void bugreport_show_folder(const char *zip) {
    char url[1200] = "file:///";
    size_t at = strlen(url);
    for (const char *p = zip; *p && at + 1 < sizeof url; p++) url[at++] = *p == '\\' ? '/' : *p;
    url[at] = 0;
    char *slash = strrchr(url, '/');  /* the folder, not the zip (which Windows would open as a folder too) */
    if (slash && slash > url + 8) *slash = 0;
    open_url(url);
}
