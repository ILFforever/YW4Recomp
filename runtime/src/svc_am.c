/* appletOE (the applet manager: the game's link to the system), apm (performance mode) and
 * lm (logging) (decision 0014). Commands are added as the game first uses them; every
 * other one stops (ipc_unimplemented). Command ids and names: Switchbrew
 * "Applet_Manager_services", "PPC_services", "Log_services"; LAS Recomp's services/am.c
 * (MIT) as the cross-check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc_simple.h"
#include "savefs.h"

/* ---- the message queue the system uses to tell the application things */

static KEvent *message_event;
static uint32_t message_handle;
static uint32_t messages[16];
static int nmessages;

static void am_init(void) {
    if (message_event) return;
    message_event = yw4_event_new(&message_handle);
    messages[nmessages++] = 15;  /* FocusStateChanged: the game starts in focus */
    yw4_event_signal(message_event);
}

#define AM_NO_DATA_IN_CHANNEL 0x480u  /* am (module 128), description 2 */
#define AM_NO_MESSAGE 0x680u          /* am ResultNoMessage (module 128, description 3) */
#define AM_OUT_OF_BOUNDS 0x3EE80u     /* am, description 503 */
#define APPLET_RESOURCE_USER_ID 0x81ull  /* any non-zero id: hid and others key their state on it */

static const SimpleCommand common_state_getter_simple[] = {
    {5, SIMPLE_U8, 1, "GetOperationMode: console (docked), so the game renders for a TV"},
    {9, SIMPLE_U8, 1, "GetCurrentFocusState: in focus"},
};

