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
  soon.showModal();  // the check screen comes next
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
  if (screen === 'files-ready') {  // screenshots of the ready state: example paths, nothing real is read
    Object.assign(chosen.files, { base: 'D:\\Switch\\Yo-kai Watch 4++ [BASE].nsp',
      update: 'D:\\Switch\\Yo-kai Watch 4++ 2.2.0 [UPD].nsp', keys: 'D:\\Switch\\keys' });
    renderFields();
    screen = 'files';
  }
  if (screen) { show(screen); return; }
  show('home');
  let hidden = false;
  try { hidden = localStorage.getItem(HIDE) === '1'; } catch (e) { /* storage unavailable: show it */ }
  if (!hidden) welcome.showModal();
})();
