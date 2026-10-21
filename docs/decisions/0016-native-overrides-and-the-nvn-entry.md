# 0016 — Native overrides by symbol; NVN starts at nvnBootstrapLoader

Date: 2026-10-21
Status: accepted (follows 0005's graphics plan and 0014's "overrides by symbol stay possible")

## Context

With threads (0015), main's start-up reached `sm: GetServiceHandle("nvdrv")`, the GPU driver service. The backtrace (`tools/yw4/where.py` names the sdk addresses) shows who opens it: `nnMain` → game code → **`nv::InitializeGraphics(memory, 8 MiB)`**, the set-up of the NVN driver inside the sdk. That driver turns NVN calls into Maxwell GPU commands sent through `nvdrv`. Emulating that path is option A of decision 0005, which was rejected. Graphics is done at the NVN API level instead.

## Decision

**Overrides live in the translator** (`recomp/overrides.toml`: module, symbol, native function, why). The overridden function keeps its address and name (`s_<offset>`), but its body becomes one call to the native function. So every way of reaching it is covered: direct `BL` inside a module, calls through the PLT, and function pointers. A runtime-only hook in `yw4_resolve` would miss direct calls. The translator refuses an override whose symbol isn't a defined function. This is the "per-function config for stubs and patches (TOML, N64Recomp-style)" from the Phase 2 plan. Natives use the AArch64 calling convention on the `CPUState`, like the old HLE functions.

**The graphics boundary is three functions:**
- `nvnBootstrapLoader`, NVN's only entry point: it returns our native functions (`runtime/src/nvn.c`).
- `nv::InitializeGraphics` / `nv::FinalizeGraphics`: do nothing. The 8 MiB the game hands over is the driver's bookkeeping memory, and we have no driver.

`nv::SetGraphicsAllocator` and the other `nv::` setters stay recompiled: they only store pointers.

**NVN names.** The game asks for `nvnDeviceGetProcAddress`, then for each NVN function by name. Every name gets a guest address (host callbacks can now carry a data pointer, `yw4_host_function_data`, so one "missing" stub stands behind all unwritten names). Calling an unwritten function stops with its name, its caller and the first arguments. Real NVN returns NULL for unknown names; the game only asks for names it uses, so giving every name an address is the stricter choice: a NULL might be silently tolerated.

## Result

The game skips the sdk's GPU driver and stops at its first NVN call, `nvnDeviceBuilderSetDefaults` (main+0x94a778). From here on it's Phase 4a: the NVN → Vulkan layer.
