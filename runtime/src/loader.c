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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "yw4load.h"

uint64_t yw4_image_base;
uint64_t yw4_sdk_base;

void *yw4_reserve(uint64_t want, size_t size) {
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p && want) p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p;
#else
    void *p = mmap((void *)(uintptr_t)want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* Address space without memory behind it, at exactly `want` (NULL if taken). */
void *yw4_reserve_range(uint64_t want, size_t size) {
#ifdef _WIN32
    return VirtualAlloc((void *)(uintptr_t)want, size, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap((void *)(uintptr_t)want, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED && p != (void *)(uintptr_t)want) {
        munmap(p, size);
        return NULL;
    }
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* Zeroed read/write memory inside a reserved range; 0 on success. */
int yw4_commit(uint64_t addr, size_t size) {
#ifdef _WIN32
    return VirtualAlloc((void *)(uintptr_t)addr, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
#else
    return mprotect((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE);
#endif
}

/* Give the memory back but keep the range reserved; the next commit reads as zeros. */
void yw4_decommit(uint64_t addr, size_t size) {
#ifdef _WIN32
    VirtualFree((void *)(uintptr_t)addr, size, MEM_DECOMMIT);
#else
    mmap((void *)(uintptr_t)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#endif
}

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

#define MAX_HOST_FNS 256
static GuestFn host_fns[MAX_HOST_FNS];
static size_t nhost_fns;
static uint64_t host_fn_base;

static GuestFn host_lookup(uint64_t a) {
    uint64_t off = a - host_fn_base;
    if (!host_fn_base || off >= nhost_fns * 16 || off % 16) return NULL;
    return host_fns[off / 16];
}

uint64_t yw4_host_function(GuestFn fn) {
    if (!host_fn_base) {
        host_fn_base = (uint64_t)(uintptr_t)yw4_reserve(YW4_HOST_FN_BASE, MAX_HOST_FNS * 16);
        yw4_lookup_hook = host_lookup;
    }
    if (!host_fn_base || nhost_fns == MAX_HOST_FNS) {
        fprintf(stderr, "yw4: no room for host callbacks\n");
        exit(1);
    }
    host_fns[nhost_fns] = fn;
    return host_fn_base + 16 * nhost_fns++;
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
