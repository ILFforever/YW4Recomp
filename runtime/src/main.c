/* yw4: load main and the sdk, then start the game the way the Switch does (decision 0014).
 *
 *   yw4 <exefs work directory: main.bin, sdk.bin> <romfs folder> [save data folder]
 *
 * Without a save folder (on the command line or as YW4_SAVE in game.ini) the saves go to the user's data folder:
 * $XDG_DATA_HOME/YW4Recomp/save (~/.local/share/YW4Recomp/save) on Linux, %APPDATA%/YW4 Recomp/save on Windows.
 *
 * rtld is not needed: the loader relocates both modules. Then the main guest thread calls
 * the sdk's nn::init::Start(thread handle, argument region, init, fini), as rtld would. It
 * calls back to run the sdk's DT_INIT right after nnosInitialize, and main's DT_INIT
 * after nninitStartup (when the game has set up its heap), then calls nnMain. The two
 * callbacks are host functions (yw4_host_function), as in LAS Recomp's start-up.
 *
 * The process's main thread runs the window (window.c, decision 0019); the main guest
 * thread gets its own host thread.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>  /* _chdir, _getcwd */
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>

#include "game_ini.h"
#include "host.h"
#include "kernel.h"
#include "romfs.h"
#include "savefs.h"
#include "settings.h"
#include "version.h"
#include "window.h"
#include "yw4load.h"
#include "host.h"

static void init_module(CPUState *ctx, size_t i) {
    const Yw4Module *m = &yw4_modules[i];
    fprintf(stderr, "yw4: running %s DT_INIT (%s+%#" PRIx64 ")\n", m->name, m->name, m->dt_init);
    yw4_resolve(*m->base + m->dt_init)(ctx);
    fprintf(stderr, "yw4: %s DT_INIT returned\n", m->name);
    yw4_boot_mark(i == YW4_SDK ? "sdk's constructors done" : "main's constructors done");
}
static void init_sdk(CPUState *ctx) { init_module(ctx, YW4_SDK); }
static void init_main(CPUState *ctx) { init_module(ctx, YW4_MAIN); }

/* The main guest thread, on its own host thread while the window owns the main one. */
static void start_guest(void *unused) {
    (void)unused;
    CPUState *ctx = calloc(1, sizeof *ctx);
    uint8_t *stack = yw4_reserve(YW4_STACK_BASE, yw4_main_stack_size);
    if (!ctx || !stack) {
        fprintf(stderr, "yw4: out of memory\n");
        yw4_exit(1, __FILE__, __LINE__);
    }
    ctx->sp = ((uint64_t)(uintptr_t)(stack + yw4_main_stack_size)) & ~15ull;
    ctx->x[0] = yw4_kernel_init(ctx, (uint64_t)(uintptr_t)stack, yw4_main_stack_size);
    ctx->x[1] = 0;  /* argument region: none */
    ctx->x[2] = yw4_host_function(init_sdk);
    ctx->x[3] = yw4_host_function(init_main);
    ctx->x[30] = 0;  /* returning from the entry is a normal C return; x30 is never jumped to */
    fprintf(stderr, "yw4: main thread: stack %#" PRIx64 " bytes, priority %u; running nn::init::Start (sdk+%#" PRIx64 ")\n",
            yw4_main_stack_size, yw4_main_priority, yw4_entry_init_start);
    yw4_resolve(yw4_sdk_base + yw4_entry_init_start)(ctx);
    fprintf(stderr, "yw4: nn::init::Start returned, x0=%#" PRIx64 "\n", ctx->x[0]);
}

void yw4_log_start(void);  /* log.c */

/* The runtime's own files are found by paths starting work/ (shaders, mods, logs, screenshots...), relative to
 * the current folder. So that the game works from wherever it is started (a double-click, a launcher), it first
 * goes to its home folder (decision 0048): YW4_HOME if set, else the folder above work/ when yw4.exe sits in
 * <home>/work/build/ (the layout recomp.build makes, for the owner and for an install), else it stays put. The
 * paths it was given are made absolute first, so they still name the same folders. */
static void absolute(char **arg, char *buf, size_t size) {
#ifdef _WIN32
    if (_fullpath(buf, *arg, size)) *arg = buf;
#else
    if (realpath(*arg, buf)) *arg = buf;  /* buf is PATH_MAX long */
    (void)size;
#endif
}

