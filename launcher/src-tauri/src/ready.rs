// Screen 5, Ready: what the launcher remembers about the install, starting the game, the mods and DLC there,
// freeing space, deleting the player's originals (once offered, never the keys) and shortcuts (decision 0050).

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Command;

/// The launcher's own bookkeeping, in %AppData%\YW4 Recomp\launcher.json: which install it built and with what.
/// Not the game's saves or settings (those go where the player chose, game.ini says where).
#[derive(Serialize, Deserialize, Clone, Default)]
pub struct State {
    install: String,
    mode: String,
    paths: HashMap<String, String>,
    saves: String,
    fsr: bool,
    dlss: bool,
    #[serde(rename = "freeUp")]
    free_up: bool,
    desktop: bool,
    #[serde(rename = "startMenu")]
    start_menu: bool,
    #[serde(rename = "originalsOffered", default)]
    originals_offered: bool,
    /// Settings: what the launcher does on Play ("hide" until the game closes, "minimise", "keep", "close")
    #[serde(rename = "onPlay", default = "default_on_play")]
    on_play: String,
    /// Settings: start the game with the mods folder (false: none, e.g. Japanese without the English patch)
    #[serde(default = "yes")]
    mods: bool,
    /// Settings: show the game's console window with its log while playing (off by default)
    #[serde(rename = "showLog", default)]
    show_log: bool,
    /// Disabled mods by name: skipped during LayeredFS overlay
    #[serde(rename = "disabledMods", default)]
    pub disabled_mods: Vec<String>,
    /// Cached icon path so the launcher can display the game icon instantly on start
    #[serde(default)]
    pub icon: Option<String>,
}

fn default_on_play() -> String { "hide".into() }
fn yes() -> bool { true }

fn state_path() -> PathBuf {
    if let Ok(p) = std::env::var("YW4_LAUNCHER_STATE") {  // tests: keep their state out of the player's AppData
        return PathBuf::from(p);
    }
    PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp").join("launcher.json")
}

fn read_state() -> Option<State> {
    serde_json::from_str(&std::fs::read_to_string(state_path()).ok()?).ok()
}

#[tauri::command]
pub fn load_state(app: tauri::AppHandle) -> Option<State> {
    // at the launcher's start, before anything reads the install: a game an update left set aside goes back first
    if let Some(s) = read_state() {
        if !exe(&s.install).is_file() && !crate::buildrun::building(&app) { put_back_previous(Path::new(&s.install)); }
    }
    current_state()
}

/// The saved state, if its install is still there (with its icon made when it has none yet).
pub fn current_state() -> Option<State> {
    let mut s = read_state().filter(|s| exe(&s.install).is_file())?;
    if s.icon.is_none() {
        let dir = Path::new(&s.install).join("work").join("icon");
        s.icon = ensure_icon_image(&dir).map(|p| p.display().to_string());
    }
    Some(s)
}

/// An update sets the installed game aside in work\previous\ while it builds and puts it back if the build fails;
/// a launcher closed mid-build left it there, and the next start showed Setup as if nothing were installed
/// (2026-11-11). The same as tools/yw4/build_install.py's Previous.put_back: build\* and shaders\glsl back.
fn put_back_previous(inst: &Path) {
    let work = inst.join("work");
    let prev = work.join("previous");
    if !prev.join("build").join("yw4.exe").is_file() { return; }
    let mut kept: Vec<PathBuf> = std::fs::read_dir(prev.join("build")).into_iter().flatten().flatten().map(|e| e.path()).collect();
    if prev.join("shaders").join("glsl").exists() { kept.push(prev.join("shaders").join("glsl")); }
    for src in kept {
        let Ok(rel) = src.strip_prefix(&prev) else { continue };
        let dst = work.join(rel);
        let _ = if dst.is_dir() { std::fs::remove_dir_all(&dst) } else { std::fs::remove_file(&dst) };
        if let Some(d) = dst.parent() { let _ = std::fs::create_dir_all(d); }
        let _ = std::fs::rename(&src, &dst);
    }
    if exe(&inst.to_string_lossy()).is_file() { let _ = std::fs::remove_dir_all(&prev); }
}

#[tauri::command]
pub fn save_state(state: State) -> Result<(), String> {
    let p = state_path();
    std::fs::create_dir_all(p.parent().unwrap()).map_err(|e| e.to_string())?;
    std::fs::write(&p, serde_json::to_string_pretty(&state).unwrap()).map_err(|e| e.to_string())
}

fn exe(install: &str) -> PathBuf {
    let p = Path::new(install);
    let wb = p.join("work").join("build").join("yw4.exe");
    if wb.is_file() {
        return wb;
    }
    let gm = p.join("game").join("yw4.exe");
    if gm.is_file() {
        return gm;
    }
    let rt = p.join("yw4.exe");
    if rt.is_file() {
        return rt;
    }
    wb
}

fn normalize_path(p: PathBuf) -> PathBuf {
    let s = p.to_string_lossy();
    if let Some(stripped) = s.strip_prefix(r"\\?\") {
        PathBuf::from(stripped)
    } else {
        p
    }
}

pub fn resolve_install_dir(path: &Path) -> Option<PathBuf> {
    let p = if path.is_file() {
        if path.file_name().map_or(false, |n| n.eq_ignore_ascii_case("yw4.exe")) {
            let parent = path.parent()?;
            if parent.file_name().map_or(false, |n| n.eq_ignore_ascii_case("build"))
                && parent.parent().and_then(|gp| gp.file_name()).map_or(false, |n| n.eq_ignore_ascii_case("work"))
            {
                if let Some(root) = parent.parent().and_then(|gp| gp.parent()) {
                    return Some(root.to_path_buf());
                }
            }
            if parent.file_name().map_or(false, |n| n.eq_ignore_ascii_case("game")) {
                if let Some(root) = parent.parent() {
                    return Some(root.to_path_buf());
                }
            }
            return Some(parent.to_path_buf());
        }
        path.parent()?
    } else {
        path
    };

    // 1. Root folder containing work/build/yw4.exe or game/yw4.exe
    if p.join("work").join("build").join("yw4.exe").is_file() || p.join("game").join("yw4.exe").is_file() {
        return Some(p.to_path_buf());
    }

    // 2. Subfolder work/build
    if p.file_name().map_or(false, |n| n.eq_ignore_ascii_case("build"))
        && p.parent().and_then(|gp| gp.file_name()).map_or(false, |n| n.eq_ignore_ascii_case("work"))
    {
        if let Some(root) = p.parent().and_then(|gp| gp.parent()) {
            if root.join("work").join("build").join("yw4.exe").is_file() {
                return Some(root.to_path_buf());
            }
        }
    }

    // 3. Subfolder game
    if p.file_name().map_or(false, |n| n.eq_ignore_ascii_case("game")) {
        if let Some(root) = p.parent() {
            if root.join("game").join("yw4.exe").is_file()
                || root.join("work").join("build").join("yw4.exe").is_file()
            {
                return Some(root.to_path_buf());
            }
        }
    }

    // 4. Flat root folder containing yw4.exe directly
    if p.join("yw4.exe").is_file() {
        return Some(p.to_path_buf());
    }

    // 5. Subfolder data
    if p.file_name().map_or(false, |n| n.eq_ignore_ascii_case("data")) {
        if let Some(root) = p.parent() {
            if root.join("work").join("build").join("yw4.exe").is_file()
                || root.join("game").join("yw4.exe").is_file()
                || root.join("yw4.exe").is_file()
            {
                return Some(root.to_path_buf());
            }
        }
    }

    // 6. Generic child directory of the root
    if let Some(root) = p.parent() {
        if root.join("work").join("build").join("yw4.exe").is_file()
            || root.join("game").join("yw4.exe").is_file()
            || root.join("yw4.exe").is_file()
        {
            return Some(root.to_path_buf());
        }
    }

    None
}

fn hidden(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; cmd.creation_flags(0x0800_0000); }
    cmd
}

fn dir_size(p: &Path) -> u64 {
    if p.is_file() { return p.metadata().map(|m| m.len()).unwrap_or(0); }
    crate::check::folder_size(p).0
}

