/* Virtual RomFS image of a folder (romfs.h, decision 0014).
 *
 * Image layout, as the game's own: a 0x50-byte header (padded to 0x200), file data from 0x200
 * (each file 16-byte aligned), then the directory hash table, directory entries, file hash
 * table and file entries. The game finds entries by hash, so the order inside the tables
 * only has to be consistent: directories and files sorted by name, a directory's
 * subdirectories after all of its files' and siblings'. The table construction and the name
 * hash follow LAS Recomp's runtime/romfs_build.c (MIT, github.com/peiikai/LASRecomp); here the
 * file data is not copied but read from the original files on demand.
 */
#include "romfs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define EMPTY 0xFFFFFFFFu
#define DATA_START 0x200u

typedef struct Dir Dir;
typedef struct {
    char *name, *path;
    uint64_t size, data_off;  /* data_off: relative to DATA_START */
    uint32_t entry;           /* offset in the file table */
    Dir *parent;
} File;
struct Dir {
    char *name;
    Dir *parent;
    Dir **dirs;
    File **files;
    int ndirs, nfiles;
    uint32_t entry;           /* offset in the directory table */
};

static char *dup_str(const char *s) {
    char *d = malloc(strlen(s) + 1);
    if (!d) exit(3);
    return strcpy(d, s);
}

static int64_t file_size(const char *path, int *is_dir) {
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st)) return -1;
#else
    struct stat st;
    if (stat(path, &st)) return -1;
#endif
    *is_dir = (st.st_mode & S_IFMT) == S_IFDIR;
    return (int64_t)st.st_size;
}

static int by_dir_name(const void *a, const void *b) { return strcmp((*(Dir *const *)a)->name, (*(Dir *const *)b)->name); }
static int by_file_name(const void *a, const void *b) { return strcmp((*(File *const *)a)->name, (*(File *const *)b)->name); }

static Dir *scan(const char *path, const char *name, Dir *parent, int depth) {
    DIR *d = depth < 32 ? opendir(path) : NULL;
    if (!d) return NULL;
    Dir *dir = calloc(1, sizeof *dir);
    if (!dir) exit(3);
    dir->name = dup_str(name);
    dir->parent = parent;
    int cap_d = 0, cap_f = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char *p = malloc(n);
        if (!p) exit(3);
        snprintf(p, n, "%s/%s", path, e->d_name);
        int is_dir = 0;
        int64_t size = file_size(p, &is_dir);
        if (size < 0) {
            free(p);
        } else if (is_dir) {
            Dir *c = scan(p, e->d_name, dir, depth + 1);
            free(p);
            if (!c) continue;
            if (dir->ndirs == cap_d) dir->dirs = realloc(dir->dirs, sizeof *dir->dirs * (size_t)(cap_d = cap_d ? 2 * cap_d : 8));
            dir->dirs[dir->ndirs++] = c;
        } else {
            File *f = calloc(1, sizeof *f);
            if (!f) exit(3);
            f->name = dup_str(e->d_name), f->path = p, f->size = (uint64_t)size, f->parent = dir;
            if (dir->nfiles == cap_f) dir->files = realloc(dir->files, sizeof *dir->files * (size_t)(cap_f = cap_f ? 2 * cap_f : 16));
            dir->files[dir->nfiles++] = f;
        }
    }
    closedir(d);
    if (dir->ndirs) qsort(dir->dirs, (size_t)dir->ndirs, sizeof *dir->dirs, by_dir_name);
    if (dir->nfiles) qsort(dir->files, (size_t)dir->nfiles, sizeof *dir->files, by_file_name);
    return dir;
}

/* Table order: a directory's files, then its subdirectories, then each subdirectory's. */
static Dir **dirs;
static File **files;
static int ndirs, nfiles, cap_dirs, cap_files;

static void flatten(Dir *d) {
    for (int i = 0; i < d->nfiles; i++) {
        if (nfiles == cap_files) files = realloc(files, sizeof *files * (size_t)(cap_files = cap_files ? 2 * cap_files : 256));
        files[nfiles++] = d->files[i];
    }
    for (int i = 0; i < d->ndirs; i++) {
        if (ndirs == cap_dirs) dirs = realloc(dirs, sizeof *dirs * (size_t)(cap_dirs = cap_dirs ? 2 * cap_dirs : 64));
        dirs[ndirs++] = d->dirs[i];
    }
    for (int i = 0; i < d->ndirs; i++) flatten(d->dirs[i]);
}

/* The RomFS name hash and bucket count, as the game's RomFS code computes them. */
static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = ((h >> 5) | (h << 27)) ^ *p;
    return h;
}
static uint32_t bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint32_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* The image: header (0..0x200), file data, then `tables` (from tables_off). */
static uint8_t header[DATA_START];
static uint8_t *tables;
static uint64_t tables_off, tables_size, image_size, data_size;

