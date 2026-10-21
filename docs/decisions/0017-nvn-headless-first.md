# 0017 — NVN on the host: headless first; SDL3 and Vulkan next

Date: 2026-10-21
Status: accepted (owner chose all three options below, 2026-10-21)

## Owner's choices

1. **Headless first.** NVN objects hold state, command buffers record, nothing is drawn, until the game reaches its frame loop. This needs no new dependency, shows the whole set of NVN calls a frame makes before the Vulkan design is fixed, and reaches the next non-graphics blockers (input, audio, saves) sooner.
2. **SDL3** (zlib licence) for the window later, and also for input (`hid`) and audio output (`audren`).
3. **Vulkan headers vendored** (Khronos, Apache-2.0/MIT), with `vulkan-1` loaded at run time so building needs nothing installed. The owner installs the LunarG SDK only for its validation layers.

## How NVN is built (runtime/src/nvn*.c, tegra.c)

NVN's headers aren't public. Argument orders, builder field offsets, the packing of the small state objects and the device limits come from **LAS Recomp's NVN** (MIT; `third_party/lasrecomp/README.md` lists what was adapted from which file, with its licence). Each file that adapts LAS code says so. Our versions are rewritten in this project's style, and the parts we can check are checked against our game (below).

- **Only the 177 NVN functions the game uses** (`work/recon/nvn.md`). Any other one stops with its name (0016).
- **Objects** (memory pool, texture, program, window, command buffer, texture/sampler pool) keep a pointer to our host object in their first 8 bytes. **Builders and small state objects** keep their fields in guest memory, inside their ABI size, because the game copies them by value (the command buffer copies a bound blend state, for example).
- **GPU addresses are guest addresses.** A memory pool is the game's own memory, so mapping it returns that memory and flushes are no-ops.
- **Command buffers record into the game's command memory** as records holding the call's arguments and copies of what its pointers point at. A command handle is the address of a list's first record, and lists continue across memory chunks through jump records. That way the game's own command-memory management behaves as designed. This game sets no out-of-memory callback, so running out stops.
- **Headless semantics:** work is finished the moment it is submitted, so every sync is signalled and `SyncWait` returns "already signalled". Submitting walks the lists (a broken list stops) and counts draws. `ReportCounter` writes a zero value and the current GPU timestamp. Presenting counts frames and logs the first three and every 300th.
- **Texture storage sizes** follow the Tegra X1 block-linear layout (`tegra.c`): GOBs of 64 bytes × 8 rows, the block height the driver picks, mip levels shrinking it, layers aligned to blocks. Multisampled textures are stored as a wider/taller surface (2× as 2×1, 4× as 2×2, 8× as 4×2). The game sizes its memory from these numbers, so `tests/test_nvn.py` checks them against sizes worked out by hand and checks the swizzle against an independent model of the layout. A planted sector-order bug is caught.

## nn::vi: overridden, like the GPU driver

On the Switch, NVN's window presents through a vi layer and an Android-style binder buffer queue. Our NVN presents itself, so the game's 8 `nn::vi` imports are overrides (`recomp/overrides.toml`, `runtime/src/vi.c`). The display, layer and native window are objects to hand back. The vsync event is real: a host thread signals it at 60 Hz.

Two small mechanisms came with it:
- **Guest symbols for native code** (`[[guest_symbol]]` in `overrides.toml`, `yw4_guest_symbol(name)`): the vsync override attaches its event handle with the sdk's own `nn::os::AttachReadableHandleToSystemEvent`, because the layout of `SystemEventType` is the sdk's business.
- **Host-signalled events** (`KEvent.host_signalled`, `yw4_event_signal_from_host`): a wait on one isn't "blocked forever", so the deadlock detector (0015) still works with vsync running.

## Services reached on the way (svc_am.c, svc_acc.c)

Operation mode is docked, so the game makes a 1920×1080 layer. `apm` performance settings are accepted and ignored. One local user account (fixed id, which keys the save data later) is passed as the preselected-user launch parameter. Desired language is `ja`: the game is Japanese only. Small output buffers can arrive as C (pointer) buffers instead of B buffers; `ipc_out_buffer()` takes either.

## Not verified yet

- `nvnQueueBuilderGetQueueMemorySize` returns 0x10000. That is a guess at what the game must allocate, and our NVN uses none of it.
- Storage alignment 512 bytes (32 for linear) is from LAS. Real compressible render targets may want more. Harmless headless; it matters once Vulkan textures alias game memory.
- The MSAA storage layout is from the Maxwell sample arrangement, not measured.

## Result

NVN device, window (2 textures) and display set-up all pass. The game reaches input: `hid` (`nn::hid::InitializeNpad`).