/// One NSP in data\dlc. The add-ons hold no name we can read, so a checked one is its index and title ID.
#[derive(Serialize)]
pub struct DlcItem {
    index: Option<u32>,       // None: the file isn't a known add-on (the game will leave it out)
    id: Option<String>,       // title ID
    file: String,
    #[serde(rename = "needsKeys")]
    needs_keys: bool,         // true when index 100 (4++ Upgrade Pack) hasn't been unpacked to data\dlc-romfs\100 yet
}

#[derive(Serialize)]
pub struct Info {
    version: String,          // yw4 --version
    dlc: usize,               // checked add-ons in data\dlc
    #[serde(rename = "dlcList")]
    dlc_list: Vec<DlcItem>,   // every NSP there: the checked ones by index, then the others
    mods: Vec<String>,        // mod folders in mods\ (Atmosphère's layout or plain)
    #[serde(rename = "disabledMods")]
    disabled_mods: Vec<String>, // disabled mod names from state
    icon: Option<String>,     // the game's icon, from the player's own files (the build makes it)
    shots: Vec<String>,       // screenshots, newest first (at most SHOTS)
    #[serde(rename = "shotCount")]
    shot_count: usize,        // all of them
    reclaimable: u64,         // what Free up space would delete
    #[serde(rename = "lastPlayed")]
    last_played: Option<u64>, // Unix seconds of the last start from the launcher
    #[serde(rename = "playSecs")]
    play_secs: u64,           // time played, started from the launcher
}

const SHOTS: usize = 48;

/// Play time, kept apart from launcher.json: the screens rewrite that file from their own copy, which would undo
/// what the game's exit adds here. Only starts from the launcher count (a shortcut to yw4.exe isn't seen).
#[derive(Serialize, Deserialize, Default)]
struct Played {
    install: String,
    #[serde(rename = "lastPlayed")]
    last_played: Option<u64>,
    seconds: u64,
}

fn played_path() -> PathBuf {
    state_path().with_file_name("play.json")
}

fn load_played(install: &str) -> Played {
    std::fs::read_to_string(played_path()).ok()
        .and_then(|t| serde_json::from_str::<Played>(&t).ok())
        .filter(|p| p.install == install)  // another install starts from zero
        .unwrap_or(Played { install: install.to_string(), ..Default::default() })
}

fn save_played(p: &Played) {
    let path = played_path();
    let _ = std::fs::create_dir_all(path.parent().unwrap());
    let _ = std::fs::write(path, serde_json::to_string_pretty(p).unwrap());
}

fn now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Folders the build only needs again for a rebuild: generated C, objects, downloads, shader assembly, unpacked NCAs,
/// and the compiler (decision 0052; the next build downloads it again, checked against the same hash).
fn reclaim_dirs(install: &Path) -> Vec<PathBuf> {
    let w = install.join("work");
    vec![w.join("recomp"), w.join("build").join("obj"), w.join("deps"), w.join("shaders").join("asm"), w.join("nca"),
         install.join("builder").join("toolchain")]
}

fn mod_names(mods: &Path) -> Vec<String> {
    let mut out = Vec::new();
    let read_dirs = |d: &Path| std::fs::read_dir(d).into_iter().flatten().flatten()
        .filter(|e| e.path().is_dir()).collect::<Vec<_>>();
    let has_romfs = |p: &Path| p.join("romfs").is_dir();

    if has_romfs(mods) {
        out.push("romfs".into());
    }
    for entry in read_dirs(mods) {
        let name = entry.file_name().to_string_lossy().into_owned();
        let path = entry.path();
        if name.eq_ignore_ascii_case("romfs") {
            continue;
        }
        if name.eq_ignore_ascii_case("contents") {
            for sub in read_dirs(&path) {
                let sub_name = sub.file_name().to_string_lossy().into_owned();
                let sub_path = sub.path();
                if has_romfs(&sub_path) {
                    out.push(sub_name);
                }
                for sub2 in read_dirs(&sub_path) {
                    let sub2_name = sub2.file_name().to_string_lossy().into_owned();
                    if !sub2_name.eq_ignore_ascii_case("romfs") && has_romfs(&sub2.path()) {
                        out.push(sub2_name);
                    }
                }
            }
        } else {
            if has_romfs(&path) {
                out.push(name);
            }
            for sub in read_dirs(&path) {
                let sub_name = sub.file_name().to_string_lossy().into_owned();
                if !sub_name.eq_ignore_ascii_case("romfs") && has_romfs(&sub.path()) {
                    out.push(sub_name);
                }
            }
        }
    }
    out.sort();
    out.dedup();
    out
}

/// The game's picture for the launcher. An existing icon.jpg or icon.png is preferred; if only yw4.ico
/// exists, extracts the 256x256 PNG packed inside it so WebView2 displays it reliably.
fn ensure_icon_image(dir: &Path) -> Option<PathBuf> {
    for name in ["icon.jpg", "icon.png"] {
        let p = dir.join(name);
        if p.is_file() { return Some(p); }
    }
    let ico = dir.join("yw4.ico");
    if ico.is_file() {
        let png = dir.join("icon.png");
        if let Ok(bytes) = std::fs::read(&ico) {
            if bytes.len() >= 22 {
                let count = u16::from_le_bytes([bytes[4], bytes[5]]) as usize;
                for i in 0..count {
                    let entry = 6 + i * 16;
                    if entry + 16 <= bytes.len() {
                        let size = u32::from_le_bytes([bytes[entry + 8], bytes[entry + 9], bytes[entry + 10], bytes[entry + 11]]) as usize;
                        let offset = u32::from_le_bytes([bytes[entry + 12], bytes[entry + 13], bytes[entry + 14], bytes[entry + 15]]) as usize;
                        if offset + size <= bytes.len() && bytes[offset..].starts_with(b"\x89PNG\r\n\x1a\n") {
                            let _ = std::fs::write(&png, &bytes[offset..offset + size]);
                            return Some(png);
                        }
                    }
                }
            }
        }
        return Some(ico);
    }
    None
}

#[tauri::command]
pub async fn install_info(app: tauri::AppHandle, install: String) -> Info {
    tauri::async_runtime::spawn_blocking(move || {
        let inst = PathBuf::from(&install);
        crate::buildrun::ensure_dlc_extracted(&app, &inst);
        let version = game_version(&exe(&install));
        let has_100 = inst.join("data").join("dlc-romfs").join("100").is_dir();
        let mut dlc_list: Vec<DlcItem> = std::fs::read_dir(inst.join("data").join("dlc")).into_iter().flatten().flatten()
            .map(|e| e.path())
            .filter(|p| p.extension().map_or(false, |x| x.eq_ignore_ascii_case("nsp")))
            .map(|p| {
                let index = crate::check::dlc_index_of(&p);
                DlcItem { index, id: index.map(crate::check::dlc_title_id),
                          file: p.file_name().unwrap_or_default().to_string_lossy().into_owned(),
                          needs_keys: index == Some(100) && !has_100 }
            })
            .collect();
        dlc_list.sort_by(|a, b| (a.index.is_none(), a.index, &a.file).cmp(&(b.index.is_none(), b.index, &b.file)));
        let dlc = dlc_list.iter().filter(|d| d.index.is_some()).count();
        let mut shots: Vec<(std::time::SystemTime, PathBuf)> = std::fs::read_dir(inst.join("screenshots")).into_iter().flatten().flatten()
            .filter(|e| e.path().extension().map_or(false, |x| x.eq_ignore_ascii_case("bmp") || x.eq_ignore_ascii_case("png")))
            .map(|e| (e.metadata().and_then(|m| m.modified()).unwrap_or(std::time::UNIX_EPOCH), e.path()))
            .collect();
        shots.sort_by(|a, b| b.0.cmp(&a.0));
        let shot_count = shots.len();
        let shots = shots.into_iter().take(SHOTS).map(|(_, p)| p.display().to_string()).collect();
        // icon.jpg / icon.png is the game's own picture; installs with only yw4.ico get icon.png extracted from it
        let dir = inst.join("work").join("icon");
        let icon = ensure_icon_image(&dir).map(|p| p.display().to_string());
        let reclaimable = reclaim_dirs(&inst).iter().map(|d| dir_size(d)).sum();
        let played = load_played(&install);
        let disabled_mods = current_state().map(|s| s.disabled_mods).unwrap_or_default();
        Info { version, dlc, dlc_list, mods: mod_names(&inst.join("mods")), disabled_mods, icon, shots, shot_count, reclaimable,
               last_played: played.last_played, play_secs: played.seconds }
    })
    .await
    .unwrap()
}

