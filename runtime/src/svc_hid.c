/* hid: controllers (IHidServer, Switchbrew "HID_services"; decision 0014).
 *
 * The sdk's nn::hid reads input from a 0x40000-byte shared memory block the service keeps
 * up to date (Switchbrew "HID_Shared_Memory", the layout before system version 13.0, which
 * this sdk (7.3.2) reads), and sends settings and vibration through IPC.
 *
 * One controller is connected: a Pro Controller as player 1 (npad 0, full-key style). Its
 * state comes from the window (keyboard and gamepad, window_input) and is written by
 * yw4_hid_update, which the 60 Hz vsync thread calls. Each controller style has a LIFO of
 * 17 samples: a header (u64 unused, u64 count of entries 17, u64 tail index, u64 samples
 * stored), then entries {u64 sampling number, state}; the state of a npad is {u64 sampling
 * number, u64 buttons, s32 left x, y, s32 right x, y, u32 attributes, u32 reserved}.
 *
 * YW4_INPUT=<tick>:<buttons hex>:<ticks>,... holds buttons from a vsync tick on (60 per
 * second), for testing without a person at the keyboard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc_simple.h"
#include "window.h"

#define HID_SHARED_MEMORY_SIZE 0x40000

/* Offsets in the shared memory (pre-13.0 layout) */
#define NPAD_BASE 0x9A00        /* 10 npad entries of 0x5000: ids 0-7, handheld, other */
#define NPAD_SIZE 0x5000
#define NPAD_STYLE 0x0          /* u32 style set: bit 0 full key (Pro Controller) */
#define NPAD_ASSIGNMENT 0x4     /* u32: 0 dual */
#define NPAD_FULL_KEY_COLOR 0x8 /* u32 attribute (0 ok), u32 main colour, u32 sub colour */
#define NPAD_FULL_KEY_LIFO 0x28
#define NPAD_DEVICE_TYPE 0x4188 /* u32: bit 0 full key */
#define NPAD_SYSTEM_PROPS 0x4190
#define NPAD_BATTERY 0x419C     /* u32 x3 */
#define LIFO_ENTRIES 17
#define LIFO_ENTRY_SIZE 0x30

static uint32_t shared_memory_handle;
static KSharedMemory *shared_memory;

static uint8_t *npad(int i) {
    return (uint8_t *)(uintptr_t)(shared_memory->guest_addr + NPAD_BASE + (uint64_t)i * NPAD_SIZE);
}

static void on_map(KSharedMemory *m) {
    (void)m;
    uint8_t *p = npad(0);
    *(uint32_t *)(p + NPAD_STYLE) = 1;  /* full key */
    *(uint32_t *)(p + NPAD_ASSIGNMENT) = 0;
    uint32_t color[3] = {0, 0xFF323232, 0xFF0F0F0F};  /* ok, dark grey body, black buttons */
    memcpy(p + NPAD_FULL_KEY_COLOR, color, sizeof color);
    *(uint32_t *)(p + NPAD_DEVICE_TYPE) = 1;  /* full key */
    /* system properties: ABXY oriented (11), Plus (13), Minus (14) and directional buttons (15) available */
    *(uint64_t *)(p + NPAD_SYSTEM_PROPS) = 1u << 11 | 1u << 13 | 1u << 14 | 1u << 15;
    for (int i = 0; i < 3; i++) *(uint32_t *)(p + NPAD_BATTERY + 4 * i) = 4;  /* full */
    *(uint64_t *)(p + NPAD_FULL_KEY_LIFO + 8) = LIFO_ENTRIES;
}

/* Scripted presses (YW4_INPUT) */
static uint64_t scripted(uint64_t tick) {
    static const char *spec;
    static int read;
    if (!read) spec = getenv("YW4_INPUT"), read = 1;
    uint64_t b = 0;
    for (const char *s = spec; s && *s;) {
        char *e;
        unsigned long long at = strtoull(s, &e, 10), mask = 0, len = 10;
        if (*e == ':') mask = strtoull(e + 1, &e, 16);
        if (*e == ':') len = strtoull(e + 1, &e, 10);
        if (tick >= at && tick < at + len) b |= mask;
        s = *e == ',' ? e + 1 : e;
        if (e == s && *s) break;
    }
    return b;
}

