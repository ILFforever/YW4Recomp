/* Small services (decision 0014). Commands are added as the game first uses them; Switchbrew
 * has the ids and names. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL_filesystem.h>

#include "dlc_known.h"
#include "ipc_simple.h"
#include "yw4load.h"  /* yw4_sha256_hex */

/* ---- aoc:u: add-on content (decision 0049). On boot the game looks in its DLC folder (YW4_DLC_DIR, default
 * work/dlc) for the owner's add-on NSPs and owns an add-on when the SHA-256 of an NSP's content NCA body (past
 * its 0xC00-byte header) is one in target.toml's [dlc] table (dlc_known.c): that is the same for every owner,
 * with or without a ticket, and an empty or home-made file unlocks nothing. YW4_AOC (indices, comma separated) replaces the scan, for tests. */

static uint32_t aoc[64];
static uint32_t naoc = UINT32_MAX;

#ifdef _WIN32
#include <io.h>
#include <wchar.h>
#define seek64(f, o) _fseeki64(f, (long long)(o), SEEK_SET)
#else
#define seek64(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#endif

static void aoc_add(uint32_t index) {
    for (uint32_t i = 0; i < naoc; i++)
        if (aoc[i] == index) return;
    if (naoc < 64) aoc[naoc++] = index;
}

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)s[n - m + i]) != tail[i]) return 0;
    return 1;
}

/* The NCA header (0xC00 bytes: the main header and four section headers) is left out of the hash: tools that
 * remove the ticket rewrite its rights ID and key area, and rename the file, while the body stays the same. */
enum { NCA_HEADER = 0xC00 };

/* SHA-256 of `size` bytes at `at`, read in pieces (the 4++ Upgrade Pack's NCA is 222 MiB). 0, or -1 on a short read. */
static int hash_part(FILE *f, uint64_t at, uint64_t size, char hex[65]) {
    enum { CHUNK = 1 << 20 };
    uint8_t *buf = malloc(CHUNK);
    Yw4Sha256 c;
    yw4_sha256_init(&c);
    int ok = buf && !seek64(f, at);
    while (ok && size) {
        size_t k = size < CHUNK ? (size_t)size : CHUNK;
        ok = fread(buf, 1, k, f) == k;
        yw4_sha256_update(&c, buf, k);
        size -= k;
    }
    free(buf);
    yw4_sha256_final_hex(&c, hex);
    return ok ? 0 : -1;
}

/* SHA-256 of a content NCA's body (all but the header). Hashing a 222 MiB NCA in C takes ~1.1 s; once a >1 MiB
 * NCA is hashed, remember `(name, size, first-4-KiB SHA-256) -> body SHA-256` in aoc-body.cache next to yw4.exe
 * so later boots only hash 4 KiB. (aoc.cache, from before 2026-11-12, held whole-file hashes: not read.) */
static int hash_nca(FILE *f, const char *name, uint64_t at, uint64_t size, char hex[65]) {
    if (size <= (1u << 20)) return hash_part(f, at + NCA_HEADER, size - NCA_HEADER, hex);
    char spot[65], path[1100], cname[64], cspot[65], chex[65];
    if (hash_part(f, at, 4096, spot)) return -1;
    const char *base = SDL_GetBasePath();
    snprintf(path, sizeof path, "%saoc-body.cache", base ? base : "");
    FILE *cf = fopen(path, "r");
    for (unsigned long long csize; cf && fscanf(cf, "%63s %llu %64s %64s", cname, &csize, cspot, chex) == 4;)
        if (!strcmp(cname, name) && csize == size && !strcmp(cspot, spot)) {
            memcpy(hex, chex, 65);
            fclose(cf);
            return 0;
        }
    if (cf) fclose(cf);
    if (hash_part(f, at + NCA_HEADER, size - NCA_HEADER, hex)) return -1;
    if ((cf = fopen(path, "a")) != NULL) {
        fprintf(cf, "%s %llu %s %s\n", name, (unsigned long long)size, spot, hex);
        fclose(cf);
    }
    return 0;
}

/* One NSP (a PFS0 archive): the add-on index of its content NCA, or -1. */
static long aoc_nsp(FILE *f) {
    if (!f) return -1;
    long found = -1;
    uint8_t head[16];
    uint32_t count, strtab;
    if (fread(head, 1, 16, f) == 16 && !memcmp(head, "PFS0", 4) && (memcpy(&count, head + 4, 4), count < 64) &&
        (memcpy(&strtab, head + 8, 4), strtab < 0x10000)) {
        uint8_t *table = malloc((size_t)count * 0x18 + strtab + 1);
        if (table && fread(table, 1, (size_t)count * 0x18 + strtab, f) == (size_t)count * 0x18 + strtab) {
            table[(size_t)count * 0x18 + strtab] = 0;
            const char *names = (const char *)table + (size_t)count * 0x18;
            uint64_t data = 16 + (uint64_t)count * 0x18 + strtab;
            for (uint32_t i = 0; i < count && found < 0; i++) {
                uint64_t off, size;
                uint32_t name_off;
                memcpy(&off, table + i * 0x18, 8), memcpy(&size, table + i * 0x18 + 8, 8);
                memcpy(&name_off, table + i * 0x18 + 16, 4);
                const char *name = name_off < strtab ? names + name_off : "";
                /* Every content NCA up to 256 MiB is hashed: the base game's and update's program NCAs (GiBs, when
                 * they share the folder) are skipped, their small control/manual NCAs cost nothing. */
                if (!ends_with(name, ".nca") || ends_with(name, ".cnmt.nca") || size <= NCA_HEADER ||
                    size > (256u << 20))
                    continue;
                char hex[65];
                if (!hash_nca(f, name, data + off, size, hex))
                    for (size_t k = 0; k < yw4_ndlc_known; k++)
                        if (!strcmp(hex, yw4_dlc_known[k].sha256)) found = yw4_dlc_known[k].index;
            }
        }
        free(table);
    }
    fclose(f);
    return found;
}

