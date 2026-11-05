// Launcher themes: the look only. A theme is a CSS file of colour tokens (the :root values at the top of style.css)
// in the themes folder next to the launcher. The Rust side lists the folder and reads a chosen file (themes.rs), so a
// new or edited file shows up without a rebuild. 'recomp' is the built-in look and has no file.
// The Look control is a dropdown: each theme shows two dots from its own palette. The choice is kept on this PC.
const themeInvoke = window.__TAURI__ ? window.__TAURI__.core.invoke : null;
// dots: the gold and the deep colour, for the swatches (the built-in look's values from style.css)
let THEMES = [{ id: 'recomp', name: 'YW4 Recomp', dots: ['oklch(80% 0.12 85)', 'oklch(27% 0.09 295)'] }];
const THEME_KEY = 'yw4-launcher-theme';
let themeAt = 0;

// A theme's two swatch colours, read from its file: --gold and --violet-deep (a theme without them shows grey).
function dotsOf(css) {
  const pick = (name) => (css.match(new RegExp(`${name}:\\s*([^;]+);`)) || [])[1] || '#999';
  return [pick('--gold'), pick('--violet-deep')];
}

async function refreshThemes() {
  if (!themeInvoke) return;
  try {
    const list = await themeInvoke('list_themes');
    if (!Array.isArray(list) || !list.length) return;
    for (const t of list) {
      if (t.id === 'recomp') t.dots = THEMES[0].dots;
      else {
        try { t.dots = dotsOf(await themeInvoke('read_theme', { id: t.id })); } catch (e) { t.dots = ['#999', '#999']; }
      }
    }
    THEMES = list;
  } catch (e) { /* keep the built-in one */ }
}

function savedThemeId() {
  try { return localStorage.getItem(THEME_KEY); } catch (e) { return null; }  // no storage: the built-in look
}

function dotsInto(el, dots) {
  el.textContent = '';
  for (const c of dots) {
    const s = document.createElement('span');
    s.style.background = c;
    el.append(s);
  }
}

// Shows theme i: its CSS is read now, applied as the page's style sheet, and kept. Names it on the button.
async function applyTheme(i) {
  themeAt = ((i % THEMES.length) + THEMES.length) % THEMES.length;
  const t = THEMES[themeAt];
  let css = '';
  if (t.id !== 'recomp' && themeInvoke) {
    try { css = await themeInvoke('read_theme', { id: t.id }); } catch (e) { css = ''; }
  }
  const style = document.getElementById('theme-css');
  if (style) style.textContent = css;
  try { localStorage.setItem(THEME_KEY, t.id); } catch (e) { /* not kept */ }
  const name = document.getElementById('set-look-name');
  if (name) name.textContent = t.name;
  const dots = document.getElementById('look-dots');
  if (dots) dotsInto(dots, t.dots);
}

// ---- the dropdown
const lookBtn = document.getElementById('look-btn');
const lookMenu = document.getElementById('look-menu');

function renderMenu() {
  lookMenu.textContent = '';
  THEMES.forEach((t, i) => {
    const item = document.createElement('button');
    item.type = 'button';
    item.className = 'look-item';
    item.setAttribute('role', 'option');
    item.setAttribute('aria-selected', String(i === themeAt));
    const dots = document.createElement('span');
    dots.className = 'look-dots';
    dotsInto(dots, t.dots);
    const name = document.createElement('span');
    name.className = 'look-item-name';
    name.textContent = t.name;
    item.append(dots, name);
    item.addEventListener('click', async () => { closeMenu(); await applyTheme(i); lookBtn.focus(); });
    lookMenu.append(item);
  });
}

async function openMenu() {
  await refreshThemes();  // a file added or changed since the last time shows now
  themeAt = Math.max(0, THEMES.findIndex((t) => t.id === (THEMES[themeAt] || {}).id));
  renderMenu();
  lookMenu.hidden = false;
  lookBtn.setAttribute('aria-expanded', 'true');
  lookMenu.querySelector('[aria-selected="true"]')?.focus();
}

function closeMenu() {
  lookMenu.hidden = true;
  lookBtn.setAttribute('aria-expanded', 'false');
}

lookBtn.addEventListener('click', () => (lookMenu.hidden ? openMenu() : closeMenu()));
document.addEventListener('pointerdown', (e) => {
  if (!lookMenu.hidden && !e.target.closest('.look')) closeMenu();
});
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape' && !lookMenu.hidden) { closeMenu(); lookBtn.focus(); }
});

(async () => {
  await refreshThemes();
  const i = THEMES.findIndex((t) => t.id === savedThemeId());
  await applyTheme(i < 0 ? 0 : i);
})();
