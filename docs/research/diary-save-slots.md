# Diary save slots: can the game's own list show N saves?

Status: partial. Static reading only (no game run; another agent had the game running).

## Verdict

**Feasible but big, not proven yet.** The game's code already builds a save folder name from a number
(`USERDATA%02u`), and our save host accepts any extra save folder. So the file side looks possible.
Not yet known: how many rows the Diary list shows, how the list is filled, and whether the game ever
uses an index above 0. Those need a traced run, which must wait until the game is free.

Rough size, a guess: 2 to 4 weeks of work for option 2, if the list code is as simple as it looks.

## What is proven (static)

- The save strings are in the flat main image (`work/exefs/main.bin`, file offset = `main+offset`):
  - `AUTOSAVE` at `main+0x1299ba4`
  - `USERDATA%02u` at `main+0x1299bad`
  - `HEADERSAVE` at `main+0x1299bba`
  - `USERDATA` at `main+0x1299bc4`
- 17 ARM64 `ADRP`+`ADD` sites load these strings. They sit between `main+0x8a5cc0` and `main+0x8a7e8c`.
- `f_8a5ae0` (`main+0x8a5ae0`, `work/recomp/main_144.c` line 4436) is the function that uses the
  name strings. It takes the slot index as its third argument (`x2`, a 16-bit value). It formats
  `USERDATA%02u` with that index. So the file name is built from data, not fixed at 0.
- Its callers are `f_8a5420` (`main+0x8a5420`, mode 1, index from the record at `+4`) and `f_8a57b0`
  (`main+0x8a57b0`, mode 2, index from the record at `+2`). The record pointer comes from a table in the
  caller. Each record gives a `u8` and a `u16`. Not yet traced: where that table comes from.
- The Diary's row names are built in `f_6dd6c0` (`main+0x6dd6c0`). It formats `SaveResMenu_%u` at
  `main+0x6e9d8c` and writes a `SAVE_RES_INFO` record (fields at +0x158 to +0x1ad). One such record per row.
- `LastSaveSlot` is a string at `main+0x1213673`, used at `main+0x30c2b4` (in `f_30bac0`). Not traced.
- The save host (`runtime/src/savefs.c`) takes any Switch path under the user's folder, up to 0x300 bytes.
  It can create files and folders (CreateFile, CreateDirectory), so a `USERDATA01` folder needs no host change.
  `Commit` only stamps the save time. Writes go straight to the file, with no journal.
- On disk, the test save (`work/testsaves/save_base_lategame`) has one user folder,
  `595734205245434f4d50205553455201` (ASCII `YW4 RECOMP USER` plus byte 0x01, chosen by our runtime). Inside:
  `USERDATA00/data.bin` (1,046,707 bytes), `AUTOSAVE/data.bin` (about 1 MB), `HEADERSAVE/data.bin` (1,118 bytes),
  `SYSTEM/data.bin` (428 bytes). No `USERDATA01` exists.
- `HEADERSAVE` holds a run of 32-byte records (25 of them in one block at `0x24`..`0x1c0`, then more after
  `0x23b`). Each starts with the same 6-byte prefix `c8 a7 74 17 0c 00`, which looks like a timestamp field.
  Strings `Summer`, `texture`, `Normal` appear near `0x1a0` and `0x3c0`. The meaning is NOT decoded.

## Unknown (not traced yet)

1. How many rows the Diary list has. The count comes from `[x1]` in `f_8a5ae0` (a `u32`). Where that
   count is written is not traced.
2. What `x4` (mode 1 or 2) does inside `f_8a5ae0`.
3. Whether the game ever creates or opens an index above 0 in a real play. The code path exists (the loop in
   `f_8a5ae0`), but no run has shown it.
4. The exact `HEADERSAVE` layout (header size, record size per slot, which fields are name, date, location,
   playtime, members, rank).
5. Checksums: whether the game checks a checksum on load (in `SYSTEM` or in the header). Not checked.
6. Whether the game expects a fixed number of slots in `SYSTEM`.

## What would settle it (needs the game free)

- One run from a save copy with a log line in the save host's `CreateFile` / `OpenFile` (or a
  `YW4_WATCH` on the `USERDATA%02u` format call) to see every index the game asks for.
- A watch on the `[x1]` count in `f_8a5ae0` and on the `SAVE_RES_INFO` writes in `f_6dd6c0`, to get the
  row count.
- Open the Diary on a save and read the `YW4_MOUSE_LOG=1` line for its list (item count at `+0x128`).

## Files and tools used

- `work/exefs/main.bin`, `work/recomp/main_144.c`, `work/recomp/main_123.c`, `work/recomp/funcs.h`
- `runtime/src/savefs.c`, `runtime/src/cheats.c`, `docs/decisions/0074-cheats.md` (AUTOSAVE byte at 0x822)
- Scratch scripts (not in the repo): `C:\Users\paeki\AppData\Local\Temp\claude\9519414f725d5b3e\20d99e8c-eb0a-4c0c-8989-0a45905ec844\scratchpad\diaryresearch\xref.py` (finds `ADRP`+`ADD` references to an address) and `fn.py` (maps an address to the nearest recomp function name; approximate, since `funcs.h` may not list every function).

## Session 2 (owner's questions)

### Q2. HEADERSAVE layout (decoded on the late-game save copy, static)

- File: 1,118 bytes. A short start block, then two records of 0x217 bytes each: record 1 at 0x30 (the slot)
  and record 2 at 0x247 (the Autosave). The start of each record is the same length.
