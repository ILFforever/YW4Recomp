// Updates (decision 0054): which game code a build uses, what GitHub has that is newer, and fetching a game update.
//
//   game code    the game's build code: the launcher's own copy (builder\src, from its installer) or a downloaded
//                game update (<install>\builder\game\<version>\). A build uses the newest one this launcher can run
//                (release.toml: protocol, min_launcher).
//   game update  a GitHub release tagged vX.Y.Z with yw4recomp-game-X.Y.Z.zip: downloaded, checked against the
//                SHA-256 GitHub reports for it, unpacked, then the player rebuilds when they want.
//   launcher     a release tagged launcher-vA.B.C: the player installs it themselves (its release page).
//
// The check is one request to GitHub's list of releases (no account), at most once an hour (cached beside the
// launcher's state). If GitHub can't be reached nothing is offered and nothing breaks. Windows' own curl.exe and
// tar.exe do the transfer and the unpacking (Windows 10 1803 and newer have both).

use serde::Serialize;
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

/// The build protocol this launcher speaks (tools/yw4/build_install.py PROTOCOL).
pub const PROTOCOL: i64 = 1;
pub const LAUNCHER: &str = include_str!("../../VERSION");
const RELEASES: &str = "https://api.github.com/repos/ILFforever/YW4Recomp/releases?per_page=30";

type Ver = (u32, u32, u32);

fn ver(s: &str) -> Option<Ver> {
    let mut it = s.trim().trim_start_matches('v').split('.').map(|p| p.trim().parse::<u32>().ok());
    Some((it.next()??, it.next()??, it.next()??))
}

fn launcher_ver() -> Ver {
    ver(LAUNCHER).unwrap_or((0, 0, 0))
}

fn hidden(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; cmd.creation_flags(0x0800_0000); }
    cmd
}

fn system_tool(name: &str) -> PathBuf {
    let root = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".into());
    PathBuf::from(root).join("System32").join(name)
}

/// A folder of game code this launcher can build with: its version, else None (no release.toml, too new a protocol
/// or a newer launcher needed). Err says why, for the player.
fn usable(dir: &Path) -> Result<String, String> {
    let t: toml::Table = std::fs::read_to_string(dir.join("release.toml")).map_err(|_| "no release.toml".to_string())?
        .parse().map_err(|e| format!("release.toml: {e}"))?;
    let version = t.get("version").and_then(|v| v.as_str()).ok_or("release.toml: no version")?.to_string();
    let protocol = t.get("protocol").and_then(|v| v.as_integer()).unwrap_or(0);
    let min = t.get("min_launcher").and_then(|v| v.as_str()).and_then(ver).unwrap_or((0, 0, 0));
    if protocol > PROTOCOL || min > launcher_ver() {
        return Err(format!("game update {version} needs launcher {}.{}.{} or newer", min.0, min.1, min.2));
    }
    Ok(version)
}

pub struct GameCode {
    pub dir: PathBuf,
    pub version: String,
}

/// The game code a build uses: the newest of the launcher's own and the downloaded ones it can run.
pub fn game_code(own: &Path, install: &Path) -> GameCode {
    let own_version = usable(own).ok()
        .or_else(|| std::fs::read_to_string(own.join("VERSION")).ok().map(|s| s.trim().to_string()))
        .unwrap_or_default();
    let mut best = GameCode { dir: own.to_path_buf(), version: own_version };
    if let Ok(rd) = std::fs::read_dir(install.join("builder").join("game")) {
        for d in rd.flatten().map(|e| e.path()).filter(|d| d.is_dir()) {
            if let Ok(v) = usable(&d) {
                if ver(&v) > ver(&best.version) {
                    best = GameCode { dir: d, version: v };
                }
            }
        }
    }
    best
}

/// After a build: downloaded game code other than the one just used is no longer needed.
pub fn drop_other_game_code(install: &Path, used: &Path) {
    if let Ok(rd) = std::fs::read_dir(install.join("builder").join("game")) {
        for p in rd.flatten().map(|e| e.path()) {
            if p != used {
                let _ = if p.is_dir() { std::fs::remove_dir_all(&p) } else { std::fs::remove_file(&p) };
            }
        }
    }
}

