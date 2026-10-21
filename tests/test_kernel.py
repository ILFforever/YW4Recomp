"""Guest threads on the runtime's kernel (runtime/src/kernel.c, decision 0015), without the game.

The "guest" code is C calling yw4_svc the way the sdk does; its mutex and condition variable
follow nn::os / libnx. The exclusive-increment loop is the recompiler's own output
(lift.lift), so decision 0009's atomics get tested between real threads.
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

from recomp import check, lift

RUNTIME = check.REPO / "runtime"

DRIVER = r"""
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "yw4load.h"

/* What the kernel links against, with no game: no modules, no services. */
const Yw4Module yw4_modules[1];
const size_t yw4_nmodules = 0;
const uint64_t yw4_entry_init_start = 0, yw4_main_stack_size = 0x1000;
const uint32_t yw4_main_priority = 44;
const Yw4CodeModule yw4_code[1];
const size_t yw4_ncode = 0;
const Yw4GuestSymbol yw4_guest_symbols[1];
const size_t yw4_nguest_symbols = 0;
uint32_t ipc_connect_named_port(const char *name, uint32_t *h) { (void)name, (void)h; return 1; }
uint32_t ipc_send_request(uint32_t h, uint64_t m) { (void)h, (void)m; return 1; }
int ipc_close_handle(uint32_t h) { (void)h; return 0; }

#define FAIL(...) do { printf(__VA_ARGS__); puts(""); fflush(stdout); exit(1); } while (0)
#define W 0x40000000u
#define TIMEOUT 0xEA01u
#define FOREVER ((uint64_t)-1)
#define A(p) ((uint64_t)(uintptr_t)(p))

static uint64_t sv(CPUState *c, uint32_t imm, uint64_t a, uint64_t b, uint64_t d, uint64_t e) {
    c->x[0] = a, c->x[1] = b, c->x[2] = d, c->x[3] = e;
    yw4_svc(c, imm);
    return c->x[0];
}

/* nn::os / libnx mutex: 0 = free, else the owner's handle, W = someone waits. Each test
 * thread keeps its own handle in x28. */