void yw4_hid_update(uint64_t tick) {
    KSharedMemory *m = __atomic_load_n(&shared_memory, __ATOMIC_ACQUIRE);
    if (!m || !__atomic_load_n(&m->guest_addr, __ATOMIC_ACQUIRE)) return;
    uint64_t buttons;
    int32_t sticks[4];
    window_input(&buttons, sticks);
    buttons |= scripted(tick);
    /* the sticks' direction bits, as the hardware sets them past about half way */
    static const int dir[4][2] = {{16, 18}, {19, 17}, {20, 22}, {23, 21}};  /* (negative, positive) bits */
    for (int i = 0; i < 4; i++) {
        if (sticks[i] < -16384) buttons |= 1ull << dir[i][0];
        if (sticks[i] > 16384) buttons |= 1ull << dir[i][1];
    }
    uint8_t *lifo = npad(0) + NPAD_FULL_KEY_LIFO;
    uint64_t *head = (uint64_t *)lifo;  /* unused, entries, tail, count */
    static uint64_t sampling;
    sampling++;
    uint64_t tail = (head[2] + 1) % LIFO_ENTRIES;
    uint8_t *entry = lifo + 0x20 + tail * LIFO_ENTRY_SIZE;
    struct {
        uint64_t sampling, buttons;
        int32_t sticks[4];
        uint32_t attributes, reserved;
    } state = {sampling, buttons, {sticks[0], sticks[1], sticks[2], sticks[3]}, 1 /* connected */, 0};
    __atomic_store_n((uint64_t *)entry, sampling, __ATOMIC_RELAXED);
    memcpy(entry + 8, &state, sizeof state);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (head[3] < LIFO_ENTRIES) __atomic_store_n(&head[3], head[3] + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&head[2], tail, __ATOMIC_RELEASE);
}

static void applet_resource(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* GetSharedMemoryHandle */
        if (!shared_memory_handle) {
            KSharedMemory *m = yw4_shared_memory_new(HID_SHARED_MEMORY_SIZE, on_map, &shared_memory_handle);
            __atomic_store_n(&shared_memory, m, __ATOMIC_RELEASE);
        }
        resp->copy_handles[resp->ncopy++] = shared_memory_handle;
        return;
    }
    ipc_unimplemented(s, req, resp);
}

/* Vibration: accepted and dropped until there is a host controller to rumble. */
static const SimpleCommand vibration_list_simple[] = {
    {0, SIMPLE_OK, 0, "ActivateVibrationDevice(handle)"},
};

static void vibration_list(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!IPC_SIMPLE(vibration_list_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

static const SimpleCommand hid_simple[] = {
    {100, SIMPLE_OK, 0, "SetSupportedNpadStyleSet(style set, aruid)"},
    {102, SIMPLE_OK, 0, "SetSupportedNpadIdType(aruid, X: ids)"},
    {109, SIMPLE_OK, 0, "ActivateNpadWithRevision(revision, aruid)"},
    {206, SIMPLE_OK, 0, "SendVibrationValues(aruid, X: handles, X: values): dropped"},
};

void svc_hid(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* CreateAppletResource(aruid) -> IAppletResource */
        ipc_return_object(resp, ipc_service_new("hid:IAppletResource", applet_resource, NULL));
        return;
    }
    if (req->cmd == 203) {  /* CreateActiveVibrationDeviceList -> IActiveVibrationDeviceList */
        ipc_return_object(resp, ipc_service_new("hid:IActiveVibrationDeviceList", vibration_list, NULL));
        return;
    }
    if (req->cmd == 200) {  /* GetVibrationDeviceInfo(handle) -> {u32 type, u32 position} */
        uint32_t handle = IPC_IN(uint32_t, 0);  /* bytes: style, npad id, device index (0 left, 1 right) */
        IPC_OUT(uint32_t, 0) = 1;  /* linear resonant actuator, as in every Joy-Con and Pro Controller */
        IPC_OUT(uint32_t, 4) = (handle >> 16) & 0xFF;
        ipc_out_size(resp, 8);
        return;
    }
    if (!IPC_SIMPLE(hid_simple, req, resp)) ipc_unimplemented(s, req, resp);
}
