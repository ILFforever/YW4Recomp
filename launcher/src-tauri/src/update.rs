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
// launcher's state). If GitHub can't be reached nothing is offered and nothing breaks. The system's curl does the
// transfer (Windows 10 1803 and newer have curl.exe), Windows' tar.exe or Linux's Python the unpacking (platform.rs).
// On Linux a newer launcher is offered as its release page, as for the portable zip (decision 0071).

use serde::Serialize;
use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
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

use crate::platform::{hidden, tool};

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

/// <install>\builder\game\chosen.txt: the version the player installed from a file (Settings, Update game from
/// file), which builds use even when it is older than the launcher's own code (owner, 2026-11-24: any game version
/// from its zip). Removed by a downloaded game update, so Update goes back to the newest.
const CHOSEN: &str = "chosen.txt";

fn chosen(install: &Path) -> Option<GameCode> {
    let v = std::fs::read_to_string(install.join("builder").join("game").join(CHOSEN)).ok()?;
    let v = v.trim();
    // the version names a folder: exactly "X.Y.Z", as install_game_source writes it
    if v.split('.').count() != 3 || !v.chars().all(|c| c.is_ascii_digit() || c == '.') { return None; }
    let dir = install.join("builder").join("game").join(v);
    usable(&dir).ok().filter(|u| u == v).map(|version| GameCode { dir, version })
}

/// The game code a build uses: the version the player chose from a file, else the newest of the launcher's own
/// and the downloaded ones it can run.
pub fn game_code(own: &Path, install: &Path) -> GameCode {
    if let Some(c) = chosen(install) { return c; }
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

/// The Play screen's "What's new": the version and change log of the game code builds use: a downloaded game
/// update's notes.md, or for the launcher's own code its game release's text in the cached list. None without
/// either: the launcher's own list then.
#[tauri::command]
pub fn game_news(app: tauri::AppHandle, install: String) -> Option<(String, String)> {
    let own = crate::buildrun::bundled_src(&app)?;
    let code = game_code(&own, Path::new(&install));
    let notes = std::fs::read_to_string(code.dir.join("notes.md")).ok().filter(|n| !n.trim().is_empty())
        // the launcher's own code (it came with the launcher, never downloaded): its game release's text from the
        // cached list, whatever its age; no request here, this command runs on the UI thread
        .or_else(|| {
            let c: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(cache_path()).ok()?).ok()?;
            release_notes(&c["releases"], &code.version)
        })?;
    Some((code.version, notes))
}

/// The text of game release v<version> in GitHub's list; None when it isn't there or is empty. For game code that
/// came with the launcher: it has no notes.md (2026-11-20: launcher 1.5.0 carried 2.0.0, and the rebuild question
/// showed no change log).
fn release_notes(list: &serde_json::Value, version: &str) -> Option<String> {
    let tag = format!("v{version}");
    list.as_array()?.iter()
        .find(|r| r["tag_name"].as_str() == Some(tag.as_str()) && !r["draft"].as_bool().unwrap_or(false))
        .and_then(|r| r["body"].as_str())
        .filter(|b| !b.trim().is_empty())
        .map(str::to_string)
}

/// After a build: downloaded game code other than the one just used is no longer needed (the player's choice,
/// chosen.txt, stays: it names the folder just used).
pub fn drop_other_game_code(install: &Path, used: &Path) {
    if let Ok(rd) = std::fs::read_dir(install.join("builder").join("game")) {
        for p in rd.flatten().map(|e| e.path()) {
            if p != used && p.file_name().map_or(true, |n| n != CHOSEN) {
                let _ = if p.is_dir() { std::fs::remove_dir_all(&p) } else { std::fs::remove_file(&p) };
            }
        }
    }
}

#[derive(Serialize, Clone)]
pub struct GameOffer { version: String, url: String, sha256: String, size: u64, notes: String }
/// A newer launcher. url, sha256 and size are its installer's (empty when the release has none with a digest:
/// then only its page is offered). portable: this launcher is the portable zip's (no uninstaller beside it), which
/// the installer would not update but install a second time elsewhere: then the page too, for the new zip.
#[derive(Serialize, Clone)]
pub struct LauncherOffer { version: String, page: String, notes: String, url: String, sha256: String, size: u64,
                           portable: bool }

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
    rebuild: Option<String>,          // game code on this PC other than the game built from it: its version
    rebuild_older: bool,              // ... and older than that game (a version chosen from a file)
    rebuild_notes: String,            // its change log: kept from its release when it was downloaded, else that release's text on GitHub
    offline: bool,                    // GitHub couldn't be asked (Settings' "Check now" says so)
}

