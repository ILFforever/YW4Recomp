// The launcher's screens. The Rust side is reached through window.__TAURI__ (tauri.conf.json withGlobalTauri).
const T = window.__TAURI__ || null;
const invoke = T ? T.core.invoke : null;
const $ = (id) => document.getElementById(id);
// Linux (decision 0071): WebKitGTK names it in its user agent. Frame generation and Reflex are Windows only (DLSS and
// FSR upscaling aren't, decision 0073), so their mentions go (.win-only, style.css); shortcut, saves and folder words
// follow the system. Nothing changes on Windows.
const LINUX = /Linux/.test(navigator.userAgent) && !/Windows|Android/.test(navigator.userAgent);
if (LINUX) document.documentElement.classList.add('os-linux');
const SAVES_PLACE = LINUX ? 'your home folder' : 'AppData';
if (LINUX) {
  document.querySelectorAll('.saves-place').forEach((b) => { b.textContent = 'In your home folder'; });
  document.querySelectorAll('.startmenu-name').forEach((b) => { b.textContent = 'App menu entry'; });
  $('set-startmenu').setAttribute('aria-label', 'App menu entry');
  document.querySelectorAll('#hero .slide.win-only').forEach((img) => img.remove());  // the slideshow counts what's left
}
// the focus ring only after keyboard use (style.css body.pointer): a click hides it, a key brings it back
document.addEventListener('pointerdown', () => document.body.classList.add('pointer'), true);
document.addEventListener('keydown', () => document.body.classList.remove('pointer'), true);

// ---- screens: one shown at a time; the setup ones light up their step in the top bar
const STEPS = ['files', 'check', 'options', 'build', 'play'];

function show(name) {
  for (const s of document.querySelectorAll('.screen')) s.hidden = s.dataset.screen !== name;
  const step = STEPS.indexOf(name);
  $('steps-bar').hidden = step < 0;
  document.querySelectorAll('#steps-bar li').forEach((li, i) => {
    li.className = i < step ? 'done' : i === step ? 'now' : '';
  });
}
document.querySelectorAll('[data-go]').forEach((b) => b.addEventListener('click', () => show(b.dataset.go)));

// ---- top bar (the launcher's own title bar: Windows' is off), status bar
$('github').addEventListener('click', () => invoke && invoke('open_project_page'));

// Help (the top bar's ?): from any screen; Back returns to it. A build keeps running underneath.
let helpFrom = 'home';
const currentScreen = () => (document.querySelector('.screen:not([hidden])') || {}).dataset?.screen || 'home';
$('help-btn').addEventListener('click', async () => {
  if (currentScreen() === 'help') return;
  helpFrom = currentScreen();
  show('help');
  $('help-logs').disabled = !(typeof inst !== 'undefined' && inst);  // the logs live in the game folder
  let lic = false;
  try { lic = invoke ? await invoke('licences_folder', { open: false }) : false; } catch (e) { /* none */ }
  $('help-licences').hidden = !lic;  // a launcher built from the repo has none next to it
});
$('help-back').addEventListener('click', () => show(helpFrom));
$('help-logs').addEventListener('click', () => invoke('open_folder', { install: inst.install, which: 'logs', saves: inst.saves }));
$('help-licences').addEventListener('click', () => invoke('licences_folder', { open: true }));
document.querySelectorAll('[data-link]').forEach((b) => b.addEventListener('click', () => invoke('open_link', { which: b.dataset.link })));
// the top bar's gear: the Settings page once the game is built. Until then it is disabled (syncSettingsButton).
$('top-settings').addEventListener('click', () => showSettings());
function syncSettingsButton() {  // inst is the built install's state, null before a build
  const gear = $('top-settings');
  gear.disabled = !inst;
  gear.title = inst ? 'Settings' : 'Settings are here once your game is built';
}
// Locate an installed game (a folder built earlier, or moved): the folder picker, the folder checked and kept as this
// launcher's install (locate_installed_game), then its Ready screen. Any button with data-locate does this.
async function locateGame() {
  let found;
  try { found = await invoke('locate_installed_game', {}); } catch (e) {
    if (!/No folder chosen/.test(String(e))) popup('No game found there', String(e));
    return;
  }
  inst = found;
  syncSettingsButton();
  showReady();
}
document.querySelectorAll('[data-locate]').forEach((b) => b.addEventListener('click', locateGame));

if (T && T.window) {
  const win = T.window.getCurrentWindow();
  $('win-min').addEventListener('click', () => win.minimize());
  $('win-close').addEventListener('click', () => win.close());
  // no maximise (owner, 2026-11-05: the launcher never fills the screen; tauri.conf.json caps its size too)
}
(async () => {
  if (!invoke) return;
  try { $('launcher-version').textContent = 'Launcher ' + await invoke('launcher_version'); } catch (e) { console.error(e); }
})();

// ---- placeholder for what isn't built yet
const soon = $('soon');
$('soon-ok').addEventListener('click', () => soon.close());
// the small message popup: a title and one line
function popup(title, text) {
  soon.querySelector('h2').textContent = title;
  soon.querySelector('p').textContent = text;
  soon.showModal();
}

// ---- Home
$('start').addEventListener('click', () => show('files'));

// the hero: a slide every 6 s with its feature line; dots pick one; hovering pauses; no auto-play when Windows
// asks for less motion
(() => {
  const slides = [...document.querySelectorAll('#hero .slide')];
  const dots = slides.map((s, i) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.setAttribute('aria-label', s.dataset.caption);
    b.addEventListener('click', () => go(i));
    return b;
  });
  $('hero-dots').append(...dots);
  let now = 0;
  function go(i) {
    now = (i + slides.length) % slides.length;
    slides.forEach((s, k) => s.classList.toggle('on', k === now));
    dots.forEach((d, k) => d.setAttribute('aria-current', String(k === now)));
    $('hero-cap').textContent = slides[now].dataset.caption;
  }
  go(0);
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
  let paused = false;
  $('hero').addEventListener('mouseenter', () => { paused = true; });
  $('hero').addEventListener('mouseleave', () => { paused = false; });
  setInterval(() => { if (!paused && !$('hero').closest('.screen').hidden) go(now + 1); }, 6000);
})();

// ---- 1 · Game files
// What each kind of dump needs: required fields first. `pick`: what the picker chooses.
const FIELDS = {
  files: [
    { id: 'base', label: 'Base game', note: 'NSP or XCI, any version', pick: 'file', required: true },
    { id: 'update', label: 'Update 2.2.0', note: "NSP · not needed if your XCI has it", pick: 'nsp' },
    { id: 'keys', label: 'Keys folder', note: 'Holds prod.keys', pick: 'folder', required: true },
    { id: 'dlc', label: 'DLC folder', note: 'Optional', pick: 'folder' },
  ],
  folders: [
    { id: 'exefs', label: 'ExeFS folder', note: 'Has main and sdk', pick: 'folder', required: true },
    { id: 'romfs', label: 'RomFS folder', note: 'Update applied', pick: 'folder', required: true },
    { id: 'dlc', label: 'DLC folder', note: 'Optional', pick: 'folder' },
    { id: 'keys', label: 'Keys folder', note: 'Optional · for the 4++ Upgrade Pack', pick: 'folder' },
  ],
};
const NOTES = {
  files: 'An NSP also needs title.keys in the keys folder.',
  folders: 'These folders are copied, not changed.',
};
const chosen = { files: {}, folders: {} };  // what the player picked, per mode (kept when switching back and forth)
let mode = 'files';
let rebuildFlow = false;  // these steps were opened by Settings' Rebuild, for an install that already has a game
$('files-back').addEventListener('click', () => { if (rebuildFlow) showSettings(); else show('home'); });
// A rebuild of an install with all its game files: a third choice on Game files, chosen first. With it the originals
// aren't needed and the build skips unpacking; with another, the usual fields, and the build extracts or copies again.
let foundFiles = false, useFound = true, freshFiles = false;
let foundFrom = null;  // using them for a build in another folder: that folder has none, so they're copied from here
const usingFound = () => rebuildFlow && foundFiles && useFound;
function renderFound() {
  $('mode-found').hidden = !(rebuildFlow && foundFiles);
  document.querySelectorAll('.mode').forEach((m) => m.setAttribute('aria-checked',
    String(m.dataset.mode === 'found' ? usingFound() : !usingFound() && m.dataset.mode === mode)));
  $('fields').hidden = usingFound();
  $('found-note').hidden = !usingFound();
  updateCheck();
}

function renderFields() {
  const box = $('fields');
  box.textContent = '';
  for (const f of FIELDS[mode]) {
    const row = document.createElement('div');
    row.className = 'field';
    const label = document.createElement('label');
    label.htmlFor = 'f-' + f.id;
    label.innerHTML = '<b></b><small></small>';
    label.querySelector('b').textContent = f.label;
    label.querySelector('small').textContent = f.note;
    const input = document.createElement('input');
    input.id = 'f-' + f.id;
    input.readOnly = true;
    input.value = chosen[mode][f.id] || '';
    input.placeholder = f.required ? 'Not chosen' : 'None';
    const browse = document.createElement('button');
    browse.type = 'button';
    browse.className = 'btn';
    browse.textContent = 'Browse';
    browse.addEventListener('click', () => pick(f));
    row.append(label, input, browse);
    box.append(row);
  }
  $('files-note').textContent = NOTES[mode];
  updateCheck();
}

async function pick(f) {
  if (!T || !T.dialog) return;
  const opts = { title: f.label, multiple: false, directory: f.pick === 'folder' };
  if (f.pick === 'file') opts.filters = [{ name: 'NSP or XCI', extensions: ['nsp', 'xci'] }];
  if (f.pick === 'nsp') opts.filters = [{ name: 'NSP', extensions: ['nsp'] }];
  const path = await T.dialog.open(opts);
  if (typeof path === 'string' && path) {
    chosen[mode][f.id] = path;
    renderFields();
  }
}