- Inside each record (offsets from the record start):
  - +0x184: recorder name, NUL-terminated. Both records: `Summer`.
  - +0x1d4: date, 8 bytes: year u16 LE, month u8, day u8, hour u8, minute u8, second u8, pad u8.
    Record 1: `e8 07 09 13 0a 07 22 00` = 2024-09-19 10:07:34 (matches the slot's 09/19/2024 10:07).
    Record 2: `ea 07 01 15 10 37 0c 00` = 2026-01-21 16:55:12 (matches Autosave 01/21/2026 16:55).
  - +0x1e4: playtime, u32 seconds. Record 1: 131287 (036:28:07). Record 2: 131723 (036:35:23, the owner's value).
- Not yet located: location (Mystic Grove, probably a numeric map id) and watch rank B. Members not located.
- Checksum: none found. I tried CRC32 over the header and over both record ranges, and looked for the result
  stored anywhere in the file. No match.
- Also in the start block: a run of 32-byte rows (about 25 of them, each starting with the prefix `c8 a7 74 17 0c 00`).
  Not decoded. They may be the Diary's member or list data.

### Q4. Index passed to f_8a5ae0 (run 1, late-game copy, title then Continue, replay shots_title.txt)

- Probe `main+0x8a5ae0` hit 3 times: frame 1 (x3=1), frame 6 (x3=3), frame 2224 (x3=2, the Continue load).
- x2 (the slot index) was 0 on all three calls. So only index 0 (`USERDATA00`) was used in this run.
- Caller: `main+0x8a5a68` (inside `f_8a57b0`).

### Q1. Row count (partial)

- The u32 read by `f_8a5ae0` at `[x1]` (x1 = the list state at +0x660) was **0 on entry** in all three calls.
  So this field is not the row count the function uses; it is probably filled by the function itself.
- Diary list control item count from `YW4_MOUSE_LOG`: not yet found. The menu log lines show `lv=0` for the
  title's menus, so no list view was seen. Run 2 is set up to count row builds instead.

### Screenshots

- Run 1 saved frames at ticks 100, 1480, 1700, 1900, 2000, 2100, 2150, 2200, 2300 under
  `work/shots_diaryresearch/`. Not inspected (binary BMP; the tool here can't show it).

### Run 2 (log work/logs/yw4_20261116_103845.log)

- Probes on `main+0x6dd6c0` (33 hits), `main+0x8a5420` (0 hits), `main+0x8a5ae0` (3 hits, the same as run 1).
- The `SaveResMenu_%u` path (`main+0x6e9d8c`, inside `f_6dd6c0`) was NOT reached. None of the 33 hits came from
  `main+0x6e9xxx`. So the replay (title to Continue, load) did not build the save rows, or those rows are built
  only on another screen (the Diary). Q1 (row count) is still unanswered.
- `f_8a5420` was never called in this replay (0 hits), so `f_8a57b0` (via `main+0x8a5a68`) is the only caller seen.

### Q3. New Game (NOT tested)

- Not run. The title's menu layout and the button presses for New Game were not mapped, and the replay file only
  has A presses for Continue. Guessing the presses would cost a run with no proof. Needed: one screenshot of the
  title menu (the BMPs are in work/shots_diaryresearch/) and a replay that picks New Game, then compare the
  copy's USERDATA00, HEADERSAVE and AUTOSAVE hashes before and after, plus the warning text.

### Verdict (updated)

**Feasible but big, not proven.** Still open: the Diary row count (Q1), Q3 (New Game), and the header row layout
(only the slot and Autosave records are decoded). Index 0 only in the runs seen (Q4), so the index path (`USERDATA%02u`)
is never exercised in play here.

**Work estimate:** 3 to 6 weeks, a guess. The file side is cheap (savefs.c takes any extra save folder). The
expensive parts: the list's row count and loading, the header's row format, and the New Game and delete logic,
which need traces.

### Budget used

- Game runs: 2 of 3 (run 1 late-game title and Continue; run 2 row-builder probes). Both on the late-game save copy.
- Scratch files: `C:\Users\paeki\AppData\Local\Temp\claude\9519414f725d5b3e\20d99e8c-eb0a-4c0c-8989-0a45905ec844\scratchpad\diaryresearch\` (replay `shots_title.txt`, `xref.py`, `fn.py`).

## Session 3: New Game and the Diary (owner's questions)

### Q1. New Game (answered, partly)

- **Warning:** title > New Game shows the prompt "Are you sure you want to erase the current saved data?"
  with **Yes** and **No**. **No** has the focus by default. Screenshot: `work/shots_newgame2/frame_001991.bmp`
  (PNG copy in the scratchpad `diaryresearch` folder, `ng2_frame_001991.png`).
- **Run (log work/logs/yw4_20261116_110318.log, save copy work/testsaves/save_run):** the A press at tick 1990 took
  the default **No**. The screen went back to the title (New Game focused, frame 2151). The game did not write a save.
- **Files:** USERDATA00, HEADERSAVE, AUTOSAVE and SYSTEM hashed the same before and after the run (sha256 prefixes
  692cf44e, badf054a, 34467066, bccdf1f7). So nothing was erased, because the prompt was declined.
- **Not tested:** what Yes does. The run never chose Yes, so when the files are erased (at once on Yes, or at the
  first save) is still unknown. That needs one more run that presses Left then A on the prompt.

### Q2. Diary row count (partly answered)

- From the frame at tick 2150 of run 1 (`frame_002150.bmp`): the Diary list shows **two rows**: "Summer 09/19/20…"
  (the slot) and "Autosave". The list has room for two rows and no scroll bar is visible, so the list does not scroll
  with two saves. The screen also shows "Watch Rank B", the diary panel, and a load prompt ("Would you like to load
  this diary's save data?", Yes / No).
- Item count and list-control hash: NOT measured. The mouse log shows no list-view line for the Diary (`lv=0`), so
  the Diary list is not a `lives::CMenuListView` in those logs.

### Verdict (updated)

**Feasible but big, not proven.** Known now: the save list is a two-row list in practice (slot + Autosave), with the
row data in HEADERSAVE (two 0x217-byte records). Still open: the row count the game code uses (Q2 item count), what
New Game does on Yes, and the full HEADERSAVE field map (location, rank). Estimate: 3 to 6 weeks (a guess), unchanged.

### Budget and locks

- Game runs this round: 2 (the first New Game attempt, which pressed Up and went to Languages; and the corrected
  New Game run above). The lock file (scratchpad `game.lock`) was taken and released by my waiter around the corrected
  run. A lock written at 11:06 by another agent is still present, and I did not touch it.


---

## Round 4 Update (2026-11-16): Complete Findings, PoC Results & Implementation Plan

### 1. New Game -> Yes: Timing, File Hashes, and Functions
Tested in-game on a copy of `work/testsaves/save_base_lategame` via `python -m tools.dev.run` using replay `replay_newgame_yes.txt` (D-Pad Up `0x2000` on Title, where item order top-to-bottom is `New Game`, `Continue` [default], `Languages`, `DLC`; then `A` -> D-Pad Left `0x1000` on `"Are you sure you want to erase the current saved data?"` -> `A` -> `"Choose a Name! Welcome to the world of YO-KAI WATCH!"`, screenshots `frame_001925.bmp`, `frame_002040.bmp`, `frame_002500.bmp`, `frame_002850.bmp`; run log `work/logs/yw4_20261116_130928.log`):

- **Zero disk writes occur when confirming Yes.** All 4 save files in `work/testsaves/save_run/010086c00af7c000/595734205245434f4d50205553455201/` had identical SHA-256 hashes before starting New Game and right after reaching `"Choose a Name!"`, and `yw4_20261116_130928.log` recorded zero `Commit` lines:
  - `USERDATA00/data.bin` (1,046,707 B): `692cf44eb192d9e5...` (unchanged)
  - `HEADERSAVE/data.bin` (1,118 B): `badf054a956afc13...` (unchanged)
  - `AUTOSAVE/data.bin` (1,046,707 B): `34467066b1f2e031...` (unchanged)
  - `SYSTEM/data.bin` (428 B): `bccdf1f7147f0f30...` (unchanged)
- **Code path (`work/recomp/main_140.c`, `work/recomp/main_054.c`):**
  - `f_884f50` (`L_885088`) -> `f_32e540` only clears in-memory save structs (`save_mgr + 0xf = 0`, zeroes `mgr + 0x5758..0x5800`).
  - Disk files are overwritten later when the player actually saves (`f_32f470` -> `f_8a7b10` for `USERDATA%02u` / `AUTOSAVE`, and `f_32ec30` -> `f_8a7b10` for `HEADERSAVE`). *(First-save after the intro cutscene was not played through in this test; verified statically and on in-game Diary save.)*

### 2. Diary Row Count & Correction to Earlier `f_6dd6c0` Lead
- **Correction:** `f_6dd6c0` (`L_6e9d8c`, `SaveResMenu_%u` at `0x1227c20` in `work/recomp/main_123.c`) is an unrelated save-result resource binder, **not** the Diary menu's row builder.
- **Actual Diary menu (`work/recomp/main_128.c`):** Class `0x14d87f8` (`0xdd015806`, `f_761f70`) is not a dynamic `CMenuListView`. It hardcodes two `0x288`-byte row structs at `menu + 0x210` (Row 0, manual save) and `menu + 0x498` (Row 1, Autosave).
- **Fixed 2-row population (`f_761aa0`):** Loops exactly twice over the two `0xa0`-byte records in `headersave_obj` (`x22 = 0; while (x22 != 0x510) { ... x22 += 0x288; x24 += 0xa0; }`), where `headersave_obj = *(u64*)(*(u64*)(yw4_image_base + 0x1548fd8) + 0x57f8)`. Both the Title Diary load screen and the in-game Diary Save screen call `f_761aa0` (verified at runtime).
- **Selection & load routing:** `f_75fb40` toggles `*(u8*)(menu + 0x720)` between Row 0 and Row 1; `f_75e9b0` selects whichever row has the newer timestamp on open (`Latest!` badge). `f_266fc0` (`work/recomp/main_034.c`) maps arg `0 -> USERDATA, slot=0` and `1 -> AUTOSAVE, slot=0`.
- **Underlying engine already multi-slot (`work/recomp/main_144.c`):** `f_8a7dc0(req, flags)` (load) and `f_8a7b10(req, flags)` (save) call `f_8a5fc0` / `f_8a57b0` -> `f_8a5ae0`, which formats `USERDATA%02u` for indices `0..31` (`USERDATA00`..`USERDATA31`).

### 3. `HEADERSAVE` Layout (In-Memory & On-Disk TLV, Incl. Location and Watch Rank)
Decoded from `work/recomp/main_056.c` (TLV serializer lines 29078-29265), `f_32ec30` (`main_054.c`), and `f_761aa0` (`main_128.c`); verified byte-for-byte against `work/testsaves/save_base_lategame/.../HEADERSAVE/data.bin` (`1118` B, Record 0 @ `0x30`, Record 1 @ `0x247`, each `0x217` B) and verified live at runtime in `yw4_hook_diary_populate`:

| Field | On-Disk Tag | On-Disk Record Offset | Size | In-Memory Offset (`f_761aa0` `hdr + i*0xa0 + off`) | Verified Values (`save_base_lategame` Rec 0 / Rec 1) |
|---|---|---|---|---|---|
| Party 12 members | `07f65d6b` + `c8a77417` | `+0x000..+0x17b` | 12 x 32 B | `+0x0c..+0x6b` (writer base) | Rec 0 Lv: 38,35,39,39,37,37,37,34,36,41,27,22 |
| Player Name | `e1895bce` | `+0x17c` (`+0x184` val) | `0x24` B | `+0x6c..+0x8f` (`x24+0x14`) | `"Summer"` / `"Summer"` |
| Raw Map ID | `12cde120` | `+0x1a8` (`+0x1b0` val) | 4 B | `+0x90` (`x24+0x38`) | `0xf7210003` / `0x78c01d2a` |
| Location Text ID | `7da9d6eb` | `+0x1b4` (`+0x1bc` val) | 4 B | `+0x94` (`x24+0x3c`) | `0x15ff9d29` ("North Stream") / `0x133b1216` ("Mystic Grove") |
| Chapter / Progress ID | `64c490b5` | `+0x1c0` (`+0x1c8` val) | 4 B | `+0x98` (`x24+0x40`) | `80150` / `80160` |
| Calendar Timestamp | `cdf07708` | `+0x1cc` (`+0x1d4` val) | 8 B | `+0x9c` (`x24+0x44`) | `2024-09-19 10:07:34` / `2026-01-21 16:55:12` |
| Playtime (seconds) | `94669164` | `+0x1dc` (`+0x1e4` val) | 4 B | `+0xa4` (`x24+0x4c`) | `131287` (`036:28:07`) / `131723` (`036:35:23`) |
| Watch Rank | `10378058` | `+0x1e8` (`+0x1f0` val) | 1 B | `+0xa8` (`x24+0x50`) | `0x03` ("Watch Rank B") in both |
| Valid Flag | `394db4ec` | `+0x1f1` (`+0x1f9` val) | 1 B | `+0xa9` (`x24+0x51`) | `1` / `1` |
| Clear Flag | `2d0c880a` | `+0x1fa` (`+0x202` val) | 1 B | `+0xaa` (`x24+0x52`) | `0` / `0` |

Note: in `f_761aa0`, `x24` starts at `hdr + 0x58` for row 0 and advances by `+0xa0` (`Rec + 0x6c == x24 + 0x14`, etc.).

### 4. Proof of Concept (`YW4_SAVE_SLOT_TEST=1`)
Added opt-in hooks in `recomp/overrides.toml` and `runtime/src/savefs.c`, gated strictly behind `YW4_SAVE_SLOT_TEST=1` (OFF by default; returns `0` immediately with zero side effects and zero logging when unset or `"0"`):

- `yw4_hook_save_load` at `0x8a7dc0`: when `req[0] == 0` (`USERDATA`) and `u16(req + 2) == 0`, rewrites slot to `1` (`USERDATA01`).
- `yw4_hook_save_write` at `0x8a7b10`: when `req[0] == 0` (`USERDATA`) and `u16(req + 4) == 0`, rewrites slot to `1` (`USERDATA01`).
- `yw4_hook_diary_populate` at `0x761aa0`: logs the decoded `HEADERSAVE` fields above from `headersave_obj` for rows 0 and 1.
- `save_fs` in `runtime/src/savefs.c`: logs `CreateFile`, `CreateDirectory`, and `OpenFile` paths when `YW4_SAVE_SLOT_TEST=1`.

**Verified in-game on a temporary save copy (`USERDATA00` renamed to `USERDATA01`; no `USERDATA00` on disk):**
- **Load (`work/logs/dev_run_20261116_132825.log`):** `yw4_hook_diary_populate` printed exact `row=0` (`North Stream`, `rank=3`, `time=131287s`) and `row=1` (`Mystic Grove`, `rank=3`, `time=131723s`) metadata matching `HEADERSAVE/data.bin`. Selecting Row 0 (`Summer`) opened `/USERDATA01/data.bin` via `f_8a7dc0` slot `0 -> 1` and loaded cleanly into 3D gameplay in `North Stream` (`frame_003050.bmp`).
- **Save (`work/logs/dev_run_20261116_133359.log`):** Opening the in-game Diary Save screen and confirming `"Yes"` redirected `f_8a7b10` slot `0 -> 1`, opened `/USERDATA01/data.bin (mode=6)`, committed both `/USERDATA01/data.bin` and `/HEADERSAVE/data.bin`, updated playtime (`131287s -> 131301s`), and displayed `"Finished saving."` (`11/16/2026 13:35`). Only `AUTOSAVE`, `HEADERSAVE`, `SYSTEM`, `USERDATA01` existed in `save_run`; `USERDATA00` was never read or created.
- **Control (`work/logs/dev_run_20261116_134103.log`):** Running with `YW4_SAVE_SLOT_TEST` unset produced zero `save_slot_test` log lines and loaded `USERDATA00` normally.

### 5. Step-by-Step Implementation Plan for the Real Feature, Risks & Honest Estimate

Because the underlying save engine (`f_8a5ae0`) already supports `USERDATA00`..`USERDATA31` and `savefs.c` works on any `USERDATAxx` folder, **no Switch save format breakage is required**. Everything missing is slot management above `f_8a5ae0` and the Diary UI.

#### Recommended Architecture vs Fallback
- **Path 1 (Native Diary extension / paging — matches goal):** Keep `USERDATA00..USERDATA<N-1>` on disk via `f_8a5ae0` + a host-side slot-summary cache (or reading/maintaining `0xa0`-byte `headersave_obj` records per slot while keeping `HEADERSAVE/data.bin` Record 0 synced to the active slot). Update Diary (`0xdd015806`) either by **(1a)** hooking `f_761aa0` / `f_75fb40` / `f_760750` to page slots through the native Row 0 widget + optional `ui.c` slot-list overlay so the existing notebook preview shows any chosen slot cleanly, or **(1b)** modifying RomFS/menu layout nodes so 3+ native game rows display simultaneously.
- **Path 2 (Lower-effort fallback — profile switcher):** Keep the vanilla 2-row Diary unchanged and switch active `USERDATA` sets via the host overlay before Continue / New Game. Only consider if native Diary integration is explicitly ruled out by design decisions.

#### Implementation Steps (Real Feature)
1. **Collect owner design decisions** (see Open Questions below — no design choices assumed here).
2. **Host slot index & summary persistence (`runtime/src/savefs.c` + hook):** Scan `USERDATA00..31` on startup/Diary open and maintain each slot's `0xa0`-byte `headersave_obj` summary record on save commit (`f_8a7b10` -> `HEADERSAVE`), keeping Switch `HEADERSAVE/data.bin` Record 0 compatible.
3. **Slot routing (`f_266fc0`, `f_8a7dc0`, `f_8a7b10`):** Maintain `active_slot` (`0..N-1`) and rewrite load/write requests cleanly per selected Diary slot (and per-slot `AUTOSAVE` if chosen by owner).
4. **Non-destructive New Game (`f_884f50` / `L_885088`):** When starting New Game, select the first unused `USERDATA%02u` slot (or user-selected target slot) before `f_32e540` instead of clobbering `USERDATA00`.
5. **Title Diary UI & In-Game Save UI (`0xdd015806`, `main_128.c` / `ui.c`):** Expose the slot list on Title -> Continue (replacing/extending the 2-row limit in `f_761f70` / `f_761aa0`) AND add slot selection on the in-game Diary Save screen (which currently displays no slot list at all, only the current slot card + Whisper's autosave bubble).
6. **Delete Save:** Add a dedicated, confirmed Delete Save flow (removes `USERDATA%02u` and clears its summary entry without affecting other slots).
7. **Tests:** Unit tests for slot scanning/indexing plus `tools.dev.run` replay tests covering multi-slot load, save to slot 1 leaving slot 0 hash-identical, New Game preserving slot 0, and delete.

#### Key Technical Risks
- **Hardcoded 2-row Diary layout (`menu + 0x210` and `menu + 0x498`):** Unlike `CMenuListView`, Diary's native left-hand list nodes come from static menu layout resources. Adding more *simultaneously visible* native game rows requires reverse-engineering RomFS menu layout / node cloning (`Path 1b`, higher risk); feeding selected slots into the native notebook card + a matched `ui.c` list (`Path 1a`) avoids RomFS layout edits.
- **In-game Diary Save screen has no row list:** `frame_003180.bmp` shows only a single save card + Whisper's autosave bubble, so picking another slot while saving in-game needs new UI/prompt plumbing.
- **Global `AUTOSAVE` collision:** Vanilla has one `AUTOSAVE/data.bin`. Without per-slot autosave separation, playing a New Game in Slot 1 will overwrite Slot 0's `AUTOSAVE`.
- **Untested paths in this PoC:** First manual/auto save right after `New Game` cutscene (`SYSTEM` / initial file creation path), `HEADERSAVE` Record 0 empty while `USERDATA01` exists, and callers of `f_266fc0` outside Continue.

#### Honest Effort Estimate
- **Slot backend + non-destructive New Game + routing + automated tests:** ~1-2 focused sessions (low risk; engine already supports `USERDATA00..31`).
- **Diary & in-game Save UI (`Path 1a`: native notebook preview + `ui.c` / paged Row 0 selector + Delete):** ~1-2 additional focused sessions (low-medium risk).
- **RomFS native multi-row widget expansion (`Path 1b`):** ~3-5+ sessions (uncertain; menu asset format for `0xdd015806` not yet reverse-engineered).

#### Open Design Questions for the Owner
1. **Slot count:** How many manual slots (e.g., 3, 5, 10, up to engine max 32)?
2. **Autosave scoping:** One shared global `AUTOSAVE` (vanilla), or per-slot autosave alongside each manual slot?
3. **UI style (`1a` vs `1b`):** Keep the native notebook card and pick/scroll slots via paging or a matched overlay panel (`1a`), or reverse-engineer RomFS widgets to stack >2 native game rows at once (`1b`)?
4. **In-game Save behavior:** Always overwrite the currently loaded slot, or ask which slot to save to?
5. **New Game flow:** Automatically pick the first empty slot (and change/skip *"erase the current saved data?"*), or ask the player which slot (including overwrite)?
6. **Delete Save:** Where should Delete Save live (Diary action button vs overlay menu), and what confirmation UX?

---

## Round 5 Audit & Fixes (2026-11-16): Static Verification of 10-Slot Implementation

Audited the 10-slot Diary implementation (`runtime/src/savefs.c`, `runtime/src/savefs.h`, `runtime/src/svc_hid.c`, `recomp/overrides.toml`) directly against the generated C (`work/recomp/main_034.c`, `main_054.c`, `main_056.c`, `main_121.c`, `main_125.c`, `main_128.c`, `main_140.c`, `main_144.c`) and existing on-disk test saves:

1. **`HEADERSAVE` prefix & cross-slot record pollution (`f_32f470` in `main_054.c:15028-15101`, `f_32ec30` at `main_054.c:14259`, `f_268090` at `main_034.c:1258`, `f_727820` at `main_125.c:16390`):**
   - `f_32ec30(is_auto)` updates only Row `is_auto` (`0` for manual save, `1` for autosave) of `headersave_obj` (`hdr + 0x0c + is_auto * 0xa0`), and `headersave_obj->vtable->Serialize()` then writes **both** Row 0 and Row 1 into `HEADERSAVE/data.bin`.
   - Because `write_slot_to_hdr_locked` writes `"<n>/10 <Name>"` (or `"<n>/10 Empty Diary"`, `"<n>/10 DELETE? (Press X)"`) into `hdr + 0x0c + i * 0xa0 + 0x60` for UI display, the non-updated row (`1 - is_auto`) kept its `"<n>/10 "` prefix (and any other slot's record if the user had paged slots in the Diary) when serialized to disk (proven in `work/save_manual_20261116_231239/.../HEADERSAVE/data.bin`, where Record 1 had `1/10 Summer` on disk).
   - **Fix:** Added `restore_clean_hdr_locked` in `yw4_hook_save_write` to restore the target slot's unprefixed records into `headersave_obj` before `f_32ec30` and `Serialize()` run (handling both normal saves `req[2] == 0` and combined saves `req[2] != 0` in `f_268090`/`f_727820`), and added `strip_slot_prefix_inplace` in `parse_slot_from_disk_locked` to self-heal any already-prefixed `HEADERSAVE/data.bin`.
2. **Modal / busy guard (`is_general_win_active`):**
   - `*(uint64_t *)(mgr + 0x5800)` is the `SYSTEM/data.bin` save object (`main_054.c:13803`), and `+0x3b` is a `SYSTEM` save flag (`f_268010`/`f_268044` in `main_034.c:1213-1256`), **not** `CMenuGeneralWindow`.
   - **Fix:** Replaced with the live `CMenuGeneralWindow` instance count at `*(uint32_t *)(yw4_image_base + 0x5511a84)` (incremented in `CMenuGeneralWindow` ctor `main_121.c:2789`, decremented in dtors `f_6bdab0`/`f_6bf4c0`) plus the active save/load job IDs at `save_mgr + 0x18`, `+0x30`, `+0x48` (`*(uint64_t *)(yw4_image_base + 0x154b2b8)`, `f_267eb0` in `main_034.c:1100`).
3. **Title screen Continue when Slot 1 (`slot 0`) is empty (`f_884f50` at `main_140.c:8621-8728`):**
   - The Title screen checks `headersave_obj` (`hdr[0xa9] != 0 || hdr[0x149] != 0`) before Continue is clicked.
   - **Fix:** In `savefs_open` and `yw4_hook_save_load` (`req[0] == 3` on Title), if `active_slot` has no data, route `active_slot` and `diary_slot` to `find_first_used_slot_locked()`.
4. **All 10 slots full on New Game & delete-arm cancellation:**
   - `find_first_empty_slot_locked` now returns `-1` when all 10 slots are occupied so `yw4_hook_save_clear` keeps `diary_slot` and logs a warning instead of silently overwriting Slot 1.
   - `delete_confirm_slot` now disarms immediately on any non-`X` button press edge in `yw4_hook_diary_update`.

## Review (2026-11-17)

The design is now decision 0078 (`docs/decisions/0078-diary-save-slots.md`; first written as 0077, renumbered
because 0077 is taken on the `discord-presence` branch). Corrections to Round 5 above:

- The Diary open hook is at `main+0x75e9b0` (`f_75e9b0`); no function starts at `0x75e5f0`.
- "All 10 full" did not keep anything safe: New Game overwrote the last slot looked at. New Game now asks for the
  slot (overlay list, then an overwrite question for an occupied slot).
- Slot folders are `SLOT02..SLOT10` (they were `SLOT01..SLOT09`, one less than the slot shown).
- Files and folders are made on demand only in a slot's own folder, not for every path.
- Round 4's "`f_884f50` -> `f_32e540` on New Game > Yes" is wrong in play: three runs from title > New Game > Yes
  through both names into the intro never called `f_32e540` (`dev_run_20261117_023036`, `_023231`, `_023319`). The
  slot is now picked before the boy's name popup (swkbd.c -> `savefs_new_game_naming`).

## Stacked rows (2026-11-17): five save rows and the Autosave row, no paging

Owner, 2026-11-17: "I want the save slots to stack and not having to use right and left to cycle it's confusing."
Chosen: real game rows (not an overlay list). Status: works in test runs from a mod folder; not yet built into the
runtime (see "Still to do").

### How the Diary's layout is made (measured)

- The menu is data: `data/common/gamedata/menu/cfg/save_load_menu_setting.cfg.bin` (Level-5 cfg.bin, in
  `gamedata.cpk`). `MENU_LAYER_INFO` lines name each layer, its object file and **how many copies** it has:
  `diary_load_slot` (one save row, crc32 `0xd14f93d6`) has **2**, `diary_member_icon` 12. `MENU_FOCUS_GROUP` and
  `MENU_CMD_INFO` give the rows their Up / Down (`CMD_FCS_NEXT` / `CMD_FCS_BACK`).
- Each layer's object (`data/common/gamedata/menu/obj/*.objbin`, same format) names its model and its
  `CMenuAttachLocator` lines: `PROP_PARAM 'GlobalLayerName', id, bone, child layer, child copy, child root bone`.
  `diary_load_locator.objbin` puts row copy 0 on bone `_pos_load_bar_locate00` and copy 1 on `_locate01`.
- Models are G4PK packages (`data/common/menu/104_save/104_save00/save00_0N/*.g4pkm`, in `menu.cpk`): G4SK
  skeleton, G4MD mesh, G4MT materials, G4RA animation. `save00_06` is the locator model: `_pos_load_bar00` at
  (96, -440), its two row bones at (0, 0) and (0, -192) under it (1920 x 1080 units, y down negative).
  `save00_05` is one row (plate, cursor bone, pencil / AUTO icons, Latest!, name and date texts).
- G4PK: 0x40 header (+0xc size - 0x40, +0x20 sub-file count), u32 offsets (/4, from 0x40) then u32 sizes, sub-files
  0x20-aligned. G4SK v104: +0x20 u16 bone count, +0x24 11 u16 section offsets (/4, from 0x40): world 3x4 matrices,
  inverse matrices, local scale / rotation / position (0x30 each, sections 0x20-aligned), name crc32s, parents (u16,
  root = count), indices sorted by crc, u8 flags, u32 each, name offsets (u16, table padded to 4) + names. A
  writer of this (`work/diary_rows_tools/g4.py`) rebuilds all 539 menu packages and 281 of 282 v104 skeletons
  byte for byte. Bones added to the skeleton alone work: the mesh's own bone list need not change (run 1 below).
- The animation (G4RA) moves `_locate00` / `_locate01` (the slide-in); the new bones are not in it, so the rows
  don't slide in yet.

### What the code does with the rows (main_128.c)

- `f_75ee60(menu, &layer hash, copy)`: the per-copy setup. For the row layer it reads the row's data at
  `menu + 0x210 + copy * 0x288` (two rows of data only) and shows parts with `f_9eaf10(copy obj, part crc, on)`:
  copy 0 the save look (portrait, pencil), copy 1 the Autosave look (AUTO); "Latest!" where copy == `menu + 0x748`;
  date from row + 0x480..0x485. For copy 0 it also calls `f_9e9340(copy0, id, 2, 1)`: the **focus grid** (rows at
  copy0 + 0x138, columns + 0x13a, the focused index + 0x13c), 2 x 1.
- `f_9f3e90(menu, layer hash, copy)`: a copy's object.
- `f_75fb40(menu, &layer hash)`: called when the rows' focus moved; moves the cursor with
  `f_70c920(menu id, layer, 0xb1f061c1, copy0 + 0x13c, n = 2)` (the attach line turned off on copies 0..n-1, then on
  for that copy), flips `menu + 0x720` (0 / 1) and redraws the card (`f_75fbf0`, `f_760750`) once the menu has opened
  (`menu + 0x750`).

### The change (tested 2026-11-17, `dev_run_20261117_051355` .. `_052822`)

- Data (built from the owner's own files by `work/diary_rows_tools/build_mod.py` into
  `work/mods_diarytest/.../zz_diary_rows/romfs`, with `cpk_list` freeing the 3 files): row copies 2 -> 6; six new
  bones `_pos_load_bar_locate10..15` under `_pos_load_bar00` (saves 100 apart from y -200, the Autosave 128 below the
  last save: owner, "lower the spacing between each save, keep the spacing on autosave the same"); six locator lines.
- Code (`savefs.c`, hooks in `overrides.toml`): the game's setup is skipped for copies 2+ (`yw4_hook_diary_setup`);
  `diary_paint_rows` draws every copy with the game's own row code, `f_9f3e90`'s lookup of row j sent to copy k
  (`yw4_hook_menu_layer`), the row-0-only focus calls skipped in the pass (`yw4_hook_menu_row_focus`); the focus grid
  set to 6 rows; the focus handler maps copy -> slot / Autosave and pre-sets `+0x720` so the game's flip lands right
  (`yw4_hook_diary_focus`); the cursor's attach line moved over all 6 copies (`yw4_hook_menu_attach`). Left / Right
  paging is off when the rows are stacked; the mouse wheel moves up and down. Without the 6 copies (no data change),
  the Diary pages as before.
- Seen: six rows (1/5 Summer with portrait and date, 2/5..5/5 Empty Diary, Autosave with AUTO and Latest!); Down
  x5 and Up move the green focus, the hand cursor and the card through all six and wrap; the Autosave row shows the
  Autosave card. The Diary opens on the newest row (as the game does).

### Still to do

- **Ship it without game files:** the 3 changed files are derived from the game, so they can't be in the repo. The
  runtime must make them in memory when it builds the RomFS (as `modmerge.c` does for cfg.bin), from the owner's own
  files: port `build_mod.py` (G4SK bones, the locator lines, the copy count, the `cpk_list` entries) to C.
- On opening, the green highlight shows on 2/5 for the first frames (the engine drew its own first choice, row 1)
  while the cursor and card are on the newest row; right after the first Up / Down all agree. The engine's
  focus-apply call for a given index is not found yet (`f_9f3470` is its input-driven mover).
- The new rows don't slide in (no G4RA tracks for the new bones).
- Not tested: loading from rows 2..5 and the Autosave row with A, X delete on a stacked row, the mouse, the in-game
  Save screen (it has no row layer: it still pages), English text overflow on long names.

### Built in, and the owner's first test (2026-11-17, later)

- **Built in:** `runtime/src/diaryrows.c` makes the three files in memory at start (`romfs.c`, after the mods: a mod's
  copy is the one changed; all three or none), and frees them in the archive list. Its output is byte for byte the
  Python tool's that was tested in the game. Log: `diary rows: on (6 row copies; 3 list entries freed)`.
  `YW4_DIARY_ROWS=0` keeps the game's two rows. No mod folder is needed.
- **One Autosave** (owner: "autosave is autosave", "the game tells you that the save is newest anyways"): `AUTOSAVE`
  is always the main folder's, whatever slot is active; a slot's summary row 1 is the autosave its game made, the
  newest of them is the one on disk. Deleting a slot leaves the Autosave.
- **More of the game's two-row code found and handled with the rows stacked:**
  - `f_75f6b0(menu, &layer)`: a layer opened (base open `f_6c5cb0`, then the rows' first focus = `menu + 0x748`,
    kept below the grid's rows): the grid made 6 and 0x748 the newest row before it runs (skipping it hid the rows).
  - Copy 1's own setup gave it the Autosave look (dark text, no pencil): skipped too; the pencil shows on empty rows.
  - `f_9f2a00(menu, &layer, wrap)`: the engine's one-up; wraps only with wrap set: set for the rows.
  - `f_9f4020(menu, &layer, 0)`: applies the focus look (the engine calls the rows' focus handler back from it).
  - `f_2672d0`: the Lua call "has row N a save?" (`hdr + N * 0xa0 + 0xa9`, two rows only): answered from the
    focused row (the Autosave row said "There is no save data available.").
  - `f_266fc0`: the Lua call "load row N" (kind: row 1 -> 2, else the row: copy 5 asked for kind 5): at `0x267030`
    its kind is set 0 or 2 from the focused row.
  - The load request's kind is also set from the row in `yw4_hook_save_load`; the title's "empty active slot ->
    first used" fallback is off with the rows (it loaded slot 1 for an empty slot 2).
  - On an empty save row A is held back from the game (`savefs_filter_buttons`, svc_hid.c): no "load this diary?"
    box (owner). The game's "denied" sound is not played yet (its sound call not found).
- **Labels:** no "<n>/5" with the rows (owner: "do we need to show the numbers?").
- Tested (`dev_run_20261117_055132` .. `_060435`): opens on the newest row with focus, cursor and card together;
  Up / Down wrap both ways; A on an empty slot does nothing; A on a save asks and loads that slot's save (kind 0);
  A on the Autosave asks and loads it (kind 2).
- Still open: the denied sound; the rows' slide-in; the in-game Save screen (no row layer: it still pages, with
  numbers); X delete and the mouse on the stacked rows not run.

### The in-game Save, the badge, and test speed (2026-11-17, later)

- **Opens on the slot being played:** the first-focus hook runs on the title's Continue only (`menu + 0x721`): the
  in-game Save has the row layer too, unshown, and it had opened on the newest slot.
- **Its hint:** text `0xcdcfb802` in `system_text` (the `diary_save` script passes it): "`<CMD_ENTER|5>[CY2]Save[C]
  <CMD_BACK>Back`". In memory at start (`diaryrows_save_hint`, the English line only) it becomes
  "`[$gaiji_key_lt_s000][$gaiji_key_rt_s000]Change Slot   ...`": the D-pad's left and right as two icons, so a
  keyboard shows their keys (`gaiji_key_all_s100`, the pair, is a group and keeps the Switch icon: `bindings.c`
  `gaiji_swap`). The game's help text gives the tags: "`<CMD_CHANGE_BACK1>`, `<CMD_CHANGE_NEXT1>` (L / R), or
  `[$gaiji_key_all_s100]` (the D-pad's left / right) to switch screens".
- **Paging held back while its "Would you like to save all data to your diary?" box is up:** the box isn't a
  `CMenuGeneralWindow` the count at `main+0x5511a84` sees (0 with it up). The menu's base bytes `+0x1a6` and
  `+0x1ac` are 0 while the box has the input and 1 again after it (menu dumps, `dev_run_20261117_065209`); the
  engine's layer state (`+0x51`, `+0x52`) didn't change.
- **"Latest!" over the row above:** each row's `CMenuRenderComponent` `m_drawPriority` (`+0x94`; the property table
  in `f_9d96b0`) is set 20 + row, the cursor's 26 (a higher one is drawn later). The component is found in the
  copy's object's list (`+0xe8`, next `+0x10`) by its type name (`N5lives20CMenuRenderComponentE`,
  `main+0x12bfe30`), as `f_70c920` finds a locator. Rows 100 apart from -250 (centred on the card).
- **Open time:** 832 ms (26 frames) with and without the stacked rows (four runs); the painting ~1 ms.
- **Test speed:** `YW4_FPS=60` with `--exit` runs the 4,500-tick route in 81 s (30: ~190 s). Unlimited (`0`)
  doesn't keep to the route: the frame clock counts the frames a load takes, so input lands early.
