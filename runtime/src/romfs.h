/* A RomFS image of a folder, built in memory at start-up (decision 0014).
 *
 * The game reads its data as one raw RomFS image (fsp-srv OpenDataStorageByCurrentProcess)
 * and the sdk parses the RomFS tables itself. The owner has an extracted romfs/ folder, so
 * the runtime builds the tables from it and serves reads from the file-data area straight
 * from the files on disk: no 8.5 GB second copy. Later, a mod folder (LayeredFS) can
 * replace files in the same build. */
#ifndef ROMFS_H
#define ROMFS_H

#include <stddef.h>
#include <stdint.h>

/* Builds the image of `dir`; returns 0, or prints why not and returns -1. */
int romfs_open(const char *dir);
uint64_t romfs_size(void);
/* Reads [offset, offset + size) of the image; returns 0, or -1 on a host read error. */
int romfs_read(uint64_t offset, void *out, uint64_t size);

#endif
