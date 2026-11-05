# 0054 — Game updates and launcher updates, each on its own
<!-- decision
state: plan
date: 2026-11-05
replaces: 0052 (part)
code: launcher/VERSION, launcher/package.py, launcher/src-tauri/src/buildrun.rs, tools/yw4/build_install.py
-->

Status: **accepted** (2026-11-05, the owner's answers below). Built so far: the launcher's own number (0.5.0) and
the build steps in the game code with the old game kept during an update (`tools/yw4/build_install.py`). Not yet:
the game zip, the update check and the Play screen band. Changes decision
0052 (the game's build code no longer lives only inside the launcher's installer) and carries out decision 0047's
"the launcher has its own number".

## Context

Since decision 0052 the installer carries two things: the launcher (the window, its Rust side, the bundled Python,
`glslang`, `hactool`) and the game's build code (`builder\src`: the recompiler, the runtime and `tools/yw4`, about
3 MB, from `git archive`). So today:

- a game fix (most of our changes: the renderer is still moving fast) needs a whole new installer, and
- a launcher fix (a button) changes the number players see and would tell them to rebuild for nothing.

Decision 0047 already wanted the two apart: "A launcher fix shouldn't force a rebuild of the game, and a game fix
shouldn't need a new launcher." On 2026-11-05 the status bar was made to read the repo's `VERSION` file to fix a
stale "0.1.0"; that tied them together again and is undone here.

The build code is different from the downloads we already fetch (compiler, SDL, FSR, DLSS): those are fixed files
pinned by hash in the code. A game update is new code by definition, so it can't be pinned in advance. It is
compiled and run on the player's PC, so it needs a way to prove it came from us.

## Proposal

### Two numbers, two kinds of release

| | Game | Launcher |
|---|---|---|
| Number | `VERSION` (0.7.0 now), as in 0047 | `launcher/VERSION`, its own, starting at 0.5.0 (1.0 once every page is done) |
| GitHub release tag | `vX.Y.Z` | `launcher-vA.B.C` |
| Release files | `yw4recomp-game-X.Y.Z.zip` (the build code) | the installer and the portable zip |
| What a player does | the launcher downloads it (~3 MB), checks it, then offers a rebuild | the launcher says a new one is out; the player installs it over the old one |
| Needs a rebuild | yes (~5 min, not measured yet) | no |

The installer still carries the game code of its day, so a first install needs no extra download.

### Where the game code lives on the player's PC

`<install>\builder\game\X.Y.Z\` for each downloaded version, next to the compiler (`builder\toolchain\`). For a
build the launcher picks the newest of: the version it carries itself, and the downloaded ones. It keeps the
previous one too. Free up space deletes everything but
the one in use.

### The build steps move into the game code

Today the list of steps (extract, ingest, translate, shaders, build) is written into the launcher's Rust code
(`buildrun.rs`). A game update that adds or changes a step would then need a new launcher, which defeats the
point. So the game code gets one entry point, `python -m tools.yw4.build_install`, that runs every step itself.
At the start it prints the steps it will run (`@steps <id> <label> <seconds> ...`), then the usual `@progress`
lines. The launcher only runs it and draws what it reports, so its build screen works with any game version.

Each game release also says which launcher it needs, in a small `release.toml` inside the zip
(`version`, `min_launcher`, `python`). If the player's launcher is too old, it says "Update the launcher to get game
update X" instead of downloading code it can't run.

### Checking a game update: GitHub's hash, no signing key for now

Each download is checked against the SHA-256 GitHub reports for that release file (its API's `digest`), which
catches broken and partial downloads. That trusts the owner's GitHub account, as the installer download already
does; the account should have two-factor sign-in.

**Not done for now (owner, 2026-11-05): signing with a private key.** It would also stop someone who took over the
GitHub account from shipping code to players, at the price of a key to look after (lost key: no game updates
until players install a launcher with a new public key). It can be added later with one launcher update: the
launcher would then refuse unsigned game zips. Worth it if other people get publish rights or the project grows.

### If an update's build fails, the old game stays playable

Before a rebuild the launcher moves the working game aside (`work\build`'s `yw4.exe`, its DLLs, `shaders\` and
`game.ini`, about 200 MB) and puts it back if the build fails, so Play still starts the old version and the player
can try the update again later. It is deleted once the new build has finished.

### When the launcher looks, and what the player sees

- **On start**, one request to GitHub's list of releases (no account, no token), at most once an hour. If GitHub
  can't be reached, nothing is shown and nothing breaks. A launcher built from the repo (`cargo build`) never
  looks: it builds the repo's own code.
- The newest release of each kind counts; pre-releases (such as `deps-mirror`) never do.
- On the Play screen, one band at most:
  1. **Game update:** "Game update 0.8.0 · What's new · Update": download, check its hash, then the build
     screen (the player can wait and do it later).
  2. **Launcher update:** "Launcher 1.1.0 is out · Download": opens the release page in the browser.
  3. Both: the game band, unless the game update needs the newer launcher; then the launcher band, which says why.

### Releasing (the owner)

- `python launcher/package.py game`: bumps nothing by itself. Makes the game zip from the commit and prints the files and the `vX.Y.Z` tag to publish.
- `python launcher/package.py launcher`: makes the installer and zip as today, with the newest game code inside.
- Publishing stays by hand at first (`gh release create`). A GitHub Actions job can do it later.

## Alternatives

- **Keep one installer for everything** (today): simplest, but every game fix is a full reinstall, and launcher
  fixes look like game updates.
- **Sign game updates with a private key:** also stops a stolen GitHub login from shipping code; left for later
  (above).
- **Tauri's own updater for the launcher too:** one click, but it needs its own signing key and setup. Worth it
  later, once releases come regularly.

## The owner's answers (2026-11-05)

1. Signing: not now (GitHub's hash, two-factor sign-in on the account; signing can come later).
2. The launcher's number: 0.5.0; 1.0 when every page is done.
3. The build steps move into the game code: yes.
4. No new dependencies needed without signing.
5. A failed update build keeps the previous game playable: yes.

## Then

Build in this order: the launcher's own number; `tools.yw4.build_install` with the launcher using it; the game zip in
`package.py`; setting the old game aside during a rebuild; the update check and the band; then a full test (an old install updated to a newer
game version, and a launcher update with no rebuild).
