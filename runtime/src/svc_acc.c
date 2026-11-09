/* acc:u0: user accounts (IAccountServiceForApplication, Switchbrew "Account services").
 *
 * One local user, already selected when the game starts (am's PreselectedUser launch
 * parameter carries the same id). Save data is keyed by this id, so it must never change.
 */
#include <stdlib.h>
#include <string.h>

#include "ipc_simple.h"

/* Any fixed non-zero 128-bit id ("YW4 RECOMP USER" in ASCII, then 1). */
static const uint8_t user_id[16] = {'Y', 'W', '4', ' ', 'R', 'E', 'C', 'O', 'M', 'P', ' ', 'U', 'S', 'E', 'R', 1};

void yw4_account_user_id(uint8_t out[16]) { memcpy(out, user_id, 16); }

/* IManagerForApplication: the user's Nintendo Account (online), which this user doesn't have */
static void manager(Service *s, IpcRequest *req, IpcResponse *resp) { ipc_unimplemented(s, req, resp); }

/* The user's nickname, as other players see it in local play: YW4_NICKNAME (settings.ini), else the Windows
 * user's name; at most 10 characters, as on the Switch, in UTF-8. */
static void nickname(uint8_t out[0x21]) {
    const char *n = getenv("YW4_NICKNAME");
    if (!n || !*n) n = getenv("USERNAME");
    if (!n || !*n) n = "Player";
    memset(out, 0, 0x21);
    size_t i = 0, chars = 0;
    while (n[i] && chars < 10) {
        size_t len = 1;
        unsigned char b = (unsigned char)n[i];
        if (b >= 0xF0) len = 4; else if (b >= 0xE0) len = 3; else if (b >= 0xC0) len = 2;
        if (i + len > 0x20) break;
        i += len, chars++;
    }
    memcpy(out, n, i);
}

/* ProfileBase (0x38): Uid, u64 last edit (POSIX), nickname[0x20] */
static void profile_base(uint8_t *p) {
    uint8_t name[0x21];
    memset(p, 0, 0x38);
    memcpy(p, user_id, 16);
    uint64_t when = 1735689600;  /* 2025-01-01 */
    memcpy(p + 0x10, &when, 8);
    nickname(name);
    memcpy(p + 0x18, name, 0x20);
}

/* IProfile: one user's name and picture (2026-11-10: local play asks the nickname, nn::account::GetNickname) */
static void profile(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 0: {  /* Get -> ProfileBase, UserData (out buffer 0x80) */
        IpcBuffer out = ipc_out_buffer(req, 0);
        if (out.addr && out.size) memset(GPTR(out.addr), 0, out.size < 0x80 ? out.size : 0x80);
        profile_base(resp->data);
        ipc_out_size(resp, 0x38);
        return;
    }
    case 1:  /* GetBase -> ProfileBase */
        profile_base(resp->data);
        ipc_out_size(resp, 0x38);
        return;
    case 10:  /* GetImageSize -> u32: no picture */
        IPC_OUT(uint32_t, 0) = 0;
        ipc_out_size(resp, 4);
        return;
    case 11:  /* LoadImage(out buffer) -> u32 size */
        IPC_OUT(uint32_t, 0) = 0;
        ipc_out_size(resp, 4);
        return;
    }
    ipc_unimplemented(s, req, resp);
}

static const SimpleCommand acc_simple[] = {
    {140, SIMPLE_OK, 0, "InitializeApplicationInfo(pid)"},
    {150, SIMPLE_U8, 0, "IsUserAccountSwitchLocked: no"},
};

void svc_acc(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 101) {  /* GetBaasAccountManagerForApplication(uid) -> IManagerForApplication */
        ipc_return_object(resp, ipc_service_new("acc:IManagerForApplication", manager, NULL));
        return;
    }
    if (req->cmd == 5) {  /* GetProfile(uid) -> IProfile */
        ipc_return_object(resp, ipc_service_new("acc:IProfile", profile, NULL));
        return;
    }
    if (req->cmd == 0) {  /* GetUserCount -> s32 */
        IPC_OUT(int32_t, 0) = 1;
        ipc_out_size(resp, 4);
        return;
    }
    if (req->cmd == 2 || req->cmd == 3) {  /* ListAllUsers / ListOpenUsers -> out buffer of Uid */
        IpcBuffer out = ipc_out_buffer(req, 0);
        if (out.addr && out.size) {
            memset(GPTR(out.addr), 0, out.size);
            if (out.size >= 16) memcpy(GPTR(out.addr), user_id, 16);
        }
        return;
    }
    if (req->cmd == 4) {  /* GetLastOpenedUser -> Uid */
        memcpy(resp->data, user_id, 16);
        ipc_out_size(resp, 16);
        return;
    }
    if (req->cmd == 1) {  /* GetUserExistence(uid) -> bool */
        IPC_OUT(uint8_t, 0) = memcmp(req->data, user_id, 16) == 0;
        ipc_out_size(resp, 1);
        return;
    }
    if (!IPC_SIMPLE(acc_simple, req, resp)) ipc_unimplemented(s, req, resp);
}
