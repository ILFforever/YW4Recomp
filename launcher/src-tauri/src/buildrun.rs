// Screen 4, Build: runs the build tools (bundled in a download, decision 0052) into the install folder
// and tells the screen how far it is (decision 0050). Each Python step runs with YW4_PROGRESS=1 and its
// "@progress <step> <done> <total> <what>" lines go to the screen as they come; everything else goes to the log.
//
//   <install>\data\     exefs\, romfs\ (extracted or copied), dlc\ (checked add-ons, copied)
//   <install>\work\     the build: exefs\ (checked), recomp\, shaders\, build\yw4.exe + game.ini, deps\
//   <install>\mods\  screenshots\  logs\   (save\ and settings.ini too, when kept in the install folder)
//   <install>\builder\toolchain\   the compiler, downloaded on the first build; Free up space deletes it
//   <install>\config.toml   the tools' config for this install (YW4_CONFIG)
//
// A step whose result is already there is skipped, so Build after a Cancel picks up where it stopped.

use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::{AppHandle, Emitter, Manager};

#[derive(Deserialize, Clone)]
pub struct Plan {
    mode: String,                   // "files" or "folders"
    paths: HashMap<String, String>, // screen 1's choices
    install: String,
    fsr: bool,
    dlss: bool,
    saves: String,                  // "install" or "appdata"
}

#[derive(Default)]
pub struct Running {
    child: Mutex<Option<Child>>,
    cancel: AtomicBool,
    busy: AtomicBool,
}

#[derive(Serialize, Clone)]
struct StepEvent { id: String, state: String }
#[derive(Serialize, Clone)]
struct ProgressEvent { step: String, done: u64, total: u64, what: String }
#[derive(Serialize, Clone)]
struct DoneEvent { ok: bool, message: String }

/// What runs the build (decision 0052). A download has builder\ next to the launcher (launcher/package.py): its own
/// Python, the build code and tools, nothing taken from the PC. A launcher built with plain `cargo build` has no
/// builder\ and uses the repo it was built from, with the repo's Python and config.local.toml.
struct Builder {
    python: PathBuf,
    src: PathBuf,        // the folder the build's Python modules run from
    tools: Option<PathBuf>,  // builder\tools; None = the repo's
}

fn bundled_dir(app: &AppHandle) -> Option<PathBuf> {
    let b = plain_path(app.path().resource_dir().ok()?).join("builder");
    b.join("python").join("python.exe").is_file().then_some(b)
}

