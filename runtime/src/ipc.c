/* IPC message codec, sessions, domains and the service registry (decision 0014).
 *
 * Message layout (Switchbrew "IPC_Marshalling"), 32-bit words at the start of the TLR:
 *   word 0  type (bits 0-15), number of X / A / B / W descriptors (16-19, 20-23, 24-27, 28-31)
 *   word 1  raw data size in words (0-9), C descriptor mode (10-13), handle descriptor (31)
 *   [handle descriptor: send PID (0), copy count (1-4), move count (5-8); PID; handles]
 *   X descriptors (2 words), A, B, W descriptors (3 words each)
 *   raw data, starting 16-byte aligned: [domain header], "SFCI", version, command, token, args
 *   C descriptors after the raw data
 * The reply replaces the request: type 0, optional handles, then "SFCO", 0, result, 0, output.
 * LAS Recomp's runtime/ipc.c (MIT) was the cross-check for this layout.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "ipc.h"

#define R_OK 0u
#define R_NOT_FOUND 0xF201u
#define R_INVALID_HANDLE 0xE401u

/* ---- registry: every native service, by the name sm:GetServiceHandle uses */

static const struct {
    const char *name;
    ServiceHandler handler;
} registry[] = {
    {"sm:", svc_sm},
    {"appletOE", svc_appletOE},
    {"apm", svc_apm},
    {"lm", svc_lm},
    {"aoc:u", svc_aoc_u},
    {"pctl", svc_pctl},
    {"pctl:s", svc_pctl},  /* the same interface with other access rights */
    {"pctl:a", svc_pctl},
    {"pctl:r", svc_pctl},
    {"time:u", svc_time},
    {"time:a", svc_time},
    {"time:s", svc_time},
    {"fsp-srv", svc_fsp_srv},
};

static int stub_mode(void) {
    static int on = -1;
    if (on < 0) {
        const char *s = getenv("YW4_IPC_STUB");
        on = s && *s && *s != '0';
    }
    return on;
}

Service *ipc_service_new(const char *name, ServiceHandler handler, void *state) {
    Service *s = calloc(1, sizeof *s);
    if (!s) exit(3);
    s->name = name;
    s->handler = handler;
    s->state = state;
    return s;
}

void ipc_unimplemented(Service *s, IpcRequest *req, IpcResponse *resp) {
    if (!stub_mode()) {
        fprintf(stderr, "yw4: unimplemented IPC command %u on %s (%u bytes of arguments, X%d A%d B%d C%d, "
                        "%d copy / %d move handles)\n", req->cmd, s->name, req->data_size, req->nx, req->na, req->nb,
                req->nc, req->ncopy, req->nmove);
        yw4_stop();
    }
    fprintf(stderr, "yw4: [stub] %s command %u -> success, zeroed output\n", s->name, req->cmd);
    ipc_out_size(resp, 0x40);
}

Service *ipc_service_lookup(const char *name) {
    for (size_t i = 0; i < sizeof registry / sizeof *registry; i++)
        if (!strcmp(registry[i].name, name)) return ipc_service_new(registry[i].name, registry[i].handler, NULL);
    if (!stub_mode()) return NULL;
    char *copy = malloc(strlen(name) + 1);
    strcpy(copy, name);
    fprintf(stderr, "yw4: [stub] service %s\n", name);
    return ipc_service_new(copy, ipc_unimplemented, NULL);
}

void ipc_return_object(IpcResponse *resp, Service *s) { resp->objects[resp->nobjects++] = s; }

/* ---- sessions */

typedef struct {
    Service *objects[256];         /* object id -> interface; 0 is never used */
} Domain;

typedef struct {
    KObject obj;
    Service *svc;
    Domain *domain;                /* set once converted; shared by clones */
} Session;

static uint32_t session_new(Service *s, Domain *domain) {
    Session *k = calloc(1, sizeof *k);
    if (!k) exit(3);
    k->obj.type = KOBJ_SESSION;
    k->svc = s;
    k->domain = domain;
    s->refs++;
    return yw4_handle_new(&k->obj);
}

static void service_release(Service *s) {
    if (s && --s->refs == 0) free(s);
}

