/* The pipeline cache on disk: every pipeline the driver compiles is kept between runs.
 *
 * Without one, every graphics pipeline was compiled from nothing in every run, on the GPU thread, the first
 * time a draw needed it: 40-300 ms each on a Radeon RX 6650 XT (2026-11-08), 117 of them (2.9 s) when the town
 * first appears, and a hitch whenever a new effect, character or menu showed up. With the cache a pipeline
 * built once before comes back from the driver in about a millisecond. All pipeline builds pass gpu_pcache().
 *
 * Saved by a thread every 10 s when pipelines were built since the last save, and at exit; written to a
 * temporary file and renamed, so a crash never leaves half a file. YW4_PIPELINE_CACHE=<file> moves it
 * (default work/pipelines.cache), =none turns it off.
 *
 * When the files are thrown away (decision 0063). The cache and the pipeline journal (gpu_draw.c) are only good
 * for the GPU, the driver and the translated shaders they were made with, so a third file beside the cache,
 * <cache>.stamp, says which those were:
 *   - the GPU and driver: vendorID, deviceID and pipelineCacheUUID of VkPhysicalDeviceProperties;
 *   - the shaders: a sum over the names, sizes and modification times of the *.spv and *.iface files in the two
 *     shader folders (the game's translated ones and our own). One directory listing each, no file is read: the
 *     shader build writes thousands of files and hashing their contents at every start would cost more than the
 *     cache saves on a warm run. A rebuilt shader has a new time even when its bytes are the same; then the files
 *     go although they were still good, which costs one run of first-time builds.
 * Another GPU or driver, or no stamp: the cache and the journal are deleted before either is read. Only the
 * shaders changed: both are kept and the journal's pipelines are built again at start (decision 0070). The cache file's
 * own header (Vulkan's VkPipelineCacheHeaderVersionOne: the same three values) is checked as well before it is
 * handed to the driver, so a damaged or foreign file is never loaded; the specification asks applications to do
 * this themselves and not to rely on the driver. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef GPTR  /* windows.h's; hle.h (through gpu_internal.h) has its own */
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "gpu_internal.h"
#include "host.h"

static VkPipelineCache cache;
static char path[1024];
static char journal_path[1024];  /* "" off */
static uint64_t builds, saved_builds;
static HostLock save_lock;

VkPipelineCache gpu_pcache(void) { return cache; }

void gpu_pcache_built(void) { __atomic_add_fetch(&builds, 1, __ATOMIC_RELAXED); }

static void save(void) {
    if (!cache) return;
    host_lock(&save_lock);
    uint64_t now = __atomic_load_n(&builds, __ATOMIC_RELAXED);
    if (now != saved_builds) {
        size_t n = 0;
        void *data = NULL;
        if (vkGetPipelineCacheData(vk.device, cache, &n, NULL) == VK_SUCCESS && n && (data = malloc(n)) &&
            vkGetPipelineCacheData(vk.device, cache, &n, data) == VK_SUCCESS) {
            char tmp[1040];
            snprintf(tmp, sizeof tmp, "%s.tmp", path);
            FILE *f = fopen(tmp, "wb");
            if (f && fwrite(data, 1, n, f) == n && fclose(f) == 0) {
                remove(path);
                if (rename(tmp, path) == 0) saved_builds = now;
            } else if (f) {
                fclose(f);
            }
        }
        free(data);
    }
    host_unlock(&save_lock);
}

static void saver(void *arg) {
    (void)arg;
    for (;;) {
        host_sleep_ns(10000000000ll);
        save();
    }
}

static void at_exit(void) { save(); }

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static int shader_name(const char *name) {
    size_t n = strlen(name);
    return (n > 4 && !strcmp(name + n - 4, ".spv")) || (n > 6 && !strcmp(name + n - 6, ".iface"));
}

/* One number for the shader files in `dir`: each file's name, size and modification time hashed, the hashes
 * added up (so the order the system lists them in doesn't matter). 0 for no folder or no shaders. */
static uint64_t folder_stamp(const char *dir) {
    uint64_t sum = 0;
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;  /* the listing carries size and time: no call per file */
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !shader_name(fd.cFileName)) continue;
        uint64_t v[2] = {(uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow,
                         (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime};
        sum += fnv(fnv(1469598103934665603ull, fd.cFileName, strlen(fd.cFileName)), v, sizeof v);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (!shader_name(e->d_name)) continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, 0) || !S_ISREG(st.st_mode)) continue;
        uint64_t v[2] = {(uint64_t)st.st_size, (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec};
        sum += fnv(fnv(1469598103934665603ull, e->d_name, strlen(e->d_name)), v, sizeof v);
    }
    closedir(d);
#endif
    return sum;
}

/* What the cache and the journal were made with, as one line of text: the GPU, its driver, the shaders. */
static void stamp_now(char *out, size_t size, const VkPhysicalDeviceProperties *gp) {
    const char *game = getenv("YW4_SHADER_DIR"), *ours = getenv("YW4_NATIVE_SHADER_DIR");
    uint64_t shaders = folder_stamp(game && *game ? game : "work/shaders/glsl") * 31 +
                       folder_stamp(ours && *ours ? ours : "work/build/shaders");
    int n = snprintf(out, size, "YW4PIPES1 gpu %08x:%08x:", (unsigned)gp->vendorID, (unsigned)gp->deviceID);
    for (int i = 0; i < (int)VK_UUID_SIZE && (size_t)n < size; i++)
        n += snprintf(out + n, size - (size_t)n, "%02x", gp->pipelineCacheUUID[i]);
    if ((size_t)n < size) snprintf(out + n, size - (size_t)n, " shaders %016" PRIx64, shaders);
}

