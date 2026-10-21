/* IPC between the recompiled sdk and native system services (decision 0014).
 *
 * The sdk sends requests with svcSendSyncRequest: the message sits in the calling thread's
 * thread-local region (Switchbrew "IPC_Marshalling": a HIPC header with handle and buffer
 * descriptors, then a CMIF payload "SFCI" + command id + arguments). ipc.c decodes it into
 * an IpcRequest, the service's handler fills an IpcResponse, and ipc.c writes the reply
 * ("SFCO" + result + output) back over the request.
 *
 * Unknown services and commands stop the program with their name and number, so the next
 * thing to implement is obvious. YW4_IPC_STUB=1 instead answers them with "success, zeroed
 * output" and logs each one: for surveying what the game needs, not for playing.
 */
#ifndef IPC_H
#define IPC_H

#include <stdint.h>

#include "hle.h"
#include "kernel.h"

typedef struct {
    uint64_t addr, size;
} IpcBuffer;

typedef struct {
    uint32_t type;                 /* 4 request, 5 control, 2 close (+2: with context) */
    uint32_t cmd;                  /* CMIF command id */
    const uint8_t *data;           /* arguments after the 16-byte CMIF header */
    uint32_t data_size;
    uint64_t pid;                  /* if the client sent its process id */
    uint32_t copy_handles[8], move_handles[8];
    int ncopy, nmove;
    IpcBuffer x[8], a[8], b[8], c[8];  /* pointer (X), send (A), receive (B), receive-list (C) */
    int nx, na, nb, nc;
} IpcRequest;

typedef struct Service Service;

typedef struct {
    uint32_t result;
    uint8_t data[0x200];
    uint32_t data_size;
    uint32_t copy_handles[8], move_handles[8];
    int ncopy, nmove;
    Service *objects[8];           /* new interfaces to return: sessions, or domain objects */
    int nobjects;
} IpcResponse;

typedef void (*ServiceHandler)(Service *s, IpcRequest *req, IpcResponse *resp);

struct Service {
    const char *name;
    ServiceHandler handler;
    void *state;
    int refs;                      /* sessions and domain slots holding it */
};

/* Argument/output helpers for handlers. */
#define IPC_IN(type, off) (*(const type *)(req->data + (off)))
#define IPC_OUT(type, off) (*(type *)(resp->data + (off)))
static inline void ipc_out_size(IpcResponse *resp, uint32_t n) { if (n > resp->data_size) resp->data_size = n; }

Service *ipc_service_new(const char *name, ServiceHandler handler, void *state);
void ipc_return_object(IpcResponse *resp, Service *s);
/* A named service from the registry (ipc.c), or NULL / a stub (YW4_IPC_STUB=1). */
Service *ipc_service_lookup(const char *name);
/* The default branch of every handler: stops, or logs and succeeds in stub mode. */
void ipc_unimplemented(Service *s, IpcRequest *req, IpcResponse *resp);

/* Kernel side (kernel.c): ConnectToNamedPort, SendSyncRequest(WithUserBuffer), CloseHandle. */
uint32_t ipc_connect_named_port(const char *name, uint32_t *handle);
uint32_t ipc_send_request(uint32_t handle, uint64_t message);
int ipc_close_handle(uint32_t handle);  /* 1 if it was a session */

/* Services (svc_*.c) */
void svc_sm(Service *s, IpcRequest *req, IpcResponse *resp);
void svc_appletOE(Service *s, IpcRequest *req, IpcResponse *resp);
void svc_apm(Service *s, IpcRequest *req, IpcResponse *resp);
void svc_lm(Service *s, IpcRequest *req, IpcResponse *resp);
void svc_aoc_u(Service *s, IpcRequest *req, IpcResponse *resp);
void svc_pctl(Service *s, IpcRequest *req, IpcResponse *resp);
void svc_time(Service *s, IpcRequest *req, IpcResponse *resp);

#endif
