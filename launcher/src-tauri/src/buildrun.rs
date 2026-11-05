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
struct StepEvent { id: &'static str, state: &'static str }
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
    let b = app.path().resource_dir().ok()?.join("builder");
    b.join("python").join("python.exe").is_file().then_some(b)
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

/// One Python step of the build (`python -m <module> <args>`), its lines to the screen; Err on failure or Cancel.
fn python(app: &AppHandle, run: &Running, b: &Builder, cfg: &Path, module: &str, args: &[&str]) -> Result<(), String> {
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

/// Copies a folder tree with progress (extracted folders → data\), skipping files already there at full size.
fn copy_tree(app: &AppHandle, run: &Running, step: &str, from: &Path, to: &Path) -> Result<(), String> {
    let (total, _) = crate::check::folder_size(from);
    let mut done = 0u64;
    let mut stack = vec![(from.to_path_buf(), to.to_path_buf())];
    while let Some((src, dst)) = stack.pop() {
        std::fs::create_dir_all(&dst).map_err(|e| e.to_string())?;
        for e in std::fs::read_dir(&src).map_err(|e| e.to_string())?.flatten() {
            if run.cancel.load(Ordering::SeqCst) { return Err("cancelled".into()); }
            let (s, d) = (e.path(), dst.join(e.file_name()));
            if e.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                stack.push((s, d));
                continue;
            }
            let len = e.metadata().map(|m| m.len()).unwrap_or(0);
            if d.metadata().map(|m| m.len()).ok() != Some(len) {
                std::fs::copy(&s, &d).map_err(|e| format!("copying {}: {e}", s.display()))?;
            }
            done += len;
            let _ = app.emit("build-progress", ProgressEvent { step: step.into(), done, total, what: String::new() });
        }
    }
    Ok(())
}

fn game_files(app: &AppHandle, run: &Running, b: &Builder, plan: &Plan, inst: &Path, cfg: &Path) -> Result<(), String> {
    let (exefs, romfs) = (inst.join("data/exefs"), inst.join("data/romfs"));
    let complete = exefs.join("main").is_file() && crate::check::folder_size(&romfs).0 == crate::check::romfs_bytes();
    if complete {
        emit_log(app, "game files already here: skipped");
        return Ok(());
    }
    if plan.mode == "folders" {
        let get = |k: &str| PathBuf::from(plan.paths.get(k).cloned().unwrap_or_default());
        copy_tree(app, run, "copy", &get("exefs"), &exefs)?;
        return copy_tree(app, run, "copy", &get("romfs"), &romfs);
    }
    // NSP/XCI: the extractor wants empty folders; a stopped extraction left ours half full
    for d in [&exefs, &romfs] {
        let _ = std::fs::remove_dir_all(d);
        std::fs::create_dir_all(d).map_err(|e| e.to_string())?;
    }
    python(app, run, b, cfg, "tools.yw4.extract", &[])
}

/// The checked add-ons, copied into data\dlc (the game checks them again on boot, decision 0049).
fn copy_dlc(app: &AppHandle, plan: &Plan, inst: &Path) -> Result<(), String> {
    let dst = inst.join("data/dlc");
    std::fs::create_dir_all(&dst).map_err(|e| e.to_string())?;
    let Some(src) = plan.paths.get("dlc").filter(|d| !d.is_empty()) else { return Ok(()) };
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

/// game.ini next to yw4.exe: its folders relative to it (decision 0050), so the install folder can move.
fn write_game_ini(plan: &Plan, inst: &Path) -> Result<(), String> {
    let (save, settings) = if plan.saves == "appdata" {
        let base = PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp");
        (base.join("save").display().to_string(), base.join("settings.ini").display().to_string())
    } else {
        ("../../save".to_string(), "../../settings.ini".to_string())
    };
    if plan.saves == "appdata" {
        std::fs::create_dir_all(&save).map_err(|e| e.to_string())?;
    } else {
        std::fs::create_dir_all(inst.join("save")).map_err(|e| e.to_string())?;
    }
    let ini = format!("# Written by the YW4 Recomp launcher: how yw4.exe runs on its own (decision 0050).\n\
        YW4_EXEFS=../exefs\nYW4_ROMFS=../../data/romfs\nYW4_SAVE={save}\nYW4_SETTINGS_FILE={settings}\n\
        YW4_DLC_DIR=../../data/dlc\nYW4_MODS=../../mods\nYW4_SCREENSHOT_DIR=../../screenshots\n\
        YW4_LOG_DIR=../../logs\nYW4_PERF_DIR=../../logs\nYW4_RECORD=0\n");
    std::fs::write(inst.join("work/build/game.ini"), ini).map_err(|e| e.to_string())
}

fn step(app: &AppHandle, id: &'static str, f: impl FnOnce() -> Result<(), String>) -> Result<(), String> {
    let _ = app.emit("build-step", StepEvent { id, state: "start" });
    let r = f();
    let _ = app.emit("build-step", StepEvent { id, state: if r.is_ok() { "done" } else { "failed" } });
    r
}

fn run_all(app: &AppHandle, run: &Running, plan: &Plan) -> Result<(), String> {
    let inst = PathBuf::from(&plan.install);
    for d in ["data", "work", "mods", "screenshots", "logs"] {
        std::fs::create_dir_all(inst.join(d)).map_err(|e| format!("can't create {}: {e}", inst.join(d).display()))?;
    }
    let b = builder(app);
    let cfg = write_config(&b, plan, &inst).map_err(|e| e.to_string())?;
    // First, so a failed download shows before minutes of unpacking. From the repo: nothing to fetch.
    step(app, "tools", || match &b.tools {
        Some(_) => {
            let dest = inst.join("builder").join("toolchain");
            python(app, run, &b, &cfg, "tools.yw4.deps", &["--toolchain", &dest.to_string_lossy()])
        }
        None => Ok(()),
    })?;
    step(app, "game", || game_files(app, run, &b, plan, &inst, &cfg))?;
    step(app, "dlc", || copy_dlc(app, plan, &inst))?;
    step(app, "version", || python(app, run, &b, &cfg, "tools.yw4.ingest", &[]))?;
    step(app, "translate", || python(app, run, &b, &cfg, "recomp.translate", &[]))?;
    step(app, "shaders", || {
        python(app, run, &b, &cfg, "tools.yw4.shaders", &[])?;
        python(app, run, &b, &cfg, "recomp.shader", &["--check"])
    })?;
    let mut build_args = vec![];
    if !plan.fsr { build_args.push("--no-fsr"); }
    if !plan.dlss { build_args.push("--no-dlss"); }
    step(app, "compile", || python(app, run, &b, &cfg, "recomp.build", &build_args))?;
    step(app, "finish", || write_game_ini(plan, &inst))
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