static void common_state_getter(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 0:  /* GetEventHandle: signalled while messages are queued */
        resp->copy_handles[resp->ncopy++] = message_handle;
        return;
    case 1:  /* ReceiveMessage */
        if (!nmessages) {
            resp->result = AM_NO_MESSAGE;
            return;
        }
        IPC_OUT(uint32_t, 0) = messages[0];
        memmove(messages, messages + 1, (size_t)--nmessages * sizeof *messages);
        if (!nmessages) yw4_event_clear(message_event);
        ipc_out_size(resp, 4);
        return;
    }
    if (!IPC_SIMPLE(common_state_getter_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

static const SimpleCommand self_controller_simple[] = {
    {11, SIMPLE_OK, 0, "SetOperationModeChangedNotification"},
    {13, SIMPLE_OK, 0, "SetFocusHandlingMode"},
    {14, SIMPLE_OK, 0, "SetPerformanceModeChangedNotification"},
    {16, SIMPLE_OK, 0, "SetOutOfFocusSuspendingEnabled"},
    {91, SIMPLE_EVENT, 0, "GetAccumulatedSuspendedTickChangedEvent: never suspended"},
};

static void self_controller(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!IPC_SIMPLE(self_controller_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

static const SimpleCommand window_controller_simple[] = {
    {1, SIMPLE_U64, APPLET_RESOURCE_USER_ID, "GetAppletResourceUserId"},
    {10, SIMPLE_OK, 0, "AcquireForegroundRights"},
};

static void window_controller(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!IPC_SIMPLE(window_controller_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

/* ---- storages: blocks of bytes passed between applets (launch parameters) */

typedef struct {
    uint8_t *data;
    uint64_t size;
} Storage;

static void storage_accessor(Service *s, IpcRequest *req, IpcResponse *resp) {
    Storage *st = s->state;
    switch (req->cmd) {
    case 0:  /* GetSize -> i64 */
        IPC_OUT(uint64_t, 0) = st->size;
        ipc_out_size(resp, 8);
        return;
    case 11: {  /* Read(i64 offset, B buffer) */
        uint64_t off = IPC_IN(uint64_t, 0);
        IpcBuffer out = ipc_out_buffer(req, 0);
        if (off > st->size || out.size > st->size - off) {
            resp->result = AM_OUT_OF_BOUNDS;
            return;
        }
        memcpy(GPTR(out.addr), st->data + off, out.size);
        return;
    }
    }
    ipc_unimplemented(s, req, resp);
}

static void storage(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* Open -> IStorageAccessor */
        ipc_return_object(resp, ipc_service_new("IStorageAccessor", storage_accessor, s->state));
        return;
    }
    ipc_unimplemented(s, req, resp);
}

static Service *storage_new(const void *data, uint64_t size) {
    Storage *st = calloc(1, sizeof *st);
    if (!st || !(st->data = malloc(size))) exit(3);
    memcpy(st->data, data, size);
    st->size = size;
    return ipc_service_new("IStorage", storage, st);
}

static const SimpleCommand application_functions_simple[] = {
    {21, SIMPLE_U64, 0x616A, "GetDesiredLanguage: \"ja\""},
    {22, SIMPLE_OK, 0, "SetTerminateResult"},
    {40, SIMPLE_OK, 0, "NotifyRunning / FinishStartupLogo: the boot logo goes away"},
    /* LanguageCode: the language's tag as up to 8 chars in a u64; the game only has Japanese */
};

static void application_functions(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 1) {  /* PopLaunchParameter(u32 kind) -> IStorage */
        uint32_t kind = IPC_IN(uint32_t, 0);
        if (kind != 2) {  /* 1 = user channel: nothing was passed */
            resp->result = AM_NO_DATA_IN_CHANNEL;
            return;
        }
        /* 2 = the user picked at launch (Switchbrew "PreselectedUser"): magic, "selected",
         * the user id; 0x88 bytes */
        uint8_t p[0x88] = {0};
        uint32_t magic = 0xC79497CA;
        memcpy(p, &magic, 4);
        p[4] = 1;
        yw4_account_user_id(p + 8);
        ipc_return_object(resp, storage_new(p, sizeof p));
        return;
    }
    if (req->cmd == 20) {  /* EnsureSaveData(uid) -> u64 size still needed: creates the save if missing */
        resp->result = savefs_open(YW4_PROGRAM_ID, req->data, 1, NULL);
        IPC_OUT(uint64_t, 0) = 0;
        ipc_out_size(resp, 8);
        return;
    }
    if (req->cmd == 50) {  /* GetPseudoDeviceId -> Uuid: a fixed id stands in for this "console" */
        static const uint8_t id[16] = {'y', 'w', '4', 'r', 'e', 'c', 'o', 'm', 'p', 'd', 'e', 'v', 'i', 'd', 0, 1};
        memcpy(resp->data, id, 16);
        ipc_out_size(resp, 16);
        return;
    }
    if (!IPC_SIMPLE(application_functions_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

static void application_proxy(Service *s, IpcRequest *req, IpcResponse *resp) {
    static const struct {
        uint32_t cmd;
        const char *name;
        ServiceHandler handler;
    } interfaces[] = {
        {0, "ICommonStateGetter", common_state_getter},
        {1, "ISelfController", self_controller},
        {2, "IWindowController", window_controller},
        {3, "IAudioController", ipc_unimplemented},
        {4, "IDisplayController", ipc_unimplemented},
        {10, "IProcessWindingController", ipc_unimplemented},
        {11, "ILibraryAppletCreator", ipc_unimplemented},
        {20, "IApplicationFunctions", application_functions},
        {1000, "IDebugFunctions", ipc_unimplemented},
    };
    for (size_t i = 0; i < sizeof interfaces / sizeof *interfaces; i++)
        if (interfaces[i].cmd == req->cmd) {
            ipc_return_object(resp, ipc_service_new(interfaces[i].name, interfaces[i].handler, NULL));
            return;
        }
    ipc_unimplemented(s, req, resp);
}

void svc_appletOE(Service *s, IpcRequest *req, IpcResponse *resp) {
    am_init();
    if (req->cmd == 0) {  /* OpenApplicationProxy(pid, process handle) */
        ipc_return_object(resp, ipc_service_new("IApplicationProxy", application_proxy, NULL));
        return;
    }
    ipc_unimplemented(s, req, resp);
}

/* ---- apm: performance modes (CPU/GPU clocks), which mean nothing on the host */

static const SimpleCommand apm_session_simple[] = {
    {0, SIMPLE_OK, 0, "SetPerformanceConfiguration(mode, configuration)"},
};

static void apm_session(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!IPC_SIMPLE(apm_session_simple, req, resp)) ipc_unimplemented(s, req, resp);
}

void svc_apm(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* OpenSession */
        ipc_return_object(resp, ipc_service_new("apm:ISession", apm_session, NULL));
        return;
    }
    ipc_unimplemented(s, req, resp);
}

/* ---- lm: the sdk's log output (nn::diag) */

static void lm_logger(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd != 0) {  /* 0 = Log(A buffer) */
        ipc_unimplemented(s, req, resp);
        return;
    }
    if (!req->na && !req->nx) return;
    const IpcBuffer *b = req->na ? &req->a[0] : &req->x[0];
    /* A log packet: a 0x18-byte header, then (key, length, value) chunks. */
    const uint8_t *p = GPTR(b->addr), *end = p + b->size;
    p += 0x18;
    char line[1024];
    int n = 0;
    while (p + 2 <= end && n < (int)sizeof line) {
        uint8_t key = p[0], len = p[1];
        p += 2;
        if (p + len > end) break;
        if (key == 2)  /* the message text */
            n += snprintf(line + n, sizeof line - (size_t)n, "%.*s", len, (const char *)p);
        p += len;
    }
    if (n) fprintf(stderr, "[game lm] %s%s", line, line[n - 1] == '\n' ? "" : "\n");
}

void svc_lm(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* OpenLogger(pid) */
        ipc_return_object(resp, ipc_service_new("lm:ILogger", lm_logger, NULL));
        return;
    }
    ipc_unimplemented(s, req, resp);
}
