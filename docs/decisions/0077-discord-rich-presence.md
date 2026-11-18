# 0077 — Discord Rich Presence: "Playing Yo-kai Watch 4", where and the Watch Rank, on unless turned off

<!-- decision
state: accepted
date: 2026-11-15
replaces: none
code: runtime/src/discord.c, runtime/src/discord.h, runtime/src/discord_game.c, runtime/src/discord_game.h, runtime/src/run.c, runtime/src/discord_ipc.c, runtime/src/discord_ipc.h, runtime/src/settings.c, runtime/src/main.c, tests/test_discord.py
-->

Status: accepted (owner, 2026-11-15, through the relay session). Tested against a stand-in Discord on Windows and
Linux, and on 2026-11-16 against the real Discord app on Windows with the owner's application ID: "discord:
connected", the owner saw "Playing Yo-kai Watch 4" on their profile ("works"), clean exit after a ~3 h run.

## Context

The owner wanted the player's Discord profile to show that they are playing the port. The first ask had the current
area and the player's level too; the owner then cut it to the basics: the game's name and the time played.

## Decision

- **What shows:** the Discord application's name (the owner names it "Yo-kai Watch 4"), its art uploaded as `icon`,
  and "elapsed" from when the game started; since 2026-11-18 also where the player is and the Watch Rank (below).
- **On unless turned off** (since 1.7.0, owner 2026-11-18; before: off unless turned on): DISCORD STATUS (Settings,
  GENERAL), OFF / ON, saved as `YW4_DISCORD=0/1`, default ON. Most games and emulators show it, and Discord's own
  Activity Privacy setting hides it for a player who doesn't want it. Off in test runs (`YW4_SETTINGS_FILE=none`).
  Turning it off clears the activity at once.
- **The application ID** is `YW4_DISCORD_APP_ID` in `runtime/src/discord.h`: the owner's application
  `1571532450742665347` (created 2026-11-15; public, it goes in every build). An environment variable of the same
  name overrides it. Empty: no thread at all, and the row is greyed out ("NOT SET UP IN THIS BUILD").
- **No library.** Discord's local IPC is small: frames of an 8-byte header (op, length) and JSON. `discord_ipc.c`
  has the transport and the three messages (handshake, SET_ACTIVITY, clearing it) in about 250 lines. Windows: the
  named pipe `\\.\pipe\discord-ipc-0` to `-9`. Linux: the socket `discord-ipc-N` in `$XDG_RUNTIME_DIR`, `$TMPDIR`,
  `$TMP`, `$TEMP` or `/tmp`, and their `app/com.discordapp.Discord/` (Flatpak) and `snap.discord/` (Snap) folders.
  Discord's own SDK (a large DLL, its own licence) and discord-rpc (archived) were not needed for this.
- **Quiet:** a thread of its own wakes once a second. While on, it tries to connect every 15 s; no Discord means no
  error, no window and one log line at most ("connected", "status cleared"). It sets the activity once per connection
  and reads what Discord sends back (answers, pings) so the pipe never fills. Handshake replies other than READY (a
  wrong ID gets a CLOSE) end that attempt.

## Where the player is (added 2026-11-18, owner's ask)

**The card** (an emoji in front of each line; `discord_presence_text` in `discord.c`):

- Title's menus (`scene_on_title`): "🏠 In the main menu".
- Field: one of 20 wordings with the place, "🗺️ Exploring Meeting Bridge", "❓ Lost in …", "🤪 Mucking about in
  …" (the owner added those two) and so on; a new one, never the same twice, each time the place changes.
- Battle (`battle_active`): one of 6, "⚔️ Battling in …", "⚡ Unleashing a Soultimate in …", a new one each battle;
  the field's wording stays as it was when the battle ends in the same place.