/// Supplies prod.keys on the DLC tab when the 4++ Upgrade Pack was added after a build that had no keys_dir.
#[tauri::command]
pub async fn set_dlc_keys(app: tauri::AppHandle, install: String, path: String) -> Result<Info, String> {
    let inst = PathBuf::from(&install);
    let p = PathBuf::from(&path);
    let dir = if p.is_file() { p.parent().unwrap_or(&p).to_path_buf() } else { p };
    if !dir.join("prod.keys").is_file() {
        return Err("No prod.keys found in that folder.".into());
    }
    crate::buildrun::save_keys_dir(&inst, &dir).map_err(|e| e.to_string())?;
    let app2 = app.clone();
    let inst2 = inst.clone();
    tauri::async_runtime::spawn_blocking(move || crate::buildrun::ensure_dlc_extracted(&app2, &inst2))
        .await.map_err(|e| e.to_string())?;
    if !inst.join("data").join("dlc-romfs").join("100").is_dir() {
        return Err("Couldn't unpack the 4++ Upgrade Pack with that prod.keys file.".into());
    }
    Ok(install_info(app, install).await)
}

/// Enables or disables an individual mod by name; updates and saves the launcher's state.
#[tauri::command]
pub fn set_mod_enabled(mod_name: String, enabled: bool) -> Result<Vec<String>, String> {
    let mut state = read_state().unwrap_or_default();
    if enabled {
        state.disabled_mods.retain(|m| !m.eq_ignore_ascii_case(&mod_name));
    } else if !state.disabled_mods.iter().any(|m| m.eq_ignore_ascii_case(&mod_name)) {
        state.disabled_mods.push(mod_name);
    }
    save_state(state.clone())?;
    Ok(state.disabled_mods)
}

/// Starts the game: yw4.exe with no arguments reads game.ini next to it. Notes the start, and when the game
/// exits adds the time played and tells the screens ("played"), so the Ready screen's numbers update.
/// Settings (Play): with `mods` off the game starts with none (YW4_MODS=none: an environment variable wins over
/// game.ini's line); `on_play` says what the launcher's window does meanwhile, and "hide" brings it back afterwards.
#[tauri::command]
pub fn play(app: tauri::AppHandle, install: String, mods: Option<bool>, on_play: Option<String>, theme: Option<String>, show_log: Option<bool>, disabled_mods: Option<Vec<String>>) -> Result<(), String> {
    use tauri::{Emitter, Manager};
    crate::buildrun::ensure_dlc_extracted(&app, Path::new(&install));
    let e = exe(&install);
    let mut cmd = Command::new(&e);
    cmd.current_dir(e.parent().unwrap());
    // the game is a console program: its window shows the log live. Off unless Settings asks for it (the log file is written either way)
    #[cfg(windows)]
    if show_log != Some(true) { use std::os::windows::process::CommandExt; cmd.creation_flags(0x0800_0000); }  // CREATE_NO_WINDOW
    if mods == Some(false) {
        cmd.env("YW4_MODS", "none");
    } else {
        let disabled = disabled_mods.or_else(|| read_state().map(|s| s.disabled_mods));
        if let Some(d) = disabled.filter(|d| !d.is_empty()) {
            cmd.env("YW4_MODS_DISABLED", d.join(";"));
        }
    }
    // the game reads the launcher's theme files (decision 0055): its folder, so an installed game finds them
    if let Some(dir) = crate::themes::themes_dir(&app) { cmd.env("YW4_THEMES_DIR", dir); }
    // the theme goes both ways: the game opens in the launcher's theme, and a theme chosen in the game comes back
    // to the launcher when the game closes (the 'game-theme' event)
    if let Some(t) = theme.as_deref().filter(|t| !t.is_empty()) { cmd.env("YW4_THEME", t); }
    let started = std::time::SystemTime::now();
    let mut child = cmd.spawn().map_err(|e| format!("couldn't start the game ({e})"))?;
    let start = now();
    let mut p = load_played(&install);
    p.last_played = Some(start);
    save_played(&p);
    let on_play = on_play.unwrap_or_else(default_on_play);
    let win = app.get_webview_window("main");
    match (on_play.as_str(), &win) {
        ("hide", Some(w)) => { let _ = w.hide(); }
        ("minimise", Some(w)) => { let _ = w.minimize(); }
        ("close", _) => { app.exit(0); return Ok(()); }  // nothing stays to count the play time
        _ => {}
    }
    std::thread::spawn(move || {
        let status = child.wait();
        let mut p = load_played(&install);
        p.seconds += now().saturating_sub(start);
        save_played(&p);
        if on_play == "hide" {
            if let Some(w) = app.get_webview_window("main") { let _ = w.show(); let _ = w.unminimize(); let _ = w.set_focus(); }
        }
        if let Some(id) = game_theme(&install, started) { let _ = app.emit("game-theme", id); }
        let _ = app.emit("played", ());
        // the game ends with 0 when the player quits (or closes the window); anything else is a crash or a stop
        // (3: the recompiled code stopped, kernel.c): offer to report it, with this play's log (the Report a bug form, save_report)
        if let Ok(s) = status {
            if !s.success() {
                let code = s.code().unwrap_or(-1);
                let log = newest_log(&install, started).map(|p| p.to_string_lossy().into_owned()).unwrap_or_default();
                let _ = app.emit("game-crashed", serde_json::json!({ "code": code, "log": log }));
            }
        }
    });
    Ok(())
}

/// A path from game.ini (`key`, relative to work\build or absolute), if game.ini has it.
fn game_ini_path(install: &str, key: &str) -> Option<PathBuf> {
    let build = PathBuf::from(install).join("work").join("build");
    let ini = std::fs::read_to_string(build.join("game.ini"))
        .or_else(|_| std::fs::read_to_string(Path::new(install).join("game").join("game.ini")))
        .or_else(|_| std::fs::read_to_string(Path::new(install).join("game.ini")))
        .ok()?;
    let rel = ini.lines().find_map(|l| l.strip_prefix(key).and_then(|r| r.strip_prefix('=')))?.trim().to_string();
    Some(if Path::new(&rel).is_absolute() { PathBuf::from(&rel) } else { build.join(&rel) })
}

/// The log the game wrote during the play that started at `since` (the newest yw4_*.log in its log folder).
fn newest_log(install: &str, since: std::time::SystemTime) -> Option<PathBuf> {
    let dir = game_ini_path(install, "YW4_LOG_DIR").unwrap_or_else(|| PathBuf::from(install).join("logs"));
    std::fs::read_dir(dir).ok()?
        .filter_map(|e| e.ok())
        .filter(|e| e.file_name().to_string_lossy().starts_with("yw4_"))
        .filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path())))
        .filter(|(t, _)| *t >= since)
        .max_by_key(|(t, _)| *t)
        .map(|(_, p)| p)
}

/// A zip that stores its files (no compression): a report is a few text files and a small save.
fn stored_zip(files: &[(String, Vec<u8>)]) -> Vec<u8> {
    fn crc32(d: &[u8]) -> u32 {
        let mut c = 0xFFFF_FFFFu32;
        for &b in d {
            c ^= b as u32;
            for _ in 0..8 { c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 }; }
        }
        !c
    }
    let (mut out, mut central) = (Vec::new(), Vec::new());
    let le16 = |v: &mut Vec<u8>, x: u16| v.extend_from_slice(&x.to_le_bytes());
    let le32 = |v: &mut Vec<u8>, x: u32| v.extend_from_slice(&x.to_le_bytes());
    for (name, data) in files {
        let (crc, size, at) = (crc32(data), data.len() as u32, out.len() as u32);
        le32(&mut out, 0x0403_4b50); le16(&mut out, 20); le16(&mut out, 0x0800); le16(&mut out, 0);
        le16(&mut out, 0); le16(&mut out, 0x21);  // time, date: 1980-01-01 (not needed)
        le32(&mut out, crc); le32(&mut out, size); le32(&mut out, size);
        le16(&mut out, name.len() as u16); le16(&mut out, 0);
        out.extend_from_slice(name.as_bytes()); out.extend_from_slice(data);
        le32(&mut central, 0x0201_4b50); le16(&mut central, 20); le16(&mut central, 20); le16(&mut central, 0x0800);
        le16(&mut central, 0); le16(&mut central, 0); le16(&mut central, 0x21);
        le32(&mut central, crc); le32(&mut central, size); le32(&mut central, size);
        le16(&mut central, name.len() as u16); for _ in 0..4 { le16(&mut central, 0); }
        le32(&mut central, 0); le32(&mut central, at);
        central.extend_from_slice(name.as_bytes());
    }
    let start = out.len() as u32;
    out.extend_from_slice(&central);
    le32(&mut out, 0x0605_4b50); le16(&mut out, 0); le16(&mut out, 0);
    le16(&mut out, files.len() as u16); le16(&mut out, files.len() as u16);
    le32(&mut out, central.len() as u32); le32(&mut out, start); le16(&mut out, 0);
    out
}

