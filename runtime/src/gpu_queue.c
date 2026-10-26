/* The GPU worker (decision 0029): the game's command lists are translated to Vulkan on a thread
 * of our own, in the order the game submitted them, instead of inside the game's submit call.
 *
 * nvnQueueSubmitCommands used to run gpu_execute on the game's render thread, under gpu_lock,
 * before returning: ~9 ms a frame of our work on the chain the game's frame waits for (the busy
 * towns). Now a submission, a present and the freeing of an object queued work may still use
 * are jobs in one queue, run by one worker in order.
 *
 * Syncs: a fence takes a ticket, the number of jobs queued so far (not a Vulkan submission:
 * the work may not be submitted yet). Waiting for a ticket waits for the worker to have run that
 * job, then for the Vulkan submissions it had made by then to complete. So nothing the game
 * waits for is signalled before its work has been recorded and run.
 *
 * Changing an object queued work reads (a program's shaders) first waits for the queue to
 * empty (gpu_queue_drain); freeing one goes through the queue (gpu_queue_call).
 *
 * YW4_GPU_THREAD=0: everything runs in the caller as before. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "gpu.h"
#include "gpu_internal.h"
#include "host.h"
#include "kernel.h"
#include "nvn.h"

enum { JOBS = 1024, TICKETS = 4096 };
enum { JOB_EXECUTE, JOB_PRESENT, JOB_CALL };
typedef struct {
    int kind;
    uint64_t a;          /* execute: the command list's handle; present: the frame */
    void *p;             /* present: the texture; call: the argument */
    void (*fn)(void *);  /* call */
    int crop[4];         /* present */
} Job;

static Job jobs[JOBS];
static uint64_t queued, done;        /* tickets: jobs queued, jobs run (job t is jobs[(t - 1) % JOBS]) */
static uint64_t seq_after[TICKETS];  /* the Vulkan submissions made when job t had run */
static HostLock lock = HOST_LOCK_INIT;
static HostCond work = HOST_COND_INIT, progress = HOST_COND_INIT;
static int on = -1;

static int queue_on(void) {
    int o = __atomic_load_n(&on, __ATOMIC_ACQUIRE);
    if (o >= 0) return o;
    host_lock(&lock);
    if (on < 0) {
        const char *e = getenv("YW4_GPU_THREAD");
        int v = gpu_ready() && (!e || atoi(e));
        if (v) fprintf(stderr, "yw4: gpu: command lists translated on a worker thread (decision 0029)\n");
        __atomic_store_n(&on, v, __ATOMIC_RELEASE);
    }
    host_unlock(&lock);
    return on;
}

static void run(const Job *j) {
    switch (j->kind) {
    case JOB_EXECUTE: gpu_execute(j->a); break;
    case JOB_PRESENT: gpu_present((NvnTexture *)j->p, j->a, j->crop); break;
    case JOB_CALL: j->fn(j->p); break;
    }
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        host_lock(&lock);
        while (done == queued) host_cond_wait(&work, &lock, -1);
        Job j = jobs[done % JOBS];
        host_unlock(&lock);
        yw4_profile_thread(YW4_ROLE_SUBMIT, gpu_frames());
        run(&j);
        uint64_t seq = gpu_submitted();
        host_lock(&lock);
        done++;
        seq_after[done % TICKETS] = seq;
        host_cond_broadcast(&progress);
        host_unlock(&lock);
    }
}

static void put(const Job *j) {
    static int started;
    host_lock(&lock);
    if (!started) {
        started = 1;
        if (host_thread_start(worker, NULL, 1 << 20)) {
            fprintf(stderr, "yw4: gpu: cannot start the GPU worker thread\n");
            exit(3);
        }
    }
    while (queued - done >= JOBS) host_cond_wait(&progress, &lock, -1);  /* the worker is far behind */
    jobs[queued % JOBS] = *j;
    queued++;
    host_cond_signal(&work);
    host_unlock(&lock);
}

void gpu_queue_execute(uint64_t handle) {
    if (!queue_on()) {
        gpu_execute(handle);
        return;
    }
    put(&(Job){.kind = JOB_EXECUTE, .a = handle});
}

void gpu_queue_present(NvnTexture *texture, uint64_t frame, const int crop[4]) {
    if (!queue_on()) {
        gpu_present(texture, frame, crop);
        return;
    }
    Job j = {.kind = JOB_PRESENT, .a = frame, .p = texture};
    for (int i = 0; i < 4; i++) j.crop[i] = crop[i];
    put(&j);
}

void gpu_queue_call(void (*fn)(void *), void *arg) {
    if (!queue_on()) {
        fn(arg);
        return;
    }
    put(&(Job){.kind = JOB_CALL, .fn = fn, .p = arg});
}

uint64_t gpu_queue_fence(void) {
    if (!queue_on()) return gpu_submitted();
    host_lock(&lock);
    uint64_t t = queued;
    host_unlock(&lock);
    return t;
}

/* 0 already signalled, 1 waited, 2 timed out (NVN's sync results) */
int gpu_queue_wait(uint64_t fence, uint64_t timeout_ns) {
    if (!queue_on()) return gpu_wait(fence, timeout_ns);
    if (!fence) return 0;
    int forever = timeout_ns >= (uint64_t)INT64_MAX / 2;
    int64_t start = host_now_ns(), deadline = forever ? 0 : start + (int64_t)timeout_ns;
    int waited = 0;
    host_lock(&lock);
    while (done < fence) {
        int64_t left = forever ? -1 : deadline - host_now_ns();
        if (!forever && left <= 0) {
            host_unlock(&lock);
            return 2;
        }
        waited = 1;
        host_cond_wait(&progress, &lock, left);
    }
    /* the submissions job `fence` had made; a ticket long past: whatever has been submitted */
    uint64_t seq = done - fence < TICKETS ? seq_after[fence % TICKETS] : gpu_submitted();
    host_unlock(&lock);
    uint64_t left = forever ? UINT64_MAX : deadline > host_now_ns() ? (uint64_t)(deadline - host_now_ns()) : 0;
    int r = gpu_wait(seq, left);
    return r == 2 ? 2 : waited || r;
}

void gpu_queue_drain(void) {
    if (!queue_on()) return;
    host_lock(&lock);
    uint64_t t = queued;
    while (done < t) host_cond_wait(&progress, &lock, -1);
    host_unlock(&lock);
}
