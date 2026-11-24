# 0090 — Thread-safety rules (from the 2026-11-24 code audit)

<!-- decision
state: accepted
date: 2026-11-24
replaces: none
code: runtime/src/gpu.c, runtime/src/gbuffer.c, runtime/src/bindings.c, runtime/src/kernel.c, runtime/src/window.c, runtime/src/socket.c, runtime/src/perf.c, runtime/src/theme.c, runtime/src/gpu_shadow.c, runtime/src/gpu_passes.c, runtime/src/msvc_shim.c
-->

Status: accepted (2026-11-24). Came out of the code audit in `work/tasks/code-quality-report.md` (Gemini's report,
fixes by Claude, Sonnet agents and Gemini; commits a45c7e0, df78cf9 and the batch after them).

## Context

The runtime has five kinds of threads that share state:

| thread | what runs there |
|---|---|
| game threads | the recompiled guest code: NVN calls (`nvn_*.c`), SVCs (`kernel.c`), services (`socket.c`, `savefs.c`), hooks |
| GPU worker | `gpu_execute` and everything it calls; holds `gpu_lock` for a whole command list |
| present thread | `present_image` / `menu_draw` (`window.c`) → `gamemenu_draw`, the HUD |
| window (SDL) thread | `window_run`: events, keys, mouse → `gamemenu_mouse`, bindings edits, toggles (F5 ...) |
| helper threads | relay reader/heartbeat, the log copier, pipeline compiles, the FG pacer |

The audit found about 20 places where one of these wrote state that another read with no lock or atomic. Most could
only go wrong rarely (a torn value, a counter off by one), but some could crash: a game thread walking the GPU pool
list while another added to it, the overlay reading a gamepad SDL had just closed, a static initialiser in NVIDIA's
library run twice.

## Decision

Rules for new code, and what the fixes follow:

1. **Say which thread owns a variable.** Next to a file-scope variable that more than one thread touches, a comment
   names the threads and how it is protected (a lock, an atomic, "written once before threads start").
2. **Flags and counters: `__atomic` builtins.** Relaxed when a stale value only shows one frame late (menu mode, HUD
   flags, skip counters). Release on the write / acquire on the read when the flag *publishes* other data (`note_on`
   in `window.c`, `mode` in `profile.c`, `nrows` in `perf.c`, `pacer_started` in `gpu_fg.c`): fill the data first,
   set the flag last.
3. **Structures: a small lock, held briefly, copy out.** Take the lock, copy what is needed, release, then work on the
   copy. Examples: `pad_snapshot` (`window.c`, under `input_lock`: the pad's name is copied because SDL frees it with
   the pad), `snap()` in `socket.c` (the fd entry is copied under `fd_lock`; changes go back through
   `commit_bound`/`commit_peer`/`fd_set_nonblock`, found by host handle).
4. **Never block or call out while holding a lock.** No `recv`/`poll`/`connect`, no Vulkan waits, no calls into
   another module. Every new lock is a *leaf*: nothing else is locked while it is held (`sets_lock`, `ab_lock`,
   `vp_lock`, `events_lock`, `theme_lock`, `pool_lock`).
5. **Lock order where two are nested:** `gpu_lock` → `pool_lock` (`gpu.c`: `pool_import` and `gpu_pool_finalize`
   change the pool list under both; the GPU worker reads it under `gpu_lock` alone, game threads under `pool_lock`
   alone, so neither waits for the other's long hold).
6. **Our locks are not re-entrant** (SRW locks on Windows). A function that is called both with and without the lock
   held comes in two forms: `x_locked()` assumes the lock, `x()` takes it (`copies_set_layout` /
   `copies_layout_locked` in `gpu_shadow.c`).
7. **Toggles from the window thread don't change GPU state directly.** They set a request flag; the GPU worker applies
   it at the start of its next submission (`gpu_passes_toggle` → `apply_toggle` in `gpu_passes_begin`).
8. **Read environment switches once**, before any guest thread starts (`svc_env_init` in `yw4_kernel_init`), not
   lazily on the hot path from several threads.
9. **Pointers into a copy must outlive it.** A struct whose pointers point into itself (`RenderTargets`: `color[i]`
   → `color_snap[i]`) must not be a stack local when something keeps those pointers (`render_targets[]` in
   `nvn_cmdbuf.c` stays `static`; a review caught a change that would have left it pointing into a dead stack frame).
10. **MSVC thread-safe statics** (`msvc_shim.c`, for NVIDIA's static libraries): the header waits on a condition
    variable while another thread initialises (-1), claims the static, and returns *without* the lock; the footer
    publishes and wakes the waiters. The lock is never held while an initialiser runs, so nested statics work.

## Why

- Locks held briefly with copies keep the hot paths (per draw, per packet) cheap: an uncontended SRW lock costs tens
  of nanoseconds, and nothing waits on a long hold.
- Leaf locks and one fixed order make deadlocks impossible by construction, without needing to trace every path.
- Atomics for flags are free on x86-64 and ARM64 for aligned words; they also stop the compiler from caching a value
  in a register across a loop.

## Testing

- `python -m pytest tests` (98 passed). The tests that compile `bindings.c` on its own got stub `host_lock`/
  `host_unlock` (`tests/test_runtime.py`).
- Game runs from a save copy: the town walk with the overlay and Controls page, and the late-game bamboo forest
  (1,800 shadow layers, 41k draws). Both exit 0 with no errors. No battle was run (the battle replay doesn't reach one
  from this save).
- Linux: every changed file syntax-checked with zig (`x86_64-linux-gnu`), including the `realpath` and `poll` paths.
- Races don't show up reliably in a few runs; these fixes were checked by reading each diff for lock re-entry, lock
  order, blocking under a lock, and pointers into copies.

## Open

The audit's remaining items are in the report's status column ("open" / "PARTLY"): mostly function splits,
duplication and tests.
