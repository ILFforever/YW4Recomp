# 0041 — Frame generation

Status: accepted, first versions with AMD FSR and NVIDIA DLSS (2026-10-29).

## Context

In busy places the game's own CPU work limits the frame rate (~70 frames/s at the owner's 4K 21:9 settings,
`docs/known-issues.md`): upscaling frees the GPU there but can't add frames. Frame generation makes a picture
between each two of the game's from their motion and depth, so about twice as many are shown. It adds about half
a frame of delay (the game's frame waits while the generated one is on screen), so it suits a game that already
runs at 60 or more.

Both vendors' kits normally replace the window's swapchain and present on their own. Ours draws our HUD and the
settings panel onto the window image at every present (`window.c`), and the game's picture is copied, scaled and
letterboxed into the window there.

## Options considered

| Option | For | Against |
|---|---|---|
| **A. FSR without its swapchain** (`FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY`): FSR writes the generated frame into our image, we present and pace | Our present path stays; HUD and panel drawn sharp onto each frame; any GPU | Our own pacing (a thread) |
| B. FSR's frame-interpolation swapchain | AMD's pacing | Replaces our swapchain and present; the HUD and panel would be interpolated or need its UI hooks |
| C. DLSS frame generation first | NVIDIA's quality | RTX 40 and newer only; Streamline and Reflex to integrate first |

## Decision

1. **A, FSR first** (owner: FSR, then DLSS). The FSR package we fetch for the upscaler has the frame generation
   DLL (`amd_fidelityfx_framegeneration_vk.dll`, MIT like the rest we take).
2. **Where:** FSR's prepare pass right after the anti-aliasing (`gpu_aa.c`), with the 3D's depth, the game's
   motion and the jitter as the upscaler gets them; so frame generation needs anti-aliasing TAA, FSR or DLSS
   (only they copy the game's motion). At the present (`gpu.c`) the game's finished image is copied into one of
   two slots and FSR makes the generated frame into the same slot (`runtime/src/gpu_fg.c`).
3. **Pacing:** a host thread presents the generated frame at once and the game's frame half the game's frame time
   later (smoothed over ~10 frames). The main thread only hands the slot over, so it never waits for the half
   frame; it waits only when both slots are still to be shown. While frame generation is on, the window doesn't
   wait for the monitor (mailbox or immediate present).
4. **A HUD-less copy keeps the menus still.** The game draws them into the 3D's image, then copies that into the
   8-bit window image it presents. Without help FSR moved the menus with the 3D's motion (doubled HUD and minimap
   when the camera turned). Right after the anti-aliasing (before the menus) the 3D's picture is now blitted into
   an image in the presented format, at the place in it the window shows (the window image is larger than the
   picture: 1920×1080 around 1600×900 at the default resolution): the conversion the game's own copy makes,
   identical to the presented frame outside the menus. FSR gets it as `HUDLessColor` and keeps the menus still.
5. **sRGB:** the game presents `B8G8R8A8_SRGB`, which has no storage use. Our slot images are made in that format
   with a UNORM view too; FSR is told UNORM, so it reads and writes the encoded bytes (its sRGB transfer function)
   and the window's copy decodes them as it does the game's.

6. **DLSS frame generation** (setting 2) runs through NVIDIA Streamline 2.14.16 (`runtime/src/gpu_dlssg.cpp`, C++
   because Streamline's headers are; the build compiles `runtime/src/*.cpp` with the same clang, no exceptions or
   RTTI, so no C++ runtime library). Unlike FSR it owns the present: Streamline sits between us and Vulkan, puts
   its generated frame in at `vkQueuePresentKHR` and paces both itself, so our pacing thread is not used.
   - Started before the Vulkan instance on every PC with an NVIDIA GPU (asked through DXGI, as Vulkan can't be
     asked yet; `YW4_STREAMLINE=0`: never), so DLSS-G and Reflex switch while playing. Loaded with both off it
     cost nothing measurable (busy spot: 81.7 against 80.2 frames/s, within the noise). Every Vulkan function then
     comes from `sl.interposer.dll`'s `vkGetInstanceProcAddr` (Vulkan's own,
     Streamline's for instance and device creation, where it adds its extensions and an optical-flow queue, the
     surface, the swapchain and the present). Its manual hooking mode (only those few from it, the rest from SDL's
     loader) crashed in `sl.common` during `vkCreateDevice` on a null function pointer.
   - `sl.interposer.dll` is loaded at run time; its core functions are reached through pointers, and our
     definitions of them forward there so Streamline's inline helpers work unchanged.
   - Per frame: a frame token, Reflex's sleep and the markers (always, as NVIDIA asks). Reflex's low-latency mode
     is the REFLEX setting (off by default, on, on + boost) and at least on while DLSS-G is chosen (NVIDIA requires
     it; the panel row is greyed then). Where the CPU limits the frame rate it costs frames (busy spot: ~14%);
 after the anti-aliasing the 3D's depth and the game's motion are tagged with the camera constants;
     before the present the game's rectangle in the window image is tagged as the backbuffer subrect (black bars
     copied as they are).
   - The game's camera matrices aren't known: a projection from the 45° field of view the FSR code uses, the
     camera motion taken as part of the game's motion vectors (it is).
   - The swapchain is made again whenever DLSS-G switches on or off (NVIDIA's guidance), and its image list is
     read in two calls (count, then images): Streamline's version returned bad handles for one call with room for 8.
   - Its HUD-less copy: DLSS-G works on the window image (the game's picture scaled into it, black bars, our HUD
     and panel on top), so the HUD-less copy is made the same way: the picture before the UI blitted into a
     window-sized image at the same rectangle with the same filter, in the present's own submission, tagged with
     the backbuffer's rectangle. Walking through town (only generated frames shown,
     `YW4_DLSSG_SHOW_GENERATED=1`, captured from the screen): the HUD, minimap and text stay sharp, and running
     characters keep clean arms and legs.
   - DLSS-G stops generating while the window isn't focused (Streamline's own rule): background test runs need
     the window brought to the front.

