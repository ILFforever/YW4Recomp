// Screen 3, Options: what the PC has (graphics cards, free space where the game will go).

use serde::Serialize;
use std::path::Path;

/// The graphics cards Windows reports, by name (Win32_VideoController, through PowerShell with no window).
#[tauri::command]
pub async fn gpu_names() -> Vec<String> {
    tauri::async_runtime::spawn_blocking(|| {
        let mut cmd = std::process::Command::new("powershell");
        cmd.args(["-NoProfile", "-NonInteractive", "-Command",
                  "Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name }"]);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flashing up
        }
        cmd.output().ok()
            .map(|o| String::from_utf8_lossy(&o.stdout).lines().map(str::trim).filter(|l| !l.is_empty())
                .map(String::from).collect())
            .unwrap_or_default()
    })
    .await
    .unwrap_or_default()
}

#[derive(Serialize)]
pub struct Space {
    free: u64,      // bytes free on the drive the folder would be on
    drive: String,  // e.g. "C:"
}

/// Free space for an install folder that may not exist yet: measured at its nearest existing parent.
#[tauri::command]
pub fn disk_space(path: String) -> Option<Space> {
    let mut p = Path::new(&path);
    while !p.exists() {
        p = p.parent()?;
    }
    let drive = p.components().next().map(|c| c.as_os_str().to_string_lossy().into_owned()).unwrap_or_default();
    free_bytes(p).map(|free| Space { free, drive })
}

#[cfg(windows)]
fn free_bytes(p: &Path) -> Option<u64> {
    use std::os::windows::ffi::OsStrExt;
    let wide: Vec<u16> = p.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut free: u64 = 0;
    // SAFETY: a NUL-terminated wide path and a valid out pointer; the other two outputs are optional
    let ok = unsafe {
        windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW(wide.as_ptr(), &mut free, std::ptr::null_mut(), std::ptr::null_mut())
    };
    (ok != 0).then_some(free)
}

#[cfg(not(windows))]
fn free_bytes(_: &Path) -> Option<u64> {
    None
}
