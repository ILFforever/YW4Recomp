/* C library: memory, strings, character classes, integer parsing (decision 0012).
 *
 * Only functions whose result is fully defined by their inputs live here, so the host can
 * compute them. Where C leaves the exact value open (the sign-carrying int of memcmp and
 * friends), musl's choice is reproduced: the difference of the first differing bytes, as
 * unsigned chars. Character classes are musl's "C" locale: ASCII only, nothing above 127.
 *
 * Arguments are taken from x0..x3; int arguments are the low 32 bits.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#define ARG_INT(i) ((int)(uint32_t)ctx->x[i])
#define RET_INT(v) (ctx->x[0] = (uint64_t)(int64_t)(int)(v))

/* errno: one for the single guest thread so far (per thread once threads exist). */
static int guest_errno;
void hle_libc_errno_location(CPUState *ctx) { ctx->x[0] = GADDR(&guest_errno); }

/* Memory */
void hle_libc_memcpy(CPUState *ctx) { memcpy(GPTR(ctx->x[0]), GPTR(ctx->x[1]), ctx->x[2]); }
void hle_libc_memmove(CPUState *ctx) { memmove(GPTR(ctx->x[0]), GPTR(ctx->x[1]), ctx->x[2]); }
void hle_libc_memset(CPUState *ctx) { memset(GPTR(ctx->x[0]), ARG_INT(1), ctx->x[2]); }

void hle_libc_memchr(CPUState *ctx) {
    ctx->x[0] = GADDR(memchr(GPTR(ctx->x[0]), (unsigned char)ARG_INT(1), ctx->x[2]));
}

void hle_libc_memcmp(CPUState *ctx) {
    const uint8_t *l = GPTR(ctx->x[0]), *r = GPTR(ctx->x[1]);
    size_t n = ctx->x[2];
    for (; n && *l == *r; n--, l++, r++) {}
    RET_INT(n ? *l - *r : 0);
}

/* errno_t memset_s(void *s, rsize_t smax, int c, rsize_t n) (C11 Annex K): on n > smax it
 * still fills smax bytes and reports EINVAL. */
void hle_libc_memset_s(CPUState *ctx) {
    uint64_t smax = ctx->x[1], n = ctx->x[3];
    if (!ctx->x[0] || smax > (SIZE_MAX >> 1)) {
        RET_INT(EINVAL);
        return;
    }
    memset(GPTR(ctx->x[0]), ARG_INT(2), n > smax ? smax : n);
    RET_INT(n > smax || n > (SIZE_MAX >> 1) ? EINVAL : 0);
}

/* Strings: results are pointers or lengths, identical on any correct libc. */
void hle_libc_strlen(CPUState *ctx) { ctx->x[0] = strlen(GPTR(ctx->x[0])); }
void hle_libc_strnlen(CPUState *ctx) {
    const char *s = GPTR(ctx->x[0]);
    size_t n = 0;
    while (n < ctx->x[1] && s[n]) n++;
    ctx->x[0] = n;
}
void hle_libc_strcpy(CPUState *ctx) { strcpy(GPTR(ctx->x[0]), GPTR(ctx->x[1])); }
void hle_libc_strncpy(CPUState *ctx) { strncpy(GPTR(ctx->x[0]), GPTR(ctx->x[1]), ctx->x[2]); }
void hle_libc_strcat(CPUState *ctx) { strcat(GPTR(ctx->x[0]), GPTR(ctx->x[1])); }
void hle_libc_strncat(CPUState *ctx) { strncat(GPTR(ctx->x[0]), GPTR(ctx->x[1]), ctx->x[2]); }
void hle_libc_strchr(CPUState *ctx) { ctx->x[0] = GADDR(strchr(GPTR(ctx->x[0]), (char)ARG_INT(1))); }
void hle_libc_strrchr(CPUState *ctx) { ctx->x[0] = GADDR(strrchr(GPTR(ctx->x[0]), (char)ARG_INT(1))); }
void hle_libc_strstr(CPUState *ctx) { ctx->x[0] = GADDR(strstr(GPTR(ctx->x[0]), GPTR(ctx->x[1]))); }
void hle_libc_strpbrk(CPUState *ctx) { ctx->x[0] = GADDR(strpbrk(GPTR(ctx->x[0]), GPTR(ctx->x[1]))); }
void hle_libc_strspn(CPUState *ctx) { ctx->x[0] = strspn(GPTR(ctx->x[0]), GPTR(ctx->x[1])); }

void hle_libc_strcmp(CPUState *ctx) {
    const uint8_t *l = GPTR(ctx->x[0]), *r = GPTR(ctx->x[1]);
    for (; *l == *r && *l; l++, r++) {}
    RET_INT(*l - *r);
}

