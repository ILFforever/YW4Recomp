/* acc:u0: user accounts (IAccountServiceForApplication, Switchbrew "Account services").
 *
 * One local user, already selected when the game starts (am's PreselectedUser launch
 * parameter carries the same id). Save data is keyed by this id, so it must never change.
 */
#include <string.h>

#include "ipc_simple.h"

/* Any fixed non-zero 128-bit id ("YW4 RECOMP USER" in ASCII, then 1). */
static const uint8_t user_id[16] = {'Y', 'W', '4', ' ', 'R', 'E', 'C', 'O', 'M', 'P', ' ', 'U', 'S', 'E', 'R', 1};

void yw4_account_user_id(uint8_t out[16]) { memcpy(out, user_id, 16); }

/* IManagerForApplication: the user's Nintendo Account (online), which this user doesn't have */
static void manager(Service *s, IpcRequest *req, IpcResponse *resp) { ipc_unimplemented(s, req, resp); }

static const SimpleCommand acc_simple[] = {
    {140, SIMPLE_OK, 0, "InitializeApplicationInfo(pid)"},
    {150, SIMPLE_U8, 0, "IsUserAccountSwitchLocked: no"},
};

void svc_acc(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 101) {  /* GetBaasAccountManagerForApplication(uid) -> IManagerForApplication */
        ipc_return_object(resp, ipc_service_new("acc:IManagerForApplication", manager, NULL));
        return;
    }
    if (req->cmd == 1) {  /* GetUserExistence(uid) -> bool */
        IPC_OUT(uint8_t, 0) = memcmp(req->data, user_id, 16) == 0;
        ipc_out_size(resp, 1);
        return;
    }
    if (!IPC_SIMPLE(acc_simple, req, resp)) ipc_unimplemented(s, req, resp);
}
