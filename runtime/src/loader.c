/* Load the recompiled modules and relocate them (decisions 0010, 0014).
 *
 * Does what the Switch's rtld does, minus symbol lookup (already done at translation time):
 *   1. reserve one block per module for its image and .bss, at its preferred address if
 *      possible (main at its Switch address 0x7100000000, the sdk right after it)
 *   2. copy the flat image, after checking its size and SHA-256 against the build
 *   3. RELATIVE relocations: base + addend
 *   4. symbolic relocations: the target module's base + value (or 0 for weak symbols
 *      nobody defines)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#endif

#include "host.h"
#include "yw4load.h"

uint64_t yw4_image_base;
uint64_t yw4_sdk_base;

/* Which 4 KiB pages of the guest's range [YW4_MAP_LO, YW4_MAP_HI) have memory: one bit each (1 MiB), set by
 * yw4_reserve and yw4_commit once the memory is there, cleared by yw4_decommit before it goes. Asking this instead of
 * VirtualQuery (host_readable): the graphics driver holds the process's address-space lock while it makes GPU memory,
 * and VirtualQuery waits for it, up to 41 ms a call; the render threads' calls froze the first Demon Hand of a
 * session for 125-344 ms (2026-11-12). */
#define YW4_MAP_LO YW4_GUEST_LO  /* the whole guest range (yw4load.h) */
#define YW4_MAP_HI YW4_GUEST_HI
static uint64_t page_map[(YW4_MAP_HI - YW4_MAP_LO) >> 12 >> 6];

static void map_pages(uint64_t addr, uint64_t size, int on) {
    if (!size) return;
    uint64_t lo = addr < YW4_MAP_LO ? YW4_MAP_LO : addr, hi = addr + size > YW4_MAP_HI ? YW4_MAP_HI : addr + size;
    if (lo >= hi) return;
    for (uint64_t p = (lo - YW4_MAP_LO) >> 12, e = (hi - YW4_MAP_LO + 4095) >> 12; p < e; p++) {
        uint64_t bit = 1ull << (p & 63);
        if (on) __atomic_fetch_or(&page_map[p >> 6], bit, __ATOMIC_RELEASE);
        else __atomic_fetch_and(&page_map[p >> 6], ~bit, __ATOMIC_RELEASE);
    }
}

/* 1: every page of [addr, addr + size) has memory (by the map); 0: not, or outside the guest range (-1: unknown) */
int yw4_guest_mapped(uint64_t addr, uint64_t size) {
    if (addr < YW4_MAP_LO || addr + size > YW4_MAP_HI || addr + size < addr) return -1;
    for (uint64_t p = (addr - YW4_MAP_LO) >> 12, e = (addr + size - YW4_MAP_LO + 4095) >> 12; p < e; p++)
        if (!(__atomic_load_n(&page_map[p >> 6], __ATOMIC_ACQUIRE) >> (p & 63) & 1)) return 0;
    return 1;
}

void *yw4_reserve(uint64_t want, size_t size) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p && want) p = VirtualAlloc((void *)(uintptr_t)want, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p && want) p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) map_pages((uint64_t)(uintptr_t)p, size, 1);
    return p;
#else
    if (want && mprotect((void *)(uintptr_t)want, size, PROT_READ | PROT_WRITE) == 0) {  /* inside the reserved range */
        map_pages(want, size, 1);
        return (void *)(uintptr_t)want;
    }
    void *p = mmap((void *)(uintptr_t)want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) map_pages((uint64_t)(uintptr_t)p, size, 1);
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* Address space without memory behind it, at exactly `want` (NULL if taken). */
void *yw4_reserve_range(uint64_t want, size_t size) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)want, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        DWORD err = GetLastError();
        fprintf(stderr, "yw4: yw4_reserve_range: VirtualAlloc at %#" PRIx64 " (%" PRIu64 " bytes) failed: error %lu\n",
                want, (uint64_t)size, (unsigned long)err);
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)(uintptr_t)want, &mbi, sizeof mbi)) {
            char mod[MAX_PATH] = "";
            if (mbi.Type == MEM_IMAGE && mbi.AllocationBase) {
                GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, sizeof mod);
            }
            fprintf(stderr,
                    "yw4:   collision at %#" PRIx64 ": base=%#" PRIx64 " size=%#" PRIx64 " state=%#x type=%#x%s%s\n",
                    want, (uint64_t)(uintptr_t)mbi.BaseAddress, (uint64_t)mbi.RegionSize,
                    (unsigned)mbi.State, (unsigned)mbi.Type, mod[0] ? " module=" : "", mod[0] ? mod : "");
        }
    }
    return p;
