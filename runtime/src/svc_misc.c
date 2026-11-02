/* Small services (decision 0014). Commands are added as the game first uses them; Switchbrew
 * has the ids and names. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlc_known.h"
#include "ipc_simple.h"
#include "yw4load.h"  /* yw4_sha256_hex */

/* ---- aoc:u: add-on content (decision 0049). On boot the game looks in its DLC folder (YW4_DLC_DIR, default
 * work/dlc) for the owner's add-on NSPs and owns an add-on when the SHA-256 of an NSP's content NCA is one in
 * target.toml's [dlc] table (dlc_known.c): that file is the same for every owner, a renamed or empty file
 * unlocks nothing. YW4_AOC (indices, comma separated) replaces the scan, for tests. */

static uint32_t aoc[64];
static uint32_t naoc = UINT32_MAX;

#ifdef _WIN32
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

/* One NSP (a PFS0 archive): the add-on index of its content NCA, or -1. */
static long aoc_nsp(const char *path) {
    FILE *f = fopen(path, "rb");
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
                if (!ends_with(name, ".nca") || ends_with(name, ".cnmt.nca") || size > (16u << 20)) continue;
                uint8_t *nca = malloc(size ? (size_t)size : 1);
                char hex[65];
                if (nca && !seek64(f, data + off) &&fread(nca, 1, (size_t)size, f) == size) {
                    yw4_sha256_hex(nca, (size_t)size, hex);
                    for (size_t k = 0; k < yw4_ndlc_known; k++)
                        if (!strcmp(hex, yw4_dlc_known[k].sha256)) found = yw4_dlc_known[k].index;
                }
                free(nca);
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
    DIR *d = opendir(dir);
    uint32_t files = 0, unknown = 0;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!ends_with(e->d_name, ".nsp")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        long index = aoc_nsp(path);
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
    fprintf(stderr, "yw4: aoc: %u add-on content item(s) from %u file(s) in %s%s\n", naoc, files, dir,
            unknown ? "; unknown files left out" : "");
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
