# AMD FSR 3.1 on Linux

What `tools/yw4/fsr_linux.py` needs to build AMD's FSR 3.1 upscaler and frame generation for Linux from AMD's own
FidelityFX SDK v1.1.4 source ([decision 0073](../../docs/decisions/0073-dlss-fsr-on-linux.md)). AMD ships that SDK for
Windows only.

## From xplane-fsr3 (MIT)

The Linux port of the SDK comes from **[xplane-fsr3](https://github.com/JonathanOrr/xplane-fsr3)** by Jonathan Orr,
commit `fa51577` (2026-10-18), folder `fsr3-linux/`, under its MIT licence ([LICENSE](LICENSE)). Unchanged copies:

| Here | What |
|---|---|
| `ffx_sc/` | AMD's shader compiler (FidelityFX_SC, from the SDK's `sdk/tools/ffx_shader_compiler/src`) ported to Linux, GLSL path only. AMD's own code under AMD's MIT licence (the header in each file), with the port's changes |
| `patches/fidelityfx-sdk-v1.1.4-linux.patch` | Four fixes to AMD's Vulkan backend: scratch alignment, descriptor pool size (and AMD's `poolSizeCount` bug), memory types on integrated GPUs, an overridable context size |
| `patches/fsr-3.1.5-backport.patch` | The upscaler from 3.1.4 to 3.1.5 (SDK 2.0's RCAS fix) |
| `ffx_linux_compat.h` | The MSVC runtime names the SDK uses (`_countof`, `wcscpy_s`, ...) |
| `ffx_host_vk.h`, `ffx_host_vk.cpp` | The host shim: AMD's backend sees only the extensions and features the game enabled |
| `ffx_fg_stub.cpp` | The one frame-generation swapchain function the backend names (AMD's swapchain is not built: the game presents and paces itself) |
| `PORTING.md` | xplane-fsr3's own account of every change and why |

Their `gen_shaders.sh` and CMake build are replaced by `tools/yw4/fsr_linux.py` (the same shader compiler arguments).

## From NX2navi (MIT)

Frame generation follows **[NX2navi](https://github.com/tangerineturtle1/NX2navi)**'s pull request
[#455](https://github.com/tangerineturtle1/NX2navi/pull/455) by loteran (MIT), which builds on the same port: which SDK
sources and shader options frame interpolation and optical flow need (they mirror AMD's own CMake files), and the
`%s` to `%ls` fix in AMD's frame generation provider (a wide string in a wide format, `patches/yw4_ffx_api_linux.py`).
No file of theirs is copied: their frame interpolation swapchain and its Win32 layer are not needed here.

## Ours

| Here | What |
|---|---|
| `patches/yw4_ffx_api_linux.py` | The SDK's `ffx-api/` (the five functions the game calls) on Linux: the FSR 3 and frame generation providers (no FSR 2, no AMD swapchain), no DX12 driver hook, headers usable from C |
| `yw4_ffx_host.cpp` | `yw4FfxHostSet`, the library's entry for the game to name its Vulkan loader and what it enabled |

Checked 2026-11-14 (decision 0073): the patches read against AMD's source, the descriptor limits against the generated
shaders, xplane-fsr3's self-test reproduced on Lavapipe, and the game's FSR on Linux against AMD's own Windows library
on the same frame.