/// Percent-encoding for a link's value.
fn url_value(s: &str) -> String {
    s.bytes().map(|b| if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") }).collect()
}

/// The launcher's Report a bug form (the game overlay's two pages, decision 0053): what's wrong, what happened, how
/// often, and what to attach. After a crash the form comes with that play's log and exit code.
#[derive(Deserialize)]
pub struct ReportForm {
    kind: String,
    often: String,
    text: String,
    /// attach: the game's log (the crash's, else the newest) and settings.ini; the save; the last build's log
    log: bool,
    save: bool,
    #[serde(rename = "buildLog")]
    build_log: bool,
    #[serde(rename = "crashLog", default)]
    crash_log: Option<String>,
    #[serde(rename = "crashCode", default)]
    crash_code: Option<i32>,
}

/// The game log a report means: the crash's, else the newest yw4_*.log.
fn report_log(install: &str, form: &ReportForm) -> Option<PathBuf> {
    form.crash_log.as_deref().filter(|l| !l.is_empty()).map(PathBuf::from)
        .or_else(|| newest_log(install, std::time::UNIX_EPOCH))
}

/// The game's version and GPU, from its log's start lines ("unknown" and "" without a log).
fn version_gpu(log: &str) -> (String, String) {
    let find = |prefix: &str| log.lines().find_map(|l| l.strip_prefix(prefix)).map(|s| s.trim().to_string());
    let version = find("yw4: YW4 Recomp ").unwrap_or_else(|| "unknown".into());
    let gpu = find("yw4: window: Vulkan on ").map(|g| g.split(" (API").next().unwrap_or("").to_string()).unwrap_or_default();
    (version, gpu)
}

/// GitHub's bug form (.github/ISSUE_TEMPLATE/bug.yml) with the answers filled in, as the game's (bugreport.c).
fn report_url(form: &ReportForm, version: &str, gpu: &str) -> String {
    let kind = if form.kind.is_empty() { "Bug" } else { form.kind.as_str() };
    let mut what = form.text.clone();
    if let Some(code) = form.crash_code {
        what = format!("The game closed unexpectedly (exit code {code}).{}{what}", if what.is_empty() { "" } else { "\n\n" });
    }
    let start: String = form.text.chars().take(80).collect();
    let title = if start.is_empty() { kind.to_string() } else { format!("{kind}: {start}") };
    let what: String = what.chars().take(4000).collect();
    format!("https://github.com/ILFforever/YW4Recomp/issues/new?template=bug.yml&title={}&what={}&often={}&version={}&pc={}",
        url_value(&title), url_value(&what), url_value(&form.often), url_value(version), url_value(gpu))
}

/// `secs` (Unix time) as UTC "2026-11-10 14:35:12" and, for file names, "2026-11-10_143512" (the game's names are
/// the same shape, in local time).
fn utc_stamp(secs: u64) -> (String, String) {
    let (days, rest) = ((secs / 86400) as i64, secs % 86400);
    let (h, m, s) = (rest / 3600, rest % 3600 / 60, rest % 60);
    // days since 1970-01-01 to a date (Howard Hinnant's civil_from_days)
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let mo = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if mo <= 2 { 1 } else { 0 };
    (format!("{y:04}-{mo:02}-{d:02} {h:02}:{m:02}:{s:02}"), format!("{y:04}-{mo:02}-{d:02}_{h:02}{m:02}{s:02}"))
}

fn open_url(url: &str) {
    if std::env::var("YW4_REPORT_OPEN").as_deref() != Ok("0") { let _ = Command::new("explorer").arg(url).spawn(); }
}

/// Every file under `dir`, named `prefix/<path inside>` (the save folder, a few files).
fn dir_files(dir: &Path, prefix: &str, out: &mut Vec<(String, Vec<u8>)>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let name = format!("{prefix}/{}", e.file_name().to_string_lossy());
        let p = e.path();
        if p.is_dir() { dir_files(&p, &name, out); } else if let Ok(d) = std::fs::read(&p) { out.push((name, d)); }
    }
}

/// Report a bug (Save only, Send to GitHub): a zip in the install's reports folder with report.txt (the answers,
/// version, GPU, launcher version) and what the player ticked; with `send`, GitHub's bug form opened with the
/// answers filled in. Nothing is uploaded: the player drags the zip in. Returns the zip's path.
#[tauri::command]
pub fn save_report(install: String, saves: String, form: ReportForm, send: bool) -> Result<String, String> {
    let log_path = report_log(&install, &form);
    let log_bytes = log_path.as_ref().and_then(|p| std::fs::read(p).ok()).unwrap_or_default();
    let log_text = String::from_utf8_lossy(&log_bytes);
    let (version, gpu) = version_gpu(&log_text);
    let dash = |s: &str| if s.is_empty() { "-".to_string() } else { s.to_string() };
    let mut report = format!("YW4 Recomp bug report (from the launcher)\r\n\r\nWhat's wrong: {}\r\nHow often: {}\r\nWhat happened:\r\n{}\r\n\r\n",
        dash(&form.kind), dash(&form.often), form.text.replace('\n', "\r\n"));
    let (when, stamp) = utc_stamp(now());
    report += &format!("Version: {version}\r\nGPU: {}\r\nLauncher: {}\r\nTime: {when} UTC\r\n", dash(&gpu), env!("CARGO_PKG_VERSION"));
    let mut files: Vec<(String, Vec<u8>)> = Vec::new();
    let mut attached = Vec::new();
    if form.log && !log_bytes.is_empty() {
        files.push(("log.txt".into(), log_bytes.clone()));
        attached.push("log");
        let settings = game_ini_path(&install, "YW4_SETTINGS_FILE").filter(|p| p.is_file())
            .unwrap_or_else(|| saves_place(Path::new(&install), &saves).1);
        if let Ok(s) = std::fs::read(settings) { files.push(("settings.ini".into(), s)); attached.push("settings"); }
    }
    if form.save {
        let n = files.len();
        let save = game_ini_path(&install, "YW4_SAVE").filter(|p| p.is_dir())
            .unwrap_or_else(|| saves_place(Path::new(&install), &saves).0);
        dir_files(&save, "save", &mut files);
        if files.len() > n { attached.push("save"); }
    }
    if form.build_log {
        if let Ok(b) = std::fs::read(Path::new(&install).join("logs").join("build_last.log")) {
            files.push(("build_log.txt".into(), b));
            attached.push("build log");
        }
    }
    report += &format!("Attached: {}\r\n", if attached.is_empty() { "-".to_string() } else { attached.join(", ") });
    if let Some(code) = form.crash_code {
        let last: Vec<&str> = log_text.lines().rev().take(12).collect::<Vec<_>>().into_iter().rev().collect();
        report += &format!("\r\nThe game closed unexpectedly (exit code {code}). The log's last lines:\r\n{}\r\n", last.join("\r\n"));
    }
    files.insert(0, ("report.txt".into(), report.into_bytes()));
    let dir = PathBuf::from(&install).join("reports");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let zip = dir.join(format!("yw4_report_{stamp}.zip"));
    std::fs::write(&zip, stored_zip(&files)).map_err(|e| format!("couldn't save the report ({e})"))?;
    if send { open_url(&report_url(&form, &version, &gpu)); }
    Ok(zip.to_string_lossy().into_owned())
}

