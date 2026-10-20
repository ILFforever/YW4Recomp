// Exclusive-access loops in the shapes main uses (32-bit and byte LDAXR/STLXR, LDAR/STLR,
// CLREX on the failure path). Same calling convention as funcs.c: x0 = buf, x1 = a, x2 = b,
// x3 = c. Single-threaded, so every STLXR after a matching LDAXR must succeed.

    .text
    .p2align 2

// Atomic add of a to the 32-bit word buf[0], (b & 15) + 1 times. Returns the last old value.
    .global t_atomic_add
t_atomic_add:
    and   w9, w2, #15
    add   w9, w9, #1
.Lretry_add:
    ldaxr w8, [x0]
    add   w10, w8, w1
    stlxr w11, w10, [x0]
    cbnz  w11, .Lretry_add
    subs  w9, w9, #1
    b.ne  .Lretry_add
    mov   w0, w8
    ret

// Compare-and-swap on the word at buf+4: if it equals the expected value, store b.
// Half the time (a odd) the expected value is the current one, so both paths run.
// Returns old value << 1 | stored.
    .global t_cas
t_cas:
    add   x12, x0, #4
    ldr   w13, [x12]
    tst   w1, #1
    csel  w1, w13, w1, ne
.Lretry_cas:
    ldaxr w8, [x12]
    cmp   w8, w1
    b.ne  .Lcas_fail
    stlxr w9, w2, [x12]
    cbnz  w9, .Lretry_cas
    lsl   x0, x8, #1
    orr   x0, x0, #1
    ret
.Lcas_fail:
    clrex
    lsl   x0, x8, #1
    ret

// A store-exclusive without a reservation (after CLREX) must fail and store nothing.
// Returns the status (1).
    .global t_stxr_no_reservation
t_stxr_no_reservation:
    ldaxr w8, [x0]
    clrex
    stlxr w9, w1, [x0]
    mov   w0, w9
    ret

// Byte and 64-bit exclusives, plain (no acquire/release) forms.
    .global t_exclusive_sizes
t_exclusive_sizes:
    add   x12, x0, #16
.Lretry_b:
    ldxrb w8, [x12]
    eor   w8, w8, w1
    stxrb w9, w8, [x12]
    cbnz  w9, .Lretry_b
    add   x13, x0, #24
.Lretry_x:
    ldxr  x10, [x13]
    add   x10, x10, x2
    stxr  w9, x10, [x13]
    cbnz  w9, .Lretry_x
    add   x0, x10, x8
    ret

// Load-acquire / store-release, byte and word.
    .global t_acquire_release
t_acquire_release:
    add   x9, x0, #32
    ldarb w8, [x9]
    add   w8, w8, w1
    stlrb w8, [x9]
    add   x10, x0, #40
    ldar  w11, [x10]
    eor   w11, w11, w2
    stlr  w11, [x10]
    ldar  x12, [x0]
    add   x0, x12, x11
    add   x0, x0, x8
    ret
