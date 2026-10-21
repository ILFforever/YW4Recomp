/* Performance log (YW4_PERF=0 turns it off, YW4_PERF_DIR sets the folder, default work/perf).
 *
 * One row per presented frame in perf_<time>.csv, and a summary in perf_<time>.txt (also on
 * stderr every 600 frames and at exit). The game paces itself on the display's vsync, so the
 * frame interval mostly shows the cap (30 frames/s = 33.3 ms). What says how much headroom
 * there is is `work_ms`: the time from the end of the last present to the start of this one,
 * i.e. everything the game's thread did for the frame (game logic, recompiled code, command
 * recording, waits on the GPU). `present_ms` is the host's share of the present (copying or
 * handing the frame to the window), before the wait for the flip.
 *
 * Columns: frame, t_s, interval_ms, work_ms, present_ms, wait_ms, draws, cpu_pct (process CPU
 * time over the interval, 100 = one core), rss_mb. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "host.h"

static FILE *csv;
static char summary_path[600];
static int enabled = -1;
static int64_t t0, last_exit, last_enter, last_cpu;
static uint64_t peak_rss;
static double *interval_ms, *work_ms;
static size_t count, cap;
static uint64_t last_summary_frame;
static double cpu_pct_sum;

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Percentile p (0..100) of v[from..n), sorted copy. */
static double percentile(const double *v, size_t from, size_t n, double p) {
    size_t m = n - from;
    if (!m) return 0;
    double *c = malloc(m * sizeof *c);
    if (!c) return 0;
    memcpy(c, v + from, m * sizeof *c);
    qsort(c, m, sizeof *c, cmp_double);
    double r = c[(size_t)((double)(m - 1) * p / 100.0 + 0.5)];
    free(c);
    return r;
}

static void print_summary(FILE *f, size_t from, const char *what) {
    size_t n = count;
    if (n <= from) return;
    double sum = 0, wsum = 0;
    size_t over34 = 0, over50 = 0;
    for (size_t i = from; i < n; i++) {
        sum += interval_ms[i], wsum += work_ms[i];
        over34 += interval_ms[i] > 34.5, over50 += interval_ms[i] > 50;
    }
    double frames = (double)(n - from);
    fprintf(f, "%s: %zu frames, %.1f frames/s average\n", what, n - from, 1000.0 * frames / sum);
    fprintf(f, "  interval ms: p50 %.1f  p95 %.1f  p99 %.1f  max %.1f   (slower than 34.5 ms: %zu = %.1f%%, than 50 ms: %zu)\n",
            percentile(interval_ms, from, n, 50), percentile(interval_ms, from, n, 95),
            percentile(interval_ms, from, n, 99), percentile(interval_ms, from, n, 100), over34,
            100.0 * (double)over34 / frames, over50);
    fprintf(f, "  game work ms: avg %.1f  p50 %.1f  p95 %.1f  p99 %.1f  max %.1f   (budget at 30 frames/s: 33.3)\n", wsum / frames,
            percentile(work_ms, from, n, 50), percentile(work_ms, from, n, 95), percentile(work_ms, from, n, 99),
            percentile(work_ms, from, n, 100));
    fprintf(f, "  process CPU: %.0f%% of one core on average, memory peak %.0f MB\n", cpu_pct_sum / (double)n,
            (double)peak_rss / 1048576.0);
}

static void write_summary(void) {
    FILE *f = fopen(summary_path, "w");
    if (f) {
        print_summary(f, 0, "whole run");
        fclose(f);
    }
}

static void finish(void) {
    if (!csv) return;
    fclose(csv);
    csv = NULL;
    write_summary();
    print_summary(stderr, 0, "yw4: perf, whole run");
}

static void start(void) {
    enabled = 0;
    const char *on = getenv("YW4_PERF");
    if (on && *on == '0') return;
    const char *dir = getenv("YW4_PERF_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s", dir && *dir ? dir : "work/perf");
    for (char *p = path + 1; *p; p++)  /* make every folder on the way */
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
    char when[32], csv_path[600];
    strftime(when, sizeof when, "%Y%m%d_%H%M%S", &tmv);
    snprintf(csv_path, sizeof csv_path, "%s/perf_%s.csv", path, when);
    snprintf(summary_path, sizeof summary_path, "%s/perf_%s.txt", path, when);
    csv = fopen(csv_path, "w");
    if (!csv) return;
    fprintf(csv, "frame,t_s,interval_ms,work_ms,present_ms,wait_ms,draws,cpu_pct,rss_mb\n");
    fprintf(stderr, "yw4: performance log %s\n", csv_path);
    uint64_t rss;
    t0 = host_now_ns();
    host_process_usage(&last_cpu, &rss);
    atexit(finish);
    enabled = 1;
}

/* Called by nvnQueuePresentTexture: enter = on entry, shown = after the host's part of the
 * present, exit = after the wait for the flip. */
void yw4_perf_frame(uint64_t frame, int64_t enter, int64_t shown, int64_t exit_, uint64_t draws) {
    if (enabled < 0) start();
    if (enabled <= 0) return;
    if (!last_exit) {  /* the first present: nothing to compare with yet */
        last_exit = exit_, last_enter = enter;
        return;
    }
    double interval = (double)(exit_ - last_exit) / 1e6;
    double work = (double)(enter - last_exit) / 1e6;
    int64_t cpu;
    uint64_t rss;
    host_process_usage(&cpu, &rss);
    double cpu_pct = exit_ > last_exit ? 100.0 * (double)(cpu - last_cpu) / (double)(exit_ - last_exit) : 0;
    last_cpu = cpu, last_exit = exit_, last_enter = enter;
    if (rss > peak_rss) peak_rss = rss;
    if (count == cap) {
        cap = cap ? cap * 2 : 4096;
        interval_ms = realloc(interval_ms, cap * sizeof *interval_ms);
        work_ms = realloc(work_ms, cap * sizeof *work_ms);
        if (!interval_ms || !work_ms) exit(3);
    }
    interval_ms[count] = interval, work_ms[count] = work;
    cpu_pct_sum += cpu_pct;
    count++;
    fprintf(csv, "%" PRIu64 ",%.3f,%.2f,%.2f,%.2f,%.2f,%" PRIu64 ",%.0f,%.0f\n", frame, (double)(exit_ - t0) / 1e9, interval,
            work, (double)(shown - enter) / 1e6, (double)(exit_ - shown) / 1e6, draws, cpu_pct, (double)rss / 1048576.0);
    if (count % 60 == 0) fflush(csv);
    if (frame - last_summary_frame >= 600) {
        print_summary(stderr, count > 600 ? count - 600 : 0, "yw4: perf, last 600 frames");
        write_summary();  /* so a killed or crashed run leaves its numbers */
        last_summary_frame = frame;
    }
}
