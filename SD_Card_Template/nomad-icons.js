// Nomad's icons: 24 grid, 2px round strokes in currentColor so they follow the theme
// (emoji don't). Play/skip are solid to match the music player.
//
//   <button data-icon="download"></button>        filled in when the page loads
//   el.innerHTML = NomadIcons.svg('folder', 18)    for anything built later
(function () {
  const STROKE = {
    'download':       '<path d="M12 3v12"/><polyline points="7 10 12 15 17 10"/><path d="M5 21h14"/>',
    'plus':           '<line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>',
    'check':          '<polyline points="20 6 9 17 4 12"/>',
    'x':              '<line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>',
    'chevron-up':     '<polyline points="6 15 12 9 18 15"/>',
    'chevron-down':   '<polyline points="6 9 12 15 18 9"/>',
    'chevron-left':   '<polyline points="15 6 9 12 15 18"/>',
    'chevron-right':  '<polyline points="9 6 15 12 9 18"/>',
    'music':          '<path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/>',
    'folder':         '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
    'book':           '<path d="M2 4h6a4 4 0 0 1 4 4v13a3 3 0 0 0-3-3H2z"/><path d="M22 4h-6a4 4 0 0 0-4 4v13a3 3 0 0 1 3-3h7z"/>',
    'books':          '<rect x="3" y="4" width="5" height="16" rx="1"/><rect x="10" y="4" width="5" height="16" rx="1"/><path d="M17 6.2l3.4-.9 3 12.6-3.4.9z"/>',
    'headphones':     '<path d="M3 18v-6a9 9 0 0 1 18 0v6"/><path d="M21 19a2 2 0 0 1-2 2h-1a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2h3z"/><path d="M3 19a2 2 0 0 0 2 2h1a2 2 0 0 0 2-2v-3a2 2 0 0 0-2-2H3z"/>',
    'search':         '<circle cx="11" cy="11" r="7"/><line x1="21" y1="21" x2="16" y2="16"/>',
    'menu':           '<line x1="4" y1="6" x2="20" y2="6"/><line x1="4" y1="12" x2="20" y2="12"/><line x1="4" y1="18" x2="20" y2="18"/>',
    'list':           '<line x1="9" y1="6" x2="20" y2="6"/><line x1="9" y1="12" x2="20" y2="12"/><line x1="9" y1="18" x2="20" y2="18"/><circle cx="4.5" cy="6" r="1"/><circle cx="4.5" cy="12" r="1"/><circle cx="4.5" cy="18" r="1"/>',
    'home':           '<path d="M3 11l9-8 9 8"/><path d="M5 10v10h14V10"/>',
    'refresh':        '<path d="M21 12a9 9 0 1 1-3-6.7"/><polyline points="21 3 21 9 15 9"/>',
    'undo':           '<path d="M3 12a9 9 0 1 0 3-6.7"/><polyline points="3 3 3 9 9 9"/>',
    'warning':        '<path d="M10.3 3.9L1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/>',
    'error':          '<circle cx="12" cy="12" r="9"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/>',
    'success':        '<circle cx="12" cy="12" r="9"/><polyline points="16 9 10.5 15 8 12.5"/>',
    'info':           '<circle cx="12" cy="12" r="9"/><line x1="12" y1="11" x2="12" y2="16"/><line x1="12" y1="8" x2="12.01" y2="8"/>',
    'terminal':       '<polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/>',
    'users':          '<circle cx="9" cy="8" r="4"/><path d="M2 21v-1a7 7 0 0 1 14 0v1"/><path d="M16 4.1a4 4 0 0 1 0 7.8"/><path d="M22 21v-1a7 7 0 0 0-4-6.3"/>',
    'lock':           '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
    'thermometer':    '<path d="M14 14.8V4a2 2 0 0 0-4 0v10.8a4 4 0 1 0 4 0z"/>',
    'timer':          '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2.5 2.5"/><line x1="9" y1="2" x2="15" y2="2"/>',
    'pencil':         '<path d="M16.5 3.5l4 4L8 20H4v-4z"/>',
    'sun':            '<circle cx="12" cy="12" r="4"/><line x1="12" y1="2" x2="12" y2="4"/><line x1="12" y1="20" x2="12" y2="22"/><line x1="2" y1="12" x2="4" y2="12"/><line x1="20" y1="12" x2="22" y2="12"/><line x1="4.9" y1="4.9" x2="6.3" y2="6.3"/><line x1="17.7" y1="17.7" x2="19.1" y2="19.1"/><line x1="4.9" y1="19.1" x2="6.3" y2="17.7"/><line x1="17.7" y1="6.3" x2="19.1" y2="4.9"/>',
    'moon':           '<path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/>',
    'swap':           '<polyline points="17 2 21 6 17 10"/><path d="M3 6h18"/><polyline points="7 22 3 18 7 14"/><path d="M21 18H3"/>',
    'layers':         '<polygon points="12 3 22 8.5 12 14 2 8.5"/><polyline points="2 13 12 18.5 22 13"/>',
    'locate':         '<circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="2.5"/><line x1="12" y1="1.5" x2="12" y2="4.5"/><line x1="12" y1="19.5" x2="12" y2="22.5"/><line x1="1.5" y1="12" x2="4.5" y2="12"/><line x1="19.5" y1="12" x2="22.5" y2="12"/>',
    'pin':            '<path d="M12 22s7-6.6 7-12a7 7 0 0 0-14 0c0 5.4 7 12 7 12z"/><circle cx="12" cy="10" r="2.5"/>',
    'nearby':         '<rect x="3" y="3" width="18" height="18" rx="4"/><line x1="12" y1="7.5" x2="12" y2="16.5"/><line x1="7.5" y1="12" x2="16.5" y2="12"/>',
    'directions':     '<path d="M11.3 2.7a1 1 0 0 1 1.4 0l8.6 8.6a1 1 0 0 1 0 1.4l-8.6 8.6a1 1 0 0 1-1.4 0l-8.6-8.6a1 1 0 0 1 0-1.4z"/><path d="M9 14v-2a2 2 0 0 1 2-2h4"/><polyline points="13 8 15 10 13 12"/>',
    'navigate':       '<polygon points="12 2 20 21 12 17 4 21"/>',
    'car':            '<path d="M5 17h14v-4.5l-2-5H7l-2 5z"/><circle cx="8" cy="17" r="1.8"/><circle cx="16" cy="17" r="1.8"/><line x1="5" y1="12.5" x2="19" y2="12.5"/>',
    'walk':           '<circle cx="13" cy="4" r="1.8"/><path d="M10 21l2.5-6 3 3v3"/><path d="M8 11l3-3.5 3 1.5 2 3.5"/><path d="M12.5 15l-1-5"/>',
    'arrow-up':       '<line x1="12" y1="20" x2="12" y2="4"/><polyline points="6 10 12 4 18 10"/>',
    'turn-left':      '<path d="M16 20v-8a3 3 0 0 0-3-3H5"/><polyline points="9 5 5 9 9 13"/>',
    'turn-right':     '<path d="M8 20v-8a3 3 0 0 1 3-3h8"/><polyline points="15 5 19 9 15 13"/>',
    'slight-left':    '<path d="M15 21v-7L7 6"/><polyline points="7 12 7 6 13 6"/>',
    'slight-right':   '<path d="M9 21v-7l8-8"/><polyline points="17 12 17 6 11 6"/>',
    'uturn':          '<path d="M8 21V9a4 4 0 0 1 8 0v4"/><polyline points="12 10 16 14 20 10"/>',
    'flag':           '<path d="M5 22V3"/><path d="M5 4h12l-2.5 4L17 12H5"/>',
    'volume':         '<polygon points="4 9 8 9 13 5 13 19 8 15 4 15"/><path d="M16.5 8.5a5 5 0 0 1 0 7"/><path d="M19 6a8.5 8.5 0 0 1 0 12"/>',
    'volume-off':     '<polygon points="4 9 8 9 13 5 13 19 8 15 4 15"/><line x1="17" y1="9" x2="22" y2="14"/><line x1="22" y1="9" x2="17" y2="14"/>',
  };
  const SOLID = {
    'play':           '<path d="M7 4.5v15a1 1 0 0 0 1.53.85l12-7.5a1 1 0 0 0 0-1.7l-12-7.5A1 1 0 0 0 7 4.5z"/>',
    'skip-back':      '<rect x="4" y="5" width="2.5" height="14" rx="1"/><path d="M20 6.1v11.8a1 1 0 0 1-1.55.83L9.6 12.83a1 1 0 0 1 0-1.66l8.85-5.9A1 1 0 0 1 20 6.1z"/>',
    'skip-forward':   '<rect x="17.5" y="5" width="2.5" height="14" rx="1"/><path d="M4 6.1v11.8a1 1 0 0 0 1.55.83l8.85-5.9a1 1 0 0 0 0-1.66L5.55 5.27A1 1 0 0 0 4 6.1z"/>',
  };

  function svg(name, size, cls) {
    const solid = SOLID[name], line = STROKE[name];
    if (!solid && !line) return '';
    const s = size ? `width="${size}" height="${size}"` : 'width="1em" height="1em"';
    const paint = solid ? 'fill="currentColor" stroke="none"'
      : 'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"';
    return `<svg class="nomad-icon${cls ? ' ' + cls : ''}" viewBox="0 0 24 24" ${s} ${paint} aria-hidden="true" focusable="false">${solid || line}</svg>`;
  }

  // fill every [data-icon]. an optional data-icon-size sets the pixel size (default: 1em)
  function fill(root) {
    (root || document).querySelectorAll('[data-icon]').forEach((el) => {
      if (el.querySelector(':scope > svg.nomad-icon')) return;
      el.insertAdjacentHTML('afterbegin', svg(el.dataset.icon, el.dataset.iconSize));
    });
  }

  // 1em icons sit on the text baseline like a glyph would
  const style = document.createElement('style');
  style.textContent = 'svg.nomad-icon{display:inline-block;vertical-align:-0.15em;flex-shrink:0}';
  document.head.appendChild(style);

  window.NomadIcons = { svg, fill };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => fill());
  else fill();
})();