#else
    void *p = mmap((void *)(uintptr_t)want, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED && p != (void *)(uintptr_t)want) {
        munmap(p, size);
        fprintf(stderr, "yw4: yw4_reserve_range: mmap placed at %p instead of %#" PRIx64 "\n", p, want);
        return NULL;
    }
    if (p == MAP_FAILED) {
        fprintf(stderr, "yw4: yw4_reserve_range: mmap at %#" PRIx64 " (%" PRIu64 " bytes) failed: %s\n",
                want, (uint64_t)size, strerror(errno));
        return NULL;
    }
    return p;
#endif
}

/* Zeroed read/write memory inside a reserved range; 0 on success. */
volatile uint32_t yw4_memory_generation;

/* Only yw4_decommit changes yw4_memory_generation: a commit can turn "not readable" into "readable", never back, and
 * gbuffer.c's "not readable" answers last a second anyway. Commits came in hundreds while the Demon Hand loaded, and
 * each emptied every answer: the render threads asked VirtualQuery again and again, a 343 ms frame (2026-11-12). */
int yw4_commit(uint64_t addr, size_t size) {
#ifdef _WIN32
    int r = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
#else
    int r = 0;
    if (mprotect((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE) != 0) {
        void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,  /* not reserved: map it */
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        r = p == (void *)(uintptr_t)addr ? 0 : -1;
    }
#endif
    if (!r) map_pages(addr, size, 1);
    return r;
}

/* Reserves every free part of [lo, hi), so later commits work at any 4 KiB page: Windows
 * places reservations only at 64 KiB boundaries, but the game maps at 4 KiB ones. Linux
 * maps pages anywhere, so there is nothing to do there. */
void yw4_reserve_free(uint64_t lo, uint64_t hi) {
#ifdef _WIN32
    for (uint64_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (end > hi) end = hi;
        if (mbi.State == MEM_FREE) VirtualAlloc((void *)(uintptr_t)a, (size_t)(end - a), MEM_RESERVE, PAGE_NOACCESS);
        a = end;
    }
#else
    (void)lo, (void)hi;
#endif
}

static int guest_memory_ready;

int yw4_guest_memory_init(void) {
    if (guest_memory_ready) return 0;
#ifdef _WIN32
    if (!host_reserve_watched(YW4_HEAP_BASE, YW4_HEAP_SIZE) || !yw4_reserve_range(YW4_TLR_BASE, YW4_TLR_SIZE)) {
        return -1;
    }
    yw4_reserve_free(YW4_GUEST_LO, YW4_GUEST_HI);
#else
    if (!yw4_reserve_range(YW4_GUEST_LO, (size_t)(YW4_GUEST_HI - YW4_GUEST_LO))) {
        return -1;
    }
#endif
    guest_memory_ready = 1;
    return 0;
}

/* Give the memory back but keep the range reserved; the next commit reads as zeros. */
void yw4_decommit(uint64_t addr, size_t size) {
    map_pages(addr, size, 0);  /* before the memory goes */
#ifdef _WIN32
    VirtualFree((void *)(uintptr_t)addr, size, MEM_DECOMMIT);
#else
    mmap((void *)(uintptr_t)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#endif
    __atomic_add_fetch(&yw4_memory_generation, 1, __ATOMIC_RELEASE);  /* after: no answer from before it stays */
}

void yw4_host_sleep_ns(int64_t ns) { host_sleep_ns(ns); }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* ---- host callbacks: a guest address per host function, found through the lookup hook */

#define MAX_HOST_FNS 4096
static struct {
    GuestFn fn;
    void *data;
} host_fns[MAX_HOST_FNS];
static size_t nhost_fns;  /* entries below this are complete (published with release order) */
static uint64_t host_fn_base;
static HostLock host_fns_lock = HOST_LOCK_INIT;
static _Thread_local void *host_data;

static GuestFn host_lookup(uint64_t a) {
    uint64_t off = a - host_fn_base;
    if (!host_fn_base || off >= __atomic_load_n(&nhost_fns, __ATOMIC_ACQUIRE) * 16 || off % 16) return NULL;
    host_data = host_fns[off / 16].data;  /* the call follows the lookup on this same thread */
    return host_fns[off / 16].fn;
}

uint64_t yw4_host_function_data(GuestFn fn, void *data) {
    host_lock(&host_fns_lock);  /* guest threads may register callbacks at the same time */
    if (!host_fn_base) {
        host_fn_base = (uint64_t)(uintptr_t)yw4_reserve(YW4_HOST_FN_BASE, MAX_HOST_FNS * 16);
        yw4_lookup_hook = host_lookup;
    }
    if (!host_fn_base || nhost_fns == MAX_HOST_FNS) {
        fprintf(stderr, "yw4: no room for host callbacks\n");
        yw4_exit(1, __FILE__, __LINE__);
    }
    size_t i = nhost_fns;
    host_fns[i].fn = fn;
    host_fns[i].data = data;
    __atomic_store_n(&nhost_fns, i + 1, __ATOMIC_RELEASE);
    host_unlock(&host_fns_lock);
    return host_fn_base + 16 * i;
}

uint64_t yw4_host_function(GuestFn fn) { return yw4_host_function_data(fn, NULL); }

void *yw4_host_data(void) { return host_data; }

uint64_t yw4_guest_symbol(const char *name) {
    for (size_t i = 0; i < yw4_nguest_symbols; i++)
        if (!strcmp(yw4_guest_symbols[i].name, name))
            return *yw4_modules[yw4_guest_symbols[i].module].base + yw4_guest_symbols[i].offset;
    fprintf(stderr, "yw4: guest symbol %s is not in recomp/overrides.toml [[guest_symbol]]\n", name);
    yw4_exit(3, __FILE__, __LINE__);
}

/* ---- modules */

static int load_one(const Yw4Module *m, const char *dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, m->file);
    size_t len;
    uint8_t *file = read_file(path, &len);
    if (!file) {
        fprintf(stderr, "yw4: cannot read %s\n", path);
        return 1;
    }
    char hash[65];
    yw4_sha256_hex(file, len, hash);
    if (len != m->file_size || strcmp(hash, m->sha256) != 0) {
        fprintf(stderr, "yw4: %s is not the build this was recompiled from\n"
                        "  expected %s (%" PRIu64 " bytes)\n  got      %s (%zu bytes)\n",
                path, m->sha256, m->file_size, hash, len);
        free(file);
        return 1;
    }
    uint8_t *base = yw4_reserve(m->preferred_base, (size_t)m->mem_size);
    if (!base || ((uintptr_t)base & 0xFFF)) {
        fprintf(stderr, "yw4: cannot reserve %#" PRIx64 " bytes (4 KiB aligned) for %s\n", m->mem_size, m->name);
        return 1;
    }
    memcpy(base, file, len);  /* the rest (.bss) is already zero */
    free(file);
    *m->base = (uint64_t)(uintptr_t)base;
    return 0;
}

int yw4_load_modules(const char *exefs_dir) {
    for (size_t i = 0; i < yw4_nmodules; i++)
        if (load_one(&yw4_modules[i], exefs_dir)) return 1;
    size_t nrel = 0;
    for (size_t i = 0; i < yw4_nmodules; i++) {
        const Yw4Module *m = &yw4_modules[i];
        uint64_t base = *m->base;
        for (size_t k = 0; k < m->nrelative; k++)
            st64(base + m->relative[k].offset, base + (uint64_t)m->relative[k].addend);
        for (size_t k = 0; k < m->nsymrelocs; k++) {
            const Yw4SymReloc *r = &m->symrelocs[k];
            st64(base + r->offset, r->module == YW4_NO_MODULE ? 0 : *yw4_modules[r->module].base + r->value);
        }
        nrel += m->nrelative + m->nsymrelocs;
    }
    for (size_t i = 0; i < yw4_nmodules; i++) {
        const Yw4Module *m = &yw4_modules[i];
        fprintf(stderr, "yw4: %-4s loaded at %#014" PRIx64 "%s\n", m->name, *m->base,
                *m->base == m->preferred_base ? "" : "  (NOT its preferred address)");
    }
    fprintf(stderr, "yw4: %zu relocations applied\n", nrel);
    return 0;
}
