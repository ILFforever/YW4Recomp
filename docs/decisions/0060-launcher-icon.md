# 0060 — The launcher's icon: the owner's own Whisper silhouette
<!-- decision
state: accepted
date: 2026-11-10
replaces: none
code: launcher/icon-source.png, launcher/src-tauri/icons
-->

Status: accepted (2026-11-10). The owner's choice: the repo's icon now shows a character from the game.

## Context

The launcher and its installer need an icon before setup has run, so it can't come from the player's files the
way `yw4.exe`'s does (decision 0048). Until now it was our own placeholder: "YW4" in a gold ring on a purple
rounded square, with no Nintendo or LEVEL-5 art.

The owner wanted an icon fans recognise at a glance. Logo ideas built from the official 4++ logo were tried
first and dropped: the AI drafts garbled the lettering.

## Decision

The icon is the owner's own artwork: a white silhouette of Whisper, made by the owner, inside the old gold ring
on the old purple rounded square (base `#45297a`, ring `#e6bc50`). `launcher/icon-source.png` (1024 px) is the
source; the icons Tauri uses (`32x32`, `128x128`, `128x128@2x`, `icon.png`, `icon.ico`) were made from it with
`npx tauri icon icon-source.png -o src-tauri/icons` in `launcher/`.

## Consequences

- The repo now ships one piece of fan art of a game character. It is not game data (rule 1 covers dumps,
  extracted files, binaries and recompiled code), but it is a trademark and copyright risk the owner accepts. If
  LEVEL-5 or Nintendo object, the old "YW4" icon is in git history (`launcher/icon-source.png` before this
  record) and goes back in one commit.
- `yw4.exe` still gets the game's own icon from the player's files (0048); only the launcher and installer use
  this one.
- Windows caches taskbar icons: after a swap the old one can show until Explorer restarts.