/// The saved page's Open GitHub (again): the same filled-in form.
#[tauri::command]
pub fn open_report_github(install: String, form: ReportForm) {
    let log = report_log(&install, &form).and_then(|p| std::fs::read(p).ok()).unwrap_or_default();
    let (version, gpu) = version_gpu(&String::from_utf8_lossy(&log));
    open_url(&report_url(&form, &version, &gpu));
}

/// The saved page's Show the zip: Explorer with the zip selected (only a zip in the install's reports folder).
#[tauri::command]
pub fn show_report(install: String, zip: String) -> Result<(), String> {
    let p = PathBuf::from(&zip);
    if p.parent() != Some(Path::new(&install).join("reports").as_path()) || !p.is_file() {
        return Err("not a report".into());
    }
    Command::new("explorer").arg(format!("/select,{}", p.display())).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// The theme the game saved (its settings.ini line YW4_THEME=<id>), if the game's settings file can be found: the
/// game.ini next to it says where (YW4_SETTINGS_FILE, relative to work\build or absolute in AppData).
/// Only a file written since `since` counts: the game writes settings.ini only when a setting changes, so after a
/// play with no change its line is still an older play's theme, and taking it would undo the launcher's choice.
fn game_theme(install: &str, since: std::time::SystemTime) -> Option<String> {
    let build = PathBuf::from(install).join("work").join("build");
    let ini = std::fs::read_to_string(build.join("game.ini")).ok()?;
    let rel = ini.lines().find_map(|l| l.strip_prefix("YW4_SETTINGS_FILE="))?.trim().to_string();
    let path = if Path::new(&rel).is_absolute() { PathBuf::from(&rel) } else { build.join(&rel) };
    if std::fs::metadata(&path).ok()?.modified().ok()? < since { return None; }
    let settings = std::fs::read_to_string(path).ok()?;
    let id = settings.lines().find_map(|l| l.trim().strip_prefix("YW4_THEME="))?.trim().to_string();
    (!id.is_empty() && id.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')).then_some(id)
}

/// Opens one of the install's folders in Explorer (a fixed set: nothing else can be named).
#[tauri::command]
pub fn open_folder(install: String, which: String, saves: String) -> Result<(), String> {
    let inst = PathBuf::from(&install);
    let dir = match which.as_str() {
        "mods" => {
            let m = inst.join("mods");
            let _ = std::fs::create_dir_all(m.join("contents"));
            m
        }
        "screenshots" | "logs" | "reports" => inst.join(&which),
        "install" => inst.clone(),
        "dlc" => inst.join("data").join("dlc"),
        "save" if saves == "appdata" => PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp").join("save"),
        "save" => inst.join("save"),
        _ => return Err("unknown folder".into()),
    };
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Command::new("explorer").arg(&dir).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Opens one screenshot in the player's picture viewer: only a file directly in the install's screenshots folder.
#[tauri::command]
pub fn open_shot(install: String, path: String) -> Result<(), String> {
    let p = PathBuf::from(&path);
    let dir = Path::new(&install).join("screenshots");
    if p.parent() != Some(dir.as_path()) || !p.is_file() {
        return Err("not a screenshot".into());
    }
    Command::new("explorer").arg(&p).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Deletes what only a rebuild needs; returns the bytes freed. The game keeps working.
#[tauri::command]
pub async fn free_space(install: String) -> u64 {
    tauri::async_runtime::spawn_blocking(move || {
        let mut freed = 0;
        for d in reclaim_dirs(Path::new(&install)) {
            let n = dir_size(&d);
            if std::fs::remove_dir_all(&d).is_ok() { freed += n; }
        }
        freed
    })
    .await
    .unwrap_or(0)
}

#[derive(Serialize)]
pub struct Original { path: String, size: u64 }

/// The player's own files the install no longer needs: the NSP/XCI (or extracted folders) and the add-on NSPs
/// that were checked and copied. Never the keys, never anything else in those folders.
fn originals_of(s: &State) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let get = |k: &str| s.paths.get(k).filter(|p| !p.is_empty()).map(PathBuf::from);
    let keys = ["base", "update", "exefs", "romfs"];
    for k in if s.mode == "folders" { &keys[2..] } else { &keys[..2] } {
        if let Some(p) = get(k).filter(|p| p.exists()) { out.push(p); }
    }
    if let Some(dlc) = get("dlc") {
        for p in std::fs::read_dir(dlc).into_iter().flatten().flatten().map(|e| e.path()) {
            if p.extension().map_or(false, |x| x.eq_ignore_ascii_case("nsp")) && crate::check::dlc_index_of(&p).is_some() {
                out.push(p);
            }
        }
    }
    out
}

#[tauri::command]
pub async fn originals(state: State) -> Vec<Original> {
    tauri::async_runtime::spawn_blocking(move || {
        originals_of(&state).into_iter().map(|p| Original { size: dir_size(&p), path: p.display().to_string() }).collect()
    })
    .await
    .unwrap_or_default()
}

#[tauri::command]
pub async fn delete_originals(state: State) -> Result<u64, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let mut freed = 0;
        for p in originals_of(&state) {
            let n = dir_size(&p);
            let r = if p.is_dir() { std::fs::remove_dir_all(&p) } else { std::fs::remove_file(&p) };
            r.map_err(|e| format!("couldn't delete {} ({e})", p.display()))?;
            freed += n;
        }
        Ok(freed)
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Desktop and Start menu shortcuts to the launcher, with the game's icon when the build made one.
#[tauri::command]
pub fn make_shortcuts(state: State) -> Result<(), String> {
    let target = std::env::current_exe().map_err(|e| e.to_string())?;
    let icon = Path::new(&state.install).join("work").join("icon").join("yw4.ico");
    let icon = if icon.is_file() { icon } else { target.clone() };
    let mut places = Vec::new();
    if state.desktop { places.push("[Environment]::GetFolderPath('Desktop')"); }
    if state.start_menu { places.push("[Environment]::GetFolderPath('Programs')"); }
    // the ones switched off go (Settings): only our own shortcut's name, nothing else in those folders
    for (on, place) in [(state.desktop, "[Environment]::GetFolderPath('Desktop')"),
                        (state.start_menu, "[Environment]::GetFolderPath('Programs')")] {
        if !on {
            let script = format!("Remove-Item -LiteralPath (Join-Path ({place}) 'Yo-kai Watch 4 Recomp.lnk') -ErrorAction SilentlyContinue");
            let _ = hidden(Command::new("powershell").args(["-NoProfile", "-NonInteractive", "-Command", &script])).status();
        }
    }
    for place in places {
        // ({place}): a .NET call given straight to a command is read as text, not run (Join-Path then fails and no
        // shortcut was ever made: found 2026-11-04 in the first full Home -> Ready test)
        let script = format!(
            "$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path ({place}) 'Yo-kai Watch 4 Recomp.lnk')); \
             $s.TargetPath=$env:YW4_LNK_TARGET; $s.WorkingDirectory=(Split-Path $env:YW4_LNK_TARGET); \
             $s.IconLocation=$env:YW4_LNK_ICON; $s.Save()");
        let ok = hidden(Command::new("powershell").args(["-NoProfile", "-NonInteractive", "-Command", &script])
            .env("YW4_LNK_TARGET", &target).env("YW4_LNK_ICON", &icon)).status().map(|s| s.success()).unwrap_or(false);
        if !ok { return Err("couldn't make a shortcut".into()); }
    }
    Ok(())
}

/// Where the saves and settings.ini live for `saves` ("install" or "appdata").
fn saves_place(install: &Path, saves: &str) -> (PathBuf, PathBuf) {
    if saves == "appdata" {
        let base = PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp");
        (base.join("save"), base.join("settings.ini"))
    } else {
        (install.join("save"), install.join("settings.ini"))
    }
}

fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for e in std::fs::read_dir(from)?.flatten() {
        let (s, d) = (e.path(), to.join(e.file_name()));
        if e.file_type()?.is_dir() { copy_dir(&s, &d)?; } else { std::fs::copy(&s, &d)?; }
    }
    Ok(())
}

fn game_running() -> bool {
    hidden(Command::new("tasklist").args(["/FI", "IMAGENAME eq yw4.exe", "/NH"])).output()
        .map(|o| String::from_utf8_lossy(&o.stdout).to_lowercase().contains("yw4.exe")).unwrap_or(false)
}

/// This install's own game is running: Windows won't open a running program for writing (5 or 32), and a build
/// would fail at its very end when it can't replace it. By file, not by name: another copy of the game elsewhere
/// is no reason to refuse. Opening it this way changes nothing in the file.
pub fn exe_in_use(install: &str) -> bool {
    match std::fs::OpenOptions::new().write(true).open(exe(install)) {
        Ok(_) => false,
        Err(e) => matches!(e.raw_os_error(), Some(5) | Some(32)),
    }
}

/// Settings: moves the saves and settings.ini to the other place and points game.ini there. Refuses while the
/// game runs, and when the other place already holds saves (nothing is ever overwritten). Copy first, then delete,
/// so a failure leaves the saves where they were.
#[tauri::command]
pub fn move_saves(install: String, to: String) -> Result<(), String> {
    if to != "install" && to != "appdata" { return Err("unknown place".into()); }
    if game_running() { return Err("Close the game first.".into()); }
    let inst = PathBuf::from(&install);
    let from = if to == "appdata" { "install" } else { "appdata" };
    let ((src_save, src_ini), (dst_save, dst_ini)) = (saves_place(&inst, from), saves_place(&inst, &to));
    let has_files = |d: &Path| std::fs::read_dir(d).map(|mut r| r.next().is_some()).unwrap_or(false);
    if has_files(&dst_save) || dst_ini.is_file() {
        return Err(format!("There are already saves in {}. Move or delete them first.", dst_save.parent().unwrap().display()));
    }
    if src_save.is_dir() { copy_dir(&src_save, &dst_save).map_err(|e| format!("copying the saves: {e}"))?; }
    else { std::fs::create_dir_all(&dst_save).map_err(|e| e.to_string())?; }
    if src_ini.is_file() { std::fs::copy(&src_ini, &dst_ini).map_err(|e| format!("copying settings.ini: {e}"))?; }
    // game.ini: the two lines that say where (decision 0050); relative inside the install, absolute in AppData
    let ini_path = inst.join("work").join("build").join("game.ini");
    let (save_v, set_v) = if to == "appdata" {
        (dst_save.display().to_string(), dst_ini.display().to_string())
    } else {
        ("../../save".to_string(), "../../settings.ini".to_string())
    };
    if let Ok(ini) = std::fs::read_to_string(&ini_path) {
        let out: Vec<String> = ini.lines().map(|l| {
            if l.starts_with("YW4_SAVE=") { format!("YW4_SAVE={save_v}") }
            else if l.starts_with("YW4_SETTINGS_FILE=") { format!("YW4_SETTINGS_FILE={set_v}") }
            else { l.to_string() }
        }).collect();
        std::fs::write(&ini_path, out.join("\n") + "\n").map_err(|e| format!("game.ini: {e}"))?;
    }
    let _ = std::fs::remove_dir_all(&src_save);
    let _ = std::fs::remove_file(&src_ini);
    Ok(())
}

/// Settings: the game's own "full screen at start" (YW4_WINDOW in its settings.ini, the same line F11 and the
/// WINDOW MODE row save), read and written here so the two never disagree.
#[tauri::command]
pub fn game_full_screen(install: String, saves: String) -> bool {
    let (_, ini) = saves_place(Path::new(&install), &saves);
    std::fs::read_to_string(ini).map(|s| s.lines().any(|l| l.trim() == "YW4_WINDOW=fullscreen")).unwrap_or(false)
}

#[tauri::command]
pub fn set_game_full_screen(install: String, saves: String, full: bool) -> Result<(), String> {
    if game_running() { return Err("Close the game first.".into()); }
    let (_, ini) = saves_place(Path::new(&install), &saves);
    let line = format!("YW4_WINDOW={}", if full { "fullscreen" } else { "windowed" });
    let old = std::fs::read_to_string(&ini).unwrap_or_default();
    let mut lines: Vec<String> = old.lines().filter(|l| !l.starts_with("YW4_WINDOW=")).map(String::from).collect();
    lines.push(line);
    if let Some(dir) = ini.parent() { std::fs::create_dir_all(dir).map_err(|e| e.to_string())?; }
    std::fs::write(&ini, lines.join("\n") + "\n").map_err(|e| e.to_string())
}

// ---- Uninstall (Settings). Only the launcher's own parts of the install folder are deleted, by name, and only in
// a folder whose config.toml the launcher wrote; the folder itself goes only if nothing else is left in it.

const BUILT_PARTS: [&str; 5] = ["data", "work", "builder", "logs", "config.toml"];

fn ours(inst: &Path) -> bool {
    std::fs::read_to_string(inst.join("config.toml"))
        .map(|s| s.starts_with("# Written by the YW4 Recomp launcher")).unwrap_or(false)
}

fn uninstaller() -> Option<PathBuf> {
    std::env::current_exe().ok()?.parent().map(|d| d.join("uninstall.exe")).filter(|p| p.is_file())
}

#[derive(Serialize)]
pub struct UninstallInfo { game: u64, saves: u64, shots: u64, mods: u64, launcher: bool }

/// What Uninstall would delete, for its popup: sizes of the game files and build, the saves (+ settings.ini),
/// screenshots and mods; whether this launcher has a Windows uninstaller (the installer's; the portable zip's not).
#[tauri::command]
pub async fn uninstall_info(install: String, saves: String) -> UninstallInfo {
    tauri::async_runtime::spawn_blocking(move || {
        let inst = PathBuf::from(&install);
        let (save, ini) = saves_place(&inst, &saves);
        UninstallInfo {
            game: BUILT_PARTS.iter().map(|p| dir_size(&inst.join(p))).sum(),
            saves: dir_size(&save) + dir_size(&ini),
            shots: dir_size(&inst.join("screenshots")),
            mods: dir_size(&inst.join("mods")),
            launcher: uninstaller().is_some(),
        }
    }).await.unwrap_or(UninstallInfo { game: 0, saves: 0, shots: 0, mods: 0, launcher: false })
}

fn remove(p: &Path) {
    let _ = if p.is_dir() { std::fs::remove_dir_all(p) } else { std::fs::remove_file(p) };
}

/// Deletes the install (what the popup listed, keeping what the player ticked), the shortcuts and the launcher's
/// own records; with `launcher`, then runs its Windows uninstaller and closes.
#[tauri::command]
pub async fn uninstall(app: tauri::AppHandle, install: String, saves: String, keep_saves: bool, keep_shots: bool,
                       keep_mods: bool, launcher: bool) -> Result<(), String> {
    if game_running() { return Err("Close the game first.".into()); }
    let inst = PathBuf::from(&install);
    if !ours(&inst) { return Err(format!("{} doesn't look like a game folder this launcher made.", inst.display())); }
    tauri::async_runtime::spawn_blocking(move || {
        for p in BUILT_PARTS { remove(&inst.join(p)); }
        if !keep_shots { remove(&inst.join("screenshots")); }
        if !keep_mods { remove(&inst.join("mods")); }
        if !keep_saves {
            let (save, ini) = saves_place(&inst, &saves);
            remove(&save);
            remove(&ini);
        }
        let _ = std::fs::remove_dir(&inst);  // only if empty: what was kept, or anything else, stays
        make_shortcuts(State { desktop: false, start_menu: false, install: install.clone(), ..Default::default() })
    }).await.map_err(|e| e.to_string())??;
    // the launcher's own records: its state, play time, update cache; its AppData folder if that leaves it empty
    let state = state_path();
    remove(&state);
    remove(&played_path());
    let appdata = PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp");
    remove(&appdata.join("releases.json"));
    let _ = std::fs::remove_dir(&appdata);
    if launcher {
        if let Some(u) = uninstaller() {
            // after we close: it removes this program's files and its entry in Windows' installed apps
            hidden(Command::new(u).arg("/S")).spawn().map_err(|e| e.to_string())?;
            app.exit(0);
        }
    }
    Ok(())
}

/// `yw4.exe --version`'s line, or "" if it doesn't answer within 5 s. A broken install (a DLL missing, a half
/// copied file) must never hang the screen; Windows' own error box for it is turned off in main.rs.
fn game_version(exe: &Path) -> String {
    crate::quiet_errors();  // set again here: the window's start-up resets it (seen 2026-11-05)
    let Ok(mut child) = hidden(Command::new(exe).arg("--version")).stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null()).spawn() else { return String::new() };
    let started = std::time::Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) if started.elapsed() < std::time::Duration::from_secs(5) => std::thread::sleep(std::time::Duration::from_millis(50)),
            _ => { let _ = child.kill(); let _ = child.wait(); return String::new(); }
        }
    }
    let mut out = String::new();
    if let Some(mut s) = child.stdout.take() { use std::io::Read; let _ = s.read_to_string(&mut out); }
    out.trim().to_string()
}

