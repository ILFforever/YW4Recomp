/* nn::web's offline pages: the title screen's Manual button (decision 0082's crash survey, 2026-11-19).
 *
 * On a Switch, nn::web::ShowOfflineHtmlPage starts the system's web applet on a page of the game's own HtmlDocument
 * content ("manual.htdocs/index.html"); the sdk's version asks for a library applet, which we don't have, so the run
 * stopped there. Here the same pages, taken from the player's own NSP or XCI at extract time (tools/yw4/manual.py,
 * into work/manual; YW4_MANUAL_DIR in game.ini), open in the PC's web browser, and the game goes on at once as if the
 * player had closed the manual. Without them a popup says how to get them (a rebuild in the launcher). An override of
 * the sdk function (recomp/overrides.toml). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>

#include "gamemenu.h"
#include "hle.h"
#include "host.h"
#include "window.h"
#include "yw4load.h"

/* The document path the game put in the argument (ShowOfflineHtmlPageArg: a block of typed fields, the path one of
 * them, NUL-terminated): the first run of printable characters ending in ".html" / ".htm" in its first 8 KiB. */
static int document_path(const uint8_t *arg, char *out, size_t n) {
    for (size_t i = 0; i + 5 < 0x2000; i++) {
        if (arg[i] < 0x21 || arg[i] > 0x7e || (i && arg[i - 1] >= 0x21 && arg[i - 1] <= 0x7e)) continue;  /* a start */
        size_t k = i;
        while (k < 0x2000 && arg[k] >= 0x21 && arg[k] <= 0x7e) k++;
        if (k < 0x2000 && arg[k] == 0 && k - i < n &&
            ((k - i > 5 && !memcmp(arg + k - 5, ".html", 5)) || (k - i > 4 && !memcmp(arg + k - 4, ".htm", 4)))) {
            memcpy(out, arg + i, k - i);
            out[k - i] = 0;
            return 1;
        }
        i = k;
    }
    return 0;
}

/* file:// URL of a local path: slashes forward, anything but plain letters, digits and - . _ ~ / : %-encoded */
static void file_url(const char *path, char *out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = (size_t)snprintf(out, n, "file:///");
    for (const unsigned char *p = (const unsigned char *)path; *p && o + 4 < n; p++) {
        unsigned char ch = *p == '\\' ? '/' : *p;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-._~/:", ch))
            out[o++] = (char)ch;
        else
            out[o++] = '%', out[o++] = hex[ch >> 4], out[o++] = hex[ch & 15];
    }
    out[o < n ? o : n - 1] = 0;
}

static void popup(const char *title, const char *body) {
    if (window_repaint(1) != 0) return;
    gamemenu_message_begin(title, body);
    char ignored[8];
    while (!gamemenu_prompt_take(ignored, sizeof ignored)) {
        window_repaint(0);
        host_sleep_ns(16 * 1000000LL);
    }
    window_repaint(0);
}

#ifndef _WIN32
/* realpath() into a 1024-byte buffer can overflow (PATH_MAX is 4096): let it allocate, then copy what fits. 0 if the
 * path doesn't resolve. */
static int resolve_path(const char *src, char *dst, size_t n) {
    char *rp = realpath(src, NULL);
    if (!rp) return 0;
    snprintf(dst, n, "%s", rp);
    free(rp);
    return 1;
}
#endif

/* ShowOfflineHtmlPage(OfflineHtmlPageReturnValue *, const ShowOfflineHtmlPageArg &) -> Result. The return value keeps
 * what its constructor put there (the page closed normally). */
void yw4_web_ShowOfflineHtmlPage(CPUState *c) {
    char doc[512] = "", rel[1024], full[1024], url[3200];
    if (!document_path(GPTR(c->x[1]), doc, sizeof doc)) snprintf(doc, sizeof doc, "manual.htdocs/index.html");
    const char *dir = getenv("YW4_MANUAL_DIR");
    snprintf(rel, sizeof rel, "%s/%s", dir && *dir ? dir : "work/manual", doc);
#ifdef _WIN32
    if (!_fullpath(full, rel, sizeof full)) snprintf(full, sizeof full, "%s", rel);
#else
    if (!resolve_path(rel, full, sizeof full)) snprintf(full, sizeof full, "%s", rel);
#endif
    FILE *f = fopen(full, "rb");
    if (!f) {
        fprintf(stderr, "yw4: web: the game's page %s is not here (%s): python -m tools.yw4.manual takes it from the "
                        "NSP or XCI\n", doc, full);
        popup("The manual isn't here yet",
              "This install doesn't have the game's manual pages. They come from your own game files: rebuild the "
              "game in the launcher (Settings, Rebuild) and the Manual button opens them in your web browser.");
        c->x[0] = 0;
        return;
    }
    fclose(f);
    /* the one-page manual in the site's look (tools/yw4/manual.py, manual.html), at the asked-for page's section
     * ("manual.htdocs/page08.html" -> manual.html#page08); without it, the game's own page */
    char one[1024];
    snprintf(one, sizeof one, "%s/manual.html", dir && *dir ? dir : "work/manual");
#ifdef _WIN32
    if (!_fullpath(full, one, sizeof full)) snprintf(full, sizeof full, "%s", one);
#else
    if (!resolve_path(one, full, sizeof full)) snprintf(full, sizeof full, "%s", one);
#endif
    char anchor[128] = "";
    if ((f = fopen(full, "rb")) != NULL) {
        fclose(f);
        const char *base = strrchr(doc, '/') ? strrchr(doc, '/') + 1 : doc;
        size_t len = strcspn(base, ".");
        if (len && len < sizeof anchor - 1) anchor[0] = '#', memcpy(anchor + 1, base, len), anchor[len + 1] = 0;
    } else {
#ifdef _WIN32
        if (!_fullpath(full, rel, sizeof full)) snprintf(full, sizeof full, "%s", rel);
#else
        if (!resolve_path(rel, full, sizeof full)) snprintf(full, sizeof full, "%s", rel);
#endif
    }
    file_url(full, url, sizeof url);
    strncat(url, anchor, sizeof url - strlen(url) - 1);
    int ok = SDL_OpenURL(url);
    fprintf(stderr, "yw4: web: the game's page %s %s %s\n", doc, ok ? "opened in the browser:" : "couldn't be opened:",
            ok ? url : SDL_GetError());
    if (!ok) popup("The manual couldn't be opened", "Your web browser didn't open the manual. It is in the game's "
                                                    "folder: work/manual/manual.html.");
    c->x[0] = 0;
}
