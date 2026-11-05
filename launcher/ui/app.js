// The launcher's screens. The Rust side is reached through window.__TAURI__ (tauri.conf.json withGlobalTauri).
const T = window.__TAURI__ || null;
const invoke = T ? T.core.invoke : null;
const $ = (id) => document.getElementById(id);
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
if (T && T.window) {
  const win = T.window.getCurrentWindow();
  $('win-min').addEventListener('click', () => win.minimize());
  $('win-max').addEventListener('click', () => win.toggleMaximize());
  $('win-close').addEventListener('click', () => win.close());
  // the maximise button shows "restore" while maximised (double-clicking the bar maximises too)
  const syncMax = async () => {
    const max = await win.isMaximized();
    document.body.classList.toggle('maximised', max);
    $('win-max').setAttribute('aria-label', max ? 'Restore' : 'Maximise');
    $('win-max').title = max ? 'Restore' : 'Maximise';
  };
  win.onResized(syncMax);
  syncMax();
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
  optionsFromPlay = false;
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
// A download brings everything but the compiler, which the first build fetches (decision 0052): 182 MB, 737 MB
// unpacked. A launcher built from the repo uses the repo's compiler.
let bundled = false;
if (invoke) invoke('builder_bundled').then((b) => { bundled = b; renderSwitches(); renderSummary(); });
// What the install folder takes: the copied RomFS (8.5 GB) + ExeFS, the tools (1.5 GB) and build files (0.7 GB),
// the game itself, and the downloads (target.toml [extracted], measured sizes: FSR 240 MB, DLSS + Streamline 325 MB)
const downloadMB = () => (bundled ? 182 : 0) + (options.fsr ? 240 : 0) + (options.dlss ? 325 : 0);
const diskNeeded = () => 8.5 * GB + 2.4 * GB + (bundled ? 737 * 1024 ** 2 : 0) + downloadMB() * 1024 ** 2;

const SWITCHES = [
  { key: 'fsr', title: 'AMD FSR', note: () => 'Upscaling and frame generation, any graphics card · about 240 MB' },
  { key: 'dlss', title: 'NVIDIA DLSS', note: () => rtx ? 'Upscaling, frame generation, Reflex · about 325 MB'
    : 'Needs an NVIDIA RTX graphics card', needsRtx: true },
  { key: 'freeUp', title: 'Free up space after building', note: () => `Deletes the tools and build files (${bundled ? '2 GB' : '1.3 GB'}). Updates then download them and rebuild` },
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
// Each step's share of the bar is its measured time in seconds (the installed launcher's first build, 2026-11-05,
// 16-thread laptop, game on an SSD); compiling scales with the CPU's threads. Inside a step, a progress line's own
// step name says which part of it is moving.
const CORES = navigator.hardwareConcurrency || 16;
const BUILD_STEPS = () => [
  ...(bundled ? [{ id: 'tools', label: 'Downloading the compiler', weight: 30 }] : []),
  { id: 'game', label: mode === 'files' ? 'Unpacking your game files' : 'Copying your game files', weight: 60 },
  { id: 'dlc', label: 'Copying DLC', weight: 1 },
  { id: 'version', label: 'Checking the version', weight: 3 },
  { id: 'translate', label: 'Translating the game code', weight: 30 },
  { id: 'shaders', label: 'Preparing shaders', weight: 40 },
  { id: 'compile', label: 'Compiling', weight: Math.round(360 * Math.min(3, Math.max(0.5, 16 / CORES))) },
  { id: 'finish', label: 'Finishing up', weight: 1 },
];
// where a progress line's step sits inside a build step: [start, end] of that step
const PARTS = {
  compiler: ['tools', 0, 0.6], compilerunpack: ['tools', 0.6, 1],
  unpack: ['game', 0, 0.07], extract: ['game', 0.07, 0.99], icon: ['game', 0.99, 1], copy: ['game', 0, 1],
  ingest: ['version', 0, 1], shaders: ['shaders', 0.3, 1],
  download: ['compile', 0, 0.1], compile: ['compile', 0.1, 0.95], link: ['compile', 0.95, 1],
};
const NOW_TEXT = {
  compiler: 'Downloading the compiler', compilerunpack: 'Unpacking the compiler',
  unpack: 'Unpacking the NSP or XCI', extract: 'Extracting the game files', icon: 'Taking the game icon', copy: 'Copying',
  ingest: 'Checking the game code', shaders: 'Translating shaders', compile: 'Compiling', link: 'Linking',
};
// steps whose counts are bytes: shown as sizes
const BYTE_STEPS = new Set(['compiler', 'compilerunpack', 'download', 'unpack', 'extract', 'copy']);
// what a downloaded file is, for the line under the bar
function downloadName(file) {
  if (/^SDL/i.test(file)) return 'SDL';
  if (/FidelityFX/i.test(file)) return 'AMD FSR';
  if (/streamline/i.test(file)) return 'NVIDIA Streamline';
  return 'NVIDIA DLSS';
}
function nowText(p) {
  if (p.step === 'download') return `Downloading ${downloadName(p.what)} · ${gb(p.done)} of ${gb(p.total)}`;
  const files = p.step === 'compile' && /^(\d+)\/(\d+)$/.exec(p.what);
  if (files) return `Compiling · ${(+files[1]).toLocaleString()} of ${(+files[2]).toLocaleString()} files`;
  const what = NOW_TEXT[p.step] || 'Translating the game code';
  if (p.total <= 1) return what;
  return BYTE_STEPS.has(p.step) ? `${what} · ${gb(p.done)} of ${gb(p.total)}`
    : `${what} · ${p.done.toLocaleString()} of ${p.total.toLocaleString()}`;
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
  const pct = Math.min(100, Math.floor((100 * done) / total));
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
    // each part of a step is timed on its own (downloads and compiling go at very different speeds)
    const key = part.join();
    if (!s.mark || s.mark.key !== key) s.mark = { key, t: Date.now(), f: Math.max(s.frac, part[1]), end: part[2] };
    const f = payload.total ? payload.done / payload.total : 0;
    s.frac = Math.max(s.frac, part[1] + (part[2] - part[1]) * f);
    $('build-now').textContent = nowText(payload);
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
  else showReady();
});

// After a successful build: the launcher remembers the install, makes the shortcuts, frees space if chosen
async function afterBuild() {
  inst = { install: options.install, mode, paths: { ...chosen[mode] }, saves: options.saves, fsr: options.fsr,
    dlss: options.dlss && rtx, freeUp: options.freeUp, desktop: options.desktop, startMenu: options.startMenu,
    originalsOffered: inst && inst.install === options.install ? !!inst.originalsOffered : false };
  try { await invoke('save_state', { state: inst }); } catch (e) { logLine('could not remember the install: ' + e); }
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
  version: '0.7.0',
  items: ['Contact shadows: soft shadows where things touch', 'Fewer stutters in menus and towns',
    'XCI cartridge dumps work', 'Version numbers, shown in the game and here'],
};

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

async function showReady() {
  show('ready');
  const info = await invoke('install_info', { install: inst.install });
  const icon = info.icon ? T.core.convertFileSrc(info.icon) : '';
  $('game-icon').hidden = !icon;
  if (icon) $('game-icon').src = icon;
  // the band: the newest screenshot; before there is one, the icon blurred into colour
  const band = $('band-img');
  const pic = info.shots[0] ? T.core.convertFileSrc(info.shots[0]) : icon;
  band.hidden = !pic;
  if (pic) band.src = pic;
  band.classList.toggle('blurred', !info.shots[0]);

  $('stat-last').textContent = lastPlayed(info.lastPlayed);
  $('stat-time').textContent = playTime(info.playSecs);
  $('stat-shots').textContent = String(info.shotCount);
  const v = (info.version.match(/YW4 Recomp (\S+)/) || [])[1];
  $('built-line').textContent = v ? `Built and checked · ${v}` : 'Built and checked';
  setGameVersion(info);

  $('news-title').textContent = `What's new in ${NEWS.version}`;
  const news = $('news-list');
  news.textContent = '';
  for (const n of NEWS.items) news.append(chip(n));

  // screenshots: four on the overview, the newest ones on their tab; none yet: how to take one
  $('recent').hidden = !info.shots.length;
  $('no-shots').hidden = !!info.shots.length;
  $('see-all').textContent = `See all ${info.shotCount}`;
  $('recent-grid').replaceChildren(...info.shots.slice(0, 8).map(shotButton));  // a narrow window shows 4 (CSS)
  $('shot-grid').replaceChildren(...info.shots.map(shotButton));
  $('shots-line').textContent = !info.shotCount ? 'None yet. Press F12 in the game.'
    : info.shotCount > info.shots.length ? `The newest ${info.shots.length} of ${info.shotCount}. Press F12 in the game for more.`
    : 'Press F12 in the game for more.';

  const mods = $('mod-list');
  mods.textContent = '';
  if (info.mods.length) for (const m of info.mods) mods.append(chip(m));
  else { const li = chip('None. Put mods in the mods folder.'); li.className = 'none'; mods.append(li); }
  $('row-mods').textContent = info.mods.length === 1 ? info.mods[0] : info.mods.length ? `${info.mods.length} mods` : 'None';
  $('dlc-line').textContent = info.dlc ? `${info.dlc} add-ons, checked.` : 'None. Put DLC NSPs in the DLC folder.';
  $('row-dlc').textContent = info.dlc ? `${info.dlc} checked` : 'None';

  reclaimable = info.reclaimable;
  $('row-free').textContent = reclaimable > 0.05 * GB ? gb(reclaimable) : 'Done';
  $('free-btn').disabled = reclaimable <= 0.05 * GB;
  $('offer').hidden = !!inst.originalsOffered;
}
let reclaimable = 0;
// the game closed: new play time, maybe new screenshots
if (T && T.event) T.event.listen('played', () => { if (inst && !document.querySelector('[data-screen="ready"]').hidden) showReady(); });

$('play').addEventListener('click', async () => {
  try {
    await invoke('play', { install: inst.install });
    $('stat-last').textContent = 'Today';
  } catch (e) { popup('Something went wrong', String(e)); }
});
$('news-more').addEventListener('click', () => invoke('open_project_page'));
document.querySelectorAll('[data-open]').forEach((b) => b.addEventListener('click', () =>
  invoke('open_folder', { install: inst.install, which: b.dataset.open, saves: inst.saves })));
// Play's gear: game and launcher settings, not designed yet (the owner decides what goes there)
$('to-options').addEventListener('click', () => {
  popup('Settings', 'Coming soon.');
});
// Options is reached from setup (after Check) or from Play's Rebuild row: Back returns where it came from
let optionsFromPlay = false;
$('options-back').addEventListener('click', () => {
  if (optionsFromPlay) { optionsFromPlay = false; showReady(); } else show('check');
});
$('rebuild-btn').addEventListener('click', () => {  // back into setup with the remembered choices
  optionsFromPlay = true;
  mode = inst.mode;
  Object.assign(chosen[mode], inst.paths);
  Object.assign(options, { install: inst.install, saves: inst.saves, fsr: inst.fsr, freeUp: inst.freeUp,
    desktop: inst.desktop, startMenu: inst.startMenu });
  renderFields();
  show('options');
  loadOptions();
});

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
  showReady();
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
  if (screen && screen !== 'ready') { show(screen); return; }
  // a built install: straight to Play (the welcome popup is for the first run)
  try { inst = invoke ? await invoke('load_state') : null; } catch (e) { inst = null; }
  if (inst) { showReady(); return; }
  show('home');
  let hidden = false;
  try { hidden = localStorage.getItem(HIDE) === '1'; } catch (e) { /* storage unavailable: show it */ }
  if (!hidden) welcome.showModal();
})();