fn cache_path() -> PathBuf {
    crate::platform::data_dir().join("releases.json")
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
    let out = hidden(crate::platform::command(tool("curl")).args([
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
pub(crate) fn built_version(line: &str) -> Option<(u32, u32, u32)> {
    ver(line.split_whitespace().skip_while(|w| *w != "Recomp").nth(1)?)
}

/// What the Play screen offers: a game update, a newer launcher, or a rebuild. Nothing for a launcher run from the
/// repo (it builds the repo's own code), except a test feed's launcher offers (YW4_UPDATE_FEED: the box can then be
/// seen in `tauri dev`). github false (Settings' Check for updates off): only the rebuild, GitHub isn't asked.
#[tauri::command]
pub async fn check_updates(app: tauri::AppHandle, install: String, built: String, fresh: Option<bool>,
                           github: Option<bool>) -> Updates {
    let own = crate::buildrun::bundled_src(&app);
    if own.is_none() && std::env::var("YW4_UPDATE_FEED").is_err() { return Updates::default() }
    tauri::async_runtime::spawn_blocking(move || {
        let inst = PathBuf::from(&install);
        let code = own.as_deref().map(|own| game_code(own, &inst));
        let mut u = Updates::default();
        if let Some((Some(have), Some(built))) = code.as_ref().map(|c| (ver(&c.version), built_version(&built))) {
            let code = code.as_ref().unwrap();
            // newer code to build, or an older version the player chose from a file
            if have != built {
                u.rebuild = Some(code.version.clone());
                u.rebuild_older = have < built;
                u.rebuild_notes = std::fs::read_to_string(code.dir.join("notes.md")).unwrap_or_default();
            }
        }
        if github == Some(false) { return u; }
        let Some(list) = releases(fresh.unwrap_or(false)) else { u.offline = true; return u };
        if let Some(v) = u.rebuild.as_deref().filter(|_| u.rebuild_notes.trim().is_empty()) {
            u.rebuild_notes = release_notes(&list, v).unwrap_or_default();  // code that came with the launcher
        }
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
                                                        url, sha256, size,
                                                        portable: crate::ready::uninstaller().is_none() }));
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
        // (not for a version the player chose from a file, older or newer than the build: building it is what they
        // asked for. 2026-11-24: 1.4.0 from a zip over a built 1.0.2, with 2.2.1 out, got no rebuild question)
        if u.game.is_some() && chosen(&inst).is_none() { u.rebuild = None; u.rebuild_notes.clear(); }
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
    crate::platform::open(&url)
}

/// Downloads `url` to a `.part` file beside `dest` with `curl --max-time`, checks its SHA-256 against `sha256`,
/// and renames it to `dest` only when verified. Removes the `.part` file on any failure.
fn download_verified(url: &str, dest: &Path, sha256: &str, max_time: u32, mismatch_msg: &str) -> Result<(), String> {
    let mut part_os = dest.as_os_str().to_os_string();
    part_os.push(".part");
    let part = PathBuf::from(part_os);
    let _ = std::fs::remove_file(dest);
    let timeout = max_time.to_string();
    let ok = hidden(crate::platform::command(tool("curl")).args(["-sSfL", "--max-time", &timeout, "-o"]).arg(&part).arg(url))
        .status().map(|s| s.success()).unwrap_or(false);
    if !ok {
        let _ = std::fs::remove_file(&part);
        return Err("The download didn't finish. Check your connection and try again.".into());
    }
    let data = std::fs::read(&part).map_err(|e| e.to_string())?;
    let got: String = Sha256::digest(&data).iter().map(|b| format!("{b:02x}")).collect();
    if got != sha256.to_lowercase() {
        let _ = std::fs::remove_file(&part);
        return Err(mismatch_msg.into());
    }
    std::fs::rename(&part, dest).map_err(|e| e.to_string())?;
    Ok(())
}

/// Downloads a game update into <install>\builder\game\<version>\: checked against GitHub's SHA-256 before it is
/// unpacked, then its release.toml (a newer launcher needed: refused, with the reason).
#[tauri::command]
pub async fn download_game_update(install: String, version: String, url: String, sha256: String, notes: String) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || {
        // GitHub only; a test feed (YW4_UPDATE_FEED) may also point at a local file
        let local_test = std::env::var("YW4_UPDATE_FEED").is_ok() && url.starts_with("file:///");
        // The version names the folders deleted below, so it is exactly "X.Y.Z" (ver() alone reads the first three
        // numbers only: "1.2.3.\..\..\x" passed), and the file is this repo's release file for that version, as
        // for the launcher's own update (review 2026-11-19: any github.com address was taken).
        let plain = version.split('.').count() == 3 && version.chars().all(|c| c.is_ascii_digit() || c == '.');
        let github = format!("https://github.com/ILFforever/YW4Recomp/releases/download/v{version}/yw4recomp-game-{version}.zip");
        if !plain || ver(&version).is_none() || sha256.len() != 64 || !(url == github || local_test) {
            return Err("not a game update".to_string());
        }
        let dir = PathBuf::from(&install).join("builder").join("game");
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let (zip, tmp, dst) = (dir.join(format!("{version}.zip")), dir.join(format!("{version}.unpack")), dir.join(&version));
        download_verified(&url, &zip, &sha256, 300,
            "The download isn't the file GitHub lists for this update. Try again later.")?;
        let _ = std::fs::remove_dir_all(&tmp);
        std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
        // Windows' bsdtar and Python's zipfile both refuse absolute paths and ".." in an archive
        let ok = crate::platform::unzip(&zip, &tmp);
        let _ = std::fs::remove_file(&zip);
        let checked = if ok { usable(&tmp) } else { Err("couldn't unpack the update".into()) };
        match checked {
            Ok(v) if v == version => {
                let _ = std::fs::remove_dir_all(&dst);
                std::fs::rename(&tmp, &dst).map_err(|e| e.to_string())?;
                let _ = std::fs::write(dst.join("notes.md"), notes);  // the change log, for the rebuild question
                let _ = std::fs::remove_file(dir.join(CHOSEN));  // Update: back to the newest version
                Ok(version)
            }
            other => {
                let _ = std::fs::remove_dir_all(&tmp);
                Err(match other { Err(e) => e, Ok(v) => format!("the download holds version {v}, not {version}") })
            }
        }
    }).await.map_err(|e| e.to_string())?
}

/// Installs game code the player already has, with no GitHub: a yw4recomp-game-X.Y.Z.zip, or the folder it unpacks to.
/// It goes into <install>\builder\game\<version>\ like a download, after the same usable() check, and becomes the
/// version builds use (chosen.txt), older ones too (owner, 2026-11-24). A file from someone else has no GitHub
/// SHA-256 to check: install only one you trust. Returns the version.
#[tauri::command]
pub async fn install_game_file(app: tauri::AppHandle, install: String, path: String) -> Result<String, String> {
    if crate::buildrun::building(&app) {
        return Err("A build is running. Install the game file when it has finished.".into());
    }
    // an empty folder would mean a relative "builder" folder beside the launcher
    if install.trim().is_empty() { return Err("No install folder.".into()); }
    tauri::async_runtime::spawn_blocking(move || {
        let dir = PathBuf::from(&install).join("builder").join("game");
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        install_game_source(Path::new(&path), &dir)
    }).await.map_err(|e| e.to_string())?
}

/// The work behind install_game_file: unpack (a zip) or read (a folder) the source, check it, copy it into
/// <dir>\<version>\ (the old copy of that version is replaced). The temporary unpack folder is always removed.
fn install_game_source(src: &Path, dir: &Path) -> Result<String, String> {
    let tmp = dir.join("file.unpack");
    let _ = std::fs::remove_dir_all(&tmp);
    let result: Result<String, String> = (|| {
        let is_zip = src.is_file() && src.extension().map_or(false, |e| e.eq_ignore_ascii_case("zip"));
        let base = if src.is_dir() {
            src.to_path_buf()
        } else if is_zip {
            std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
            // Windows' bsdtar and Python's zipfile both refuse absolute paths and ".." in an archive
            if !crate::platform::unzip(src, &tmp) { return Err("couldn't unpack that zip".into()); }
            tmp.clone()
        } else {
            return Err("Choose a game zip file or the folder it unpacks to.".into());
        };
        let root = find_game_root(&base).ok_or("No game code there: no release.toml in it or one folder down.")?;
        let version = usable(&root)?;
        // the version names a folder that is deleted and replaced below: exactly "X.Y.Z", as for a download
        let plain = version.split('.').count() == 3 && version.chars().all(|c| c.is_ascii_digit() || c == '.');
        if !plain || ver(&version).is_none() { return Err(format!("the game code's version \"{version}\" isn't a version number")); }
        let staged = dir.join(format!("{version}.copy"));
        let _ = std::fs::remove_dir_all(&staged);
        copy_tree(&root, &staged).map_err(|e| e.to_string())?;
        let dst = dir.join(&version);
        let _ = std::fs::remove_dir_all(&dst);
        std::fs::rename(&staged, &dst).map_err(|e| e.to_string())?;
        std::fs::write(dir.join(CHOSEN), &version).map_err(|e| e.to_string())?;
        Ok(version)
    })();
    let _ = std::fs::remove_dir_all(&tmp);
    result
}

/// The folder that holds release.toml: the folder itself, or the one folder inside it (a zip with a top folder).
fn find_game_root(dir: &Path) -> Option<PathBuf> {
    if dir.join("release.toml").is_file() { return Some(dir.to_path_buf()); }
    std::fs::read_dir(dir).ok()?.flatten().map(|e| e.path())
        .find(|p| p.is_dir() && p.join("release.toml").is_file())
}

/// Copies a folder tree. Symbolic links are skipped: only the files and folders themselves are game code.
fn copy_tree(src: &Path, dst: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(dst)?;
    for e in std::fs::read_dir(src)?.flatten() {
        let (from, to, ft) = (e.path(), dst.join(e.file_name()), e.file_type()?);
        if ft.is_symlink() { continue; }
        if ft.is_dir() { copy_tree(&from, &to)?; } else { std::fs::copy(&from, &to)?; }
    }
    Ok(())
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
        let exe = dir.join(&name);
        download_verified(&url, &exe, &sha256, 600,
            "The download isn't the file GitHub lists for this launcher. Try again later.")?;
        *CHECKED_INSTALLER.lock().map_err(|e| e.to_string())? = Some(exe);
        Ok(())
    }).await.map_err(|e| e.to_string())?
}