// "Check my files": a dashed slot with "N left" until the required rows are chosen, then the gold button with an
// arrow. The note says what's missing; clicking early marks the missing rows.
const ARROW = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.8" ' +
  'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 12h14M13 6l6 6-6 6"/></svg>';
const missingFields = () => (usingFound() ? [] : FIELDS[mode].filter((f) => f.required && !chosen[mode][f.id]));

function updateCheck() {
  const missing = missingFields();
  const btn = $('check');
  btn.classList.toggle('not-ready', missing.length > 0);
  btn.innerHTML = 'Check my files ' + (missing.length
    ? '<span class="left-badge">' + missing.length + ' left</span>'
    : ARROW);
  btn.setAttribute('aria-label', missing.length ? `Check my files (${missing.length} left)` : 'Check my files');
  $('files-note').textContent = missing.length
    ? 'Choose the ' + missing.map((f) => f.label.toLowerCase()).join(' and the ') + '.'
    : usingFound() ? '' : NOTES[mode];
}

function markMissing() {
  const ids = new Set(missingFields().map((f) => 'f-' + f.id));
  document.querySelectorAll('#fields .field').forEach((row) => {
    row.classList.toggle('missing', ids.has(row.querySelector('input').id));
  });
}

document.querySelectorAll('.mode').forEach((b) => b.addEventListener('click', () => {
  useFound = b.dataset.mode === 'found';
  if (!useFound) mode = b.dataset.mode;  // "mode" stays NSP or folders: what the install's paths are
  renderFields();
  renderFound();
}));
$('check').addEventListener('click', () => {
  if (missingFields().length) { markMissing(); return; }
  show('check');
  runCheck();
});

// ---- 2 · Check: the quick checks (Rust: check.rs); the exact version check runs as the build's first step
const MARKS = {
  ok: '<path d="M5 12.5l4.5 4.5L19 7"/>',
  warn: '<path d="M12 7v6M12 17v.01"/>',
  bad: '<path d="M7 7l10 10M17 7L7 17"/>',
};
let checkBad = 0;

function resultRow(state, label, detail) {
  const li = document.createElement('li');
  const mark = document.createElement('span');
  mark.className = 'mark ' + state;
  if (MARKS[state]) {
    mark.innerHTML = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" ' +
      'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + MARKS[state] + '</svg>';
  }
  const b = document.createElement('b');
  b.textContent = label;
  const d = document.createElement('span');
  d.className = 'detail';
  d.textContent = detail;
  li.append(mark, b, d);
  return li;
}

function setContinue(bad) {
  checkBad = bad;
  const btn = $('continue');
  btn.classList.toggle('not-ready', bad > 0);
  btn.innerHTML = 'Continue ' + (bad ? `<span class="left-badge">${bad} to fix</span>` : ARROW);
}

async function runCheck() {
  const list = $('results');
  list.textContent = '';
  const labels = mode === 'files' ? ['Base game', 'Update', 'Keys', 'DLC', 'Version'] : ['ExeFS', 'RomFS', 'DLC', 'Version'];
  for (const l of labels) list.append(resultRow('wait', l, 'Checking…'));
  $('check-title').textContent = 'Checking your files…';
  $('check-lead').textContent = 'A few seconds. Nothing is changed.';
  $('check-note').textContent = '';
  setContinue(1);
  let rows = [];
  try {
    rows = usingFound()
      ? [{ label: 'Game files', state: 'ok', detail: 'From your last build, the exact size of 2.2.0' },
        { label: 'Version', state: 'ok', detail: "Checked exactly while building (the game code's hash)" }]
      : await invoke('check_game', { req: { mode, paths: chosen[mode] } });
  } catch (e) {
    rows = [{ label: 'Check', state: 'bad', detail: String(e) }];
  }
  list.textContent = '';
  for (const r of rows) list.append(resultRow(r.state, r.label, r.detail));
  const bad = rows.filter((r) => r.state === 'bad').length;
  $('check-title').textContent = bad ? 'Something needs fixing' : 'All set';
  $('check-lead').textContent = bad ? 'Go back and fix the rows marked with a cross.' : 'Your files are ready to build.';
  $('check-note').textContent = bad ? '' : 'Takes about 10–15 minutes.';
  setContinue(bad);
}

$('continue').addEventListener('click', () => {
  if (checkBad) return;
  optionsFromPlay = false;
  show('options');
  loadOptions();
});

// ---- 3 · Options: downloads (by the graphics card), where the game goes, saves, shortcuts
const GB = 1024 ** 3;
const options = {
  fsr: true, dlss: false, freeUp: false, desktop: true, startMenu: false,
  install: LINUX ? '' : 'C:\\Games\\YW4 Recomp', saves: 'install',
};
// Linux: ~/Games/YW4 Recomp, once the home folder is known
if (LINUX && T && T.path) T.path.homeDir().then((h) => {
  if (!options.install) { options.install = h.replace(/\/$/, '') + '/Games/YW4 Recomp'; renderSummary(); }
}).catch(() => {});
let rtx = false;
let spaceFree = null;
// A download brings everything but the compiler, which the first build fetches (decision 0052): 182 MB, 737 MB
// unpacked. A launcher built from the repo uses the repo's compiler.
let bundled = false;
if (invoke) invoke('builder_bundled').then((b) => { bundled = b; renderSwitches(); renderSummary(); });
// What the install folder takes: the copied RomFS (8.5 GB) + ExeFS, the tools (1.5 GB) and build files (0.7 GB),
// the game itself, and the downloads (target.toml [extracted], measured sizes: FSR 240 MB, DLSS + Streamline 325 MB)
// Linux downloads no compiler: the system's builds (tools/yw4/system.py)
// Linux's DLSS and FSR are upscaling only (decision 0073): NVIDIA's library alone, no Streamline; AMD's SDK source (built
// on the PC, about a minute)
const downloadMB = () => (bundled && !LINUX ? 182 : 0) + (options.fsr ? (LINUX ? 200 : 240) : 0) + (options.dlss ? (LINUX ? 60 : 325) : 0);
const diskNeeded = () => (usingFound() && options.install === inst.install ? 0 : 8.5 * GB) + 2.4 * GB + (bundled && !LINUX ? 737 * 1024 ** 2 : 0) + downloadMB() * 1024 ** 2;

const SWITCHES = [
  { key: 'fsr', title: 'AMD FSR', note: () => LINUX ? 'Upscaling, any graphics card · about 200 MB'
    : 'Upscaling and frame generation, any graphics card · about 240 MB' },
  { key: 'dlss', title: 'NVIDIA DLSS', note: () => !rtx ? 'Needs an NVIDIA RTX graphics card'
    : LINUX ? 'Upscaling · about 60 MB' : 'Upscaling, frame generation, Reflex · about 325 MB', needsRtx: true },
  { key: 'freeUp', title: 'Free up space after building', note: () => `Deletes the tools and build files (${bundled ? '2 GB' : '1.3 GB'}). Updates then download them and rebuild` },
  { key: 'desktop', title: 'Desktop shortcut', note: () => 'Opens the launcher' },
  { key: 'startMenu', title: LINUX ? 'App menu entry' : 'Start menu entry', note: () => 'Opens the launcher' },
];

function renderSwitches() {
  const box = $('switches');
  box.textContent = '';
  for (const s of SWITCHES) {
    const off = s.needsRtx && !rtx;
    const row = document.createElement('div');
    row.className = 'switch-row' + (off ? ' off-limits' : '');
    const text = document.createElement('span');
    text.innerHTML = '<b></b><small></small>';
    text.querySelector('b').textContent = s.title;
    text.querySelector('small').textContent = s.note();
    const t = document.createElement('button');
    t.type = 'button';
    t.className = 'toggle';
    t.setAttribute('role', 'switch');
    t.setAttribute('aria-label', s.title);
    t.setAttribute('aria-checked', String(!off && options[s.key]));
    t.disabled = off;
    t.addEventListener('click', () => { options[s.key] = !options[s.key]; renderSwitches(); renderSummary(); });
    row.append(text, t);
    box.append(row);
  }
}

// sizes for people: GB from 1 GB up, MB below (a DLC file is ~0.1 MB, an NSP several GB)
const gb = (b) => b >= GB ? (b / GB).toFixed(b >= 100 * GB ? 0 : 1) + ' GB'
  : Math.max(0.1, b / 1024 ** 2).toFixed(b >= 10 * 1024 ** 2 ? 0 : 1) + ' MB';

function renderSummary() {
  $('install-dir').value = options.install;
  $('sum-download').textContent = downloadMB() ? `About ${downloadMB()} MB` : 'Nothing';
  $('sum-disk').textContent = 'About ' + gb(diskNeeded());
  const line = $('space-line');
  const short = spaceFree !== null && spaceFree.free < diskNeeded();
  line.classList.toggle('short', short);
  line.textContent = spaceFree === null ? 'Checking free space…'
    : `${gb(spaceFree.free)} free on ${spaceFree.drive}` + (short ? ' · not enough' : '');
  const btn = $('build-go');
  btn.classList.toggle('not-ready', short);
  btn.innerHTML = 'Build my game ' + (short ? '<span class="left-badge">not enough space</span>' : ARROW);
  $('options-note').textContent = short ? 'Choose a folder on a drive with more space.' : '';
}

async function refreshSpace() {
  spaceFree = null;
  renderSummary();
  try { spaceFree = invoke ? await invoke('disk_space', { path: options.install }) : null; } catch (e) { spaceFree = null; }
  renderSummary();
}

