/* The draw path: NVN command lists run on Vulkan (decision 0021).
 *
 * Without a window (YW4_HEADLESS=1) or a capable GPU, gpu_ready() is 0 and NVN keeps its
 * headless behaviour (0017, 0019): lists are walked on the CPU and every sync is signalled.
 */
#ifndef YW4_GPU_H
#define YW4_GPU_H

#include <stddef.h>
#include <stdint.h>

#include "nvn.h"

void gpu_init(void);  /* window.c, once the Vulkan device exists */
int gpu_ready(void);  /* waits until the start is over (gpu_start_done) */
void gpu_start_done(void);  /* window.c: Vulkan is up, or there is none; the guest's GPU work may begin */
/* From the window's key presses: Shift+F11 describes the draws of the next frame in the log (gpu_draw.c),
 * F10 dumps every multi-layer depth image (shadow maps) at the next present (gpu.c). */
void gpu_request_trace(int draws);
void gpu_request_dump(void);
/* Shadow quality: the shadow maps' size (1024 is the game's; YW4_SHADOW_RES at start), and the panel's
 * cycle 1024 -> 2048 -> 4096 -> 1024. */
int gpu_shadow_res(void);
void gpu_shadow_res_cycle(void);
/* Resolution of the 3D view: the height of the game's 1600x900 targets as made (900 is the game's;
 * YW4_RES at start), and the panel's cycle 900 -> 1080 -> 1440 -> 2160 -> 900. */
int gpu_res_height(void);  /* what the images are made at (follows the choice, gpu_sizes_settle) */
int gpu_res_choice(void);  /* the panel's choice */
void gpu_res_cycle(void);
/* Upscaling (decision 0039): the 3D drawn smaller than the output, made full size at the anti-aliasing point.
 * 0 off, 1 quality (1.5x), 2 balanced (1.7x), 3 performance (2x); YW4_UPSCALE, the settings panel. */
int gpu_upscale(void);          /* in effect: the choice when the anti-aliasing is FSR or DLSS, else 0 */
int gpu_upscale_setting(void);  /* the choice (the panel shows it greyed out without FSR) */
void gpu_upscale_cycle(void);
int gpu_render_height(void);  /* the 3D images' height in the game's 900-line terms (= gpu_res_height() when off) */
/* The picture settings in one line (anti-aliasing, upscaling, the 3D's size, sharpening, frame generation): logged
 * as "yw4: picture:" at the first present and on every change, and in the perf summary; how often it changed. */
void gpu_picture_text(char *buf, size_t n);
int gpu_picture_changes(void);
/* A full-screen video is on screen (the movie program drew lately, no 3D object): framerate.c holds 30 frames/s. */
int gpu_movie_screen(void);
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
void gpu_aa_step(int dir);
/* SHARPENING (gpu_aa.c, decision 0076): a slider after any anti-aliasing, in percent, at its five levels 0 (off),
 * 25, 50 (default), 75, 100 (YW4_SHARPEN at start). get / set: the slider's 0..1. parse: a saved value, the slider's
 * fraction or a level of the five-option row before it (1..4: 25..100%). */
int gpu_sharpen(void);
float gpu_sharpen_get(void);
void gpu_sharpen_set(float frac);
int gpu_sharpen_parse(const char *value);
/* SHARPEN TEXT AND MENUS: 1 if the game's UI is sharpened too (YW4_SHARPEN_UI at start). */
int gpu_sharpen_ui(void);
void gpu_sharpen_ui_toggle(void);
/* Picture shape (widescreen.c): 0 16:9 (the game's), 1 16:10, 2 18:9, 3 19:9, 4 19.5:9, 5 20:9, 6 21:9, 7 32:9
 * (YW4_ASPECT at start), and
 * the width factor of the screen-sized targets against 16:9. */
int gpu_wide(void);         /* the aspect ratio in use (follows the choice, gpu_sizes_settle) */
int gpu_wide_choice(void);  /* the panel's choice */
void gpu_wide_settle(void);
void gpu_wide_cycle(void);
/* The images follow the size choices once they have rested a few frames (gpu.c, every present). */
void gpu_sizes_settle(uint64_t frame);
void gpu_wide_factor(uint32_t *num, uint32_t *den);
/* F5: per-pass GPU timing on or off (gpu_passes.c) */
void gpu_passes_toggle(void);
/* Soft shadows (PCSS): 0 off (the game's filter), 1..3 on with stronger softening; the panel cycles. */
int gpu_pcss_level(void);
void gpu_pcss_cycle(void);
/* The shadow mask (shadow_mask.c): our program in place of the game's (YW4_SHADOW_MASK, the panel)? */
int shadow_mask_native(void);
void shadow_mask_toggle(void);
/* The screen passes (gpu_draw.c, decision 0046): our programs in the engine's draws (YW4_SCREEN_PASSES, the panel)? */
int screen_passes_native(void);
void screen_passes_toggle(void);
/* Its soft shadows' speed-ups (gpu_draw.c): one filter per pixel (YW4_MASK_FILTER=once), an 8-tap blocker
 * search (always: shadow_mask.frag). The panel switches the filter. */
int gpu_mask_filter_once(void);
void gpu_mask_filter_toggle(void);
int gpu_contact(void);
void gpu_contact_toggle(void);
/* Lamp shadows in our lamp lights (gpu_draw.c, YW4_LAMP_SHADOWS, the panel) */
int gpu_lamp_shadows(void);
void gpu_lamp_shadows_toggle(void);  /* off, or on at the last strength */
int gpu_lamp_shadow_level(void);     /* the panel's row: 0 off, 1-4 light .. full */
void gpu_lamp_shadow_cycle(void);
/* ... and an 8-tap soft filter turned each frame (YW4_PCSS_FILTER_TAPS=8) */
int gpu_pcss_filter8(void);
void gpu_pcss_filter8_toggle(void);

void gpu_pool_init(NvnPool *p);  /* imports the pool's memory into Vulkan */
/* Our own code wrote [addr, addr + size) of a pool (WriteTexels): marked as written where the host can't see
 * writes, so textures there are uploaded again (gpu.c cpu_changed). */
void gpu_note_written(uint64_t addr, uint64_t size);
/* The game's FlushMappedRange of [addr, addr + size) (gpu.c). */
void gpu_note_flushed(uint64_t addr, uint64_t size);
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
int64_t gpu_queue_worker_cpu_ns(void);  /* the GPU worker thread's CPU time so far, ns (0: none) */

#endif
