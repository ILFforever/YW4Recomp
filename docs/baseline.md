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

- A busier scene (town, battle) once the recompiled build gets that far.
- The recompiled build measured the same way (its own log is in `work/perf/`, see README).

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
