// Linux: an install folder whose name has a backslash, moved to the folders its name meant.
//
// Launchers before 1.5.0 added "\YW4 Recomp" to a folder chosen on Linux, which made ONE folder named e.g.
// "Yokai 4 install\YW4 Recomp" (issue #22). The build's tools trip over that name one by one (zig's dependency lists,
// Clang's #embed, AMD's shader compiler), and the game code's links around it (tools/yw4/config.py no_backslash, the
// zig wrapper) didn't catch every case: a player's build still failed on every file (2026-11-20). So the launcher
// renames the folder itself, once, with the player's OK: "Yokai 4 install\YW4 Recomp" becomes
// "Yokai 4 install/YW4 Recomp". A rename on the same drive: nothing is copied. game.ini names its folders relative to
// yw4 (decision 0050), so the game runs from the new place as it is; the launcher's state and the install's
// config.toml are pointed there.

use std::path::{Path, PathBuf};

/// `p` with every backslash a folder separator, doubled slashes and a slash at the end dropped.
fn split_backslashes(p: &str) -> String {
    let mut s = p.replace('\\', "/");
    while s.contains("//") { s = s.replace("//", "/"); }
    if s.len() > 1 { s = s.trim_end_matches('/').to_string(); }
    s
}

/// Linux: where an install folder whose path has a backslash should be. None: no backslash (and always on Windows,
/// where \ is the separator).
fn fixed(p: &str) -> Option<String> {
    if cfg!(windows) || !p.contains('\\') { return None; }
    Some(split_backslashes(p))
}

/// The UI asks before a build and at the launcher's start: Some(new path) when the folder should move.
#[tauri::command]
pub fn install_slash_fix(install: String) -> Option<String> {
    fixed(&install)
}

/// Renames each folder in `old` whose name has a backslash into the folders its name meant, outermost first.
/// A folder that isn't there yet (a first build into a new folder) is only renamed on paper. Refuses when the new
/// place is there already with something in it: two installs are never merged.
fn move_folders(old: &str) -> Result<String, String> {
    let Some(new) = fixed(old) else { return Ok(old.to_string()) };
    let mut done = PathBuf::new();  // the path so far, as it is now on disk (earlier renames already made)
    for comp in Path::new(old).components() {
        let name = comp.as_os_str().to_string_lossy().into_owned();
        if !name.contains('\\') {
            done.push(comp);
            continue;
        }
        let src = done.join(&name);
        let mut dst = done.clone();
        for part in name.split('\\').filter(|s| !s.is_empty()) { dst.push(part); }
        if src.exists() {
            if dst.exists() {
                let empty = std::fs::read_dir(&dst).map(|mut d| d.next().is_none()).unwrap_or(false);
                if !empty || std::fs::remove_dir(&dst).is_err() {
                    return Err(format!("{} is there already. Move or rename it, then start the launcher again.", dst.display()));
                }
            }
            if let Some(parent) = dst.parent() {
                std::fs::create_dir_all(parent).map_err(|e| format!("can't create {}: {e}", parent.display()))?;
            }
            std::fs::rename(&src, &dst).map_err(|e| format!("can't move {} to {}: {e}", src.display(), dst.display()))?;
        }
        done = dst;
    }
    Ok(new)
}

/// `s` under `old` (itself or inside it): the same under `new`.
fn repoint(s: &str, old: &str, new: &str) -> Option<String> {
    if s == old { return Some(new.to_string()); }
    s.strip_prefix(old).filter(|rest| rest.starts_with('/') || rest.starts_with('\\'))
        .map(|rest| format!("{new}{}", split_backslashes(rest)))
}

fn repoint_json(v: &mut serde_json::Value, old: &str, new: &str) {
    match v {
        serde_json::Value::String(s) => { if let Some(n) = repoint(s, old, new) { *s = n; } }
        serde_json::Value::Array(a) => a.iter_mut().for_each(|x| repoint_json(x, old, new)),
        serde_json::Value::Object(o) => o.values_mut().for_each(|x| repoint_json(x, old, new)),
        _ => {}
    }
}

fn repoint_toml(v: &mut toml::Value, old: &str, new: &str) {
    match v {
        toml::Value::String(s) => { if let Some(n) = repoint(s, old, new) { *s = n; } }
        toml::Value::Array(a) => a.iter_mut().for_each(|x| repoint_toml(x, old, new)),
        toml::Value::Table(t) => t.iter_mut().for_each(|(_, x)| repoint_toml(x, old, new)),
        _ => {}
    }
}

