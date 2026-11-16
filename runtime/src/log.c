/* Log file: everything the runtime prints on stderr is also saved in work/logs/yw4_<time>.log
 * (YW4_LOG=0 turns it off, YW4_LOG_DIR moves it), so a run started without a visible terminal
 * still leaves its messages. stderr is replaced by a pipe; a thread copies what comes through
 * to the original stderr and to the file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define PIPE(fds) _pipe(fds, 1 << 16, _O_BINARY)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define PIPE(fds) pipe(fds)
#endif

#include <stdint.h>

#include "host.h"

static int pipe_fds[2], saved_stderr;
static FILE *logfile;
static char log_name[600];

/* The log file being written ("" when there is none): the bug report copies it. */
const char *yw4_log_path(void) { return log_name; }

static void copy_thread(void *arg) {
    (void)arg;
    host_thread_set_priority(NULL, 60);  /* background disk logging: yield to gameplay and rendering */
    char buf[4096];
    for (;;) {
#ifdef _WIN32
        int n = _read(pipe_fds[0], buf, sizeof buf);
#else
        ssize_t n = read(pipe_fds[0], buf, sizeof buf);
#endif
        if (n <= 0) return;
#ifdef _WIN32
        _write(saved_stderr, buf, (unsigned)n);
#else
        if (write(saved_stderr, buf, (size_t)n) < 0) {}
#endif
        fwrite(buf, 1, (size_t)n, logfile);
        fflush(logfile);
    }
}

/* Start-up timing: where the time from launch to the first frame goes (grep "boot:" in the log). */
void yw4_boot_mark(const char *what) {
    fprintf(stderr, "yw4: boot: %6.0f ms: %s\n", (double)host_process_age_ns() / 1e6, what);
}

/* ---- Old runs' files: every start writes a new log (here) and new performance files (perf.c), and nothing removed
 * them, so a player's logs folder grew for good: about 3 MB a start on average, 17-30 MB an hour of play, nearly all
 * of it the per-frame perf_*.csv (owner, 2026-11-16: "cap the size to 1gb and keep only last 5"). Files in `dir`
 * named <prefix><YYYYmmdd_HHMMSS>... are one run's; the newest YW4_LOG_KEEP runs (default 5, the current one among
 * them) are kept, as long as they come to no more than YW4_LOG_CAP_MB (default 1024) together; the current run is
 * always kept. YW4_LOG_KEEP=0 keeps everything (the dev tools' runs: tools/dev/run.py). */
enum { STAMP = 15, MAX_FILES = 4096 };
typedef struct { char name[260], stamp[STAMP + 1]; uint64_t size; } RunFile;

static int by_stamp_newest(const void *a, const void *b) {
    return strcmp(((const RunFile *)b)->stamp, ((const RunFile *)a)->stamp);
}

static int run_file(const char *name, const char *prefix, char stamp[STAMP + 1]) {
    size_t p = strlen(prefix);
    if (strncmp(name, prefix, p) || strlen(name) < p + STAMP || strlen(name) >= 260) return 0;
    memcpy(stamp, name + p, STAMP), stamp[STAMP] = 0;
    for (int i = 0; i < STAMP; i++)  /* digits, an underscore at 8: not someone else's file that starts the same */
        if (i == 8 ? stamp[i] != '_' : (stamp[i] < '0' || stamp[i] > '9')) return 0;
    return 1;
}

