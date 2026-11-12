// <!-- Version 5 -->
/* Runs in <head> so dark mode and the theme apply on first paint, no flash. localStorage
 * only: theme-manager.js refreshes it from /.system-theme.json after load.
 * Dark mode: this browser's choice (nomad_dark_mode) wins, else the card's default
 * (nomad_dark_default): 'dark', 'auto' (follow the device) or unset for light. */
(function() {
  window.nomadIsDark = function() {
    try {
      const own = localStorage.getItem('nomad_dark_mode');
      if (own === 'true' || own === 'false') return own === 'true';
      const d = localStorage.getItem('nomad_dark_default');
      if (d === 'dark') return true;
      if (d === 'auto') return !!(window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches);
    } catch (e) {}
    return false;
  };
  try {
    if (window.nomadIsDark()) {
      document.documentElement.classList.add('dark');
      // master.css keys dark mode on body.dark and <body> doesnt exist yet while this runs
      // in <head>. tag it the instant the parser creates it, otherwise dark pages flash light
      new MutationObserver((m, o) => {
        if (document.body) { document.body.classList.add('dark'); o.disconnect(); }
      }).observe(document.documentElement, { childList: true });
    }

    const saved = localStorage.getItem('nomad_custom_theme');
    if (saved) {
      const colors = JSON.parse(saved);
      function toRgb(hex){ const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(hex||'').trim()); return m ? parseInt(m[1],16)+', '+parseInt(m[2],16)+', '+parseInt(m[3],16) : ''; }
      const style = document.createElement('style');
      style.id = 'custom-theme-style';
      style.textContent = `
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
      document.head.appendChild(style);
    }
  } catch(e) {}
})();
