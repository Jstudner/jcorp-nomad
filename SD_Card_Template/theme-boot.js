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
      // the primary colour mixed toward black, worked out here: color-mix() in a stylesheet needs iOS 16.2
      function shade(hex, pct){ const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(hex||'').trim()); if (!m) return 'color-mix(in srgb, ' + hex + ' ' + pct + '%, #000)'; const h = (v) => ('0' + Math.round(parseInt(v, 16) * pct / 100).toString(16)).slice(-2); return '#' + h(m[1]) + h(m[2]) + h(m[3]); }
      function toRgb(hex){ const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(String(hex||'').trim()); return m ? parseInt(m[1],16)+', '+parseInt(m[2],16)+', '+parseInt(m[3],16) : ''; }
      const style = document.createElement('style');
      style.id = 'custom-theme-style';
      style.textContent = `
        :root {
          --primary: ${colors.primary} !important;
          --primary-rgb: ${toRgb(colors.primary)} !important;
          --primary-dark: ${colors.primaryDark} !important;
          --primary-600: ${shade(colors.primary, 88)} !important;
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
          --primary-dark: ${shade(colors.primaryDarkTheme, 80)} !important;
          --primary-600: ${shade(colors.primaryDarkTheme, 88)} !important;
        }
      `;
      document.head.appendChild(style);
    }
  } catch(e) {}
  // older phones (iOS before 15, Chrome before 88) ignore aspect-ratio and covers collapse
  // to nothing: set the height from the width there
  if (!(window.CSS && CSS.supports && CSS.supports('aspect-ratio', '1'))) {
    var shapes = [['.thumb-wrapper', 1.5], ['.resume-thumb', 1.5], ['.np-art', 1]];
    if (/gallery/.test(location.pathname)) shapes.push(['.media-card', 1]);
    var fitting = false;
    var fit = function() {
      fitting = false;
      for (var i = 0; i < shapes.length; i++) {
        var els = document.querySelectorAll(shapes[i][0]);
        for (var j = 0; j < els.length; j++) {
          var w = els[j].getBoundingClientRect().width;
          if (w) els[j].style.height = Math.round(w * shapes[i][1]) + 'px';
        }
      }
    };
    var soon = function() { if (!fitting) { fitting = true; setTimeout(fit, 60); } };
    document.addEventListener('DOMContentLoaded', function() {
      fit();
      if (window.MutationObserver) new MutationObserver(soon).observe(document.body, { childList: true, subtree: true });
    });
    window.addEventListener('resize', soon);
    window.addEventListener('orientationchange', soon);
  }
})();