let optionsLoaded = false;
async function loadOptions() {
  renderSwitches();
  refreshSpace();
  if (optionsLoaded) return;
  optionsLoaded = true;
  let names = [];
  try { names = invoke ? await invoke('gpu_names') : []; } catch (e) { names = []; }
  const nvidia = names.find((n) => /nvidia/i.test(n));
  rtx = !!(nvidia && /rtx/i.test(nvidia));
  // on by default on an RTX card; a rebuild keeps what this install chose
  options.dlss = rebuildFlow && inst.dlss !== undefined ? inst.dlss && rtx : rtx;
  $('gpu-name').innerHTML = names.length
    ? 'Your graphics card: <b></b>' : 'No graphics card found';
  if (names.length) $('gpu-name').querySelector('b').textContent = nvidia || names[0];
  renderSwitches();
  renderSummary();
}

$('install-change').addEventListener('click', async () => {
  if (!T || !T.dialog) return;
  const dir = await T.dialog.open({ title: 'Install to', directory: true });
  if (typeof dir === 'string' && dir) {
    let next = /YW4 Recomp$/i.test(dir) ? dir : dir.replace(/[\\/]$/, '') + '\\YW4 Recomp';
    if (rebuildFlow || optionsFromPlay) {  // this install has a game already
      const low = (p) => p.replace(/[\\/]+$/, '').toLowerCase(), old = low(inst.install);
      if (low(dir) === old) next = inst.install;  // its own folder again: no new folder inside it
      else if ((low(next) + '\\').startsWith(old + '\\')) {
        popup('Choose another folder', 'That is inside your game folder. Deleting the old copy would delete the new one too.');
        return;
      } else if (low(next) !== old && !await confirmNewFolder(next)) return;
    }
    options.install = next;
    refreshSpace();
  }
});
// A rebuild into another folder leaves the old copy behind: say how big it is, that it is theirs to delete, and
// that what they'd want to keep is in there.
async function confirmNewFolder(next) {
  let u = { game: 0, saves: 0, shots: 0, mods: 0 };
  try { u = await invoke('uninstall_info', { install: inst.install, saves: inst.saves }); } catch (e) { /* no size then */ }
  const inFolder = inst.saves === 'install', total = u.game + u.shots + u.mods + (inFolder ? u.saves : 0);
  $('newfolder-text').textContent = `The game is built again in ${next}. The old copy stays in ${inst.install} and keeps taking up ${gb(total)}: delete that folder yourself when the new one works.`;
  $('newfolder-keep').textContent = inFolder
    ? 'Your saves, settings, screenshots and mods are in the old folder and the new copy starts without them. Copy them across before you delete it.'
    : `Your screenshots and mods are in the old folder. Copy them across before you delete it. Your saves are safe in ${SAVES_PLACE}.`;
  const dlg = $('newfolder-dlg');
  dlg.showModal();
  return new Promise((done) => {
    const answer = (yes) => () => { dlg.close(); done(yes); };
    $('newfolder-no').onclick = answer(false);
    $('newfolder-yes').onclick = answer(true);
    dlg.oncancel = () => done(false);  // Escape
  });
}
document.querySelectorAll('.radio[data-saves]').forEach((b) => b.addEventListener('click', () => {
  options.saves = b.dataset.saves;
  document.querySelectorAll('.radio[data-saves]').forEach((r) => r.setAttribute('aria-checked', String(r === b)));
}));
$('build-go').addEventListener('click', () => {
  if (spaceFree && spaceFree.free < diskNeeded()) return;
  show('build');
  freshFiles = rebuildFlow && foundFiles && !useFound;
  foundFrom = usingFound() && options.install !== inst.install ? inst.install : null;
  startBuild(optionsFromPlay || rebuildFlow ? 'rebuild' : 'first');
  rebuildFlow = false;
});

// ---- 4 · Build: the steps come from the game code (tools.yw4.build_install, decision 0054): its "@steps" list
// (id, label, measured seconds, and where each progress step sits inside it: [from, to, text, unit]), then
// "@step" and "@progress" lines. Each step's share of the bar is its seconds. The launcher adds only its DLC copy.
// what a downloaded file is, for the line under the bar
function downloadName(file) {
  if (/^SDL/i.test(file)) return 'SDL';
  if (/FidelityFX/i.test(file)) return 'AMD FSR';
  if (/streamline/i.test(file)) return 'NVIDIA Streamline';
  return 'NVIDIA DLSS';
}
// a progress step's place: "<step>:<what>" first (the translator's two halves), then "<step>"
function findPart(p) {
  for (const key of [`${p.step}:${p.what}`, p.step]) {
    for (const s of build.steps) {
      const part = s.parts && s.parts[key];
      if (part) return { step: s, key, from: part[0], to: part[1], text: part[2] || s.label, unit: part[3] || '' };
    }
  }
  return null;
}
function nowText(p, part) {
  if (p.step === 'download') return `Downloading ${downloadName(p.what)} · ${gb(p.done)} of ${gb(p.total)}`;
  const files = /^(\d+)\/(\d+)$/.exec(p.what);
  if (part.unit === 'bytes+files' && files) return `${part.text} · ${(+files[1]).toLocaleString()} of ${(+files[2]).toLocaleString()} files`;
  if (p.total <= 1) return part.text;
  return part.unit.startsWith('bytes') ? `${part.text} · ${gb(p.done)} of ${gb(p.total)}`
    : `${part.text} · ${p.done.toLocaleString()} of ${p.total.toLocaleString()}`;
}
let build = null;

function renderBuildSteps() {
  const list = $('build-steps');
  list.textContent = '';
  for (const s of build.steps) {
    const li = document.createElement('li');
    li.className = s.state === 'start' ? 'now' : s.state === 'done' ? 'done' : '';
    const state = s.state === 'done' ? 'ok' : s.state === 'failed' ? 'bad' : s.state === 'start' ? 'wait' : 'todo';
    const mark = resultRow(state, '', '').firstChild;  // the same marks as screen 2
    const label = document.createElement('span');
    label.textContent = s.label;
    li.append(mark, label);
    list.append(li);
  }
}

function renderBuildBar() {
  const total = build.steps.reduce((a, s) => a + s.weight, 0);
  const done = build.steps.reduce((a, s) => a + s.weight * s.frac, 0);
  const pct = total ? Math.min(100, Math.floor((100 * done) / total)) : 0;
  $('build-pct').textContent = pct + '%';
  $('build-fill').style.width = pct + '%';
  $('build-bar').setAttribute('aria-valuenow', String(pct));
  // Time left, step by step: the part that is running at its own measured speed (once it has moved 5%), the rest of
  // that step and the steps after it at their measured times. A whole-build average was too hopeful: the early
  // steps are quick, compiling is slow.
  const started = build.steps.some((s) => s.state);
  if (!started || done >= total) { $('build-eta').textContent = ''; return; }
  let left = 0;
  for (const s of build.steps) {
    if (s.state === 'done' || s.frac >= 1) continue;
    const m = s.mark;
    if (s.state === 'start' && m && s.frac - m.f > 0.05 * (m.end - m.f)) {
      const rate = (s.frac - m.f) / ((Date.now() - m.t) / 1000);
      left += (m.end - s.frac) / rate + s.weight * (1 - m.end);
    } else {
      left += s.weight * (1 - s.frac);
    }
  }
  const mins = Math.round(left / 60);
  $('build-eta').textContent = left < 45 ? 'Almost done' : mins <= 1 ? 'About a minute left' : `About ${mins} minutes left`;
}

