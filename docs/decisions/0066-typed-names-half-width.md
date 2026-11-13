# 0066 — Typed names keep their plain letters

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: runtime/src/swkbd.c, recomp/overrides.toml
-->

Status: accepted. Owner-tested 2026-11-14 (a rename to "Summer" is accepted and reads normally).

## Context

A name typed in our keyboard (new game, or a rename) showed spread out on the game's name plates: "N a t e",
"H a m". The body text and other speakers' names were fine, and names typed on the owner's Switch read normally.

## Findings

- Our keyboard answers plain UTF-8, as the game asks (`utf8` in its KeyboardConfig), the same form the Switch's
  keyboard gives (a result code, then the text; Switchbrew, Ryujinx).
- After an OK answer the game's keyboard function (`f_959be0`) copies the text (`strcpy`) into a buffer at
  `[main+0x56b8288]`, then its handler `f_8aa550` runs it through `f_9997e0(dst, src)`: each character through a
  156-entry table at `main+0x12be9d0` (u16 from, u16 to). The table maps every ASCII character to its full-width
  form (A to Ａ, space to U+3000) and half-width katakana to normal katakana. The script then reads the buffer and
  stores the name (`f_268140`), as Ｎａｔｅ, which the name plate draws a full-width cell per letter.
- Found with the write watch (`YW4_WATCHW`) on the stored name and then on that buffer, during a rename.
- The game's name check (the text-check callback, `f_959ee0`) runs the same conversion and then allows
  full-width letters only: skipping the conversion there makes every English name "cannot be used".
- Not explained: why Switch-typed names stayed plain (the owner played only this version). The conversion above
  runs for every OK answer, so something on the console differs; it doesn't change the fix.

## Decision

A hook on `f_9997e0` (`yw4_hook_kbd_fullwidth`): for the call that converts the answer the game keeps
(`f_8aa550`, return address `main+0x8aa5f8`) it does the same conversion with the game's own table, except that
ASCII stays as typed. Katakana are still made full-size, so Japanese names are as before. Every other call,
including the name check's, runs the game's conversion unchanged.

## Rejected

- Converting the stored name back afterwards: the name is copied to several places (save record, text
  buffers, message strings) before we could see it.
- Skipping the conversion for every caller: the name check then rejects English names.
