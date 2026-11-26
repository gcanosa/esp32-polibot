// Light or dark on both pages: what the visitor chose, else what their device asks for.
// Loaded in the head, so a page never flashes in the wrong colours.
const THEME_KEY = 'polibot_theme';
const darkDevice = matchMedia('(prefers-color-scheme: dark)');

function chosenTheme() {
  try { return localStorage.getItem(THEME_KEY); } catch(e) { return null; }
}

const showTheme = theme => document.documentElement.dataset.theme = theme || (darkDevice.matches ? 'dark' : 'light');

function toggleTheme() {
  const next = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark';
  showTheme(next);
  try { localStorage.setItem(THEME_KEY, next); } catch(e) { /* a private window: it lasts until the page is left */ }
}

showTheme(chosenTheme());
darkDevice.addEventListener('change', () => showTheme(chosenTheme()));
