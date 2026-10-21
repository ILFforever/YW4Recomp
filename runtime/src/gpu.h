/* The draw path: NVN command lists run on Vulkan (decision 0021).
 *
 * Without a window (YW4_HEADLESS=1) or a capable GPU, gpu_ready() is 0 and NVN keeps its
 * headless behaviour (0017, 0019): lists are walked on the CPU and every sync is signalled.
 */
#ifndef YW4_GPU_H
#define YW4_GPU_H

#include <stdint.h>

#include "nvn.h"

void gpu_init(void);  /* window.c, once the Vulkan device exists */
int gpu_ready(void);

void gpu_pool_init(NvnPool *p);  /* imports the pool's memory into Vulkan */
void gpu_pool_finalize(NvnPool *p);
void gpu_texture_finalize(NvnTexture *t);
void gpu_program_finalize(NvnProgram *p);

/* Runs a recorded command list (nvn_cmdbuf.c) on the GPU: one submission. */
void gpu_execute(uint64_t handle);

/* Submissions are numbered from 1; an NVN sync remembers a number and waits for it. */
uint64_t gpu_submitted(void);
/* 0: done before the call, 1: done within the timeout, 2: still running. */
int gpu_wait(uint64_t seq, uint64_t timeout_ns);

/* GPU time of the submissions made while frame `frame` was being built (the presents are
 * counted from 1): the sum of their durations and the span from the first start to the last
 * end, from timestamp queries. Results arrive a few frames late, so ask about a frame some
 * frames back. 0 if there are none (yet) or timestamps aren't available. */
int gpu_frame_time(uint64_t frame, double *busy_ms, double *span_ms);

/* Shows texture t (the window's) once everything submitted before has run: its crop
 * rectangle (x, y, w, h; all zero: the whole texture), scaled to the window. The game
 * renders at a lower resolution and crops to it (dynamic resolution). */
void gpu_present(NvnTexture *t, uint64_t frame, const int crop[4]);

#endif
