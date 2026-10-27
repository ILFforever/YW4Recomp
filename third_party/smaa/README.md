# SMAA

Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego
Gutierrez, from https://github.com/iryoku/smaa (master, fetched 2026-10-27). MIT license: `LICENSE.txt`.

Unchanged copies of `SMAA.hlsl` and the lookup textures `Textures/AreaTex.h`, `Textures/SearchTex.h`. Used by the
anti-aliasing pass between the 3D scene and the UI (`runtime/src/gpu_aa.c`, `runtime/shaders/smaa_*`).
