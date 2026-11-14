// What differs between Windows and Linux (decision 0071), in one place: the rest of the launcher calls these and
// never names a Windows or Linux program itself. On Windows each one does exactly what the launcher did before.
//
//   data_dir      the launcher's own records and the "appdata" saves: %APPDATA%\YW4 Recomp on Windows,
//                 $XDG_DATA_HOME/YW4Recomp (~/.local/share/YW4Recomp) on Linux, the folder the game itself uses
//                 for its default saves (runtime/src/main.c default_save_dir)
//   open, reveal  a link, a folder or a file in the player's browser or file manager: explorer / xdg-open
//   tool          the system's curl and tar: Windows' own copies in System32 (never one earlier on the PATH),
//                 on Linux the ones on the PATH
//   kill_tree     a build step and every process it started: taskkill /T, or the step's own process group
//   python        the build's Python when the launcher runs from the repo: python / python3

use std::ffi::OsStr;
use std::path::{Path, PathBuf};
use std::process::Command;

/// The game's file name in its build folder (tools/yw4/build_install.py writes it under the same name).
pub const GAME_EXE: &str = if cfg!(windows) { "yw4.exe" } else { "yw4" };

/// No console window flashing up for a tool the launcher starts (Windows); nothing to do on Linux.
pub fn hidden(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    { use std::os::windows::process::CommandExt; cmd.creation_flags(0x0800_0000); }
    cmd
}

/// The launcher's own folder for its records (launcher.json, play time, the update cache) and the "appdata" saves.
pub fn data_dir() -> PathBuf {
    #[cfg(windows)]
    { PathBuf::from(std::env::var("APPDATA").unwrap_or_default()).join("YW4 Recomp") }
    #[cfg(not(windows))]
    {
        // as the game: an XDG_DATA_HOME that isn't absolute is to be ignored (XDG Base Directory spec)
        match std::env::var("XDG_DATA_HOME") {
            Ok(x) if x.starts_with('/') => PathBuf::from(x).join("YW4Recomp"),
            _ => PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".local/share/YW4Recomp"),
        }
    }
}

/// Opens a link or a folder the way a double-click would.
pub fn open(target: impl AsRef<OsStr>) -> Result<(), String> {
    let prog = if cfg!(windows) { "explorer" } else { "xdg-open" };
    Command::new(prog).arg(target).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// The file manager on the folder holding `file`, with the file selected where the system can do that (Windows).
pub fn reveal(file: &Path) -> Result<(), String> {
    #[cfg(windows)]
    { Command::new("explorer").arg(format!("/select,{}", file.display())).spawn().map(|_| ()).map_err(|e| e.to_string()) }
    #[cfg(not(windows))]
    { open(file.parent().unwrap_or(file)) }
}

/// A system tool by its plain name ("curl", "tar").
pub fn tool(name: &str) -> PathBuf {
    #[cfg(windows)]
    {
        let root = std::env::var("SystemRoot").unwrap_or_else(|_| r"C:\Windows".into());
        PathBuf::from(root).join("System32").join(format!("{name}.exe"))
    }
    #[cfg(not(windows))]
    { PathBuf::from(name) }
}

/// The Python that runs the build from the repo (a launcher without a bundled builder).
pub fn python() -> PathBuf {
    PathBuf::from(if cfg!(windows) { "python" } else { "python3" })
}

/// Makes `cmd` easy to stop with everything it starts: on Linux it gets its own process group (kill_tree ends the
/// whole group); Windows' taskkill /T follows the process tree by itself.
pub fn own_group(cmd: &mut Command) -> &mut Command {
    #[cfg(unix)]
    { use std::os::unix::process::CommandExt; cmd.process_group(0); }
    cmd
}

/// Ends a process started with own_group and everything it started.
pub fn kill_tree(pid: u32) {
    #[cfg(windows)]
    { let _ = hidden(Command::new("taskkill").args(["/T", "/F", "/PID", &pid.to_string()])).status(); }
    #[cfg(not(windows))]
    { let _ = Command::new("kill").args(["-KILL", "--", &format!("-{pid}")]).status(); }
}

/// Is any copy of the game running?
pub fn game_running() -> bool {
    #[cfg(windows)]
    {
        hidden(Command::new("tasklist").args(["/FI", "IMAGENAME eq yw4.exe", "/NH"])).output()
            .map(|o| String::from_utf8_lossy(&o.stdout).to_lowercase().contains("yw4.exe")).unwrap_or(false)
    }
    #[cfg(not(windows))]
    { Command::new("pgrep").args(["-x", GAME_EXE]).output().map(|o| o.status.success()).unwrap_or(false) }
}

/// The error an open-for-writing of a running program gives: Windows 5 / 32 (access denied, sharing violation),
/// Linux ETXTBSY (26, "text file busy").
pub fn busy_error(code: Option<i32>) -> bool {
    if cfg!(windows) { matches!(code, Some(5) | Some(32)) } else { code == Some(26) }
}

/// Unpacks a zip into `dir`. Windows' own tar.exe (bsdtar) reads zips; GNU tar on Linux doesn't, so there Python's
/// zipfile does it (the build needs Python 3 anyway), which also refuses absolute paths and "..".
pub fn unzip(zip: &Path, dir: &Path) -> bool {
    #[cfg(windows)]
    { hidden(Command::new(tool("tar")).arg("-xf").arg(zip).arg("-C").arg(dir)).status().map(|s| s.success()).unwrap_or(false) }
    #[cfg(not(windows))]
    {
        Command::new(python()).args(["-c", "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])"])
            .arg(zip).arg(dir).status().map(|s| s.success()).unwrap_or(false)
    }
}

/// Free bytes on the drive holding `p` (an existing path).
pub fn free_bytes(p: &Path) -> Option<u64> {
    #[cfg(windows)]
    {
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
    {
        // coreutils' df (every desktop distribution has it): bytes available to this user
        let out = Command::new("df").args(["-B1", "--output=avail"]).arg(p).output().ok()?;
        String::from_utf8_lossy(&out.stdout).lines().nth(1)?.trim().parse().ok()
    }
}

/// The graphics cards' names, for the Options screen (which offers DLSS only with an RTX card).
pub fn gpu_names() -> Vec<String> {
    #[cfg(windows)]
    let out = hidden(Command::new("powershell").args(["-NoProfile", "-NonInteractive", "-Command",
        "Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name }"])).output();
    // Linux: the Vulkan devices' names when vulkaninfo is there (the names the game's log shows), else lspci's
    // display controllers
    #[cfg(not(windows))]
    let out = Command::new("sh").args(["-c",
        "vulkaninfo --summary 2>/dev/null | sed -n 's/^[[:space:]]*deviceName[[:space:]]*= //p' | grep -v llvmpipe \
         || lspci 2>/dev/null | sed -n 's/^[^ ]* \\(VGA compatible controller\\|3D controller\\|Display controller\\): //p'"]).output();
    out.ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).lines().map(str::trim).filter(|l| !l.is_empty()).map(String::from).collect())
        .unwrap_or_default()
}

