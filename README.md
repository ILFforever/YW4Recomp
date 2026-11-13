<h1 align="center">YW4 Recomp</h1>

<p align="center">
  <b>Yo-kai Watch 4++, recompiled for PC.</b>
  <br>
  4K · Ultrawide 21:9 / 32:9 · PCSS soft shadows · TAA / SMAA · ×16 texture filtering · Xbox, PlayStation and Joy-Cons
  <br>
  <sub>A fan static recompilation of the Nintendo Switch game (update 2.2.0), in the style of <a href="https://github.com/N64Recomp/N64Recomp">N64Recomp</a>: the game's code is translated into a native Windows program.</sub>
</p>

<p align="center">
  <img alt="Phase 4: renderer" src="https://img.shields.io/badge/phase-4%20·%20renderer-6f42c1">
  <img alt="Platform: Windows, Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational">
  <img alt="Graphics" src="https://img.shields.io/badge/graphics-Vulkan-ac162c">
  <img alt="Version: 1.1.0" src="https://img.shields.io/badge/version-1.1.0-blue">
  <a href="https://buymeacoffee.com/ilfforever"><img alt="Buy Me a Coffee" src="https://img.shields.io/badge/Buy%20Me%20a%20Coffee-ilfforever-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black"></a>
</p>

<p align="center">
  <img src="docs/screenshots/05-title-screen.jpg" alt="The title screen, running recompiled on Windows" width="820">
  <br>
  <sub>The title screen, recompiled and running natively on Windows (Vulkan, 30 frames/s).</sub>
</p>

