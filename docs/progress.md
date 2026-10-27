# Progress

How the recompiled game got where it is. Newest first. Numbers in brackets are the decision records in
[decisions/](decisions/) that explain each step.

**Current state (2026-10-27):** playable from boot through a new game (logos, title menu, name entry, the opening
cutscenes, the Amano house and town, Yo-kai Pad menus, diary and autosave) and from late-game saves (towns at night,
the bamboo forest, the Yo-kai World), at the game's 30 frames/s in most places tested, with sound. Our own renderer
draws the shadow casters, the G-buffer and the lit material pass from the game's data; the 3D view renders at up to
4K, with sharper textures, better shadows and anti-aliasing, all in a settings panel (F1). Open gaps are in
[known-issues.md](known-issues.md).

| Date | Step | Result |
|---|---|---|
| 2026-10-27 | Model detail at a distance: the engine's level-of-detail class (`gmdCObjModelLodInfo`) found, but never set up in the town, the bamboo forest or the Yo-kai World ([frame.md](frame.md)) | Nothing to push further there; the distance drop-off was the blur and the filtering (above) |
| 2026-10-27 | Settings panel (F1), styled after the game's menus: groups, every option shown, an explanation per setting and option (checked by a test), scaled with the window; the settings' own function keys removed | **All graphics settings in one place**, explained |
| 2026-10-27 | Anti-aliasing between the 3D scene and the UI: FXAA (ours) and SMAA (`third_party/smaa`, MIT, ultra preset), checked with debug views and a test pattern | **Smooth edges, sharp text**; an emulator can only filter the finished picture |
| 2026-10-27 | Effects resolution: the game's half-size shadow, light and occlusion targets at the 3D view's size; the distance blur's own images keep the game's ratio; a dark-sky fault in the Yo-kai World found by bisecting the targets | **Cleaner shadow edges**, ~2 ms of GPU |
| 2026-10-27 | Texture filtering (anisotropic ×16 and blending between mip levels; the game had neither) and a distance blur setting (the game's depth of field: half by default) | **Sharp distance without 4K**: up to twice the distant detail (blur off) |
| 2026-10-27 | Resolution scaling of the 3D view: the screen-sized targets made larger by a ratio, viewports, scissors, crop and the camera's tap offsets scaled (0031) | **1080p, 1440p, 4K** at 30 frames/s; GPU 6.4 → 9.6 ms at 4K |
| 2026-10-27 | Geometry in video memory: its table emptied when it crowds, in two halves so emptying doesn't wait for the GPU | A drop to 14 frames/s after ~9 minutes of play is gone |
| 2026-10-26 – 27 | The takeover (0030): every per-draw input decoded from the game's own data (textures, samplers, render state, programs, constants, bones, vertex layout), the engine's per-draw work skipped | **85-93% of the G-buffer's and lit pass's draws made by us**, checked equal on 4 saves and by the owner's playtests |
| 2026-10-26 | Command lists translated on a GPU worker thread (0029); dynamic state set only when it changes; textures remember their image | Evening street: game work 23.2 → 12.9 ms |
| 2026-10-26 | The game's geometry copied into video memory instead of read across PCIe (0028); a per-pass GPU timer | Slow late-game towns: GPU 39 → 12 ms, **30 frames/s** where they were at 20-26 |
| 2026-10-26 | License: MIT (0027) | |
| 2026-10-25 – 26 | Our G-buffer and lit material passes (0026): our draws and readable programs, every main-view program read from the shaders' own headers; check mode and same-frame A/B | Identical to the game's on every draw checked |
| 2026-10-25 | Performance HUD; shadow map size 1024 / 2048 / 4096; command lists that overflow continue in new chunks | Sharper shadows; a crash in a late-game area fixed |
| 2026-10-25 | PCSS soft contact shadows in the game's shadow mask shader, built at start-up; F7, HUD line, F12 screenshots with the HUD; defaults 4096 maps + PCSS x2 (0025) | **Sharp contact shadows, soft canopy shade**, no extra load time |
| 2026-10-22 – 24 | Our shadow pass (0024): hooks on game functions, native passes in the command stream; static, instanced, cut-out, skinned, fading and wind-swayed casters; cutscene flicker fixed | **The game's shadow casters drawn by our renderer** |
| 2026-10-22 | The engine's frame mapped ([frame.md](frame.md)): render jobs, shadow scheduling, named stages, the model renderer's draw list; plan for our own renderer (0023) | Which engine function draws which pass |
| 2026-10-22 | CPU work: -O2, faster floating point and vector helpers, native memory functions, call-site caches | Busy street vs Eden: CPU 111% against 210% of a core, memory 3.9 against 15.7 GB |
| 2026-10-22 | AudioOut PCM to the host's sound card (SDL3 audio stream, 5.1 kept), paced by the stream so the clocks can't drift | **Sound and music play** |
| 2026-10-22 | Performance tools: game work split, per-thread CPU, sampling profiler (`YW4_PROFILE=1`); screenshots fixed (colours, folder, 1.7 s freeze) | The main thread is the CPU bottleneck; graphics translation runs on other threads |
| 2026-10-22 | Cube texture views count layers in cubes: the game's ambient/reflection cube map (view "layers 0+1") was replaced by a black dummy | **Interiors lit like on Eden**: warm diffuse light, metal sink and kettle reflect again, softer shadows |
| 2026-10-22 | Diagnostics: capture keys (F10/F11/F12), input recording and replay, mid-frame render-target capture, HDR and depth image dumps ([debugging.md](debugging.md)) | Lighting bugs can be traced pass by pass without a person at the keyboard |
| 2026-10-22 | Draws without render targets skipped instead of stopping; eShop add-on list answered; exit hang in the performance log fixed | The tip windows no longer stop the game |
| 2026-10-22 | Compute dispatches: the auto-exposure shaders run; storage buffers are not clamped to the size the game binds | Cutscenes no longer blown out white; brightness adapts |
| 2026-10-22 | Performance log: per-frame CSV and summary, GPU time from Vulkan timestamp queries; Eden baseline measured ([baseline.md](baseline.md)) | Numbers to compare against the emulator |
| 2026-10-22 | New game (0022): the keyboard applet answered natively; switch tables the recompiler missed (scheduling gaps, table base loaded after the branch); `time:u` clock snapshots; 32-bit integer vertex inputs | Name entry, the first 3D character, and the **opening scene with dialogue** render |
| 2026-10-21 | Recompiler fix: a function whose code includes a block placed below its entry now starts with a jump to the entry (5 functions; one is the engine's resource lookup by name hash, main+0xa7d8bc) | **Menus render**: every menu mesh had been skipped because its texture links were never found |
| 2026-10-21 | Draw path step 2 (0021): pipelines, descriptors, texture uploads, quads, window crop; the translated shaders run | **First real frames**: the LEVEL-5 logo and the autosave notice |
| 2026-10-21 | Draw path step 1 (0021): the game's memory pools imported into Vulkan in place, an image per texture, clears and present on the GPU, NVN syncs on a timeline semaphore | 30 frames/s on the GPU path; still black (draws next) |
| 2026-10-21 | Shader translator `recomp/shader.py`: NV_gpu_program5 → GLSL 4.60, registers as raw bits, one statement per instruction with the source as a comment, Vulkan binding scheme (0020) | All 1,349 programs translate and compile to SPIR-V |
| 2026-10-21 | Decision: translate shaders from their shipped NV_gpu_program5 assembly; `HBH1` container decoded; `tools/yw4/shaders.py` extracts them keyed by a hash of the text (0020) | 1,891 programs, 1,349 distinct, 58 opcodes; no bindless emulation needed |
| 2026-10-21 | The window: SDL3 (fetched, pinned) and Vulkan (headers vendored, loaded at run time); screenshots (0019) | A window at 30 frames/s; clears show a 1600×900 deferred G-buffer |
| 2026-10-21 | Precise timing on Windows (1 ms system timer, high-resolution sleeps); presenting waits for the display's flip like the Switch does | 11 frames/s → a steady 30.0 frames/s |
| 2026-10-21 | Input (`hid`), network (offline), time zone, audio (`audout` in real time, `audren` device events), save data in host folders, `WriteTexels`, address arbiter type 2; setjmp/longjmp for Lua's errors; a crash handler (0017, 0018) | The frame loop runs: 8,400 frames without a stop |
| 2026-10-21 | Headless NVN: the game's 177 NVN functions, Tegra texture layout; `nn::vi` with a 60 Hz vsync thread; account, launch parameter, operation mode, language (0017) | NVN device and window set up |
| 2026-10-21 | Native overrides by symbol in the translator (`recomp/overrides.toml`); NVN entered at `nvnBootstrapLoader`; `tools/yw4/where.py` names addresses (0016) | The game makes its first NVN call |
| 2026-10-21 | Guest threads as host threads: one kernel lock, mutexes, condition variables, events, thread exit/join, address arbiter, a deadlock detector; transfer memory (0015) | 8 threads run in parallel |
| 2026-10-21 | Clock service; filesystem service with a virtual RomFS image of the owner's folder (read on demand, no copy); `MapMemory` (0014) | Through the constructors' clock and file set-up |
| 2026-10-21 | IPC: HIPC/CMIF codec, domains, service manager, first services (`appletOE`, `apm`, `lm`, `aoc:u`, `pctl`), kernel events, `svcBreak` with a guest backtrace (0014) | Into `main`'s constructors |
| 2026-10-21 | Minimal kernel: address-space layout, `GetInfo`, heap, `QueryMemory`, thread properties, sleep, system tick (0014) | The sdk's start-up runs through 50 system calls |
| 2026-10-21 | Translate and load both modules; start-up through the sdk's own `nn::init::Start` (0014) | 73,844 functions; Nintendo's start-up code runs natively |
| 2026-10-21 | Instructions the `sdk` needs: AES/SHA/PMULL crypto, SIMD integer, CRC32, fused multiply-add, `MRS`/`MSR`, cache ops, `SVC` (0014) | All of `sdk` translates; 13 forms Pi-checked (120,000 cases) |
| 2026-10-21 | Decision: recompile `sdk` as well as `main`, measured first: 99.3% of its 33,616 functions already translate (0014) | Native code moves to the kernel/service level |
| 2026-10-21 | Math library: musl 1.1.19's C (`third_party/musl`), checked against the sdk's own functions run on the Pi (0013) | All 40 math functions match the Switch bit for bit on 800,000 inputs |
| 2026-10-21 | C library basics; `__cxa_guard_*`; `__nnmusl_init_dso` (0012) | The 830 constructors start running |
| 2026-10-21 | First native `sdk` functions: start-up memory, logging and asserts, the game's heap on a TLSF allocator (0011) | The game logs `Remain Memory Size 198MiB` |
| 2026-10-21 | Loader and import stubs (0010) | `main` loads at `0x7100000000`, relocates, and runs natively until its first `sdk` call |
| 2026-10-21 | Every instruction in `main` translates (0006, 0007) | 40,228 functions, 4.25 M instructions, 0 untranslated |
| 2026-10-20 – 21 | Recompiler: integer, loads/stores, floating point and SIMD, atomics, whole functions (0006–0009) | Each instruction form checked against a real ARM64 CPU (Raspberry Pi 5, 0004) |
| 2026-10-20 | Phase 1 decision: custom runtime, NVN on Vulkan (0005) | |
| 2026-10-14 – 20 | Phase 0: scaffold, NSO pipeline with version lock, recon report, Ghidra setup (0001–0003) | [recon.md](recon.md) |
