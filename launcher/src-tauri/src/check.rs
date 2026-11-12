// Screen 2, Check: the quick checks of the player's files (seconds; nothing is unpacked or changed). The exact
// version check (the SHA-256 of the game code, target.toml [main]) needs the files extracted, so it runs as the first
// step of the build. Formats as in tools/yw4/pfs0.py and xci.py; title IDs and the known DLC from target.toml.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// The repo's target.toml, built in: the one game build this project supports.
const TARGET: &str = include_str!("../../../target.toml");

#[derive(Deserialize)]
pub struct Request {
    mode: String,                   // "files" (NSP/XCI) or "folders" (extracted)
    paths: HashMap<String, String>, // field id -> chosen path
}

#[derive(Serialize)]
pub struct Row {
    label: String,
    state: &'static str, // "ok", "warn" (fine, but note it), "bad" (can't continue)
    detail: String,
}

fn row(label: &str, state: &'static str, detail: impl Into<String>) -> Row {
    Row { label: label.into(), state, detail: detail.into() }
}

struct Target {
    app: u64,
    dlc: HashMap<String, u32>, // sha256 of a content NCA body (past 0xC00) -> add-on index
    romfs_bytes: u64,
}

fn target() -> Target {
    let t: toml::Value = TARGET.parse().expect("target.toml");
    let app = u64::from_str_radix(t["title"]["application_id"].as_str().unwrap_or("0"), 16).unwrap_or(0);
    let mut dlc = HashMap::new();
    if let Some(table) = t.get("dlc").and_then(|d| d.as_table()) {
        for (i, h) in table {
            if let (Ok(i), Some(h)) = (i.parse::<u32>(), h.as_str()) {
                dlc.insert(h.to_lowercase(), i);
            }
        }
    }
    let romfs_bytes = t.get("extracted").and_then(|e| e.get("romfs_bytes")).and_then(|b| b.as_integer()).unwrap_or(0) as u64;
    Target { app, dlc, romfs_bytes }
}

/// One file in an NSP (PFS0) or an XCI's secure partition (HFS0): name, absolute offset, size.
struct Entry {
    name: String,
    offset: u64,
    size: u64,
}

fn read_at(f: &mut File, at: u64, n: usize) -> std::io::Result<Vec<u8>> {
    f.seek(SeekFrom::Start(at))?;
    let mut b = vec![0u8; n];
    f.read_exact(&mut b)?;
    Ok(b)
}

fn u32le(b: &[u8], o: usize) -> u32 { u32::from_le_bytes(b[o..o + 4].try_into().unwrap()) }
fn u64le(b: &[u8], o: usize) -> u64 { u64::from_le_bytes(b[o..o + 8].try_into().unwrap()) }

/// A PFS0 (entry 0x18 bytes) or HFS0 (0x40 bytes) table at `at`.
fn archive(f: &mut File, at: u64, magic: &[u8; 4], entry_size: usize) -> Option<Vec<Entry>> {
    let head = read_at(f, at, 16).ok()?;
    if &head[..4] != magic {
        return None;
    }
    let (count, strtab) = (u32le(&head, 4) as usize, u32le(&head, 8) as usize);
    if count > 4096 || strtab > 1 << 20 {
        return None;
    }
    let table = read_at(f, at + 16, count * entry_size + strtab).ok()?;
    let names = &table[count * entry_size..];
    let data = at + 16 + (count * entry_size + strtab) as u64;
    let mut out = Vec::new();
    for i in 0..count {
        let e = &table[i * entry_size..];
        let name_off = u32le(e, 16) as usize;
        let end = names[name_off.min(names.len())..].iter().position(|&c| c == 0).map_or(names.len(), |p| name_off + p);
        let name = String::from_utf8_lossy(&names[name_off.min(end)..end]).into_owned();
        out.push(Entry { name, offset: data + u64le(e, 0), size: u64le(e, 8) });
    }
    Some(out)
}

/// The files of an NSP, or of an XCI's secure partition (a 0x1000-byte key area in front is allowed).
fn contents(path: &Path) -> Result<(Vec<Entry>, bool), String> {
    let mut f = File::open(path).map_err(|e| format!("can't open it ({e})"))?;
    if let Some(e) = archive(&mut f, 0, b"PFS0", 0x18) {
        return Ok((e, false));
    }
    for start in [0u64, 0x1000] {
        if read_at(&mut f, start + 0x100, 4).ok().as_deref() == Some(b"HEAD") {
            let root = u64le(&read_at(&mut f, start + 0x130, 8).map_err(|e| e.to_string())?, 0);
            let parts = archive(&mut f, start + root, b"HFS0", 0x40).ok_or("its partition table is damaged")?;
            let secure = parts.iter().find(|p| p.name == "secure").ok_or("it has no game partition")?;
            return archive(&mut f, secure.offset, b"HFS0", 0x40).map(|e| (e, true)).ok_or("its game partition is damaged".into());
        }
    }
    Err("it isn't an NSP or XCI".into())
}

