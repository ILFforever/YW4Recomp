# 0047 — Versioning: one number for the project, one for the launcher

Status: accepted (2026-11-03). The project's number is in place (0.1.0); the launcher doesn't exist yet.

## Context

Shipping to other players (option A: a launcher builds the game on the player's PC) means bug reports from
builds we never saw. Until now a build had no name: the only clue was the log's date. We need to know which
code a report came from, and the launcher needs to know whether the game it built is current.

There are two programs with their own lives: the **game** (runtime + recompiler + tools in this repo, built on the
player's PC) and the **launcher** (the setup and play window). A launcher fix shouldn't force a rebuild of the
game, and a game fix shouldn't need a new launcher. The game version is separate again: always 2.2.0
(`target.toml`, rule 3).

## Decision

- **The project:** `VERSION` at the repo root holds `MAJOR.MINOR.PATCH`, starting at `0.1.0`. 0.x while the game
  isn't playable end to end. MINOR for new features or anything that needs a rebuild of the game; PATCH for
  fixes. A release is a git tag `vX.Y.Z` on the commit that changes `VERSION`.
- **Which code exactly:** every build writes `<work_dir>/build/version.c` (`recomp/build.py`, `version_source`):
  the number, the git commit (`+dirty` when tracked files had uncommitted changes) and the build day (UTC). A
  release archive has no git, so the build reads a `COMMIT` file there instead (the release packaging writes it);
  with neither, `unknown`. It is its own object file and rewritten only when it changes, so a new commit
  recompiles one small file, not the game.
- **Where it shows** (`runtime/src/version.h`): the log's first lines (`yw4: YW4 Recomp 0.1.0 (commit d42294d,
  built 2026-11-03)`), the window title, and `yw4 --version` (one line on stdout, without starting the game), which
  the launcher reads and a player can paste into a bug report.
- **The launcher** gets its own `MAJOR.MINOR.PATCH`, kept in its own folder when it's written, shown in its status
  bar next to the game's. It rebuilds the game when the project version it would build differs from the one
  `yw4 --version` reports.

## Consequences

- Every log names its build: a report with a log is enough to find the code.
- Bumping `VERSION` is a manual step at release time; between releases the commit identifies the build.
- `+dirty` builds (the owner's day-to-day ones) are marked as such, so a report from one is easy to spot.
