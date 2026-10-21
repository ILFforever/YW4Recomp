/* hid: controllers (IHidServer, Switchbrew "HID_services"; decision 0014).
 *
 * The sdk's nn::hid reads input from a 0x40000-byte shared memory block the service keeps
 * up to date (Switchbrew "HID_Shared_Memory"), and sends settings and vibration through
 * IPC. For now the block stays zeroed: no controller has reported anything yet. It gets
 * filled from host input (SDL3) when the window comes (decision 0017).
 */
#include "ipc_simple.h"

#define HID_SHARED_MEMORY_SIZE 0x40000

static uint32_t shared_memory_handle;

static void applet_resource(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (req->cmd == 0) {  /* GetSharedMemoryHandle */
        if (!shared_memory_handle) yw4_shared_memory_new(HID_SHARED_MEMORY_SIZE, NULL, &shared_memory_handle);
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
