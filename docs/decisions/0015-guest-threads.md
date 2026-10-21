# 0015 — Guest threads are host threads; one kernel lock

Date: 2026-10-21
Status: accepted

## Context

With the sdk recompiled (0014), the game makes its own threads through `svcCreateThread` and synchronises them through the kernel: `ArbitrateLock/Unlock` (mutexes), `WaitProcessWideKeyAtomic`/`SignalProcessWideKey` (condition variables), `WaitSynchronization` (events, thread exit), `WaitForAddress`/`SignalToAddress` (address arbiter). Until now the kernel had one thread and stand-ins that could never block. Main's start-up stopped at its first `CreateThread`.

## Decision

**One host thread per guest thread.** Each has its own `CPUState` and its own thread-local region (TLR, `TPIDRRO_EL0`). Guest code runs truly in parallel on the host's cores, with no lock held. This is what decision 0005 assumed ("guest pointers are host pointers, shared memory") and what 0009's atomics were built for. LAS Recomp does the same.

**One kernel lock (`klock`).** Every SVC runs holding it, services included (they are called from `SendSyncRequest`). A thread that has to block waits on its own host condition variable, which releases the lock meanwhile. Why one lock: kernel calls are rare next to guest code (the fast path of a mutex is pure guest code), so contention on it is not a cost worth designing for yet, and one lock makes every kernel operation trivially atomic. If profiling ever shows the lock hot, it can be split.

**Semantics follow Atmosphère's kernel (mesosphere)**, step by step:
- Mutex word: 0 = free, else the owner's handle, bit 30 = someone waits. `ArbitrateLock` waits only if the word still reads "owner | waiters"; otherwise it returns and the guest retries. `ArbitrateUnlock` hands the mutex to the best waiter (highest priority, then first come), keeping bit 30 if others still wait.
- Condition variable: `WaitProcessWideKeyAtomic` releases the mutex (as unlock does), writes 1 to the key, waits. A signalled waiter takes the mutex back if it is free, else joins the mutex's queue; the key goes back to 0 when no waiter is left. **On a timeout the kernel does not re-take the mutex**: the sdk does that itself (libnx does the same). The single-thread stand-in had this wrong.
- `WaitSynchronization`: events and threads (signalled when they exit); `CancelSynchronization` ends a wait with `ResultCancelled`, or the next one if the thread isn't waiting.
- Address arbiter: `WaitIfLessThan`, `DecrementAndWaitIfLessThan`, `WaitIfEqual`; `Signal`, `SignalAndIncrementIfEqual`. `SignalAndModifyByWaitingCount` stops with a message until something uses it and its value rule is checked.
- Words in guest memory that guest code changes at the same time are changed with host atomics (compare-and-swap loops), as mesosphere uses exclusive accesses.

**Thread life.** `CreateThread` makes the thread suspended (TLR page zeroed, `x0` = argument, `sp` = stack top); `StartThread` starts its host thread (16 MiB host stack, reserved only, for the recompiled code's C frames; the guest stack is the game's). `ExitThread` jumps back to the host thread's entry (`__builtin_setjmp/longjmp`, which skips the generated C frames without unwinding them: they hold nothing to clean up). A thread is freed once it has exited and its handle is closed.

**TLRs**: one 4 KiB page per thread slot at `0x7040000000` (256 slots, reserved at start-up). Horizon packs 0x200-byte regions eight to a page; the guest can't tell the difference.

**Deadlock detector.** The kernel counts threads that can still run (timed waits count as runnable). If every thread waits with no timeout, nothing can ever wake them, because nothing outside guest threads signals anything yet. The kernel then stops and prints each thread's wait and backtrace instead of hanging. When native services start signalling events from host threads (audio, vsync), this check has to learn about them.

**Host layer** (`runtime/src/host.c`): lock, condition variable, thread start, yield, monotonic clock, for Windows (SRW lock, condition variable, `_beginthreadex`, `QueryPerformanceCounter`) and Linux (pthreads, `CLOCK_MONOTONIC`). The system counter (`CNTPCT_EL0`) now uses the monotonic clock.

## What we don't do (known differences)

- **Priorities and core masks are recorded, not enforced.** The host schedules. On the Switch, two threads pinned to the same core never run at the same time, and a higher-priority thread on a core runs until it blocks. Code that relies on that instead of locks would race here. Emulators run the same games with host scheduling, so this is usually safe; if a race turns up, the fix is per-core run locks for threads pinned to one core.
- **Timeout resolution on Windows is 1 ms** (waits round up, never short). Frame pacing may want better later.
- **No priority inheritance** for mutex owners.
- Exclusives keep 0009's ABA difference.

## Testing

`tests/test_kernel.py` builds the kernel with a C driver (no game) whose guest threads use an nn::os-style mutex and condition variable, and checks:
- 8 threads × 20,000 increments of a plain counter under a contended mutex (the kernel path must actually run);
- a bounded queue with 4 producers and 4 consumers on two condition variables (count and sum);
- joining threads through `WaitSynchronization`, half ending by `ExitThread`, half by returning;
- timeouts (event wait, condition variable: mutex not held after a timeout, timeout 0), events, `CancelSynchronization` (while waiting and pending), the address arbiter;
- **0009's exclusives between real threads**: 8 × 100,000 increments with the recompiler's own `ldaxr`/`stlxr` output;
- the deadlock detector stops (exit code 3) instead of hanging.

Planted bugs caught: unlock drops the waiters bit, unlock hands over without waking, a signalled waiter skips taking the mutex, thread exit not signalled, a pending cancel ignored, `stlxr` as a plain store (167,482 of 800,000 increments survived). The test also found a real bug in the first version: a thread woken but not yet running again was counted as blocked, which made the deadlock detector fire falsely.

## Result

Main's start-up creates and starts 8 threads (file loading and others), which take mutexes, wait on condition variables and send IPC requests from their own threads. With transfer memory added (`svcCreateTransferMemory`: the game lends a block of its memory to a service), the game reaches its first graphics service, `nvdrv`.