/// Verifies an install folder, writes launcher.json, and returns the resulting state.
pub fn locate_installed_game_path(raw_path: PathBuf) -> Result<State, String> {
    let norm_path = if let Ok(c) = raw_path.canonicalize() {
        normalize_path(c)
    } else {
        raw_path.clone()
    };

    let Some(install_dir) = resolve_install_dir(&norm_path).or_else(|| resolve_install_dir(&raw_path)) else {
        return Err(format!("No Yo-kai Watch 4 game found in '{}'.", raw_path.display()));
    };

    let install_str = install_dir.display().to_string();
    let exe_file = exe(&install_str);
    if !exe_file.is_file() {
        return Err(format!("No yw4.exe found in '{}'.", install_str));
    }

    let ver = game_version(&exe_file);
    if !ver.is_empty() && !ver.to_lowercase().contains("yw4") && !ver.to_lowercase().contains("recomp") {
        return Err(format!("'{}' is not Yo-kai Watch 4 Recomp.", exe_file.display()));
    }

    // Build or update state
    let mut state = read_state().unwrap_or_default();
    let prev_install = std::mem::replace(&mut state.install, install_str.clone());

    if prev_install != install_str {
        state.install = install_str.clone();
        state.originals_offered = true;
        state.free_up = false;
        state.desktop = false;
        state.start_menu = false;
        state.paths.clear();
        state.disabled_mods.clear();
    }

    // Read config.toml if present
    let cfg_path = install_dir.join("config.toml");
    if let Ok(content) = std::fs::read_to_string(&cfg_path) {
        if let Ok(t) = content.parse::<toml::Value>() {
            if let Some(input) = t.get("input").and_then(|i| i.as_table()) {
                if let Some(b) = input.get("base_nsp").and_then(|v| v.as_str()) {
                    state.paths.insert("base".into(), b.into());
                    state.mode = "files".into();
                }
                if let Some(u) = input.get("update_nsp").and_then(|v| v.as_str()) {
                    state.paths.insert("update".into(), u.into());
                }
                if let Some(k) = input.get("keys_dir").and_then(|v| v.as_str()) {
                    state.paths.insert("keys".into(), k.into());
                }
                if let Some(d) = input.get("dlc_dir").and_then(|v| v.as_str()) {
                    state.paths.insert("dlc".into(), d.into());
                }
                if let Some(e) = input.get("exefs_dir").and_then(|v| v.as_str()) {
                    state.paths.insert("exefs".into(), e.into());
                }
                if let Some(r) = input.get("romfs_dir").and_then(|v| v.as_str()) {
                    state.paths.insert("romfs".into(), r.into());
                }
            }
        }
    }

    if state.mode.is_empty() {
        state.mode = "folders".into();
    }
    if !state.paths.contains_key("exefs") && install_dir.join("data").join("exefs").is_dir() {
        state.paths.insert("exefs".into(), install_dir.join("data").join("exefs").display().to_string());
    }
    if !state.paths.contains_key("romfs") && install_dir.join("data").join("romfs").is_dir() {
        state.paths.insert("romfs".into(), install_dir.join("data").join("romfs").display().to_string());
    }
    if !state.paths.contains_key("dlc") && install_dir.join("data").join("dlc").is_dir() {
        state.paths.insert("dlc".into(), install_dir.join("data").join("dlc").display().to_string());
    }

    // Determine saves place
    if state.saves.is_empty() {
        state.saves = "install".into();
        if let Some(ini_save) = game_ini_path(&install_str, "YW4_SAVE") {
            let s = ini_save.to_string_lossy();
            if s.contains("APPDATA") || s.contains("AppData") || (ini_save.is_absolute() && !ini_save.starts_with(&install_dir)) {
                state.saves = "appdata".into();
            }
        }
    }

    state.fsr = true;
    state.dlss = install_dir.join("work").join("build").join("nvngx_dlss.dll").is_file()
        || install_dir.join("game").join("nvngx_dlss.dll").is_file()
        || install_dir.join("nvngx_dlss.dll").is_file();

    let dir = install_dir.join("work").join("icon");
    state.icon = ensure_icon_image(&dir)
        .or_else(|| ensure_icon_image(&install_dir.join("icon")))
        .map(|p| p.display().to_string());

    // Ensure common folders exist
    let _ = std::fs::create_dir_all(install_dir.join("mods").join("contents"));
    let _ = std::fs::create_dir_all(install_dir.join("screenshots"));
    let _ = std::fs::create_dir_all(install_dir.join("logs"));
    let _ = std::fs::create_dir_all(install_dir.join("data").join("dlc"));

    // Ensure game.ini exists next to yw4.exe
    if let Some(parent) = exe_file.parent() {
        let ini_path = parent.join("game.ini");
        if !ini_path.is_file() {
            let (save, settings) = if state.saves == "appdata" {
                ("%APPDATA%/YW4 Recomp/save", "%APPDATA%/YW4 Recomp/settings.ini")
            } else {
                ("../../save", "../../settings.ini")
            };
            let text = format!(
                "# Written by the YW4 Recomp launcher: how yw4.exe runs on its own (decision 0050).\n\
                 YW4_EXEFS=../exefs\nYW4_ROMFS=../../data/romfs\nYW4_SAVE={save}\nYW4_SETTINGS_FILE={settings}\n\
                 YW4_MOUSE=1\nYW4_AUDIO=1\nYW4_RUMBLE=1\nYW4_RECORD=0\n"
            );
            let _ = std::fs::write(&ini_path, text);
        }
    }

    save_state(state.clone())?;
    Ok(state)
}

