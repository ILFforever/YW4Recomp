/* Save data on the host: IFileSystem and IFile over a folder (Switchbrew "Filesystem_services").
 *
 * Each save is a folder <save dir>/<program id>/<user id>/. Paths the game sends are
 * Switch paths ("/data/save.bin", at most 0x300 bytes); ".." and backslashes are refused,
 * so nothing outside the save folder can be reached.
 *
 * Simplification: the Switch journals save data, so writes only become permanent at
 * Commit; here they go to the file at once and Commit does nothing. A crash in the middle
 * of a save can therefore leave a half-written file that the Switch would have discarded.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_one(p) _mkdir(p)
#define truncate_file(f, n) _chsize_s(_fileno(f), (long long)(n))
#else
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0755)
#define truncate_file(f, n) ftruncate(fileno(f), (off_t)(n))
#endif

#include "ipc_simple.h"
#include "savefs.h"

#define FS_PATH_NOT_FOUND 0x202u       /* fs (module 2), description 1 */
#define FS_PATH_ALREADY_EXISTS 0x402u  /* description 2 */
#define FS_TARGET_NOT_FOUND 0x7D402u   /* description 1002 */
#define FS_INVALID_PATH 0x2EE602u      /* description 6003 (invalid path format) */
#define FS_OUT_OF_RANGE 0x3EA02u       /* description 3001 */

static char save_root[1024];

int savefs_init(const char *dir) {
    if (strlen(dir) >= sizeof save_root - 200) return -1;
    strcpy(save_root, dir);
    return 0;
}

/* mkdir -p */
static void make_dirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
            mkdir_one(p);
            *c = '/';
        }
    mkdir_one(p);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static void save_dir(char out[1024], uint64_t program_id, const uint8_t uid[16]) {
    char u[33];
    for (int i = 0; i < 16; i++) snprintf(u + 2 * i, 3, "%02x", uid[i]);
    snprintf(out, 1024, "%s/%016" PRIx64 "/%s", save_root, program_id, u);
}

/* 64-bit seek (offset -1: to the end) and tell: 0 / position, as fseek and ftell */
static int file_seek(FILE *f, int64_t off) {
#ifdef _WIN32
    return off < 0 ? _fseeki64(f, 0, SEEK_END) : _fseeki64(f, off, SEEK_SET);
#else
    return off < 0 ? fseeko(f, 0, SEEK_END) : fseeko(f, (off_t)off, SEEK_SET);
#endif
}
static int64_t file_tell(FILE *f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}

/* ---- IFile */

typedef struct {
    FILE *f;
} SaveFile;

static void save_file(Service *s, IpcRequest *req, IpcResponse *resp) {
    SaveFile *sf = s->state;
    switch (req->cmd) {
    case 0: {  /* Read(u32 option, u64 offset, u64 size, B buffer) -> u64 bytes read */
        uint64_t off = IPC_IN(uint64_t, 8), size = IPC_IN(uint64_t, 16);
        IpcBuffer out = ipc_out_buffer(req, 0);
        if (size > out.size) size = out.size;
        size_t n = 0;
        if (!file_seek(sf->f, off)) n = fread(GPTR(out.addr), 1, (size_t)size, sf->f);
        IPC_OUT(uint64_t, 0) = n;
        ipc_out_size(resp, 8);
        return;
    }
    case 1: {  /* Write(u32 option, u64 offset, u64 size, A buffer) */
        uint64_t off = IPC_IN(uint64_t, 8), size = IPC_IN(uint64_t, 16);
        const IpcBuffer *in = req->na ? &req->a[0] : &req->x[0];
        if (size > in->size || file_seek(sf->f, off) || fwrite(GPTR(in->addr), 1, (size_t)size, sf->f) != size)
            resp->result = FS_OUT_OF_RANGE;
        return;
    }
    case 2:  /* Flush */
        fflush(sf->f);
        return;
    case 3:  /* SetSize(u64) */
        fflush(sf->f);
        if (truncate_file(sf->f, IPC_IN(uint64_t, 0))) resp->result = FS_OUT_OF_RANGE;
        return;
    case 4: {  /* GetSize -> u64 */
        fflush(sf->f);
        file_seek(sf->f, -1);
        IPC_OUT(uint64_t, 0) = (uint64_t)file_tell(sf->f);
        ipc_out_size(resp, 8);
        return;
    }
    }
    ipc_unimplemented(s, req, resp);
}