/// The bytes of one file inside an NSP or XCI (at most `max`), else None.
fn entry_bytes(path: &Path, e: &Entry, max: u64) -> Option<Vec<u8>> {
    if e.size > max { return None; }
    read_at(&mut File::open(path).ok()?, e.offset, e.size as usize).ok()
}

/// Is this .tik a common ticket, whose title key the build takes from the ticket itself (tools/yw4/extract.py
/// ticket_title_key)? A personalised one needs the key from title.keys.
fn common_ticket(d: &[u8]) -> bool {
    if d.len() < 4 { return false; }
    let (sig, pad) = match u32le(d, 0) {
        0x10000 | 0x10003 => (0x200, 0x3C),
        0x10001 | 0x10004 => (0x100, 0x3C),
        0x10002 | 0x10005 => (0x3C, 0x40),
        _ => return false,
    };
    let body = 4 + sig + pad;
    d.len() >= body + 0x170 && d[body + 0x141] == 0
}

/// The rights IDs of an NSP's tickets that need title.keys: those its own common tickets don't carry the key for.
fn rights_needing_keys(path: &Path, entries: &[Entry]) -> Vec<String> {
    entries.iter().filter(|e| e.name.ends_with(".tik"))
        .filter(|e| !entry_bytes(path, e, 0x1000).is_some_and(|d| common_ticket(&d)))
        .filter_map(|e| Some(e.name.strip_suffix(".tik")?.to_lowercase()))
        .collect()
}

/// Title IDs named by an NSP's metadata XML (<Id>0x...</Id> in its .cnmt.xml), for NSPs without a ticket
/// (2026-11-09: a base game NSP whose NCAs need no title key carried no .tik, and was taken for another game).
fn cnmt_titles(path: &Path, entries: &[Entry]) -> Vec<u64> {
    entries.iter().filter(|e| e.name.ends_with(".cnmt.xml"))
        .filter_map(|e| entry_bytes(path, e, 0x10000))
        .filter_map(|d| {
            let t = String::from_utf8_lossy(&d).to_lowercase();
            let i = t.find("<id>0x")? + 6;
            u64::from_str_radix(t.get(i..i + 16)?, 16).ok()
        })
        .collect()
}

/// Title IDs named by an NSP's tickets (the first 16 hex digits of each .tik's name: its rights ID).
fn ticket_titles(entries: &[Entry]) -> Vec<(u64, String)> {
    entries.iter().filter_map(|e| {
        let rid = e.name.strip_suffix(".tik")?.to_lowercase();
        Some((u64::from_str_radix(rid.get(..16)?, 16).ok()?, rid))
    }).collect()
}

/// The add-on index of one DLC NSP: its content NCA body's SHA-256 looked up in target.toml [dlc].
pub fn dlc_index_of(path: &Path) -> Option<u32> {
    dlc_index(path, &target())
}

/// One file in a DLC folder: its add-on index, or why the game leaves it out (shown on the DLC tab).
pub fn dlc_check(path: &Path) -> Result<u32, &'static str> {
    if path.is_dir() || !path.extension().map_or(false, |x| x.eq_ignore_ascii_case("nsp")) {
        return Err("The game only reads .nsp files");
    }
    let Ok((entries, false)) = contents(path) else { return Err("Can't be read as an NSP file") };
    if let Some(i) = dlc_index(path, &target()) {
        return Ok(i);
    }
    // An add-on's content is at most 222 MiB (the 4++ Upgrade Pack); the game's and update's are GiBs
    if entries.iter().any(|e| e.name.ends_with(".nca") && e.size > 256 << 20) {
        return Err("The game or an update, not an add-on");
    }
    Err("Not a known copy of an add-on of this game")
}

/// This install already has all the game files from its last build: the same test as the build's own
/// (tools/yw4/build_install.py game_files), so what this says is what the build will do.
#[tauri::command]
pub async fn extracted_files(install: String) -> bool {
    tauri::async_runtime::spawn_blocking(move || {
        let data = Path::new(&install).join("data");
        data.join("exefs").join("main").is_file() && folder_bytes(&data.join("romfs")).0 == target().romfs_bytes
    })
    .await
    .unwrap_or(false)
}

