# 0041 — Frame generation

<!-- decision
state: accepted
date: 2026-10-29
replaces: none
code: runtime/src/gpu_fg.c, runtime/src/gpu_dlssg.cpp, runtime/src/gpu_aa.c, runtime/src/gpu.c, runtime/src/window.c
-->

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
   - The swapchain is made again when the player switches DLSS-G on or off (NVIDIA's guidance: without it the
     frames still go through DLSS-G's off-screen copy while it's off), and its image list is read in two calls
     (count, then images): Streamline's version returned bad handles for one call with room for 8.
   - Menus and loading screens (no 3D tagged) pause DLSS-G with `eRetainResourcesWhenOff`: its memory stays and
     the swapchain isn't made again. Until 2026-11-02 each pause freed everything and made the swapchain again;
     switching back on then rebuilt DLSS-G inside the next `vkQueuePresentKHR`: a 185-230 ms hitch at every menu
     closed (owner's 10-minute session: all 9 frames over 120 ms), ~57 ms more on opening one, plus 13-43 ms per
     swapchain. Now the worst of those frames is ~50 ms (most 13-22 ms) and nothing in the session passes 80 ms.
     When the player switches it off, `slFreeResources` gives the memory back, as NVIDIA's guide asks.
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

- ~~Objects moved by their world matrix (not bones) have only the camera's motion~~: done 2026-11-28, see the
  change below.
- The game's real camera matrices for DLSS-G (from the G-buffer pass's constant buffers).
- Frame generation off in menus and loading screens (NVIDIA's advice).
- The FRAME RATE setting counts the game's frames; what is shown is about twice that.
- Linux: no FSR DLLs there yet.

## Change 2026-11-19: the game's camera for frame generation

Both frame generators had been given placeholder camera values: near 0.1, far 1000, 45°, and for DLSS's also an
identity clip-to-last-clip and a camera at the origin. The game's view camera block (vertex cbuf 1,
`CBUSE_UB_WORLD_MATRIX_IDX`) holds the real ones (rows 0-3 view, 4-7 projection, 8-11 their product, 12-23 the
inverses, 24-35 last frame's; near 0.05, far ~4000-5000, 45°). It is copied when a G-buffer draw binds it, latched
with the motion, and turned into Streamline's conventions in `gpu_aa.c` (`fg_camera`): row vectors, clip depth
0..1, the view looking down +z. FSR's gets the real near, far and field of view. `YW4_FG_CAMERA=0` keeps the
placeholders.

It did not change what it was made for: with frame generation on, a slowly turning camera shows a fine noisy
outline around the characters (both DLSS and FSR frame generation; gone with frame generation off; F12 doesn't
show it, as it saves only the game's own frames). Still open. Next suspects: the characters' black outline drawn
without motion or depth (compare a motion view and a normal picture while orbiting, by hand: replays don't frame
it), and the base frame rate (~50-57 here, under NVIDIA's suggested 60).

## Change 2026-11-22: FSR frame generation on the game's rectangle only

FSR frame generation warped and banded while the camera turned (characters doubled, bamboo smeared in the
generated frame); a still camera was clean, DLSS's was clean. Cause: FSR's context had the whole window image as
its display size (1920x1080) while the game's picture is only a part of it (1600x900 at 0,0, the window's crop).
FSR maps the 3D's motion and depth from render size onto display size by plain scaling, so every motion vector was
1.2x too long and taken from the wrong place. DLSS's never had it: Streamline gets that rectangle as the
backbuffer's subrect.

Now `gpu_fg.c` (`fsr_rect`) gives FSR the shown rectangle alone: the slots and the HUD-less copy hold just that
part at 0,0, the context's display size is its size, and the pacing thread presents the slot with the rectangle
moved to 0,0. Test (`tools.dev.scene lategame_cam`, AA FSR, frames 4000/4001 real and generated): with the crop
the generated frames are sharp and only shifted; with `YW4_FG_CROP=0` (the old way) the characters are doubled.

With it, two changes of the same day, correct by FSR's API but with no visible effect in that test: FSR gets the
camera's position and axes (`YW4_FG_CAM_VEC=0`: origin and identity axes, as before), and an NVIDIA D24S8 depth
goes to FSR as a D32 copy like AMD's D32S8 (`YW4_FSR_D24_COPY=0`: as it is, through FSR's own invalid view).

## Change 2026-11-28: each object's own motion (always on)

FSR frame generation left a trail behind the walking player (owner). The motion view (`YW4_AA=3 YW4_AA_DEBUG=4`,
`tools.dev.scene walk`, frame 3600) showed why: the player's body and Whisper had the ground's colour, the camera's
motion only, though the following camera keeps them almost still on screen. They are moved by their world matrix,
and the game puts this frame's world in the "last frame's" rows; FSR moved them with the background.

`gbuffer.c` already kept each object's world from the frame before (`last_world`), off because it painted parts of
the lawn, hedges and path with bright wrong motion. Those are pieces of one model sharing the key (the record the
world is read from), drawn with different worlds, so one piece got another's world as last frame's. Now a key seen
with two different worlds in one frame keeps the camera's motion only, for as long as it keeps its slot, and a world
that moved over 2 m in one frame gives no motion (a warp, or a key that changed hands). Against the old way, the
motion view differed only on the player and Whisper (and swaying leaves); in the walk, ~6-8 thousand draws had their
own last world per 300 frames and ~27 thousand shared keys stayed at the camera's motion.

The owner watched it in play (late-game save, 4K, FSR frame generation): "object motion alone cleans it up". So it
is always on, with no menu row: off breaks frame generation and the anti-aliasing on moving objects (owner);
`YW4_OBJECT_MOTION=0` turns it off for tests. The motion also reaches TAA, FSR,
DLSS and XeSS, which get the same motion target.

Tried the same day and dropped: pacing the generated frame halfway between the two game frames' times. It waited a
whole game frame step on top of each present, held the game back and lowered the frame rate (owner).

## Change 2026-11-28: when frame generation steps aside, and DLSS's UI layer

The owner saw DLSS frame generation smear our own pictures and the battles' start:

- **The overlay** (Escape): the game pauses but its 3D stays behind the panel, so frames were still tagged and DLSS
  made frames between overlay pictures, ghosting its text and buttons. Both frame generators now step aside while
  the overlay is open, F2's quick look included (`gamemenu_open`, `gamemenu_peeking`).
- **Our layers while the 3D runs** (the Preview bar, the HUD, notices): drawn into the window image, DLSS moved
  them with the picture. They are now drawn into a UI layer of their own (`window.c` `uil`, cleared each present,
  colour premultiplied), laid over the window image with one blended draw and tagged for DLSS
  (`kBufferTypeUIColorAndAlpha`, the backbuffer's rectangle) with user interface recomposition on, so DLSS lays it
  over every frame instead of interpolating it. Preview keeps frame generation on with it. FSR never had this: we
  draw our layers onto each of its frames after it made them.
- **Battles**: with the UI layer tagged the battles flickered as they start (the game's own battle pictures are in
  neither the HUD-less copy nor the UI layer), so it is not used in battles (`battle_active`), where our layers go
  into the picture as before; Preview in a battle turns frame generation off.
- **The battle's start blur** (the render job `main+0x897d90`, `dof.c`): a whole-screen blur growing each frame,
  which DLSS turned into ghosting. Frame generation (both) steps aside while a blur of 2 rounds or more ran in the
  last 0.1 s, about 1.3 s per battle start. The game also keeps a faint one-round blur (strength 0.04) for long
  parts of a battle (22 s in one), which doesn't count.

Owner, after each step in play (late-game save, 4K, DLSS, DLSS frame generation): "looks amazing", "looks good".
