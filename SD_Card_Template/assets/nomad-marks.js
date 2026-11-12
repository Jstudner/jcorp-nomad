/* nomad-marks.js - bookmarks and notes in this phone's localStorage, never on the card.
   One store for every reader: a place in a book, a page in a PDF or comic, a time in
   audio or video. Each page supplies where you are and how to go back:

     NomadMarks.attach(button, {
       key:   () => '/Books/Moby Dick.epub',          // what the marks belong to
       title: () => 'Moby Dick',
       here:  () => ({ at: cfi, label: 'Chapter 3 · 41%' }),   // null when nowhere yet
       go:    (at) => rendition.display(at),
     }); */
(function () {
  'use strict';
  const STORE = 'nomad_marks_v1';
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const icon = (n, s) => (window.NomadIcons ? NomadIcons.svg(n, s || 18) : '');

  function all() {
    try { return JSON.parse(localStorage.getItem(STORE) || '{}') || {}; } catch (e) { return {}; }
  }
  function save(store) {
    try { localStorage.setItem(STORE, JSON.stringify(store)); return true; } catch (e) { return false; }
  }
  const list = (key) => (all()[key] || []).slice().sort((a, b) => (a.order ?? a.time) - (b.order ?? b.time));
  function add(key, mark) {
    const store = all();
    const m = Object.assign({ id: Date.now().toString(36) + Math.random().toString(36).slice(2, 6), time: Date.now() }, mark);
    (store[key] = store[key] || []).push(m);
    return save(store) ? m : null;
  }
  function update(key, id, patch) {
    const store = all();
    const m = (store[key] || []).find((x) => x.id === id);
    if (m) Object.assign(m, patch);
    return save(store);
  }
  function remove(key, id) {
    const store = all();
    store[key] = (store[key] || []).filter((x) => x.id !== id);
    if (!store[key].length) delete store[key];
    return save(store);
  }
  function exportAll() {
    const blob = new Blob([JSON.stringify({ nomadMarks: 1, saved: new Date().toISOString(), marks: all() }, null, 2)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'nomad-bookmarks-' + new Date().toISOString().slice(0, 10) + '.json';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
  }

  const CSS = `
  .nm-veil{position:fixed;inset:0;z-index:9500;background:rgba(0,0,0,.5);display:flex;align-items:flex-end;justify-content:center}
  @media (min-width:700px){.nm-veil{align-items:center}}
  .nm-sheet{background:var(--card-bg,var(--card,#fff));color:var(--text,#222);width:100%;max-width:460px;max-height:86vh;overflow:auto;box-sizing:border-box;
    border-radius:18px 18px 0 0;padding:16px 16px calc(16px + env(safe-area-inset-bottom));box-shadow:0 -10px 40px rgba(0,0,0,.3);font-family:inherit}
  @media (min-width:700px){.nm-sheet{border-radius:18px}}
  .nm-head{display:flex;align-items:center;gap:8px;margin-bottom:12px}
  .nm-head h3{margin:0;font-size:1rem;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .nm-btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:38px;padding:0 14px;border-radius:10px;border:1px solid var(--card-border,rgba(0,0,0,.15));
    background:var(--card-bg,var(--card,#fff));color:var(--text,#222);font:inherit;font-weight:700;font-size:.88rem;cursor:pointer}
  .nm-btn.primary{background:var(--primary,#007bff);border-color:var(--primary,#007bff);color:#fff}
  .nm-btn.ic{width:38px;padding:0}
  .nm-btn svg{width:17px;height:17px}
  .nm-add{display:grid;gap:8px;margin-bottom:14px;padding-bottom:14px;border-bottom:1px solid var(--card-border,rgba(0,0,0,.12))}
  .nm-here{font-size:.82rem;color:var(--muted,#666)}
  .nm-note{width:100%;box-sizing:border-box;min-height:64px;resize:vertical;padding:9px 11px;border-radius:10px;border:1px solid var(--card-border,rgba(0,0,0,.15));
    background:var(--bg,#fafafa);color:var(--text,#222);font:inherit;font-size:.92rem}
  .nm-item{display:flex;gap:10px;align-items:flex-start;padding:10px 4px;border-bottom:1px solid color-mix(in srgb,var(--card-border,#ddd) 60%,transparent)}
  .nm-item .go{flex:1;min-width:0;text-align:left;border:0;background:none;color:inherit;font:inherit;cursor:pointer;padding:0}
  .nm-item b{display:block;font-size:.9rem}
  .nm-item span{display:block;font-size:.82rem;color:var(--muted,#666);white-space:pre-wrap;overflow-wrap:anywhere;margin-top:2px}
  .nm-item small{display:block;font-size:.72rem;color:var(--muted,#888);margin-top:3px}
  .nm-empty{color:var(--muted,#666);font-size:.88rem;padding:8px 2px}
  .nm-foot{display:flex;justify-content:space-between;align-items:center;margin-top:12px;font-size:.76rem;color:var(--muted,#888)}
  .nm-foot button{border:0;background:none;color:var(--primary,#007bff);font:inherit;font-weight:700;cursor:pointer;padding:4px}
  `;
  function css() {
    if (document.getElementById('nm-style')) return;
    const st = document.createElement('style'); st.id = 'nm-style'; st.textContent = CSS; document.head.appendChild(st);
  }

  function when(t) {
    const d = new Date(t);
    return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' }) + ', ' + d.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
  }

  // the popup
  function open(o) {
    css();
    const key = o.key();
    if (!key) return;
    const veil = document.createElement('div');
    veil.className = 'nm-veil';
    veil.innerHTML = `<div class="nm-sheet" role="dialog" aria-modal="true">
      <div class="nm-head"><h3>Bookmarks · ${esc(o.title ? o.title() : '')}</h3>
        <button class="nm-btn ic" data-a="close" aria-label="Close">${icon('x')}</button></div>
      <div class="nm-add"></div>
      <div class="nm-list"></div>
      <div class="nm-foot"><span>Kept on this phone only</span><button data-a="export">Save all as a file</button></div>
    </div>`;
    document.body.appendChild(veil);
    const sheet = veil.firstElementChild, q = (s) => sheet.querySelector(s);
    const close = () => { veil.remove(); document.removeEventListener('keydown', onKey); };
    const onKey = (e) => { if (e.key === 'Escape') close(); };
    document.addEventListener('keydown', onKey);
    veil.addEventListener('click', (e) => { if (e.target === veil) close(); });

    const here = o.here ? o.here() : null;
    q('.nm-add').innerHTML = here
      ? `<div class="nm-here">Here: ${esc(here.label)}</div>
         <textarea class="nm-note" maxlength="1000" placeholder="Note (optional)"></textarea>
         <button class="nm-btn primary" data-a="add">${icon('plus')}Bookmark this spot</button>`
      : `<div class="nm-here">Nothing open yet</div>`;

    function draw() {
      const marks = list(key);
      q('.nm-list').innerHTML = marks.length ? marks.map((m) => `<div class="nm-item" data-id="${esc(m.id)}">
          <button class="go" data-a="go"><b>${esc(m.label)}</b>${m.note ? `<span>${esc(m.note)}</span>` : ''}<small>${esc(when(m.time))}</small></button>
          <button class="nm-btn ic" data-a="edit" aria-label="Edit note">${icon('pencil')}</button>
          <button class="nm-btn ic" data-a="del" aria-label="Delete">${icon('x')}</button></div>`).join('')
        : '<div class="nm-empty">No bookmarks yet.</div>';
    }
    draw();

    sheet.addEventListener('click', (e) => {
      const b = e.target.closest('[data-a]'); if (!b) return;
      const item = b.closest('.nm-item'), id = item && item.dataset.id;
      const m = id && list(key).find((x) => x.id === id);
      switch (b.dataset.a) {
        case 'close': close(); break;
        case 'export': exportAll(); break;
        case 'add': {
          const h = o.here && o.here();
          if (!h) return;
          const note = q('.nm-note').value.trim();
          if (!add(key, { at: h.at, label: h.label, note, order: h.order })) { q('.nm-here').textContent = "Couldn't save: this phone's storage is full."; return; }
          q('.nm-note').value = '';
          draw(); o.onChange && o.onChange();
          break;
        }
        case 'go': if (m) { close(); o.go(m.at); } break;
        case 'edit': {
          if (!m) return;
          const n = prompt('Note for ' + m.label, m.note || '');
          if (n !== null) { update(key, id, { note: n.trim().slice(0, 1000) }); draw(); }
          break;
        }
        case 'del': if (m && confirm('Delete the bookmark at ' + m.label + '?')) { remove(key, id); draw(); o.onChange && o.onChange(); } break;
      }
    });
  }

  // a toolbar button that opens the popup, with a dot when this item has bookmarks
  function attach(button, o) {
    if (!button) return;
    button.addEventListener('click', () => open(o));
    const mark = () => { try { button.classList.toggle('has-marks', !!(o.key() && list(o.key()).length)); } catch (e) {} };
    o.onChange = mark;
    mark();
    window.addEventListener('storage', (e) => { if (e.key === STORE) mark(); });
    return { refresh: mark, open: () => open(o) };
  }

  // seconds as 1:02:15 or 4:07, for the time-based readers
  const clock = (s) => {
    s = Math.max(0, Math.floor(s || 0));
    const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, x = String(s % 60).padStart(2, '0');
    return h ? `${h}:${String(m).padStart(2, '0')}:${x}` : `${m}:${x}`;
  };

  window.NomadMarks = { attach, open, list, add, update, remove, exportAll, clock };
})();