int romfs_open(const char *dir) {
    Dir *root = scan(dir, "", NULL, 0);
    if (!root) {
        fprintf(stderr, "yw4: cannot read the RomFS folder %s\n", dir);
        return -1;
    }
    dirs = realloc(dirs, sizeof *dirs * (size_t)(cap_dirs = 64));
    dirs[ndirs++] = root;
    flatten(root);

    uint32_t dsize = 0, fsize = 0;
    for (int i = 0; i < ndirs; i++) dirs[i]->entry = dsize, dsize += 0x18 + align4((uint32_t)strlen(dirs[i]->name));
    for (int i = 0; i < nfiles; i++) files[i]->entry = fsize, fsize += 0x20 + align4((uint32_t)strlen(files[i]->name));
    for (int i = 0; i < nfiles; i++) {
        data_size = (data_size + 15) & ~15ull;
        files[i]->data_off = data_size;
        data_size += files[i]->size;
    }

    uint32_t nbd = bucket_count((uint32_t)ndirs), nbf = bucket_count((uint32_t)nfiles);
    tables_off = (DATA_START + data_size + 3) & ~3ull;
    uint64_t dhash = 0, dmeta = 4ull * nbd, fhash = dmeta + dsize, fmeta = fhash + 4ull * nbf;
    tables_size = fmeta + fsize;
    tables = malloc(tables_size);
    if (!tables) exit(3);
    for (uint32_t i = 0; i < nbd; i++) put32(tables + dhash + 4 * i, EMPTY);
    for (uint32_t i = 0; i < nbf; i++) put32(tables + fhash + 4 * i, EMPTY);
    memset(tables + dmeta, 0, dsize);
    memset(tables + fmeta, 0, fsize);

    for (int i = 0; i < ndirs; i++) {  /* parent, sibling, first child dir, first file, hash chain, name */
        Dir *d = dirs[i];
        uint8_t *e = tables + dmeta + d->entry;
        uint32_t parent = d->parent ? d->parent->entry : 0, nl = (uint32_t)strlen(d->name), sibling = EMPTY;
        if (d->parent)
            for (int k = 0; k + 1 < d->parent->ndirs; k++)
                if (d->parent->dirs[k] == d) sibling = d->parent->dirs[k + 1]->entry;
        uint8_t *bucket = tables + dhash + 4 * (name_hash(parent, d->name) % nbd);
        put32(e, parent);
        put32(e + 4, sibling);
        put32(e + 8, d->ndirs ? d->dirs[0]->entry : EMPTY);
        put32(e + 12, d->nfiles ? d->files[0]->entry : EMPTY);
        memcpy(e + 16, bucket, 4);
        put32(e + 20, nl);
        memcpy(e + 0x18, d->name, nl);
        put32(bucket, d->entry);
    }
    for (int i = 0; i < nfiles; i++) {  /* parent, sibling, data offset, size, hash chain, name */
        File *f = files[i];
        uint8_t *e = tables + fmeta + f->entry;
        uint32_t nl = (uint32_t)strlen(f->name), sibling = EMPTY;
        for (int k = 0; k + 1 < f->parent->nfiles; k++)
            if (f->parent->files[k] == f) sibling = f->parent->files[k + 1]->entry;
        uint8_t *bucket = tables + fhash + 4 * (name_hash(f->parent->entry, f->name) % nbf);
        put32(e, f->parent->entry);
        put32(e + 4, sibling);
        put64(e + 8, f->data_off);
        put64(e + 16, f->size);
        memcpy(e + 24, bucket, 4);
        put32(e + 28, nl);
        memcpy(e + 0x20, f->name, nl);
        put32(bucket, f->entry);
    }

    uint64_t hv[10] = {0x50, tables_off + dhash, 4ull * nbd, tables_off + dmeta, dsize,
                       tables_off + fhash, 4ull * nbf, tables_off + fmeta, fsize, DATA_START};
    memcpy(header, hv, sizeof hv);
    image_size = tables_off + tables_size;
    fprintf(stderr, "yw4: RomFS: %d files, %d directories, %.1f GiB of data (from %s)\n", nfiles, ndirs - 1,
            (double)data_size / (1 << 30), dir);
    return 0;
}

uint64_t romfs_size(void) { return image_size; }

/* A few open files: reads come in long runs from the same file. */
#define OPEN_FILES 32
static struct {
    const File *file;
    FILE *fp;
} open_files[OPEN_FILES];
static int next_slot;

static FILE *file_handle(const File *f) {
    for (int i = 0; i < OPEN_FILES; i++)
        if (open_files[i].file == f) return open_files[i].fp;
    int i = next_slot++ % OPEN_FILES;
    if (open_files[i].fp) fclose(open_files[i].fp);
    open_files[i].file = f;
    open_files[i].fp = fopen(f->path, "rb");
    return open_files[i].fp;
}

static int read_file_part(const File *f, uint64_t at, uint8_t *out, uint64_t n) {
    FILE *fp = file_handle(f);
    if (!fp) return -1;
#ifdef _WIN32
    if (_fseeki64(fp, (long long)at, SEEK_SET)) return -1;
#else
    if (fseeko(fp, (off_t)at, SEEK_SET)) return -1;
#endif
    return fread(out, 1, (size_t)n, fp) == n ? 0 : -1;
}

int romfs_read(uint64_t offset, void *out_, uint64_t size) {
    uint8_t *out = out_;
    if (offset > image_size || size > image_size - offset) return -1;
    while (size) {
        uint64_t n;
        if (offset < DATA_START) {  /* header */
            n = DATA_START - offset < size ? DATA_START - offset : size;
            memcpy(out, header + offset, (size_t)n);
        } else if (offset >= tables_off) {  /* tables */
            n = size;
            memcpy(out, tables + (offset - tables_off), (size_t)n);
        } else {  /* file data: find the last file starting at or before this offset */
            uint64_t rel = offset - DATA_START;
            int lo = 0, hi = nfiles;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (files[mid]->data_off <= rel) lo = mid + 1;
                else hi = mid;
            }
            const File *f = lo ? files[lo - 1] : NULL;
            uint64_t next = lo < nfiles ? DATA_START + files[lo]->data_off : tables_off;
            if (f && rel < f->data_off + f->size) {
                uint64_t left = f->data_off + f->size - rel;
                n = left < size ? left : size;
                if (read_file_part(f, rel - f->data_off, out, n)) return -1;
            } else {  /* alignment padding between files */
                n = next - offset < size ? next - offset : size;
                memset(out, 0, (size_t)n);
            }
        }
        out += n, offset += n, size -= n;
    }
    return 0;
}
