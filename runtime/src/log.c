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
#define MKDIR(p) _mkdir(p)
#define PIPE(fds) _pipe(fds, 1 << 16, _O_BINARY)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define PIPE(fds) pipe(fds)
#endif

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
}