> [!IMPORTANT]
> **Built with AI.** Claude (Anthropic's model, used through Claude Code) wrote most of this repository: the
> recompiler, the runtime, the reverse engineering notes and these docs. A human decides what to work on, plays and
> tests every build, compares it against the real game and reviews the changes. It is a personal fan project that
> happens to be public. If AI-written code is not something you want to use or read, feel free to pass on it; if you
> do run it, that is your call.

> [!NOTE]
> Platforms:
> - Windows - x86-64, Vulkan graphics backend (this is what is built and tested): NVIDIA RTX, and AMD Radeon
>   ([decision 0059](docs/decisions/0059-amd-depth-format-fallback.md)): a Ryzen 5 7530U laptop's integrated graphics
>   runs it at about 35-40 frames/s at native resolution, where the Eden emulator gets 22-30
> - Linux - x86-64, Vulkan: works. Tester [ouni666](https://github.com/ouni666) plays it on AOSC OS (KDE Plasma, Wayland) with an AMD RX 6600 XT at
>   about 110-120 frames/s, the GPU at full load, with sound and controls working. Built from source for now (the launcher is Windows only). FSR,
>   DLSS, frame generation and Reflex are Windows only for now

> [!NOTE]
> **Bring your own game.** Nothing from the game is in this repository. You need files from your own copy of
> *Yo-kai Watch 4++* with the last update, **2.2.0**. Getting those files is up to you and is not covered here.
>

## Status

> [!TIP]
> **1.1.0 is out (beta):** get the launcher, it installs the game and keeps it up to date.
> [**Download it here**](https://github.com/ILFforever/YW4Recomp/releases/latest), then see [How to play](#how-to-play).

**Playable, but not finished.** Nobody has played the game from start to finish yet.

- **Tested:** from boot through the start of a new game (the opening, the Amano house and the town), and from
  late-game saves: towns at night, the bamboo forest, the Yo-kai World and battles.
- **Not tested:** most of the story in between. Expect missing effects and crashes in places nobody has reached.
- **Found a bug?** Use **Report a bug** in the launcher or in the game's overlay.

More: [what is known to be missing or wrong](docs/known-issues.md) · [how it got here, step by step](docs/progress.md)

## How to play

**[Download the launcher](https://github.com/ILFforever/YW4Recomp/releases/latest)** (Windows): the installer, or
the portable zip, which is the same app with no installer.

It takes your game files (NSP, XCI or extracted folders), checks them, builds the game on your PC once (about 10 to
15 minutes) and starts it. After that it opens on **Play**, with what's new, your screenshots, mods, DLC, saves and
logs. Its theme is synced with the in-game overlay's, and game updates come through it. Every step and screen is on
[the launcher's page](https://ilfforever.github.io/YW4Recomp/launcher/). On Linux, or to work on the code,
[build from source](#building-from-source).

<details>
<summary>The launcher building the game (video)</summary>

[![YW4 Recomp building process (video on YouTube)](https://i.ytimg.com/vi/Vnpo5EPti9w/maxresdefault.jpg)](https://youtu.be/Vnpo5EPti9w)

</details>

![The launcher: the first start with the welcome popup; your game files; Play with what's new; Settings](docs/screenshots/99-launcher.jpg)

![Launcher themes: Jibanyan and Whisper](docs/screenshots/99-launcher-themes.jpg)

## Why not just use an emulator?

Switch emulators already run *Yo-kai Watch 4++* at full speed on a good PC. This project aims further:
**better rendering, better graphics and better performance** than the Switch version (see [Goals](#goals)).

An emulator can't do most of this. It imitates the console and plays the game's GPU commands as they are, so the
game still decides its shadow quality, its 16:9 UI, its 30 frames/s and its effects. Here the game's renderer is
being replaced with our own, so those become ours to change: higher resolution, sharper textures, bigger and soft
shadows, anti-aliasing that leaves the UI sharp (TAA included) and ultrawide are already in, as are 60 frames/s and
above, upscaling with NVIDIA DLSS or AMD FSR (the world drawn smaller and rebuilt at full size, the menus kept
sharp) and frame generation with either (a frame made between each two of the game's).

A town street, same spot: the game's shadows (1024 map, no PCSS) against ours (4096 map, PCSS ×2). The
characters' shadows keep their shape, and the tree shade on the pavement is soft:

![Stock (the game's): Stock shadows: blurry character shadows, hard tree shade; Improved (ours): Improved shadows: defined character shadows, soft tree shade](docs/screenshots/82-shadows-stock-vs-improved.jpg)

It is also lighter to run. The same busy street on the same laptop ([docs/baseline.md](docs/baseline.md),
2026-10-22), both at the game's own settings (1600×900, 30 frames/s, every improvement of this project off), so
the two do the same work:

| | Emulator (Eden) | This project |
|---|---|---|
| CPU | 210% of one core | 111% |
| Memory | ~7 GB | 3.9 GB |
| GPU | 30% | 33% |

Memory: Eden's is private + mapped (the emulated console's RAM is mapped memory, which Task Manager's Memory
column leaves out: it shows about 1.3 GB for Eden and 2.8 GB for this project). Ours is everything the game has
committed.

On an ASUS Zenbook (Ryzen 5 7530U, integrated Radeon graphics): about 35-40 frames/s here, 22-30 in Eden (more on the site's [Showcases](https://ilfforever.github.io/YW4Recomp/showcases/) page).

<details>
<summary>YW4 Recomp vs Eden on the Zenbook (video)</summary>

[![YW4 Recomp vs Eden comparison on Potato PC (video on YouTube)](https://i.ytimg.com/vi/HfCrAkMDOzs/maxresdefault.jpg)](https://youtu.be/HfCrAkMDOzs)

</details>

## Features

What the PC version adds to the Switch game. Open an entry for details, pictures and measurements; more in [docs/graphics.md](docs/graphics.md).

**[Compare them with before/after sliders →](https://ilfforever.github.io/YW4Recomp/compare/)** (the project's site: [ilfforever.github.io/YW4Recomp](https://ilfforever.github.io/YW4Recomp/))

**Graphics**

<details>
<summary><b>Resolution</b>: up to 4K, menus and text at the same size</summary>

The 3D world at 1600×900 (the Switch's), 1920×1080, 2560×1440 or 3840×2160; the menus and text are drawn at the same size on top. About 90+ frames/s at 4K on an RTX 4070 laptop with the other features off; with upscaling (under Performance) higher.

</details>

<details>
<summary><b>Aspect ratio</b>: 16:10, 21:9 and 32:9, the UI kept 16:9 in the middle</summary>

16:9 (the Switch's), 16:10, 21:9 and 32:9. Below, 21:9 in the schoolyard: the world goes on at
the sides; the minimap and button hints stay where they are on the Switch.

![21:9 ultrawide: a wider schoolyard with the UI in the middle](docs/screenshots/23-ultrawide-21-9.jpg)

Exploring, cutscenes and battles all work at the wider ratios. 21:9 at 4K height (5120×2160), from play:

![Town: 21:9 town street; Mansion: 21:9 mansion hall; Cutscene: arriving at Mount Oomori: 21:9 cutscene with the location title; Cutscene: dialogue: 21:9 cutscene with Jibanyan and Whisper](docs/screenshots/83-ultrawide-play.jpg)

Layers over the whole screen, like the dimming behind a dialog and the loading screen's black, cover the full
width; pictures made for 16:9 (the copyright notice, movies, the dungeon map's backdrop) stay 16:9 in the middle
with black bars, and the town's fast-travel map shows more of the town at the sides. At 4K ultrawide there are
occasional stutters in heavy scenes; more optimization is planned ([known issues](docs/known-issues.md)).

Menus, maps and battles at 21:9 (the dialog at 2133×900, the rest at 5120×2160):

![Yo-kai Pad: 21:9 Yo-kai Pad menu; Fast-travel map: more of the town at the sides: 21:9 town fast-travel map reaching the edges; Dungeon map: a 16:9 picture with black bars: 21:9 bamboo forest map with black side bars; Dialog: the dimming covers the full width: 21:9 dialog with the whole screen dimmed; Into a battle: 21:9 swirl into a battle; Battle start: 21:9 battle start with the enemy yo-kai; Battle: 21:9 battle with the command wheel; Battle results: 21:9 battle results over the blurred field](docs/screenshots/84-ultrawide-menus-battles.jpg)

</details>

<details>
<summary><b>Shadow map size</b>: up to 4096, for sharper shadow edges</summary>

A bigger shadow map gives sharper, more detailed shadow edges, most visibly on shadows far from
the camera. The bamboo forest, PCSS off:

![1024 (the game's): 1024 shadow map; 2048 shadow map; 4096 (recommended): 4096 shadow map](docs/screenshots/85-shadow-map-sizes.jpg)

</details>

<details>
<summary><b>Soft shadows (PCSS)</b>: sharp where a shadow touches its caster, softer farther away</summary>

PCSS (percentage-closer soft shadows) makes shadows behave more like real ones. It softens a shadow by how far it
is from what casts it:

- **Close shadows stay sharp**: where a shadow touches its caster (feet, poles, the base of a wall).
- **Farther shadows get softer**: the farther the caster is from the ground, the softer the edge, so tree canopies
  become soft dappled shade instead of a field of hard dots.
- **Less shimmering**: hard edges flicker as the camera moves; softer ones spread that out, so distant shadows
  shimmer much less.

![PCSS off: hard, speckled leaf shadows; PCSS ×2 (recommended): PCSS x2](docs/screenshots/86-pcss-off-vs-x2.jpg)

</details>

<details>
<summary><b>Contact shadows</b>: the small shadows the shadow map is too coarse for</summary>

The shadow map can't see small, close detail. Contact shadows fill it in from the picture itself: a short ray from
each point toward the sun finds what's in the way.

- **Small objects cast their shadow**: the taps over the schoolyard sink now shade the sink below them.
- **Bushes, trees and flower beds get depth**: the leaves shade each other, so they are no longer flat green.
- **Under ledges and eaves**: a thin dark line where a wall's cap, a roof edge or a planter sticks out.
- **Cheap**: about 0.25 ms per frame at 4K in a sunny town, less in the shade.

They only come from what's on screen, so something just outside the picture casts none. Darkness under benches
and in corners is a different effect (ambient occlusion), not this one. In the panel (F1, SHADOWS, CONTACT
SHADOWS) to compare.

Off and on, same spot:

![Taps · contact shadows off: no shadow under the taps; on: each tap shades the sink below it](docs/screenshots/93-contact-shadows-taps.jpg)

![Balconies · contact shadows off; on: lines under the balconies](docs/screenshots/94-contact-shadows-balconies.jpg)

![Awning · contact shadows off; on: shade under the window's awning](docs/screenshots/95-contact-shadows-awning.jpg)

![Hedges · contact shadows off; on: depth in the hedges](docs/screenshots/96-contact-shadows-hedges.jpg)

</details>

<details>
<summary><b>Anti-aliasing</b>: FXAA, SMAA and TAA, with the UI kept sharp</summary>

The Switch version has none. Three kinds are in, all applied to the 3D scene before the UI is
drawn, so text and menus stay sharp:

- **FXAA**: fast, smooths jagged edges in one frame.
- **SMAA**: sharper edge smoothing than FXAA, also one frame at a time.
- **TAA**: also fixes shimmer. Thin things (fences, the goal net, wires, leaves) fall between pixels differently
  every frame, so they crawl while the camera moves. TAA shifts the 3D scene by a fraction of a pixel each frame and
  blends the frames together, using the motion the game already writes for every pixel. In the schoolyard the goal
  net breaks into dots and dashes without AA; with TAA it is a continuous mesh. A light sharpen keeps textures crisp
  ([decision 0032](docs/decisions/0032-taa.md)).

Yo-kai World with TAA, 21:9 at 4K height (5120×2160):

![Yo-kai World street with TAA](docs/screenshots/29-aa-taa-yokai-world.jpg)

The same frame zoomed in 3×: the red railing, the fence by the lamp post and the road line.

![Railing · Off: Railing, no AA: stair-stepped edges; SMAA: Railing, SMAA; TAA: Railing, TAA: smooth edges; Fence · Off: Fence, no AA: sparkling bars; SMAA: Fence, SMAA; TAA: Fence, TAA: steady bars; Road line · Off: Road line, no AA; SMAA: Road line, SMAA; TAA: Road line, TAA](docs/screenshots/87-aa-zoomed.png)

</details>

<details>
<summary><b>Texture filtering</b>: anisotropic up to ×16, sharp ground into the distance</summary>

The Switch version uses none, so the ground blurs a short way ahead of the camera. Anisotropic
filtering keeps the road, pavement and walls sharp into the distance, even at a low angle. Yo-kai World with ×16:

![Yo-kai World street with ×16 texture filtering](docs/screenshots/39-filtering-x16-yokai-world.jpg)

The same spot zoomed in 2×: the crosswalk far ahead, the pavement on the right and the road surface.

![Crosswalk · Off (the Switch's): Crosswalk, off: faded, blurred stripes; ×2: Crosswalk, x2; ×4: Crosswalk, x4; ×8: Crosswalk, x8; ×16: Crosswalk, x16: crisp white stripes; Pavement · Off (the Switch's): Pavement, off: smooth blur; ×2: Pavement, x2; ×4: Pavement, x4; ×8: Pavement, x8; ×16: Pavement, x16: brick pattern visible; Road · Off (the Switch's): Road, off: smeared; ×2: Road, x2; ×4: Road, x4; ×8: Road, x8; ×16: Road, x16: grainy asphalt](docs/screenshots/88-filtering-zoomed.png)

</details>

**Performance**

<details>
<summary><b>Frame rate</b>: 30, 60, 120, 144 or no limit, switchable while playing</summary>

FRAME RATE in the settings panel: the game's 30 frames/s, or 60, 120, 144 or no limit. The game already moves by the real time between frames, so walking, battles, menus and cutscenes all work at any rate (~140 frames/s at 1600×900). At 4K the GPU is the limit: upscaling lifts it, frame generation doubles what is shown.

</details>

<details>
<summary><b>Upscaling (NVIDIA DLSS, AMD FSR)</b>: the world drawn smaller, rebuilt at full size, menus kept sharp</summary>

With ANTI-ALIASING set to DLSS or FSR, the UPSCALING row draws the 3D world smaller than the resolution and the
upscaler rebuilds a full-size picture from it and the frames before, using the motion the game writes. The menus
and text are drawn afterwards at full size.

**Which to use: DLSS for the cleanest picture, FSR for the most frames.** On an NVIDIA RTX GPU, DLSS keeps fine
patterns (perforated fences, leaves) clean where FSR turns them to grain, and shadow edges still. It uses NVIDIA's
newest model, which costs more GPU time than FSR; with it, **balanced is the fastest DLSS mode** (its performance
mode uses a heavier model). FSR runs on any GPU and gives more frames in every mode. DLSS needs NVIDIA RTX;
choosing it elsewhere falls back to FSR.

Bamboo forest at 4K 21:9 on an RTX 4070 laptop, frames/s (for comparison, native 5120×2160 with TAA: **~81**):

| Mode | World drawn at | DLSS | FSR |
|---|---|---|---|
| native, UPSCALING off (DLSS / FSR as anti-aliasing only) | 5120×2160 | ~61 (DLAA) | ~58 |
| quality | 3413×1440 | ~85 | **~94** |
| balanced | 3013×1271 | ~92 | **~106** |
| performance | 2560×1080 | ~82 | **~118** |

DLSS's own work costs 5.2-5.7 ms of GPU time per frame at this output size (7.4 ms in performance mode), FSR's
3.6-4.3 ms (6.3 at native). Measured 2026-10-31, standing still on the late-game save, FRAME RATE unlimited, frame
generation and Reflex off; the other settings at their defaults: EFFECTS RESOLUTION half, SHADOW MAP 4096, SOFT
SHADOWS x2, TEXTURE FILTERING x16, DISTANCE BLUR half, and our own renderer's passes on.

Each image below: on top the whole native frame with the cropped areas in yellow, underneath the 1:1 crops, each
labelled with its mode. The DLSS images were taken with its older, faster model, removed since because it made
shadow edges flicker; they haven't been retaken with the newest model yet.

Quality (the world drawn at 3413×1440): native, FSR, DLSS. The quest text is as sharp in all three (the UI is
drawn at full size); DLSS keeps the bamboo and the lantern closest to native.

![Bamboo forest in native, FSR quality and DLSS quality](docs/screenshots/63-fsr-dlss-quality-vs-native.jpg)

Performance (the world drawn at 2560×1080, a quarter of the pixels): native, FSR, DLSS. DLSS keeps the bamboo and
the leaves crisper.

![Bamboo forest in native, FSR performance and DLSS performance](docs/screenshots/67-dlss-vs-fsr-performance-forest.jpg)

The schoolyard's perforated high fence at performance: native, FSR, DLSS. FSR turns the holes into grain, DLSS
keeps a clean pattern.

![The perforated fence in native, FSR performance and DLSS performance](docs/screenshots/65-dlss-vs-fsr-fence.jpg)

The schoolyard's far goal net at balanced (the world drawn at 3013×1271): native, FSR, DLSS. Both keep the net
at this mode; DLSS's mesh is the more even.

![Goal net in native, FSR balanced and DLSS balanced](docs/screenshots/68-dlss-vs-fsr-goal-net-balanced.jpg)

A running character at quality, then at performance: native, FSR, DLSS. No trail behind the moving leg in either.

![Running character in native, FSR quality and DLSS quality](docs/screenshots/64-fsr-dlss-running-quality.jpg)

![Running character in native, FSR performance and DLSS performance](docs/screenshots/69-fsr-dlss-running-performance.jpg)

A whole frame in DLSS performance, the world drawn at 2560×1080 and shown at 5120×2160 (scaled down here), ~112
frames/s on the laptop:

![A town street in DLSS performance](docs/screenshots/70-dlss-performance-town.jpg)

How they work and what was measured: [decisions 0039 (FSR)](docs/decisions/0039-fsr-upscaling.md) and
[0040 (DLSS)](docs/decisions/0040-dlss-upscaling.md).

NVIDIA DLSS is a technology of NVIDIA Corporation, used under NVIDIA's RTX SDK licence; its files are downloaded
by the build and are not part of this repository. AMD FSR is AMD's, under the MIT licence.

</details>

<details>
<summary><b>Frame generation (NVIDIA DLSS, AMD FSR)</b>: a frame made between each two of the game's, the menus kept still</summary>

With ANTI-ALIASING set to TAA, FSR or DLSS, the FRAME GENERATION row makes one frame between each two the game
draws, from the motion the game writes and the depth, and shows it in between: about twice as many frames on screen.
It switches while playing. The menus, our HUD and the settings panel stay still and sharp: frame generation is given
the picture from before the menus are drawn, so it can tell them from the world.

**Which to use: on an NVIDIA RTX 40 or newer, choose DLSS.** It leaves the game more frames of its own and keeps
moving characters cleaner. **On other GPUs, use FSR** (DLSS frame generation needs RTX 40 or newer; the row skips it
elsewhere).

Busy late-game town at 4K 21:9 on an RTX 4070 laptop (DLSS performance upscaling, FRAME RATE unlimited). Without
frame generation: **~70 frames/s**. With it:

| Frame generation | Game frames/s | Frames on screen/s |
|---|---|---|
| off | ~70 | ~70 |
| FSR | ~63 | ~126 |
| DLSS | **~75** | **~150** |

The game draws fewer frames of its own with FSR because its frame generation runs on the same GPU (~7 ms per frame
at 5120×2160); DLSS's runs on the RTX's own hardware for it. The HUD (F3) shows both numbers: FPS counts the game's
frames, SHOWN the frames on screen.

Things to know:

- It adds about half a frame of input delay, so it feels best where the game already runs at 60 or more.
- FRAME RATE limits the game's own frames; what is shown is about twice that.
- DLSS frame generation pauses while the game window isn't the focused window (NVIDIA's rule).
- With DLSS frame generation, NVIDIA Reflex is on (NVIDIA requires it). The REFLEX row sets it otherwise: it lowers
  input delay most when the GPU limits the frame rate; where the CPU does, it costs some frames (~14% in a busy town).

FSR, Nate running across the schoolyard: the game's frame before, the frame FSR made, the game's frame after. The
minimap, clock and buttons stay sharp. Fast arms and legs, and Whisper's edge, come out blended in the made frame:
FSR's weak spot with thin things that move on their own (the thin dark line at the right edge is FSR's too).

![FSR frame generation: the game's frame before, FSR's frame and the frame after](docs/screenshots/71-fg-fsr-schoolyard.jpg)

DLSS, by the school's windows: a frame DLSS made. DLSS makes its frames after ours are presented, so they can only
be captured from the screen (here a 3440×1369 window with only DLSS's frames shown); the crops are 1:1 of that
capture. The HUD, minimap and characters come out clean.

![A frame made by DLSS frame generation, with the HUD and the minimap](docs/screenshots/72-fg-dlss-generated.jpg)

How they work and what was measured: [decision 0041](docs/decisions/0041-frame-generation.md).

NVIDIA DLSS Frame Generation, Reflex and Streamline are NVIDIA Corporation's (Streamline under the MIT licence, the
DLSS and Reflex files under NVIDIA's RTX SDK licence), downloaded by the build and not part of this repository. AMD
FSR is AMD's, under the MIT licence.

</details>

**Controls**

<details>
<summary><b>Keyboard, mouse and controllers</b>: rebind everything, the mouse turns the camera, a quick look at what every button does</summary>

**Controls quick look** (hold Back, or F2): a card over the game shows the controller with what each button does.
It uses your bindings and your controller's button names, and has one layout for exploring and one for battles.

![The quick look with a DualSense while exploring: each button of the controller joined to what it does, from Watch scan on L2 to Interact / confirm on cross](docs/screenshots/76-controls-quick-look-controller.jpg)

With a keyboard it shows the keys instead:

![The quick look on the keyboard: every bound key coloured and named: game buttons, moving and the camera, menus and screenshots](docs/screenshots/76-controls-quick-look-keyboard.jpg)

**Controls** in the overlay, with a DualSense: every Switch button with its keys and its controller input, named as
printed on yours.

![The overlay's Controls page with a DualSense: the mouse's sensitivities and buttons at the top, then each Switch button with its keys and its PlayStation symbol](docs/screenshots/75-controls-dualsense.jpg)

**Mouse buttons**: what left, right, the wheel press and turning the wheel do, for exploring and for battles.

![The Mouse buttons page for battles: the mouse drawn with Attack on the left button and Aim on the right, the wheel switching character](docs/screenshots/75-controls-mouse-buttons.jpg)

**Controllers:** Xbox, PlayStation and Joy-Cons work, and the controls page names the buttons as printed on
yours (A B X Y and LB/RT on Xbox; × ○ □ △ and L1-R3 on PlayStation; ZL/ZR on Nintendo). By default the game's
confirm is your controller's own confirm: × on a DualSense, A on Xbox. **Haptics and vibration** work through the
controller's motors ([decision 0058](docs/decisions/0058-controller-rumble.md)), downmixing dual actuators to low/high rumble.
It is on by default; a **CONTROLLER VIBRATION** switch on the overlay's Controls page turns it off.
Tested by playing with a **DualSense (PS5)**, battles included, and with the controls page.

**Mouse:** the game's code still has a mouse camera that the Switch version never feeds; we feed it, so the mouse
turns the camera. A click in the window gives the mouse to the game, Alt or opening the panel or the overlay gives it
back. The mouse also works in the Yo-kai Pad and its pages: hover, click, and the wheel scrolls lists (work in
progress, [decision 0057](docs/decisions/0057-mouse-menu-lists.md)). By default the left button attacks and the right
button aims in battles, and the wheel selects characters; each is set on the Mouse buttons page.

</details>

<details>
<summary><b>Button icons</b>: the keys or buttons you actually use, in the HUD and in text</summary>

**Button icons** in the HUD and in text follow what you last pressed, a key or a controller, and your bindings.
The game's files still hold keyboard, Xbox and PlayStation icon sets that the Switch version never shows; we
show those instead of the Switch buttons. Below, the shortcut wheel and the minimap's search button with each:

![Switch (the game's): Switch icons: ZL, ZR and R; Keyboard icons: the bound keys 1, 3 and E; Xbox icons: LT, RT and RB; PlayStation icons: L2, R2 and R1](docs/screenshots/89-button-icons.png)

</details>

**Mods**

<details>
<summary><b>Mods</b>: the English translation patch and other Switch mods, without touching your dump</summary>

Put a mod in `work/mods`, in the layout a modded Switch uses: `work/mods/contents/010086c00af7c000/<mod name>/romfs/...`
(or simply `work/mods/<mod name>/romfs/...`). At start its files replace the game's at the same paths and its new
files are added, as on a Switch; your dump is not changed. The log names each mod it loaded. The launcher has an
on/off switch for each mod. `YW4_MODS` points to
another folder, `YW4_MODS=none` turns mods off. The fan **English translation patch** works this way (not included,
get yours from [GameBanana](https://gamebanana.com/mods/480884)):

![The warning before the title: The copyright warning in English; The town: "Head to Mt. Wildwood!": The town with the quest marker in English: Head to Mt. Wildwood!](docs/screenshots/90-english-patch.jpg)

</details>

<details>
<summary><b>YKW Mod Merger™</b>: mods that change the same game file work together, which no Switch or emulator setup does</summary>

![The launcher's Mods tab with six mods on: one group of mods clash, Crank-a-kai Overhaul and Unrestricted Crank; Free Tag Battles, the English translation, Uncapped Training and Unshackled Equipment work alongside](docs/screenshots/91-mod-merger.jpg)

Six mods on at once: the English translation and five gameplay mods, three of which change the same item list.
The launcher flags one pair only: the two Crank-a-kai mods, which change the same prizes. The rest are merged when
the game starts.

| | Modded Switch | Emulators | YW4 Recomp |
|---|---|---|---|
| Several mods | One `romfs` folder per game: you copy them all into it | A folder per mod, each on or off | A folder per mod, each on or off in the launcher |
| Two mods ship `cpk_list` (the game's list of which file is in which archive) | The copy made last wins: the other mod's files, often the translation's, come from the Japanese archives again | One copy wins | Merged: every file any mod takes out of the archives stays out, with its real size |
| Two mods change one data file (items, shops, NPCs...) | One copy wins, the other mod does nothing | One copy wins | Merged entry by entry against the game's own copy: both work unless they change the same entries |
| Clashes | Found while playing | Found while playing | The launcher names the mods that clash before you play |

**Limitations**
- **Code patches don't work** (`exefs` `.ips` / `.pchtxt`, such as FPS or name-spacing patches): the game's code is
  recompiled for PC, so its original bytes are never run. Frame rate, widescreen and English names are built in.
- **Only data lists are merged**: Level-5's `.cfg.bin` files. Textures, models, sound and whole archives (`.cpk`)
  can't be: when two mods ship one of those, the one whose folder name sorts later is used.
- **Two mods changing the same entries still clash**, as the two Crank-a-kai mods do: keep one on.
- A mod that adds a new kind of entry to a list isn't merged into it (the later copy is used, and the launcher says so).
- Mods aren't tested one by one: one made for another game version can still misbehave.

How it works: [decision 0067](docs/decisions/0067-mods-merge-cpk-list.md) (the archive list),
[decision 0068](docs/decisions/0068-mods-merge-data-files.md) (data files).

</details>

**Settings**

<details>
<summary><b>In-game overlay</b>: a PC menu over the game, like Steam's overlay, with full mouse and keyboard (Escape)</summary>

A custom menu drawn over the game, as Steam's overlay is. It works with the **mouse**
(hover, click, drag the sliders, right-click to go back), the keyboard or a controller; the button hints show
whichever you used last. Press **Escape** to pause and open it. On a controller: the **touchpad** on PlayStation, **Share** on Xbox
Series, **Capture** on a Switch Pro controller, or **Back + Start** together on any controller. The game pauses the
way its own pause does (the whole world stops), without its pause screen. From the side panel:

- **Settings**: every option of the settings panel, by section, with what each choice means (the developer options
  need LEGACY MENUS on, see below). **Preview** (P, or Y /
  △) lets the game run again with a bar at the bottom holding that section's settings, so you see each change live
  and what it does to the frame rate; Escape pauses again.
- **Controls**: a picture of your keyboard or controller with what every key and button does (exploring, battle,
  cutscene), and the list to change them: choose one, then press the new key or button.
- **Overlay**: the performance HUD off, compact (the frame rate only) or detailed, which corner it sits in, its size and opacity, and
  what the detailed one shows.
- **Report a bug**: describe the problem, then save a zip with a screenshot and the log, or send it to GitHub (it
  opens the form). **Exit game** (it asks first). F12 takes screenshots, in the overlay or not.

W/S or the arrow keys choose, A/D or ←/→ change a setting, Enter selects, Q/E jump between sections (LB/RB or
LT/RT on a controller). Holding a direction keeps moving. Escape, Backspace or B goes back one step; from the side
panel it goes back to the game, as Resume does. On a DualSense the **mic button** shows or hides the performance HUD.

![The in-game overlay: Settings with the display options; Preview: the game running under a bar with the shadow settings; Controls with a DualSense's buttons; Overlay: the detailed performance HUD with a live preview](docs/screenshots/97-overlay.jpg)

**Themes:** Settings › Look › Theme gives the overlay and the performance HUD the colours of a Yo-kai, synced with
the launcher: the game opens in the launcher's theme, and a theme picked in the game is the launcher's too after it
closes (a theme file added to the launcher's themes folder shows up in both).

![Overlay themes: Jibanyan, Komasan, Shmoopie and Whisper](docs/screenshots/98-overlay-themes.jpg)

</details>

<details>
<summary><b>Settings</b>: every option in game, F1 for settings, F2 for controls, F3 for the performance HUD</summary>

**F1** opens the overlay's **Settings** page: every option with all its choices, and what each one does (arrow keys or
D-pad to choose and change). **F2** shows the **quick look** at the controls over the running game (see Controls
above); Escape from there opens the overlay's **Controls** page: two keys or mouse buttons and one gamepad
button for every Switch button and stick direction, changed by pressing the new one, and the mouse's camera
sensitivity, left-right and up-down. **F3** steps the performance HUD: off, compact, detailed. F1 pauses the
game while its page is open; the same key again goes back to the game. On a controller, open the pages from the
in-game overlay (above), and hold Back for the quick look.

**LEGACY MENUS** (Settings, ADVANCED) turns the keys back to the older panels: F1 opens the old settings panel, F2 the
old controls page, F3 shows or hides the HUD. The **developer options**, the switches players don't need, are at the
end of Settings, ADVANCED: which parts of the picture the game draws and which our own renderer does (the picture is the same), and
speed-ups that look the same. Your choices are saved in
`settings.ini` next to `yw4.exe` and kept for the next start. **F11** switches between full screen and a window (also
the panel's WINDOW MODE row). **F12** takes a screenshot. The other function keys are debugging tools ([docs/debugging.md](docs/debugging.md)).

![Settings (F1): The settings panel: display, anti-aliasing and upscaling, frame generation and latency, detail, shadows; Developer options: The developer options: shadow pass, G-buffer, bloom, distance blur pass, battle blur](docs/screenshots/91-settings-panels.png)

| Setting | What it does |
|---|---|
| **Display** | |
| Resolution | 1600×900 (the Switch's), 1920×1080, 2560×1440 or 3840×2160; the UI is drawn at the same size |
| Aspect ratio | 16:9 (the Switch's), 16:10, 21:9 or 32:9: the world shows more at the sides, the UI stays 16:9 in the middle |
| Frame rate | 30 (the Switch's), 60, 120, 144, VSYNC (the monitor's refresh rate, smoothest; applies after a restart) or unlimited; the game keeps its speed at any rate |
| HUD scale | A slider: auto (default, grows with the window) or 0.75× to 3× in steps of 0.05: the performance HUD (F3) in the window's corner |
| **Anti-aliasing and upscaling** | |
| Anti-aliasing | Off (default), FXAA, SMAA, TAA, FSR or DLSS, applied to the 3D scene before the UI is drawn, so text stays sharp. TAA, FSR and DLSS also steady thin things that shimmer: fences, nets, leaves |
| Upscaling | Off (default), quality, balanced or performance: the world drawn smaller and rebuilt at full size by FSR or DLSS. Needs anti-aliasing set to FSR or DLSS |
| **Frame generation and latency** | |
| Frame generation | Off (default), FSR (any GPU) or DLSS (NVIDIA RTX 40 and newer): a frame made between each two of the game's, about twice as many on screen. Needs anti-aliasing set to TAA, FSR or DLSS |
| Reflex | NVIDIA GPUs: off (default), on or on + boost: less input delay, most where the GPU limits the frame rate. On while frame generation is DLSS |
| **Detail** | |
| Texture filtering | Off (the Switch's) or anisotropic ×2 to ×16: distant and slanted ground, roads and walls stay sharp. Default ×16 |
| Effects resolution | Half (the Switch's) or full: the shadow mask, lamp lights and ambient occlusion at the 3D view's size, for sharper shadow edges. Costs ~2 ms of GPU at 1600×900 |
| Distance blur | Full (the Switch's), half or off: the game blurs everything far away. Default half |
| **Shadows** | |
| Shadow map | 1024 (the Switch's), 2048 or 4096: sharper shadow edges. Default 4096 |
| Soft shadows (PCSS) | Off (the Switch's), ×1, ×2, ×4. Default ×2 |
| **Developer options** | |
| Shadow pass, G-buffer, bloom, distance blur pass | The game's renderer or ours draws that part of the picture; the same picture. Ours by default |
| Battle blur | The game's or fast (default): the blur as a battle starts with a quarter of the rounds, the same look, ~3 ms less GPU time |

The performance HUD sits in the window's corner (F3 shows or hides it with LEGACY MENUS on): frame time, CPU and GPU cost and memory; with frame generation on,
the big number is the frames on screen with DLSS or FSR beside it, and the game's own FPS goes small. The overlay's Overlay page chooses compact
or detailed, the corner, the parts, the size and how solid its background is.

The game starts at the Switch's 1600×900 and 16:9 with a 4096 shadow map, PCSS ×2, ×16 texture filtering, half
distance blur, half-size effects and no anti-aliasing. For the game's original look, set every setting to its first choice in the
panel.

</details>

## Screenshots

All captured from the recompiled build on Windows.

![Four screenshots: the Amano house kitchen; the opening scene in English; a Yo-kai Pad hint; the opening movie](docs/screenshots/92-screenshots.jpg)

On Ordinary Street with the performance HUD on (F3). The frame takes about 19 ms of the game's own work and 14% of a 16-core laptop's CPU, against the 33 ms the game's 30 frames/s allow; at
3840×2160 with DLSS, frame generation turns the 51 frames a second it renders into 103 on screen.

<p align="center">
  <img src="docs/screenshots/11-hud-in-town.jpg" alt="Riding a bike down snowy Ordinary Street at 4K, with the performance HUD: 103 frames a second with DLSS, frame times, GPU, CPU, memory and a frame-time graph" width="820">
</p>

## Goals

<details>
<summary><b>Planned</b> (2)</summary>

| Goal | What it will be |
|---|---|
| **Built-in cheats** | Cheats switched on from the panel while playing, such as unlimited Crank-a-kai, with no separate cheat tool |
| **Launcher for Linux** | The same launcher on Linux, so the game no longer has to be built from source there |

</details>

<details>
<summary><b>Ongoing</b> (4)</summary>

| Goal | Where it stands |
|---|---|
| **Our own renderer** | Ours: shadows, surfaces, bloom, distance blur, lighting, sky, tone mapping. **Left:** choosing what to draw, two screen passes, the UI and menus |
| **Steadier shadows** | Shadows still shimmer a little as the camera moves; steadier cascades next |
| **Better anti-aliasing** | FXAA, SMAA and TAA (steady thin edges: fences, nets, leaves) on the 3D scene before the UI is drawn, so text stays crisp; per-object motion for TAA next (a first version is in, off by default) |
| **Mouse control for the Yo-kai Pad** | Pointing, clicking and wheel scrolling work on the Yo-kai Pad and the pages opened from it, such as the item and quest lists, Ukiukipedia and Members ([decision 0057](docs/decisions/0057-mouse-menu-lists.md)). On 2026-11-08 some pages still had open issues (Watch skills, Soulmate, Trophies, Treasure photos, Switch moves) |

</details>

<details>
<summary><b>Finished</b> (13)</summary>

| Goal | Where it stands |
|---|---|
| **Performance** | The game's 30 frames/s everywhere tested, and well above it with the FRAME RATE setting (60, 120, 144 or no limit) |
| **Higher frame rates** | 30, 60, 120, 144, VSYNC or no limit, switchable while playing (FRAME RATE in the panel; VSYNC after a restart): the game already moves by the real time between frames, so walking, battles, menus and cutscenes all work (~140 frames/s at 1600×900). At 4K the GPU is the limit; upscaling lifts it, frame generation doubles what is shown |
| **Sharper distance** | Anisotropic texture filtering and a softer distance blur: distant ground, roads and buildings stay sharp without 4K |
| **Better shadows** | Bigger shadow maps (up to 4096), PCSS soft shadows and contact shadows ([decisions 0025](docs/decisions/0025-pcss-soft-shadows.md), [0045](docs/decisions/0045-contact-shadows.md)) |
| **Higher resolution** | The 3D view at 1080p, 1440p or 4K instead of the game's 1600×900 (30 frames/s at 4K on an RTX 4070 laptop) |
| **Ultrawide and other aspect ratios** | 16:10, 21:9 and 32:9 in the panel: a wider view of the world with the UI kept 16:9 in the middle; exploring, battles and cutscenes work; dialog dimming and the loading screen cover the full width |
| **Upscaling (NVIDIA DLSS, AMD FSR)** | The world drawn smaller and rebuilt at full size, menus and text kept full size. **DLSS for the cleanest picture (NVIDIA RTX), FSR for the most frames (any GPU).** At 4K 21:9 on an RTX 4070 laptop, instead of ~81 frames/s with TAA: DLSS ~85 / ~92 / ~82 (quality / balanced / performance), FSR ~94 / ~106 / ~118. DLSS keeps fine patterns (far nets, perforated fences) clean where FSR gets grainy ([decisions 0039](docs/decisions/0039-fsr-upscaling.md), [0040](docs/decisions/0040-dlss-upscaling.md)) |
| **Frame generation (NVIDIA DLSS, AMD FSR)** | A frame made between each two of the game's: about twice as many on screen, the menus kept still. **On RTX 40 and newer use DLSS**: more game frames left and cleaner moving characters. In a busy town at 4K 21:9 on an RTX 4070 laptop, instead of ~70 frames/s: DLSS ~150 shown (~75 game), FSR ~126 shown (~63 game, any GPU). NVIDIA Reflex in the panel too ([decision 0041](docs/decisions/0041-frame-generation.md)) |
| **Keyboard, mouse and controllers** | Bindings on the panel's CONTROLS page, saved, with separate keys for battles and your controller's own button names; tested with a DualSense. Controller rumble and haptics, on by default, with an on/off switch on the overlay's Controls page ([decision 0058](docs/decisions/0058-controller-rumble.md)). On-screen button icons follow what you play with and your bindings (keyboard, Xbox, PlayStation or Nintendo, [decision 0042](docs/decisions/0042-button-icons-and-mouse.md)). The mouse turns the camera through the game's own unused mouse code |
| **Mod support** | Mods in the Switch's layout are laid over your game files at start, as on a modded Switch, without touching your dump: the English translation patch runs (see [Features](#features), [decision 0043](docs/decisions/0043-mods-layeredfs.md)). The YKW Mod Merger™ makes mods that change the same file work together ([0067](docs/decisions/0067-mods-merge-cpk-list.md), [0068](docs/decisions/0068-mods-merge-data-files.md)) |
| **In-game overlay** | Escape or a controller button pauses the game and opens a side panel: settings, controls, the performance HUD's options, screenshots and exit, drawn like its design ([decision 0053](docs/decisions/0053-in-game-overlay.md)); Preview shows graphics changes live, and themes match the launcher's ([decision 0055](docs/decisions/0055-overlay-themes.md)) |
| **Launcher** | A Windows app that takes your game files, checks them, builds the game and starts it, with mods, DLC, saves and screenshots in one place. Installer and portable zip ([decision 0052](docs/decisions/0052-launcher-packaging.md)), tested on a second PC. Released with 1.0.0 (2026-11-13): [download](https://github.com/ILFforever/YW4Recomp/releases/latest) |
| **Linux** | Tester [ouni666](https://github.com/ouni666) plays it on AOSC OS with an AMD RX 6600 XT at about 110-120 frames/s, sound and controls included (2026-11-13). Built from source (the launcher is Windows only); FSR, DLSS, frame generation and Reflex are Windows only |

</details>

## Behind the scenes

- **Translated ahead of time, not interpreted.** Every ARM64 function in the game's `main` and `sdk` modules (73,844
  functions) becomes C before the game runs ([recomp/](recomp/README.md)); Clang compiles it into one native program.
- A native runtime ([runtime/](runtime/)) stands in for the Switch: the kernel (threads, memory, synchronisation),
  system services (filesystem with your RomFS read in place, input, audio, time, saves, applets) and the NVN graphics
  API on top of Vulkan.
- The game's own shaders are translated from the NV_gpu_program5 assembly they ship with into GLSL and SPIR-V
  ([decision 0020](docs/decisions/0020-shaders-from-the-assembly.md)).
- No emulator code is linked ([decision 0005](docs/decisions/0005-phase1-runtime-strategy.md)). Each instruction form
  of the recompiler is checked against a real ARM64 CPU (a Raspberry Pi 5).
- Why things are built this way is written down in [docs/decisions/](docs/decisions/).

## Building from source

Requirements: Python 3.11+, Git, a C compiler (Clang 19 or newer recommended:
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on Windows; GCC 15 or newer also works), a GPU with Vulkan 1.3, and
`glslangValidator` (from the [Vulkan SDK](https://vulkan.lunarg.com/), or a standalone glslang). SDL3 is fetched
automatically on Windows. Full steps in [docs/setup.md](docs/setup.md), and step by step for Windows and Linux on
[the build page](https://ilfforever.github.io/YW4Recomp/building/).

Disk space: an install takes about 11.3 GB, or 9.3 GB after "Free up space". Building from an NSP or XCI peaks near
20.7 GB; from extracted folders, about 11.4 GB ([disk usage](docs/disk-usage.md)).

1. If you will commit: install the guard that keeps game data out of commits, `python -m tools.yw4.guard --install`.
2. Copy `config.example.toml` to `config.local.toml`. Point `exefs_dir` and `romfs_dir` at your extracted ExeFS and
   RomFS; set `[tools] cc` to your Clang (the default is gcc) and `[tools] glslang` if it isn't on your PATH.
3. Check the config: `python tools/yw4/config.py` prints the resolved paths or says what is missing.
4. Verify and decompress the game code: `python -m tools.yw4.ingest`.
5. Recompile and build: `python -m recomp.translate`, `python -m tools.yw4.shaders`, `python -m recomp.shader --check`,
   then `python -m recomp.build --run`.

Runtime options are environment variables, for example `YW4_SHADOW_RES=1024`: in PowerShell
`$env:YW4_SHADOW_RES = "1024"`, in cmd `set YW4_SHADOW_RES=1024`, before starting the game.

Runtime options, capture keys, input replay and the frame-inspection tools are in
[docs/debugging.md](docs/debugging.md). Every run writes its log to `work/logs/`: it names the GPU and driver in use,
and is the file to send with a bug report.

### Building on Linux

The same steps as above, with the tools from your distribution: Clang 19+ (or GCC 15+), Python 3.11+, Git,
pkg-config, glslang and SDL3 with its headers, plus a Vulkan driver (Mesa or NVIDIA's).

```
sudo pacman -S clang python git pkgconf glslang sdl3                        # Arch
sudo dnf install clang python3 git pkgconf glslang SDL3-devel               # Fedora
sudo apt install clang python3 git pkg-config glslang-tools libsdl3-dev     # Debian 13, Ubuntu 25.04+
```

Set `[tools] cc = "clang"` in `config.local.toml`, then build as in steps 1-5. After that, start the game with
`./work/build/yw4` alone: the build writes your game and DLC folders into `work/build/game.ini`. Saves go to
`~/.local/share/YW4Recomp/save`. Ubuntu 24.04 has no SDL3 package: see
[docs/setup.md](docs/setup.md#requirements). AMD FSR, NVIDIA DLSS, frame generation and Reflex are Windows only for now.

## Project layout

| Path | What |
|---|---|
| `recomp/` | AArch64 → C recompiler and the shader translator |
| `runtime/` | Native runtime: kernel, services, NVN on Vulkan |
| `tools/` | Python input pipeline and analysis tools |
| `tests/` | Tests on synthetic data (no game files) |
| `docs/` | Setup, debugging, graphics options, progress, known issues, decision records, recon report, Eden baseline |
| `third_party/musl/` | musl 1.1.19's math functions (MIT) |
| `third_party/smaa/` | SMAA's shader and lookup textures (MIT) |
| `work/`, `build/` | Generated output from your own game files (gitignored, never committed) |

## Credits

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** - the model this project follows: ship the tools, let
  every player recompile their own copy.
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai - a static recompilation of *Link's Awakening*
  for Switch; its approach to the Switch SDK and its README layout inspired this project's.
- **[musl](https://musl.libc.org/)** - the math library (`third_party/musl`, MIT).
- **[SMAA](https://github.com/iryoku/smaa)** by Jorge Jimenez et al. - the SMAA anti-aliasing shader and lookup textures (`third_party/smaa`, MIT).
- **[SDL3](https://www.libsdl.org/)** and the **[Vulkan](https://www.vulkan.org/)** headers.
- **Eden** (a Switch emulator) - used only as a visual and performance reference; none of its code is used.
- **LEVEL-5** for Yo-kai Watch 4. This project is not affiliated with or endorsed by LEVEL-5 or Nintendo.

## Support

YW4 Recomp is free and for the community. If you enjoy it, a coffee is always appreciated.

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-ilfforever-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://buymeacoffee.com/ilfforever)

## License

[MIT](LICENSE), for the code and docs in this repository only: not the game, its files, or anything built from
your copy of it. Third-party code keeps its own license (`third_party/musl`, `third_party/smaa`: MIT).
