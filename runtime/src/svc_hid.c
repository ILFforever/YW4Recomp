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

#include "gbuffer.h"
#include "ipc_simple.h"
#include "settings.h"
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

/* ---- Input recording and replay (YW4_RECORD=0 turns recording off; YW4_REPLAY=<file> plays one)
 *
 * Every change of the physical input (buttons, sticks) and every capture key (F9/F10/F11/F12) is
 * written, with the vsync tick (60 per second since start), to work/inputs/rec_<time>.txt:
 *     s <tick> <buttons hex> <lx> <ly> <rx> <ry>
 *     k <tick> <F9|F10|F11|F12|F6|F1|UP|DOWN|LEFT|RIGHT>  (the last four: the settings panel's keys)
 * A replay feeds the same states back at the same ticks (instead of the keyboard and gamepad)
 * and presses the same capture keys, so a route a person played can be run again by a script.
 * The game's loading times vary a little from run to run, so long routes can drift. */
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

void gpu_request_trace(int draws);
void gpu_request_dump(void);
void shadow_toggle(void);

static FILE *rec_file;
static uint64_t rec_tick;  /* the tick of the latest update, for key notes from the window thread */
static HostLock rec_lock = HOST_LOCK_INIT;

static void rec_open(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    const char *on = getenv("YW4_RECORD");
    if (on && *on == '0') return;
    MKDIR("work");
    MKDIR("work/inputs");
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char when[32], path[256];
    strftime(when, sizeof when, "%Y%m%d_%H%M%S", &tmv);
    snprintf(path, sizeof path, "work/inputs/rec_%s.txt", when);
    rec_file = fopen(path, "w");
    if (rec_file) fprintf(stderr, "yw4: recording input in %s\n", path);
}

void yw4_hid_note_key(const char *name) {
    host_lock(&rec_lock);
    rec_open();
    if (rec_file) {
        fprintf(rec_file, "k %llu %s\n", (unsigned long long)rec_tick, name);
        fflush(rec_file);
    }
    host_unlock(&rec_lock);
}

typedef struct {
    uint64_t tick, buttons;
    int32_t sticks[4];
} RecState;
typedef struct {
    uint64_t tick;
    char name[8];
} RecKey;
static RecState *replay_states;
static RecKey *replay_keys;
static size_t n_states, n_keys, next_key;
static int replay_loaded;

static void replay_load(void) {
    replay_loaded = 1;
    const char *path = getenv("YW4_REPLAY");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "yw4: replay: cannot open %s\n", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long long tick, buttons;
        int lx, ly, rx, ry;
        char name[8];
        if (sscanf(line, "s %llu %llx %d %d %d %d", &tick, &buttons, &lx, &ly, &rx, &ry) == 6) {
            replay_states = realloc(replay_states, (n_states + 1) * sizeof *replay_states);
            replay_states[n_states++] = (RecState){tick, buttons, {lx, ly, rx, ry}};
        } else if (sscanf(line, "k %llu %7s", &tick, name) == 2) {
            replay_keys = realloc(replay_keys, (n_keys + 1) * sizeof *replay_keys);
            replay_keys[n_keys].tick = tick;
            snprintf(replay_keys[n_keys].name, sizeof replay_keys[n_keys].name, "%s", name);
            n_keys++;
        }
    }
    fclose(f);
    fprintf(stderr, "yw4: replay: %zu input states and %zu key presses from %s\n", n_states, n_keys, path);
}

/* The state a replay gives at `tick`; 0 if there is no replay. */
static int replay_state(uint64_t tick, uint64_t *buttons, int32_t sticks[4]) {
    if (!replay_loaded) replay_load();
    if (!n_states && !n_keys) return 0;
    /* After the last line the player takes over (e.g. a replay that only loads the save). */
    if (next_key == n_keys && (!n_states || tick > replay_states[n_states - 1].tick)) {
        static int handed_over;
        if (!handed_over) {
            handed_over = 1;
            fprintf(stderr, "yw4: replay: finished at tick %llu, input is live\n", (unsigned long long)tick);
            /* YW4_REPLAY_EXIT=1: a test run ends with its replay instead of idling */
            const char *e = getenv("YW4_REPLAY_EXIT");
            if (e && atoi(e)) {
                fprintf(stderr, "yw4: replay: done, exiting (YW4_REPLAY_EXIT)\n");
                exit(0);
            }
        }
        return 0;
    }
    static size_t cur;
    while (cur + 1 < n_states && replay_states[cur + 1].tick <= tick) cur++;
    if (n_states && replay_states[cur].tick <= tick) {
        *buttons = replay_states[cur].buttons;
        memcpy(sticks, replay_states[cur].sticks, 4 * sizeof(int32_t));
    } else {
        *buttons = 0;
        memset(sticks, 0, 4 * sizeof(int32_t));
    }
    for (; next_key < n_keys && replay_keys[next_key].tick <= tick; next_key++) {
        const char *k = replay_keys[next_key].name;
        fprintf(stderr, "yw4: replay: key %s at tick %llu\n", k, (unsigned long long)tick);
        if (!strcmp(k, "F11")) gpu_request_trace(450);
        else if (!strcmp(k, "F10")) gpu_request_dump();
        else if (!strcmp(k, "F12")) window_request_screenshot();
        else if (!strcmp(k, "F9")) shadow_toggle();
        else if (!strcmp(k, "F6")) gbuffer_toggle();
        else if (!strcmp(k, "F1")) settings_show(0);  /* the settings panel, and its keys while open */
        else if (!strcmp(k, "F2")) settings_show(1);  /* its controls page */
        else if (!strcmp(k, "UP")) settings_action(SETTINGS_UP);
        else if (!strcmp(k, "DOWN")) settings_action(SETTINGS_DOWN);
        else if (!strcmp(k, "LEFT")) settings_action(SETTINGS_LEFT);
        else if (!strcmp(k, "RIGHT")) settings_action(SETTINGS_RIGHT);
        else if (!strcmp(k, "ENTER")) settings_action(SETTINGS_ACCEPT);
        else if (!strcmp(k, "DELETE")) settings_action(SETTINGS_CLEAR);
        else if (!strcmp(k, "PAGE")) settings_action(SETTINGS_PAGE);
    }
    return 1;
}

void yw4_hid_update(uint64_t tick) {
    KSharedMemory *m = __atomic_load_n(&shared_memory, __ATOMIC_ACQUIRE);
    if (!m || !__atomic_load_n(&m->guest_addr, __ATOMIC_ACQUIRE)) return;
    uint64_t buttons;
    int32_t sticks[4];
    rec_tick = tick;
    if (!replay_state(tick, &buttons, sticks)) {
        window_input(&buttons, sticks);
        /* record changes of the physical input */
        static uint64_t last_buttons = ~0ull;
        static int32_t last_sticks[4];
        if (buttons != last_buttons || memcmp(sticks, last_sticks, sizeof last_sticks)) {
            host_lock(&rec_lock);
            rec_open();
            if (rec_file) {
                fprintf(rec_file, "s %llu %llx %d %d %d %d\n", (unsigned long long)tick, (unsigned long long)buttons,
                        sticks[0], sticks[1], sticks[2], sticks[3]);
                fflush(rec_file);
            }
            host_unlock(&rec_lock);
            last_buttons = buttons;
            memcpy(last_sticks, sticks, sizeof last_sticks);
        }
    }
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
