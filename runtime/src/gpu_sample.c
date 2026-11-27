/* What the GPU thread is doing, sampled from another thread: see gpu_sample.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gpu_sample.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

int gpu_phase;

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

extern char **environ;

#if defined(__x86_64__)
#define IOCTL_NR 16
#elif defined(__aarch64__)
#define IOCTL_NR 29
#else
#define IOCTL_NR -100
#endif

static const char *const phase_names[GPU_PHASES] = {"other", "idle", "recording", "end", "vkQueueSubmit", "present",
                                                    "window present"};

/* one combination seen: the part, the thread's state, the system call (-2 running, -1 none) and an ioctl's request */
typedef struct { uint8_t phase; char state; int nr; uint64_t cmd; long n; } Bucket;
enum { BUCKETS = 256 };
static Bucket table[BUCKETS];
static long nsamples, nlost, phase_n[GPU_PHASES];
static HostLock table_lock = HOST_LOCK_INIT;
static int tid;          /* the GPU thread's Linux thread id */
static int started;

static void add(int phase, char state, int nr, uint64_t cmd) {
    uint64_t h = ((uint64_t)phase * 31 + (uint64_t)(unsigned char)state) * 1000003u + (uint64_t)(nr + 2) * 7919u + cmd;
    host_lock(&table_lock);
    nsamples++;
    phase_n[phase]++;
    for (unsigned i = 0; i < BUCKETS; i++) {
        Bucket *b = &table[(h + i) % BUCKETS];
        if (!b->n) *b = (Bucket){(uint8_t)phase, state, nr, cmd, 0};
        if (b->phase == phase && b->state == state && b->nr == nr && b->cmd == cmd) {
            b->n++;
            host_unlock(&table_lock);
            return;
        }
    }
    nlost++;
    host_unlock(&table_lock);
}

