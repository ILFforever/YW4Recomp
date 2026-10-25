# 0027 — The project's license: MIT

Date: 2026-10-26
Status: accepted (owner: "mit sure", after comparing MIT and GPL)

## The question

The repository had no license, so nobody could legally reuse or contribute to it. MIT or GPL?

## Decision

**MIT** (`LICENSE` at the root). It covers our code and docs only: not the game, not files from it, not anything
generated from a player's copy (recompiled C, translated shaders, builds), which never enter the repository
(CLAUDE.md rule 1).

## Why

- No emulator code is linked ([0005](0005-phase1-runtime-strategy.md)), so GPL's main advantage for this project,
  being able to reuse code from the GPL yuzu forks (Eden, Citron, Suyu), is not needed today.
- It matches N64Recomp, the model the project follows, and most recompilation projects.
- What we ship from others fits: musl is MIT, SDL3 (fetched, not committed) is zlib.

## Consequences

- **No GPL code can be copied in** from now on (the yuzu forks included) without relicensing the project, which
  needs every contributor's agreement. CLAUDE.md rule 5 already asks to flag any emulator code before copying it.
  Code under MIT-compatible licenses (MIT, BSD, zlib, Apache 2.0) can be used, with its notice kept.
- Anyone may make closed forks of our code; that was accepted as the price of the simpler license.
- Switching to GPL later stays possible; going back from GPL would not be.

## Alternative

**GPL v3**: forks would have to stay open, and code from the yuzu forks could be reused. Rejected for now: we take
none of their code, and it would rule out MIT-only reuse of ours.
