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
}

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

#[derive(Serialize)]
pub struct Info {
    version: String,          // yw4 --version
    dlc: usize,               // checked add-ons in data\dlc
    mods: Vec<String>,        // mod folders in mods\ (Atmosphère's layout or plain)
    picture: Option<String>,  // the newest screenshot, else the game's icon (the player's own files)
    reclaimable: u64,         // what Free up space would delete
}

/// Folders the build only needs again for a rebuild: generated C, objects, downloads, shader assembly, unpacked NCAs.
fn reclaim_dirs(install: &Path) -> Vec<PathBuf> {
    let w = install.join("work");
    vec![w.join("recomp"), w.join("build").join("obj"), w.join("deps"), w.join("shaders").join("asm"), w.join("nca")]
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
        let version = hidden(Command::new(exe(&install)).arg("--version")).output().ok()
            .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string()).unwrap_or_default();
        let dlc = std::fs::read_dir(inst.join("data").join("dlc")).into_iter().flatten().flatten()
            .filter(|e| crate::check::dlc_index_of(&e.path()).is_some()).count();
        let newest = std::fs::read_dir(inst.join("screenshots")).into_iter().flatten().flatten()
            .filter(|e| e.path().extension().map_or(false, |x| x.eq_ignore_ascii_case("bmp") || x.eq_ignore_ascii_case("png")))
            .max_by_key(|e| e.metadata().and_then(|m| m.modified()).ok())
            .map(|e| e.path());
        let icon = inst.join("work").join("icon").join("yw4.ico");
        let picture = newest.or_else(|| icon.is_file().then_some(icon)).map(|p| p.display().to_string());
        let reclaimable = reclaim_dirs(&inst).iter().map(|d| dir_size(d)).sum();
        Info { version, dlc, mods: mod_names(&inst.join("mods")), picture, reclaimable }
    })
    .await
    .unwrap()
}

/// Starts the game: yw4.exe with no arguments reads game.ini next to it.
#[tauri::command]
pub fn play(install: String) -> Result<(), String> {
    let e = exe(&install);
    Command::new(&e).current_dir(e.parent().unwrap()).spawn().map(|_| ()).map_err(|e| format!("couldn't start the game ({e})"))
}

/// Opens one of the install's folders in Explorer (a fixed set: nothing else can be named).
#[tauri::command]
pub fn open_folder(install: String, which: String, saves: String) -> Result<(), String> {
    let inst = PathBuf::from(&install);
    let dir = match which.as_str() {
        "mods" | "screenshots" | "logs" => inst.join(&which),
        "dlc" => inst.join("data").join("dlc"),
        "save" if saves == "appdata" => PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp").join("save"),
        "save" => inst.join("save"),
        _ => return Err("unknown folder".into()),
    };
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Command::new("explorer").arg(&dir).spawn().map(|_| ()).map_err(|e| e.to_string())
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
    for place in places {
        let script = format!(
            "$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path {place} 'Yo-kai Watch 4 Recomp.lnk')); \
             $s.TargetPath=$env:YW4_LNK_TARGET; $s.WorkingDirectory=(Split-Path $env:YW4_LNK_TARGET); \
             $s.IconLocation=$env:YW4_LNK_ICON; $s.Save()");
        let ok = hidden(Command::new("powershell").args(["-NoProfile", "-NonInteractive", "-Command", &script])
            .env("YW4_LNK_TARGET", &target).env("YW4_LNK_ICON", &icon)).status().map(|s| s.success()).unwrap_or(false);
        if !ok { return Err("couldn't make a shortcut".into()); }
    }
    Ok(())
}
