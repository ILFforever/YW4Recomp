# 0018 — setjmp and longjmp in recompiled code

Date: 2026-10-21
Status: accepted

## Problem

The game's Lua (5.2) handles script errors with `setjmp`/`longjmp` (`luaD_rawrunprotected` in main, through the PLT to the sdk's musl `_setjmp`). The guest `setjmp` saves x19-x30, sp and d8-d15; `longjmp` restores them and ends with `br x30`, a jump to the return address of the `setjmp` call. In recompiled code that address is the middle of a C function, not a function, so the jump stopped with "indirect call to main+0xd2ce38: no translated function there".

Measured: one `setjmp` call site in main, four in the sdk. There is no `sigsetjmp` and no other `longjmp` user.

## Decision

- **The translator** (`recomp/translate.py`, `function.py`) finds the setjmp functions: the sdk's `_setjmp`/`setjmp`/`__setjmp` (one address), and in each module the PLT stubs whose GOT slot resolves to it. At each call to one, it emits a host `setjmp` in the calling function's own C frame, with the `jmp_buf` declared at the function's top level so it lives as long as the frame. On the first return, it registers the point and calls the guest `setjmp` as before.
- **The runtime** (`runtime/src/setjmp.c`) keeps the registered points per thread, keyed by (return address, guest sp), ordered outermost first. A new point drops the deeper ones, whose frames must have returned. When `yw4_resolve` misses, it asks `yw4_setjmp_resume`. A matching point is jumped to with the host `longjmp`, which lands in that frame right after the call. The guest registers are already the restored ones, and x0 is the longjmp value.
- **The C library's `setjmp`/`longjmp`, not `__builtin_setjmp`.** The first version used the GCC/Clang builtins, which are cheaper and don't unwind. On Windows x64 they broke. In a frame over 128 bytes Clang sets `rbp = rsp + 0x80` and addresses locals through `rbp`, but `__builtin_setjmp` saves the canonical frame address, so after the jump `rbp` was 0x80 too low and `ctx` was read from the wrong slot (an access violation in `f_d2cde0`, seen in the disassembly). The CRT's `longjmp` unwinds properly; on Windows that's `RtlUnwind` over the unwind data Clang emits for every x64 function. Lua errors are rare, so the cost doesn't matter. The kernel's `ExitThread` jump (0015) moved to the CRT pair as well: it only worked before because `thread_main`'s frame is small.

## Also in this step: a crash handler

A host crash (access violation, segmentation fault) now prints the host address, the offset in `yw4.exe` (look it up with `llvm-nm -n`), and the guest thread's backtrace (`host_install_crash_handler`, `yw4_stop`). An indirect call to nowhere prints the backtrace too.

## Testing

`tests/test_kernel.py` repeats what generated code does three times: a caller with a 320-byte frame does `setjmp` and registers, a "guest longjmp" six calls deeper restores sp and branches to the return address, and the caller checks x0, sp and its locals. A planted wrong jump fails the test.

## Result

The game passes its first Lua error and stays in its frame loop: 8,400 frames in 13 minutes without a stop. That is about 11 frames per second, not 60, still to be explained. Every frame shows the same 27 draws, so it is probably waiting on one screen.