/// An add-on's title ID from its index: the application's + 0x1000 + index (target.toml).
pub fn dlc_title_id(index: u32) -> String {
    format!("{:016X}", target().app + 0x1000 + index as u64)
}

pub fn folder_size(dir: &Path) -> (u64, u64) {
    folder_bytes(dir)
}

fn dlc_index(path: &Path, t: &Target) -> Option<u32> {
    let (entries, _) = contents(path).ok()?;
    let mut f = File::open(path).ok()?;
    // The body past the 0xC00-byte header is hashed (decision 0049): ticket-removal tools rewrite the header and
    // rename the file, so neither the name nor the whole file identifies an add-on.
    const HEADER: u64 = 0xC00;
    for e in entries.iter().filter(|e| {
        e.name.ends_with(".nca") && !e.name.ends_with(".cnmt.nca") && e.size > HEADER && e.size <= 256 << 20
    }) {
        f.seek(SeekFrom::Start(e.offset + HEADER)).ok()?;
        let mut h = Sha256::new();
        std::io::copy(&mut (&mut f).take(e.size - HEADER), &mut h).ok()?;
        let hex: String = h.finalize().iter().map(|b| format!("{b:02x}")).collect();
        if let Some(&i) = t.dlc.get(&hex) {
            return Some(i);
        }
    }
    None
}

fn dlc_row(dir: Option<&String>, has_keys: bool, t: &Target) -> Row {
    let Some(dir) = dir.filter(|d| !d.is_empty()) else {
        return row("DLC", "ok", "None chosen. You can add it later without rebuilding");
    };
    let Ok(list) = std::fs::read_dir(dir) else { return row("DLC", "bad", "Can't open the DLC folder") };
    let mut found = std::collections::BTreeSet::new();
    let mut nsps = 0;
    for p in list.flatten().map(|e| e.path()) {
        if p.extension().map_or(false, |x| x.eq_ignore_ascii_case("nsp")) {
            nsps += 1;
            if let Some(i) = dlc_index(&p, t) {
                found.insert(i);
            }
        }
    }
    // NSPs that failed the check: the DLC tab names each with its reason after the build
    let out = if nsps > found.len() { format!(" · {} not recognised (left out)", nsps - found.len()) } else { String::new() };
    match found.len() {
        0 if nsps == 0 => row("DLC", "warn", "No NSP files in that folder"),
        0 => row("DLC", "warn", format!("No known add-ons of this game in that folder{out}")),
        n if found.contains(&100) && !has_keys => row("DLC", "warn",
            format!("{n} of {} add-ons found{out} · Yo-kai Watch 4++ Upgrade needs prod.keys (choose Keys folder or add later)", t.dlc.len())),
        n => row("DLC", "ok", format!("{n} of {} add-ons found and checked{out}", t.dlc.len())),
    }
}

fn keys_row(dir: Option<&String>, rights: &[String]) -> Row {
    let Some(dir) = dir.filter(|d| !d.is_empty()) else { return row("Keys", "bad", "Choose the folder with prod.keys") };
    let dir = Path::new(dir);
    if !dir.join("prod.keys").is_file() {
        return row("Keys", "bad", "No prod.keys in that folder");
    }
    if rights.is_empty() {
        return row("Keys", "ok", "prod.keys found");
    }
    let title_keys = std::fs::read_to_string(dir.join("title.keys")).unwrap_or_default().to_lowercase();
    let missing = rights.iter().filter(|r| !title_keys.lines().any(|l| l.trim_start().starts_with(r.as_str()))).count();
    if missing == 0 {
        row("Keys", "ok", "prod.keys and the title keys these files need")
    } else if title_keys.is_empty() {
        row("Keys", "bad", "The NSP files also need title.keys in that folder")
    } else {
        row("Keys", "bad", format!("title.keys lacks {missing} key(s) these files need"))
    }
}

