// <!-- Version 5 -->
/* Theme manager. The card holds one theme for every device in /.system-theme.json: the
   colours plus an optional "mode" ('dark' or 'auto'). theme-boot.js paints this browser's
   copy first, then this file fetches the card's once per page and updates the copy. */

const ThemeManager = {
  STORAGE_KEY: 'nomad_dark_mode',          // this browser's own choice: 'true' / 'false' / none
  DEFAULT_KEY: 'nomad_dark_default',       // the card's default, copied here: 'dark' / 'auto' / none
  THEME_STORAGE_KEY: 'nomad_custom_theme', // the card's colours, copied here
  THEME_FILE_PATH: '/.system-theme.json',
  COLOR_KEYS: ['primary', 'primaryDark', 'bgLight', 'cardLight', 'textLight', 'mutedLight',
               'bgDark', 'cardDark', 'textDark', 'mutedDark', 'primaryDarkTheme'],
  MODES: ['light', 'dark', 'auto'],

  init() {
    try {
      this.applyDarkMode(this.getDarkMode());
      const cached = this.loadThemeFromStorage();
      if (cached) this.applyThemeColors(cached);
      this.sync();
    } catch (e) {
      console.error('[ThemeManager] Init failed:', e);
    }
  },

  // the card's theme file now: an object, null when the card has none, undefined when the
  // card didn't answer (then nothing changes: the copy here is the best there is)
  async fetchCard() {
    try {
      const r = await fetch(this.THEME_FILE_PATH, { cache: 'no-cache' });
      if (r.status === 404) return null;
      if (!r.ok) return undefined;
      const t = await r.json();
      return t && typeof t === 'object' ? t : null;
    } catch (e) {
      return undefined;
    }
  },

  async sync() {
    const file = await this.fetchCard();
    if (file === undefined) return;
    const before = this.getDarkMode();
    this.remember(file);
    this.applyThemeColors(this.colorsOf(file));
    const now = this.getDarkMode();
    if (now !== before) this.applyDarkMode(now);
  },

  // keep this browser's copy matching the card
  remember(file) {
    try {
      const colors = this.colorsOf(file);
      if (colors) localStorage.setItem(this.THEME_STORAGE_KEY, JSON.stringify(colors));
      else localStorage.removeItem(this.THEME_STORAGE_KEY);
      const mode = file && (file.mode === 'dark' || file.mode === 'auto') ? file.mode : '';
      if (mode) localStorage.setItem(this.DEFAULT_KEY, mode);
      else localStorage.removeItem(this.DEFAULT_KEY);
    } catch (e) {}
  },

  colorsOf(file) {
    if (!file || !file.primary) return null;
    const c = {};
    for (const k of this.COLOR_KEYS) if (file[k]) c[k] = file[k];
    return c;
  },

  modeOf(file) {
    return file && (file.mode === 'dark' || file.mode === 'auto') ? file.mode : 'light';
  },

  getDarkMode() {
    if (window.nomadIsDark) return window.nomadIsDark();
    try { return localStorage.getItem(this.STORAGE_KEY) === 'true'; } catch (e) { return false; }
  },

  // this browser's own choice, from a sun/moon button
  setDarkMode(isDark) {
    try { localStorage.setItem(this.STORAGE_KEY, isDark ? 'true' : 'false'); } catch (e) {}
    this.applyDarkMode(isDark);
  },

  // back to the card's default on this browser
  clearOwnDarkMode() {
    try { localStorage.removeItem(this.STORAGE_KEY); } catch (e) {}
    this.applyDarkMode(this.getDarkMode());
  },

  hasOwnDarkMode() {
    try { const v = localStorage.getItem(this.STORAGE_KEY); return v === 'true' || v === 'false'; } catch (e) { return false; }
  },

  applyDarkMode(isDark) {
    if (!document.body) return;
    document.body.classList.toggle('dark', !!isDark);
    document.documentElement.classList.toggle('dark', !!isDark);
    // pages that colour things by hand (shows, movies, admin) listen for this
    try { window.dispatchEvent(new CustomEvent('nomad-dark', { detail: !!isDark })); } catch (e) {}
  },

  toggleDarkMode() {
    this.setDarkMode(!this.getDarkMode());
  },

  // Writing the theme file needs admin (root dotfile, same rule as /config). The editor
  // runs in the admin page, so the sign-in token is here. No admin password: no header.
  adminHeaders() {
    try {
      const t = sessionStorage.getItem('nomad_admin_token') || localStorage.getItem('nomad_admin_token');   // kept sign-ins live in localStorage
      return t ? { 'X-Admin-Token': t } : {};
    } catch (e) {
      return {};
    }
  },

  why(status) {
    return status === 401 ? 'Sign in to the admin page first.' : `Nomad didn't save it (HTTP ${status}).`;
  },

  // one whole-file write (/save goes through the config writer), or delete when empty
  async writeCard(file) {
    try {
      const empty = !file || !Object.keys(file).length;
      const r = empty
        ? await fetch('/delete', { method: 'POST', headers: Object.assign({ 'Content-Type': 'application/x-www-form-urlencoded' }, this.adminHeaders()),
            body: new URLSearchParams({ filename: this.THEME_FILE_PATH }) })
        : await fetch('/save', { method: 'POST', headers: Object.assign({ 'Content-Type': 'application/x-www-form-urlencoded' }, this.adminHeaders()),
            body: new URLSearchParams({ filename: this.THEME_FILE_PATH, content: JSON.stringify(file, null, 2) }) });
      if (r.ok || (empty && r.status === 404)) { this.remember(empty ? null : file); return { ok: true }; }
      return { ok: false, msg: this.why(r.status) };
    } catch (e) {
      return { ok: false, msg: "Couldn't reach Nomad." };
    }
  },

  // the colours for every device; the dark mode default already on the card is kept
  async saveThemeToServer(colors) {
    const file = await this.fetchCard();
    if (file === undefined) return { success: false, message: "Couldn't reach Nomad. Nothing was saved." };
    const next = Object.assign({}, this.colorsOf(colors) || {});
    if (this.modeOf(file) !== 'light') next.mode = this.modeOf(file);
    const r = await this.writeCard(next);
    if (!r.ok) return { success: false, message: r.msg };
    this.applyThemeColors(this.colorsOf(next));
    return { success: true, message: 'Saved. Every device picks it up on its next page load.' };
  },

  saveThemeToStorage(colors) {
    return this.saveThemeToServer(colors);
  },

  // what dark mode starts as on every device: 'light', 'dark' or 'auto' (follow the device)
  async setDefaultMode(mode) {
    if (!this.MODES.includes(mode)) return { success: false, message: 'Unknown mode.' };
    const file = await this.fetchCard();
    if (file === undefined) return { success: false, message: "Couldn't reach Nomad. Nothing was saved." };
    const next = Object.assign({}, this.colorsOf(file) || {});
    if (mode !== 'light') next.mode = mode;
    const r = await this.writeCard(next);
    if (!r.ok) return { success: false, message: r.msg };
    this.applyDarkMode(this.getDarkMode());
    return { success: true, message: 'Saved.' };
  },

  // the default colours back; the dark mode default stays as it is
  async resetTheme() {
    const file = await this.fetchCard();
    if (file === undefined) return { success: false, message: "Couldn't reach Nomad. Nothing was changed." };
    const next = this.modeOf(file) !== 'light' ? { mode: this.modeOf(file) } : null;
    const r = await this.writeCard(next);
    if (!r.ok) return { success: false, message: r.msg };
    this.applyThemeColors(null);
    return { success: true, message: 'Default colours restored on every device.' };
  },

  async loadThemeFromServer() {
    const file = await this.fetchCard();
    return file === undefined ? null : this.colorsOf(file);
  },

  loadThemeFromStorage() {
    try {
      const stored = localStorage.getItem(this.THEME_STORAGE_KEY);
      return stored ? this.colorsOf(JSON.parse(stored)) : null;
    } catch (e) {
      return null;
    }
  },

  // colours on this page (null: the built-in ones)
  applyThemeColors(colors) {
    let styleEl = document.getElementById('custom-theme-style');
    if (!colors) { if (styleEl) styleEl.remove(); return; }
    function toRgb(hex){ const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(hex||'').trim()); return m ? parseInt(m[1],16)+', '+parseInt(m[2],16)+', '+parseInt(m[3],16) : ''; }
    if (!styleEl) {
      styleEl = document.createElement('style');
      styleEl.id = 'custom-theme-style';
      document.head.appendChild(styleEl);
    }
    styleEl.textContent = `
      :root {
        --primary: ${colors.primary} !important;
        --primary-rgb: ${toRgb(colors.primary)} !important;
        --primary-dark: ${colors.primaryDark} !important;
        --primary-600: color-mix(in srgb, ${colors.primary} 88%, #000) !important;
        --bg: ${colors.bgLight} !important;
        --card: ${colors.cardLight} !important;
        --card-bg: ${colors.cardLight} !important;
        --text: ${colors.textLight} !important;
        --muted: ${colors.mutedLight} !important;
      }
      body.dark {
        --bg: ${colors.bgDark} !important;
        --card: ${colors.cardDark} !important;
        --card-bg: ${colors.cardDark} !important;
        --text: ${colors.textDark} !important;
        --muted: ${colors.mutedDark} !important;
        --primary: ${colors.primaryDarkTheme} !important;
        --primary-rgb: ${toRgb(colors.primaryDarkTheme)} !important;
        --primary-dark: color-mix(in srgb, ${colors.primaryDarkTheme} 80%, #000) !important;
        --primary-600: color-mix(in srgb, ${colors.primaryDarkTheme} 88%, #000) !important;
      }
    `;
  },

  async loadThemes() {
    try {
      const response = await fetch('/default-themes.json');
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const data = await response.json();
      return data.themes || [];
    } catch (error) {
      console.error('[ThemeManager] Load themes error:', error);
      return [];
    }
  }
};

if (typeof window !== 'undefined') {
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => ThemeManager.init());
  } else {
    ThemeManager.init();
  }
}
