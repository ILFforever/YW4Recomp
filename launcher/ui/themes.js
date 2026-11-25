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

// ---- Dark mode (owner, 2026-11-26: "a dark mode toggle which switches every theme to dark"). The theme's own accents
// stay (its gold, Play, selection colour and the hue of its purple); the page, cards, lines and text become dark ones
// tinted with the theme's hue. The game's overlay does the same sums from the same file (runtime/src/theme.c
// theme_darken): change both together.
const DARK_KEY = 'yw4-launcher-dark';

// a CSS colour (#rgb, #rrggbb, rgb(), oklch(), as computed) -> [r, g, b] in 0..1, or null
function parseColour(s) {
  s = (s || '').trim();
  let m = s.match(/^#([0-9a-f]{3}|[0-9a-f]{6})$/i);
  if (m) {
    const h = m[1].length === 3 ? m[1].replace(/./g, '$&$&') : m[1];
    return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16) / 255);
  }
  m = s.match(/^rgba?\(\s*([\d.]+)[\s,]+([\d.]+)[\s,]+([\d.]+)/i);
  if (m) return [m[1], m[2], m[3]].map((v) => +v / 255);
  m = s.match(/^oklch\(\s*([\d.]+)(%?)\s+([\d.]+)(%?)\s+([\d.]+)/i);
  if (m) return oklchToRgb(m[2] ? m[1] / 100 : +m[1], m[4] ? m[3] * 0.004 : +m[3], +m[5]);
  return null;
}

const lin = (c) => (c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4);
const gam = (c) => (c <= 0.0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - 0.055);
// Björn Ottosson's OKLab matrices (theme.c uses the same)
function rgbToOklch([r, g, b]) {
  [r, g, b] = [r, g, b].map(lin);
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  const L = 0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s;
  const A = 1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s;
  const B = 0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s;
  return [L, Math.hypot(A, B), ((Math.atan2(B, A) * 180) / Math.PI + 360) % 360];
}
function oklchToRgb(L, C, H) {
  const a = C * Math.cos((H * Math.PI) / 180), b = C * Math.sin((H * Math.PI) / 180);
  const l = (L + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (L - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (L - 0.0894841775 * a - 1.291485548 * b) ** 3;
  return [4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s].map((c) => Math.min(1, Math.max(0, gam(c))));
}
const hex = (rgb) => '#' + rgb.map((c) => Math.round(c * 255).toString(16).padStart(2, '0')).join('');

// The dark tokens for the theme now on the page: { '--token': '#rrggbb', ... }
function darkTokens() {
  const cs = getComputedStyle(document.documentElement);
  const lch = (name, fallback) => {
    const c = parseColour(cs.getPropertyValue(name));
    return c ? rgbToOklch(c) : fallback;
  };
  const deep = lch('--violet-deep', [0.27, 0.09, 295]), violet = lch('--violet', [0.36, 0.11, 295]);
  const gold = lch('--gold', [0.8, 0.12, 85]), sel = lch('--select', [0.5, 0.11, 300]);
  // the neutrals' tint: the title bar's hue, or the purple's when the bar is grey
  const [hn, cn] = deep[1] >= 0.01 ? [deep[2], Math.min(deep[1], 0.03)] : [violet[2], Math.min(violet[1], 0.02)];
  const n = (L, k = 1) => hex(oklchToRgb(L, cn * k, hn));
  const at = (L, C, H) => hex(oklchToRgb(L, C, H));
  const vC = Math.min(Math.max(violet[1], 0.06), 0.14);
  const t = {
    '--lilac-0': n(0.17, 0.8), '--lilac-1': n(0.215), '--lilac-2': n(0.29), '--lilac-3': n(0.4, 1.2),
    '--white': n(0.2), '--cream': at(0.235, Math.min(gold[1], 0.02), gold[2]),
    '--ink': n(0.93, 0.4), '--ink-2': n(0.75, 0.6), '--ink-3': n(0.6, 0.6),
    '--violet': at(0.66, vC, violet[2]), '--violet-soft': at(0.55, vC, violet[2]), '--violet-deep': n(0.12),
    '--snow': n(0.97, 0.3), '--title-ink': n(0.97, 0.3),
    '--bar-btn': n(0.21), '--bar-btn-hover': n(0.28), '--bar-hover': n(0.25), '--bar-ink': n(0.88, 0.6),
    '--dot': n(0.12), '--track': n(0.33),
    '--select': at(Math.max(sel[0], 0.7), sel[1], sel[2]), '--select-ink': at(0.15, 0.02, sel[2]),
    '--line-gold': at(0.32, 0.03, gold[2]), '--line-gold-strong': at(0.38, 0.035, gold[2]),
    '--gold-soft-press': at(0.3, 0.04, gold[2]),
    '--ok-bg': at(0.27, 0.05, 155), '--bad-bg': at(0.27, 0.05, 25), '--bad': at(0.75, 0.14, 25),
    '--bad-text': at(0.78, 0.13, 25), '--warn-icon': at(0.75, 0.13, 75), '--warn-ink': at(0.82, 0.11, 85),
  };
  const bar = oklchToRgb(0.12, cn, hn).map((c) => Math.round(c * 255));
  t['--bar'] = `rgb(${bar.join(' ')} / .92)`;
  t['--shadow'] = 'rgb(0 0 0 / .35)';
  t['--shadow-strong'] = 'rgb(0 0 0 / .6)';
  // text on gold: the theme's own when it is dark enough, else a dark one in the gold's hue
  const gi = lch('--gold-ink', null);
  if (!gi || gi[0] > 0.5) t['--gold-ink'] = at(0.16, 0.03, gold[2]);
  return t;
}

function darkOn() {
  try { return localStorage.getItem(DARK_KEY) === '1'; } catch (e) { return false; }
}

// Dark mode over the theme now shown (after applyTheme): computed from the theme alone, so the dark style is
// emptied first
function applyDark(on) {
  const style = document.getElementById('dark-css');
  if (!style) return;
  style.textContent = '';
  if (on) style.textContent = ':root {' + Object.entries(darkTokens()).map(([k, v]) => `${k}: ${v};`).join(' ') + '}';
  document.documentElement.toggleAttribute('data-dark', !!on);
  const sw = document.getElementById('set-dark');
  if (sw) sw.setAttribute('aria-checked', String(!!on));
}

// on / off: kept on this PC and passed to the game when it starts (play: dark); the game's own switch comes back
// through the 'game-dark' event (app.js)
function setDark(on) {
  try { localStorage.setItem(DARK_KEY, on ? '1' : '0'); } catch (e) { /* this run only */ }
  applyDark(on);
}
document.getElementById('set-dark')?.addEventListener('click', () => setDark(!darkOn()));

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
  applyDark(darkOn());
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
  const currentId = (THEMES[themeAt] || {}).id;  // before the list is replaced: themeAt indexes the old one
  await refreshThemes();  // a file added or changed since the last time shows now
  themeAt = Math.max(0, THEMES.findIndex((t) => t.id === currentId));
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
  await applyTheme(i < 0 ? 0 : i);})();