/* Vulkan's header of a pipeline cache's data (VkPipelineCacheHeaderVersionOne): for this GPU and driver? */
static int header_ok(const void *data, size_t n, const VkPhysicalDeviceProperties *gp) {
    uint32_t h[4];  /* headerSize, headerVersion, vendorID, deviceID; then pipelineCacheUUID */
    if (n < 16 + VK_UUID_SIZE) return 0;
    memcpy(h, data, sizeof h);
    return h[0] == 16 + VK_UUID_SIZE && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == gp->vendorID &&
           h[3] == gp->deviceID && !memcmp((const char *)data + 16, gp->pipelineCacheUUID, VK_UUID_SIZE);
}

const char *gpu_pcache_journal_path(void) { return *journal_path ? journal_path : NULL; }

void gpu_pcache_init(void) {
    const char *e = getenv("YW4_PIPELINE_CACHE"), *j = getenv("YW4_PIPELINE_JOURNAL");
    int cache_on = !(e && !strcmp(e, "none")), journal_on = !(j && !strcmp(j, "none"));
    snprintf(path, sizeof path, "%s", cache_on && e && *e ? e : "work/pipelines.cache");
    if (journal_on) snprintf(journal_path, sizeof journal_path, "%s", j && *j ? j : "work/pipelines.journal");
    if (!cache_on && !journal_on) return;
    host_lock_init(&save_lock);
    VkPhysicalDeviceProperties gp;
    vkGetPhysicalDeviceProperties(vk.gpu, &gp);

    /* made with another GPU, driver or set of shaders (or before there was a stamp): both files go */
    char stamp_path[1040], now[160], was[160] = "";
    snprintf(stamp_path, sizeof stamp_path, "%s.stamp", path);
    int64_t t0 = host_now_ns();
    stamp_now(now, sizeof now, &gp);
    int64_t stamp_ms = (host_now_ns() - t0) / 1000000;
    FILE *f = fopen(stamp_path, "rb");
    if (f) {
        size_t k = fread(was, 1, sizeof was - 1, f);
        was[k] = 0;
        was[strcspn(was, "\r\n")] = 0;
        fclose(f);
    }
    /* Only the shaders changed (a game update): both files stay (decision 0070). The cache's entries are keyed by
     * the shaders' bytes, so the old ones just don't match; the unchanged ones still do. The journal's records name
     * shaders by the game's keys and the pipeline state, which a rebuild doesn't change: at start the prewarm builds
     * them again from the new files in the background, before the game asks for them. A cache that grew past
     * 64 MiB through updates goes (the journal fills it again). */
    const char *now_sh = strstr(now, " shaders "), *was_sh = strstr(was, " shaders ");
    if (strcmp(now, was) != 0 && now_sh && was_sh && now_sh - now == was_sh - was && !strncmp(now, was, (size_t)(now_sh - now))) {
        f = cache_on ? fopen(path, "rb") : NULL;
        long len = 0;
        if (f) fseek(f, 0, SEEK_END), len = ftell(f), fclose(f);
        int too_big = len > 64l << 20;
        if (too_big) remove(path);
        fprintf(stderr, "yw4: gpu: pipeline cache%s and journal kept: the shaders changed; the journal's pipelines are "
                "built again in the background at start\n", too_big ? " (over 64 MiB: emptied)" : "");
        if ((f = fopen(stamp_path, "wb")) != NULL) {
            fprintf(f, "%s\n", now);
            fclose(f);
        }
    } else if (strcmp(now, was) != 0) {
        char tmp[1040];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        int had = 0;
        if (cache_on) had |= remove(path) == 0, remove(tmp);
        if (journal_on) {
            had |= remove(journal_path) == 0;
            snprintf(tmp, sizeof tmp, "%s.old", journal_path);  /* and the one journal_open set aside, if any */
            remove(tmp);
        }
        if (had)
            fprintf(stderr, "yw4: gpu: pipeline cache and journal thrown away: %s\n",
                    *was ? "another GPU, driver or set of shaders than they were made with" : "no stamp beside them");
        if ((f = fopen(stamp_path, "wb")) != NULL) {  /* after the files are gone: a crash in between repeats this */
            fprintf(f, "%s\n", now);
            fclose(f);
        }
    }
    if (stamp_ms >= 20) fprintf(stderr, "yw4: gpu: pipeline cache: listing the shader folders took %lld ms\n", (long long)stamp_ms);
    if (!cache_on) return;

    size_t n = 0;
    void *data = NULL;
    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0 && (data = malloc((size_t)len)) && fread(data, 1, (size_t)len, f) == (size_t)len) n = (size_t)len;
        fclose(f);
    }
    if (n && !header_ok(data, n, &gp)) {
        /* not this GPU's and driver's (or damaged): never handed to the driver; the next save replaces it */
        fprintf(stderr, "yw4: gpu: pipeline cache %s: its header isn't this GPU's and driver's, not loaded\n", path);
        n = 0;
        remove(path);
    }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = n,
                                    .pInitialData = n ? data : NULL};
    if (vkCreatePipelineCache(vk.device, &ci, NULL, &cache) != VK_SUCCESS) {
        /* a file the driver won't take after all: start empty */
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        n = 0;
        if (vkCreatePipelineCache(vk.device, &ci, NULL, &cache) != VK_SUCCESS) cache = VK_NULL_HANDLE;
    }
    free(data);
    fprintf(stderr, "yw4: gpu: pipeline cache %s: %zu KiB loaded\n", cache ? path : "off", n / 1024);
    if (!cache) return;
    atexit(at_exit);
    if (host_thread_start(saver, NULL, 1 << 16)) fprintf(stderr, "yw4: gpu: cannot start the pipeline cache saver\n");
}

void gpu_pcache_save(void) { save(); }