static void aoc_load(void) {
    if (naoc != UINT32_MAX) return;
    naoc = 0;
    const char *s = getenv("YW4_AOC");
    if (s && *s) {
        while (*s && naoc < 64) {
            char *e;
            unsigned long v = strtoul(s, &e, 10);
            if (e == s) break;
            aoc_add((uint32_t)v);
            s = *e == ',' ? e + 1 : e;
        }
        fprintf(stderr, "yw4: aoc: %u add-on content item(s) from YW4_AOC\n", naoc);
        return;
    }
    const char *dir = getenv("YW4_DLC_DIR");
    if (!dir || !*dir) dir = "work/dlc";
    struct timespec t0, t1;
    timespec_get(&t0, TIME_UTC);
    uint32_t files = 0, unknown = 0;
#ifdef _WIN32
    /* _wfindfirst64 / _wfopen: add-on NSPs often have Japanese titles in their file names, and UCRT's narrow
     * readdir turns characters outside the active code page into '?' before fopen sees them. */
    wchar_t wdir[1024], pat[1040];
    size_t wn = mbstowcs(wdir, dir, 1024);
    if (wn != (size_t)-1 && wn < 1024) {
        swprintf(pat, 1040, L"%ls\\*", wdir);
        struct _wfinddata64_t fd;
        intptr_t h = _wfindfirst64(pat, &fd);
        for (int more = h != -1; more; more = _wfindnext64(h, &fd) == 0) {
            size_t len = wcslen(fd.name);
            char name[512] = "";
            for (size_t i = 0; i < len && i + 1 < sizeof name; i++)
                name[i] = fd.name[i] < 128 ? (char)fd.name[i] : '?', name[i + 1] = 0;
            if (!ends_with(name, ".nsp")) continue;
            wchar_t wpath[1536];
            swprintf(wpath, 1536, L"%ls\\%ls", wdir, fd.name);
            long index = aoc_nsp(_wfopen(wpath, L"rb"));
            files++;
            if (index >= 0) {
                aoc_add((uint32_t)index);
                fprintf(stderr, "yw4: aoc: %s: add-on %ld\n", name, index);
            } else {
                unknown++;
                fprintf(stderr, "yw4: aoc: %s is not a known add-on (left out)\n", name);
            }
        }
        if (h != -1) _findclose(h);
    }
#else
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!ends_with(e->d_name, ".nsp")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        long index = aoc_nsp(fopen(path, "rb"));
        files++;
        if (index >= 0) {
            aoc_add((uint32_t)index);
            fprintf(stderr, "yw4: aoc: %s: add-on %ld\n", e->d_name, index);
        } else {
            unknown++;
            fprintf(stderr, "yw4: aoc: %s is not a known add-on (left out)\n", e->d_name);
        }
    }
    if (d) closedir(d);
#endif
    timespec_get(&t1, TIME_UTC);
    fprintf(stderr, "yw4: aoc: %u add-on content item(s) from %u file(s) in %s%s (%.0f ms)\n", naoc, files, dir,
            unknown ? "; unknown files left out" : "",
            (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6);
}

int aoc_owned(uint32_t index) {
    aoc_load();
    for (uint32_t i = 0; i < naoc; i++)
        if (aoc[i] == index) return 1;
    return 0;
}

void svc_aoc_u(Service *s, IpcRequest *req, IpcResponse *resp) {
    aoc_load();
    switch (req->cmd) {
    case 2:  /* CountAddOnContent(pid) -> u32 */
        IPC_OUT(uint32_t, 0) = naoc;
        ipc_out_size(resp, 4);
        return;
    case 3: {  /* ListAddOnContent(u32 offset, u32 count, pid, B: u32 indices[]) -> u32 written */
        uint32_t off = IPC_IN(uint32_t, 0), count = IPC_IN(uint32_t, 4), n = 0;
        IpcBuffer out = ipc_out_buffer(req, 0);
        uint32_t *ids = out.addr ? (uint32_t *)GPTR(out.addr) : NULL;
        for (uint32_t i = off; i < naoc && n < count && ids && (n + 1) * 4 <= out.size; i++) ids[n++] = aoc[i];
        fprintf(stderr, "yw4: aoc: ListAddOnContent(offset %u, count %u, buffer %llu bytes) -> %u\n", off, count,
                (unsigned long long)out.size, n);
        IPC_OUT(uint32_t, 0) = n;
        ipc_out_size(resp, 4);
        return;
    }
    case 5:  /* GetAddOnContentBaseId(pid) -> u64: the application's ID + 0x1000 */
        IPC_OUT(uint64_t, 0) = 0x010086C00AF7D000ull;
        ipc_out_size(resp, 8);
        return;
    case 7:  /* PrepareAddOnContent(u32 index, pid): nothing to mount until the game reads it */
        return;
    }
    ipc_unimplemented(s, req, resp);
}

/* ---- pctl: parental controls. No restrictions are set. */

static const SimpleCommand pctl_service_simple[] = {
    {1, SIMPLE_OK, 0, "Initialize"},
};

static void pctl_service(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!IPC_SIMPLE(pctl_service_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

void svc_pctl(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0 || req->cmd == 1) {  /* CreateService / CreateServiceWithoutInitialize */
        ipc_return_object(resp, ipc_service_new("IParentalControlService", pctl_service, NULL));
        return;
    }
    ipc_unimplemented(s, req, resp);
}
