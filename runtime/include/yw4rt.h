/* What generated code needs from the runtime besides the CPU helpers in yw4cpu.h.
 *
 * Every guest function is `void f_<offset>(CPUState *ctx)` (decision 0007).
 */
#ifndef YW4RT_H
#define YW4RT_H

#include <stddef.h>
#include <stdint.h>

#include "yw4cpu.h"

typedef void (*GuestFn)(CPUState *ctx);

/* Host addresses of the recompiled modules: main and the sdk (decision 0014). Guest
 * addresses are host addresses (decision 0005), so PC-relative values and return addresses
 * are module offsets plus these. Must be 4 KiB aligned, because ADRP results are computed
 * at translation time from page offsets. */
extern uint64_t yw4_image_base;
extern uint64_t yw4_sdk_base;

/* The recompiler's tables of every translated function, one per module, sorted by offset
 * (generated func_table.c). */
typedef struct {
    uint64_t offset;
    GuestFn fn;
} FuncEntry;
typedef struct {
    const char *name;
    const uint64_t *base;   /* where the module was loaded */
    uint64_t size;          /* image + .bss */
    const FuncEntry *funcs;
    size_t nfuncs;
} Yw4CodeModule;
extern const Yw4CodeModule yw4_code[];
extern const size_t yw4_ncode;

/* "main+0x1234", "sdk+0x56" or the raw address, for messages. Returns buf. */
const char *yw4_describe(uint64_t addr, char buf[48]);

/* Function at a guest address, or NULL. */
GuestFn yw4_lookup(uint64_t addr);

/* Optional extra lookup tried first (the loader uses it for import stub addresses). */
extern GuestFn (*yw4_lookup_hook)(uint64_t addr);

/* Function at a guest address for an indirect call or tail call (BLR, BR).
 * Traps if no translated function starts there. */
GuestFn yw4_resolve(uint64_t target);

/* An instruction the recompiler can't translate yet, or a guest trap (UDF/BRK). */
_Noreturn void yw4_unimplemented(CPUState *ctx, uint64_t pc, uint32_t enc);
_Noreturn void yw4_guest_trap(CPUState *ctx, uint64_t pc, uint32_t enc);

/* System instructions (decision 0014, runtime/src/kernel.c): SVC #imm, the system counter
 * (CNTPCT_EL0, 19.2 MHz as on the Switch), and writes to FPCR (only the default mode is
 * supported, decision 0008). */
void yw4_svc(CPUState *ctx, uint32_t imm);
uint64_t yw4_cntpct(void);
void yw4_msr_fpcr(CPUState *ctx, uint64_t value);

/* Tail call to another guest function. Clang guarantees no host stack growth with musttail,
 * which matters for loops that go through tail calls; other compilers usually optimise the
 * plain call+return to a jump at -O2 but don't promise it. */
#if defined(__clang__)
/* Wrapped in a block: right after a label, a bare attribute would attach to a declaration. */
#define TAILCALL(f) do { __attribute__((musttail)) return f(ctx); } while (0)
#define TAILCALL_INDIRECT(t) do { GuestFn fn_ = yw4_resolve(t); __attribute__((musttail)) return fn_(ctx); } while (0)
#else
#define TAILCALL(f) do { f(ctx); return; } while (0)
#define TAILCALL_INDIRECT(t) do { yw4_resolve(t)(ctx); return; } while (0)
#endif

#endif
