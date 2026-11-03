// The launcher's screens. The Rust side is reached through window.__TAURI__ (tauri.conf.json withGlobalTauri).
const invoke = window.__TAURI__ ? window.__TAURI__.core.invoke : null;

// The launcher's own version in the status bar, asked from the Rust side (proof the two talk)
(async () => {
  if (!invoke) return;
  try {
    document.getElementById('launcher-version').textContent = 'Launcher ' + await invoke('launcher_version');
  } catch (e) {
    console.error(e);
  }
})();

// GitHub: the Rust side opens the project page in the browser
document.getElementById('github').addEventListener('click', () => invoke && invoke('open_project_page'));

// Get started: the setup screens come next
const soon = document.getElementById('soon');
const getStarted = () => soon.showModal();
document.getElementById('start').addEventListener('click', getStarted);
document.getElementById('soon-ok').addEventListener('click', () => soon.close());

// The welcome popup, on launch until "Don't show this again" is ticked (remembered in this window's storage;
// moves to the launcher's settings file once it has one)
const welcome = document.getElementById('welcome');
const HIDE = 'yw4.welcome.hidden';
let hidden = false;
try { hidden = localStorage.getItem(HIDE) === '1'; } catch (e) { /* storage unavailable: show it */ }
const closeWelcome = () => {
  try {
    if (document.getElementById('welcome-hide').checked) localStorage.setItem(HIDE, '1');
  } catch (e) { /* not remembered */ }
  welcome.close();
};
document.getElementById('welcome-close').addEventListener('click', closeWelcome);
if (!hidden) welcome.showModal();