## Results

Busy late-game spot (`save_base_busy`), RTX 4070 laptop, unlimited frame rate: every game frame got a generated
one. At 4K 21:9 with DLSS performance ~63 game frames/s, ~126 shown (~70 game frames/s without frame generation:
FSR's frame generation takes ~7 ms of GPU time per frame at 5120×2160, so the GPU is nearly busy). Generated
frames checked in screenshots against the game's at the same spot: same brightness, no broken areas.

DLSS frame generation, same spot and settings, window focused: 2 frames shown per game frame (Streamline's
count), ~75 game frames/s, ~150 shown; our queue's GPU time 6.3 ms (DLSS-G's own queues aren't in that number).
Its generated frames can't be screenshot by us (Streamline makes them after our present).

The HUD (F3) shows the frames on screen in a SHOWN line under FPS (the game's): FSR's share of frames with a
generated one, DLSS-G's own count.

Walking through town with FSR, the generated frames sit between the game's (camera turns, the road, poles and
trees move half way); the menus stay still with the HUD-less copy. A character's arms and legs can come out
doubled while they swing: last frame's bones are right (the palette ring moves on every frame, measured), so
this is where FSR blends instead of moving the pixels.

## Not done yet

- Objects moved by their world matrix (not bones) have only the camera's motion; a history of each object's world
  is in `gbuffer.c` (`YW4_OBJECT_MOTION=1`), off until objects sharing a model get keys of their own.
- The game's real camera matrices for DLSS-G (from the G-buffer pass's constant buffers).
- Frame generation off in menus and loading screens (NVIDIA's advice).
- The FRAME RATE setting counts the game's frames; what is shown is about twice that.
- Linux: no FSR DLLs there yet.