#[derive(Serialize, Clone)]
pub struct GameOffer { version: String, url: String, sha256: String, size: u64, notes: String }
#[derive(Serialize, Clone)]
pub struct LauncherOffer { version: String, page: String }
#[derive(Serialize, Clone, Default)]
pub struct Updates {
    game: Option<GameOffer>,          // newer game code on GitHub
    launcher: Option<LauncherOffer>,  // a newer launcher on GitHub
    rebuild: Option<String>,          // game code on this PC newer than the game built from it: its version
}

fn cache_path() -> PathBuf {
    PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp").join("releases.json")
}

fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// GitHub's list of releases (JSON), from the hour-old cache when there is one. YW4_UPDATE_FEED=<file>: that file
/// instead (tests: a fake list in GitHub's format, never cached).
fn releases() -> Option<serde_json::Value> {
    if let Ok(feed) = std::env::var("YW4_UPDATE_FEED") {
        return serde_json::from_str(&std::fs::read_to_string(feed).ok()?).ok();
    }
    let cache = cache_path();
    if let Some(c) = std::fs::read_to_string(&cache).ok().and_then(|s| serde_json::from_str::<serde_json::Value>(&s).ok()) {
        if now().saturating_sub(c["at"].as_u64().unwrap_or(0)) < 3600 {
            return Some(c["releases"].clone());
        }
    }
    let out = hidden(Command::new(system_tool("curl.exe")).args([
        "-sSfL", "--max-time", "10", "-H", "Accept: application/vnd.github+json", "-H", "User-Agent: YW4-Recomp-Launcher",
        RELEASES,
    ])).output().ok()?;
    if !out.status.success() { return None; }
    let list: serde_json::Value = serde_json::from_slice(&out.stdout).ok()?;
    if let Some(dir) = cache.parent() { let _ = std::fs::create_dir_all(dir); }
    let _ = std::fs::write(&cache, serde_json::json!({ "at": now(), "releases": list }).to_string());
    Some(list)
}

/// The version in `yw4 --version`'s line ("YW4 Recomp 0.7.0 (commit, built day)").
fn built_version(line: &str) -> Option<Ver> {
    ver(line.split_whitespace().skip_while(|w| *w != "Recomp").nth(1)?)
}

/// What the Play screen offers: a game update, a newer launcher, or a rebuild. Nothing for a launcher run from the
/// repo (it builds the repo's own code).
#[tauri::command]
pub async fn check_updates(app: tauri::AppHandle, install: String, built: String) -> Updates {
    let Some(own) = crate::buildrun::bundled_src(&app) else { return Updates::default() };
    tauri::async_runtime::spawn_blocking(move || {
        let inst = PathBuf::from(&install);
        let code = game_code(&own, &inst);
        let mut u = Updates::default();
        if let (Some(have), Some(built)) = (ver(&code.version), built_version(&built)) {
            if have > built { u.rebuild = Some(code.version.clone()); }
        }
        let Some(list) = releases() else { return u };
        let (mut game, mut launcher): (Option<(Ver, GameOffer)>, Option<(Ver, LauncherOffer)>) = (None, None);
        for r in list.as_array().into_iter().flatten() {
            if r["draft"].as_bool().unwrap_or(false) || r["prerelease"].as_bool().unwrap_or(false) { continue; }
            let tag = r["tag_name"].as_str().unwrap_or("");
            if let Some(v) = tag.strip_prefix("launcher-v").and_then(ver) {
                if launcher.as_ref().map_or(true, |(b, _)| v > *b) {
                    let page = r["html_url"].as_str().unwrap_or("").to_string();
                    launcher = Some((v, LauncherOffer { version: format!("{}.{}.{}", v.0, v.1, v.2), page }));
                }
            } else if let Some(v) = tag.strip_prefix('v').and_then(ver) {
                let name = format!("yw4recomp-game-{}.{}.{}.zip", v.0, v.1, v.2);
                let Some(a) = r["assets"].as_array().into_iter().flatten().find(|a| a["name"] == name.as_str()) else { continue };
                let sha256 = a["digest"].as_str().unwrap_or("").trim_start_matches("sha256:").to_string();
                if sha256.len() != 64 || game.as_ref().map_or(false, |(b, _)| v <= *b) { continue; }
                game = Some((v, GameOffer {
                    version: format!("{}.{}.{}", v.0, v.1, v.2),
                    url: a["browser_download_url"].as_str().unwrap_or("").to_string(),
                    sha256, size: a["size"].as_u64().unwrap_or(0),
                    notes: r["body"].as_str().unwrap_or("").to_string(),
                }));
            }
        }
        let have = ver(&code.version).unwrap_or((0, 0, 0));
        u.game = game.filter(|(v, _)| *v > have).map(|(_, g)| g);
        u.launcher = launcher.filter(|(v, _)| *v > launcher_ver()).map(|(_, l)| l);
        u
    }).await.unwrap_or_default()
}

/// A newer launcher's release page in the player's browser: only this repo's release pages.
#[tauri::command]
pub fn open_release_page(url: String) -> Result<(), String> {
    if !url.starts_with("https://github.com/ILFforever/YW4Recomp/releases/") || url.contains(['"', ' ', '&', '|']) {
        return Err("not a release page".into());
    }
    hidden(Command::new("explorer").arg(&url)).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Downloads a game update into <install>\builder\game\<version>\: checked against GitHub's SHA-256 before it is
/// unpacked, then its release.toml (a newer launcher needed: refused, with the reason).
#[tauri::command]
pub async fn download_game_update(install: String, version: String, url: String, sha256: String) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || {
        // GitHub only; a test feed (YW4_UPDATE_FEED) may also point at a local file
        let local_test = std::env::var("YW4_UPDATE_FEED").is_ok() && url.starts_with("file:///");
        if ver(&version).is_none() || sha256.len() != 64 || !(url.starts_with("https://github.com/") || local_test) {
            return Err("not a game update".to_string());
        }
        let dir = PathBuf::from(&install).join("builder").join("game");
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let (zip, tmp, dst) = (dir.join(format!("{version}.zip.part")), dir.join(format!("{version}.unpack")), dir.join(&version));
        let ok = hidden(Command::new(system_tool("curl.exe")).args(["-sSfL", "--max-time", "300", "-o"]).arg(&zip).arg(&url))
            .status().map(|s| s.success()).unwrap_or(false);
        if !ok {
            let _ = std::fs::remove_file(&zip);
            return Err("The download didn't finish. Check your connection and try again.".into());
        }
        let data = std::fs::read(&zip).map_err(|e| e.to_string())?;
        let got: String = Sha256::digest(&data).iter().map(|b| format!("{b:02x}")).collect();
        if got != sha256.to_lowercase() {
            let _ = std::fs::remove_file(&zip);
            return Err("The download isn't the file GitHub lists for this update. Try again later.".into());
        }
        let _ = std::fs::remove_dir_all(&tmp);
        std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
        // Windows' bsdtar refuses absolute paths and ".." in an archive unless told otherwise (-P)
        let ok = hidden(Command::new(system_tool("tar.exe")).arg("-xf").arg(&zip).arg("-C").arg(&tmp))
            .status().map(|s| s.success()).unwrap_or(false);
        let _ = std::fs::remove_file(&zip);
        let checked = if ok { usable(&tmp) } else { Err("couldn't unpack the update".into()) };
        match checked {
            Ok(v) if v == version => {
                let _ = std::fs::remove_dir_all(&dst);
                std::fs::rename(&tmp, &dst).map_err(|e| e.to_string())?;
                Ok(version)
            }
            other => {
                let _ = std::fs::remove_dir_all(&tmp);
                Err(match other { Err(e) => e, Ok(v) => format!("the download holds version {v}, not {version}") })
            }
        }
    }).await.map_err(|e| e.to_string())?
}
