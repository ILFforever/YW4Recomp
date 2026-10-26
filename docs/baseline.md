# Baseline: the game on an emulator (Eden v0.2.1)

Status: partial (2026-10-22). Frame rate for three screens, resource use at the name-entry screen and the opening scene; the recompiled build is still to be measured the same way.

Machine: AMD Ryzen 9 7940HS (8 cores, 16 threads), 36 GB RAM, NVIDIA GeForce RTX 4070 Laptop (8 GB), Windows 11.
Eden settings: Vulkan, balanced accuracy, docked, scale 1x, bilinear, no AA. Game: Yo-kai Watch 4++ 2.2.0.

## Frame rate (Eden status bar)

| Screen | Game FPS | Frame time |
|---|---|---|
| Autosave notice | 30 | 33.33 ms |
| Title menu | 30 | 33.34 ms |
| Name entry | 30 | 33.34 ms |
| Opening scene (Whisper dialogue) | 30 | 34.01 ms |

The game caps itself at 30 frames/s, so a steady 33.3 ms means Eden keeps up; it says nothing about headroom.

## System resources (30 s, 1 s samples)

Name-entry screen:

| Measure | Average | Peak |
|---|---|---|
| Eden CPU | 185% of one core | 323% |
| Eden memory, total working set (includes mapped memory, see below) | 6.98 GB | 7.08 GB |
| GPU use, whole GPU | 32% | 58% |
| GPU memory, whole GPU | 3.59 GB | 3.81 GB |

Opening scene (Whisper dialogue), same method:

| Measure | Average | Peak |
|---|---|---|
| Eden CPU | 146% of one core | 170% |
| Eden memory, total working set (includes mapped memory, see below) | 6.97 GB | 6.97 GB |
| GPU use, whole GPU | 35% | 45% |
| GPU memory, whole GPU | 3.84 GB | 3.85 GB |

GPU use and GPU memory are for the whole GPU, so other programs (desktop, browser) are included.

## To do

- The recompiled build measured the same way (its own log is in `work/perf/`, see README).

## Recompiled build vs Eden, busy street (~1,480 draws), 2026-10-22

