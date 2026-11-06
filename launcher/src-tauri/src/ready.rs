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
}

fn default_on_play() -> String { "hide".into() }
fn yes() -> bool { true }

fn state_path() -> PathBuf {
    if let Ok(p) = std::env::var("YW4_LAUNCHER_STATE") {  // tests: keep their state out of the player's AppData
        return PathBuf::from(p);
    }
    PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp").join("launcher.json")
}

#[tauri::command]
pub fn load_state() -> Option<State> {
    let s: State = serde_json::from_str(&std::fs::read_to_string(state_path()).ok()?).ok()?;
    exe(&s.install).is_file().then_some(s)  // only an install that's still there
}

#[tauri::command]
pub fn save_state(state: State) -> Result<(), String> {
    let p = state_path();
    std::fs::create_dir_all(p.parent().unwrap()).map_err(|e| e.to_string())?;
    std::fs::write(&p, serde_json::to_string_pretty(&state).unwrap()).map_err(|e| e.to_string())
}

fn exe(install: &str) -> PathBuf {
    Path::new(install).join("work").join("build").join("yw4.exe")
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
}

#[derive(Serialize)]
pub struct Info {
    version: String,          // yw4 --version
    dlc: usize,               // checked add-ons in data\dlc
    #[serde(rename = "dlcList")]
    dlc_list: Vec<DlcItem>,   // every NSP there: the checked ones by index, then the others
    mods: Vec<String>,        // mod folders in mods\ (Atmosphère's layout or plain)
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
    let read = |d: &Path| std::fs::read_dir(d).into_iter().flatten().flatten().filter(|e| e.path().is_dir())
        .map(|e| e.file_name().to_string_lossy().into_owned()).collect::<Vec<_>>();
    for name in read(mods) {
        if name.eq_ignore_ascii_case("contents") {  // contents\<title id>\<mod>
            for tid in read(&mods.join(&name)) {
                out.extend(read(&mods.join(&name).join(&tid)));
            }
        } else {
            out.push(name);
        }
    }
    out.sort();
    out
}

#[tauri::command]
pub async fn install_info(install: String) -> Info {
    tauri::async_runtime::spawn_blocking(move || {
        let inst = PathBuf::from(&install);
        let version = game_version(&exe(&install));
        let mut dlc_list: Vec<DlcItem> = std::fs::read_dir(inst.join("data").join("dlc")).into_iter().flatten().flatten()
            .map(|e| e.path())
            .filter(|p| p.extension().map_or(false, |x| x.eq_ignore_ascii_case("nsp")))
            .map(|p| {
                let index = crate::check::dlc_index_of(&p);
                DlcItem { index, id: index.map(crate::check::dlc_title_id),
                          file: p.file_name().unwrap_or_default().to_string_lossy().into_owned() }
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
        // icon.jpg is the game's own picture; installs built before it existed have only the .ico
        let dir = inst.join("work").join("icon");
        let icon = [dir.join("icon.jpg"), dir.join("yw4.ico")].into_iter().find(|p| p.is_file()).map(|p| p.display().to_string());
        let reclaimable = reclaim_dirs(&inst).iter().map(|d| dir_size(d)).sum();
        let played = load_played(&install);
        Info { version, dlc, dlc_list, mods: mod_names(&inst.join("mods")), icon, shots, shot_count, reclaimable,
               last_played: played.last_played, play_secs: played.seconds }
    })
    .await
    .unwrap()
}

/// Starts the game: yw4.exe with no arguments reads game.ini next to it. Notes the start, and when the game
/// exits adds the time played and tells the screens ("played"), so the Ready screen's numbers update.
/// Settings (Play): with `mods` off the game starts with none (YW4_MODS=none: an environment variable wins over
/// game.ini's line); `on_play` says what the launcher's window does meanwhile, and "hide" brings it back afterwards.
#[tauri::command]
pub fn play(app: tauri::AppHandle, install: String, mods: Option<bool>, on_play: Option<String>, theme: Option<String>) -> Result<(), String> {
    use tauri::{Emitter, Manager};
    let e = exe(&install);
    let mut cmd = Command::new(&e);
    cmd.current_dir(e.parent().unwrap());
    if mods == Some(false) { cmd.env("YW4_MODS", "none"); }
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
        // (3: the recompiled code stopped, kernel.c): offer to report it, with this play's log (report_crash)
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
    let ini = std::fs::read_to_string(build.join("game.ini")).ok()?;
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

/// A zip that stores its files (no compression): the crash report is a log and a settings file.
fn stored_zip(files: &[(&str, Vec<u8>)]) -> Vec<u8> {
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

/// After a crash (the 'game-crashed' event, Report it): a zip in the install's reports folder with a short
/// report.txt (exit code, version and GPU from the log), the play's log and settings.ini; the folder shown, and
/// GitHub's bug form opened with the version and GPU filled in. Nothing is uploaded: the player drags the zip in.
#[tauri::command]
pub fn report_crash(install: String, code: i32, log: String) -> Result<String, String> {
    let log_text = std::fs::read(&log).unwrap_or_default();
    let text = String::from_utf8_lossy(&log_text);
    let find = |prefix: &str| text.lines().find_map(|l| l.strip_prefix(prefix)).map(|s| s.trim().to_string());
    let version = find("yw4: YW4 Recomp ").unwrap_or_else(|| "unknown".into());
    let gpu = find("yw4: window: Vulkan on ").map(|g| g.split(" (API").next().unwrap_or("").to_string()).unwrap_or_default();
    let last: Vec<&str> = text.lines().rev().take(12).collect::<Vec<_>>().into_iter().rev().collect();
    let report = format!("YW4 Recomp crash report\r\n\r\nThe game closed unexpectedly (exit code {code}).\r\nVersion: {version}\r\nGPU: {gpu}\r\n\r\nThe log's last lines:\r\n{}\r\n",
        last.join("\r\n"));
    let mut files: Vec<(&str, Vec<u8>)> = vec![("report.txt", report.into_bytes())];
    if !log_text.is_empty() { files.push(("log.txt", log_text.clone())); }
    if let Some(s) = game_ini_path(&install, "YW4_SETTINGS_FILE").and_then(|p| std::fs::read(p).ok()) { files.push(("settings.ini", s)); }
    let dir = PathBuf::from(&install).join("reports");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let zip = dir.join(format!("yw4_crash_{}.zip", now()));
    std::fs::write(&zip, stored_zip(&files)).map_err(|e| format!("couldn't save the report ({e})"))?;
    let url = format!("https://github.com/ILFforever/YW4Recomp/issues/new?template=bug.yml&title={}&what={}&version={}&pc={}",
        url_value("Crash: the game closed unexpectedly"), url_value(&format!("The game closed unexpectedly (exit code {code}).")),
        url_value(&version), url_value(&gpu));
    if std::env::var("YW4_REPORT_OPEN").as_deref() != Ok("0") {
        let _ = Command::new("explorer").arg(format!("/select,{}", zip.display())).spawn();
        let _ = Command::new("explorer").arg(&url).spawn();
    }
    Ok(zip.to_string_lossy().into_owned())
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
        "mods" | "screenshots" | "logs" => inst.join(&which),
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
