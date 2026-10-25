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
 * time over the interval, 100 = one core), rss_mb, gpu_busy_ms, gpu_span_ms (empty: none),
 * submit_ms / submit_other_ms (time in nvnQueueSubmitCommands, i.e. the host's translation of
 * the command lists, on the presenting thread / on other threads), sync_ms (nvnSyncWait on
 * the presenting thread). submit_ms and sync_ms are part of work_ms; the rest of work_ms is
 * the game's own code. */
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
#include "hud.h"
#include "kernel.h"
#include "nvn.h"

int gpu_frame_time(uint64_t frame, double *busy_ms, double *span_ms);  /* gpu.c */

#define PERF_DELAY 6

typedef struct {
    uint64_t frame, draws;
    double t, interval, work, present, wait, cpu, rss, submit, submit_other, sync;
} Row;

_Thread_local int yw4_perf_presenter;
static int64_t acc_submit, acc_submit_other, acc_sync;  /* ns since the last present */

void yw4_perf_add(int what, int64_t ns) {
    int64_t *a = what == PERF_SYNC ? (yw4_perf_presenter ? &acc_sync : NULL)
                                   : yw4_perf_presenter ? &acc_submit : &acc_submit_other;
    if (a) __atomic_add_fetch(a, ns, __ATOMIC_RELAXED);
}

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
    double sub = 0, sync = 0, other = 0;
    for (size_t i = from; i < n; i++) sub += rows[i].submit, sync += rows[i].sync, other += rows[i].submit_other;
    fprintf(f, "  of the game work: submit (host translation) avg %.1f, GPU waits avg %.1f, game code avg %.1f; submit on other threads avg %.1f\n",
            sub / frames, sync / frames, (wsum - sub - sync) / frames, other / frames);
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
            fprintf(csv, "%.2f,%.2f,", busy, span);
            gpu_busy[ngpu] = busy, gpu_span[ngpu] = span, ngpu++;
        } else {
            fprintf(csv, ",,");
        }
        fprintf(csv, "%.2f,%.2f,%.2f\n", r->submit, r->submit_other, r->sync);
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
    fprintf(csv, "frame,t_s,interval_ms,work_ms,present_ms,wait_ms,draws,cpu_pct,rss_mb,gpu_busy_ms,gpu_span_ms,submit_ms,submit_other_ms,sync_ms\n");
    fprintf(stderr, "yw4: performance log %s\n", csv_path);
    uint64_t rss;
    t0 = host_now_ns();
    host_process_usage(&last_cpu, &rss);
    atexit(finish);
    enabled = 1;
}

/* The HUD (hud.h): averages over the last 30 frames; lows, min and max over the last 600 (20 s);
 * the graph is the last HUD_POINTS frames. */
void yw4_perf_hud(HudContent *c) {
    c->nlines = c->npoints = 0;
    c->target_ms = 1000.0f / 30.0f;
#define LINE(...) (c->nlines < HUD_LINES ? snprintf(c->lines[c->nlines++], HUD_COLS, __VA_ARGS__) : 0)
    if (enabled <= 0 || !nrows) {
        LINE(enabled == 0 ? "PERF OFF (YW4_PERF=0)" : "PERF: NO FRAMES YET");
    } else {
        size_t k = nrows < 30 ? nrows : 30, from = nrows - k;
        double interval = 0, work = 0, cpu = 0, draws = 0;
        for (size_t i = from; i < nrows; i++)
            interval += rows[i].interval, work += rows[i].work, cpu += rows[i].cpu, draws += (double)rows[i].draws;
        interval /= (double)k, work /= (double)k, cpu /= (double)k, draws /= (double)k;
        /* 1% / 0.1% low: the frame rate of the 99th / 99.9th percentile frame time */
        static double recent[600];
        size_t m = nrows < 600 ? nrows : 600;
        double lo = 1e9, hi = 0;
        for (size_t i = 0; i < m; i++) {
            recent[i] = rows[nrows - m + i].interval;
            if (recent[i] < lo) lo = recent[i];
            if (recent[i] > hi) hi = recent[i];
        }
        double p99 = percentile(recent, 0, m, 99), p999 = percentile(recent, 0, m, 99.9);
        LINE("FPS %4.1f  1%% %4.1f  0.1%% %4.1f", interval > 0 ? 1000.0 / interval : 0.0, p99 > 0 ? 1000.0 / p99 : 0.0,
             p999 > 0 ? 1000.0 / p999 : 0.0);
        LINE("FRAME %4.1f  MIN %4.1f  MAX %5.1f MS", interval, lo, hi);
        size_t g = ngpu < 30 ? ngpu : 30;  /* GPU times arrive PERF_DELAY frames late */
        double busy = 0;
        for (size_t i = ngpu - g; i < ngpu; i++) busy += gpu_busy[i];
        if (g) LINE("WORK %4.1f  GPU %4.1f MS", work, busy / (double)g);
        else LINE("WORK %4.1f  GPU - MS", work);
        /* cpu is 100 per busy core: shown as the share of the whole CPU (as task managers do) and in cores */
        static int ncpu;
        if (!ncpu) ncpu = host_cpu_count();
        LINE("CPU %.0f%% (%.1f OF %d CORES)", cpu / ncpu, cpu / 100.0, ncpu);
        LINE("DRAWS %.0f  RAM %.0f MB", draws, rows[nrows - 1].rss);
        size_t p = nrows < HUD_POINTS ? nrows : HUD_POINTS;
        for (size_t i = 0; i < p; i++) {
            c->frame_ms[i] = (float)rows[nrows - p + i].interval;
            c->work_ms[i] = (float)rows[nrows - p + i].work;
        }
        c->npoints = (int)p;
    }
    LINE("SHADOWS: %s  %d", shadow_mode_name(), gpu_shadow_res());
    static const char *const soft[4] = {"OFF", "X1", "X2", "X4"};
    LINE("PCSS: %s (F7)", soft[gpu_pcss_level() & 3]);
#undef LINE
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
                          .cpu = cpu_pct, .rss = (double)rss / 1048576.0,
                          .submit = (double)__atomic_exchange_n(&acc_submit, 0, __ATOMIC_RELAXED) / 1e6,
                          .submit_other = (double)__atomic_exchange_n(&acc_submit_other, 0, __ATOMIC_RELAXED) / 1e6,
                          .sync = (double)__atomic_exchange_n(&acc_sync, 0, __ATOMIC_RELAXED) / 1e6};
    last_cpu = cpu, last_exit = exit_;
    if (rss > peak_rss) peak_rss = rss;
    cpu_pct_sum += cpu_pct;
    if (nrows > PERF_DELAY) write_rows(nrows - PERF_DELAY);
    if (nrows % 60 == 0) fflush(csv);
    if (frame - last_summary_frame >= 600) {
        print_summary(stderr, nrows > 600 ? nrows - 600 : 0, "yw4: perf, last 600 frames");
        static int64_t last_threads;  /* host time of the last per-thread report */
        if (last_threads) fprintf(stderr, "  threads, last 600 frames:\n");
        yw4_thread_cpu_report(stderr, last_threads ? exit_ - last_threads : 0);  /* 0: only takes the start values */
        last_threads = exit_;
        write_summary();  /* so a killed or crashed run leaves its numbers */
        last_summary_frame = frame;
    }
}