Same spot (the street by the school gate where the owner's save loads), standing still, both
running at the same time, per-process Windows counters over ~50 s. Ours after the CPU work of
2026-10-22 (FP fast paths, -O2, restrict, SSE vector maths, native memcpy, call caches).

| Measure | Eden | Recompiled build |
|---|---|---|
| CPU | 210% of one core | 111% |
| GPU, 3D engine (this process only) | 29.7% average (peak 33%) | 33.0% average (peak 35%) |
| Memory, private bytes (committed; not Task Manager's column) | 15.7 GB | 3.9 GB |
| Frames over 34.5 ms | (no per-frame log) | 0.2%, p99 33.9 ms |

Notes:
- Our GPU time per frame from timestamps is 18-19 ms here; Eden's GPU share is a little lower,
  so the renderer is now the side with more to gain (Phase 4b/4c).
- Committed private memory is not what Task Manager shows (see the memory note below); it is
  given here only because both processes were measured the same way.

## Recompiled build vs Eden, opening scene (Whisper dialogue), 2026-10-22

Both running at the same time, same scene, measured per process (Windows per-process counters, so the other program does not count). The whole-GPU figures above include everything on the GPU (desktop, browser, video), so use this table for the comparison.

| Measure | Eden | Recompiled build |
|---|---|---|
| Frame rate | 30 fps (34.01 ms) | 29.2 fps average over the run, 33.3 ms typical |
| CPU | 146% of one core (peak 170%) | 108% (peak 134%) |
| Memory, private (what Task Manager's Memory column shows) | 1.36 GB | 2.78 GB |
| GPU, 3D engine (this process only) | 8% average (peak 10%) | 16% average (peak 20%) |
| GPU time per frame (timestamps) | not available | 6.5 ms average, 11.9 ms at p95 |

Notes:
- Memory, corrected: an earlier version of this file compared Eden's 6.9 GB *working set* with our 2.9 GB. That was misleading: Eden's working set includes 5.5 GB of shared, memory-mapped pages (the emulated console's RAM mapping), which Task Manager does not count in its Memory column. Measured the way Task Manager does: Eden 1.36 GB private (7.1 GB committed, 6.9 GB working set with the mapped part), ours 2.78 GB private (3.84 GB committed). So Eden uses less private RAM than the recompiled build, which keeps the game's 2.9 GB heap as ordinary memory.
- GPU: Eden 8-10% (20 s and 60 s averages), ours 16% in one 20 s sample, 12.2% in a Task Manager snapshot. Ours is heavier but by an uncertain amount, between 1x and 2x; repeat over a longer run once the crash below is fixed.
- GPU: ours uses more GPU time than Eden for the same scene. Expected at this stage: full barriers between all work, images kept in the general layout, no pipeline cache across runs, and textures re-uploaded each frame. Optimising this is a later phase.
- Visual difference seen in the same scene: Eden's floor and Jibanyan are more saturated and brighter; ours is flatter and darker. Something in the lighting or post-processing is still missing.

## Recompiled build vs Eden, schoolyard street corner, 2026-10-26

After decisions 0028 (geometry in video memory) and 0029 (the GPU worker thread). The owner stood at the same street
corner (daytime) in both, side by side; Eden's numbers from its frame-time overlay, ours from the HUD (F3) and the
performance log of the same session.

| Measure | Eden v0.2.1 | Recompiled build |
|---|---|---|
| Frame rate | 30 fps (min 29, max 31) | 29.8 fps on the HUD; 29.9 over the whole session after loading |
| Frame time | avg 33.4 ms, min 32.0, max 80.6 | min 31.6, max 66.7 ms on the HUD; whole session (11,506 frames): avg 33.4 ms, p99 34.0, 0.2% of frames over 34.5 ms, one frame of 133 ms |
| GPU time per frame | not available | 7.4 ms |
| Game work per frame | not available | 18.5 ms (of the 33.3 ms budget) |
| CPU (30 s side by side, per process) | 308% of one core (peak 501%) | 159% (peak 174%); the HUD showed 1.7 of 16 cores |
| GPU, 3D engine, this process only | 11.6% (peak 12.9%) | 17.5% (peak 20.3%); 8.5 ms of GPU time a frame |
| Memory, private (Task Manager's column) | 1.26 GB | 2.86 GB |
| Memory, committed | 6.74 GB | 5.21 GB |
| Frame time over the same 30 s | (overlay above) | avg 33.35 ms, p99 34.1, max 50.1; game work 15.2 ms |

Notes:
- The same frame rate as Eden at the game's 30 cap, with a shorter worst frame at this spot. The session's single
  133 ms frame is not explained yet (a first-use pipeline build or an area load are the likely causes).
- The GPU is idle most of each frame (7.4 of 33 ms): room for higher resolution later.
- CPU: about half of Eden's, with much smaller peaks. GPU: about 1.5x Eden's, partly our own settings (4096 shadow
  maps where Eden draws the game's 1024, PCSS x2) and partly our translation's barriers; still 8.5 of 33 ms a frame.
- Memory: as on 2026-10-22, Task Manager's column counts our 2.9 GB game heap but not Eden's mapped console RAM;
  committed memory is lower for us.
- **At the game's own shadow settings** (F8 to 1024, F7 PCSS off; same spot, both running, 30 s): ours GPU 14.3%
  (5.5 ms a frame) vs Eden 13.2%; CPU 147% (peak 161%) vs Eden 318% (peak 604%); frame time avg 33.37 ms, p99 34.0.
  So the GPU gap above is our shadow upgrade (4096 maps + PCSS x2: ~3 ms of GPU time a frame); at equal settings
  the GPU use is about Eden's. Eden's own GPU share moved between the two samples (11.6% / 13.2%).
- Measured with a script sampling both processes once a second for 30 s (CPU time, private working set, commit, GPU
  engine counters by process id).
- The owner's slow evening and night towns (`work/testsaves/save_base_lag`..`lag4`), 20-26 fps before 0028, now run
  at 29.9 fps with 12 ms of GPU time and ~13 ms of game work a frame.
