// Screen 3, Options: what the PC has (graphics cards, free space where the game will go).

use serde::Serialize;
use std::path::Path;

/// The graphics cards' names (platform.rs: Windows' Win32_VideoController, Linux's Vulkan devices or lspci).
#[tauri::command]
pub async fn gpu_names() -> Vec<String> {
    tauri::async_runtime::spawn_blocking(crate::platform::gpu_names).await.unwrap_or_default()
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
    crate::platform::free_bytes(p).map(|free| Space { free, drive })
}
