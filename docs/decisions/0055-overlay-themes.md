# 0055 — Overlay and HUD themes: the launcher's theme files, read when the game starts
<!-- decision
state: accepted
date: 2026-11-06
replaces: none
code: runtime/src/theme.c, runtime/src/theme.h, runtime/src/gamemenu.c, runtime/src/hud_style.c, runtime/src/settings.c, launcher/src-tauri/src/ready.rs
-->

Status: **accepted** (2026-11-07: the owner OK'd it, "push all", and asked for the sync below). The owner asked for themes for the in-game overlay and the HUD, switched live
from the overlay's Settings. The question of reading the files at run time or building them in was put to the
owner, who went to bed and left the job to finish; run time was picked as below.

## Context

The launcher already has themes: one CSS file per theme in `launcher/ui/themes/<id>.css` (a `name:` comment, then a
`:root` block of colour tokens with the names at the top of `launcher/ui/style.css`). Anyone can drop a new file in
that folder. The overlay and the HUD had their colours fixed in the code (the overlay design's).

## Decision

- **One set of themes for both.** The game reads the same files, when it starts. A theme added to the folder
  shows up in the launcher and in the game, with no build.
- **Built in: the design's colours** ("YW4 Recomp", id `recomp`, no file). Any token a theme leaves out falls back
  to it, so a theme can't break the overlay, and a missing folder only means one theme.
- **Roles, not colours.** `theme.h` lists what each colour does (body text, the dark, headings, the focus ring,
  fills, hover, the HUD's card...), and each role takes one launcher token: `--ink`, `--violet-deep`, `--violet`,
  `--lilac-0..3`, `--gold` for gold fills, `--select` for the focus ring (the launcher's "selected" outline: some
  themes' gold is too pale for a ring), `--bar-ink` for soft text on the dark, `--danger`. The HUD's traffic-light
  colours (good, slow, bad) stay the same in every theme: they mean something.
- **Colours in the files:** `#rgb`, `#rrggbb`, `rgb()`, `oklch()` are read; anything else (a `var(...)`) keeps the
  built-in colour.
- **Where the folder is:** `YW4_THEMES_DIR`, else `themes` next to yw4.exe, else the repo's
  `launcher/ui/themes` (a build in `work/build`). An installed game is in `<install>\work\build`, so the launcher
  passes `YW4_THEMES_DIR` (its own themes folder) when it starts the game.
- **The choice:** the overlay's Settings > Look > Theme (Left / Right step through them, as any row; Preview shows
  them on the bar), saved as `YW4_THEME=<id>` in settings.ini.
- **Synced with the launcher** (owner, 2026-11-07): on Play the launcher passes its theme (`YW4_THEME`, which wins
  over settings.ini) and its themes folder (`YW4_THEMES_DIR`); when the game closes, the launcher reads the game's
  settings.ini line back and takes that theme (`launcher/src-tauri/src/ready.rs` game_theme, the `game-theme`
  event). A theme picked in either place is the other's too.

**Dark mode (2026-11-25/26, owner: the launcher and settings "have me blind"; then "instead of a dark theme add a dark
mode toggle which switches every theme to dark").** A switch, not a theme: the launcher's Settings > Theme and start >
Dark mode, the overlay's Look > Dark mode (`YW4_DARK`, saved in settings.ini; both ways like the theme: ready.rs
passes it, `game_setting` reads it back, the `game-dark` event). Every theme keeps its accents (gold, Play, the focus
ring's colour, its purple's hue) and gets dark neutrals tinted with the hue of its darkest colour (`--violet-deep`):
the same sums in `launcher/ui/themes.js` (`darkTokens`) and `runtime/src/theme.c` (`darken`), from the same file, in
OKLCH. Done in script, not CSS relative colours: the Linux launcher's WebKitGTK may lack them.
A dark page needs three colours the light themes shared: text on the dark bars was `--white` (the cards' fill),
the overlay's page titles and chosen values were `night` (the bars' fill), text on gold was `--ink`. New tokens
`--snow` (launcher; overlay `snow`, `hud_text`), `--title-ink` (overlay role `title`: 21 text calls that drew
`NIGHT` on the page) and `--gold-ink` (now also the overlay's `on_gold`). A theme file without them keeps exactly
the old colours (`load_file`), so light mode doesn't change. A first try, a separate "Dark" theme file, was
dropped for the switch. Checked: the launcher's Play screen with five themes and its Settings page; the overlay's
Settings page with Jibanyan. Not checked: the launcher's popups and other pages, the overlay's other pages.

## Why not build them in

Built in, a new theme needs a rebuild of the game, and the two lists drift apart. Reading about ten small files at
start costs nothing measurable.

## Open

- The old F1 panel stays in its own colours (its pixel font and look are separate).
