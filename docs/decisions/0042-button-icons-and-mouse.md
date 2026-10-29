# 0042 — Button icons that follow the device, and the mouse

Status: accepted, first version (2026-10-30). Icons checked in game for all four styles; the mouse camera and
switching devices while playing still need the owner's test.

## Context

The game's text and HUD show Switch buttons. On a PC the player uses a keyboard and mouse, or an Xbox,
PlayStation or Nintendo controller, and may rebind every input (CONTROLS page, F2). The icons should show what
the player actually presses.

The game still has the PC build's pieces: five icon sheets in `data/nx/font/font.cpk` (PlayStation, Xbox,
keyboard and mouse, the game's own, Switch), all loaded at start, and a camera path for mouse movement in the
field camera (`f_846260`) that the Switch build never feeds.

## Findings

- **Icons.** Text names an icon by a Switch tag, e.g. `[$gaiji_bt02_s000]`; command tags such as `<CMD_BACK>`
  become such tags through `menu_nx_gaiji_config.cfg.bin`. Each frame `f_a8c3f0(sheet, _, requests, count)`
  draws, for one sheet, the requests whose name it finds there, if the sheet's byte `+0x1c` is set; a request is
  0x38 bytes with the CRC-32 of the icon name at `+0`. The sheets are listed at `[main+0x58654f8]`.
- **Which btNN is which button.** The config file lists its 35 entries in Sony's pad bit order (select, L3, R3,
  start, up, right, down, left, L2, R2, L1, R1, triangle, circle, cross, square): bt09, bt11, bt12, bt10, the four
  D-pad icons, bt06, bt08, bt05, bt07, bt01..bt04. The game's text agrees (`bt05` + D-pad = the L shortcuts), and
  in game the PlayStation sheet's bt06/bt08/bt07 show L2/R2/R1 where the Switch shows ZL/ZR/R.
- **Variants.** `key_all_s100` / `s200` and `lst_`/`rst_` `s100`/`s200` are the left-right and up-down forms
  (text: "switch screens with L/R or `key_all_s100`"); the PlayStation and Xbox sheets name them `key_lr`,
  `key_updwn`, `l_lr`, `l_updwn`, `r_lr`, `r_updwn`. Their sticks are `gaiji_l` / `gaiji_r`, not `lst` / `rst`.
- **Mouse camera.** At `main+0x846bac` the camera reads a movement from `+0xcc`/`+0xd0` of its pad state; if it
  is nonzero and camera input is allowed (`+0x4ba`, `+0x4bb` both 0) it turns by it, scaled by the game's mouse
  camera speed (`main+0x56b9130`), instead of by the right stick.

## Decision

1. **Swap names, not pixels.** A hook on `f_a8c3f0`, before it runs for sheet 0, switches the PlayStation or
   Xbox sheet on and replaces each Switch button's CRC with the icon of what is bound to that Switch input now:
   the first bound key that has an icon, or the bound controller button by its position, in the connected
   controller's sheet (a Nintendo controller keeps the Switch sheet and its style suffix). The game's own name is
   remembered per request address, so a request swapped once can be swapped again when the device changes even if
   the game doesn't rewrite it. Groups (a whole stick, the whole D-pad) keep the Switch icon in keyboard mode: the
   keyboard sheet has no icon for a set of keys. Keyboard icons for punctuation and numpad operators are left out
   until someone looks at the sheet (`gaiji_symbolNN` / `gaiji_num_euNN` are not named by key).
2. **Hooks inside a function.** The recompiler's hooks only ran at a function's start (0024). A `[[hook]]` with
   `at = "<offset>"` now runs the native function just before that instruction, also when a branch lands there;
   its result is ignored, and that function keeps its registers in the CPUState (no 0035 caching) so the hook sees
   them. The mouse hook writes the window's movement since the last frame there, every frame (nothing else clears
   it), and drops it while the game locks the camera.
3. **Mouse as input.** The window takes the mouse (relative mode) at start and on a click; Alt and the settings
   panel give it back. Clicks count only while the game has the mouse, and the click that gives it back is not a
   press. Mouse buttons and the wheel can be bound, but the defaults stay keys only (Enter and Escape stay A and B).

## Rejected

- Copying keyboard pixels over the Switch sheet's cells at upload: works without knowing the draw code, but can't
  follow the bindings or switch between controller makers.
- Mapping mouse movement to the right stick: no game change needed, but a stick is a speed, so the camera would
  drift on after the mouse stops and feel floaty.
- Editing the generated C by hand (what the first draft of this did for the mouse hook): lost at the next
  `recomp.translate`.
