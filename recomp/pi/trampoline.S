// Run one test instruction with a full register file loaded from memory.
//
//   void run_case(TestState *s)   (s in x0)
//
// TestState layout (must match testcase.h):
//   0x000  x[31]   x0..x30
//   0x0F8  sp
//   0x100  nzcv    flags in bits 31..28
//   0x110  v[32]   v0..v31, 16 bytes each
//   0x310  fpcr
//   0x318  fpsr
//   0x320  host_sp (the harness's own SP while the test runs)
//
// The harness copies this whole routine into an executable page and patches
// the word at `slot` with the instruction under test. The test instruction
// must not branch.
//
// SP is a test register too, so the host stack is not usable while the test
// runs. The host SP is saved in the state block, and the state pointer is
// parked in v31 (d[0]; the test's x0 goes to d[1] afterwards). Tests must
// therefore never use v31; its result is not compared.

    .text
    .global tramp_start, tramp_slot, tramp_end
    .p2align 2
tramp_start:
    // Save callee-saved registers (x19-x28, d8-d15) and the frame.
    stp x29, x30, [sp, #-160]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x1, sp
    str x1, [x0, #0x320]         // host SP
    fmov d31, x0                 // state pointer

    ldr x1, [x0, #0x100]
    msr nzcv, x1
    ldr x1, [x0, #0x310]
    msr fpcr, x1
    ldr x1, [x0, #0x318]
    msr fpsr, x1
    ldr x1, [x0, #0xF8]
    mov sp, x1
    add x1, x0, #0x110
    ldp q0, q1, [x1, #0x000]
    ldp q2, q3, [x1, #0x020]
    ldp q4, q5, [x1, #0x040]
    ldp q6, q7, [x1, #0x060]
    ldp q8, q9, [x1, #0x080]
    ldp q10, q11, [x1, #0x0A0]
    ldp q12, q13, [x1, #0x0C0]
    ldp q14, q15, [x1, #0x0E0]
    ldp q16, q17, [x1, #0x100]
    ldp q18, q19, [x1, #0x120]
    ldp q20, q21, [x1, #0x140]
    ldp q22, q23, [x1, #0x160]
    ldp q24, q25, [x1, #0x180]
    ldp q26, q27, [x1, #0x1A0]
    ldp q28, q29, [x1, #0x1C0]
    ldr q30, [x1, #0x1E0]
    ldp x1, x2, [x0, #0x08]
    ldp x3, x4, [x0, #0x18]
    ldp x5, x6, [x0, #0x28]
    ldp x7, x8, [x0, #0x38]
    ldp x9, x10, [x0, #0x48]
    ldp x11, x12, [x0, #0x58]
    ldp x13, x14, [x0, #0x68]
    ldp x15, x16, [x0, #0x78]
    ldp x17, x18, [x0, #0x88]
    ldp x19, x20, [x0, #0x98]
    ldp x21, x22, [x0, #0xA8]
    ldp x23, x24, [x0, #0xB8]
    ldp x25, x26, [x0, #0xC8]
    ldp x27, x28, [x0, #0xD8]
    ldp x29, x30, [x0, #0xE8]
    ldr x0, [x0, #0x00]          // x0 last: it was the base register

tramp_slot:
    nop                          // patched with the instruction under test

    mov v31.d[1], x0             // free x0 to hold the state pointer again
    fmov x0, d31
    stp x1, x2, [x0, #0x08]
    stp x3, x4, [x0, #0x18]
    stp x5, x6, [x0, #0x28]
    stp x7, x8, [x0, #0x38]
    stp x9, x10, [x0, #0x48]
    stp x11, x12, [x0, #0x58]
    stp x13, x14, [x0, #0x68]
    stp x15, x16, [x0, #0x78]
    stp x17, x18, [x0, #0x88]
    stp x19, x20, [x0, #0x98]
    stp x21, x22, [x0, #0xA8]
    stp x23, x24, [x0, #0xB8]
    stp x25, x26, [x0, #0xC8]
    stp x27, x28, [x0, #0xD8]
    stp x29, x30, [x0, #0xE8]
    mov x1, sp
    str x1, [x0, #0xF8]
    mrs x1, nzcv
    str x1, [x0, #0x100]
    mrs x1, fpcr
    str x1, [x0, #0x310]
    mrs x1, fpsr
    str x1, [x0, #0x318]
    add x1, x0, #0x110
    stp q0, q1, [x1, #0x000]
    stp q2, q3, [x1, #0x020]
    stp q4, q5, [x1, #0x040]
    stp q6, q7, [x1, #0x060]
    stp q8, q9, [x1, #0x080]
    stp q10, q11, [x1, #0x0A0]
    stp q12, q13, [x1, #0x0C0]
    stp q14, q15, [x1, #0x0E0]
    stp q16, q17, [x1, #0x100]
    stp q18, q19, [x1, #0x120]
    stp q20, q21, [x1, #0x140]
    stp q22, q23, [x1, #0x160]
    stp q24, q25, [x1, #0x180]
    stp q26, q27, [x1, #0x1A0]
    stp q28, q29, [x1, #0x1C0]
    str q30, [x1, #0x1E0]
    mov x1, v31.d[1]             // the test's x0 result
    str x1, [x0, #0x00]
    ldr x1, [x0, #0x320]
    mov sp, x1                   // host stack back
    mov x1, xzr
    msr fpcr, x1                 // host FP mode back (the C ABI expects the default)

    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #160
    ret
tramp_end:
