/* <endian.h> for hosts that don't have it (Windows). musl's libm.h only needs the byte order. */
#ifndef YW4_MUSL_ENDIAN_H
#define YW4_MUSL_ENDIAN_H
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
