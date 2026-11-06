// Launcher themes (decision: themes are files, so anyone can make one). A theme is a CSS file of colour tokens in the
// `themes` folder next to the launcher: themes\<id>.css. It is read when it is chosen, so a new or edited file shows up
// without a rebuild. A first line `/* name: Jibanyan */` gives the name shown on the Settings page; without it the
// file name is the name. The built-in look ('recomp') has no file and is always listed first.
use std::path::{Path, PathBuf};

use serde::Serialize;
use tauri::{AppHandle, Manager};

#[derive(Serialize)]
pub struct ThemeInfo {
    id: String,
    name: String,
}

/// The themes folder: next to the installed launcher (the installer copies launcher/ui/themes there), or the repo's
/// own folder when the launcher runs from a build of the repo.
pub fn themes_dir(app: &AppHandle) -> Option<PathBuf> {
    let installed = app.path().resource_dir().ok()?.join("themes");
    if installed.is_dir() {
        return Some(installed);
    }
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("ui").join("themes");
    repo.is_dir().then_some(repo)
}

/// A theme id is a file name: lower-case letters, digits and dashes only, so nothing outside the folder can be read.
fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 40 && id.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
}

fn name_of(css: &str, id: &str) -> String {
    let first = css.lines().next().unwrap_or("");
    first.trim()
        .strip_prefix("/*")
        .and_then(|s| s.strip_suffix("*/"))
        .and_then(|s| s.trim().strip_prefix("name:"))
        .map(|n| n.trim().to_string())
        .filter(|n| !n.is_empty())
        .unwrap_or_else(|| id.to_string())
}

/// The built-in look first, then every .css file in the folder, by name.
#[tauri::command]
pub fn list_themes(app: AppHandle) -> Vec<ThemeInfo> {
    let mut out = vec![ThemeInfo { id: "recomp".into(), name: "YW4 Recomp".into() }];
    let Some(dir) = themes_dir(&app) else { return out };
    let mut found: Vec<ThemeInfo> = std::fs::read_dir(&dir).into_iter().flatten().flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().map_or(false, |x| x.eq_ignore_ascii_case("css")))
        .filter_map(|p| {
            let id = p.file_stem()?.to_string_lossy().into_owned();
            if !valid_id(&id) || id == "recomp" {
                return None;
            }
            let css = std::fs::read_to_string(&p).ok()?;
            Some(ThemeInfo { name: name_of(&css, &id), id })
        })
        .collect();
    found.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
    out.extend(found);
    out
}

/// The CSS of one theme, read now. Empty for the built-in look.
#[tauri::command]
pub fn read_theme(app: AppHandle, id: String) -> Result<String, String> {
    if id == "recomp" {
        return Ok(String::new());
    }
    if !valid_id(&id) {
        return Err("not a theme".into());
    }
    let dir = themes_dir(&app).ok_or("no themes folder")?;
    std::fs::read_to_string(dir.join(format!("{id}.css"))).map_err(|e| format!("can't read the theme ({e})"))
}
