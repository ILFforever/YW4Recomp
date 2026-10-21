# 0022 — From the title menu into a new game

Date: 2026-10-22
Status: accepted (small steps, each forced by the next stop of the running game)

## What stopped the game, and what we did

| Stop | Cause | Fix |
|---|---|---|
| `ShowKeyboard` -> `CreateLibraryApplet` (name entry) | The Switch's keyboard is a library applet; there is none here | `nn::swkbd::ShowKeyboard` is a native override (`runtime/src/swkbd.c`). It answers at once with `YW4_PLAYER_NAME` (default "Nate"). The out string is `{char16_t *buffer; size_t size}`, measured from the game's caller. Typing a real name (SDL text input) is a later step |
| Indirect call to `main+0x9a6c78` | A `br` through a switch table that the recompiler did not recognise: the compiler had put ~40 vector instructions between the bound check (`cmp`/`b.hi`) and the table load | `recomp/function.py`: the search for the bound compare reaches 8x further back |
| Indirect call to `main+0xa3e014` | Same, but the table's base register is loaded by an `adrp` laid out *after* the branch | `recomp/function.py`: if the backward scan finds no constant, take an `adrp`/`add` into that register within 32 KiB of the branch whose table has only code addresses within 64 KiB of the branch. A wrong guess still fails loudly for targets outside the table, as before. 3 more tables in `main`, 21 in `sdk` |
| `time:u` 400, 500; `IApplicationFunctions` 50 | Clock snapshots, snapshot difference, pseudo device id | `svc_time.c` (ClockSnapshot, 0xD0 bytes, layout from Switchbrew), `svc_am.c` (fixed id) |
| Vertex input 1 of shader `97a2b1f006a064b9`: `R32_UINT` read "as float" | A plain `MOV.U` copies bits and says nothing about the type, so the translator calls the input a float. 32-bit integers have no scaled format in Vulkan | `gpu_draw.c`: 32-bit integer formats are fetched as the float of the same width (bits unchanged); the shader copies them to a flat varying |

## Result

The game gets through the name entry, the new-game flow and into the opening scene: a 3D room with Whisper, Nate and Jibanyan, and the first line of dialogue (`docs/screenshots/06-opening-scene.jpg`).