fn check_files(paths: &HashMap<String, String>, t: &Target) -> Vec<Row> {
    let upd_id = t.app | 0x800;
    let mut rows = Vec::new();
    let mut rights = Vec::new();
    let base_path = paths.get("base").filter(|p| !p.is_empty());
    let mut base_is_xci = false;
    match base_path.map(|p| contents(Path::new(p))) {
        None => rows.push(row("Base game", "bad", "Choose the base game")),
        Some(Err(e)) => rows.push(row("Base game", "bad", format!("This file can't be used: {e}"))),
        Some(Ok((entries, xci))) => {
            base_is_xci = xci;
            let titles = ticket_titles(&entries);
            let path = Path::new(base_path.unwrap());
            let named = cnmt_titles(path, &entries);
            if xci {
                rows.push(row("Base game", "ok", "Game card image (XCI)"));
            } else if titles.iter().any(|(id, _)| *id == t.app) || named.contains(&t.app) {
                rows.push(row("Base game", "ok", format!("Yo-kai Watch 4++ · {:016X}", t.app)));
            } else if titles.is_empty() && named.is_empty() {
                rows.push(row("Base game", "warn", "No ticket or metadata to check it by: checked while building"));
            } else {
                rows.push(row("Base game", "bad", "This NSP isn't Yo-kai Watch 4++"));
            }
            rights.extend(rights_needing_keys(path, &entries));
        }
    }
    match paths.get("update").filter(|p| !p.is_empty()).map(|p| contents(Path::new(p))) {
        None if base_is_xci => rows.push(row("Update", "warn", "None chosen: fine if your XCI includes 2.2.0 (checked while building)")),
        None => rows.push(row("Update", "bad", "Choose the 2.2.0 update NSP")),
        Some(Err(e)) => rows.push(row("Update", "bad", format!("This file can't be used: {e}"))),
        Some(Ok((entries, _))) => {
            let titles = ticket_titles(&entries);
            let path = Path::new(paths.get("update").unwrap());
            let named = cnmt_titles(path, &entries);
            if titles.iter().any(|(id, _)| *id == upd_id) || named.contains(&upd_id) {
                rows.push(row("Update", "ok", format!("An update of this game · {upd_id:016X}")));
            } else if titles.is_empty() && named.is_empty() {
                rows.push(row("Update", "warn", "No ticket or metadata to check it by: checked while building"));
            } else {
                rows.push(row("Update", "bad", "This NSP isn't an update of Yo-kai Watch 4++"));
            }
            rights.extend(rights_needing_keys(path, &entries));
        }
    }
    let krow = keys_row(paths.get("keys"), &rights);
    let has_keys = krow.state == "ok";
    rows.push(krow);
    rows.push(dlc_row(paths.get("dlc"), has_keys, t));
    rows
}

fn folder_bytes(dir: &Path) -> (u64, u64) {
    let (mut bytes, mut files) = (0u64, 0u64);
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).into_iter().flatten().flatten() {
            match e.file_type() {
                Ok(t) if t.is_dir() => stack.push(e.path()),
                Ok(_) => { bytes += e.metadata().map(|m| m.len()).unwrap_or(0); files += 1; }
                Err(_) => {}
            }
        }
    }
    (bytes, files)
}

fn check_folders(paths: &HashMap<String, String>, t: &Target) -> Vec<Row> {
    let mut rows = Vec::new();
    match paths.get("exefs").filter(|p| !p.is_empty()).map(Path::new) {
        None => rows.push(row("ExeFS", "bad", "Choose the ExeFS folder")),
        Some(d) if d.join("main").is_file() && d.join("sdk").is_file() => rows.push(row("ExeFS", "ok", "main and sdk found")),
        Some(_) => rows.push(row("ExeFS", "bad", "No main and sdk in that folder")),
    }
    match paths.get("romfs").filter(|p| !p.is_empty()).map(Path::new) {
        None => rows.push(row("RomFS", "bad", "Choose the RomFS folder")),
        Some(d) => {
            let (bytes, files) = folder_bytes(d);
            if files == 0 {
                rows.push(row("RomFS", "bad", "That folder is empty"));
            } else if bytes == t.romfs_bytes {
                rows.push(row("RomFS", "ok", format!("{files} files, the exact size of 2.2.0")));
            } else {
                rows.push(row("RomFS", "bad", format!("{files} files, but not the size of 2.2.0 with the update applied")));
            }
        }
    }
    let mut has_keys = false;
    if let Some(k) = paths.get("keys").filter(|p| !p.is_empty()) {
        let r = keys_row(Some(k), &[]);
        has_keys = r.state == "ok";
        rows.push(r);
    }
    rows.push(dlc_row(paths.get("dlc"), has_keys, t));
    rows
}

/// The quick checks of screen 2. Runs off the window's thread (the RomFS walk and DLC hashes take a moment).
#[tauri::command]
pub async fn check_game(req: Request) -> Vec<Row> {
    tauri::async_runtime::spawn_blocking(move || {
        let t = target();
        let mut rows = if req.mode == "folders" { check_folders(&req.paths, &t) } else { check_files(&req.paths, &t) };
        rows.push(row("Version", "ok", "Checked exactly while building (the game code's hash)"));
        rows
    })
    .await
    .unwrap_or_default()
}
