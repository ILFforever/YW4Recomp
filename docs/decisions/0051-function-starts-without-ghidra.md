# 0051 — Function starts without Ghidra: 597 addresses in the repo
<!-- decision
state: accepted
date: 2026-11-03
replaces: none
code: recomp/main_extra_functions.txt, recomp/translate.py
-->

Status: accepted (2026-11-03). `recomp.translate` gives byte-identical output (all 152 generated files) with the
file as it did with Ghidra's export.

## Context

`recomp.translate` finds `main`'s functions from the player's own file: `.eh_frame` starts, address-taken code
pointers, exported symbols and BL targets (39,631 functions), plus Ghidra's function list
(`<work_dir>/ghidra/main.functions.json`, `python -m tools.yw4.ghidra`). The launcher builds the game on players'
PCs (decision 0050), where there is no Ghidra (a Java install, a Switch loader, a long analysis): its first real
build stopped at the translate step for want of that file.

Ghidra's list has 597 starts the rules miss: 74 reached only by a tail call (B), 328 only by a conditional
branch, 202 by no direct branch at all (jump tables, pointers in data). No single rule finds them; leaving them
out would fold them into neighbouring functions and trap if the game ever jumps to one indirectly.

## Decision

`recomp/main_extra_functions.txt`: those 597 offsets, one per line with a header saying what they are. The
translator reads it instead of the Ghidra export. It is numbers only (where functions begin in 2.2.0, as
`target.toml` holds hashes and sizes): no game code and no recompiled output (rule 1). Only the starts the
translator can't find from the player's file are kept; the other ~39,600 still come from that file.

## Consequences

- Building needs no Ghidra; Ghidra stays a research tool (`docs/ghidra.md`).
- When the translator's own rules improve, the file can shrink; it must be regenerated (from a Ghidra export)
  only if the target build ever changed, which rule 3 rules out.