/// Windows' long-path form `\\?\C:\...` (Tauri gives the resource folder that way) back to `C:\...`, for the
/// config file people read. A network path (`\\?\UNC\...`) is left as it is.
fn plain_path(p: PathBuf) -> PathBuf {
    match p.to_str().and_then(|s| s.strip_prefix(r"\\?\")) {
        Some(rest) if !rest.starts_with("UNC\\") => PathBuf::from(rest),
        _ => p,
    }
}

fn builder(app: &AppHandle) -> Builder {
    match bundled_dir(app) {
        Some(b) => Builder { python: b.join("python").join("python.exe"), src: b.join("src"), tools: Some(b.join("tools")) },
        None => {
            let p = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("..");
            Builder { python: PathBuf::from("python"), src: p.canonicalize().unwrap_or(p), tools: None }
        }
    }
}

/// True in a download: the Options screen then counts the compiler's download and disk space.
#[tauri::command]
pub fn builder_bundled(app: AppHandle) -> bool {
    bundled_dir(&app).is_some()
}

fn emit_log(app: &AppHandle, line: &str) {
    let _ = app.emit("build-log", line.to_string());
}

/// The repo's own [tools] (compiler, glslang), when running from the repo.
fn repo_tools(repo: &Path) -> toml::Table {
    std::fs::read_to_string(repo.join("config.local.toml")).ok()
        .and_then(|s| s.parse::<toml::Table>().ok())
        .and_then(|t| t.get("tools").and_then(|v| v.as_table()).cloned())
        .unwrap_or_default()
}

fn lit(p: &Path) -> String {
    format!("'{}'", p.display())  // a TOML literal string: no escapes, Windows paths can't contain '
}

fn write_config(b: &Builder, plan: &Plan, inst: &Path) -> std::io::Result<PathBuf> {
    let get = |k: &str| plan.paths.get(k).filter(|v| !v.is_empty()).map(PathBuf::from);
    let mut s = String::from("# Written by the YW4 Recomp launcher for this install folder (decision 0050).\n[input]\n");
    s += &format!("exefs_dir = {}\nromfs_dir = {}\n", lit(&inst.join("data/exefs")), lit(&inst.join("data/romfs")));
    if plan.mode == "files" {
        if let Some(b) = get("base") { s += &format!("base_nsp = {}\n", lit(&b)); }
        if let Some(u) = get("update") { s += &format!("update_nsp = {}\n", lit(&u)); }
    }
    s += &format!("dlc_dir = {}\n[output]\nwork_dir = {}\n[tools]\n", lit(&inst.join("data/dlc")), lit(&inst.join("work")));
    if let Some(tools) = &b.tools {
        s += &format!("cc = {}\n", lit(&inst.join("builder/toolchain/llvm-mingw/bin/clang.exe")));
        s += &format!("glslang = {}\nhactool_exe = {}\n", lit(&tools.join("glslang.exe")), lit(&tools.join("hactool.exe")));
    } else {
        let tools = repo_tools(&b.src);
        for key in ["cc", "glslang", "hactool_exe"] {
            if let Some(v) = tools.get(key).and_then(|v| v.as_str()).filter(|v| !v.is_empty()) {
                s += &format!("{key} = {}\n", lit(Path::new(v)));
            }
        }
    }
    if let Some(k) = get("keys") { s += &format!("keys_dir = {}\n", lit(&k)); }
    let path = inst.join("config.toml");
    std::fs::write(&path, s)?;
    Ok(path)
}

/// One run of the game code's Python (`python -m <module> <args>`), its lines to the screen; Err on failure or
/// Cancel. The build's own lines (decision 0054): `@steps <json>` the list of steps (the launcher adds its DLC
/// copy at the end), `@step <id> <state>` each step starting and ending, `@progress` the bar.
fn python(app: &AppHandle, run: &Running, b: &Builder, cfg: &Path, module: &str, args: &[String]) -> Result<(), String> {
    if run.cancel.load(Ordering::SeqCst) { return Err("cancelled".into()); }
    let mut cmd = Command::new(&b.python);
    cmd.arg("-u").arg("-m").arg(module).args(args).current_dir(&b.src)
        .env("YW4_CONFIG", cfg).env("YW4_PROGRESS", "1").env("PYTHONIOENCODING", "utf-8")
        .stdout(Stdio::piped()).stderr(Stdio::piped());
    if b.tools.is_some() {
        // A player's own CC would override the compiler we tested. Python's caches go in the build folder: the
        // launcher's folder stays as installed, so uninstalling leaves nothing.
        cmd.env_remove("CC").env("PYTHONPYCACHEPREFIX", cfg.with_file_name("work").join("pycache"));
    }
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; cmd.creation_flags(0x0800_0000); }
    emit_log(app, &format!("> python -m {module} {}", args.join(" ")));
    let mut child = cmd.spawn().map_err(|e| format!("couldn't start Python ({e})"))?;
    let out = child.stdout.take().unwrap();
    let err = child.stderr.take().unwrap();
    *run.child.lock().unwrap() = Some(child);
    let app2 = app.clone();
    let err_thread = std::thread::spawn(move || {  // stderr: the log, and the last lines for an error message
        let mut tail = Vec::new();
        for l in BufReader::new(err).lines().map_while(Result::ok) {
            emit_log(&app2, &l);
            tail.push(l);
            if tail.len() > 8 { tail.remove(0); }
        }
        tail.join("\n")
    });
    for line in BufReader::new(out).lines().map_while(Result::ok) {
        if let Some(rest) = line.strip_prefix("@steps ") {
            if let Ok(mut list) = serde_json::from_str::<serde_json::Value>(rest) {
                if let Some(a) = list.as_array_mut() {
                    a.push(serde_json::json!({ "id": "dlc", "label": "Copying DLC", "seconds": 1, "parts": {} }));
                }
                let _ = app.emit("build-steps", list);
            }
            continue;
        }
        if let Some((id, state)) = line.strip_prefix("@step ").and_then(|r| r.split_once(' ')) {
            let _ = app.emit("build-step", StepEvent { id: id.into(), state: state.into() });
            continue;
        }
        if let Some(rest) = line.strip_prefix("@progress ") {
            let mut it = rest.splitn(4, ' ');
            let (step, done, total, what) = (it.next(), it.next(), it.next(), it.next().unwrap_or(""));
            if let (Some(step), Some(Ok(done)), Some(Ok(total))) = (step, done.map(str::parse), total.map(str::parse)) {
                let _ = app.emit("build-progress", ProgressEvent { step: step.into(), done, total, what: what.into() });
                continue;
            }
        }
        emit_log(app, &line);
    }
    let tail = err_thread.join().unwrap_or_default();
    let status = run.child.lock().unwrap().take().map(|mut c| c.wait());
    if run.cancel.load(Ordering::SeqCst) { return Err("cancelled".into()); }
    match status {
        Some(Ok(s)) if s.success() => Ok(()),
        _ => Err(if tail.is_empty() { format!("{module} failed") } else { tail }),
    }
}