uint32_t ipc_connect_named_port(const char *name, uint32_t *handle) {
    if (strcmp(name, "sm:") != 0) {  /* the only named port an application uses */
        fprintf(stderr, "yw4: ConnectToNamedPort(\"%s\"): unknown port\n", name);
        return R_NOT_FOUND;
    }
    *handle = session_new(ipc_service_lookup("sm:"), NULL);
    return R_OK;
}

static Session *session_get(uint32_t handle) {
    KObject *o = yw4_handle_get(handle);
    return o && o->type == KOBJ_SESSION ? (Session *)o : NULL;
}

int ipc_close_handle(uint32_t handle) {
    Session *k = session_get(handle);
    if (!k) return 0;
    yw4_handle_close(handle);
    if (!k->domain) service_release(k->svc);  /* a domain's objects are closed one by one */
    free(k);
    return 1;
}

/* ---- decoding */

static uint64_t abw_address(const uint32_t *w) {
    return w[1] | (uint64_t)(w[2] >> 28) << 32 | (uint64_t)((w[2] >> 2) & 7) << 36;
}

/* Fills req from the message; returns the raw data region (where padding starts). */
static uint32_t *decode(uint32_t *msg, IpcRequest *req, uint32_t *raw_words) {
    req->type = msg[0] & 0xFFFF;
    int nx = (msg[0] >> 16) & 0xF, na = (msg[0] >> 20) & 0xF, nb = (msg[0] >> 24) & 0xF, nw = (msg[0] >> 28) & 0xF;
    *raw_words = msg[1] & 0x3FF;
    uint32_t cmode = (msg[1] >> 10) & 0xF;
    uint32_t *p = msg + 2;
    if (msg[1] & 0x80000000u) {
        uint32_t hd = *p++;
        if (hd & 1) {
            req->pid = p[0] | (uint64_t)p[1] << 32;
            p += 2;
        }
        req->ncopy = (hd >> 1) & 0xF;
        req->nmove = (hd >> 5) & 0xF;
        for (int i = 0; i < req->ncopy; i++) req->copy_handles[i] = *p++;
        for (int i = 0; i < req->nmove; i++) req->move_handles[i] = *p++;
    }
    for (int i = 0; i < nx; i++, p += 2) {
        req->x[i].addr = p[1] | (uint64_t)((p[0] >> 12) & 0xF) << 32 | (uint64_t)((p[0] >> 6) & 7) << 36;
        req->x[i].size = p[0] >> 16;
    }
    for (int i = 0; i < na; i++, p += 3) {
        req->a[i].size = p[0] | (uint64_t)((p[2] >> 24) & 0xF) << 32;
        req->a[i].addr = abw_address(p);
    }
    for (int i = 0; i < nb; i++, p += 3) {
        req->b[i].size = p[0] | (uint64_t)((p[2] >> 24) & 0xF) << 32;
        req->b[i].addr = abw_address(p);
    }
    p += 3 * nw;  /* exchange buffers: none seen yet */
    req->nx = nx, req->na = na, req->nb = nb;
    if (cmode >= 2) {
        uint32_t *c = p + *raw_words;
        req->nc = cmode == 2 ? 1 : (int)cmode - 2;
        for (int i = 0; i < req->nc && i < 8; i++) {
            req->c[i].addr = c[2 * i] | (uint64_t)(c[2 * i + 1] & 0xFFFF) << 32;
            req->c[i].size = c[2 * i + 1] >> 16;
        }
    }
    return p;
}

/* ---- control messages (on the session itself, not a service command) */

static void control(Session *k, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 0:  /* ConvertCurrentObjectToDomain -> object id */
        k->domain = calloc(1, sizeof *k->domain);
        if (!k->domain) exit(3);
        k->domain->objects[1] = k->svc;
        IPC_OUT(uint32_t, 0) = 1;
        ipc_out_size(resp, 4);
        return;
    case 2: case 4:  /* CloneCurrentObject(Ex) -> a new session to the same object */
        resp->move_handles[resp->nmove++] = session_new(k->svc, k->domain);
        return;
    case 3:  /* QueryPointerBufferSize */
        IPC_OUT(uint16_t, 0) = 0x8000;
        ipc_out_size(resp, 4);
        return;
    default:
        fprintf(stderr, "yw4: unimplemented IPC control command %u on %s\n", req->cmd, k->svc->name);
        exit(3);
    }
}

/* ---- one request */

