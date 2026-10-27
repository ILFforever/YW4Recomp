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
/* From the window's key presses: F11 describes the draws of the next frame in the log (gpu_draw.c),
 * F10 dumps every multi-layer depth image (shadow maps) at the next present (gpu.c). */
void gpu_request_trace(int draws);
void gpu_request_dump(void);
/* Shadow quality: the shadow maps' size (1024 is the game's; YW4_SHADOW_RES at start), and the panel's
 * cycle 1024 -> 2048 -> 4096 -> 1024. */
int gpu_shadow_res(void);
void gpu_shadow_res_cycle(void);
/* Resolution of the 3D view: the height of the game's 1600x900 targets as made (900 is the game's;
 * YW4_RES at start), and the panel's cycle 900 -> 1080 -> 1440 -> 2160 -> 900. */
int gpu_res_height(void);
void gpu_res_cycle(void);
/* Texture filtering: 0 the game's, else blending between mip levels and anisotropic filtering with up
 * to this many taps (16 by default; YW4_ANISO at start), and the panel's cycle 0 -> 2 -> 4 -> 8 -> 16 -> 0. */
int gpu_aniso(void);
void gpu_aniso_cycle(void);
/* Distance blur (depth of field, gpu_draw.c): 2 the game's, 1 half, 0 off (YW4_DOF at start). */
int gpu_dof(void);
void gpu_dof_cycle(void);
/* Effects resolution (gpu.c): 1 if the game's half-size targets are made the 3D view's size (YW4_EFFECTS). */
int gpu_effects_full(void);
void gpu_effects_cycle(void);
/* Anti-aliasing between the scene and the UI (gpu_aa.c): 0 off, 1 FXAA, 2 SMAA, 3 TAA (YW4_AA at start). */
int gpu_aa(void);
void gpu_aa_cycle(void);
/* Picture shape (widescreen.c): 0 16:9 (the game's), 1 16:10, 2 21:9, 3 32:9 (YW4_ASPECT at start), and
 * the width factor of the screen-sized targets against 16:9. */
int gpu_wide(void);
void gpu_wide_cycle(void);
void gpu_wide_factor(uint32_t *num, uint32_t *den);
/* F5: per-pass GPU timing on or off (gpu_passes.c) */
void gpu_passes_toggle(void);
/* Soft shadows (PCSS): 0 off (the game's filter), 1..3 on with stronger softening; the panel cycles. */
int gpu_pcss_level(void);
void gpu_pcss_cycle(void);

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

/* The GPU worker (gpu_queue.c, decision 0029): command lists, presents and the freeing of objects
 * queued work may use run on one worker thread in submission order (YW4_GPU_THREAD=0: in the
 * caller). A fence is a ticket in that queue; waiting for one waits for the worker, then the GPU
 * (0 already done, 1 waited, 2 timed out). Drain: wait until everything queued has run. */
void gpu_queue_execute(uint64_t handle);
void gpu_queue_present(NvnTexture *texture, uint64_t frame, const int crop[4]);
void gpu_queue_call(void (*fn)(void *), void *arg);
uint64_t gpu_queue_fence(void);
int gpu_queue_wait(uint64_t fence, uint64_t timeout_ns);
void gpu_queue_drain(void);

#endif
