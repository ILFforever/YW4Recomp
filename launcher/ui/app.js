// The launcher's screens. The Rust side is reached through window.__TAURI__ (tauri.conf.json withGlobalTauri).
const T = window.__TAURI__ || null;
const invoke = T ? T.core.invoke : null;
const $ = (id) => document.getElementById(id);

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

// ---- top bar, status bar
$('github').addEventListener('click', () => invoke && invoke('open_project_page'));
(async () => {
  if (!invoke) return;
  try { $('launcher-version').textContent = 'Launcher ' + await invoke('launcher_version'); } catch (e) { console.error(e); }
})();

// ---- placeholder for what isn't built yet
const soon = $('soon');
$('soon-ok').addEventListener('click', () => soon.close());

// ---- Home
$('start').addEventListener('click', () => show('files'));

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
  ],
};
const NOTES = {
  files: 'An NSP also needs title.keys in the keys folder.',
  folders: 'These folders are copied, not changed.',
};
const chosen = { files: {}, folders: {} };  // what the player picked, per mode (kept when switching back and forth)
let mode = 'files';

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
    browse.textContent = 'Browse…';
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
const missingFields = () => FIELDS[mode].filter((f) => f.required && !chosen[mode][f.id]);

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
    : NOTES[mode];
}

function markMissing() {
  const ids = new Set(missingFields().map((f) => 'f-' + f.id));
  document.querySelectorAll('#fields .field').forEach((row) => {
    row.classList.toggle('missing', ids.has(row.querySelector('input').id));
  });
}

document.querySelectorAll('.mode').forEach((b) => b.addEventListener('click', () => {
  mode = b.dataset.mode;
  document.querySelectorAll('.mode').forEach((m) => m.setAttribute('aria-checked', String(m === b)));
  renderFields();
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
    rows = await invoke('check_game', { req: { mode, paths: chosen[mode] } });
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
  show('options');
  loadOptions();
});

// ---- 3 · Options: downloads (by the graphics card), where the game goes, saves, shortcuts
const GB = 1024 ** 3;
const options = {
  fsr: true, dlss: false, freeUp: false, desktop: true, startMenu: false,
  install: 'C:\\Games\\YW4 Recomp', saves: 'install',
};
let rtx = false;
let spaceFree = null;
// What the install folder takes: the copied RomFS (8.5 GB) + ExeFS, the tools (1.5 GB) and build files (0.7 GB),
// the game itself, and the downloads (target.toml [extracted], measured sizes: FSR 240 MB, DLSS + Streamline 325 MB)
const downloadMB = () => (options.fsr ? 240 : 0) + (options.dlss ? 325 : 0);
const diskNeeded = () => 8.5 * GB + 2.4 * GB + downloadMB() * 1024 ** 2;

const SWITCHES = [
  { key: 'fsr', title: 'AMD FSR', note: () => 'Upscaling and frame generation, any graphics card · about 240 MB' },
  { key: 'dlss', title: 'NVIDIA DLSS', note: () => rtx ? 'Upscaling, frame generation, Reflex · about 325 MB'
    : 'Needs an NVIDIA RTX graphics card', needsRtx: true },
  { key: 'freeUp', title: 'Free up space after building', note: () => 'Deletes the tools and build files (2.2 GB). Updates then rebuild from scratch' },
  { key: 'desktop', title: 'Desktop shortcut', note: () => 'Opens the launcher' },
  { key: 'startMenu', title: 'Start menu entry', note: () => 'Opens the launcher' },
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

const gb = (b) => (b / GB).toFixed(b >= 100 * GB ? 0 : 1) + ' GB';

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
  options.dlss = rtx;  // on by default on an RTX card
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
    options.install = /YW4 Recomp$/i.test(dir) ? dir : dir.replace(/[\\/]$/, '') + '\\YW4 Recomp';
    refreshSpace();
  }
});
document.querySelectorAll('.radio[data-saves]').forEach((b) => b.addEventListener('click', () => {
  options.saves = b.dataset.saves;
  document.querySelectorAll('.radio[data-saves]').forEach((r) => r.setAttribute('aria-checked', String(r === b)));
}));
$('build-go').addEventListener('click', () => {
  if (spaceFree && spaceFree.free < diskNeeded()) return;
  show('build');
  startBuild();
});

