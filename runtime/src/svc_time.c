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
/* ITimeZoneService: the device's time zone is the host's local one. */
static void time_zone(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 101: {  /* ToCalendarTimeWithMyRule(PosixTime) -> CalendarTime, CalendarAdditionalInfo */
        time_t t = (time_t)IPC_IN(int64_t, 0);
        struct tm local, utc;
#ifdef _WIN32
        localtime_s(&local, &t);
        gmtime_s(&utc, &t);
#else
        localtime_r(&t, &local);
        gmtime_r(&t, &utc);
#endif
        utc.tm_isdst = local.tm_isdst;
        int32_t offset = (int32_t)difftime(mktime(&local), mktime(&utc));  /* seconds east of UTC */
        uint8_t *o = resp->data;
        /* CalendarTime (8): year u16, month, day, hour, minute, second, pad */
        uint16_t year = (uint16_t)(local.tm_year + 1900);
        memcpy(o, &year, 2);
        o[2] = (uint8_t)(local.tm_mon + 1), o[3] = (uint8_t)local.tm_mday;
        o[4] = (uint8_t)local.tm_hour, o[5] = (uint8_t)local.tm_min, o[6] = (uint8_t)local.tm_sec;
        /* CalendarAdditionalInfo (0x18): day of week, day of year, zone name[8], is DST, UTC offset */
        IPC_OUT(uint32_t, 8) = (uint32_t)local.tm_wday;
        IPC_OUT(uint32_t, 12) = (uint32_t)local.tm_yday;
        memcpy(o + 16, "LOCAL", 6);
        IPC_OUT(uint32_t, 24) = local.tm_isdst > 0;
        IPC_OUT(int32_t, 28) = offset;
        ipc_out_size(resp, 0x20);
        return;
    }
    }
    ipc_unimplemented(s, req, resp);
}

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
