/* The pipeline cache on disk: every pipeline the driver compiles is kept between runs.
 *
 * Without one, every graphics pipeline was compiled from nothing in every run, on the GPU thread, the first
 * time a draw needed it: 40-300 ms each on a Radeon RX 6650 XT (2026-11-08), 117 of them (2.9 s) when the town
 * first appears, and a hitch whenever a new effect, character or menu showed up. With the cache a pipeline
 * built once before comes back from the driver in about a millisecond. All pipeline builds pass gpu_pcache();
 * the driver checks the file's header (vendor, device, driver UUID) itself and ignores a cache from another
 * GPU or driver.
 *
 * Saved by a thread every 10 s when pipelines were built since the last save, and at exit; written to a
 * temporary file and renamed, so a crash never leaves half a file. YW4_PIPELINE_CACHE=<file> moves it
 * (default work/pipelines.cache), =none turns it off.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_internal.h"
#include "host.h"

static VkPipelineCache cache;
static char path[1024];
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

void gpu_pcache_init(void) {
    const char *e = getenv("YW4_PIPELINE_CACHE");
    if (e && !strcmp(e, "none")) return;
    snprintf(path, sizeof path, "%s", e && *e ? e : "work/pipelines.cache");
    host_lock_init(&save_lock);
    size_t n = 0;
    void *data = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0 && (data = malloc((size_t)len)) && fread(data, 1, (size_t)len, f) == (size_t)len) n = (size_t)len;
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = n,
                                    .pInitialData = n ? data : NULL};
    if (vkCreatePipelineCache(vk.device, &ci, NULL, &cache) != VK_SUCCESS) {
        /* a file the driver won't take (another driver's, damaged): start empty */
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        if (vkCreatePipelineCache(vk.device, &ci, NULL, &cache) != VK_SUCCESS) cache = VK_NULL_HANDLE;
    }
    free(data);
    fprintf(stderr, "yw4: gpu: pipeline cache %s: %zu KiB loaded\n", cache ? path : "off", n / 1024);
    if (!cache) return;
    atexit(at_exit);
    if (host_thread_start(saver, NULL, 1 << 16)) fprintf(stderr, "yw4: gpu: cannot start the pipeline cache saver\n");
}

void gpu_pcache_save(void) { save(); }