/// Desktop and app-menu shortcuts to the launcher: `.lnk` files on Windows (Desktop, Start menu), `.desktop` files
/// on Linux (the desktop folder, ~/.local/share/applications). `on` false removes our own shortcut there only.
pub fn shortcut(desktop: bool, on: bool, target: &Path, icon: &Path) -> Result<(), String> {
    #[cfg(windows)]
    {
        let place = if desktop { "[Environment]::GetFolderPath('Desktop')" } else { "[Environment]::GetFolderPath('Programs')" };
        if !on {
            let script = format!("Remove-Item -LiteralPath (Join-Path ({place}) 'Yo-kai Watch 4 Recomp.lnk') -ErrorAction SilentlyContinue");
            let _ = hidden(Command::new("powershell").args(["-NoProfile", "-NonInteractive", "-Command", &script])).status();
            return Ok(());
        }
        // ({place}): a .NET call given straight to a command is read as text, not run (Join-Path then fails and no
        // shortcut was ever made: found 2026-11-04 in the first full Home -> Ready test)
        let script = format!(
            "$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path ({place}) 'Yo-kai Watch 4 Recomp.lnk')); \
             $s.TargetPath=$env:YW4_LNK_TARGET; $s.WorkingDirectory=(Split-Path $env:YW4_LNK_TARGET); \
             $s.IconLocation=$env:YW4_LNK_ICON; $s.Save()");
        let ok = hidden(Command::new("powershell").args(["-NoProfile", "-NonInteractive", "-Command", &script])
            .env("YW4_LNK_TARGET", target).env("YW4_LNK_ICON", icon)).status().map(|s| s.success()).unwrap_or(false);
        if ok { Ok(()) } else { Err("couldn't make a shortcut".into()) }
    }
    #[cfg(not(windows))]
    {
        let home = PathBuf::from(std::env::var("HOME").unwrap_or_default());
        let dir = if desktop {
            // the desktop folder's name follows the system's language: xdg-user-dir knows it
            Command::new("xdg-user-dir").arg("DESKTOP").output().ok()
                .map(|o| PathBuf::from(String::from_utf8_lossy(&o.stdout).trim()))
                .filter(|p| p.is_absolute())
                .unwrap_or_else(|| home.join("Desktop"))
        } else {
            match std::env::var("XDG_DATA_HOME") {
                Ok(x) if x.starts_with('/') => PathBuf::from(x).join("applications"),
                _ => home.join(".local/share/applications"),
            }
        };
        let file = dir.join("yw4-recomp.desktop");
        if !on {
            let _ = std::fs::remove_file(&file);
            return Ok(());
        }
        if desktop && !dir.is_dir() { return Ok(()); }  // no desktop folder (a desktop without icons): nothing to do
        // an AppImage runs from a temporary mount: the shortcut has to start the AppImage file itself
        let target = std::env::var_os("APPIMAGE").map(PathBuf::from).unwrap_or_else(|| target.to_path_buf());
        // Exec quoting (Desktop Entry spec): the whole path in double quotes, with \ " ` $ escaped
        let quoted: String = target.display().to_string().chars()
            .flat_map(|c| if matches!(c, '\\' | '"' | '`' | '$') { vec!['\\', c] } else { vec![c] }).collect();
        let text = format!(
            "[Desktop Entry]\nType=Application\nName=Yo-kai Watch 4 Recomp\nComment=Sets up, builds and starts YW4 Recomp\n\
             Exec=\"{quoted}\"\nIcon={}\nTerminal=false\nCategories=Game;\n", icon.display());
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        std::fs::write(&file, text).map_err(|e| e.to_string())?;
        // GNOME starts a desktop-folder launcher only when it is executable
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&file, std::fs::Permissions::from_mode(0o755));
        Ok(())
    }
}