static void go_home(void) {
    char home[1024] = "";
    const char *env = getenv("YW4_HOME");
    if (env && *env) {
        snprintf(home, sizeof home, "%s", env);
    } else {
        const char *base = SDL_GetBasePath();  /* the exe's folder, ending in a separator */
        if (!base) return;
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", base);
        size_t n = strlen(dir);
        while (n && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = 0;  /* .../work/build */
        char *cut = strrchr(dir, '/'), *cut2 = strrchr(dir, '\\');
        if (cut2 > cut) cut = cut2;
        if (!cut) return;
        *cut = 0;  /* .../work */
        char *name = strrchr(dir, '/'), *name2 = strrchr(dir, '\\');
        if (name2 > name) name = name2;
        if (!name || strcmp(name + 1, "work") != 0) return;  /* not the <home>/work/build layout: stay */
        *name = 0;
        snprintf(home, sizeof home, "%s", dir);
    }
#ifdef _WIN32
    if (_chdir(home) != 0) return;
#else
    if (chdir(home) != 0) return;
#endif
}

/* The save folder when none is given: the platform's per-user data folder, so the saves don't live inside the
 * game's folder (deleting or re-cloning it would lose them). Linux follows the XDG Base Directory spec, which says
 * to ignore an XDG_DATA_HOME that isn't an absolute path. savefs_init makes the folder when it's missing. NULL
 * when the environment names no home folder. */
static const char *default_save_dir(void) {
    static char buf[1024];
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return NULL;
    snprintf(buf, sizeof buf, "%s/YW4 Recomp/save", appdata);  /* as the launcher's "appdata" choice */
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/')
        snprintf(buf, sizeof buf, "%s/YW4Recomp/save", xdg);
    else if (home && *home)
        snprintf(buf, sizeof buf, "%s/.local/share/YW4Recomp/save", home);
    else
        return NULL;
#endif
    return buf;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) {  /* for the launcher and bug reports: one line on stdout */
        printf("YW4 Recomp %s (%s, built %s)\n", yw4_version, yw4_git_commit, yw4_build_date);
        return 0;
    }
    if (yw4_guest_memory_init()) {
        const char *retried = getenv("YW4_ASLR_RETRY");
        if (!retried || !*retried) {
            fprintf(stderr, "yw4: guest memory collision; retrying with a fresh ASLR layout...\n");
            host_respawn(argc, argv);
        }
        fprintf(stderr, "yw4: fatal: cannot reserve guest address space (0x7000000000 - 0x7800000000)\n");
        return 3;
    }
    /* No arguments (a double-click, a shortcut): the folders come from game.ini next to the exe (decision 0050) */
    static char *ini_args[4];
    int from_ini = 0, default_save = 0;
    if (argc == 1 && game_ini_load()) {
        const char *e = getenv("YW4_EXEFS"), *r = getenv("YW4_ROMFS"), *s = getenv("YW4_SAVE");
        if (!s || !*s) s = default_save_dir(), default_save = 1;
        if (e && r && s) {
            ini_args[0] = argv[0], ini_args[1] = (char *)e, ini_args[2] = (char *)r, ini_args[3] = (char *)s;
            argv = ini_args, argc = 4, from_ini = 1;
        }
    }
    if (argc == 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "YW4 Recomp",
                                 "The game isn't set up yet.\n\nStart the YW4 Recomp launcher: it sets up the game "
                                 "from your own copy and then starts it.", NULL);
        return 1;
    }
    static char *args3[4];
    if (argc == 3 && default_save_dir()) {  /* exefs and romfs only: the default save folder */
        args3[0] = argv[0], args3[1] = argv[1], args3[2] = argv[2], args3[3] = (char *)default_save_dir();
        argv = args3, argc = 4, default_save = 1;
    }
    static char paths[3][4096];
    for (int i = 1; i < argc && i <= 3; i++) absolute(&argv[i], paths[i - 1], sizeof paths[i - 1]);
    go_home();  /* before the log: its folder is a work/ path too */
    yw4_log_start();
    fprintf(stderr, "yw4: YW4 Recomp %s (commit %s, built %s)\n", yw4_version, yw4_git_commit, yw4_build_date);
    {
        char cwd[1024];
#ifdef _WIN32
        if (_getcwd(cwd, sizeof cwd)) fprintf(stderr, "yw4: home %s\n", cwd);
#else
        if (getcwd(cwd, sizeof cwd)) fprintf(stderr, "yw4: home %s\n", cwd);
#endif
    }
    if (from_ini) fprintf(stderr, "yw4: folders from game.ini\n");
    yw4_boot_mark("main() running, guest memory reserved, log open");
    settings_load();
    if (argc != 4) {
        fprintf(stderr, "usage: yw4 <exefs work directory with main.bin and sdk.bin> <romfs folder> [save data folder]\n");
        return 1;
    }
    fprintf(stderr, "yw4: saves in %s%s\n", argv[3], default_save ? " (the default save folder)" : "");
    window_prestart();
    if (yw4_load_modules(argv[1])) return 1;
    yw4_boot_mark("main and sdk loaded");
    if (romfs_open(argv[2])) return 1;
    yw4_boot_mark("RomFS and mods scanned");
    if (savefs_init(argv[3])) return 1;
    yw4_probe_init();
    yw4_watch_init();
    {
        void ai_trace_start(void);  /* ai_trace.c: YW4_TRACE_AI=1 only */
        ai_trace_start();
    }

    window_run(start_guest, NULL);
    return 0;
}
