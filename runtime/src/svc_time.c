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
/* CalendarTime (8) and CalendarAdditionalInfo (0x18) for a POSIX time in the host's time zone. */
static void calendar(time_t t, uint8_t *cal, uint8_t *info) {
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
    /* CalendarTime: year u16, month, day, hour, minute, second, pad */
    uint16_t year = (uint16_t)(local.tm_year + 1900);
    memcpy(cal, &year, 2);
    cal[2] = (uint8_t)(local.tm_mon + 1), cal[3] = (uint8_t)local.tm_mday;
    cal[4] = (uint8_t)local.tm_hour, cal[5] = (uint8_t)local.tm_min, cal[6] = (uint8_t)local.tm_sec;
    /* CalendarAdditionalInfo: day of week, day of year, zone name[8], is DST, UTC offset */
    uint32_t wday = (uint32_t)local.tm_wday, yday = (uint32_t)local.tm_yday, dst = local.tm_isdst > 0;
    memcpy(info, &wday, 4);
    memcpy(info + 4, &yday, 4);
    memcpy(info + 8, "LOCAL", 6);
    memcpy(info + 16, &dst, 4);
    memcpy(info + 20, &offset, 4);
}

/* ITimeZoneService: the device's time zone is the host's local one. */
static void time_zone(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 101: {  /* ToCalendarTimeWithMyRule(PosixTime) -> CalendarTime, CalendarAdditionalInfo */
        calendar((time_t)IPC_IN(int64_t, 0), resp->data, resp->data + 8);
        ipc_out_size(resp, 0x20);
        return;
    }
    }
    ipc_unimplemented(s, req, resp);
}

/* ClockSnapshot (0xD0), as in Switchbrew's "PSC services": user and network clock context
 * (0x20 each), POSIX times, calendar times (8) and additional infos (0x18), the steady clock
 * time point (0x18), the location name (0x24), the auto-correction flag and the type. */
static void clock_snapshot(uint8_t *p, uint8_t type) {
    memset(p, 0, 0xD0);
    system_clock_context(p + 0x00);
    system_clock_context(p + 0x20);
    int64_t now = (int64_t)time(NULL);
    memcpy(p + 0x40, &now, 8);
    memcpy(p + 0x48, &now, 8);
    calendar((time_t)now, p + 0x50, p + 0x60);
    calendar((time_t)now, p + 0x58, p + 0x78);
    int64_t steady = steady_seconds();
    memcpy(p + 0x90, &steady, 8);
    memcpy(p + 0x98, clock_source, 16);
    memcpy(p + 0xA8, "LOCAL", 6);
    p[0xCD] = type;
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
    case 500: {  /* CalculateStandardUserSystemClockDifferenceByUser(snapshot a, snapshot b) -> TimeSpan ns */
        if (req->nx < 2 || req->x[0].size < 0xD0 || req->x[1].size < 0xD0) break;
        int64_t a, b;
        memcpy(&a, (uint8_t *)GPTR(req->x[0].addr) + 0x40, 8);
        memcpy(&b, (uint8_t *)GPTR(req->x[1].addr) + 0x40, 8);
        IPC_OUT(int64_t, 0) = (b - a) * 1000000000;
        ipc_out_size(resp, 8);
        return;
    }
    case 400: {  /* GetClockSnapshot(u8 type) -> ClockSnapshot (receive buffer) */
        IpcBuffer out = ipc_out_buffer(req, 0);
        if (out.size < 0xD0) break;
        clock_snapshot(GPTR(out.addr), IPC_IN(uint8_t, 0));
        return;
    }
    default:
        ipc_unimplemented(s, req, resp);
    }
}
