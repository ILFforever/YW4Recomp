/* Small services (decision 0014). Commands are added as the game first uses them; Switchbrew
 * has the ids and names. */
#include <stdio.h>
#include <stdlib.h>

#include "ipc_simple.h"

/* ---- aoc:u: add-on content. The owner's add-ons are the indices in YW4_AOC (comma
 * separated; `python -m recomp.build --run` derives them from [input] dlc_dir, e.g. the 4++
 * upgrade). Without it, none are installed. */

static uint32_t aoc[64];
static uint32_t naoc = UINT32_MAX;

static void aoc_load(void) {
    if (naoc != UINT32_MAX) return;
    naoc = 0;
    const char *s = getenv("YW4_AOC");
    while (s && *s && naoc < 64) {
        char *e;
        unsigned long v = strtoul(s, &e, 10);
        if (e == s) break;
        aoc[naoc++] = (uint32_t)v;
        s = *e == ',' ? e + 1 : e;
    }
    fprintf(stderr, "yw4: aoc: %u add-on content item(s)\n", naoc);
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