/* ---- IDirectory: the entries, read once when it is opened */

typedef struct {
    char name[0x301];
    uint8_t type;  /* 0 directory, 1 file */
    int64_t size;
} DirEntry;

typedef struct {
    DirEntry *entries;
    uint64_t n, next;
} SaveDir;

static void save_directory(Service *s, IpcRequest *req, IpcResponse *resp) {
    SaveDir *d = s->state;
    switch (req->cmd) {
    case 0: {  /* Read(B: DirectoryEntry[], 0x310 each) -> u64 count */
        IpcBuffer out = ipc_out_buffer(req, 0);
        uint64_t n = 0;
        uint8_t *p = GPTR(out.addr);
        for (; n < out.size / 0x310 && d->next < d->n; n++, d->next++, p += 0x310) {
            /* DirectoryEntry: name[0x301], pad[3], u8 type, pad[3], s64 size */
            memset(p, 0, 0x310);
            memcpy(p, d->entries[d->next].name, strlen(d->entries[d->next].name));
            p[0x304] = d->entries[d->next].type;
            memcpy(p + 0x308, &d->entries[d->next].size, 8);
        }
        IPC_OUT(uint64_t, 0) = n;
        ipc_out_size(resp, 8);
        return;
    }
    case 1:  /* GetEntryCount -> u64 */
        IPC_OUT(uint64_t, 0) = d->n - d->next;
        ipc_out_size(resp, 8);
        return;
    }
    ipc_unimplemented(s, req, resp);
}

/* mode: 1 directories, 2 files, 3 both. NULL if the path is no directory. */
static SaveDir *read_directory(const char *path, uint32_t mode) {
    DIR *dir = opendir(path);
    if (!dir) return NULL;
    SaveDir *d = calloc(1, sizeof *d);
    if (!d) exit(3);
    uint64_t cap = 0;
    for (struct dirent *e; (e = readdir(dir));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) > 0x300) continue;
        char full[2048];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st)) continue;
        int is_directory = (st.st_mode & S_IFMT) == S_IFDIR;
        if (!(mode & (is_directory ? 1 : 2))) continue;
        if (d->n == cap && !(d->entries = realloc(d->entries, (cap = cap ? 2 * cap : 16) * sizeof *d->entries))) exit(3);
        DirEntry *de = &d->entries[d->n++];
        strcpy(de->name, e->d_name);
        de->type = is_directory ? 0 : 1;
        de->size = is_directory ? 0 : (int64_t)st.st_size;
    }
    closedir(dir);
    return d;
}

/* ---- IFileSystem over one save folder */

typedef struct {
    char root[1024];
} SaveFs;

/* The host path for the Switch path in the first X buffer; 0 if it is malformed. */
static int host_path(const SaveFs *fs, const IpcRequest *req, int x, char out[1024]) {
    if (req->nx <= x) return 0;
    const char *p = GPTR(req->x[x].addr);
    size_t n = strnlen(p, req->x[x].size < 0x301 ? req->x[x].size : 0x301);
    if (n == 0 || n > 0x300 || p[0] != '/' || memchr(p, '\\', n) || strstr(p, "/../") ||
        (n >= 3 && !memcmp(p + n - 3, "/..", 3)) || strstr(p, ":"))
        return 0;
    snprintf(out, 1024, "%s%.*s", fs->root, (int)n, p);
    size_t len = strlen(out);
    while (len > strlen(fs->root) + 1 && out[len - 1] == '/') out[--len] = 0;  /* "/dir/" -> "/dir" */
    return 1;
}

