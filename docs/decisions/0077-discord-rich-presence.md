# 0077 — Discord Rich Presence: "Playing Yo-kai Watch 4", off unless turned on

<!-- decision
state: accepted
date: 2026-11-15
replaces: none
code: runtime/src/discord.c, runtime/src/discord.h, runtime/src/discord_ipc.c, runtime/src/discord_ipc.h, runtime/src/settings.c, runtime/src/main.c, tests/test_discord.py
-->

Status: accepted (owner, 2026-11-15, through the relay session). Tested against a stand-in Discord on Windows and
Linux; not yet against the real Discord app, which needs the owner's application ID.

## Context

The owner wanted the player's Discord profile to show that they are playing the port. The first ask had the current
area and the player's level too; the owner then cut it to the basics: the game's name and the time played.

## Decision

- **What shows:** the Discord application's name (the owner names it "Yo-kai Watch 4"), its art uploaded as `icon`,
  and "elapsed" from when the game started. Nothing about the game's state is read, so nothing in the game's threads
  changes.
- **Off unless turned on:** DISCORD STATUS (Settings, ADVANCED), OFF / ON, saved as `YW4_DISCORD=0/1`, default OFF.
  Turning it off clears the activity at once.
- **The application ID** is `YW4_DISCORD_APP_ID` in `runtime/src/discord.h`, empty until the owner creates the
  application; an environment variable of the same name overrides it. Empty: no thread at all, and the row is greyed
  out ("NOT SET UP IN THIS BUILD").
- **No library.** Discord's local IPC is small: frames of an 8-byte header (op, length) and JSON. `discord_ipc.c`
  has the transport and the three messages (handshake, SET_ACTIVITY, clearing it) in about 250 lines. Windows: the
  named pipe `\\.\pipe\discord-ipc-0` to `-9`. Linux: the socket `discord-ipc-N` in `$XDG_RUNTIME_DIR`, `$TMPDIR`,
  `$TMP`, `$TEMP` or `/tmp`, and their `app/com.discordapp.Discord/` (Flatpak) and `snap.discord/` (Snap) folders.
  Discord's own SDK (a large DLL, its own licence) and discord-rpc (archived) were not needed for this.
- **Quiet:** a thread of its own wakes once a second. While on, it tries to connect every 15 s; no Discord means no
  error, no window and one log line at most ("connected", "status cleared"). It sets the activity once per connection
  and reads what Discord sends back (answers, pings) so the pipe never fills. Handshake replies other than READY (a
  wrong ID gets a CLOSE) end that attempt.

## Not done

- **Area and player level.** Looked into before the owner cut them: the save header (`HEADERSAVE/data.bin`, 1118
  bytes, not encrypted, fixed layout) holds 12 (character ID hash, level) pairs, the party, at 0x34-0x1A0 (the town
  save's first two, levels 2 and 2, match the Yo-kai Pad's "N Lv 2" and "Jibanyan Lv 2"), and a pair at 0x1E0/0x1EC
  that changes with the location (the same in saves at the same place). Both are rewritten when the game saves (an
  autosave changed the header's second block), so they would show the last save, not the moment. Still open: which
  party slot is "the player", and area IDs to names (the player's RomFS text). A live value would need the game's
  memory found and hooked.
- The real Discord app: waiting for the application ID (and its `icon` art).

## Tests

`tests/test_discord.py`: the frame bytes, the handshake and activity JSON byte for byte (escaping, clearing, a buffer
too small), a connect with nobody listening (fails quietly), and the presence thread against a stand-in Discord that
checks the handshake, answers READY and checks the activity. Passes on Windows (llvm-mingw clang, named pipe) and
Linux (WSL: Clang 19 and GCC 13, Unix socket).
