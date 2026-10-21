/* fsp-srv: the filesystem service (decision 0014). The game's data is the RomFS image
 * (romfs.c); save data lives in host folders (savefs.c); the rest comes as the game first
 * uses it. Command ids:
 * Switchbrew "Filesystem_services"; LAS Recomp's services/fs.c (MIT) as the cross-check. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ipc_simple.h"
#include "romfs.h"
#include "savefs.h"

#define FS_OUT_OF_RANGE 0x3EA02u  /* fs ResultOutOfRange (module 2, description 3001) */

/* The receive buffer of a Read: a B (or C) descriptor. */
static const IpcBuffer *out_buffer(const IpcRequest *req) {
    if (req->nb && req->b[0].size) return &req->b[0];
    if (req->nc && req->c[0].size) return &req->c[0];
    return req->nb ? &req->b[0] : req->nc ? &req->c[0] : NULL;
}

/* IStorage over the RomFS image */
static void romfs_storage(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 0: {  /* Read(offset, size) -> B buffer */
        uint64_t offset = IPC_IN(uint64_t, 0), size = IPC_IN(uint64_t, 8);
        const IpcBuffer *b = out_buffer(req);
        if (!b || size > b->size) size = b ? b->size : 0;
        if (romfs_read(offset, GPTR(b ? b->addr : 0), size)) {
            fprintf(stderr, "yw4: fs: RomFS read [%#" PRIx64 ", +%#" PRIx64 ") failed\n", offset, size);
            resp->result = FS_OUT_OF_RANGE;
        }
        return;
    }
    case 4:  /* GetSize */
        IPC_OUT(uint64_t, 0) = romfs_size();
        ipc_out_size(resp, 8);
        return;
    default:
        ipc_unimplemented(s, req, resp);
    }
}

static const SimpleCommand fsp_simple[] = {
    {1, SIMPLE_OK, 0, "SetCurrentProcess"},
    {1005, SIMPLE_U32, 0, "GetGlobalAccessLogMode: off"},
};


/* SaveDataAttribute (0x40): program id, user id, system save id, type, rank, index */
static uint64_t attribute_program(const uint8_t *a) {
    uint64_t id;
    memcpy(&id, a, 8);
    return id ? id : YW4_PROGRAM_ID;  /* 0 means this game */
}

void svc_fsp_srv(Service *s, IpcRequest *req, IpcResponse *resp) {
    switch (req->cmd) {
    case 22:  /* CreateSaveDataFileSystem(attribute, creation info, meta info) */
        resp->result = savefs_open(attribute_program(req->data), req->data + 8, 1, NULL);
        return;
    case 51: {  /* OpenSaveDataFileSystem(u8 space id, attribute at +8) -> IFileSystem */
        Service *fs = NULL;
        resp->result = savefs_open(attribute_program(req->data + 8), req->data + 16, 0, &fs);
        if (fs) ipc_return_object(resp, fs);
        return;
    }
    case 200:  /* OpenDataStorageByCurrentProcess: the game's RomFS */
        ipc_return_object(resp, ipc_service_new("IStorage (RomFS)", romfs_storage, NULL));
        return;
    }
    if (!IPC_SIMPLE(fsp_simple, req, resp)) ipc_unimplemented(s, req, resp);
}