/// The launcher's state (launcher.json) and the install's config.toml, pointed from `old` to `new`.
fn repoint_files(state: &Path, old: &str, new: &str) -> Result<(), String> {
    if let Ok(text) = std::fs::read_to_string(state) {
        if let Ok(mut v) = serde_json::from_str::<serde_json::Value>(&text) {
            repoint_json(&mut v, old, new);
            std::fs::write(state, serde_json::to_string_pretty(&v).unwrap()).map_err(|e| e.to_string())?;
        }
    }
    let cfg = Path::new(new).join("config.toml");
    if let Ok(mut v) = std::fs::read_to_string(&cfg).map_err(|_| ()).and_then(|t| t.parse::<toml::Value>().map_err(|_| ())) {
        repoint_toml(&mut v, old, new);
        let text = toml::to_string(&v).map_err(|e| e.to_string())?;
        std::fs::write(&cfg, format!("# Written by the YW4 Recomp launcher for this install folder (decision 0050).\n{text}"))
            .map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// Moves the install folder (see the module text) and returns its new path. The UI then saves its state as usual and
/// remakes the shortcuts (their icon is in the install folder).
#[tauri::command]
pub fn move_install_folder(install: String) -> Result<String, String> {
    if crate::ready::exe_in_use(&install) {
        return Err("Close the game first, then try again.".into());
    }
    let new = move_folders(&install)?;
    repoint_files(&crate::ready::state_path(), &install, &new)?;
    Ok(new)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backslashes_become_folders() {
        assert_eq!(split_backslashes(r"/home/deck/Yokai 4 install\YW4 Recomp"), "/home/deck/Yokai 4 install/YW4 Recomp");
        assert_eq!(split_backslashes(r"/home/deck/Documents/yk4\\YW4 Recomp\"), "/home/deck/Documents/yk4/YW4 Recomp");
        assert_eq!(split_backslashes("/"), "/");
    }

    #[test]
    fn repoint_only_inside() {
        let (old, new) = (r"/g/a\YW4 Recomp", "/g/a/YW4 Recomp");
        assert_eq!(repoint(old, old, new).as_deref(), Some(new));
        assert_eq!(repoint(&format!("{old}/work/icon/icon.png"), old, new).as_deref(), Some("/g/a/YW4 Recomp/work/icon/icon.png"));
        assert_eq!(repoint(&format!("{old}\\data\\exefs"), old, new).as_deref(), Some("/g/a/YW4 Recomp/data/exefs"));
        assert_eq!(repoint(&format!("{old} 2/x"), old, new), None);  // another folder whose name starts the same
        assert_eq!(repoint("/elsewhere/base.nsp", old, new), None);
    }

    #[cfg(unix)]
    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("yw4-slashfix-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[cfg(unix)]
    #[test]
    fn moves_the_folder_and_its_references() {
        let root = temp("move");
        let old = root.join(r"Yokai 4 install\YW4 Recomp");
        std::fs::create_dir_all(old.join("work/build")).unwrap();
        std::fs::write(old.join("work/build/yw4"), "x").unwrap();
        let old_s = old.to_string_lossy().into_owned();
        std::fs::write(old.join("config.toml"), format!("[output]\nwork_dir = {}\n[input]\nbase_nsp = \"/dumps/base.nsp\"\n",
            toml::Value::String(format!("{old_s}/work")))).unwrap();
        let state = root.join("launcher.json");
        std::fs::write(&state, serde_json::json!({ "install": old_s, "icon": format!("{old_s}/work/icon/icon.png"),
            "paths": { "base": "/dumps/base.nsp" } }).to_string()).unwrap();

        let new = move_folders(&old_s).unwrap();
        repoint_files(&state, &old_s, &new).unwrap();
        let want = root.join("Yokai 4 install").join("YW4 Recomp");
        assert_eq!(Path::new(&new), want);
        assert!(want.join("work/build/yw4").is_file() && !old.exists());
        let s: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&state).unwrap()).unwrap();
        assert_eq!(s["install"].as_str(), Some(new.as_str()));
        assert_eq!(s["icon"].as_str().unwrap(), format!("{new}/work/icon/icon.png"));
        assert_eq!(s["paths"]["base"].as_str(), Some("/dumps/base.nsp"));
        let c: toml::Table = std::fs::read_to_string(want.join("config.toml")).unwrap().parse().unwrap();
        assert_eq!(c["output"]["work_dir"].as_str().unwrap(), format!("{new}/work"));
        assert_eq!(c["input"]["base_nsp"].as_str(), Some("/dumps/base.nsp"));
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn never_merges_into_a_folder_with_files() {
        let root = temp("taken");
        let old = root.join(r"a\YW4 Recomp");
        std::fs::create_dir_all(&old).unwrap();
        std::fs::create_dir_all(root.join("a/YW4 Recomp")).unwrap();
        std::fs::write(root.join("a/YW4 Recomp/keep.txt"), "x").unwrap();
        assert!(move_folders(&old.to_string_lossy()).is_err());
        assert!(old.is_dir() && root.join("a/YW4 Recomp/keep.txt").is_file());
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn a_folder_not_made_yet_is_only_renamed_on_paper() {
        let root = temp("new");
        let old = root.join(r"x\YW4 Recomp");
        assert_eq!(move_folders(&old.to_string_lossy()).unwrap(), root.join("x/YW4 Recomp").to_string_lossy());
        assert!(!root.join("x").exists());
        std::fs::remove_dir_all(&root).unwrap();
    }
}
