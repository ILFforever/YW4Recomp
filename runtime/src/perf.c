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
 * The GPU's own time comes from timestamp queries (gpu.c) and arrives a few frames late, so
 * a row is written PERF_DELAY frames after its frame. `gpu_busy_ms` adds up the durations of
 * the frame's GPU submissions; `gpu_span_ms` is from the first start to the last end (busy
 * plus the gaps where the GPU waited for work). Both are the work for the frame as the game
 * submitted it, on this host GPU, which is not the Switch's: they show how heavy the frame
 * is for the recompiled renderer, not what a Switch needs.
 *
 * Columns: frame, t_s, interval_ms, work_ms, present_ms, wait_ms, draws, cpu_pct (process CPU
 * time over the interval, 100 = one core), rss_mb, gpu_busy_ms, gpu_span_ms (empty: none). */
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

int gpu_frame_time(uint64_t frame, double *busy_ms, double *span_ms);  /* gpu.c */

#define PERF_DELAY 6

typedef struct {
    uint64_t frame, draws;
    double t, interval, work, present, wait, cpu, rss;
} Row;

static FILE *csv;
static char summary_path[600];
static int enabled = -1, exiting;
static int64_t t0, last_exit, last_cpu;
static uint64_t peak_rss;
static Row *rows;
static double *gpu_busy, *gpu_span;  /* per written row that had GPU times */
static size_t nrows, cap, written, ngpu;
static uint64_t last_summary_frame;
static double cpu_pct_sum;

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Percentile p (0..100) of v[from..n), sorted copy. */
static double percentile(const double *v, size_t from, size_t n, double p) {
    size_t m = n > from ? n - from : 0;
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
    size_t n = nrows;
    if (n <= from) return;
    double sum = 0, wsum = 0;
    size_t over34 = 0, over50 = 0;
    for (size_t i = from; i < n; i++) {
        sum += rows[i].interval, wsum += rows[i].work;
        over34 += rows[i].interval > 34.5, over50 += rows[i].interval > 50;
    }
    double frames = (double)(n - from);
    double *interval = malloc(n * sizeof *interval), *work = malloc(n * sizeof *work);
    if (!interval || !work) return;
    for (size_t i = 0; i < n; i++) interval[i] = rows[i].interval, work[i] = rows[i].work;
    fprintf(f, "%s: %zu frames, %.1f frames/s average\n", what, n - from, 1000.0 * frames / sum);
    fprintf(f, "  interval ms: p50 %.1f  p95 %.1f  p99 %.1f  max %.1f   (slower than 34.5 ms: %zu = %.1f%%, than 50 ms: %zu)\n",
            percentile(interval, from, n, 50), percentile(interval, from, n, 95), percentile(interval, from, n, 99),
            percentile(interval, from, n, 100), over34, 100.0 * (double)over34 / frames, over50);
    fprintf(f, "  game work ms: avg %.1f  p50 %.1f  p95 %.1f  p99 %.1f  max %.1f   (budget at 30 frames/s: 33.3)\n", wsum / frames,
            percentile(work, from, n, 50), percentile(work, from, n, 95), percentile(work, from, n, 99),
            percentile(work, from, n, 100));
    free(interval);
    free(work);
    size_t g0 = ngpu > (n - from) ? ngpu - (n - from) : 0;  /* the same stretch, roughly */
    if (ngpu > g0) {
        double bsum = 0, ssum = 0;
        for (size_t i = g0; i < ngpu; i++) bsum += gpu_busy[i], ssum += gpu_span[i];
        double cnt = (double)(ngpu - g0);
        fprintf(f, "  GPU busy ms: avg %.1f  p50 %.1f  p95 %.1f  p99 %.1f  max %.1f   (span, with gaps: avg %.1f  p95 %.1f)\n",
                bsum / cnt, percentile(gpu_busy, g0, ngpu, 50), percentile(gpu_busy, g0, ngpu, 95),
                percentile(gpu_busy, g0, ngpu, 99), percentile(gpu_busy, g0, ngpu, 100), ssum / cnt,
                percentile(gpu_span, g0, ngpu, 95));
    } else {
        fprintf(f, "  GPU time: not available\n");
    }
    fprintf(f, "  process CPU: %.0f%% of one core on average, memory peak %.0f MB\n", cpu_pct_sum / (double)n,
            (double)peak_rss / 1048576.0);
}

/* Writes the rows up to (but not including) index `upto`, with their GPU times. */
static void write_rows(size_t upto) {
    for (; written < upto; written++) {
        Row *r = &rows[written];
        double busy, span;
        /* At exit the GPU lock may be held by the thread that is exiting (a stop inside a draw):
         * asking for GPU times then would deadlock, so the last rows go without them. */
        int has = exiting ? 0 : gpu_frame_time(r->frame, &busy, &span);
        fprintf(csv, "%" PRIu64 ",%.3f,%.2f,%.2f,%.2f,%.2f,%" PRIu64 ",%.0f,%.0f,", r->frame, r->t, r->interval, r->work,
                r->present, r->wait, r->draws, r->cpu, r->rss);
        if (has) {
            fprintf(csv, "%.2f,%.2f\n", busy, span);
            gpu_busy[ngpu] = busy, gpu_span[ngpu] = span, ngpu++;
        } else {
            fprintf(csv, ",\n");
        }
    }
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
    exiting = 1;
    write_rows(nrows);
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
    fprintf(csv, "frame,t_s,interval_ms,work_ms,present_ms,wait_ms,draws,cpu_pct,rss_mb,gpu_busy_ms,gpu_span_ms\n");
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
        last_exit = exit_;
        return;
    }
    int64_t cpu;
    uint64_t rss;
    host_process_usage(&cpu, &rss);
    double cpu_pct = exit_ > last_exit ? 100.0 * (double)(cpu - last_cpu) / (double)(exit_ - last_exit) : 0;
    if (nrows == cap) {
        cap = cap ? cap * 2 : 4096;
        rows = realloc(rows, cap * sizeof *rows);
        gpu_busy = realloc(gpu_busy, cap * sizeof *gpu_busy);
        gpu_span = realloc(gpu_span, cap * sizeof *gpu_span);
        if (!rows || !gpu_busy || !gpu_span) exit(3);
    }
    rows[nrows++] = (Row){.frame = frame, .draws = draws, .t = (double)(exit_ - t0) / 1e9,
                          .interval = (double)(exit_ - last_exit) / 1e6, .work = (double)(enter - last_exit) / 1e6,
                          .present = (double)(shown - enter) / 1e6, .wait = (double)(exit_ - shown) / 1e6,
                          .cpu = cpu_pct, .rss = (double)rss / 1048576.0};
    last_cpu = cpu, last_exit = exit_;
    if (rss > peak_rss) peak_rss = rss;
    cpu_pct_sum += cpu_pct;
    if (nrows > PERF_DELAY) write_rows(nrows - PERF_DELAY);
    if (nrows % 60 == 0) fflush(csv);
    if (frame - last_summary_frame >= 600) {
        print_summary(stderr, nrows > 600 ? nrows - 600 : 0, "yw4: perf, last 600 frames");
        write_summary();  /* so a killed or crashed run leaves its numbers */
        last_summary_frame = frame;
    }
}