uint32_t ipc_send_request(uint32_t handle, uint64_t message) {
    Session *k = session_get(handle);
    if (!k) {
        fprintf(stderr, "yw4: SendSyncRequest on handle %#x, which is not a session\n", handle);
        return R_INVALID_HANDLE;
    }
    uint32_t *msg = GPTR(message);
    IpcRequest req = {0};
    IpcResponse resp = {0};
    uint32_t raw_words;
    uint32_t *raw = decode(msg, &req, &raw_words);
    uint8_t *payload = (uint8_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    uint32_t base = req.type & ~2u;  /* 6/7 = 4/5 with a context token */

    if (req.type == 2) {  /* close the session */
        ipc_close_handle(handle);
        return R_OK;
    }
    int domain = 0;
    Service *target = k->svc;
    if (base == 4 && k->domain) {
        domain = 1;
        uint8_t dcmd = payload[0];
        uint32_t object = *(uint32_t *)(payload + 4);
        if (object >= 256 || !k->domain->objects[object]) {
            fprintf(stderr, "yw4: IPC to missing domain object %u on %s\n", object, k->svc->name);
            exit(3);
        }
        if (dcmd == 2) {  /* close this domain object */
            service_release(k->domain->objects[object]);
            k->domain->objects[object] = NULL;
            goto reply;
        }
        target = k->domain->objects[object];
        payload += 16;
    }
    if (memcmp(payload, "SFCI", 4) != 0) {
        fprintf(stderr, "yw4: IPC request to %s without SFCI header (type %u)\n", target->name, req.type);
        exit(3);
    }
    req.cmd = *(uint32_t *)(payload + 8);
    req.data = payload + 16;
    req.data_size = raw_words * 4 > 32 ? raw_words * 4 - 32 : 0;
    static int trace = -1;
    if (trace < 0) {
        const char *e = getenv("YW4_TRACE_IPC");
        trace = e && *e && *e != '0';
    }
    if (base == 4)
        target->handler(target, &req, &resp);
    else if (base == 5)
        control(k, &req, &resp);
    else {
        fprintf(stderr, "yw4: IPC message type %u not supported\n", req.type);
        exit(3);
    }

reply:;
    uint32_t object_ids[8];
    int nobject_ids = 0;
    for (int i = 0; i < resp.nobjects; i++) {
        if (domain) {  /* a new object in the same domain */
            int id = 1;
            while (id < 256 && k->domain->objects[id]) id++;
            if (id == 256) {
                fprintf(stderr, "yw4: IPC domain of %s is full\n", k->svc->name);
                exit(3);
            }
            k->domain->objects[id] = resp.objects[i];
            resp.objects[i]->refs++;
            object_ids[nobject_ids++] = (uint32_t)id;
        } else {  /* a new session */
            resp.move_handles[resp.nmove++] = session_new(resp.objects[i], NULL);
        }
    }

    memset(msg, 0, 0x100);
    int handles = resp.ncopy || resp.nmove;
    uint32_t payload_words = 4 + (resp.data_size + 3) / 4 + (domain ? 4 + (uint32_t)nobject_ids : 0);
    msg[0] = 0;
    msg[1] = (payload_words + 4) | (handles ? 0x80000000u : 0);  /* + up to 4 words of alignment */
    uint32_t *q = msg + 2;
    if (handles) {
        *q++ = (uint32_t)resp.ncopy << 1 | (uint32_t)resp.nmove << 5;
        for (int i = 0; i < resp.ncopy; i++) *q++ = resp.copy_handles[i];
        for (int i = 0; i < resp.nmove; i++) *q++ = resp.move_handles[i];
    }
    uint32_t *out = (uint32_t *)(((uintptr_t)q + 15) & ~(uintptr_t)15);
    if (domain) {
        out[0] = (uint32_t)nobject_ids;
        out += 4;
    }
    memcpy(out, "SFCO", 4);
    out[1] = 0;
    out[2] = resp.result;
    out[3] = 0;
    memcpy(out + 4, resp.data, resp.data_size);
    for (int i = 0; i < nobject_ids; i++) out[4 + (resp.data_size + 3) / 4 + i] = object_ids[i];
    if (trace)
        fprintf(stderr, "yw4: ipc %s cmd %u (%u bytes in) -> result %#x, %u bytes out, %d handles, %d objects\n",
                target->name, req.cmd, req.data_size, resp.result, resp.data_size, resp.ncopy + resp.nmove,
                resp.nobjects);
    return R_OK;
}