- Second line "⌚ Watch Rank B". The icon's hover: the area ("Springdale City"). A button "Get YW4 Recomp" to the
  project site (Discord hides a player's own buttons from them).
- The place is the zone when it has a name ("Meeting Bridge"), else the area. At most one update per 5 s (Discord
  takes 5 per 20 s), the newest text. The random pick is the thread's own xorshift, not `rand()`.

**The area** is read live, not from the last save: the game data object at `*(*(main+0x1548fd8) + 0x5778)` is what
the save summary copies when the game saves (`f_32ec30`): +0x17 the Watch Rank (0 E .. 5 S), +0x30 the map ID. The
name is the game's own text, looked up as the Diary does: map ID -> text ID with `f_70ff90` (a pointer to the ID) or
else `f_70fdc0`, then `f_9b10d0(table, text, 0)` in the text tables at `*(G+8)` then `*(G+0)`. Map ID 0 (a save
still loading) is skipped: its text is an unrelated string ("Yo-kai Soulmates").

**The zone** ("Meeting Bridge") is what the expanded minimap shows under it; the game works it out only then
(`f_758430`, found by logging every text lookup, `YW4_PROBE=main+9b10d0`, while the owner opened it). The same steps
here: the zone slot at the data object's +0x301, its handle at FM + slot*0x14 + 0x44 (FM = `*(main+0x154b190)`, the
field manager; low 16 bits an index below 0x200, checked against FM + index*0xa60 + 0x148), whose record +0x15c is a
block at FM + block*0x100 (+0x18dab0: -1 for none, +0x18db90 the zone's number); the map's internal name from
`f_4d8e60(*(G+0x230), FM's map ID)`; `sprintf(main+0x12524ff, name, number)` ("block_name_t201_07"); its CRC-32
with the game's table (main+0x12bd348) is the text ID. No such text ("block_name_t201_00"): the area. On the way to
another area the field's map (and zone) turns over a moment before the data's map ID; the zone waits until both name
the same map (they are the same IDs: 0x6c6eefe0 in town).

All of these are the game's functions, so `discord_game.c` runs them on the game's thread, in the field player
controller hook (`run.c`), only when the map or zone changed and only while DISCORD STATUS is on.

Seen (2026-11-18): test runs, the late-game save `Mystic Grove`, rank 3 (B), as its save summary; the town walk
`Uptown Springdale` then the zone `Springdale Elementary`. The owner on the real Discord app (Windows): Mystic Grove,
then Springdale City and South Stream on walking there ("works"); zones Spring Breeze Blvd., Springdale Elementary,
Wildwood Way. Rank badges (art `rank_e` .. `rank_s` as the small picture) were drawn and dropped (owner, 2026-11-18).

## Not done

- **Player level.** (The area and zone are shown since 2026-11-18, above.) Looked into before the owner cut them: the save header (`HEADERSAVE/data.bin`, 1118
  bytes, not encrypted, fixed layout) holds 12 (character ID hash, level) pairs, the party, at 0x34-0x1A0 (the town
  save's first two, levels 2 and 2, match the Yo-kai Pad's "N Lv 2" and "Jibanyan Lv 2"), and a pair at 0x1E0/0x1EC
  that changes with the location (the same in saves at the same place). Both are rewritten when the game saves (an
  autosave changed the header's second block), so they would show the last save, not the moment. Still open: which
  party slot is "the player", and area IDs to names (the player's RomFS text). A live value would need the game's
  memory found and hooked.
- No invite / cover image on the Discord application (owner skipped it, 2026-11-16).
- Linux against the real Discord app: not run yet (stand-in only).

## Tests

`tests/test_discord.py`: the frame bytes, the handshake and activity JSON byte for byte (escaping, clearing, a buffer
too small), a connect with nobody listening (fails quietly), and the presence thread against a stand-in Discord that
checks the handshake, answers READY and checks the activity (its lines, hover and button, with a faked game
state); every wording names the place. Passes on Windows (llvm-mingw clang, named pipe) and
Linux (WSL: Clang 19 and GCC 13, Unix socket).
