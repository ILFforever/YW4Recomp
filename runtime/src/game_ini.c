/* game.ini: how yw4.exe runs on its own, without arguments (decision 0050).
 *
 * The launcher writes it next to yw4.exe; a double-click then starts the game. One `YW4_...=value` per line, as
 * in settings.ini, `#` starts a comment. The game's folders:
 *
 *   YW4_EXEFS=../data/exefs        main.bin and sdk.bin (python -m tools.yw4.ingest)
 *   YW4_ROMFS=../data/romfs        the game's RomFS
 *   YW4_SAVE=save                  save data (optional: main.c's default_save_dir without it)
 *
 * and any other variable of docs/debugging.md (YW4_DLC_DIR, YW4_MODS, YW4_SETTINGS_FILE, YW4_HOME...). A value
 * naming a path (the variable's name ends in _DIR, _FILE, _HOME, or is one of the three above or YW4_MODS) is
 * relative to game.ini's folder, so the install folder can be moved. A variable already in the environment wins
 * over its line, as with settings.ini. With arguments on the command line, game.ini is not read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_filesystem.h>

#include "game_ini.h"

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && !strcmp(s + n - m, tail);
}

static int is_path(const char *name) {
    return !strcmp(name, "YW4_EXEFS") || !strcmp(name, "YW4_ROMFS") || !strcmp(name, "YW4_SAVE") ||
           !strcmp(name, "YW4_MODS") || ends_with(name, "_DIR") || ends_with(name, "_FILE") || ends_with(name, "_HOME");
}

static int is_absolute(const char *p) {
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

int game_ini_load(void) {
    const char *base = SDL_GetBasePath();  /* the exe's folder, ending in a separator */
    char path[1100];
    snprintf(path, sizeof path, "%sgame.ini", base ? base : "");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1100];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || strncmp(line, "YW4_", 4) != 0) continue;
        *eq = 0;
        const char *value = eq + 1;
        if (getenv(line)) continue;  /* the environment wins */
        char full[2200];
        if (is_path(line) && *value && !is_absolute(value) && strcmp(value, "none") != 0) {
            snprintf(full, sizeof full, "%s%s", base ? base : "", value);
            value = full;
        }
        set_env(line, value);
        n++;
    }
    fclose(f);
    fprintf(stderr, "yw4: game.ini: %d setting(s) from %s\n", n, path);
    return 1;
}
