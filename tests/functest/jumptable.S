// Switch tables in the shape Clang emits them in main (recomp/function.py, find_jump_table).
// Same calling convention as funcs.c: x0 = buf, x1 = a, x2 = b, x3 = c.

    .text
    .p2align 2

// Plain table right before the BR.
    .global t_jumptable
t_jumptable:
    and   w8, w1, #7
    cmp   w8, #4
    b.hi  .Ldefault1
    adrp  x9, table1
    add   x9, x9, :lo12:table1
    ldrsw x8, [x9, x8, lsl #2]
    add   x8, x8, x9
    br    x8
.Lc10: add   x0, x2, x3
    ret
.Lc11: sub   x0, x2, x3
    ret
.Lc12: mul   x0, x2, x3
    ret
.Lc13: eor   x0, x2, x3
    b     .Lplus1
.Lc14: lsr   x0, x2, #3
    ret
.Lplus1:
    add   x0, x0, #1
    ret
.Ldefault1:
    mov   x0, x1
    ret

// Table address set up once before a loop and copied to another register,
// as Clang does when it hoists it out of the loop.
    .global t_jumptable_loop
t_jumptable_loop:
    adrp  x10, table2
    add   x10, x10, :lo12:table2
    mov   x11, x10
    mov   x0, xzr
    and   x4, x1, #15            // iterations
.Lloop:
    cbz   x4, .Ldone
    and   x8, x2, #3
    lsr   x2, x2, #2
    sub   x4, x4, #1
    cmp   w8, #2
    b.hi  .Ldefault2
    ldrsw x8, [x11, x8, lsl #2]
    add   x8, x8, x11
    br    x8
.Lc20: add   x0, x0, #1
    b     .Lloop
.Lc21: add   x0, x0, x3
    b     .Lloop
.Lc22: lsl   x0, x0, #1
    b     .Lloop
.Ldefault2:
    eor   x0, x0, #0xff
    b     .Lloop
.Ldone:
    ret

    .section .rodata
    .p2align 2
table1:
    .word .Lc10 - table1, .Lc11 - table1, .Lc12 - table1, .Lc13 - table1, .Lc14 - table1
table2:
    .word .Lc20 - table2, .Lc21 - table2, .Lc22 - table2