static int arbitrations;
static uint32_t self_tag(CPUState *c) { return (uint32_t)c->x[28]; }
static void lock(CPUState *c, uint32_t *m) {
    uint32_t self = self_tag(c), v = __atomic_load_n(m, __ATOMIC_ACQUIRE);
    for (;;) {
        if (v == 0) {
            if (__atomic_compare_exchange_n(m, &v, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) return;
            continue;
        }
        if ((v & ~W) == self) FAIL("thread %#x: the mutex is already mine", self);
        if (!(v & W) && !__atomic_compare_exchange_n(m, &v, v | W, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) continue;
        __atomic_add_fetch(&arbitrations, 1, __ATOMIC_RELAXED);
        if (sv(c, 0x1A, v & ~W, A(m), self, 0)) FAIL("ArbitrateLock failed");
        v = __atomic_load_n(m, __ATOMIC_ACQUIRE);
        if ((v & ~W) == self) return;
    }
}
static void unlock(CPUState *c, uint32_t *m) {
    uint32_t self = self_tag(c), v = self;
    if (__atomic_compare_exchange_n(m, &v, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    if ((v & ~W) != self) FAIL("unlock of a mutex that isn't mine: %#x", v);
    sv(c, 0x1B, A(m), 0, 0, 0);
}
static uint32_t cv_wait(CPUState *c, uint32_t *m, uint32_t *cv, int64_t timeout) {
    uint32_t r = (uint32_t)sv(c, 0x1C, A(m), A(cv), self_tag(c), (uint64_t)timeout);
    if (r == TIMEOUT) {  /* the kernel does not take the mutex back on a timeout */
        if ((__atomic_load_n(m, __ATOMIC_ACQUIRE) & ~W) == self_tag(c)) FAIL("timed-out wait came back holding the mutex");
        lock(c, m);
    } else if (r) {
        FAIL("WaitProcessWideKeyAtomic: %#x", r);
    } else if ((__atomic_load_n(m, __ATOMIC_ACQUIRE) & ~W) != self_tag(c)) {
        FAIL("woken without the mutex");
    }
    return r;
}
static void cv_signal(CPUState *c, uint32_t *cv, int32_t n) { sv(c, 0x1D, A(cv), (uint64_t)(uint32_t)n, 0, 0); }

/* ---- the tests' guest threads: x0 = index */

static uint32_t handles[64];
static CPUState main_ctx;
static int which;

static uint32_t mtx, counter;
static uint32_t qm, not_empty, not_full, queue[4], qhead, qcount;
static uint64_t consumed_sum, consumed_n;
static uint32_t ev_w, ev_r, word, excl_word;
static volatile uint32_t sink;

static void lifted_increment(CPUState *ctx) {  /* again: ldaxr w1,[x0]; add w1,w1,#1; stlxr w2,w1,[x0]; cbnz w2, again */
again:
    LDAXR
    ADD
    STLXR
    if ((uint32_t)ctx->x[2]) goto again;
}

static void entry(CPUState *c) {
    int i = (int)c->x[0];
    c->x[28] = handles[i];
    switch (which) {
    case 1:  /* mutex: a non-atomic counter, with a slow critical section */
        for (int k = 0; k < 20000; k++) {
            lock(c, &mtx);
            uint32_t v = counter;
            for (int s = 0; s < 20; s++) sink = sink + 1;
            counter = v + 1;
            unlock(c, &mtx);
        }
        break;
    case 2:  /* bounded queue: even threads produce, odd ones consume */
        for (int k = 0; k < 5000; k++) {
            lock(c, &qm);
            if (i % 2 == 0) {
                while (qcount == 4) cv_wait(c, &qm, &not_full, -1);
                queue[(qhead + qcount++) % 4] = (uint32_t)(i * 100000 + k);
                cv_signal(c, &not_empty, 1);
            } else {
                while (qcount == 0) cv_wait(c, &qm, &not_empty, -1);
                consumed_sum += queue[qhead];
                consumed_n++;
                qhead = (qhead + 1) % 4, qcount--;
                cv_signal(c, &not_full, 1);
            }
            unlock(c, &qm);
        }
        break;
    case 3: {  /* wait for the event; the outcome goes in counter */
        uint32_t r = (uint32_t)sv(c, 0x18, 0, A(&ev_r), 1, FOREVER);
        __atomic_store_n(&counter, r == 0 && c->x[1] == 0 ? 1 : 0x10000 | r, __ATOMIC_SEQ_CST);
        break;
    }
    case 4: {  /* address arbiter: wait while word == 0 */
        uint32_t r = (uint32_t)sv(c, 0x34, A(&word), 2, 0, FOREVER);
        __atomic_store_n(&counter, r == 0 ? 1 : 0x10000 | r, __ATOMIC_SEQ_CST);
        break;
    }
    case 5:  /* exclusives under contention */
        for (int k = 0; k < 100000; k++) {
            c->x[0] = A(&excl_word);
            lifted_increment(c);
        }
        break;
    }
    if (i % 2) sv(c, 0x0A, 0, 0, 0, 0);  /* odd threads end with ExitThread, even ones by returning */
}

static void start(int n) {
    uint64_t fn = yw4_host_function(entry);
    CPUState *c = &main_ctx;
    for (int i = 0; i < n; i++) {
        if (sv(c, 0x08, 0, fn, (uint64_t)i, 0x7000100000ull) != 0) FAIL("CreateThread");
        handles[i] = (uint32_t)c->x[1];
        if (sv(c, 0x09, handles[i], 0, 0, 0) != 0) FAIL("StartThread");
    }
}
static void join(int n) {
    CPUState *c = &main_ctx;
    for (int i = 0; i < n; i++) {
        uint32_t r = (uint32_t)sv(c, 0x18, 0, A(&handles[i]), 1, FOREVER);
        if (r != 0 || c->x[1] != 0) FAIL("join thread %d: %#x", i, r);
        sv(c, 0x16, handles[i], 0, 0, 0);
    }
}
static void settle(void) { sv(&main_ctx, 0x0B, 20000000, 0, 0, 0); }  /* 20 ms for a thread to reach its wait */
static void wait_counter(uint32_t want) {
    for (int k = 0; k < 2000 && __atomic_load_n(&counter, __ATOMIC_SEQ_CST) != want; k++) sv(&main_ctx, 0x0B, 1000000, 0, 0, 0);
}

int main(int argc, char **argv) {
    CPUState *c = &main_ctx;
    c->x[28] = yw4_kernel_init(c, 0x7000000000ull, 0x1000);
    sv(c, 0x45, 0, 0, 0, 0);
    ev_w = (uint32_t)c->x[1], ev_r = (uint32_t)c->x[2];

    if (argc > 1 && !strcmp(argv[1], "deadlock")) {  /* nobody can signal this: must stop, not hang */
        which = 3, start(1);
        sv(c, 0x18, 0, A(&ev_r), 1, FOREVER);
        FAIL("a wait nobody can end returned");
    }

    which = 1, start(8), join(8);
    if (counter != 8 * 20000) FAIL("mutex: counter %u, want %u", counter, 8 * 20000);
    if (!arbitrations) FAIL("mutex: never contended, so the kernel path wasn't tested");
    if (mtx != 0) FAIL("mutex word left at %#x", mtx);

    which = 2, start(8), join(8);
    uint64_t want = 0;
    for (int i = 0; i < 8; i += 2)
        for (int k = 0; k < 5000; k++) want += (uint64_t)(i * 100000 + k);
    if (consumed_n != 4 * 5000 || consumed_sum != want) FAIL("queue: %" PRIu64 " items, sum %" PRIu64, consumed_n, consumed_sum);

    /* timeouts */
    int64_t t0 = host_now_ns();
    uint32_t r = (uint32_t)sv(c, 0x18, 0, A(&ev_r), 1, 20000000);
    if (r != TIMEOUT) FAIL("event wait with a timeout: %#x", r);
    if (host_now_ns() - t0 < 20000000) FAIL("the timeout came early");
    lock(c, &mtx);
    if (cv_wait(c, &mtx, &not_empty, 10000000) != TIMEOUT) FAIL("condition variable timeout");
    unlock(c, &mtx);
    lock(c, &mtx);
    if (sv(c, 0x1C, A(&mtx), A(&not_empty), self_tag(c), 0) != TIMEOUT) FAIL("condition variable timeout 0");
    if (mtx != 0) FAIL("timeout 0 kept the mutex");

    /* events */
    which = 3, counter = 0, start(1), settle();
    if (counter) FAIL("event wait ended before the signal");
    sv(c, 0x11, ev_w, 0, 0, 0);
    wait_counter(1);
    if (counter != 1) FAIL("event wait: %#x", counter);
    join(1);
    sv(c, 0x12, ev_w, 0, 0, 0);

    /* CancelSynchronization: of a waiting thread, then one pending before the wait */
    which = 3, counter = 0, start(1), settle();
    sv(c, 0x19, handles[0], 0, 0, 0);
    wait_counter(0x10000 | 0xEC01);
    if (counter != (0x10000 | 0xEC01)) FAIL("cancel: %#x", counter);
    join(1);
    sv(c, 0x19, 0xFFFF8000u, 0, 0, 0);  /* pending, through the current-thread pseudo-handle */
    if (sv(c, 0x18, 0, A(&ev_r), 1, FOREVER) != 0xEC01) FAIL("pending cancel");

    /* address arbiter */
    word = 1;
    if (sv(c, 0x34, A(&word), 2, 0, FOREVER) != 0xFA01) FAIL("WaitIfEqual on another value");
    word = 0, which = 4, counter = 0, start(1), settle();
    if (counter) FAIL("address wait ended before the signal");
    if (sv(c, 0x35, A(&word), 1, 0, 1) != 0 || word != 1) FAIL("SignalAndIncrementIfEqual");
    wait_counter(1);
    if (counter != 1) FAIL("address wait: %#x", counter);
    join(1);

    which = 5, start(8), join(8);
    if (excl_word != 8 * 100000) FAIL("exclusives: %u increments, want %u", excl_word, 8 * 100000);

    puts("ok");
    return 0;
}
"""


class KernelThreadTest(unittest.TestCase):
    """Guest threads as host threads: blocking, waking, timeouts, exclusives (decision 0015)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        t = Path(cls.tmp.name)
        src = (DRIVER.replace("LDAXR", lift.lift(0x885FFC01)).replace("ADD", lift.lift(0x11000421))
               .replace("STLXR", lift.lift(0x8802FC01)))
        (t / "drv.c").write_text(src)
        cls.exe = t / "kernel.exe"
        rt = RUNTIME / "src"
        files = [rt / f for f in ("kernel.c", "host.c", "loader.c", "dispatch.c", "sha256.c")]
        subprocess.run([check.host_cc(), "-std=c11", "-O1", "-Wall", "-Werror", f"-I{RUNTIME / 'include'}", f"-I{rt}",
                        "-o", str(cls.exe), str(t / "drv.c"), *map(str, files), "-pthread"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_threads(self):
        got = subprocess.run([str(self.exe)], capture_output=True, text=True, timeout=120)
        self.assertEqual(got.stdout.strip(), "ok", got.stderr[-2000:])

    def test_deadlock_stops(self):
        got = subprocess.run([str(self.exe), "deadlock"], capture_output=True, text=True, timeout=60)
        self.assertEqual(got.returncode, 3, got.stdout + got.stderr[-2000:])
        self.assertIn("every guest thread waits forever", got.stderr)


if __name__ == "__main__":
    unittest.main()