fn locate_game_sync(app: &tauri::AppHandle, target: Option<String>) -> Result<State, String> {
    let chosen_path = match target {
        Some(p) if !p.trim().is_empty() => Some(PathBuf::from(p.trim())),
        _ => {
            use tauri_plugin_dialog::DialogExt;
            app.dialog()
                .file()
                .set_title("Locate installed game")
                .blocking_pick_folder()
                .and_then(|fp| fp.as_path().map(|p| p.to_path_buf()))
        }
    };
    let Some(raw_path) = chosen_path else {
        return Err("No folder chosen.".into());
    };
    locate_installed_game_path(raw_path)
}

async fn locate_game_impl(app: tauri::AppHandle, target: Option<String>) -> Result<State, String> {
    tauri::async_runtime::spawn_blocking(move || {
        locate_game_sync(&app, target)
    })
    .await
    .map_err(|e| e.to_string())?
}

/// Locates an already installed game on disk, verifies it, saves it to launcher.json, and returns the state.
/// If `path` is omitted (or empty), prompts the user to select the game folder via a folder picker dialog.
#[tauri::command]
pub async fn locate_installed_game(
    app: tauri::AppHandle,
    path: Option<String>,
    install: Option<String>,
) -> Result<State, String> {
    locate_game_impl(app, path.or(install)).await
}