/* strcoll in the "C" locale is strcmp. */
void hle_libc_strcoll(CPUState *ctx) { hle_libc_strcmp(ctx); }

void hle_libc_strncmp(CPUState *ctx) {
    const uint8_t *l = GPTR(ctx->x[0]), *r = GPTR(ctx->x[1]);
    size_t n = ctx->x[2];
    if (!n) {
        RET_INT(0);
        return;
    }
    for (; *l && *r && n > 1 && *l == *r; l++, r++, n--) {}
    RET_INT(*l - *r);
}

static int c_lower(int c) { return c >= 'A' && c <= 'Z' ? c | 32 : c; }
static int c_upper(int c) { return c >= 'a' && c <= 'z' ? c & 0x5f : c; }

void hle_libc_strcasecmp(CPUState *ctx) {
    const uint8_t *l = GPTR(ctx->x[0]), *r = GPTR(ctx->x[1]);
    for (; *l && *r && (*l == *r || c_lower(*l) == c_lower(*r)); l++, r++) {}
    RET_INT(c_lower(*l) - c_lower(*r));
}

/* char *strtok(char *s, const char *sep): keeps its position between calls, like libc's
 * (which isn't thread-safe either). */
void hle_libc_strtok(CPUState *ctx) {
    static char *pos;
    char *s = GPTR(ctx->x[0]);
    const char *sep = GPTR(ctx->x[1]);
    if (!s && !(s = pos)) {
        ctx->x[0] = 0;
        return;
    }
    s += strspn(s, sep);
    if (!*s) {
        pos = NULL;
        ctx->x[0] = 0;
        return;
    }
    pos = s + strcspn(s, sep);
    if (*pos) *pos++ = '\0';
    else pos = NULL;
    ctx->x[0] = GADDR(s);
}

/* Character classes, "C" locale. EOF (-1) and everything above 127 are in no class. */
#define CTYPE(name, test)                                   \
    void hle_libc_##name(CPUState *ctx) {                   \
        int c = ARG_INT(0);                                 \
        RET_INT((unsigned)c < 128 && (test));               \
    }
CTYPE(isdigit, c >= '0' && c <= '9')
CTYPE(islower, c >= 'a' && c <= 'z')
CTYPE(isalnum, (c >= '0' && c <= '9') || (c | 32) - 'a' < 26u)
CTYPE(isxdigit, (c >= '0' && c <= '9') || (c | 32) - 'a' < 6u)
CTYPE(isspace, c == ' ' || (c >= '\t' && c <= '\r'))
CTYPE(iscntrl, c < 32 || c == 127)
CTYPE(ispunct, c > 32 && c < 127 && !((c >= '0' && c <= '9') || (c | 32) - 'a' < 26u))
void hle_libc_tolower(CPUState *ctx) { RET_INT(c_lower(ARG_INT(0))); }
void hle_libc_toupper(CPUState *ctx) { RET_INT(c_upper(ARG_INT(0))); }

/* Integers. Guest long is 64-bit (LP64); the Windows host's long is 32-bit, so the long
 * functions go through the long long ones. errno values are the same numbers in musl and
 * the host CRT for the two that occur here (EINVAL 22, ERANGE 34). */
void hle_libc_abs(CPUState *ctx) {
    int v = ARG_INT(0);
    RET_INT(v < 0 ? 0u - (unsigned)v : (unsigned)v);
}

/* int atoi(const char *): as musl, accumulates negatively and wraps on overflow. */
void hle_libc_atoi(CPUState *ctx) {
    const char *s = GPTR(ctx->x[0]);
    unsigned n = 0;
    int neg = 0;
    while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') n = 10 * n - (unsigned)(*s++ - '0');
    RET_INT(neg ? (int)n : (int)(0u - n));
}

static void strto_finish(CPUState *ctx, uint64_t value, char *end) {
    if (ctx->x[1]) *(uint64_t *)GPTR(ctx->x[1]) = GADDR(end);
    if (errno == ERANGE || errno == EINVAL) guest_errno = errno;
    ctx->x[0] = value;
}

void hle_libc_strtol(CPUState *ctx) {
    char *end;
    errno = 0;
    long long v = strtoll(GPTR(ctx->x[0]), &end, ARG_INT(2));
    strto_finish(ctx, (uint64_t)v, end);
}

void hle_libc_strtoull(CPUState *ctx) {
    char *end;
    errno = 0;
    unsigned long long v = strtoull(GPTR(ctx->x[0]), &end, ARG_INT(2));
    strto_finish(ctx, v, end);
}

/* unsigned long is 64-bit on the guest: the same function as strtoull. */
void hle_libc_strtoul(CPUState *ctx) { hle_libc_strtoull(ctx); }
