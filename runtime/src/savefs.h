/* Save data in host folders (savefs.c). */
#ifndef SAVEFS_H
#define SAVEFS_H

#include <stdint.h>

#include "ipc.h"

#define YW4_PROGRAM_ID 0x010086C00AF7C000ull  /* Yo-kai Watch 4++ (CLAUDE.md) */

/* The folder all saves go under (yw4's third argument). 0 on success. */
int savefs_init(const char *dir);

/* The save of (program, user): with create = 0, an IFileSystem in *out, or fs
 * ResultTargetNotFound if it doesn't exist; with create = 1, makes it (out unused). */
uint32_t savefs_open(uint64_t program_id, const uint8_t uid[16], int create, Service **out);

#endif