// the game's version in the status bar (from its build stamp)
function setGameVersion(info) {
  $('game-version').textContent = info && info.version ? info.version.replace(/\s*\(.*$/, '') : 'YW4 Recomp';
}

function logLine(line) {
  const log = $('build-log');
  log.textContent += line + '\n';
  if (log.textContent.length > 60000) log.textContent = log.textContent.slice(-40000);
  log.scrollTop = log.scrollHeight;
}

// kind: 'first' (setup), 'update' (a game update's rebuild) or 'rebuild' (other downloads); "Build again" keeps it
async function startBuild(kind = build ? build.kind : 'first') {
  build = { steps: [], started: Date.now(), kind };  // the list arrives with the build's first line
  $('build-title').textContent = kind === 'first' ? 'Building your game' : kind === 'rebuild' ? 'Rebuilding your game' : 'Updating your game';
  $('build-lead').textContent = kind === 'first' ? 'This happens once. You can minimise this window.'
    : 'Your saves stay as they are. You can minimise this window.';
  $('build-now').textContent = '';
  $('build-eta').textContent = '';
  $('build-note').textContent = '';
  $('build-next').hidden = true;
  $('build-cancel').hidden = false;
  $('build-log').textContent = '';
  $('log-toggle').hidden = false;
  $('build-back').hidden = true;
  renderBuildSteps();
  renderBuildBar();
  if (kind === 'first') await newestGameCode();
  const paths = foundFrom ? { exefs: `${foundFrom}\\data\\exefs`, romfs: `${foundFrom}\\data\\romfs`, dlc: `${foundFrom}\\data\\dlc` } : chosen[mode];
  const plan = { mode: foundFrom ? 'folders' : mode, paths, install: options.install, fsr: options.fsr, dlss: options.dlss && rtx, saves: options.saves, fresh: freshFiles };
  try {
    await invoke('start_build', { plan });
  } catch (e) {
    buildFinished({ ok: false, message: String(e) });
  }
}

// A first build uses the newest game code. The installer carries the code of its own release day, so a game update
// out since then is downloaded first, into the install's builder\game, and the build takes the newest code there
// (update.rs game_code). Without it a new player built the old version, then got the update on the Play screen
// and built again (2026-11-13). No GitHub, or a failed download: the build goes on with the code it has.
async function newestGameCode() {
  const now = $('build-now'), cancel = $('build-cancel');
  cancel.hidden = true;  // a few seconds; stopping here wouldn't stop the build that follows
  now.textContent = 'Checking for a newer game version…';
  try {
    const u = await invoke('check_updates', { install: options.install, built: '', fresh: true });
    if (u && u.game) {
      now.textContent = `Downloading game update ${u.game.version} (${gb(u.game.size)})…`;
      await invoke('download_game_update', { install: options.install, version: u.game.version, url: u.game.url,
        sha256: u.game.sha256, notes: u.game.notes });
      logLine(`game update ${u.game.version} downloaded: this build uses it`);
    }
  } catch (e) {
    logLine(`no game update this time (${e}): building the version this launcher carries`);
  }
  now.textContent = '';
  cancel.hidden = false;
}

function buildFinished({ ok, message }) {
  $('build-cancel').hidden = true;
  const next = $('build-next');
  next.hidden = false;
  if (ok) {
    freshFiles = false;
    foundFrom = null;
    for (const s of build.steps) { s.frac = 1; s.state = 'done'; }
    renderBuildSteps();
    renderBuildBar();
    afterBuild();
    invoke('install_info', { install: options.install }).then(setGameVersion).catch(() => {});
    $('build-title').textContent = 'Your game is ready';
    const mins = Math.max(1, Math.round((Date.now() - build.started) / 60000));
    $('build-lead').textContent = `Built in ${mins} minute${mins === 1 ? '' : 's'}.`;
    $('build-now').textContent = '';
    next.innerHTML = 'Continue ' + ARROW;
    next.dataset.action = 'play';
  } else {
    const stopped = message === 'cancelled';
    $('build-title').textContent = stopped ? 'Stopped' : 'The build stopped';
    // the last line of the error, short; the whole of it is in the details
    const last = message.split('\n').map((l) => l.trim()).filter(Boolean).pop() || 'Unknown error';
    $('build-lead').textContent = stopped ? 'Finished steps are kept.' : (last.length > 140 ? last.slice(0, 137) + '…' : last);
    $('build-now').textContent = '';
    next.innerHTML = 'Build again ' + ARROW;
    next.dataset.action = 'again';
    $('build-back').hidden = false;
    // the details open on a failure; refused before it began (the game is open) there are none to show
    const log = $('build-log'), hasLog = Boolean(log.textContent);
    $('log-toggle').hidden = !hasLog;
    if (!hasLog) log.hidden = true;
    else if (!stopped && log.hidden) $('log-toggle').click();
  }
}

if (T && T.event) {
  T.event.listen('build-steps', ({ payload }) => {
    if (!build) return;
    build.steps = payload.map((s) => ({ id: s.id, label: s.label, weight: s.seconds, parts: s.parts, state: '', frac: 0 }));
    renderBuildSteps();
    renderBuildBar();
  });
  T.event.listen('build-step', ({ payload }) => {
    const s = build && build.steps.find((x) => x.id === payload.id);
    if (!s) return;
    s.state = payload.state;
    if (payload.state === 'done') s.frac = 1;
    renderBuildSteps();
    renderBuildBar();
  });
  T.event.listen('build-progress', ({ payload }) => {
    if (!build) return;
    const part = findPart(payload);
    if (!part) return;
    const s = part.step;
    // each part of a step is timed on its own (downloads and compiling go at very different speeds)
    if (!s.mark || s.mark.key !== part.key) s.mark = { key: part.key, t: Date.now(), f: Math.max(s.frac, part.from), end: part.to };
    const f = payload.total ? payload.done / payload.total : 0;
    s.frac = Math.max(s.frac, part.from + (part.to - part.from) * f);
    $('build-now').textContent = nowText(payload, part);
    renderBuildBar();
  });
  T.event.listen('build-log', ({ payload }) => logLine(payload));
  T.event.listen('build-done', ({ payload }) => buildFinished(payload));
}

$('log-toggle').addEventListener('click', () => {
  const log = $('build-log');
  log.hidden = !log.hidden;
  $('log-toggle').textContent = log.hidden ? 'Show details' : 'Hide details';
  $('log-toggle').setAttribute('aria-expanded', String(!log.hidden));
});
const stopDialog = $('stop');
$('build-cancel').addEventListener('click', () => stopDialog.showModal());
$('stop-no').addEventListener('click', () => stopDialog.close());
$('stop-yes').addEventListener('click', () => { stopDialog.close(); invoke && invoke('cancel_build'); });
// a stopped build: back to where its choices can change. Setup: Options (and its own Back to Check and Game files).
// A rebuild or update: Settings, as the game built before is still there.
$('build-back').addEventListener('click', () => {
  if (build && build.kind === 'first') { show('options'); loadOptions(); } else showSettings();
});
$('build-next').addEventListener('click', () => {
  if ($('build-next').dataset.action === 'again') startBuild();
  else showReady();
});

// After a successful build: the launcher remembers the install, makes the shortcuts, frees space if chosen
async function afterBuild() {
  inst = { install: options.install, mode, paths: { ...chosen[mode] }, saves: options.saves, fsr: options.fsr,
    dlss: options.dlss && rtx, freeUp: options.freeUp, desktop: options.desktop, startMenu: options.startMenu,
    originalsOffered: inst && inst.install === options.install ? !!inst.originalsOffered : false,
    onPlay: (inst && inst.onPlay) || 'hide', mods: !(inst && inst.mods === false), showLog: !!(inst && inst.showLog),
    controlsHint: !(inst && inst.controlsHint === false), disabledMods: (inst && inst.disabledMods) || [], icon: inst && inst.icon };  // Settings' choices stay
  syncSettingsButton();
  try { await invoke('save_state', { state: inst }); } catch (e) { logLine('could not remember the install: ' + e); }
  // the icon the build just made: a first build has none cached, and Ready's install_info (DLC checks) is slow, so
  // the sidebar showed no icon for a moment (load_state fills it in cheaply from work\icon)
  if (!inst.icon) {
    try { const s = await invoke('load_state'); if (s && s.icon) inst.icon = s.icon; } catch (_) {}
  }
  if (options.desktop || options.startMenu) {
    try { await invoke('make_shortcuts', { state: inst }); } catch (e) { logLine(String(e)); }
  }
  if (options.freeUp) {
    try { await invoke('free_space', { install: inst.install }); } catch (e) { logLine(String(e)); }
  }
}

// ---- 5 · Ready (style F): the game in the sidebar; a band with play stats and tabs (Overview, Screenshots,
// Mods, DLC). Every picture is the player's own: the icon from their files, their screenshots.
let inst = null;  // the launcher's state of the install (Rust: ready.rs, %AppData%\YW4 Recomp\launcher.json)

// What's new: written for each release (the launcher ships with the notes of the version it builds)
const NEWS = {
  version: '1.1.0',
  items: ['Warnings when mods clash', 'Readable mod names', 'More mods found', 'The installer carries game 1.2.0'],
};

function showNews(version, items) {
  $('news-title').textContent = `What's new in ${version}`;
  $('news-list').replaceChildren(...items.map(chip));
}

// A release's notes (markdown) as short lines: each bullet's bold lead ("**Better settings.** More words" ->
// "Better settings"), else the bullet itself; at most five.
function newsItems(md) {
  const out = [];
  for (const raw of md.split(/\r?\n/)) {
    const m = /^\s*[-*]\s+(.*)$/.exec(raw);
    if (!m) continue;
    const bold = /^\*\*(.+?)\*\*/.exec(m[1]);
    out.push((bold ? bold[1] : m[1]).replace(/\*\*|`/g, '').replace(/[.:]\s*$/, '').trim());
  }
  return out.filter(Boolean).slice(0, 5);
}

function chip(text) {
  const li = document.createElement('li');
  li.textContent = text;
  return li;
}

function lastPlayed(secs) {
  if (!secs) return 'Never';
  const d = new Date(secs * 1000), today = new Date();
  const days = Math.round((new Date(today.toDateString()) - new Date(d.toDateString())) / 864e5);
  if (days === 0) return 'Today';
  if (days === 1) return 'Yesterday';
  if (days < 7) return `${days} days ago`;
  return d.toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: today.getFullYear() === d.getFullYear() ? undefined : 'numeric' });
}
const playTime = (s) => s < 3600 ? `${Math.round(s / 60)} min` : `${(s / 3600).toFixed(s < 36000 ? 1 : 0)} h`;

function shotButton(path) {
  const b = document.createElement('button');
  b.type = 'button';
  b.className = 'shot';
  b.title = 'Open';
  const img = document.createElement('img');
  img.loading = 'lazy';
  img.alt = 'Screenshot';
  img.src = T.core.convertFileSrc(path);
  b.append(img);
  b.addEventListener('click', () => invoke('open_shot', { install: inst.install, path }));
  return b;
}

function showTab(name) {
  document.querySelectorAll('.tabs [data-tab]').forEach((t) => t.setAttribute('aria-selected', String(t.dataset.tab === name)));
  document.querySelectorAll('.ready [data-panel]').forEach((p) => { p.hidden = p.dataset.panel !== name; });
}
document.querySelectorAll('.ready [data-tab]').forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)));

// A mod's name as shown: its folder name without the download site's "_" + 5 hex digits, "_" as spaces, each word
// capitalised. The folder name stays the mod's identity (the switch, the disabled list, the game's load order).
function modLabel(folder) {
  return folder.replace(/_[0-9a-f]{5}$/, '').replace(/_/g, ' ').replace(/(^| )([a-z])/g, (_, a, b) => a + b.toUpperCase());
}

// Groups of mods that clash now: the mods that replace one same file (info.modOverlaps, every mod on or off), while
// two or more of them are on. The later mod's file wins in the game, so the others' change does nothing. A set
// known to work together (o.ok) is no clash while only its mods are on. The player sees groups, never file names.
function modClashes(info, disabledList) {
  const off = new Set((disabledList || []).map((s) => s.toLowerCase()));
  const isOn = (m) => !off.has(m.toLowerCase());
  const seen = new Set();
  const groups = [];
  for (const o of info.modOverlaps || []) {
    const on = o.mods.filter(isOn), key = o.mods.join('\n');
    if (on.length < 2 || seen.has(key)) continue;
    if (o.ok.length && on.every((m) => o.ok.includes(m))) continue;
    seen.add(key);
    groups.push(o.mods);
  }
  // a group inside a bigger one says nothing new: keeping one on in the bigger group settles it too
  return groups.filter((g) => !groups.some((h) => h !== g && h.length > g.length && g.every((m) => h.includes(m))));
}

function modsSummary(totalMods, disabledList) {
  const disabledSet = new Set((disabledList || []).map((s) => s.toLowerCase()));
  const enabled = totalMods.filter((m) => !disabledSet.has(m.toLowerCase()));
  if (!totalMods.length) return 'None';
  if (!disabledSet.size) return totalMods.length === 1 ? modLabel(totalMods[0]) : `${totalMods.length} mods`;
  return `${enabled.length} of ${totalMods.length} active`;
}

const WARN_ICON = (size, width) => `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" ` +
  `stroke-width="${width}" stroke-linejoin="round" aria-hidden="true"><path d="M12 3l10 18H2z"/>` +
  '<path d="M12 10v4M12 17.5h.01" stroke-linecap="round"/></svg>';

let modsInfo = null;     // the last install_info: the Mods tab is redrawn from it on every switch, without a new scan
let modsFixed = false;   // a switch settled the last clash since the last scan: the green line shows
let modBusy = false;     // one switch at a time: each answer is the whole disabled list

// One mod's row with its switch, for the list ('list') or a clash group ('clash').
// use: in a clash group, 'used' (the game uses this mod's copy) or 'unused' (another one's); else nothing.
function modRow(m, on, where, clashes, use) {
  const li = document.createElement('li');
  li.className = 'mod-row';
  if (!on) li.classList.add('disabled');
  const label = document.createElement('span');
  label.className = 'mod-name';
  const name = document.createElement('b');
  name.textContent = modLabel(m);
  name.title = m;  // the folder, to find it in the mods folder
  label.append(name);
  if (clashes) {
    const tag = document.createElement('span');
    tag.className = 'clash-tag';
    tag.innerHTML = WARN_ICON(11, 2.8) + 'Clashes';
    label.append(tag);
  }
  if (use) {
    const tag = document.createElement('span');
    tag.className = use === 'used' ? 'use-tag used' : 'use-tag';
    tag.innerHTML = use === 'used'
      ? '<svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3.5" ' +
        'stroke-linecap="round" aria-hidden="true"><path d="M4 12l5 5L20 6"/></svg>Used'
      : 'Not used';
    label.append(tag);
  }
  const toggle = document.createElement('button');
  toggle.className = 'toggle';
  toggle.type = 'button';
  toggle.setAttribute('role', 'switch');
  toggle.setAttribute('aria-label', modLabel(m));
  toggle.setAttribute('aria-checked', String(on));
  toggle.dataset.mod = m;
  toggle.dataset.where = where;
  toggle.addEventListener('click', () => toggleMod(m, !on, toggle));
  li.append(label, toggle);
  return li;
}

async function toggleMod(m, next, toggle) {
  if (modBusy) return;
  modBusy = true;
  const before = modClashes(modsInfo, inst && inst.disabledMods).length;
  toggle.setAttribute('aria-checked', String(next));  // the knob slides now; the tab is redrawn once it has
  try {
    const [updated] = await Promise.all([invoke('set_mod_enabled', { modName: m, enabled: next }),
      new Promise((r) => setTimeout(r, 160))]);
    if (inst) inst.disabledMods = updated;
    modsInfo.disabledMods = updated;
    const after = modClashes(modsInfo, updated).length;
    modsFixed = !after && (before > 0 || modsFixed);
  } catch (e) {
    popup('Mod toggle failed', String(e));
  } finally {
    modBusy = false;
  }
  const where = toggle.dataset.where;
  renderMods();
  // the redraw replaced the switch: the keyboard stays on the same mod (in the list if its group is gone)
  const again = [...document.querySelectorAll('[data-panel="mods"] .toggle')].filter((t) => t.dataset.mod === m);
  const focus = again.find((t) => t.dataset.where === where) || again[0];
  if (focus) focus.focus();
}

// the Mods tab, its tab button and its sidebar row, from modsInfo and the switches' state
function renderMods() {
  const info = modsInfo, disabledList = (inst && inst.disabledMods) || info.disabledMods || [];
  const disabled = new Set(disabledList.map((s) => s.toLowerCase()));
  const isOn = (m) => !disabled.has(m.toLowerCase());
  const groups = modClashes(info, disabledList);
  const clashing = new Set(groups.flat().filter(isOn));

  $('mod-clash').hidden = !groups.length;
  $('mod-clash-title').textContent = groups.length === 1 ? '1 group of mods clash' : `${groups.length} groups of mods clash`;
  $('mod-clash-groups').replaceChildren(...groups.map((g) => {
    const ul = document.createElement('ul');
    ul.className = 'clash-group';
    // the group's mods are in load order (the game's: folder names byte by byte): the last one on is the one used
    const used = g.filter(isOn).pop();
    ul.append(...g.map((m) => modRow(m, isOn(m), 'clash', false, isOn(m) ? (m === used ? 'used' : 'unused') : null)));
    return ul;
  }));
  const fixed = $('mod-fixed'), showFixed = modsFixed && !groups.length;
  if (showFixed !== !!fixed.firstChild) {  // filled only when it changes, so a screen reader hears it once
    fixed.innerHTML = showFixed ? '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="3" stroke-linecap="round" aria-hidden="true"><path d="M4 12l5 5L20 6"/></svg>All your mods work together.' : '';
  }

  const mods = $('mod-list');
  mods.textContent = '';
  $('mod-list-label').hidden = !info.mods.length;
  if (info.mods.length) {
    mods.append(...info.mods.map((m) => modRow(m, isOn(m), 'list', clashing.has(m))));
  } else {
    const li = chip('None. Put mods in mods/contents/ (as individual mod folders or the game ID folder).');
    li.className = 'none';
    mods.append(li);
  }

  // seen from the other tabs too: a dot on the Mods tab, and the sidebar row says so
  $('tab-mods-dot').hidden = !groups.length;
  if (groups.length) $('tab-mods').setAttribute('aria-label', 'Mods, some clash');
  else $('tab-mods').removeAttribute('aria-label');
  const row = $('row-mods');
  row.parentElement.classList.toggle('warn', !!groups.length);
  if (groups.length) row.innerHTML = WARN_ICON(14, 2.4) + 'Some clash';
  else row.textContent = modsSummary(info.mods, disabledList);
}

// the Mods and DLC tabs and their sidebar rows, from install_info (also after their Refresh buttons)
function renderModsDlc(info) {
  const currentDisabled = (info && info.disabledMods) || (inst && inst.disabledMods) || [];
  if (inst) inst.disabledMods = currentDisabled;
  modsInfo = info;
  modsFixed = false;  // a new scan: "fixed" is only said about a clash this list showed
  renderMods();
  const dlcs = $('dlc-list');
  dlcs.textContent = '';
  const needsKeys = info.dlcList.some((d) => d.needsKeys);
  $('dlc-keys-box').hidden = !needsKeys;
  for (const d of info.dlcList) {
    const li = document.createElement('li');
    li.className = 'dlc-row';
    const name = document.createElement('b');
    name.textContent = d.index === null ? d.file : d.index === 100 ? 'Yo-kai Watch 4++ Upgrade' : `Add-on ${d.index}`;
    if (d.index !== null) {
      const id = document.createElement('code');
      id.textContent = d.id;
      li.append(name, id);
    } else {
      li.append(name);
    }
    const state = document.createElement('span');
    if (d.index === null) {
      li.classList.add('left-out');
      state.textContent = 'Not recognised';
      const why = document.createElement('small');
      why.textContent = d.reason || 'The game leaves it out';
      li.append(state, why);
    } else {
      state.textContent = d.needsKeys ? 'Needs prod.keys to unpack' : 'Checked';
      li.append(state);
    }
    dlcs.append(li);
  }
  if (!info.dlcList.length) { const li = chip('None. Put DLC NSPs in the DLC folder.'); li.className = 'none'; dlcs.append(li); }
  const out = info.dlcList.length - info.dlc;
  const outText = out ? `${out} not recognised` : '';
  $('row-dlc').textContent = needsKeys ? 'Needs prod.keys'
    : info.dlc ? (out ? `${info.dlc} checked · ${outText}` : `${info.dlc} checked`) : outText || 'None';
}
$('dlc-keys-btn').addEventListener('click', async () => {
  if (!T || !T.dialog) return;
  const path = await T.dialog.open({ title: 'prod.keys', multiple: false, filters: [{ name: 'prod.keys', extensions: ['keys'] }] });
  if (typeof path !== 'string' || !path) return;
  $('dlc-keys-input').value = path;
  $('dlc-keys-btn').disabled = true;
  try {
    renderModsDlc(await invoke('set_dlc_keys', { install: inst.install, path }));
  } catch (e) {
    popup('DLC keys', String(e));
  } finally {
    $('dlc-keys-btn').disabled = false;
  }
});
document.querySelectorAll('[data-refresh]').forEach((b) => b.addEventListener('click', async () => {
  b.disabled = true;
  b.classList.remove('spinning');
  void b.offsetWidth;  // restart the turn on a quick second click
  b.classList.add('spinning');
  const turn = new Promise((r) => setTimeout(r, 450));
  try { renderModsDlc(await invoke('install_info', { install: inst.install })); } finally {
    await turn;
    b.classList.remove('spinning');
    b.disabled = false;
  }
}));

function updateBand(src, isBlurred) {
  const band = $('band-img');
  if (!src) {
    band.hidden = true;
    band.classList.remove('loaded');
    return;
  }
  const prevSrc = band.getAttribute('src');
  band.classList.toggle('blurred', !!isBlurred);
  band.hidden = false;
  if (prevSrc !== src) {
    band.classList.remove('loaded');
    band.onload = () => { band.classList.add('loaded'); };
    band.onerror = () => { band.classList.remove('loaded'); band.hidden = true; };
    band.src = src;
    if (band.complete && band.naturalWidth) {
      requestAnimationFrame(() => band.classList.add('loaded'));
    }
  } else {
    band.classList.add('loaded');
  }
}

async function showReady() {
  const cachedIcon = (inst && inst.icon) || localStorage.getItem('yw4_icon_' + (inst && inst.install));
  if (cachedIcon) {
    // the picture loaded before the screen shows (at most a quarter second), or it pops in a moment after
    const src = T.core.convertFileSrc(cachedIcon);
    const img = $('game-icon');
    img.hidden = false;
    if (img.src !== src) {
      img.src = src;
      await Promise.race([img.decode().catch(() => {}), new Promise((r) => setTimeout(r, 250))]);
    }
    updateBand(src, true);
  }
  show('ready');
  if (!$('stat-last').textContent) $('stat-last').textContent = '--';
  if (!$('stat-time').textContent) $('stat-time').textContent = '--';
  if (!$('stat-shots').textContent) $('stat-shots').textContent = '--';
  const info = await invoke('install_info', { install: inst.install });
  const icon = info.icon ? T.core.convertFileSrc(info.icon) : '';
  if (info.icon) {
    try { localStorage.setItem('yw4_icon_' + inst.install, info.icon); } catch (_) {}
    if (inst) inst.icon = info.icon;
  }
  $('game-icon').hidden = !icon;
  if (icon && $('game-icon').src !== icon) $('game-icon').src = icon;
  // the band: the newest screenshot; before there is one, the icon blurred into colour
  const pic = info.shots[0] ? T.core.convertFileSrc(info.shots[0]) : icon;
  updateBand(pic, !info.shots[0]);

  $('stat-last').textContent = lastPlayed(info.lastPlayed);
  $('stat-time').textContent = playTime(info.playSecs);
  $('stat-shots').textContent = String(info.shotCount);
  const v = (info.version.match(/YW4 Recomp (\S+)/) || [])[1];
  $('built-line').textContent = v ? `Built and checked · ${v}` : 'Built and checked';
  setGameVersion(info);
  showUpdates(info);

  showNews(NEWS.version, NEWS.items);
  // a game update built from a download: its own release's notes, not the launcher's list (2026-11-13: game 1.0.2
  // built, the box still said "What's new in 1.0.0")
  invoke('game_news', { install: inst.install }).then((n) => {
    const items = n && v && n[0] === v ? newsItems(n[1]) : [];
    if (items.length) showNews(n[0], items);
  }).catch(() => {});

  // screenshots: four on the overview, the newest ones on their tab; none yet: how to take one
  $('recent').hidden = !info.shots.length;
  $('no-shots').hidden = !!info.shots.length;
  $('see-all').textContent = `See all ${info.shotCount}`;
  $('recent-grid').replaceChildren(...info.shots.slice(0, 8).map(shotButton));  // a narrow window shows 4 (CSS)
  $('shot-grid').replaceChildren(...info.shots.map(shotButton));
  $('shots-line').textContent = !info.shotCount ? 'None yet. Press F12 in the game.'
    : info.shotCount > info.shots.length ? `The newest ${info.shots.length} of ${info.shotCount}. Press F12 in the game for more.`
    : 'Press F12 in the game for more.';

  renderModsDlc(info);

  reclaimable = info.reclaimable;
  $('row-free').textContent = reclaimable > 0.05 * GB ? gb(reclaimable) : 'Done';
  $('free-btn').disabled = reclaimable <= 0.05 * GB;
  $('offer').hidden = !!inst.originalsOffered;
}
let reclaimable = 0;
// the game closed: new play time, maybe new screenshots
if (T && T.event) T.event.listen('played', () => { if (inst && !document.querySelector('[data-screen="ready"]').hidden) showReady(); });
// a theme chosen in the game comes back here when it closes (game-theme: its settings.ini's YW4_THEME)
if (T && T.event) T.event.listen('game-theme', async ({ payload }) => {
  await refreshThemes();
  const i = THEMES.findIndex((t) => t.id === payload);
  if (i >= 0 && (THEMES[themeAt] || {}).id !== payload) await applyTheme(i);
});

// the game crashed or stopped (it ended with an exit code other than 0): offer to report it (report_crash)
let crash = null;
if (T && T.event) T.event.listen('game-crashed', ({ payload }) => {
  crash = payload;
  const dlg = $('crash-dlg');
  if (!dlg.open) dlg.showModal();
  $('crash-yes').focus();
});
$('crash-no').addEventListener('click', () => $('crash-dlg').close());
$('crash-yes').addEventListener('click', () => { $('crash-dlg').close(); openReport(crash); });

// ---- Report a bug: the game overlay's form (gamemenu.c) as a popup. Page 1 what's wrong, what happened, how often;
// page 2 what to attach, Save only or Send to GitHub (save_report: a zip in reports\, GitHub's form filled in);
// page 3 Report saved. From a crash it comes with Game closed picked and that play's log.
const REPORT_KINDS = ['Graphics', 'Slow or stutters', 'Sound', 'Controls', 'Game got stuck', 'Game closed', 'Build or launcher', 'Other'];
const REPORT_OFTENS = ['Every time', 'Sometimes', 'Once'];
const report = { page: 1, kind: '', often: '', crash: null, zip: '', sent: false };

function reportPicks(box, list, key) {
  box.textContent = '';
  for (const name of list) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'pick';
    b.textContent = name;
    b.setAttribute('aria-pressed', String(report[key] === name));
    b.addEventListener('click', () => {
      report[key] = report[key] === name ? '' : name;
      for (const o of box.children) o.setAttribute('aria-pressed', String(report[key] === o.textContent));
      if (key === 'kind') $('report-build').checked = report.kind === 'Build or launcher';
    });
    box.append(b);
  }
}

function reportForm() {
  return { kind: report.kind, often: report.often, text: $('report-text').value.trim(), log: $('report-log').checked,
    save: $('report-save').checked, buildLog: $('report-build').checked,
    crashLog: report.crash ? report.crash.log || null : null, crashCode: report.crash ? report.crash.code : null };
}

function reportPage(n) {
  report.page = n;
  for (const i of [1, 2, 3]) $('report-p' + i).hidden = i !== n;
  $('report-step').textContent = n < 3 ? `${n} of 2` : '';
  $('report-title').firstChild.textContent = n < 3 ? 'Report a bug' : 'Report saved';
  $('report-back').textContent = n === 1 ? 'Cancel' : n === 2 ? 'Back' : 'Show the zip';
  $('report-alt').hidden = n === 1;
  $('report-gap1').hidden = n === 3;  // pages 1-2: the back button alone on the left; page 3: Show the zip and Open GitHub
  $('report-gap3').hidden = n !== 3;
  $('report-alt').textContent = n === 2 ? 'Save only' : report.sent ? 'Open GitHub again' : 'Open GitHub';
  $('report-go').textContent = n === 1 ? 'Next' : n === 2 ? 'Send to GitHub' : 'Done';
}

function openReport(fromCrash) {
  report.kind = fromCrash ? 'Game closed' : '';
  report.often = '';
  report.crash = fromCrash || null;
  report.zip = '';
  report.sent = false;
  $('report-text').value = '';
  $('report-log').checked = true;
  $('report-save').checked = true;
  $('report-build').checked = false;
  $('report-log-label').textContent = fromCrash ? "This play's log and settings" : 'Game log and settings';
  reportPicks($('report-kinds'), REPORT_KINDS, 'kind');
  reportPicks($('report-oftens'), REPORT_OFTENS, 'often');
  reportPage(1);
  $('report-dlg').showModal();
  $('report-text').focus();
}

async function reportSave(send) {
  try {
    report.zip = await invoke('save_report', { install: inst.install, saves: inst.saves || 'install', form: reportForm(), send });
    report.sent = send;
    $('report-zip').textContent = 'reports\\' + report.zip.split(/[\\/]/).pop();
    $('report-sent').textContent = send ? 'GitHub opened in your browser with your answers filled in.'
      : 'Open GitHub to post it, or share the zip another way.';
    reportPage(3);
  } catch (e) { $('report-dlg').close(); popup('Something went wrong', String(e)); }
}

$('report-back').addEventListener('click', () => {
  if (report.page === 1) $('report-dlg').close();
  else if (report.page === 2) reportPage(1);
  else invoke('show_report', { install: inst.install, zip: report.zip }).catch((e) => popup('Something went wrong', String(e)));
});
$('report-alt').addEventListener('click', () => {
  if (report.page === 2) reportSave(false);
  else { invoke('open_report_github', { install: inst.install, form: reportForm() }); report.sent = true; reportPage(3); }
});
$('report-go').addEventListener('click', () => {
  if (report.page === 1) reportPage(2);
  else if (report.page === 2) reportSave(true);
  else $('report-dlg').close();
});
$('help-report').addEventListener('click', () => { if (inst) openReport(null); });
$('help-reports').addEventListener('click', () => invoke('open_folder', { install: inst.install, which: 'reports', saves: inst.saves }));

$('play').addEventListener('click', async () => {
  if (!$('dlc-keys-box').hidden) {
    showTab('dlc');
    popup('Yo-kai Watch 4++ Upgrade needs prod.keys', 'The Yo-kai Watch 4++ Upgrade needs prod.keys once to unpack its files. Choose your prod.keys on the DLC tab.');
    return;
  }
  try {
    await invoke('play', { install: inst.install, mods: inst.mods !== false, showLog: !!inst.showLog, onPlay: inst.onPlay || 'hide',
      controlsHint: inst.controlsHint !== false, theme: THEMES[themeAt] ? THEMES[themeAt].id : 'recomp', disabledMods: (inst && inst.disabledMods) || [] });
    $('stat-last').textContent = 'Today';
  } catch (e) { popup('Something went wrong', String(e)); }
});
$('news-more').addEventListener('click', () => invoke('open_project_page'));
document.querySelectorAll('[data-open]').forEach((b) => b.addEventListener('click', () =>
  invoke('open_folder', { install: inst.install, which: b.dataset.open, saves: inst.saves })));


// ---- Settings (the top bar's gear): every change is saved at once (launcher.json); moving the saves asks first
const onScreen = (name) => !document.querySelector(`.screen[data-screen="${name}"]`).hidden;
function setToggle(id, on) { $(id).setAttribute('aria-checked', String(!!on)); }
async function saveInst() {
  try { await invoke('save_state', { state: inst }); } catch (e) { popup('Settings', 'Could not save: ' + e); }
}
async function showSettings() {
  rebuildFlow = false;
  freshFiles = false;
  foundFrom = null;
  renderFound();
  show('settings');
  settingsPage(1);
  $('set-install').textContent = inst.install;
  $('set-install').title = inst.install;  // one line, cut off when long: the whole of it on hover
  document.querySelectorAll('[data-move]').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.move === inst.saves)));
  setToggle('set-freeup', inst.freeUp);
  document.querySelectorAll('[data-onplay]').forEach((b) =>
    b.setAttribute('aria-checked', String(b.dataset.onplay === (inst.onPlay || 'hide'))));
  setToggle('set-mods', inst.mods !== false);
  setToggle('set-showlog', !!inst.showLog);
  setToggle('set-hint', inst.controlsHint !== false);
  try { setToggle('set-full', await invoke('game_full_screen', { install: inst.install, saves: inst.saves })); } catch (e) { /* off */ }
  setToggle('set-desktop', inst.desktop);
  setToggle('set-startmenu', inst.startMenu);
  const dl = [inst.fsr && 'AMD FSR', inst.dlss && 'NVIDIA DLSS'].filter(Boolean);
  $('set-downloads').textContent = dl.length ? dl.join(' and ') : 'No upscaler downloads';
  $('set-versions').textContent = `Launcher ${$('launcher-version').textContent.replace('Launcher ', '')} · Game ${$('game-version').textContent.replace('YW4 Recomp ', '')}`;
  const info = await invoke('install_info', { install: inst.install });
  reclaimable = info.reclaimable;
  $('set-free-size').textContent = reclaimable > 0.05 * GB ? `${gb(reclaimable)} the game doesn't need to run` : 'Already freed up';
  $('set-free').disabled = reclaimable <= 0.05 * GB;
}
// Settings has two pages (no scrolling): Playing, then Files; the arrows on the heading row switch
function settingsPage(n) {
  document.querySelectorAll('.settings-grid[data-page]').forEach((g) => { g.hidden = g.dataset.page !== String(n); });
  $('page-num').textContent = `${n} / 2`;
  $('page-prev').disabled = n === 1;
  $('page-next').disabled = n === 2;
  $('page-prev').title = n === 1 ? '' : 'Back: playing';
  $('page-next').title = n === 2 ? '' : 'Next: where things are kept';
}
$('page-prev').addEventListener('click', () => settingsPage(1));
$('page-next').addEventListener('click', () => settingsPage(2));
$('settings-back').addEventListener('click', () => showReady());

// Uninstall: the popup lists what goes (sizes from the install); kept by default: saves and screenshots
const unDlg = $('uninstall-dlg');
const sizeNote = (b) => (b > 0 ? `(${gb(b)})` : '(empty)');  // always shown: "empty" says it was looked at
$('set-uninstall').addEventListener('click', async () => {
  const u = await invoke('uninstall_info', { install: inst.install, saves: inst.saves });
  $('uninstall-text').textContent = `Deletes the game files and the build (${gb(u.game)}) from ${inst.install}.`;
  $('un-saves-size').textContent = sizeNote(u.saves);
  $('un-shots-size').textContent = sizeNote(u.shots);
  $('un-mods-size').textContent = sizeNote(u.mods);
  $('un-launcher-row').hidden = !u.launcher;  // the portable zip has no uninstaller: delete its folder by hand
  $('un-launcher').checked = u.launcher;
  $('un-yes').disabled = false;
  unDlg.showModal();
  $('un-no').focus();
});
$('un-no').addEventListener('click', () => unDlg.close());
$('un-yes').addEventListener('click', async () => {
  $('un-yes').disabled = true;
  try {
    await invoke('uninstall', { install: inst.install, saves: inst.saves, keepSaves: $('un-saves').checked,
      keepShots: $('un-shots').checked, keepMods: $('un-mods').checked, launcher: $('un-launcher').checked });
  } catch (e) {
    unDlg.close();
    popup("Couldn't uninstall", String(e));
    return;
  }
  unDlg.close();  // (with "Remove the launcher too" the launcher has closed by now)
  inst = null;
  syncSettingsButton();
  setGameVersion(null);
  $('game-version').textContent = 'YW4 Recomp · not built yet';
  show('home');
});
$('set-open-install').addEventListener('click', () => invoke('open_folder', { install: inst.install, which: 'install', saves: inst.saves }));
$('set-free').addEventListener('click', () => $('free-btn').click());  // the same popup as the Play screen's
$('set-rebuild').addEventListener('click', () => openRebuildOptions());
// Rebuild (owner): the setup's steps from Game files on, filled in with this install's choices. Nothing happens until
// "Build my game"; Back on Game files returns here.
$('set-rebuild-now').addEventListener('click', async () => {
  rebuildFlow = true;
  optionsFromPlay = false;
  useFound = true;
  try { foundFiles = await invoke('extracted_files', { install: inst.install }); } catch (e) { foundFiles = false; }
  choicesFromInstall();
  renderFound();
  show('files');
});
$('set-freeup').addEventListener('click', async () => { inst.freeUp = !inst.freeUp; setToggle('set-freeup', inst.freeUp); await saveInst(); });
document.querySelectorAll('[data-onplay]').forEach((b) => b.addEventListener('click', async () => {
  inst.onPlay = b.dataset.onplay;
  document.querySelectorAll('[data-onplay]').forEach((x) => x.setAttribute('aria-checked', String(x === b)));
  await saveInst();
}));
$('set-mods').addEventListener('click', async () => { inst.mods = inst.mods === false; setToggle('set-mods', inst.mods); await saveInst(); });
$('set-showlog').addEventListener('click', async () => { inst.showLog = !inst.showLog; setToggle('set-showlog', inst.showLog); await saveInst(); });
$('set-hint').addEventListener('click', async () => { inst.controlsHint = inst.controlsHint === false; setToggle('set-hint', inst.controlsHint); await saveInst(); });
$('set-full').addEventListener('click', async () => {
  const full = $('set-full').getAttribute('aria-checked') !== 'true';
  try {
    await invoke('set_game_full_screen', { install: inst.install, saves: inst.saves, full });
    setToggle('set-full', full);
  } catch (e) { popup('Full screen', String(e)); }
});
for (const [id, key] of [['set-desktop', 'desktop'], ['set-startmenu', 'startMenu']]) {
  $(id).addEventListener('click', async () => {
    inst[key] = !inst[key];
    setToggle(id, inst[key]);
    await saveInst();
    try { await invoke('make_shortcuts', { state: inst }); } catch (e) { popup('Shortcuts', String(e)); }
  });
}
// moving the saves: a popup first, then the move (the launcher refuses while the game runs or if saves are there)
const moveDlg = $('move-dlg');
document.querySelectorAll('[data-move]').forEach((b) => b.addEventListener('click', () => {
  if (b.dataset.move === inst.saves) return;
  moveDlg.dataset.to = b.dataset.move;
  $('move-text').textContent = b.dataset.move === 'appdata'
    ? `Your saves and settings move to ${SAVES_PLACE}. They stay there if the game folder is deleted.`
    : 'Your saves and settings move into the game folder, next to the game.';
  moveDlg.showModal();
}));
$('move-no').addEventListener('click', () => moveDlg.close());
$('move-yes').addEventListener('click', async () => {
  moveDlg.close();
  try {
    await invoke('move_saves', { install: inst.install, to: moveDlg.dataset.to });
    inst.saves = moveDlg.dataset.to;
    await saveInst();
  } catch (e) { popup("Saves weren't moved", String(e)); }
  showSettings();
});
// Check now: GitHub asked again (not the hour-old answer); what it finds opens the update popup
$('set-check').addEventListener('click', async () => {
  const btn = $('set-check');
  btn.disabled = true;
  btn.textContent = 'Checking…';
  const info = await invoke('install_info', { install: inst.install });
  let u = null;
  try { u = await invoke('check_updates', { install: inst.install, built: info.version || '', fresh: true }); } catch (e) { /* below */ }
  btn.disabled = false;
  btn.textContent = 'Check now';
  if (!u) return;
  laterFor.clear();  // asked for: offer again what "Later" put off
  if (u.rebuild || u.game || u.launcher) { showUpdates(info); return; }
  $('set-update-line').textContent = u.offline ? "Couldn't reach GitHub. Try again later." : 'Everything is up to date';
});
// Options is reached from setup (after Check) or, to rebuild with other downloads, from the Play screen (the
// Settings page will open it): Back returns where it came from
let optionsFromPlay = false;
$('options-back').addEventListener('click', () => {
  if (optionsFromPlay) { optionsFromPlay = false; showSettings(); } else show('check');
});
// the remembered choices of this install, back in the setup's state (for a rebuild and for a game update)
function choicesFromInstall() {
  mode = inst.mode;
  Object.assign(chosen[mode], inst.paths);
  Object.assign(options, { install: inst.install, saves: inst.saves, fsr: inst.fsr, freeUp: inst.freeUp,
    desktop: inst.desktop, startMenu: inst.startMenu });
  if (inst.dlss !== undefined) options.dlss = inst.dlss;
  renderFields();
}
function openRebuildOptions() {  // for the Settings page's "Rebuild with other downloads"
  optionsFromPlay = true;
  choicesFromInstall();
  show('options');
  loadOptions();
}

// ---- updates (decision 0054): a popup when the Play screen opens, at most one offer: a downloaded game update
// waiting for its rebuild first, then a game update on GitHub, then a newer launcher (installed by the launcher on
// Update, or by the player from its page when its release has no installer).
// "Later" doesn't ask again until the launcher is started again.
const updateDlg = $('update-dlg');
const laterFor = new Set();  // offers put off in this run
$('update-later').addEventListener('click', () => { laterFor.add(updateDlg.dataset.offer); updateDlg.close(); });
// a release's change log (its GitHub description, Markdown): "-" / "*" lines as a list, "#" lines as headings, the
// rest as paragraphs; plain text only (nothing from GitHub becomes HTML)
function renderNotes(md) {
  const body = $('update-notes-body');
  body.textContent = '';
  let list = null;
  for (const raw of (md || '').split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) { list = null; continue; }
    const item = /^[-*]\s+(.*)$/.exec(line);
    if (item) {
      if (!list) { list = document.createElement('ul'); body.append(list); }
      const li = document.createElement('li');
      li.textContent = item[1].replace(/\*\*|`/g, '');
      list.append(li);
      continue;
    }
    list = null;
    const head = /^#+\s+(.*)$/.exec(line);
    const el = document.createElement(head ? 'h4' : 'p');
    el.textContent = (head ? head[1] : line).replace(/\*\*|`/g, '');
    body.append(el);
  }
  $('update-notes').hidden = !body.childNodes.length;
}
function askUpdate(key, title, text, action, run, notes) {
  if (laterFor.has(key)) return;
  updateDlg.dataset.offer = key;
  $('update-title').textContent = title;
  $('update-text').textContent = text;
  renderNotes(notes);
  const btn = $('update-btn');
  btn.textContent = action;
  btn.disabled = false;
  $('update-later').disabled = false;
  btn.onclick = run;
  if (!updateDlg.open) updateDlg.showModal();
  btn.focus();  // Enter takes the update
}
async function showUpdates(info) {
  let u = null;
  try { u = await invoke('check_updates', { install: inst.install, built: info.version || '' }); } catch (e) { return; }
  if (u.rebuild) {
    askUpdate('rebuild ' + u.rebuild, `Game update ${u.rebuild} is ready`,
      'Rebuild the game to play it. It takes about 5 minutes; your saves stay as they are.', 'Rebuild', async () => {
        updateDlg.close();
        choicesFromInstall();
        await loadOptions();
        show('build');
        startBuild('update');
      }, u.rebuild_notes);
  } else if (u.game) {
    askUpdate('game ' + u.game.version, `Game update ${u.game.version}`,
      `A new version of the game is out: ${gb(u.game.size)} to download, then a rebuild of about 5 minutes.`, 'Update', async () => {
        const btn = $('update-btn');
        btn.disabled = true;
        $('update-later').disabled = true;
        btn.textContent = 'Downloading…';
        try {
          await invoke('download_game_update', { install: inst.install, version: u.game.version, url: u.game.url,
            sha256: u.game.sha256, notes: u.game.notes });
        } catch (e) {
          updateDlg.close();
          popup('Game update', String(e));
          return;
        }
        showUpdates(info);  // now the rebuild question, in the same popup
      }, u.game.notes);
  } else if (u.launcher && u.launcher.sha256 && !u.launcher.portable) {
    // the release has its installer: downloaded, checked and started here; the launcher closes and comes back new
    const l = u.launcher;
    askUpdate('launcher ' + l.version, `Launcher ${l.version}`,
      `A new launcher is out: ${gb(l.size)} to download. The launcher closes, installs it and opens again; your game and saves stay.`,
      'Update', async () => {
        const btn = $('update-btn');
        btn.disabled = true;
        $('update-later').disabled = true;
        btn.textContent = 'Downloading…';
        try {
          await invoke('download_launcher_update', { version: l.version, url: l.url, sha256: l.sha256 });
          btn.textContent = 'Installing…';
          await invoke('run_launcher_update');
        } catch (e) {
          updateDlg.close();
          popup('Launcher update', String(e));
        }
      }, l.notes);
  } else if (u.launcher) {  // the portable launcher, or a release without its installer: its page, by hand
    const text = u.launcher.portable
      ? `A new launcher is out. Download YW4-Recomp-Launcher-${u.launcher.version}-portable.zip and unpack it over this folder; your game and saves stay.`
      : 'A new launcher is out. Download it and install it over this one; your game and saves stay.';
    askUpdate('launcher ' + u.launcher.version, `Launcher ${u.launcher.version}`, text, 'Download', () => {
        updateDlg.close();
        invoke('open_release_page', { url: u.launcher.page });
      }, u.launcher.notes);
  }
}

const freeDlg = $('free');
$('free-btn').addEventListener('click', () => {
  $('free-title').textContent = `Free up ${gb(reclaimable)}?`;
  $('free-yes').textContent = `Free up ${gb(reclaimable)}`;
  freeDlg.showModal();
});
$('free-no').addEventListener('click', () => freeDlg.close());
$('free-yes').addEventListener('click', async () => {
  freeDlg.close();
  await invoke('free_space', { install: inst.install });
  if (onScreen('settings')) showSettings(); else showReady();
});

// the one-time offer: delete the player's originals (never the keys)
const origDlg = $('originals-dlg');
async function markOffered() {
  inst.originalsOffered = true;
  $('offer').hidden = true;
  try { await invoke('save_state', { state: inst }); } catch (e) { /* asked again next time */ }
}
$('offer-keep').addEventListener('click', markOffered);
$('offer-delete').addEventListener('click', async () => {
  const files = await invoke('originals', { state: inst });
  const list = $('originals-list');
  list.textContent = '';
  let total = 0;
  for (const f of files) { list.append(chip(`${f.path} · ${gb(f.size)}`)); total += f.size; }
  $('originals-yes').textContent = `Delete ${gb(total)}`;
  origDlg.showModal();
});
$('originals-no').addEventListener('click', () => origDlg.close());
$('originals-yes').addEventListener('click', async () => {
  origDlg.close();
  try { await invoke('delete_originals', { state: inst }); } catch (e) { popup('Something went wrong', String(e)); }
  markOffered();
});
renderFields();

// ---- the welcome popup, on launch until "Don't show this again" is ticked (remembered in this window's storage;
// moves to the launcher's settings file once it has one)
const welcome = $('welcome');
const HIDE = 'yw4.welcome.hidden';
const closeWelcome = () => {
  try { if ($('welcome-hide').checked) localStorage.setItem(HIDE, '1'); } catch (e) { /* not remembered */ }
  welcome.close();
};
$('welcome-close').addEventListener('click', closeWelcome);

// ---- start: a test switch can open a given screen without the popup (YW4_LAUNCHER_SCREEN, for screenshots)
(async () => {
  let screen = null;
  try { screen = invoke ? await invoke('start_screen') : null; } catch (e) { /* none */ }
  if (screen === 'check' || screen === 'build') {
    // tests: paths from YW4_LAUNCHER_PATHS (mode;id=path;...; "install=" sets the install folder), then run that screen
    const spec = (invoke ? await invoke('test_paths') : null) || '';
    const [m, ...pairs] = spec.split(';');
    if (m === 'files' || m === 'folders') mode = m;
    for (const p of pairs) {
      const i = p.indexOf('=');
      if (i <= 0) continue;
      if (p.slice(0, i) === 'install') options.install = p.slice(i + 1);
      else chosen[mode][p.slice(0, i)] = p.slice(i + 1);
    }
    document.querySelectorAll('.mode').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.mode === mode)));
    renderFields();
    if (screen === 'check') { show('check'); runCheck(); return; }
    await loadOptions();  // the graphics card decides DLSS
    show('build');
    startBuild('first');
    return;
  }
  if (screen === 'files-ready') {  // screenshots of the ready state: example paths, nothing real is read
    Object.assign(chosen.files, { base: 'D:\\Switch\\Yo-kai Watch 4++ [BASE].nsp',
      update: 'D:\\Switch\\Yo-kai Watch 4++ 2.2.0 [UPD].nsp', keys: 'D:\\Switch\\keys' });
    renderFields();
    screen = 'files';
  }
  if (screen === 'options') { show('options'); loadOptions(); return; }
  if (screen && screen !== 'ready') { show(screen); return; }
  // a built install: straight to Play (the welcome popup is for the first run)
  try { inst = invoke ? await invoke('load_state') : null; } catch (e) { inst = null; }
  syncSettingsButton();
  if (inst) { showReady(); return; }
  show('home');
  let hidden = false;
  try { hidden = localStorage.getItem(HIDE) === '1'; } catch (e) { /* storage unavailable: show it */ }
  if (!hidden) welcome.showModal();
})();
