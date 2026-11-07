// YW4 Recomp launcher (decision 0050): the screens are plain HTML/CSS/JS in launcher/ui, shown in Windows' own
// WebView2; this side does what a web page can't (files, processes, starting the game). One command so far.

// No console window behind the launcher in release builds.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod buildrun;
mod check;
mod options;
mod ready;
mod themes;
mod update;

/// The launcher's own number (launcher/VERSION, decisions 0047 and 0054): apart from the game's, so a launcher fix
/// never asks for a rebuild. Shown in the status bar next to the game's.
#[tauri::command]
fn launcher_version() -> String {
    update::LAUNCHER.trim().to_string()
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

/// Help's links, a fixed set: the project page and its issues (Report a problem). Nothing else can be opened.
#[tauri::command]
fn open_link(which: String) -> Result<(), String> {
    let url = match which.as_str() {
        "project" => "https://github.com/ILFforever/YW4Recomp",
        "issues" => "https://github.com/ILFforever/YW4Recomp/issues",
        _ => return Err("unknown link".into()),
    };
    std::process::Command::new("explorer").arg(url).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Help: the licence texts that ship with the launcher (builder\licenses, from launcher/package.py), if any.
#[tauri::command]
fn licences_folder(app: tauri::AppHandle, open: bool) -> bool {
    use tauri::Manager;
    let Some(dir) = app.path().resource_dir().ok().map(|d| d.join("builder").join("licenses")).filter(|d| d.is_dir())
        else { return false };
    if open { let _ = std::process::Command::new("explorer").arg(&dir).spawn(); }
    true
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

/// No Windows error boxes from what the launcher starts ("SDL3.dll was not found" from a broken install's
/// yw4.exe): the error mode is inherited by child processes, and the launcher reports such failures itself.
pub fn quiet_errors() {
    #[cfg(windows)]
    unsafe {
        use windows_sys::Win32::System::Diagnostics::Debug::{SetErrorMode, SEM_FAILCRITICALERRORS, SEM_NOOPENFILEERRORBOX};
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    }
}

fn main() {
    quiet_errors();
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())  // the file and folder pickers (Browse…)
        .manage(std::sync::Arc::new(buildrun::Running::default()))  // the build in progress (screen 4)
        // The launcher never fills the screen (owner, 2026-11-05). tauri.conf.json makes it a fixed size, maximising off;
        // Windows can still maximise a window itself (Win+Up, dragging it to the top edge): undo that.
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::Resized(size) = event {
                if window.is_maximized().unwrap_or(false) {
                    let _ = window.unmaximize();
                    return;
                }
                // snap layouts and other programs can size it past the cap too: back inside 1600 x 1000
                let scale = window.scale_factor().unwrap_or(1.0);
                let (w, h) = (size.width as f64 / scale, size.height as f64 / scale);
                if w > 1600.5 || h > 1000.5 {
                    let _ = window.set_size(tauri::LogicalSize::new(w.min(1600.0), h.min(1000.0)));
                }
            }
        })
        .invoke_handler(tauri::generate_handler![launcher_version, open_project_page, open_link, licences_folder, start_screen, test_paths, check::check_game, check::extracted_files,
            themes::list_themes, themes::read_theme, options::gpu_names, options::disk_space, buildrun::start_build, buildrun::cancel_build, buildrun::builder_bundled, update::check_updates, update::download_game_update, update::open_release_page,
            ready::load_state, ready::save_state, ready::install_info, ready::set_dlc_keys, ready::play, ready::open_folder, ready::open_shot,
            ready::free_space, ready::originals, ready::delete_originals, ready::make_shortcuts, ready::move_saves, ready::game_full_screen, ready::set_game_full_screen, ready::uninstall_info, ready::uninstall, ready::report_crash])
        .run(tauri::generate_context!())
        .expect("the launcher window could not start");
}
