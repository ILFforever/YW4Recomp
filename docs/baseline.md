# Baseline: the game on an emulator (Eden v0.2.1)

Status: partial (2026-10-22). Frame rate and resource use at the name-entry screen; the other scenes still to do.

Machine: AMD Ryzen 9 7940HS (8 cores, 16 threads), 36 GB RAM, NVIDIA GeForce RTX 4070 Laptop (8 GB), Windows 11.
Eden settings: Vulkan, balanced accuracy, docked, scale 1x, bilinear, no AA. Game: Yo-kai Watch 4++ 2.2.0.

## Frame rate (Eden status bar)

| Screen | Game FPS | Frame time |
|---|---|---|
| Autosave notice | 30 | 33.33 ms |
| Title menu | 30 | 33.34 ms |
| Name entry | 30 | 33.34 ms |

The game caps itself at 30 frames/s, so a steady 33.3 ms means Eden keeps up; it says nothing about headroom.

## System resources (name-entry screen, 30 s, 1 s samples)

| Measure | Average | Peak |
|---|---|---|
| Eden CPU | 185% of one core | 323% |
| Eden memory (working set) | 6.98 GB | 7.08 GB |
| GPU use, whole GPU | 32% | 58% |
| GPU memory, whole GPU | 3.59 GB | 3.81 GB |

GPU use and GPU memory are for the whole GPU, so other programs (desktop, browser) are included.

## To do

- Same numbers for the opening scene (needs the game played past the name entry).
- The recompiled build measured the same way (its own log is in `work/perf/`, see README).
