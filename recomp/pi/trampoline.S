// Run one test instruction with a full register file loaded from memory.
//
//   void run_case(TestState *s)   (s in x0)
//
// TestState layout (must match testcase.h):
//   0x000  x[31]   x0..x30
//   0x0F8  sp
//   0x100  nzcv    flags in bits 31..28
//
// The harness copies this whole routine into an executable page and patches
// the word at `slot` with the instruction under test. The test instruction
// must not branch.
//
// SP is a test register too, so while the test runs the host stack is not
// usable. The state pointer and the host SP are parked in d30/d31 and the
// test's x0 in d29 (caller-saved SIMD registers, which the integer tests
// never touch; SIMD tests will have to avoid v29-v31).

    .text
    .global tramp_start, tramp_slot, tramp_end
    .p2align 2
tramp_start:
    // Save callee-saved registers and the frame.
    stp x29, x30, [sp, #-96]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    fmov d30, x0                 // state pointer
    mov x1, sp
    fmov d31, x1                 // host SP

    ldr x1, [x0, #0x100]
    msr nzcv, x1
    ldr x1, [x0, #0xF8]
    mov sp, x1
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

    fmov d29, x0                 // free x0 to hold the state pointer again
    fmov x0, d30
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
    fmov x1, d29                 // the test's x0 result
    str x1, [x0, #0x00]
    fmov x1, d31
    mov sp, x1                   // host stack back

    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #96
    ret
tramp_end:
