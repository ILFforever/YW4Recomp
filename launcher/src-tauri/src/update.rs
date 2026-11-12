// Updates (decision 0054): which game code a build uses, what GitHub has that is newer, and fetching a game update.
//
//   game code    the game's build code: the launcher's own copy (builder\src, from its installer) or a downloaded
//                game update (<install>\builder\game\<version>\). A build uses the newest one this launcher can run
//                (release.toml: protocol, min_launcher).
//   game update  a GitHub release tagged vX.Y.Z with yw4recomp-game-X.Y.Z.zip: downloaded, checked against the
//                SHA-256 GitHub reports for it, unpacked, then the player rebuilds when they want.
//   launcher     a release tagged launcher-vA.B.C with YW4-Recomp-Launcher-A.B.C-setup.exe: on the player's Update
//                the launcher downloads it, checks it against GitHub's SHA-256, starts it and quits; the installer
//                replaces it and starts the new one. A release without that file: its page, to install by hand.
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
pub const PROTOCOL: i64 = 2;
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
/// A newer launcher. url, sha256 and size are its installer's (empty when the release has none with a digest:
/// then only its page is offered).
#[derive(Serialize, Clone)]
pub struct LauncherOffer { version: String, page: String, notes: String, url: String, sha256: String, size: u64 }

/// The installer a launcher release carries (package.py's name for it), with the SHA-256 GitHub reports for it.
fn launcher_installer(release: &serde_json::Value, v: Ver) -> Option<(String, String, u64)> {
    let name = format!("YW4-Recomp-Launcher-{}.{}.{}-setup.exe", v.0, v.1, v.2);
    let a = release["assets"].as_array()?.iter().find(|a| a["name"] == name.as_str())?;
    let sha256 = a["digest"].as_str()?.strip_prefix("sha256:")?.to_lowercase();
    let url = a["browser_download_url"].as_str()?.to_string();
    (sha256.len() == 64).then_some((url, sha256, a["size"].as_u64().unwrap_or(0)))
}
#[derive(Serialize, Clone, Default)]
pub struct Updates {
    game: Option<GameOffer>,          // newer game code on GitHub
    launcher: Option<LauncherOffer>,  // a newer launcher on GitHub
    rebuild: Option<String>,          // game code on this PC newer than the game built from it: its version
    rebuild_notes: String,            // its change log, kept from its release when it was downloaded
    offline: bool,                    // GitHub couldn't be asked (Settings' "Check now" says so)
}

fn cache_path() -> PathBuf {
    PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp").join("releases.json")
}

fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// GitHub's list of releases (JSON), from the hour-old cache when there is one. YW4_UPDATE_FEED=<file>: that file
/// instead (tests: a fake list in GitHub's format, never cached).
fn releases(fresh: bool) -> Option<serde_json::Value> {
    if let Ok(feed) = std::env::var("YW4_UPDATE_FEED") {
        return serde_json::from_str(&std::fs::read_to_string(feed).ok()?).ok();
    }
    let cache = cache_path();
    if let Some(c) = std::fs::read_to_string(&cache).ok().filter(|_| !fresh)
        .and_then(|s| serde_json::from_str::<serde_json::Value>(&s).ok()) {
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
/// repo (it builds the repo's own code), except a test feed's launcher offers (YW4_UPDATE_FEED: the box can then be
/// seen in `tauri dev`).
#[tauri::command]
pub async fn check_updates(app: tauri::AppHandle, install: String, built: String, fresh: Option<bool>) -> Updates {
    let own = crate::buildrun::bundled_src(&app);
    if own.is_none() && std::env::var("YW4_UPDATE_FEED").is_err() { return Updates::default() }
    tauri::async_runtime::spawn_blocking(move || {
        let inst = PathBuf::from(&install);
        let code = own.as_deref().map(|own| game_code(own, &inst));
        let mut u = Updates::default();
        if let Some((Some(have), Some(built))) = code.as_ref().map(|c| (ver(&c.version), built_version(&built))) {
            let code = code.as_ref().unwrap();
            if have > built {
                u.rebuild = Some(code.version.clone());
                u.rebuild_notes = std::fs::read_to_string(code.dir.join("notes.md")).unwrap_or_default();
            }
        }
        let Some(list) = releases(fresh.unwrap_or(false)) else { u.offline = true; return u };
        let (mut game, mut launcher): (Option<(Ver, GameOffer)>, Option<(Ver, LauncherOffer)>) = (None, None);
        for r in list.as_array().into_iter().flatten() {
            if r["draft"].as_bool().unwrap_or(false) || r["prerelease"].as_bool().unwrap_or(false) { continue; }
            let tag = r["tag_name"].as_str().unwrap_or("");
            if let Some(v) = tag.strip_prefix("launcher-v").and_then(ver) {
                if launcher.as_ref().map_or(true, |(b, _)| v > *b) {
                    let page = r["html_url"].as_str().unwrap_or("").to_string();
                    let notes = r["body"].as_str().unwrap_or("").to_string();
                    let (url, sha256, size) = launcher_installer(r, v).unwrap_or_default();
                    launcher = Some((v, LauncherOffer { version: format!("{}.{}.{}", v.0, v.1, v.2), page, notes,
                                                        url, sha256, size }));
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
        // from the repo (a test feed) no game offers: the repo's code is what it builds
        u.game = code.as_ref().and_then(|c| {
            let have = ver(&c.version).unwrap_or((0, 0, 0));
            game.filter(|(v, _)| *v > have).map(|(_, g)| g)
        });
        // a newer version out than the downloaded one waiting to be built: download that instead of building this
        // (2026-11-11: a 0.9.3 that didn't build stood in front of the 0.9.4 that fixed it)
        if u.game.is_some() { u.rebuild = None; u.rebuild_notes.clear(); }
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
pub async fn download_game_update(install: String, version: String, url: String, sha256: String, notes: String) -> Result<String, String> {
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
                let _ = std::fs::write(dst.join("notes.md"), notes);  // the change log, for the rebuild question
                Ok(version)
            }
            other => {
                let _ = std::fs::remove_dir_all(&tmp);
                Err(match other { Err(e) => e, Ok(v) => format!("the download holds version {v}, not {version}") })
            }
        }
    }).await.map_err(|e| e.to_string())?
}

/// The launcher installer download_launcher_update checked; run_launcher_update starts only this file, so the page
/// can't hand it any other.
static CHECKED_INSTALLER: std::sync::Mutex<Option<PathBuf>> = std::sync::Mutex::new(None);

/// Downloads a newer launcher's installer into the temp folder and checks it against GitHub's SHA-256. Only this
/// repo's release file for that version (a test feed, YW4_UPDATE_FEED, may also point at a local file), and not
/// while a build runs: the launcher is about to quit.
#[tauri::command]
pub async fn download_launcher_update(app: tauri::AppHandle, version: String, url: String, sha256: String) -> Result<(), String> {
    if crate::buildrun::building(&app) {
        return Err("A build is running. Update the launcher when it has finished.".into());
    }
    tauri::async_runtime::spawn_blocking(move || {
        let Some(v) = ver(&version) else { return Err("not a launcher update".to_string()) };
        let v = format!("{}.{}.{}", v.0, v.1, v.2);
        let name = format!("YW4-Recomp-Launcher-{v}-setup.exe");
        let github = format!("https://github.com/ILFforever/YW4Recomp/releases/download/launcher-v{v}/{name}");
        let local_test = std::env::var("YW4_UPDATE_FEED").is_ok() && url.starts_with("file:///");
        if sha256.len() != 64 || !(url == github || local_test) {
            return Err("not a launcher update".to_string());
        }
        let dir = std::env::temp_dir().join("YW4 Recomp");
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let (part, exe) = (dir.join(format!("{name}.part")), dir.join(&name));
        let _ = std::fs::remove_file(&exe);
        let ok = hidden(Command::new(system_tool("curl.exe")).args(["-sSfL", "--max-time", "600", "-o"]).arg(&part).arg(&url))
            .status().map(|s| s.success()).unwrap_or(false);
        if !ok {
            let _ = std::fs::remove_file(&part);
            return Err("The download didn't finish. Check your connection and try again.".into());
        }
        let data = std::fs::read(&part).map_err(|e| e.to_string())?;
        let got: String = Sha256::digest(&data).iter().map(|b| format!("{b:02x}")).collect();
        if got != sha256.to_lowercase() {
            let _ = std::fs::remove_file(&part);
            return Err("The download isn't the file GitHub lists for this launcher. Try again later.".into());
        }
        std::fs::rename(&part, &exe).map_err(|e| e.to_string())?;
        *CHECKED_INSTALLER.lock().map_err(|e| e.to_string())? = Some(exe);
        Ok(())
    }).await.map_err(|e| e.to_string())?
}

/// Starts the checked installer and quits. Its flags (Tauri 2's NSIS installer, checked in the template of
/// @tauri-apps/cli 2.12.4): /P passive (a progress window, no questions, closes when done), /UPDATE over the
/// installed launcher without its reinstall questions, /R the new launcher started again afterwards, as the user.
/// If this launcher hasn't quit by the time the installer looks, the installer closes it (Restart Manager).
#[tauri::command]
pub fn run_launcher_update(app: tauri::AppHandle) -> Result<(), String> {
    let exe = CHECKED_INSTALLER.lock().map_err(|e| e.to_string())?.take().ok_or("no checked installer")?;
    Command::new(&exe).args(["/P", "/UPDATE", "/R"]).spawn().map_err(|e| format!("couldn't start the installer: {e}"))?;
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_millis(300));  // the page's answer gets back first
        app.exit(0);
    });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn launcher_installer_from_a_release() {
        let sha = "ab".repeat(32);
        let r = serde_json::json!({ "tag_name": "launcher-v1.2.3", "assets": [
            { "name": "YW4-Recomp-Launcher-1.2.3-portable.zip", "digest": format!("sha256:{sha}"),
              "browser_download_url": "https://example/zip", "size": 5 },
            { "name": "YW4-Recomp-Launcher-1.2.3-setup.exe", "digest": format!("sha256:{}", sha.to_uppercase()),
              "browser_download_url": "https://github.com/ILFforever/YW4Recomp/releases/download/launcher-v1.2.3/YW4-Recomp-Launcher-1.2.3-setup.exe",
              "size": 7 } ] });
        let (url, got, size) = launcher_installer(&r, (1, 2, 3)).expect("the installer");
        assert!(url.ends_with("/launcher-v1.2.3/YW4-Recomp-Launcher-1.2.3-setup.exe"));
        assert_eq!((got, size), (sha, 7));
        // no digest (older GitHub uploads), or another version's file: none, the page is offered instead
        let no_digest = serde_json::json!({ "assets": [{ "name": "YW4-Recomp-Launcher-1.2.3-setup.exe",
            "browser_download_url": "x" }] });
        assert!(launcher_installer(&no_digest, (1, 2, 3)).is_none());
        assert!(launcher_installer(&r, (1, 2, 4)).is_none());
    }
}