void yw4_prune_runs(const char *dir, const char *prefix, const char *current) {
    const char *k = getenv("YW4_LOG_KEEP"), *c = getenv("YW4_LOG_CAP_MB");
    int keep = k && *k ? atoi(k) : 5;
    uint64_t cap = (uint64_t)(c && *c ? atoi(c) : 1024) << 20;
    if (keep <= 0) return;
    RunFile *f = malloc(sizeof *f * MAX_FILES);
    if (!f) return;
    int n = 0;
#ifdef _WIN32
    char pattern[600];
    snprintf(pattern, sizeof pattern, "%s\\%s*", dir, prefix);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < MAX_FILES &&
                run_file(fd.cFileName, prefix, f[n].stamp)) {
                snprintf(f[n].name, sizeof f[n].name, "%s", fd.cFileName);
                f[n++].size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
        struct stat st;
        if (n >= MAX_FILES || !run_file(e->d_name, prefix, f[n].stamp)) continue;
        if (fstatat(dirfd(d), e->d_name, &st, 0) || !S_ISREG(st.st_mode)) continue;
        snprintf(f[n].name, sizeof f[n].name, "%s", e->d_name);
        f[n++].size = (uint64_t)st.st_size;
    }
    if (d) closedir(d);
#endif
    qsort(f, (size_t)n, sizeof *f, by_stamp_newest);
    int runs = 0, removed = 0, full = 0;
    uint64_t kept = 0, freed = 0;
    for (int i = 0; i < n;) {  /* one run at a time, newest first */
        int j = i;
        uint64_t size = 0;
        while (j < n && !strcmp(f[j].stamp, f[i].stamp)) size += f[j++].size;
        int now = current && !strcmp(f[i].stamp, current);
        if (!now && (runs >= keep || kept + size > cap)) full = 1;  /* from here on, older runs all go */
        if (now || !full) {
            runs++, kept += size;
        } else {
            for (int m = i; m < j; m++) {
                char path[900];
                snprintf(path, sizeof path, "%s/%s", dir, f[m].name);
                if (remove(path) == 0) removed++, freed += f[m].size;
            }
        }
        i = j;
    }
    free(f);
    if (removed)
        fprintf(stderr, "yw4: log: removed %d old %s* file%s (%.1f MB) from %s; kept the newest %d run%s (%.1f MB)\n",
                removed, prefix, removed == 1 ? "" : "s", (double)freed / (1 << 20), dir, runs, runs == 1 ? "" : "s",
                (double)kept / (1 << 20));
}

/* At exit, give the copying thread a moment to drain the last messages (the stop message of a crash). */
static void drain(void) { host_sleep_ns(150 * 1000000LL); }

void yw4_log_start(void) {
    const char *on = getenv("YW4_LOG");
    if (on && *on == '0') return;
    const char *dir = getenv("YW4_LOG_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s", dir && *dir ? dir : "work/logs");
    for (char *p = path + 1; *p; p++)
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            MKDIR(path);
            *p = c;
        }
    MKDIR(path);
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char when[32], name[600];
    strftime(when, sizeof when, "%Y%m%d_%H%M%S", &tmv);
    snprintf(name, sizeof name, "%s/yw4_%s.log", path, when);
    logfile = fopen(name, "wb");
    if (!logfile) return;
    snprintf(log_name, sizeof log_name, "%s", name);
#ifdef _WIN32
    saved_stderr = _dup(2);
    if (PIPE(pipe_fds) != 0 || saved_stderr < 0 || _dup2(pipe_fds[1], 2) != 0) return;
#else
    saved_stderr = dup(2);
    if (PIPE(pipe_fds) != 0 || saved_stderr < 0 || dup2(pipe_fds[1], 2) < 0) return;
#endif
    setvbuf(stderr, NULL, _IONBF, 0);
    if (host_thread_start(copy_thread, NULL, 1 << 16) != 0) {
        /* Nobody reads the pipe: give stderr back, or every message is lost and the game blocks once the pipe is
         * full. (Linux, 2026-11-12: the thread's 64 KiB stack was refused, and a run that quit at start left an
         * empty terminal and an empty log.) */
#ifdef _WIN32
        _dup2(saved_stderr, 2);
#else
        dup2(saved_stderr, 2);
#endif
        fprintf(stderr, "yw4: log: cannot start the copying thread; no log file this run\n");
        return;
    }
    atexit(drain);
    fprintf(stderr, "yw4: log file %s\n", name);
    yw4_prune_runs(path, "yw4_", when);  /* after this run's file exists, so it is the newest */
}
