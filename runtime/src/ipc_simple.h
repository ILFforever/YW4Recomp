/* Table-driven answers for IPC commands that need no logic: "succeed", "return this
 * constant", or "return an event nothing signals" (decision 0014). Most of a service's
 * commands are like that; the handler keeps real code only for the rest. */
#ifndef IPC_SIMPLE_H
#define IPC_SIMPLE_H

#include "ipc.h"

typedef enum { SIMPLE_OK, SIMPLE_U8, SIMPLE_U32, SIMPLE_U64, SIMPLE_EVENT } SimpleKind;

typedef struct {
    uint32_t cmd;
    SimpleKind kind;
    uint64_t value;
    const char *name;  /* Switchbrew's name for the command: documentation only */
} SimpleCommand;

/* Answers req from the table; 0 if the command isn't in it. */
static inline int ipc_simple(const SimpleCommand *table, size_t n, IpcRequest *req, IpcResponse *resp) {
    for (size_t i = 0; i < n; i++) {
        const SimpleCommand *c = &table[i];
        if (c->cmd != req->cmd) continue;
        switch (c->kind) {
        case SIMPLE_OK: break;
        case SIMPLE_U8: IPC_OUT(uint8_t, 0) = (uint8_t)c->value; ipc_out_size(resp, 1); break;
        case SIMPLE_U32: IPC_OUT(uint32_t, 0) = (uint32_t)c->value; ipc_out_size(resp, 4); break;
        case SIMPLE_U64: IPC_OUT(uint64_t, 0) = c->value; ipc_out_size(resp, 8); break;
        case SIMPLE_EVENT: {
            uint32_t h;
            yw4_event_new(&h);
            resp->copy_handles[resp->ncopy++] = h;
            break;
        }
        }
        return 1;
    }
    return 0;
}

#define IPC_SIMPLE(table, req, resp) ipc_simple(table, sizeof table / sizeof *table, req, resp)

#endif
