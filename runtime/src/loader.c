/* Load main into host memory and relocate it (decision 0010).
 *
 * Does what the Switch's rtld does for main, minus the parts the native runtime replaces:
 *   1. reserve one block for the image and its .bss, at main's Switch address if possible
 *   2. copy the flat image, after checking its size and SHA-256 against the build
 *   3. RELATIVE relocations: base + addend
 *   4. imports: function imports get a unique address in a stub region that yw4_resolve
 *      maps to imp_<i>; data imports get a zeroed block each; weak undefined symbols are 0
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
uint64_t yw4_import_base;

#define DATA_IMPORT_SIZE 4096  /* zeroed backing for each data import until HLE fills it */

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

static GuestFn import_lookup(uint64_t a) {
    uint64_t off = a - yw4_import_base;
    if (off >= yw4_nimports * YW4_IMPORT_STRIDE || off % YW4_IMPORT_STRIDE) return NULL;
    return yw4_import_fns[off / YW4_IMPORT_STRIDE];
}

int yw4_load_image(const char *path) {
    size_t len;
    uint8_t *file = read_file(path, &len);
    if (!file) {
        fprintf(stderr, "yw4: cannot read %s\n", path);
        return 1;
    }
    char hash[65];
    yw4_sha256_hex(file, len, hash);
    if (len != yw4_image_file_size || strcmp(hash, yw4_image_sha256) != 0) {
        fprintf(stderr, "yw4: %s is not the build this was recompiled from\n"
                        "  expected %s (%" PRIu64 " bytes)\n  got      %s (%zu bytes)\n",
                path, yw4_image_sha256, yw4_image_file_size, hash, len);
        free(file);
        return 1;
    }

    uint8_t *base = yw4_reserve(YW4_PREFERRED_BASE, (size_t)yw4_image_mem_size);
    if (!base) {
        fprintf(stderr, "yw4: cannot reserve %" PRIu64 " bytes for main\n", yw4_image_mem_size);
        return 1;
    }
    memcpy(base, file, len);  /* the rest (.bss) is already zero */
    free(file);
    yw4_image_base = (uint64_t)(uintptr_t)base;
    if (yw4_image_base & 0xFFF) {
        fprintf(stderr, "yw4: image base %p is not 4 KiB aligned\n", (void *)base);
        return 1;
    }

    /* Imports: one stub region for functions, one zeroed block per data import. */
    uint8_t *stubs = yw4_reserve(0, yw4_nimports * YW4_IMPORT_STRIDE);
    uint64_t *addr = calloc(yw4_nimports, sizeof *addr);
    if (!stubs || !addr) {
        fprintf(stderr, "yw4: out of memory for imports\n");
        return 1;
    }
    yw4_import_base = (uint64_t)(uintptr_t)stubs;
    yw4_lookup_hook = import_lookup;
    size_t ndata = 0;
    for (size_t i = 0; i < yw4_nimports; i++) {
        switch (yw4_imports[i].kind) {
        case YW4_IMPORT_FUNC: addr[i] = yw4_import_base + i * YW4_IMPORT_STRIDE; break;
        case YW4_IMPORT_DATA: addr[i] = (uint64_t)(uintptr_t)calloc(1, DATA_IMPORT_SIZE); ndata++; break;
        default: addr[i] = 0; break;
        }
    }

    for (size_t i = 0; i < yw4_nrelative; i++)
        st64(yw4_image_base + yw4_relative[i].offset, yw4_image_base + (uint64_t)yw4_relative[i].addend);
    for (size_t i = 0; i < yw4_nsymrelocs; i++) {
        const Yw4SymReloc *r = &yw4_symrelocs[i];
        st64(yw4_image_base + r->offset, addr[r->import] ? addr[r->import] + (uint64_t)r->addend : 0);
    }
    free(addr);

    fprintf(stderr, "yw4: main loaded at %p (%s), %zu relocations, %zu imports (%zu data)\n",
            (void *)base, yw4_image_base == YW4_PREFERRED_BASE ? "its Switch address" : "NOT its Switch address",
            yw4_nrelative + yw4_nsymrelocs, yw4_nimports, ndata);
    return 0;
}
