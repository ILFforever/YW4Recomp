/* The Horizon kernel as the recompiled sdk sees it (decision 0014).
 *
 * Starts empty: every SVC stops with its number, the same way an unimplemented import
 * stops with its name. SVCs get implemented as the start-up path reaches them.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "yw4rt.h"

void yw4_svc(CPUState *ctx, uint32_t imm) {
    fprintf(stderr, "yw4: unimplemented SVC %#x (x0=%#" PRIx64 " x1=%#" PRIx64 " x2=%#" PRIx64 ")\n",
            imm, ctx->x[0], ctx->x[1], ctx->x[2]);
    exit(3);
}

/* CNTPCT_EL0 counts at 19.2 MHz on the Switch. */
uint64_t yw4_cntpct(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);  /* C11; a monotonic clock comes with the real kernel */
    unsigned __int128 ns = (unsigned __int128)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    return (uint64_t)(ns * 192u / 10000u);
}

void yw4_msr_fpcr(CPUState *ctx, uint64_t value) {
    if (value != 0) {  /* rounding modes, flush-to-zero, default NaN: not modelled (decision 0008) */
        fprintf(stderr, "yw4: guest sets FPCR to %#" PRIx64 "; only 0 is supported\n", value);
        exit(3);
    }
    ctx->fpcr = 0;
}
