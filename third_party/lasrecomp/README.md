# LAS Recomp (reference, MIT)

[LAS Recomp](https://github.com/peiikai/LASRecomp) is a static recompilation of Link's Awakening for Switch. Its runtime is MIT-licensed (`LICENSE` here, copyright Peiikai).

No files are vendored. Parts of our runtime are adapted from it and carry this notice:

| Our file | Adapted from (commit `da35b83`) | What |
|---|---|---|
| `runtime/src/nvn_objects.c` | `runtime/nvn/objects.c` | NVN builder field offsets, state-object packing, device limits |
| `runtime/src/nvn_cmdbuf.c` | `runtime/nvn/cmdbuf.c` | recording NVN commands into the game's command memory |
| `runtime/src/tegra.c`, `tegra.h` | `runtime/nvn/tegra.c`, `formats.c` | Tegra X1 block-linear layout, NVN format sizes |
| `runtime/src/gpu.c` | `runtime/nvn/vulkan/textures.c` | NVN format → Vulkan format table |

Earlier references, with no code taken over: the kernel (`runtime/kernel.c`, decision 0014) and the RomFS builder (`romfs_build.c`, 0014).
