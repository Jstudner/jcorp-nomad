// <!-- Version 4 -->
/* runs in <head> before master.css paints so dark mode / saved theme apply on the
 * first paint instead of flashing default. localStorage only, no network.
 * menu/theme-customization-ui/epub/pdf keep their own copy that also fetches
 * /.system-theme.json so the card's theme wins over a stale cache. */
(function() {
  try {
    const isDark = localStorage.getItem('nomad_dark_mode') === 'true';
    if (isDark) document.documentElement.classList.add('dark');

    const saved = localStorage.getItem('nomad_custom_theme');
    if (saved) {
      const colors = JSON.parse(saved);
      const style = document.createElement('style');
      style.id = 'custom-theme-style';
      style.textContent = `
        :root {
          --primary: ${colors.primary} !important;
          --primary-dark: ${colors.primaryDark} !important;
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
        }
      `;
      document.head.appendChild(style);
    }
  } catch(e) {}
})();
