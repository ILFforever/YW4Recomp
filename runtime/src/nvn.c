/* NVN, the Switch's graphics API, implemented natively (decisions 0005, 0016).
 *
 * The game reaches NVN only through nvnBootstrapLoader (an override of the sdk's, see
 * recomp/overrides.toml): it asks for nvnDeviceGetProcAddress, then for every NVN function
 * by name. Each name gets a guest address that runs native code. Functions not written yet
 * still get an address, so asking for one is fine; calling it stops with its name, like an
 * unimplemented SVC. (Real NVN returns NULL for names it doesn't know; the game only asks
 * for names it uses, so a stub address for every name is the stricter choice.)
 *
 * NVN objects (device, queue, command buffer...) are structs the game allocates with sizes
 * fixed by the API; we keep our state inside them, as the real driver does.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host.h"
#include "kernel.h"
#include "yw4load.h"

static void DeviceGetProcAddress(CPUState *ctx);

static const struct {
    const char *name;
    GuestFn fn;
} implemented[] = {
    {"nvnDeviceGetProcAddress", DeviceGetProcAddress},
};

/* ---- names to guest addresses */

#define MAX_PROCS 1024
static struct {
    char *name;
    uint64_t addr;
} procs[MAX_PROCS];
static int nprocs;
static HostLock procs_lock = HOST_LOCK_INIT;

static void missing(CPUState *ctx) {
    const char *name = yw4_host_data();
    char where[48];
    fprintf(stderr, "yw4: NVN function %s not implemented yet (called from %s; x0=%#" PRIx64 " x1=%#" PRIx64
                    " x2=%#" PRIx64 ")\n", name, yw4_describe(ctx->x[30] - 4, where), ctx->x[0], ctx->x[1], ctx->x[2]);
    yw4_stop();
}

static uint64_t proc_address(const char *name) {
    host_lock(&procs_lock);
    uint64_t addr = 0;
    for (int i = 0; i < nprocs && !addr; i++)
        if (!strcmp(procs[i].name, name)) addr = procs[i].addr;
    if (!addr) {
        if (nprocs == MAX_PROCS) {
            fprintf(stderr, "yw4: NVN: too many function names\n");
            exit(3);
        }
        char *copy = malloc(strlen(name) + 1);
        if (!copy) exit(3);
        strcpy(copy, name);
        GuestFn fn = NULL;
        for (size_t i = 0; i < sizeof implemented / sizeof *implemented && !fn; i++)
            if (!strcmp(implemented[i].name, name)) fn = implemented[i].fn;
        addr = fn ? yw4_host_function_data(fn, copy) : yw4_host_function_data(missing, copy);
        procs[nprocs].name = copy;
        procs[nprocs++].addr = addr;
    }
    host_unlock(&procs_lock);
    return addr;
}

/* nvnBootstrapLoader(name) -> function */
void yw4_nvn_BootstrapLoader(CPUState *ctx) { ctx->x[0] = proc_address((const char *)GPTR(ctx->x[0])); }

/* nvnDeviceGetProcAddress(device or NULL, name) -> function */
static void DeviceGetProcAddress(CPUState *ctx) { ctx->x[0] = proc_address((const char *)GPTR(ctx->x[1])); }

/* ---- the sdk's GPU driver, which we don't run */

/* nv::InitializeGraphics(memory, size): the memory is for the driver's own bookkeeping.
 * Ours lives on the host, so it stays unused. */
void yw4_nv_InitializeGraphics(CPUState *ctx) {
    fprintf(stderr, "yw4: nv::InitializeGraphics(%#" PRIx64 ", %#" PRIx64 "): no GPU driver to start\n", ctx->x[0],
            ctx->x[1]);
}

void yw4_nv_FinalizeGraphics(CPUState *ctx) { (void)ctx; }
