/* sm: the service manager (decision 0014). Every other service is reached through it:
 * GetServiceHandle(name) returns a session to that service. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"

void svc_sm(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 0:  /* RegisterClient / Initialize(pid) */
        return;
    case 1: {  /* GetServiceHandle(name: 8 chars, zero padded) -> session */
        char name[9] = {0};
        memcpy(name, req->data, 8);
        Service *svc = ipc_service_lookup(name);
        if (!svc) {
            fprintf(stderr, "yw4: sm: no native service \"%s\" yet\n", name);
            yw4_stop();
        }
        ipc_return_object(resp, svc);
        return;
    }
    default:
        ipc_unimplemented(s, req, resp);
    }
}