static int read_at0(int fd, char *buf, size_t n) {
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    if (r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}

static void sampler(void *arg) {
    (void)arg;
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    int fstat_ = open(path, O_RDONLY | O_CLOEXEC);
    snprintf(path, sizeof path, "/proc/self/task/%d/syscall", tid);
    int fsys = open(path, O_RDONLY | O_CLOEXEC);
    if (fstat_ < 0) {
        fprintf(stderr, "yw4: gpu thread: sampler can't read the GPU thread's state: off\n");
        return;
    }
    if (fsys < 0) fprintf(stderr, "yw4: gpu thread: sampler can't read the GPU thread's system calls: state only\n");
    for (;;) {
        int phase = __atomic_load_n(&gpu_phase, __ATOMIC_RELAXED);
        char state = '?';
        if (read_at0(fstat_, buf, sizeof buf) > 0) {
            const char *p = strrchr(buf, ')');  /* the name may hold spaces and brackets: the state follows the last ) */
            if (p && p[1] == ' ' && p[2]) state = p[2];
        }
        int nr = -3;
        uint64_t cmd = 0;
        if (fsys >= 0 && read_at0(fsys, buf, sizeof buf) > 0) {
            if (!strncmp(buf, "running", 7)) nr = -2;
            else {
                char *e;
                nr = (int)strtol(buf, &e, 10);
                if (nr == IOCTL_NR) {  /* "16 0x<fd> 0x<request> ...": the request says which driver call */
                    strtoull(e, &e, 16);
                    cmd = strtoull(e, &e, 16);
                }
            }
        }
        if (phase < 0 || phase >= GPU_PHASES) phase = GPU_PHASE_OTHER;
        add(phase, state, nr, cmd);
        host_sleep_ns(1000000);
    }
}

/* the driver call an ioctl request names: DRM's ('d') amdgpu ones and the generic sync-object waits */
static const char *ioctl_name(uint64_t cmd) {
    if (((cmd >> 8) & 0xff) != 'd') return NULL;
    unsigned c = (unsigned)(cmd & 0xff);
    static const char *const amd[] = {"amdgpu GEM_CREATE", "amdgpu GEM_MMAP", "amdgpu CTX", "amdgpu BO_LIST",
                                      "amdgpu CS (submit)", "amdgpu INFO", "amdgpu GEM_METADATA",
                                      "amdgpu GEM_WAIT_IDLE", "amdgpu GEM_VA", "amdgpu WAIT_CS"};
    if (c >= 0x40 && c < 0x40 + sizeof amd / sizeof *amd) return amd[c - 0x40];
    switch (c) {
    case 0x50: return "amdgpu GEM_OP";
    case 0x51: return "amdgpu GEM_USERPTR";
    case 0x52: return "amdgpu WAIT_FENCES";
    case 0x53: return "amdgpu VM";
    case 0x54: return "amdgpu FENCE_TO_HANDLE";
    case 0x55: return "amdgpu SCHED";
    case 0xc3: return "drm SYNCOBJ_WAIT";
    case 0xca: return "drm SYNCOBJ_TIMELINE_WAIT";
    case 0xcb: return "drm SYNCOBJ_QUERY";
    }
    return NULL;
}

static void log_environment(void) {
    static const char *const pre[] = {"RADV_", "MESA_", "AMD_", "ACO_", "VK_", "LD_PRELOAD", "ENABLE_", "MANGOHUD",
                                      "OBS_", "DXVK_", "PROTON_", "SteamGame", "SDL_VIDEODRIVER", "DISABLE_"};
    char line[2000];
    size_t len = 0;
    line[0] = 0;
    for (char **e = environ; e && *e; e++)
        for (size_t k = 0; k < sizeof pre / sizeof *pre; k++)
            if (!strncmp(*e, pre[k], strlen(pre[k])) && len + strlen(*e) + 4 < sizeof line) {
                len += (size_t)snprintf(line + len, sizeof line - len, "%s%s", len ? "  " : "", *e);
                break;
            }
    fprintf(stderr, "yw4: gpu thread: driver environment: %s\n", len ? line : "none set");
}

void gpu_sample_start(void) {
    if (started) return;
    started = 1;
    const char *v = getenv("YW4_GPU_SAMPLE");
    log_environment();
    if (v && *v == '0') return;
    tid = (int)syscall(SYS_gettid);
    if (host_thread_start(sampler, NULL, 64 << 10)) fprintf(stderr, "yw4: gpu thread: can't start the sampler\n");
}

/* the DRM driver's per-client numbers (kernel docs: drm-usage-stats): memory by place, evictions, engine time */
static void report_fdinfo(void) {
    DIR *d = opendir("/proc/self/fdinfo");
    if (!d) return;
    char seen[8][32];
    int nseen = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[300], buf[4096];
        snprintf(path, sizeof path, "/proc/self/fdinfo/%s", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        if (!strstr(buf, "drm-driver:")) continue;
        char id[32] = "", out[1500];
        size_t len = 0;
        out[0] = 0;
        for (char *ln = strtok(buf, "\n"); ln; ln = strtok(NULL, "\n")) {
            if (strncmp(ln, "drm-", 4) && strncmp(ln, "amd-", 4)) continue;
            if (!strncmp(ln, "drm-pdev", 8)) continue;
            char *colon = strchr(ln, ':');
            if (!colon) continue;
            *colon = 0;
            char *val = colon + 1;
            while (*val == ' ' || *val == '\t') val++;
            if (!strcmp(ln, "drm-client-id")) { snprintf(id, sizeof id, "%s", val); continue; }
            if (len + strlen(ln) + strlen(val) + 4 < sizeof out)
                len += (size_t)snprintf(out + len, sizeof out - len, "%s%s=%s", len ? ", " : "", ln + 4, val);
        }
        int dup = 0;
        for (int i = 0; i < nseen; i++) dup |= !strcmp(seen[i], id);
        if (dup) continue;
        if (nseen < 8) snprintf(seen[nseen++], sizeof seen[0], "%s", id);
        fprintf(stderr, "yw4: gpu thread: drm client %s: %s\n", id[0] ? id : "?", out);
    }
    closedir(d);
}

void gpu_sample_report(uint64_t frame) {
    if (!started || frame % 600) return;
    report_fdinfo();
    if (!tid) return;
    Bucket t[BUCKETS];
    long total, lost, ph[GPU_PHASES];
    host_lock(&table_lock);
    memcpy(t, table, sizeof t);
    memset(table, 0, sizeof table);
    total = nsamples, lost = nlost;
    memcpy(ph, phase_n, sizeof ph);
    nsamples = nlost = 0;
    memset(phase_n, 0, sizeof phase_n);
    host_unlock(&table_lock);
    if (!total) return;
    char line[1200];
    size_t len = 0;
    for (int p = 0; p < GPU_PHASES; p++)
        if (ph[p]) len += (size_t)snprintf(line + len, sizeof line - len, "%s%s %.1f%%", len ? ", " : "", phase_names[p],
                                           ph[p] * 100.0 / total);
    fprintf(stderr, "yw4: gpu thread: samples frames %" PRIu64 "-%" PRIu64 ": %ld (%ld unsorted); parts: %s\n",
            frame - 600, frame, total, lost, line);
    /* the most common combinations, biggest first (state R = on a CPU, in our code or the kernel's; S = sleeping;
     * D = waiting inside the kernel) */
    len = 0;
    line[0] = 0;
    for (int shown = 0; shown < 12; shown++) {
        int best = -1;
        for (int i = 0; i < BUCKETS; i++)
            if (t[i].n && (best < 0 || t[i].n > t[best].n)) best = i;
        if (best < 0 || t[best].n * 1000 < total) break;  /* under 0.1%: left out */
        Bucket *b = &t[best];
        char call[80];
        if (b->nr == -2) snprintf(call, sizeof call, "running");
        else if (b->nr == -1) snprintf(call, sizeof call, "no system call (page fault or preempted)");
        else if (b->nr == -3) snprintf(call, sizeof call, "?");
        else if (b->nr == IOCTL_NR) {
            const char *nm = ioctl_name(b->cmd);
            snprintf(call, sizeof call, "ioctl %#" PRIx64 "%s%s%s", b->cmd, nm ? " (" : "", nm ? nm : "", nm ? ")" : "");
        } else {
            const char *nm = NULL;
#if defined(__x86_64__)
            switch (b->nr) {  /* the ones a GPU thread waits in */
            case 0: nm = "read"; break;
            case 1: nm = "write"; break;
            case 7: nm = "poll"; break;
            case 9: nm = "mmap"; break;
            case 10: nm = "mprotect"; break;
            case 11: nm = "munmap"; break;
            case 24: nm = "sched_yield"; break;
            case 202: nm = "futex (a lock or condition wait)"; break;
            case 230: nm = "clock_nanosleep"; break;
            case 232: nm = "epoll_wait"; break;
            case 271: nm = "ppoll"; break;
            case 281: nm = "epoll_pwait"; break;
            }
#endif
            snprintf(call, sizeof call, "system call %d%s%s%s", b->nr, nm ? " (" : "", nm ? nm : "", nm ? ")" : "");
        }
        if (len + 120 < sizeof line)
            len += (size_t)snprintf(line + len, sizeof line - len, "%s%s %c %s %.1f%%", len ? "; " : "",
                                    phase_names[b->phase], b->state, call, b->n * 100.0 / total);
        b->n = 0;
    }
    fprintf(stderr, "yw4: gpu thread: most common (part, state, call): %s\n", line);
}

#else  /* not Linux: nothing to sample */

void gpu_sample_start(void) {}
void gpu_sample_report(uint64_t frame) { (void)frame; }

#endif
