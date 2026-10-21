/* time:u (and :a, :s): clocks and time zones (decision 0014). The clocks follow the host's.
 * Commands are added as the game first uses them; Switchbrew "Glue_services" / "PSC_services"
 * has the ids, LAS Recomp's services/time.c (MIT) was the cross-check for the shared memory.
 *
 * Time on the Switch: the steady clock counts seconds since boot (here: since the runtime
 * started, from CNTPCT); a system clock is "steady clock + offset", so its context stores
 * the offset and the steady time point it was taken at. */
#include <string.h>
#include <time.h>

#include "ipc_simple.h"

/* Identifies "this boot's" steady clock; any fixed value works. */
static const uint8_t clock_source[16] = {'y', 'w', '4', 'r', 'e', 'c', 'o', 'm', 'p', 1, 2, 3, 4, 5, 6, 7};

static int64_t steady_seconds(void) { return (int64_t)(yw4_cntpct() / 19200000u); }

/* SystemClockContext (0x20): offset, then SteadyClockTimePoint {seconds, source}. */
static void system_clock_context(uint8_t *p) {
    int64_t steady = steady_seconds();
    int64_t offset = (int64_t)time(NULL) - steady;
    memcpy(p, &offset, 8);
    memcpy(p + 8, &steady, 8);
    memcpy(p + 0x10, clock_source, 16);
}

/* The page the time service shares with every process. Each field is a LockFreeAtomicType:
 * a u32 counter, then two copies of the value; readers use copy (counter & 1). */
static void time_shared_memory(KSharedMemory *m) {
    uint8_t *p = GPTR(m->guest_addr);
    memset(p, 0, m->size);
    /* 0x00: SteadyClockContext {u64 internal offset, source}: copy 0 at +0x08 */
    memcpy(p + 0x08 + 8, clock_source, 16);
    /* 0x38: local system clock context, copy 0 at +0x40; 0x80: network system clock, +0x88 */
    system_clock_context(p + 0x40);
    system_clock_context(p + 0x88);
    /* 0xC8: automatic correction enabled (bool at +0xCC): off */
}

static void system_clock(Service *s, IpcRequest *req, IpcResponse *resp) { ipc_unimplemented(s, req, resp); }
static void steady_clock(Service *s, IpcRequest *req, IpcResponse *resp) { ipc_unimplemented(s, req, resp); }
static void time_zone(Service *s, IpcRequest *req, IpcResponse *resp) { ipc_unimplemented(s, req, resp); }

static uint32_t shared_memory_handle;

/* IStaticService: hands out the clock interfaces. */
void svc_time(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 0: case 1: case 4:  /* GetStandardUserSystemClock / NetworkSystemClock / LocalSystemClock */
        ipc_return_object(resp, ipc_service_new("ISystemClock", system_clock, NULL));
        return;
    case 2:  /* GetStandardSteadyClock */
        ipc_return_object(resp, ipc_service_new("ISteadyClock", steady_clock, NULL));
        return;
    case 3:  /* GetTimeZoneService */
        ipc_return_object(resp, ipc_service_new("ITimeZoneService", time_zone, NULL));
        return;
    case 20:  /* GetSharedMemoryNativeHandle: one page, the same for every caller */
        if (!shared_memory_handle) yw4_shared_memory_new(0x1000, time_shared_memory, &shared_memory_handle);
        resp->copy_handles[resp->ncopy++] = shared_memory_handle;
        return;
    default:
        ipc_unimplemented(s, req, resp);
    }
}