// ---- 4 · Build: the steps (Rust: buildrun.rs) and their "@progress" lines turned into one bar
// Each step's share of the bar is its measured time (seconds, 2026-11-03 on a 16-thread laptop); inside a step,
// a progress line's own step name says which part of it is moving.
const BUILD_STEPS = () => [
  { id: 'game', label: mode === 'files' ? 'Unpacking your game files' : 'Copying your game files', weight: mode === 'files' ? 190 : 60 },
  { id: 'dlc', label: 'Copying DLC', weight: 1 },
  { id: 'version', label: 'Checking the version', weight: 3 },
  { id: 'translate', label: 'Translating the game code', weight: 30 },
  { id: 'shaders', label: 'Preparing shaders', weight: 50 },
  { id: 'compile', label: 'Compiling', weight: 310 },
  { id: 'finish', label: 'Finishing up', weight: 1 },
];
// where a progress line's step sits inside a build step: [start, end] of that step
const PARTS = {
  unpack: ['game', 0, 0.07], extract: ['game', 0.07, 0.99], icon: ['game', 0.99, 1], copy: ['game', 0, 1],
  ingest: ['version', 0, 1], shaders: ['shaders', 0.3, 1], compile: ['compile', 0.1, 0.95], link: ['compile', 0.95, 1],
};
const NOW_TEXT = {
  unpack: 'Unpacking the NSP or XCI', extract: 'Extracting the game files', icon: 'Taking the game icon', copy: 'Copying',
  ingest: 'Checking the game code', shaders: 'Translating shaders', compile: 'Compiling', link: 'Linking',
};
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
  const pct = Math.min(100, Math.floor((100 * done) / total));
  $('build-pct').textContent = pct + '%';
  $('build-fill').style.width = pct + '%';
  $('build-bar').setAttribute('aria-valuenow', String(pct));
  const elapsed = (Date.now() - build.started) / 1000;
  const frac = done / total;
  if (frac > 0.04 && frac < 1) {
    const left = Math.max(1, Math.round((elapsed / frac - elapsed) / 60));
    $('build-eta').textContent = left <= 1 ? 'About a minute left' : `About ${left} minutes left`;
  } else if (frac >= 1) {
    $('build-eta').textContent = '';
  }
}

function logLine(line) {
  const log = $('build-log');
  log.textContent += line + '\n';
  if (log.textContent.length > 60000) log.textContent = log.textContent.slice(-40000);
  log.scrollTop = log.scrollHeight;
}

async function startBuild() {
  build = { steps: BUILD_STEPS().map((s) => ({ ...s, state: '', frac: 0 })), started: Date.now() };
  $('build-title').textContent = 'Building your game';
  $('build-lead').textContent = 'This happens once. You can minimise this window.';
  $('build-now').textContent = '';
  $('build-eta').textContent = '';
  $('build-note').textContent = '';
  $('build-next').hidden = true;
  $('build-cancel').hidden = false;
  $('build-log').textContent = '';
  renderBuildSteps();
  renderBuildBar();
  const plan = { mode, paths: chosen[mode], install: options.install, fsr: options.fsr, dlss: options.dlss && rtx, saves: options.saves };
  try {
    await invoke('start_build', { plan });
  } catch (e) {
    buildFinished({ ok: false, message: String(e) });
  }
}

function buildFinished({ ok, message }) {
  $('build-cancel').hidden = true;
  const next = $('build-next');
  next.hidden = false;
  if (ok) {
    for (const s of build.steps) { s.frac = 1; s.state = 'done'; }
    renderBuildSteps();
    renderBuildBar();
    $('build-title').textContent = 'Your game is ready';
    $('build-lead').textContent = 'Built in ' + Math.round((Date.now() - build.started) / 60000) + ' minutes.';
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
    if (!stopped) { $('log-toggle').click(); }
  }
}

if (T && T.event) {
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
    let part = PARTS[payload.step];
    if (payload.step === 'translate') part = payload.what === 'sdk' ? ['translate', 0.55, 1] : ['translate', 0, 0.55];
    if (!part) return;
    const s = build.steps.find((x) => x.id === part[0]);
    const f = payload.total ? payload.done / payload.total : 0;
    s.frac = Math.max(s.frac, part[1] + (part[2] - part[1]) * f);
    $('build-now').textContent = (NOW_TEXT[payload.step] || 'Translating the game code') +
      (payload.total > 1 && payload.step !== 'unpack' && payload.step !== 'extract' && payload.step !== 'copy'
        ? ` · ${payload.done.toLocaleString()} of ${payload.total.toLocaleString()}` : '');
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
$('build-next').addEventListener('click', () => {
  if ($('build-next').dataset.action === 'again') startBuild();
  else soon.showModal();  // 5 · Play comes next
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
    startBuild();
    return;
  }
  if (screen === 'files-ready') {  // screenshots of the ready state: example paths, nothing real is read
    Object.assign(chosen.files, { base: 'D:\\Switch\\Yo-kai Watch 4++ [BASE].nsp',
      update: 'D:\\Switch\\Yo-kai Watch 4++ 2.2.0 [UPD].nsp', keys: 'D:\\Switch\\keys' });
    renderFields();
    screen = 'files';
  }
  if (screen === 'options') { show('options'); loadOptions(); return; }
  if (screen) { show(screen); return; }
  show('home');
  let hidden = false;
  try { hidden = localStorage.getItem(HIDE) === '1'; } catch (e) { /* storage unavailable: show it */ }
  if (!hidden) welcome.showModal();
})();
