/* nifm:u: network connection management (Switchbrew "Network_Interface_services").
 *
 * The recompiled game is offline: no network is ever available. Online play (NEX/Pia,
 * docs/recon.md) stays out of scope for now; a request for a connection is refused the way
 * the Switch refuses it with no network set up.
 */
#include "ipc_simple.h"

static void general_service(Service *s, IpcRequest *req, IpcResponse *resp) { ipc_unimplemented(s, req, resp); }

void svc_nifm(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 4 || req->cmd == 5) {  /* CreateGeneralServiceOld / CreateGeneralService(pid) */
        ipc_return_object(resp, ipc_service_new("nifm:IGeneralService", general_service, NULL));
        return;
    }
    ipc_unimplemented(s, req, resp);
}
