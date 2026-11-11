# 0062 — First start: graphics picked for the GPU, two levels
<!-- decision
state: accepted
date: 2026-11-11
replaces: none
code: runtime/src/settings.c, runtime/src/window.c
-->

Status: accepted (2026-11-11). Tested on the owner's RTX 4070 laptop with the level forced; not yet on an
integrated GPU.

## Context

A new install has no `settings.ini`, so every setting starts at its default, and the defaults are the high ones
the owner plays with: a 4096 shadow map, soft shadows x2 (decision 0025), contact shadows (0045) and full-size
effects. On integrated graphics the game crawled. The owner asked for a first-run check.

## Decision

**Two levels, not three.** The owner (2026-11-11): a dedicated GPU with 4-8 GB runs the shadows at their highest
fine; what costs most there is resolution and aspect ratio, which stay as the player sets them. So:

- **Lighter:** an integrated GPU (Vulkan's device type is not "discrete"), or under 4 GB of video memory (the
  largest device-local heap). Shadow map 1024, soft shadows off, contact shadows off, effects resolution half:
  the Switch's values. Nothing else changes.
- **High:** everything else. The defaults stay.

**Once, in the game.** `settings_load` notes that a settings file is in use but doesn't exist yet. Right after
the Vulkan device is made (`vk_init`), the window sorts the GPU and `settings_first_run` steps the four settings
the way the panel would, then writes the file, so it never runs again and never overrides the player's choices.
The game does it, not the launcher, because it knows the GPU it actually draws on, and a double-clicked
`yw4.exe` gets it too. The launcher never makes `settings.ini`, only reads or copies it. A variable set in the
environment is left alone. Test runs (`YW4_SETTINGS_FILE=none`) never trigger it.

**A notice that fades.** A corner toast at the bottom left of the picture (the owner's pick of four mockups,
2026-11-11: the first, one long line at the top, was hard to read): a tile with a gauge, **gold** for lighter
graphics and **green** for high, then a bold title ("Lighter graphics for this PC" / "High graphics for this PC")
over a softer line with the GPU's name and "Change them in Settings", on the theme's deep violet with a soft
shadow. It fades in over 0.4 s, stays 6 s and fades out over 1.2 s. It is drawn with the HUD's blend pipeline
from its own small image, over the game and under the overlay.

## Consequences

- `YW4_FIRST_RUN_LEVEL=low|high` forces the level in tests (with `YW4_SETTINGS_FILE` pointing at a file that
  doesn't exist); `YW4_SHOT_MENU=1` puts the notice into screenshots.
- Existing installs are untouched: they already have a settings file.
- Not done: a "reset to recommended" row (the owner didn't ask for it); checking the levels on a real
  integrated GPU and an AMD card (the clean-PC test).
