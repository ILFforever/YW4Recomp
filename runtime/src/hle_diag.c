/* nn::diag: the sdk's logging and assertion reporting (decision 0011), plus the guest
 * printf formatting it needs.
 *
 * Guest variadic arguments can't be handed to the host's vprintf: they sit in guest
 * registers and on the guest stack. yw4_guest_format walks the format string, takes each
 * argument from where AAPCS64 puts it, and formats one conversion at a time with snprintf.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

GuestVarargs yw4_varargs(CPUState *ctx, unsigned fixed_gr, unsigned fixed_vr) {
    GuestVarargs va = {ctx, fixed_gr, fixed_vr, ctx->sp};
    return va;
}

uint64_t yw4_vararg_int(GuestVarargs *va) {
    if (va->next_gr < 8) return va->ctx->x[va->next_gr++];
    uint64_t v;
    memcpy(&v, GPTR(va->stack), 8);
    va->stack += 8;
    return v;
}

double yw4_vararg_double(GuestVarargs *va) {
    if (va->next_vr < 8) return va->ctx->v[va->next_vr++].df[0];
    double v;
    memcpy(&v, GPTR(va->stack), 8);
    va->stack += 8;
    return v;
}

int yw4_guest_format(char *out, size_t size, const char *fmt, GuestVarargs *va) {
    size_t len = 0;  /* length of the full output so far, even past `size` */
#define ROOM (len < size ? size - len : 0)
#define DST (len < size ? out + len : NULL)
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            if (len + 1 < size) out[len] = *p;
            len++, p++;
            continue;
        }
        /* One conversion: copy flags, width and precision into a host spec, resolving '*'. */
        char spec[48];
        size_t n = 0;
        spec[n++] = *p++;
        while (*p && strchr("-+ #0", *p) && n < 8) spec[n++] = *p++;
        for (int part = 0; part < 2; part++) {
            if (part == 1) {
                if (*p != '.') break;
                spec[n++] = *p++;
            }
            if (*p == '*') {
                n += (size_t)snprintf(spec + n, sizeof spec - n, "%d", (int)(int32_t)yw4_vararg_int(va));
                p++;
            } else {
                while (*p >= '0' && *p <= '9' && n < 30) spec[n++] = *p++;
            }
        }
        /* Length modifier: on AArch64, long/size_t/ptrdiff_t/intmax_t are all 64-bit. */
        int bits = 32;
        if (*p == 'h') { p++; bits = 16; if (*p == 'h') { p++; bits = 8; } }
        else if (*p == 'l') { p++; bits = 64; if (*p == 'l') p++; }
        else if (*p == 'z' || *p == 'j' || *p == 't') { p++; bits = 64; }
        else if (*p == 'L') {
            /* long double is 128-bit on AArch64 and passed in a q register: not supported. */
            return snprintf(out, size, "<unsupported %%L in guest format \"%s\">", fmt);
        }
        char conv = *p ? *p++ : '\0';
        int w = 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = (int64_t)yw4_vararg_int(va);
            v = bits == 64 ? v : bits == 32 ? (int32_t)v : bits == 16 ? (int16_t)v : (int8_t)v;
            memcpy(spec + n, PRId64, sizeof PRId64);
            w = snprintf(DST, ROOM, spec, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = yw4_vararg_int(va);
            v = bits == 64 ? v : bits == 32 ? (uint32_t)v : bits == 16 ? (uint16_t)v : (uint8_t)v;
            const char *m = conv == 'u' ? PRIu64 : conv == 'x' ? PRIx64 : conv == 'X' ? PRIX64 : PRIo64;
            memcpy(spec + n, m, strlen(m) + 1);
            w = snprintf(DST, ROOM, spec, v);
            break;
        }
        case 'c':
            spec[n++] = 'c', spec[n] = '\0';
            w = snprintf(DST, ROOM, spec, (int)(unsigned char)yw4_vararg_int(va));
            break;
        case 's': {
            const char *s = GPTR(yw4_vararg_int(va));
            spec[n++] = 's', spec[n] = '\0';
            w = snprintf(DST, ROOM, spec, s ? s : "(null)");
            break;
        }
        case 'p':
            w = snprintf(DST, ROOM, "0x%" PRIx64, yw4_vararg_int(va));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            spec[n++] = conv, spec[n] = '\0';
            w = snprintf(DST, ROOM, spec, yw4_vararg_double(va));
            break;
        case '%':
            w = snprintf(DST, ROOM, "%%");
            break;
        case 'n':
            yw4_vararg_int(va);  /* writes back a count: never seen in logs, skip the argument */
            break;
        default:
            return snprintf(out, size, "<unsupported %%%c in guest format \"%s\">", conv ? conv : '?', fmt);
        }
        if (w > 0) len += (size_t)w;
    }
    if (size) out[len < size ? len : size - 1] = '\0';
    return (int)len;
#undef ROOM
#undef DST
}

/* nn::diag::LogMetaData: where the log call is, then module name and severity. */
typedef struct {
    int32_t line;
    uint32_t pad;
    const char *file, *function, *module;
    int32_t severity;
} LogMetaData;

/* nn::diag::detail::LogImpl(const LogMetaData&, const char *format, ...) */
void hle_diag_LogImpl(CPUState *ctx) {
    const LogMetaData *meta = GPTR(ctx->x[0]);
    GuestVarargs va = yw4_varargs(ctx, 2, 0);
    char msg[1024];
    yw4_guest_format(msg, sizeof msg, GPTR(ctx->x[1]), &va);
    size_t n = strlen(msg);
    fprintf(stderr, "[game %s] %s%s", meta->function && *meta->function ? meta->function : "log", msg,
            n && msg[n - 1] == '\n' ? "" : "\n");
}

static const char *str_or(uint64_t p, const char *dflt) {
    const char *s = GPTR(p);
    return s && *s ? s : dflt;
}

/* nn::diag::detail::OnAssertionFailure(AssertionType, const char *condition,
 * const char *function, const char *file, int line). In a release build the strings are
 * empty, so the guest address of the call is the useful part. A failed assertion during
 * start-up means an HLE function returned something the game didn't expect: stop. */
void hle_diag_OnAssertionFailure(CPUState *ctx) {
    yw4_hle_stop(ctx, "game assertion failed (type %u): %s in %s at %s:%d", (unsigned)ctx->x[0],
                 str_or(ctx->x[1], "?"), str_or(ctx->x[2], "?"), str_or(ctx->x[3], "?"), (int)ctx->x[4]);
}

/* nn::diag::detail::AbortImpl(const char *condition, const char *function, const char *file, int line) */
void hle_diag_AbortImpl(CPUState *ctx) {
    yw4_hle_stop(ctx, "game abort: %s in %s at %s:%d", str_or(ctx->x[0], "?"), str_or(ctx->x[1], "?"),
                 str_or(ctx->x[2], "?"), (int)ctx->x[3]);
}

/* nndetailDiagAbortFormatImpl: a C function, so its signature isn't in the symbol. Stop and
 * show the raw arguments rather than guess their meaning. */
void hle_diag_AbortFormatImpl(CPUState *ctx) {
    yw4_hle_stop(ctx, "game abort (nndetailDiagAbortFormatImpl, arguments not decoded): "
                      "x0=%#" PRIx64 " x1=%#" PRIx64 " x2=%#" PRIx64 " x3=%#" PRIx64 " x4=%#" PRIx64 " x5=%#" PRIx64,
                 ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3], ctx->x[4], ctx->x[5]);
}