/// The checked add-ons, copied into data\dlc (the game checks them again on boot, decision 0049).
fn copy_dlc(app: &AppHandle, plan: &Plan, inst: &Path) -> Result<(), String> {
    let dst = inst.join("data/dlc");
    std::fs::create_dir_all(&dst).map_err(|e| e.to_string())?;
    let Some(src) = plan.paths.get("dlc").filter(|d| !d.is_empty()) else { return Ok(()) };
    // The player may have deleted their originals after the first build (the Ready screen offers it): the add-ons
    // already copied into data\dlc are what the game uses, so a missing source folder is fine.
    if !Path::new(src).is_dir() {
        emit_log(app, &format!("DLC folder {src} is gone: keeping the add-ons already in data\\dlc"));
        return Ok(());
    }
    let mut n = 0;
    for p in std::fs::read_dir(src).map_err(|e| e.to_string())?.flatten().map(|e| e.path()) {
        if p.extension().map_or(false, |x| x.eq_ignore_ascii_case("nsp")) && crate::check::dlc_index_of(&p).is_some() {
            let d = dst.join(p.file_name().unwrap());
            if !d.is_file() {
                std::fs::copy(&p, &d).map_err(|e| e.to_string())?;
            }
            n += 1;
        }
    }
    emit_log(app, &format!("{n} add-on(s) copied"));
    Ok(())
}

/// The whole build: the game code's own steps (tools.yw4.build_install, decision 0054), then the DLC copy. The
/// launcher knows no step of the game's; a game update can change them.
fn run_all(app: &AppHandle, run: &Running, plan: &Plan) -> Result<(), String> {
    let inst = PathBuf::from(&plan.install);
    for d in ["data", "work", "mods", "screenshots", "logs"] {
        std::fs::create_dir_all(inst.join(d)).map_err(|e| format!("can't create {}: {e}", inst.join(d).display()))?;
    }
    let b = builder(app);
    let cfg = write_config(&b, plan, &inst).map_err(|e| e.to_string())?;
    let mut args: Vec<String> = vec!["--mode".into(), plan.mode.clone(), "--saves".into(), plan.saves.clone()];
    if plan.mode == "folders" {
        let get = |k: &str| plan.paths.get(k).cloned().unwrap_or_default();
        args.extend(["--exefs".into(), get("exefs"), "--romfs".into(), get("romfs")]);
    }
    if b.tools.is_some() {  // a download: the compiler comes on the first build; from the repo, the repo's own
        args.extend(["--toolchain".into(), inst.join("builder").join("toolchain").to_string_lossy().into_owned()]);
    }
    if !plan.fsr { args.push("--no-fsr".into()); }
    if !plan.dlss { args.push("--no-dlss".into()); }
    if let Err(e) = python(app, run, &b, &cfg, "tools.yw4.build_install", &args) {
        if run.cancel.load(Ordering::SeqCst) {
            // Cancel ended the build's processes at once, so it couldn't put back the game it had set aside
            run.cancel.store(false, Ordering::SeqCst);
            let _ = python(app, run, &b, &cfg, "tools.yw4.build_install", &["--recover".into()]);
            run.cancel.store(true, Ordering::SeqCst);
        }
        return Err(e);
    }
    let _ = app.emit("build-step", StepEvent { id: "dlc".into(), state: "start".into() });
    let r = copy_dlc(app, plan, &inst);
    let _ = app.emit("build-step", StepEvent { id: "dlc".into(), state: if r.is_ok() { "done" } else { "failed" }.into() });
    r
}

#[tauri::command]
pub fn start_build(app: AppHandle, plan: Plan) -> Result<(), String> {
    let run = app.state::<Arc<Running>>().inner().clone();
    if run.busy.swap(true, Ordering::SeqCst) {
        return Err("a build is already running".into());
    }
    run.cancel.store(false, Ordering::SeqCst);
    std::thread::spawn(move || {
        let r = run_all(&app, &run, &plan);
        run.busy.store(false, Ordering::SeqCst);
        let _ = app.emit("build-done", match r {
            Ok(()) => DoneEvent { ok: true, message: String::new() },
            Err(e) => DoneEvent { ok: false, message: e },
        });
    });
    Ok(())
}

/// Stops the build: the running step and everything it started (translate's worker processes too).
#[tauri::command]
pub fn cancel_build(app: AppHandle) {
    let run = app.state::<Arc<Running>>().inner().clone();
    run.cancel.store(true, Ordering::SeqCst);
    let pid = run.child.lock().unwrap().as_ref().map(|c| c.id());
    if let Some(pid) = pid {
        let mut kill = Command::new("taskkill");
        kill.args(["/T", "/F", "/PID", &pid.to_string()]);
        #[cfg(windows)]
        { use std::os::windows::process::CommandExt; kill.creation_flags(0x0800_0000); }
        let _ = kill.status();
    }
}
