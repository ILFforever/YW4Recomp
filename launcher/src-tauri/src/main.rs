// YW4 Recomp launcher (decision 0050): the screens are plain HTML/CSS/JS in launcher/ui, shown in Windows' own
// WebView2; this side does what a web page can't (files, processes, starting the game). One command so far.

// No console window behind the launcher in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod buildrun;
mod check;
mod options;

/// The launcher's own version, for its status bar (decision 0047: the launcher has its own number).
#[tauri::command]
fn launcher_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}

/// The project's GitHub page in the player's browser. Only this one address: the screens can't open others.
#[tauri::command]
fn open_project_page() -> Result<(), String> {
    const URL: &str = "https://github.com/ILFforever/YW4Recomp";
    #[cfg(windows)]
    let r = std::process::Command::new("explorer").arg(URL).spawn();
    #[cfg(not(windows))]
    let r = std::process::Command::new("xdg-open").arg(URL).spawn();
    r.map(|_| ()).map_err(|e| e.to_string())
}

/// For testing and screenshots: YW4_LAUNCHER_SCREEN=<screen> opens straight on that screen, without the
/// welcome popup. None for players.
#[tauri::command]
fn start_screen() -> Option<String> {
    std::env::var("YW4_LAUNCHER_SCREEN").ok().filter(|s| !s.is_empty())
}

/// For tests: YW4_LAUNCHER_PATHS = "mode;id=path;id=path…" fills screen 1 (with YW4_LAUNCHER_SCREEN=check).
#[tauri::command]
fn test_paths() -> Option<String> {
    std::env::var("YW4_LAUNCHER_PATHS").ok().filter(|s| !s.is_empty())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())  // the file and folder pickers (Browse…)
        .manage(std::sync::Arc::new(buildrun::Running::default()))  // the build in progress (screen 4)
        .invoke_handler(tauri::generate_handler![launcher_version, open_project_page, start_screen, test_paths, check::check_game,
            options::gpu_names, options::disk_space, buildrun::start_build, buildrun::cancel_build])
        .run(tauri::generate_context!())
        .expect("the launcher window could not start");
}
