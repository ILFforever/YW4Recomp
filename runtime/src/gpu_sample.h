/* gpu_sample.c: what the GPU thread is doing, sampled from another thread (Linux; issue #30).
 *
 * The GPU thread marks which part of its work it is in (gpu_phase_set); a sampler thread looks at it ~1000 times a
 * second through /proc/self/task/<id>/{stat,syscall}: the part, the kernel's state for the thread (R running, S
 * sleeping, D waiting in the kernel) and, when it is blocked in a system call, which one (for ioctl, its request
 * number: which GPU-driver call). Every 600 frames a line with the most common combinations, and the DRM driver's
 * own per-client numbers from /proc/self/fdinfo (memory by place, evictions, engine busy time). Elsewhere: nothing.
 * YW4_GPU_SAMPLE=0 turns it off. */
#pragma once
#include <stdint.h>

enum { GPU_PHASE_OTHER, GPU_PHASE_IDLE, GPU_PHASE_RECORD, GPU_PHASE_END, GPU_PHASE_SUBMIT, GPU_PHASE_PRESENT,
       GPU_PHASE_WINDOW, GPU_PHASES };
extern int gpu_phase;  /* written by the GPU thread only */
static inline int gpu_phase_set(int p) { return __atomic_exchange_n(&gpu_phase, p, __ATOMIC_RELAXED); }

void gpu_sample_start(void);              /* on the GPU thread, once or more: notes its id, starts the sampler */
void gpu_sample_report(uint64_t frame);   /* every 600 frames (gpu.c thread_report): the lines */
