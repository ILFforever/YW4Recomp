/* fsp-srv: the filesystem service (decision 0014). The game's data is the RomFS image
 * (romfs.c); save data lives in host folders (savefs.c); the rest comes as the game first
 * uses it. Command ids:
 * Switchbrew "Filesystem_services"; LAS Recomp's services/fs.c (MIT) as the cross-check. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlc_known.h"
#include "ipc_simple.h"
#include "kernel.h"
#include "romfs.h"
#include "savefs.h"

#define FS_OUT_OF_RANGE 0x3EA02u      /* fs ResultOutOfRange (module 2, description 3001) */
#define FS_TARGET_NOT_FOUND 0x7D402u  /* fs ResultTargetNotFound (module 2, description 1002) */
#define YW4_ADDON_BASE_ID 0x010086C00AF7D000ull  /* add-on title IDs: this + index (target.toml [title]) */

/* The receive buffer of a Read: a B (or C) descriptor. */
static const IpcBuffer *out_buffer(const IpcRequest *req) {
    if (req->nb && req->b[0].size) return &req->b[0];
    if (req->nc && req->c[0].size) return &req->c[0];
    return req->nb ? &req->b[0] : req->nc ? &req->c[0] : NULL;
}

/* IStorage over the RomFS image: the game's, or an add-on's (s->state, decision 0056) */
static void romfs_storage(Service *s, IpcRequest *req, IpcResponse *resp) {
    RomfsImage *addon = s->state;
    switch (req->cmd) {
    case 0: {  /* Read(offset, size) -> B buffer */
        uint64_t offset = IPC_IN(uint64_t, 0), size = IPC_IN(uint64_t, 8);
        const IpcBuffer *b = out_buffer(req);
        if (!b || size > b->size) size = b ? b->size : 0;
        void *out = GPTR(b ? b->addr : 0);
        /* The read without the kernel lock (romfs.c has a lock of its own): a loading thread's large reads held every
         * other thread's SVCs, the presenting one's too: a 200 ms frame at the Demon Hand's first effect load
         * (2026-11-11, YW4_PROFILE=hitch: the main thread waiting in svc). */
        yw4_kernel_unlock();
        int failed = addon ? romfs_image_read(addon, offset, out, size) : romfs_read(offset, out, size);
        yw4_kernel_lock();
        if (failed) {
            fprintf(stderr, "yw4: fs: %sRomFS read [%#" PRIx64 ", +%#" PRIx64 ") failed\n", addon ? "add-on " : "",
                    offset, size);
            resp->result = FS_OUT_OF_RANGE;
        }
        return;
    }
    case 4:  /* GetSize */
        IPC_OUT(uint64_t, 0) = addon ? romfs_image_size(addon) : romfs_size();
        ipc_out_size(resp, 8);
        return;
    default:
        ipc_unimplemented(s, req, resp);
    }
}

/* The RomFS of add-on `data_id` (its title ID), from the files the build extracted into YW4_DLCROMFS_DIR/<index>/
 * (tools/yw4/dlc.py, decision 0056); built once, then kept. NULL if the game doesn't own it or it isn't there. */
static RomfsImage *addon_image(uint64_t data_id) {
    static RomfsImage *images[128];
    uint64_t index = data_id - YW4_ADDON_BASE_ID;
    if (data_id < YW4_ADDON_BASE_ID || index >= 128 || !aoc_owned((uint32_t)index)) return NULL;
    if (!images[index]) {
        const char *base = getenv("YW4_DLCROMFS_DIR");
        char dir[1100];
        snprintf(dir, sizeof dir, "%s/%u", base && *base ? base : "work/dlc-romfs", (unsigned)index);
        if (!(images[index] = romfs_image_open(dir)))
            fprintf(stderr, "yw4: fs: add-on %u is owned but its files are not in %s (the build extracts them)\n",
                    (unsigned)index, dir);
    }
    return images[index];
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
    case 202: {  /* OpenDataStorageByDataId(u8 storage id, u64 data id at +8) -> IStorage: an add-on's RomFS */
        uint8_t storage = IPC_IN(uint8_t, 0);
        uint64_t id = IPC_IN(uint64_t, 8);
        RomfsImage *m = addon_image(id);
        fprintf(stderr, "yw4: fs: OpenDataStorageByDataId(storage %u, data id %016" PRIX64 ") -> %s\n", storage, id,
                m ? "add-on RomFS" : "not found");
        if (m) ipc_return_object(resp, ipc_service_new("IStorage (add-on RomFS)", romfs_storage, m));
        else resp->result = FS_TARGET_NOT_FOUND;
        return;
    }
    }
    if (!IPC_SIMPLE(fsp_simple, req, resp)) ipc_unimplemented(s, req, resp);
}