static void save_fs(Service *s, IpcRequest *req, IpcResponse *resp) {
    SaveFs *fs = s->state;
    char path[1024];
    switch (req->cmd) {
    case 0: {  /* CreateFile(u32 option, u64 size, X path) */
        if (!host_path(fs, req, 0, path)) break;
        struct stat st;
        if (stat(path, &st) == 0) { resp->result = FS_PATH_ALREADY_EXISTS; return; }
        FILE *f = fopen(path, "wb");
        if (!f) { resp->result = FS_PATH_NOT_FOUND; return; }
        if (truncate_file(f, IPC_IN(uint64_t, 8))) resp->result = FS_OUT_OF_RANGE;
        fclose(f);
        return;
    }
    case 1:  /* DeleteFile(X path) */
        if (!host_path(fs, req, 0, path)) break;
        if (remove(path)) resp->result = FS_PATH_NOT_FOUND;
        return;
    case 2: {  /* CreateDirectory(X path) */
        if (!host_path(fs, req, 0, path)) break;
        struct stat st;
        if (stat(path, &st) == 0) { resp->result = FS_PATH_ALREADY_EXISTS; return; }
        if (mkdir_one(path)) resp->result = FS_PATH_NOT_FOUND;
        return;
    }
    case 7: {  /* GetEntryType(X path) -> u32: 0 directory, 1 file */
        if (!host_path(fs, req, 0, path)) break;
        struct stat st;
        if (stat(path, &st)) { resp->result = FS_PATH_NOT_FOUND; return; }
        IPC_OUT(uint32_t, 0) = (st.st_mode & S_IFMT) == S_IFDIR ? 0 : 1;
        ipc_out_size(resp, 4);
        return;
    }
    case 8: {  /* OpenFile(u32 mode: 1 read, 2 write, 4 append; X path) -> IFile */
        if (!host_path(fs, req, 0, path)) break;
        uint32_t mode = IPC_IN(uint32_t, 0);
        struct stat st;
        if (stat(path, &st) || (st.st_mode & S_IFMT) == S_IFDIR) { resp->result = FS_PATH_NOT_FOUND; return; }
        FILE *f = fopen(path, mode & 6 ? "r+b" : "rb");
        if (!f) { resp->result = FS_PATH_NOT_FOUND; return; }
        SaveFile *sf = calloc(1, sizeof *sf);
        if (!sf) exit(3);
        sf->f = f;  /* closed never: files of a save stay few, and the Switch's limit is the game's concern */
        ipc_return_object(resp, ipc_service_new("fs:IFile (save)", save_file, sf));
        return;
    }
    case 9: {  /* OpenDirectory(u32 mode, X path) -> IDirectory */
        if (!host_path(fs, req, 0, path)) break;
        SaveDir *d = read_directory(path, IPC_IN(uint32_t, 0));
        if (!d) { resp->result = FS_PATH_NOT_FOUND; return; }
        ipc_return_object(resp, ipc_service_new("fs:IDirectory (save)", save_directory, d));
        return;
    }
    case 10:  /* Commit: writes are already in the files */
        return;
    default:
        ipc_unimplemented(s, req, resp);
        return;
    }
    resp->result = FS_INVALID_PATH;
}

uint32_t savefs_open(uint64_t program_id, const uint8_t uid[16], int create, Service **out) {
    char dir[1024];
    save_dir(dir, program_id, uid);
    if (!is_dir(dir)) {
        if (!create) return FS_TARGET_NOT_FOUND;
        make_dirs(dir);
        if (!is_dir(dir)) {
            fprintf(stderr, "yw4: fs: cannot create the save folder %s\n", dir);
            exit(3);
        }
        fprintf(stderr, "yw4: fs: created save data in %s\n", dir);
        return 0;
    }
    if (out) {
        SaveFs *fs = calloc(1, sizeof *fs);
        if (!fs) exit(3);
        snprintf(fs->root, sizeof fs->root, "%s", dir);
        *out = ipc_service_new("fs:IFileSystem (save)", save_fs, fs);
    }
    return 0;
}
