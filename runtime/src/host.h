/* Host threads, locks, condition variables and a monotonic clock: the little the kernel
 * needs from the host OS, for Windows and Linux. Private to the runtime. */
#ifndef HOST_H
#define HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
/* SRWLOCK and CONDITION_VARIABLE are one pointer each; zero is their initial state. */
typedef struct { void *p; } HostLock;
typedef struct { void *p; } HostCond;
#define HOST_LOCK_INIT {0}
#else
#include <pthread.h>
typedef pthread_mutex_t HostLock;
typedef pthread_cond_t HostCond;
#define HOST_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#endif

void host_lock_init(HostLock *l);
void host_lock(HostLock *l);
void host_unlock(HostLock *l);

void host_cond_init(HostCond *c);
void host_cond_signal(HostCond *c);
/* Releases l, waits for a signal or at most timeout_ns (< 0: no limit), takes l again.
 * May wake early (spurious wake-ups): callers re-check their condition. */
void host_cond_wait(HostCond *c, HostLock *l, int64_t timeout_ns);

/* Runs fn(arg) on a new detached host thread; 0 on success. */
int host_thread_start(void (*fn)(void *), void *arg, size_t stack_size);
void host_yield(void);

/* Calls report(what, host pc, faulting address) when the process crashes (access
 * violation, segmentation fault...). report should not return. */
void host_install_crash_handler(void (*report)(const char *what, uint64_t pc, uint64_t addr));

/* Sleeps the calling thread for ns (no-op for ns <= 0), to well under a millisecond. */
void host_sleep_ns(int64_t ns);

/* Once at start-up: makes timed waits precise (Windows: 1 ms system timer instead of 15.6). */
void host_timer_init(void);

/* Nanoseconds from an arbitrary start, never going backwards. */
int64_t host_now_ns(void);

#endif