/// Alias for `locate_installed_game`.
#[tauri::command]
pub async fn locate_game(
    app: tauri::AppHandle,
    path: Option<String>,
    install: Option<String>,
) -> Result<State, String> {
    locate_game_impl(app, path.or(install)).await
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The same folder, however each side spells it: GitHub's Windows machines give the temp folder by its short
    /// name (C:\Users\RUNNER~1\...) where the launcher answers with the long one (C:\Users\runneradmin\...).
    fn same_folder(a: &str, b: &Path) -> bool {
        match (std::fs::canonicalize(a), std::fs::canonicalize(b)) {
            (Ok(a), Ok(b)) => a == b,
            _ => false,
        }
    }

    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn test_mod_names_variations() {
        let temp = std::env::temp_dir().join("yw4_test_mods_variations");
        let _ = std::fs::remove_dir_all(&temp);

        std::fs::create_dir_all(temp.join("contents").join("010086c00af7c000").join("ModA").join("romfs")).unwrap();
        std::fs::create_dir_all(temp.join("contents").join("ModB").join("romfs")).unwrap();
        std::fs::create_dir_all(temp.join("contents").join("010086c00af7c000").join("romfs")).unwrap();
        std::fs::create_dir_all(temp.join("ModC").join("romfs")).unwrap();

        let mods = mod_names(&temp);
        assert_eq!(mods, vec!["010086c00af7c000", "ModA", "ModB", "ModC"]);

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn test_utc_stamp() {
        assert_eq!(utc_stamp(0).0, "1970-01-01 00:00:00");
        assert_eq!(utc_stamp(951782400).0, "2000-02-29 00:00:00");  // a leap day
        assert_eq!(utc_stamp(1794261793), ("2026-11-09 22:03:13".into(), "2026-11-09_220313".into()));
    }

    #[test]
    fn test_set_mod_enabled() {
        let _guard = TEST_LOCK.lock().unwrap();
        let temp_state = std::env::temp_dir().join("yw4_test_state_mod_enabled.json");
        let _ = std::fs::remove_file(&temp_state);
        std::env::set_var("YW4_LAUNCHER_STATE", &temp_state);

        let res = set_mod_enabled("Spanish Patch".into(), false).unwrap();
        assert_eq!(res, vec!["Spanish Patch"]);

        let res = set_mod_enabled("English Patch".into(), false).unwrap();
        assert_eq!(res, vec!["Spanish Patch", "English Patch"]);

        let res = set_mod_enabled("spanish patch".into(), true).unwrap();
        assert_eq!(res, vec!["English Patch"]);

        let _ = std::fs::remove_file(&temp_state);
        std::env::remove_var("YW4_LAUNCHER_STATE");
    }

    #[test]
    fn test_resolve_install_dir_work_build() {
        let temp = std::env::temp_dir().join("yw4_test_resolve_wb");
        let _ = std::fs::remove_dir_all(&temp);

        let wb = temp.join("work").join("build");
        std::fs::create_dir_all(&wb).unwrap();
        std::fs::create_dir_all(temp.join("data")).unwrap();
        std::fs::write(wb.join("yw4.exe"), b"").unwrap();

        assert_eq!(resolve_install_dir(&temp), Some(temp.clone()));
        assert_eq!(resolve_install_dir(&wb), Some(temp.clone()));
        assert_eq!(resolve_install_dir(&wb.join("yw4.exe")), Some(temp.clone()));
        assert_eq!(resolve_install_dir(&temp.join("data")), Some(temp.clone()));
        assert_eq!(exe(&temp.display().to_string()), wb.join("yw4.exe"));

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn test_resolve_install_dir_game_folder() {
        let temp = std::env::temp_dir().join("yw4_test_resolve_game");
        let _ = std::fs::remove_dir_all(&temp);

        let gm = temp.join("game");
        std::fs::create_dir_all(&gm).unwrap();
        std::fs::write(gm.join("yw4.exe"), b"").unwrap();

        assert_eq!(resolve_install_dir(&temp), Some(temp.clone()));
        assert_eq!(resolve_install_dir(&gm), Some(temp.clone()));
        assert_eq!(resolve_install_dir(&gm.join("yw4.exe")), Some(temp.clone()));
        assert_eq!(exe(&temp.display().to_string()), gm.join("yw4.exe"));

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn test_locate_installed_game_path_success_and_fail() {
        let _guard = TEST_LOCK.lock().unwrap();
        let temp = std::env::temp_dir().join("yw4_test_locate_game");
        let _ = std::fs::remove_dir_all(&temp);

        let temp_state = std::env::temp_dir().join("yw4_test_locate_state.json");
        let _ = std::fs::remove_file(&temp_state);
        std::env::set_var("YW4_LAUNCHER_STATE", &temp_state);

        // 1. Empty folder fails
        std::fs::create_dir_all(&temp).unwrap();
        let fail = locate_installed_game_path(temp.clone());
        assert!(fail.is_err());

        // 2. Mock valid install
        let wb = temp.join("work").join("build");
        std::fs::create_dir_all(&wb).unwrap();
        std::fs::write(wb.join("yw4.exe"), b"").unwrap();
        std::fs::create_dir_all(temp.join("data").join("exefs")).unwrap();
        std::fs::create_dir_all(temp.join("data").join("romfs")).unwrap();
        std::fs::write(temp.join("config.toml"), "[input]\nbase_nsp = \"C:\\\\dump\\\\base.nsp\"\n").unwrap();

        let res = locate_installed_game_path(temp.clone()).unwrap();
        assert!(same_folder(&res.install, &temp), "{} is not {}", res.install, temp.display());
        assert_eq!(res.mode, "files");
        assert_eq!(res.paths.get("base").map(|s| s.as_str()), Some("C:\\dump\\base.nsp"));
        assert!(res.originals_offered);

        // State was saved and can be loaded
        let loaded = current_state().expect("state should be loadable");
        assert!(same_folder(&loaded.install, &temp), "{} is not {}", loaded.install, temp.display());

        // game.ini was created next to yw4.exe
        assert!(wb.join("game.ini").is_file());

        // Locating from subfolder work/build also works
        let res_sub = locate_installed_game_path(wb).unwrap();
        assert!(same_folder(&res_sub.install, &temp), "{} is not {}", res_sub.install, temp.display());

        // Locating by selecting yw4.exe file directly also works
        let res_exe = locate_installed_game_path(temp.join("work").join("build").join("yw4.exe")).unwrap();
        assert!(same_folder(&res_exe.install, &temp), "{} is not {}", res_exe.install, temp.display());

        let _ = std::fs::remove_dir_all(&temp);
        let _ = std::fs::remove_file(&temp_state);
        std::env::remove_var("YW4_LAUNCHER_STATE");
    }

    #[test]
    fn test_locate_installed_game_current_repo() {
        if !Path::new("work").join("build").join("yw4.exe").is_file() {
            return;
        }
        let _guard = TEST_LOCK.lock().unwrap();
        let temp_state = std::env::temp_dir().join("yw4_test_locate_state_repo.json");
        let _ = std::fs::remove_file(&temp_state);
        std::env::set_var("YW4_LAUNCHER_STATE", &temp_state);

        let res = locate_installed_game_path(PathBuf::from(".")).unwrap();
        let cur_dir = std::env::current_dir().unwrap();
        assert_eq!(res.install, cur_dir.display().to_string());
        assert_eq!(res.saves, "install");

        let _ = std::fs::remove_file(&temp_state);
        std::env::remove_var("YW4_LAUNCHER_STATE");
    }

    #[test]
    fn test_locate_installed_game_flat_and_appdata() {
        let _guard = TEST_LOCK.lock().unwrap();
        let temp = std::env::temp_dir().join("yw4_test_locate_flat");
        let _ = std::fs::remove_dir_all(&temp);

        let temp_state = std::env::temp_dir().join("yw4_test_locate_state_flat.json");
        let _ = std::fs::remove_file(&temp_state);
        std::env::set_var("YW4_LAUNCHER_STATE", &temp_state);

        std::fs::create_dir_all(&temp).unwrap();
        std::fs::write(temp.join("yw4.exe"), b"").unwrap();
        std::fs::write(temp.join("game.ini"), "YW4_SAVE=%APPDATA%/YW4 Recomp/save\nYW4_SETTINGS_FILE=%APPDATA%/YW4 Recomp/settings.ini\n").unwrap();

        let res = locate_installed_game_path(temp.clone()).unwrap();
        assert!(same_folder(&res.install, &temp), "{} is not {}", res.install, temp.display());
        assert_eq!(res.saves, "appdata");
        assert_eq!(res.mode, "folders");

        let _ = std::fs::remove_dir_all(&temp);
        let _ = std::fs::remove_file(&temp_state);
        std::env::remove_var("YW4_LAUNCHER_STATE");
    }
}
