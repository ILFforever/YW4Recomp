/* NVN, the Switch's graphics API, implemented natively (decisions 0005, 0016).
 *
 * The game reaches NVN only through nvnBootstrapLoader (an override of the sdk's, see
 * recomp/overrides.toml): it asks for nvnDeviceGetProcAddress, then for every NVN function
 * by name. Each name gets a guest address that runs native code. Functions not written yet
 * still get an address, so asking for one is fine; calling it stops with its name, like an
 * unimplemented SVC. (Real NVN returns NULL for names it doesn't know; the game only asks
 * for names it uses, so a stub address for every name is the stricter choice.)
 *
 * The functions themselves are in nvn_objects.c and nvn_cmdbuf.c (decision 0017).
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host.h"
#include "kernel.h"
#include "nvn.h"
#include "yw4load.h"

static void DeviceGetProcAddress(CPUState *ctx);

static const NvnImpl entry_impls[] = {
    {"nvnDeviceGetProcAddress", DeviceGetProcAddress},
    {NULL, NULL},
};
static const NvnImpl *const impl_tables[] = {entry_impls, nvn_object_impls, nvn_cmdbuf_impls};

static GuestFn find_impl(const char *name) {
    for (size_t t = 0; t < sizeof impl_tables / sizeof *impl_tables; t++)
        for (const NvnImpl *e = impl_tables[t]; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
    return NULL;
}

_Noreturn void nvn_fatal(const CPUState *c, const char *fmt, ...) {
    char where[48];
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "yw4: NVN: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (called from %s)\n", yw4_describe(c->x[30] - 4, where));
    yw4_stop();
}

void nvn_guest_call(CPUState *c, uint64_t fn) {
    uint64_t lr = c->x[30];
    yw4_resolve(fn)(c);
    c->x[30] = lr;
}

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
        GuestFn fn = find_impl(name);
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
