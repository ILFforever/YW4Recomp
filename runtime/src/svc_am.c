/* appletOE (the applet manager: the game's link to the system), apm (performance mode) and
 * lm (logging) (decision 0014). Commands are added as the game first uses them; every
 * other one stops (ipc_unimplemented). Command ids and names: Switchbrew
 * "Applet_Manager_services", "PPC_services", "Log_services"; LAS Recomp's services/am.c
 * (MIT) as the cross-check. */
#include <stdio.h>
#include <string.h>

#include "ipc_simple.h"

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

#define AM_NO_MESSAGE 0x680u  /* am ResultNoMessage (module 128, description 3) */
#define APPLET_RESOURCE_USER_ID 0x81ull  /* any non-zero id: hid and others key their state on it */

static const SimpleCommand common_state_getter_simple[] = {
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
    {13, SIMPLE_OK, 0, "SetFocusHandlingMode"},
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

static const SimpleCommand application_functions_simple[] = {
    {22, SIMPLE_OK, 0, "SetTerminateResult"},
};

static void application_functions(Service *s, IpcRequest *req, IpcResponse *resp) {
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

/* ---- apm */

void svc_apm(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* OpenSession */
        ipc_return_object(resp, ipc_service_new("apm:ISession", ipc_unimplemented, NULL));
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