/// Starts the checked installer and quits. Its flags (Tauri 2's NSIS installer, checked in the template of
/// @tauri-apps/cli 2.12.4): /P passive (a progress window, no questions, closes when done), /UPDATE over the
/// installed launcher without its reinstall questions, /R the new launcher started again afterwards, as the user.
/// If this launcher hasn't quit by the time the installer looks, the installer closes it (Restart Manager).
/// /D= the folder this launcher runs from. Without it the installer takes the folder in Windows' installed-apps entry,
/// which is the LAST install's: with two installs (W:\YW4 Recomp and a later one in AppData) the update went to the
/// other one, and this launcher stayed old and offered the update again (owner, 2026-11-26).
#[tauri::command]
pub fn run_launcher_update(app: tauri::AppHandle) -> Result<(), String> {
    let exe = CHECKED_INSTALLER.lock().map_err(|e| e.to_string())?.take().ok_or("no checked installer")?;
    let mut cmd = crate::platform::command(&exe);
    cmd.args(["/P", "/UPDATE", "/R"]);
    #[cfg(windows)]
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) {
        // NSIS reads /D= only as the last argument and unquoted, spaces and all: arg() would quote it
        use std::os::windows::process::CommandExt;
        cmd.raw_arg(installer_dir_arg(&dir));
    }
    cmd.spawn().map_err(|e| format!("couldn't start the installer: {e}"))?;
    std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_millis(300));  // the page's answer gets back first
        app.exit(0);
    });
    Ok(())
}

