/* nomad-files.js - the card's files, for the admin page's Files section and /filebrowser.
   NomadFiles.mount(element, { path }) draws the browser into the element. The listing is live
   from /listfiles. After a change the folder's library index is rebuilt in the background.
   Needs nomad-utils.js (NomadUpload, NomadIcons). */
(function () {
  'use strict';
  const CSS = `
  .nf{display:flex;flex-direction:column;min-height:0;height:100%;color:var(--text);font-size:.92rem}
  .nf-head{display:flex;align-items:center;gap:6px;padding-bottom:8px}
  .nf-crumbs{flex:1;min-width:0;display:flex;align-items:center;gap:1px;overflow-x:auto;scrollbar-width:none;white-space:nowrap;
    background:color-mix(in srgb,var(--primary) 7%,transparent);border-radius:10px;padding:0 4px;height:36px}
  .nf-crumbs::-webkit-scrollbar{display:none}
  .nf-crumbs button{border:0;background:none;color:var(--primary);font-weight:700;padding:5px 6px;border-radius:7px;cursor:pointer;font-size:.88rem;display:inline-flex;align-items:center;gap:4px}
  .nf-crumbs button svg{width:15px;height:15px}
  .nf-crumbs button:last-child{color:var(--text);cursor:default}
  .nf-crumbs .sep{color:var(--muted);opacity:.6}
  .nf-bar{display:flex;gap:6px;align-items:center;padding-bottom:6px}
  .nf-find{flex:1;min-width:0;position:relative}
  .nf-find svg{position:absolute;left:10px;top:50%;transform:translateY(-50%);width:15px;height:15px;color:var(--muted);pointer-events:none}
  .nf-find input{width:100%;box-sizing:border-box;height:36px;padding:0 10px 0 32px;border:1px solid var(--card-border);border-radius:10px;background:var(--bg);color:var(--text);font-size:.9rem}
  .nf-bar select{height:36px;padding:0 8px;border:1px solid var(--card-border);border-radius:10px;background:var(--bg);color:var(--text);font-size:.85rem;flex:0 0 auto}
  .nf-btn{display:inline-flex;align-items:center;gap:5px;height:36px;box-sizing:border-box;padding:0 12px;border-radius:10px;border:1px solid var(--card-border);background:var(--card-bg);color:var(--text);font-weight:700;font-size:.85rem;cursor:pointer;white-space:nowrap;flex:0 0 auto}
  .nf-btn.ic{width:36px;padding:0;justify-content:center}
  .nf-btn.primary{background:var(--primary);border-color:var(--primary);color:#fff}
  .nf-btn.danger{color:#e5484d;border-color:color-mix(in srgb,#e5484d 40%,transparent)}
  .nf-btn svg{width:16px;height:16px}
  @media (max-width:420px){.nf-head .nf-btn.primary .lbl{display:none}.nf-head .nf-btn.primary{width:36px;padding:0;justify-content:center}}
  .nf-status{font-size:.78rem;color:var(--muted);padding:2px 2px 6px;border-bottom:1px solid var(--card-border)}
  .nf-list{flex:1;min-height:0;overflow:auto;-webkit-overflow-scrolling:touch}
  .nf-row{display:flex;align-items:center;gap:10px;padding:7px 4px;border-bottom:1px solid color-mix(in srgb,var(--card-border) 60%,transparent);cursor:pointer;border-radius:8px}
  .nf-row:hover{background:color-mix(in srgb,var(--primary) 6%,transparent)}
  .nf-ico{flex:0 0 32px;height:32px;border-radius:9px;display:grid;place-items:center;background:color-mix(in srgb,var(--primary) 10%,transparent);color:var(--primary)}
  .nf-ico.dir{background:color-mix(in srgb,var(--primary) 18%,transparent)}
  .nf-ico svg{width:18px;height:18px}
  .nf-ico .ext{font-size:.6rem;font-weight:800;letter-spacing:.02em;text-transform:uppercase}
  .nf-main{flex:1;min-width:0}
  .nf-name{font-weight:600;line-height:1.25;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .nf-meta{font-size:.74rem;line-height:1.2;color:var(--muted);display:flex;gap:8px;align-items:center}
  .nf-dot{width:8px;height:8px;border-radius:50%;display:inline-block}
  .nf-dot.bad{background:#e5484d}.nf-dot.warn{background:#f5a524}.nf-dot.slow{background:var(--muted)}
  .nf-chev{color:var(--muted);flex:0 0 auto}
  .nf-empty{padding:28px 8px;text-align:center;color:var(--muted)}
  .nf-veil{position:fixed;inset:0;background:rgba(0,0,0,.45);z-index:9000;display:flex;align-items:flex-end;justify-content:center}
  .nf-sheet{background:var(--card-bg);color:var(--text);width:100%;max-width:520px;border-radius:16px 16px 0 0;padding:16px 16px calc(16px + env(safe-area-inset-bottom));box-shadow:0 -8px 30px rgba(0,0,0,.25);max-height:88vh;overflow:auto}
  @media (min-width:700px){.nf-veil{align-items:center}.nf-sheet{border-radius:16px}}
  .nf-sheet h3{margin:0 0 4px;font-size:1rem;word-break:break-word}
  .nf-sheet .sub{font-size:.8rem;color:var(--muted);margin-bottom:12px;word-break:break-all}
  .nf-sheet .note{font-size:.82rem;margin:-4px 0 12px;padding:8px 10px;border-radius:8px;background:color-mix(in srgb,#f5a524 14%,transparent)}
  .nf-actions{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:8px}
  .nf-actions .nf-btn{justify-content:center;padding:10px}
  .nf-sheet input[type=text]{width:100%;box-sizing:border-box;padding:9px 10px;border:1px solid var(--card-border);border-radius:10px;background:var(--bg);color:var(--text);font-size:.95rem;margin-bottom:12px}
  .nf-sheet .row{display:flex;gap:8px;justify-content:flex-end}
  .nf-editor{position:fixed;inset:0;z-index:9100;background:var(--card-bg);display:flex;flex-direction:column}
  .nf-editor header{display:flex;align-items:center;gap:8px;padding:8px 12px;border-bottom:1px solid var(--card-border)}
  .nf-editor header b{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .nf-editor textarea{flex:1;border:0;padding:12px;font:13px/1.45 ui-monospace,Menlo,Consolas,monospace;background:var(--bg);color:var(--text);resize:none;outline:none}
  `;
  const MEDIA = /\.(jpe?g|png|gif|webp|avif|bmp|svg|mp4|m4v|webm|mov|mkv|mp3|m4a|flac|ogg|opus|wav|aac|pdf|epub)$/i;
  const TEXT = /\.(txt|json|ndjson|html?|css|js|md|markdown|xml|csv|ya?ml|ini|conf|srt|vtt|cook|m3u8?)$/i;
  const VIDEO = /\.(mp4|m4v|webm|mov|mkv|avi)$/i;
  const ICON = [[/\.(mp3|m4a|flac|ogg|opus|wav|aac|alac|aiff)$/i, 'music'], [/\.(epub|pdf|mobi|azw3|cbz|cbr|djvu)$/i, 'book'], [VIDEO, 'play']];

  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const svg = (name, size) => (window.NomadIcons ? NomadIcons.svg(name, size || 18) : '');
  const norm = (p) => { p = '/' + String(p || '').replace(/\\/g, '/').replace(/\/+/g, '/').replace(/^\/+|\/+$/g, ''); return p === '/' ? '/' : p; };
  const join = (a, b) => norm(a + '/' + b);
  const parent = (p) => { p = norm(p); const i = p.lastIndexOf('/'); return i <= 0 ? '/' : p.slice(0, i); };
  const size = (n) => !n && n !== 0 ? '' : n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(0) + ' KB' : n < 1073741824 ? (n / 1048576).toFixed(1) + ' MB' : (n / 1073741824).toFixed(2) + ' GB';
  const coll = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });
  function headers(extra) {
    const h = Object.assign({}, extra || {});
    try { const t = sessionStorage.getItem('nomad_admin_token') || localStorage.getItem('nomad_admin_token'); if (t) h['X-Admin-Token'] = t; } catch (e) {}
    return h;
  }
  async function post(url, fields) {
    const r = await fetch(url, { method: 'POST', headers: headers({ 'Content-Type': 'application/x-www-form-urlencoded' }), body: new URLSearchParams(fields) });
    let msg = '';
    try { msg = await r.text(); try { msg = JSON.parse(msg).error || msg; } catch (e) {} } catch (e) {}
    return { ok: r.ok, status: r.status, msg };
  }
  const why = (r) => r.status === 401 ? 'Sign in to the admin page first.' : r.status === 403 ? (r.msg || 'Not allowed here.') : (r.msg || 'HTTP ' + r.status);

  // what the video check (admin, Overview) found about a film: the dot and a line
  const Health = {
    map: null,
    async load() {
      if (this.map) return;
      this.map = new Map();
      try {
        const r = await fetch('/.system-index/video-check.ndjson?_=' + Date.now(), { cache: 'no-store' });
        if (!r.ok) return;
        for (const line of (await r.text()).split('\n')) { try { const o = JSON.parse(line); if (o.p && o.f) this.map.set(o.p, o); } catch (e) {} }
      } catch (e) {}
    },
    about(path, sz) {
      const o = this.map && this.map.get(path);
      if (!o || (sz != null && o.sz !== sz)) return null;
      const f = o.f;
      const rules = [[2048, 'bad', "Can't read", 'No video index found.'], [1024, 'bad', "Won't play", 'AVI file. Convert to MP4.'],
        [16, 'bad', "Won't play", 'Old video format. Convert to H.264.'], [32, 'bad', "Won't play", '10-bit video. Convert to 8-bit H.264.'],
        [512, 'warn', 'No iPhone', 'MKV file. Convert to MP4.'], [64, 'warn', 'No sound in Chrome', 'Dolby audio. Convert the audio to AAC.'],
        [4, 'warn', 'Not every browser', 'HEVC video. Convert to H.264.'], [8, 'warn', 'Not older iPhones', 'AV1 video.'],
        [4096, 'warn', "Won't seek in Firefox", 'Fragmented MP4. Fast start in Nomad-Tools fixes it.'],
        [1, 'slow', 'Slow to start', 'Index is at the end of the file.'], [2, 'slow', 'Slow to start', 'Large index. Fast start in Nomad-Tools shrinks it.'],
        [128, 'slow', 'Heavy to stream', 'High bitrate. The 480p preset fixes it.'], [256, 'slow', 'Heavy to stream', '4K video.']];
      const hits = rules.filter((r) => f & r[0]);
      return hits.length ? { level: hits[0][1], label: hits[0][2], detail: hits.map((h) => h[3]).join(' ') } : null;
    },
  };

  function mount(host, opts) {
    opts = opts || {};
    if (!document.getElementById('nf-style')) { const st = document.createElement('style'); st.id = 'nf-style'; st.textContent = CSS; document.head.appendChild(st); }
    const root = document.createElement('div');
    root.className = 'nf';
    root.innerHTML = `
      <div class="nf-head">
        <nav class="nf-crumbs" aria-label="Folder"></nav>
        <button class="nf-btn ic" data-act="refresh" title="Refresh" aria-label="Refresh">${svg('refresh', 16)}</button>
        <button class="nf-btn ic" data-act="mkdir" title="New folder" aria-label="New folder">${svg('plus', 16)}</button>
        <button class="nf-btn primary" data-act="upload" title="Upload here" aria-label="Upload here">${svg('arrow-up', 16)}<span class="lbl">Upload</span></button>
      </div>
      <div class="nf-bar">
        <label class="nf-find">${svg('search', 15)}<input type="search" placeholder="Filter this folder" aria-label="Filter this folder"></label>
        <select aria-label="Sort"><option value="name">A to Z</option><option value="size">Largest</option><option value="type">Type</option></select>
      </div>
      <div class="nf-status"></div>
      <div class="nf-list" role="list"></div>
      <input type="file" multiple hidden>`;
    host.replaceChildren(root);
    const $ = (s) => root.querySelector(s);
    const crumbs = $('.nf-crumbs'), filter = $('input[type=search]'), sort = $('select'), statusEl = $('.nf-status'), list = $('.nf-list'), picker = $('input[type=file]');
    let path = norm(opts.path || '/'), entries = [], truncated = false, loading = 0;
    const say = (t) => { statusEl.textContent = t || ''; };

    async function load(p) {
      path = norm(p);
      const ticket = ++loading;
      say('Loading…');
      drawCrumbs();
      try {
        const r = await fetch('/listfiles?dir=' + encodeURIComponent(path) + '&_=' + Date.now(), { cache: 'no-store', headers: headers() });
        if (ticket !== loading) return;
        if (!r.ok) { entries = []; say(r.status === 404 ? 'This folder is not on the card.' : 'Nomad did not answer (HTTP ' + r.status + ').'); draw(); return; }
        const arr = JSON.parse(await r.text());
        truncated = arr.some((e) => e && e.truncated);
        entries = arr.filter((e) => e && e.name && !e.truncated).map((e) => ({ name: String(e.name).replace(/\/$/, ''), dir: !!e.isDir, size: +e.size || 0 }));
        if (opts.onPath) opts.onPath(path);
        draw();
      } catch (e) {
        if (ticket === loading) { say("Couldn't reach Nomad."); entries = []; draw(); }
      }
    }
    function drawCrumbs() {
      const parts = path.split('/').filter(Boolean);
      let html = `<button data-path="/">${svg('home', 15)}Card</button>`, acc = '';
      for (const p of parts) { acc += '/' + p; html += `<span class="sep">›</span><button data-path="${esc(acc)}">${esc(p)}</button>`; }
      crumbs.innerHTML = html;
      crumbs.scrollLeft = crumbs.scrollWidth;
    }
    function draw() {
      const q = filter.value.trim().toLowerCase();
      const by = sort.value;
      const ext = (n) => (n.lastIndexOf('.') > 0 ? n.slice(n.lastIndexOf('.') + 1).toLowerCase() : '');
      const shown = entries.filter((e) => !q || e.name.toLowerCase().includes(q)).sort((a, b) =>
        (b.dir - a.dir) || (by === 'size' ? b.size - a.size : by === 'type' ? coll.compare(ext(a.name), ext(b.name)) : 0) || coll.compare(a.name, b.name));
      const files = entries.filter((e) => !e.dir), total = files.reduce((s, e) => s + e.size, 0);
      say(`${entries.length - files.length} folder${entries.length - files.length === 1 ? '' : 's'}, ${files.length} file${files.length === 1 ? '' : 's'}` +
          (files.length ? ` (${size(total)})` : '') + (q ? ` · ${shown.length} match` : '') + (truncated ? ' · showing the first part' : ''));
      if (!shown.length) { list.innerHTML = `<div class="nf-empty">${q ? 'Nothing here matches.' : 'This folder is empty.'}</div>`; return; }
      list.innerHTML = shown.map((e) => {
        const full = join(path, e.name);
        let icon = 'folder';
        if (!e.dir) { const hit = ICON.find(([re]) => re.test(e.name)); icon = hit ? hit[1] : ''; }
        const h = !e.dir && VIDEO.test(e.name) ? Health.about(full, e.size) : null;
        const x = ext(e.name);
        return `<div class="nf-row" role="listitem" data-name="${esc(e.name)}" data-dir="${e.dir ? 1 : 0}">
          <div class="nf-ico${e.dir ? ' dir' : ''}">${icon ? svg(icon, 18) : `<span class="ext">${esc(x.slice(0, 4) || 'file')}</span>`}</div>
          <div class="nf-main"><div class="nf-name">${esc(e.name)}</div>
            <div class="nf-meta">${e.dir ? 'Folder' : size(e.size)}${h ? `<span class="nf-dot ${h.level}"></span>${esc(h.label)}` : ''}</div></div>
          <span class="nf-chev">${svg(e.dir ? 'chevron-right' : 'menu', 16)}</span></div>`;
      }).join('');
    }

    function sheet(html, onReady) {
      const veil = document.createElement('div');
      veil.className = 'nf-veil';
      veil.innerHTML = `<div class="nf-sheet" role="dialog" aria-modal="true">${html}</div>`;
      document.body.appendChild(veil);
      const close = () => veil.remove();
      veil.addEventListener('click', (ev) => { if (ev.target === veil) close(); });
      const onKey = (ev) => { if (ev.key === 'Escape') { close(); document.removeEventListener('keydown', onKey); } };
      document.addEventListener('keydown', onKey);
      if (onReady) onReady(veil.firstElementChild, close);
      return close;
    }
    const ask = (title, value, okLabel) => new Promise((resolve) => {
      sheet(`<h3>${esc(title)}</h3><input type="text" value="${esc(value || '')}" autocomplete="off">
        <div class="row"><button class="nf-btn" data-a="no">Cancel</button><button class="nf-btn primary" data-a="ok">${esc(okLabel || 'OK')}</button></div>`, (el, close) => {
        const inp = el.querySelector('input');
        inp.focus();
        const dot = inp.value.lastIndexOf('.');
        inp.setSelectionRange(0, dot > 0 ? dot : inp.value.length);
        const done = (v) => { close(); resolve(v); };
        el.querySelector('[data-a=no]').onclick = () => done(null);
        el.querySelector('[data-a=ok]').onclick = () => done(inp.value.trim() || null);
        inp.onkeydown = (ev) => { if (ev.key === 'Enter') done(inp.value.trim() || null); };
      });
    });
    const confirmIt = (title, text, okLabel) => new Promise((resolve) => {
      sheet(`<h3>${esc(title)}</h3><div class="sub">${esc(text)}</div>
        <div class="row"><button class="nf-btn" data-a="no">Cancel</button><button class="nf-btn danger" data-a="ok">${esc(okLabel)}</button></div>`, (el, close) => {
        el.querySelector('[data-a=no]').onclick = () => { close(); resolve(false); };
        el.querySelector('[data-a=ok]').onclick = () => { close(); resolve(true); };
      });
    });

    // after a change, the folder's library index is rebuilt
    function reindex() {
      const bucket = '/' + (path.split('/').filter(Boolean)[0] || '');
      if (bucket === '/') return;
      fetch('/api/reindex?path=' + encodeURIComponent(bucket), { method: 'POST', headers: headers() }).catch(() => {});
    }
    async function changed(msg) { await load(path); say(msg); reindex(); }

    function fileSheet(e) {
      const full = join(path, e.name);
      const h = VIDEO.test(e.name) ? Health.about(full, e.size) : null;
      const canOpen = MEDIA.test(e.name), canEdit = TEXT.test(e.name) && e.size <= 512 * 1024;
      sheet(`<h3>${esc(e.name)}</h3><div class="sub">${esc(path)} · ${size(e.size)}</div>
        ${h ? `<div class="note"><b>${esc(h.label)}.</b> ${esc(h.detail)}</div>` : ''}
        <div class="nf-actions">
          ${canOpen ? `<button class="nf-btn primary" data-a="open">Open</button>` : ''}
          <button class="nf-btn" data-a="dl">${svg('download', 16)}Download</button>
          ${canEdit ? `<button class="nf-btn" data-a="edit">${svg('pencil', 16)}Edit</button>` : ''}
          <button class="nf-btn" data-a="ren">Rename</button>
          <button class="nf-btn danger" data-a="del">Delete</button>
        </div>`, (el, close) => {
        el.addEventListener('click', async (ev) => {
          const a = ev.target.closest('[data-a]');
          if (!a) return;
          close();
          if (a.dataset.a === 'open') window.open('/media?file=' + encodeURIComponent(full), '_blank');
          else if (a.dataset.a === 'dl') { const l = document.createElement('a'); l.href = '/media?file=' + encodeURIComponent(full) + '&download=1'; l.download = e.name; document.body.appendChild(l); l.click(); l.remove(); }
          else if (a.dataset.a === 'edit') edit(full);
          else if (a.dataset.a === 'ren') rename(e);
          else if (a.dataset.a === 'del') remove(e);
        });
      });
    }
    function folderSheet(e) {
      sheet(`<h3>${esc(e.name)}</h3><div class="sub">${esc(join(path, e.name))}</div>
        <div class="nf-actions"><button class="nf-btn primary" data-a="open">Open</button>
        <button class="nf-btn" data-a="ren">Rename</button><button class="nf-btn danger" data-a="del">Delete</button></div>`, (el, close) => {
        el.addEventListener('click', (ev) => {
          const a = ev.target.closest('[data-a]');
          if (!a) return;
          close();
          if (a.dataset.a === 'open') load(join(path, e.name));
          else if (a.dataset.a === 'ren') rename(e);
          else remove(e);
        });
      });
    }
    async function rename(e) {
      const to = await ask(`Rename ${e.dir ? 'folder' : 'file'}`, e.name, 'Rename');
      if (!to || to === e.name) return;
      if (/[\/\\]/.test(to)) { say('A name cannot hold / or \\.'); return; }
      const r = await post('/rename', { oldname: join(path, e.name), newname: join(path, to) });
      if (r.ok) changed(`Renamed to ${to}.`); else say(`Not renamed: ${why(r)}`);
    }
    async function remove(e) {
      const ok = await confirmIt(`Delete ${e.name}?`, e.dir ? 'The folder and everything inside it is deleted from the card. This cannot be undone.' : 'It is deleted from the card. This cannot be undone.', 'Delete');
      if (!ok) return;
      say('Deleting…');
      const r = await post('/delete', { filename: join(path, e.name) });
      if (r.ok) changed(`Deleted ${e.name}.`);
      else say(r.status === 409 ? `Not deleted: ${e.name} is playing somewhere. Try again in a couple of minutes.` : `Not deleted: ${why(r)}`);
    }
    async function mkdir() {
      const name = await ask('New folder', '', 'Create');
      if (!name) return;
      if (/[\/\\]/.test(name)) { say('A name cannot hold / or \\.'); return; }
      const r = await post('/mkdir', { dirname: join(path, name) });
      if (r.ok) changed(`Made ${name}.`); else say(`Not made: ${why(r)}`);
    }
    async function edit(full) {
      let text = '';
      try {
        const r = await fetch('/media?file=' + encodeURIComponent(full) + '&_=' + Date.now(), { cache: 'no-store' });
        if (!r.ok) { say(`Couldn't open it (HTTP ${r.status}).`); return; }
        text = await r.text();
      } catch (e) { say("Couldn't reach Nomad."); return; }
      const ed = document.createElement('div');
      ed.className = 'nf-editor';
      ed.innerHTML = `<header><button class="nf-btn" data-a="close">Close</button><b>${esc(full)}</b><button class="nf-btn primary" data-a="save">Save</button></header><textarea spellcheck="false"></textarea>`;
      document.body.appendChild(ed);
      const ta = ed.querySelector('textarea');
      ta.value = text;
      ta.focus();
      ed.querySelector('[data-a=close]').onclick = async () => {
        if (ta.value !== text && !(await confirmIt('Close without saving?', 'Your changes are lost.', 'Close'))) return;
        ed.remove();
      };
      ed.querySelector('[data-a=save]').onclick = async () => {
        const r = await post('/save', { filename: full, content: ta.value });
        if (r.ok) { text = ta.value; ed.remove(); changed('Saved.'); } else alert(`Not saved: ${why(r)}`);
      };
    }

    crumbs.addEventListener('click', (ev) => { const b = ev.target.closest('button[data-path]'); if (b && b.dataset.path !== path) load(b.dataset.path); });
    list.addEventListener('click', (ev) => {
      const row = ev.target.closest('.nf-row');
      if (!row) return;
      const e = entries.find((x) => x.name === row.dataset.name);
      if (!e) return;
      if (e.dir) { if (ev.target.closest('.nf-chev')) folderSheet(e); else load(join(path, e.name)); }
      else fileSheet(e);
    });
    // a long press on a folder: its own actions
    let pressTimer = null;
    list.addEventListener('pointerdown', (ev) => {
      const row = ev.target.closest('.nf-row[data-dir="1"]');
      if (!row) return;
      pressTimer = setTimeout(() => { pressTimer = 'fired'; const e = entries.find((x) => x.name === row.dataset.name); if (e) folderSheet(e); }, 550);
    });
    ['pointerup', 'pointerleave', 'pointercancel'].forEach((t) => list.addEventListener(t, () => { if (pressTimer && pressTimer !== 'fired') clearTimeout(pressTimer); }));
    list.addEventListener('click', (ev) => { if (pressTimer === 'fired') { ev.stopImmediatePropagation(); pressTimer = null; } }, true);
    list.addEventListener('contextmenu', (ev) => { const row = ev.target.closest('.nf-row[data-dir="1"]'); if (row) ev.preventDefault(); });
    filter.addEventListener('input', draw);
    sort.addEventListener('change', draw);
    root.querySelector('.nf-head').addEventListener('click', (ev) => {
      const b = ev.target.closest('[data-act]');
      if (!b) return;
      if (b.dataset.act === 'upload') picker.click();
      else if (b.dataset.act === 'mkdir') mkdir();
      else load(path);
    });
    picker.addEventListener('change', async () => {
      const files = Array.from(picker.files || []);
      picker.value = '';
      if (!files.length) return;
      if (typeof NomadUpload === 'undefined') { say('The uploader did not load. Reload the page.'); return; }   // a top-level const, not on window
      const added = await NomadUpload.run(files, path);
      if (added) changed(`Uploaded ${added} file${added === 1 ? '' : 's'}.`);
    });

    Health.load().then(() => { if (entries.length) draw(); });
    load(path);
    return { load, get path() { return path; } };
  }

  window.NomadFiles = { mount };
})();
