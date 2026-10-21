/* Small services (decision 0014). Commands are added as the game first uses them; Switchbrew
 * has the ids and names. */
#include "ipc_simple.h"

/* ---- aoc:u: add-on content. None is installed. */

static const SimpleCommand aoc_simple[] = {
};

void svc_aoc_u(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!IPC_SIMPLE(aoc_simple, req, resp)) ipc_unimplemented(s, req, resp);
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