/// The installer's folder argument: `/D=` and the path as it is (NSIS takes the rest of the line).
fn installer_dir_arg(dir: &Path) -> String {
    format!("/D={}", dir.display())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn installer_dir_is_unquoted_with_spaces() {
        assert_eq!(installer_dir_arg(Path::new(r"W:\YW4 Recomp")), r"/D=W:\YW4 Recomp");
    }

    #[test]
    fn game_notes_for_code_that_came_with_the_launcher() {
        let list = serde_json::json!([
            { "tag_name": "launcher-v1.5.0", "body": "launcher text" },
            { "tag_name": "v2.0.0", "draft": true, "body": "draft text" },
            { "tag_name": "v2.0.0", "body": "### Playing online\n- **Ark Swap.**" },
            { "tag_name": "v1.8.0", "body": "  " },
        ]);
        assert_eq!(release_notes(&list, "2.0.0").as_deref(), Some("### Playing online\n- **Ark Swap.**"));
        assert_eq!(release_notes(&list, "1.8.0"), None);  // empty text: no notes box
        assert_eq!(release_notes(&list, "1.5.0"), None);  // the launcher's own release isn't the game's
    }

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

    #[test]
    fn game_code_installed_from_a_folder_without_github() {
        let base = std::env::temp_dir().join(format!("yw4-local-game-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let src = base.join("yw4recomp-game");
        std::fs::create_dir_all(src.join("recomp")).unwrap();
        std::fs::write(src.join("release.toml"), "version = \"9.8.7\"\n").unwrap();
        std::fs::write(src.join("recomp").join("a.py"), "x").unwrap();
        let dir = base.join("install").join("builder").join("game");
        std::fs::create_dir_all(&dir).unwrap();
        assert_eq!(install_game_source(&src, &dir).as_deref(), Ok("9.8.7"));
        assert!(dir.join("9.8.7").join("recomp").join("a.py").is_file());
        // the folder the player picked is left as it was
        assert!(src.join("release.toml").is_file());
        // a version that is not X.Y.Z is refused (it names a folder), and no temporary folder is left behind
        std::fs::write(src.join("release.toml"), "version = \"1.2.3/../../x\"\n").unwrap();
        assert!(install_game_source(&src, &dir).is_err());
        assert!(!dir.join("file.unpack").exists());
        assert!(!dir.join("1.2.3").exists());
        // a file that is not a zip: refused with the reason
        assert!(install_game_source(&src.join("release.toml"), &dir).is_err());
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn a_version_from_a_file_is_built_even_when_older() {
        let base = std::env::temp_dir().join(format!("yw4-chosen-game-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let code = |dir: &Path, v: &str| {
            std::fs::create_dir_all(dir).unwrap();
            std::fs::write(dir.join("release.toml"), format!("version = \"{v}\"\n")).unwrap();
        };
        let own = base.join("own");
        code(&own, "2.2.1");
        let install = base.join("install");
        let game = install.join("builder").join("game");
        // a newer download than the launcher's own code: the newest wins, as before
        code(&game.join("2.3.0"), "2.3.0");
        assert_eq!(game_code(&own, &install).version, "2.3.0");
        // an older one installed from a file: that one, and the choice is remembered
        let src = base.join("zip").join("yw4recomp-game");
        code(&src, "1.8.0");
        assert_eq!(install_game_source(&src, &game).as_deref(), Ok("1.8.0"));
        assert_eq!(std::fs::read_to_string(game.join(CHOSEN)).unwrap(), "1.8.0");
        assert_eq!(game_code(&own, &install).version, "1.8.0");
        // after its build the other code goes, the choice stays
        drop_other_game_code(&install, &game.join("1.8.0"));
        assert!(!game.join("2.3.0").exists());
        assert_eq!(game_code(&own, &install).version, "1.8.0");
        // a choice whose folder is gone, or that isn't a plain version: back to the newest
        std::fs::write(game.join(CHOSEN), "9.9.9").unwrap();
        assert_eq!(game_code(&own, &install).version, "2.2.1");
        std::fs::write(game.join(CHOSEN), "1.8.0/../../x").unwrap();
        assert_eq!(game_code(&own, &install).version, "2.2.1");
        let _ = std::fs::remove_dir_all(&base);
    }
}
