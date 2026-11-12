// <!-- Version 4 -->
/* tiny shared helpers used across the frontend pages, dependency-free */

// one leading slash, no trailing slash, no double slashes, backslashes -> forward.
function normalizePath(p) {
  if (!p) return '/';
  p = String(p).trim().replace(/\\/g, '/').replace(/\/+/g, '/');
  if (!p.startsWith('/')) p = '/' + p;
  if (p.length > 1 && p.endsWith('/')) p = p.slice(0, -1);
  return p;
}

// in-place shuffle for the music shuffle-play mode
function fisherYates(a) {
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
}

/* keeps the screen on during video. tries the Wake Lock API, falls back to
   NoSleep.js if its loaded, no-ops if neither */
let wakeLock = null;
let noSleep = null;

async function requestWakeLock(){
  try {
    if('wakeLock' in navigator){
      if(!wakeLock){
        wakeLock = await navigator.wakeLock.request('screen');
        wakeLock.addEventListener && wakeLock.addEventListener('release', () => { wakeLock = null; });
      }
      return true;
    }

    if(window.NoSleep){
      if(!noSleep) noSleep = new NoSleep();
      try { noSleep.enable(); } catch(e){ console.warn('NoSleep.enable() failed', e); }
      return true;
    }

    console.warn('No Wake Lock API or NoSleep available, screen may still sleep.');
    return false;
  } catch(e){
    console.warn('requestWakeLock error', e);
    if(window.NoSleep){
      try { if(!noSleep) noSleep = new NoSleep(); noSleep.enable(); return true; } catch(e2){}
    }
    return false;
  }
}

async function releaseWakeLock(){
  try {
    if(wakeLock){
      try { await wakeLock.release(); } catch(e){}
      wakeLock = null;
    }
    if(noSleep){
      try { noSleep.disable(); } catch(e){}
    }
  } catch(e){
    console.warn('releaseWakeLock error', e);
  }
}

/* One directory listing. /listfiles is live but truncates a folder that overflows its fixed
   JSON buffer. The NDJSON index has no cap but is only as fresh as the last index pass. Take
   /listfiles when whole, else the index, else the partial list flagged truncated.
   Entries are { name, path, isDir, size }. */
const NomadList = {
  async _fetch503(url, tries = 3) {
    for (let i = 0; i < tries; i++) {
      const res = await fetch(url, { cache: 'no-store' });
      if (res.status !== 503) return res;          // a busy card 503s right after boot
      await new Promise(r => setTimeout(r, 700 * (i + 1)));
    }
    return fetch(url, { cache: 'no-store' });
  },

  /* encode a card path for a URL, per segment: encodeURI leaves # and ? alone,
     or a file named "Cake #2.md" gets cut off at the hash */
  encPath(p) { return String(p || '').split('/').map(encodeURIComponent).join('/'); },

  async _viaListFiles(dir) {
    const res = await this._fetch503('/listfiles?dir=' + encodeURIComponent(dir) + '&_=' + Date.now());
    if (!res.ok) return null;
    const arr = JSON.parse(await res.text());
    if (!Array.isArray(arr)) return null;
    return {
      truncated: arr.some(f => f && f.truncated),
      entries: arr.filter(f => f && f.name).map(f => {
        const name = String(f.name).replace(/\/$/, '');
        return { name, path: dir + '/' + name, isDir: !!f.isDir, size: +f.size || 0 };
      }).filter(f => f.name)
    };
  },

  async _viaIndex(dir) {
    const res = await this._fetch503('/api/index?path=' + encodeURIComponent(dir));
    // 202 is "no index yet, building one" and its body is a status object, not a listing
    if (!res.ok || res.status === 202) return null;
    const lines = (await res.text()).split('\n').filter(Boolean);
    const out = [];
    for (let i = 1; i < lines.length; i++) {       // line 0 is the index header
      try {
        const e = JSON.parse(lines[i]);
        // the index writes the size as "sz"
        if (e && e.n) out.push({ name: e.n, path: e.p, isDir: e.t === 'd', size: +e.sz || 0 });
      } catch (e) {}
    }
    return out.length ? out : null;
  },

  /* -> { entries, truncated, source }. entries is always an array, [] for an empty
     or unreadable folder, so a caller can render its empty state either way. */
  async dir(path) {
    let lf = null;
    try { lf = await this._viaListFiles(path); } catch (e) {}
    if (lf && !lf.truncated) return { entries: lf.entries, truncated: false, source: 'listfiles' };

    let idx = null;
    try { idx = await this._viaIndex(path); } catch (e) {}
    if (idx) return { entries: idx, truncated: false, source: 'index' };

    if (lf) return { entries: lf.entries, truncated: lf.truncated, source: 'listfiles' };
    return { entries: [], truncated: false, source: 'none' };
  }
};

/* One reader for the per-bucket NDJSON indexes. Retries a busy card (503), throws
   INDEX_BUILDING on 202 (its body is a status object, not a listing), and recovers a
   damaged line from its first brace.
   -> { header, entries } */
const NomadIndex = {
  /* No cache-buster: the device sends each index's signature as an ETag, so the browser
     revalidates and an unchanged index costs a 304 instead of the whole file again. Frames
     in one window (the music page's tabs) share a request that is already on its way. */
  _shared(){
    try { return window.top.__nomadIndexInflight || (window.top.__nomadIndexInflight = new Map()); }
    catch (e) { return this._own || (this._own = new Map()); }
  },
  _text(path){
    const url = '/api/index?path=' + encodeURIComponent(normalizePath(path));
    const shared = this._shared();
    if (shared.has(url)) return shared.get(url);
    const job = (async () => {
      let res = await fetch(url, { cache: 'no-cache' });
      for (let i = 0; i < 3 && res.status === 503; i++) {
        await new Promise(r => setTimeout(r, 800 * (i + 1)));
        res = await fetch(url, { cache: 'no-cache' });
      }
      if (res.status === 202) throw new Error('INDEX_BUILDING');
      if (!res.ok) throw new Error('index fetch failed ' + res.status);
      return res.text();
    })();
    shared.set(url, job);
    job.then(() => {}, () => {}).then(() => setTimeout(() => shared.delete(url), 2000));
    return job;
  },
  async fetch(path){
    const lines = (await this._text(path)).split(/\r?\n/).filter(Boolean);
    let header = null;
    if (lines.length) { try { header = JSON.parse(lines[0]); } catch (e) { header = null; } }
    // a damaged line (spliced or binary bytes in the middle of an index) is skipped.
    // one entry with no path breaks the whole Shows page
    const ok = (s) => typeof s === 'string' && s.length > 0 && !/[\u0000-\u001f�]/.test(s);
    const usable = (e) => e && typeof e === 'object' && (ok(e.p) || (e.p === undefined && ok(e.n)));
    const entries = [];
    let skipped = 0;
    for (let i = 1; i < lines.length; i++) {
      const line = lines[i];
      let e = null;
      try { e = JSON.parse(line); } catch (err) {
        const brace = line.indexOf('{');
        if (brace > 0) { try { e = JSON.parse(line.slice(brace)); } catch (err2) {} }
      }
      if (usable(e)) entries.push(e); else skipped++;
    }
    if (skipped) console.warn(`[NomadIndex] ${path}: skipped ${skipped} damaged line(s)`);
    return { header, entries };
  },
  isBuilding(err){ return !!(err && err.message === 'INDEX_BUILDING'); }
};

/* Subtitle support. Browsers only render WebVTT in <track> elements, so
   buildTracks() converts .srt to VTT client-side and passes .vtt through. */
const NomadSubs = {
  _blobCache: new Map(),

  srtToVtt(text){
    let s = String(text).replace(/^﻿/, '').replace(/\r/g, '');
    s = s.replace(/(\d{2}:\d{2}:\d{2}),(\d{3})/g, '$1.$2');
    return 'WEBVTT\n\n' + s.trim() + '\n';
  },

  /* "Movie.en.srt" beside "Movie.mp4" -> "EN", plain "Movie.srt" -> "Subtitles" */
  labelFor(subPath, videoPath, i){
    const base = (p) => String(p).split('/').pop().replace(/\.[^.]+$/, '');
    const vb = videoPath ? base(videoPath) : '';
    let rest = base(subPath);
    if (vb && rest.toLowerCase().startsWith(vb.toLowerCase())) rest = rest.slice(vb.length);
    rest = rest.replace(/^[ ._-]+/, '');
    if (rest && rest.length <= 12) return rest.toUpperCase();
    return i === 0 ? 'Subtitles' : 'Subtitles ' + (i + 1);
  },

  /* A film starting up is when these are asked for, and it's when Nomad says 503 ("retry
     shortly"). Waits of 0.5, 1 and 2 s: the film is held back while this runs. An answer
     that isn't the file must throw, or its error text becomes a blank track for the visit. */
  async _fetchSub(url){
    for (let i = 0; ; i++){
      const r = await fetch(url);
      if (r.ok) return r.arrayBuffer();
      if (r.status !== 503 || i >= 3) throw new Error('subtitle: ' + r.status);
      await new Promise(res => setTimeout(res, 500 << i));
    }
  },

  /* CC stays on from one film to the next, but a new track at the same place in the list looks
     like no change to Plyr: it keeps pointing at the old film's track and never asks the browser
     for the new one's cues. Hand it the new track once the film has loaded. */
  keepOn(player, media){
    media.addEventListener('loadedmetadata', () => {
      const c = player.captions;
      if (!c || !c.active) return;
      const list = Array.from(media.textTracks).filter(t => t.kind === 'captions' || t.kind === 'subtitles');
      const track = list[Math.max(0, c.currentTrack)];
      if (track) { c.currentTrackNode = track; player.toggleCaptions(true); }
    }, { once: true });
  },

  /* subs: [{path, url}] -> Plyr-style track list ({kind,label,srclang,src}) */
  async buildTracks(subs, videoPath){
    const out = [];
    for (const s of (subs || [])){
      const ext = (String(s.path).match(/\.([a-z0-9]+)$/i) || [,''])[1].toLowerCase();
      let src = null;
      if (ext === 'vtt'){
        src = s.url;
      } else if (ext === 'srt'){
        try {
          if (this._blobCache.has(s.path)){
            src = this._blobCache.get(s.path);
          } else {
            const buf = await this._fetchSub(s.url);
            let text;
            try { text = new TextDecoder('utf-8', { fatal: true }).decode(buf); }
            catch(e){ text = new TextDecoder('windows-1252').decode(buf); } // legacy-encoded srt
            src = URL.createObjectURL(new Blob([this.srtToVtt(text)], { type: 'text/vtt' }));
            this._blobCache.set(s.path, src);
          }
        } catch(e){ continue; }
      } else {
        continue;
      }
      const label = this.labelFor(s.path, videoPath, out.length);
      out.push({ kind: 'captions', label,
        srclang: label.length === 2 ? label.toLowerCase() : 'en',
        src });
    }
    return out;
  }
};

/* UI configuration from /.system-ui.json (admin-controlled, stored on the
   card). A missing or invalid file means everything is enabled. */
const NomadUI = {
  _cfg: null,
  async config(){
    if (this._cfg) return this._cfg;
    // firmware's RAM copy first (always current), the file on the card as a
    // fallback for older firmware
    try {
      const r = await fetch('/api/ui-config?_=' + Date.now(), { cache: 'no-store' });
      if (r.ok) {
        try { this._cfg = JSON.parse(await r.text()) || {}; return this._cfg; }
        catch(e){ /* captive portal HTML from old firmware, use the file */ }
      }
    } catch(e){}
    try {
      const r = await fetch('/.system-ui.json?_=' + Date.now(), { cache: 'no-store' });
      this._cfg = r.ok ? (JSON.parse(await r.text()) || {}) : {};
    } catch(e){ this._cfg = {}; }
    return this._cfg;
  },
  /* Downloads off is <body class="nomad-no-dl"> plus a rule hiding anything marked data-dl.
     Inline display:none loses to page CSS with !important, and the class also covers rows
     rendered later. The old id list still works. */
  /* Downloads and uploads go off for everything (downloadsDisabled, uploadsDisabled) or by
     page (dlOff, ulOff: page keys). The firmware enforces both, these say what a page offers.
     A page's key is its folder name in lower case, the book readers count as books. */
  pageKey(){
    const p = (location.pathname.split('/').pop() || '').replace(/\.html?$/i, '').toLowerCase();
    return ({ epub: 'books', pdf: 'books', comics: 'books', songs: 'music', playlist: 'music', queue: 'music' })[p] || p || 'menu';
  },
  canDownload(cfg, key){ return !cfg.downloadsDisabled && !(cfg.dlOff || []).includes(key || this.pageKey()); },
  canUpload(cfg, key){ return !cfg.uploadsDisabled && !(cfg.ulOff || []).includes(key || this.pageKey()); },
  async applyDownloadPolicy(...els){
    const cfg = await this.config();
    const off = !this.canDownload(cfg);
    const paint = () => {
      if (!document.body) return;
      document.body.classList.toggle('nomad-no-dl', off);
      if (!document.getElementById('nomad-dl-style')) {
        const st = document.createElement('style');
        st.id = 'nomad-dl-style';
        st.textContent = 'body.nomad-no-dl [data-dl]{display:none !important}';
        document.head.appendChild(st);
      }
      els.forEach(sel => {
        const el = typeof sel === 'string' ? document.getElementById(sel) : sel;
        if (el) el.setAttribute('data-dl', '');
      });
    };
    if (document.body) paint();
    else document.addEventListener('DOMContentLoaded', paint);
    return off;
  },
  // kept: pages already call this name
  async hideDownloads(...els){ return this.applyDownloadPolicy(...els); },
  // for code that builds a link and needs to know before rendering
  async downloadsOff(key){ return !this.canDownload(await this.config(), key); },
  async uploadsOff(key){ return !this.canUpload(await this.config(), key); }
};

/* Small per-browser preferences. One key convention for everything new:
   nomad:<area>:<name>:v1, the shape the shared resume store nomad:watch:v1
   already uses. JSON in, JSON out, never throws (private mode, full quota). */
const NomadPrefs = {
  get(key, fallback){
    try { const v = localStorage.getItem(key); return v == null ? fallback : JSON.parse(v); }
    catch(e){ return fallback; }
  },
  set(key, value){
    try { localStorage.setItem(key, JSON.stringify(value)); return true; } catch(e){ return false; }
  },
  remove(key){ try { localStorage.removeItem(key); } catch(e){} }
};

/* Progress for every kind of thing (films, episodes, audiobooks, EPUBs, PDFs, comics), so the
   Books shelf, episode lists and the menu's Continue row can show it without knowing each
   page's own format. Each player still keeps its exact place itself. Per phone.
     path -> { k: kind, p: 0..1 (or null when only a page is known), at: last use (ms),
               t: title, g: group (a show's folder), l: short label, done: finished }
   Starting an episode drops the half-watched hold of the others in its group, so Continue
   offers the episode you're on. */
const NomadProgress = {
  KEY: 'nomad:progress:v1',
  MAX: 400,
  _cache: null,
  _all(){
    if(this._cache) return this._cache;
    const v = NomadPrefs.get(this.KEY, null);
    this._cache = (v && v.items) ? v : this._migrate();
    return this._cache;
  },
  _save(v){
    const keys = Object.keys(v.items);
    if(keys.length > this.MAX){
      keys.sort((a, b) => (v.items[a].at || 0) - (v.items[b].at || 0));
      for(const k of keys.slice(0, keys.length - this.MAX)) delete v.items[k];
    }
    NomadPrefs.set(this.KEY, v);
  },
  // another tab may have written since: read fresh before changing anything
  _fresh(){ this._cache = null; return this._all(); },

  get(path){ return this._all().items[normalizePath(path)] || null; },
  note(path, info){
    path = normalizePath(path);
    const v = this._fresh();
    const e = Object.assign({}, v.items[path] || {}, info, { at: Date.now() });
    if(e.p != null) e.p = Math.max(0, Math.min(1, +e.p || 0));
    if(info && info.done === undefined && e.done && e.p != null && e.p < 0.9) e.done = false;   // watching it again
    if(e.g){
      for(const [k, o] of Object.entries(v.items))
        if(k !== path && o.g === e.g && !o.done) delete v.items[k];
    }
    v.items[path] = e;
    this._save(v);
    return e;
  },
  finish(path, info){ return this.note(path, Object.assign({}, info, { p: 1, done: true, l: '' })); },
  clear(path){ const v = this._fresh(); delete v.items[normalizePath(path)]; this._save(v); },
  // newest first, by default only what is started and not finished
  recent(opts){
    const o = opts || {}, out = [];
    for(const [path, e] of Object.entries(this._all().items)){
      if(!o.all && e.done) continue;
      if(o.kinds && !o.kinds.includes(e.k)) continue;
      out.push(Object.assign({ path }, e));
    }
    out.sort((a, b) => (b.at || 0) - (a.at || 0));
    return o.limit ? out.slice(0, o.limit) : out;
  },
  // a film or episode stopped in the last minute, or in the last 5% (the credits), is finished
  isFinished(pos, dur){ return dur > 0 && dur - pos <= Math.max(60, dur * 0.05); },
  timeLeft(sec){
    sec = Math.max(0, Math.round(sec));
    const h = Math.floor(sec / 3600), m = Math.round((sec % 3600) / 60);
    if(h) return `${h} h ${m} min left`;
    return m ? `${m} min left` : 'Almost done';
  },
  // the stores the pages kept before this one, read once
  _migrate(){
    const v = { v: 1, items: {} };
    const put = (path, e) => { if(path) v.items[normalizePath(path)] = e; };
    const showDir = (p) => { const s = normalizePath(p).split('/').filter(Boolean); return s[0] === 'Shows' && s.length > 2 ? '/Shows/' + s[1] : null; };
    try {
      const w = JSON.parse(localStorage.getItem('nomad:watch:v1') || '{}');
      for(const [key, e] of Object.entries(w)){
        const path = key.split('|')[0];
        if(!path || !e) continue;
        const film = normalizePath(path).startsWith('/Movies/');
        const g = film ? null : showDir(path);
        const p = e.duration > 0 ? e.position / e.duration : null;
        const done = !!e.completed || (e.duration > 0 && this.isFinished(e.position || 0, e.duration));
        // only the newest of each show keeps its hold, as note() does
        if(g && !done){
          const other = Object.entries(v.items).find(([, o]) => o.g === g && !o.done);
          if(other && (other[1].at || 0) > (e.updated || 0)) continue;
          if(other) delete v.items[other[0]];
        }
        put(path, { k: film ? 'film' : 'episode', p: done ? 1 : p, at: e.updated || 0, g, done,
                    l: done || !(e.duration > 0) ? '' : this.timeLeft(e.duration - (e.position || 0)) });
      }
    } catch(e){}
    try {
      const a = JSON.parse(localStorage.getItem('nomad_resume_store_v1') || '{}');
      for(const [key, e] of Object.entries(a)){
        const path = key.split('|')[0];
        if(!path || !e || !(e.duration > 0)) continue;
        const done = e.duration - (e.time || 0) < 30;
        put(path, { k: 'audio', p: done ? 1 : (e.time || 0) / e.duration, at: e.timestamp || 0, done,
                    l: done ? '' : this.timeLeft(e.duration - (e.time || 0)) });
      }
    } catch(e){}
    try {
      for(let i = 0; i < localStorage.length; i++){
        const k = localStorage.key(i);
        let m;
        if((m = /^nomad_epub_progress_(.+)$/.exec(k))){
          const e = JSON.parse(localStorage.getItem(k) || 'null');
          if(e) put(decodeURIComponent(m[1]), { k: 'epub', p: (+e.percent || 0) / 100, at: e.time || 0, done: +e.percent >= 100 });
        } else if((m = /^nomad_pdf_progress_(.+)$/.exec(k))){
          const e = JSON.parse(localStorage.getItem(k) || 'null');
          if(e && e.page > 1) put(decodeURIComponent(m[1]), { k: 'pdf', p: null, at: e.time || 0, l: 'Page ' + e.page });
        } else if((m = /^comic_page_(.+)$/.exec(k))){
          const page = parseInt(localStorage.getItem(k) || '0', 10);
          if(page > 0) put(m[1], { k: 'comic', p: null, at: 0, l: 'Page ' + (page + 1) });
        }
      }
    } catch(e){}
    this._save(v);
    return v;
  }
};

/* The phone's Back gesture closes what is open (a viewer, a sheet, a popup) instead of
   leaving the page. open() adds a history entry for it and Back runs its close. When it is
   closed some other way (its x, Escape), done() takes that entry back off. A close that
   runs from Back can call done() too: by then it's no longer open, so nothing happens. */
const NomadBack = {
  _stack: [],
  _skip: 0,
  open(name, close){
    if(this.isOpen(name)) return;
    this._stack.push({ name, close });
    try { history.pushState({ nomadBack: name }, ''); } catch(e){}
    if(!this._on){
      this._on = true;
      window.addEventListener('popstate', () => {
        if(this._skip > 0){ this._skip--; return; }
        const top = this._stack.pop();
        if(top) try { top.close(); } catch(e){ console.warn('[NomadBack]', e); }
      });
    }
  },
  done(name){
    const i = this._stack.map(s => s.name).lastIndexOf(name);
    if(i < 0) return;
    const n = this._stack.length - i;
    this._stack.splice(i);
    this._skip++;   // one popstate for the whole jump
    try { history.go(-n); } catch(e){ this._skip--; }
  },
  isOpen(name){ return this._stack.some(s => s.name === name); }
};

/* Plyr only injects its SVG sprite when iconUrl is another origin. Served from the card it
   writes <use href="/assets/plyr.svg#...">, which browsers don't resolve, so every control
   comes out blank. The sprite is fetched and injected once and Plyr gets an empty iconUrl.

   const player = await NomadPlyr.create(videoEl, { controls: [...] })
   create() builds every video player (movies, shows, gallery, ZIM video): controls, wake
   lock, landscape lock, double-tap seek, projector correction. opts()/ready() remain for
   code that builds Plyr by hand. */
const NomadPlyr = {
  _p: null,
  ready(){
    if (this._p) return this._p;
    this._p = (async () => {
      if (document.getElementById('sprite-plyr')) return;
      const r = await fetch('/assets/plyr.svg', { cache: 'force-cache' });
      if (!r.ok) throw new Error('sprite HTTP ' + r.status);
      const svg = await r.text();
      const box = document.createElement('div');
      box.id = 'sprite-plyr';
      box.setAttribute('hidden', '');
      box.innerHTML = svg;
      document.body.insertBefore(box, document.body.firstChild);
    })().catch(e => { console.warn('[Plyr] sprite load failed', e); });
    return this._p;
  },
  opts(extra){
    // blankVideo: '' matters offline. Plyr's default points at cdn.plyr.io and is
    // loaded whenever a source is cancelled.
    return Object.assign({ iconUrl: '', loadSprite: false, blankVideo: '' }, extra || {});
  },

  // the library players' control set. gallery passes a shorter list.
  CONTROLS: ['play-large','play','progress','current-time','duration','mute','volume',
             'captions','settings','pip','airplay','fullscreen'],

  /* extra: any Plyr option, plus wakeLock:false / doubleTapSeek:false / projector:false
     to leave one of the shared behaviours out. Resolves with the Plyr instance. */
  async create(media, extra){
    await this.ready();
    if (!window.Plyr) throw new Error('Plyr not loaded');
    const o = Object.assign({
      controls: this.CONTROLS.slice(),
      settings: ['captions', 'speed'],
      fullscreen: { enabled: true, fallback: true, iosNative: false },
      clickToPlay: false,
      hideControls: true,
      resetOnEnd: false,
      keyboard: { focused: true, global: true },
      tooltips: { controls: true, seek: true }
    }, extra || {});
    const wake = o.wakeLock !== false, tap = o.doubleTapSeek !== false, proj = o.projector !== false;
    delete o.wakeLock; delete o.doubleTapSeek; delete o.projector;

    const p = new Plyr(media, this.opts(o));

    if (wake) {
      p.on('play', () => requestWakeLock().catch(()=>{}));
      p.on('pause', () => releaseWakeLock().catch(()=>{}));
      p.on('ended', () => releaseWakeLock().catch(()=>{}));
    }
    // phones: hold landscape while fullscreen. a no-op where unsupported
    p.on('enterfullscreen', () => {
      try { if (screen.orientation && screen.orientation.lock) screen.orientation.lock('landscape').catch(()=>{}); } catch(e){}
    });
    p.on('exitfullscreen', () => {
      try { if (screen.orientation && screen.orientation.unlock) screen.orientation.unlock(); } catch(e){}
    });
    if (tap) this._doubleTap(p);
    if (proj) NomadProjector.attach(p);
    return p;
  },

  /* Seek previews: the picture under your finger while you drag the progress bar. Nomad-Tools'
     "Subtitles and seek previews" job saves Film.thumbs.vtt and its sheets beside the film;
     Plyr reads the .vtt and loads a sheet of 50 frames as you scrub. Films without them just
     have a plain bar (one small 404). Call it each time a new video is put in the player. */
  async previews(player, videoPath){
    if (!player || !player.setPreviewThumbnails || !videoPath) return false;
    const vtt = encodeURI(String(videoPath).replace(/\.[^./]+$/, '') + '.thumbs.vtt');
    let ok = false;
    try { const r = await fetch(vtt, { cache: 'no-cache' }); ok = r.ok && /^WEBVTT/.test(await r.text()); } catch (e) {}
    try { player.setPreviewThumbnails(ok ? { enabled: true, src: vtt } : { enabled: false, src: '' }); } catch (e) { ok = false; }
    return ok;
  },

  /* double-tap the left or right third of the picture to skip 10s. Screen-space, so it
     is unaffected by a flipped or keystoned surface. */
  _doubleTap(p){
    const wrap = p.elements && p.elements.wrapper;
    if (!wrap) return;
    let last = 0;
    wrap.addEventListener('touchend', (e) => {
      const now = Date.now();
      if (now - last > 0 && now - last < 300) {
        last = 0;
        const rect = wrap.getBoundingClientRect();
        const x = e.changedTouches[0].clientX - rect.left;
        if (x < rect.width / 3) { e.preventDefault(); p.rewind(10); }
        else if (x > rect.width * 2 / 3) { e.preventDefault(); p.forward(10); }
      } else {
        last = now;
      }
    }, { passive: false });
  }
};

/* Projector correction: flip (rear projection, ceiling mount) and keystone, as a CSS
   transform in the browser only.

   DOM: .plyr > .nomad-stage > .nomad-surface > (video wrapper, captions). The surface is
   transformed, so captions flip with the picture. The stage clips the overflow. Controls
   and panels are siblings of the stage and never move.

   The keystone is a homography from the unit square to the corrected quad, as matrix3d. It
   is recomputed from the stage's pixel size, because the perspective terms scale with it.

   Stored per browser (nomad:player:projector:v1). Neutral = nothing stored, no transform.
   Any correction also turns on the room captions (.nomad-proj-active in master.css), sized
   through --nomad-cap. */
const NomadProjector = {
  KEY: 'nomad:player:projector:v1',
  MAX: 50,   // slider travel, percent of the edge that is pulled in at full deflection
  CAPTION: 0.05,   // room captions: type size as a share of the picture's height

  // every visible string of the projector panel
  TEXT: {
    button: 'Projector',
    flipH:  'Flip horizontal',
    flipV:  'Flip vertical',
    keyV:   'Vertical keystone',
    keyH:   'Horizontal keystone',
    reset:  'Reset'
  },
  // a trapezoid on a base line. geometric glyph in the style of Plyr's own icons
  ICON: '<svg role="presentation" focusable="false" viewBox="0 0 18 18" width="18" height="18"><path d="M4 3h10l2.5 9h-15L4 3zm1.2 1.5L3.6 10.5h10.8l-1.6-6H5.2zM3 14h12v1.6H3V14z" fill="currentColor"/></svg>',

  _state: null,
  _apply: [],   // one re-apply function per live player

  _sane(s){
    s = s || {};
    const clamp = v => { v = Math.round(Number(v) || 0); return Math.max(-this.MAX, Math.min(this.MAX, v)); };
    return { fh: !!s.fh, fv: !!s.fv, kv: clamp(s.kv), kh: clamp(s.kh) };
  },
  state(){
    if (!this._state) this._state = this._sane(NomadPrefs.get(this.KEY, null));
    return this._state;
  },
  isNeutral(s){ return !s.fh && !s.fv && !s.kv && !s.kh; },
  save(s){
    this._state = this._sane(s);
    if (this.isNeutral(this._state)) NomadPrefs.remove(this.KEY);
    else NomadPrefs.set(this.KEY, this._state);
    this._apply.forEach(fn => { try { fn(); } catch(e){} });
  },
  set(patch){ this.save(Object.assign({}, this.state(), patch)); },
  reset(){ this.save({}); },

  /* matrix3d mapping the untransformed stage (W x H px, origin top-left) onto the
     corrected quad. kv>0 pulls the top edge in, kv<0 the bottom. kh>0 the left edge,
     kh<0 the right. Flips mirror the result. */
  matrix(s, W, H){
    const fv = s.kv / 100, fh = s.kh / 100;
    const tl = [0,0], tr = [1,0], br = [1,1], bl = [0,1];
    if (fv > 0) { tl[0] += fv/2; tr[0] -= fv/2; } else if (fv < 0) { bl[0] -= fv/2; br[0] += fv/2; }
    if (fh > 0) { tl[1] += fh/2; bl[1] -= fh/2; } else if (fh < 0) { tr[1] -= fh/2; br[1] += fh/2; }
    if (s.fh) for (const c of [tl,tr,br,bl]) c[0] = 1 - c[0];
    if (s.fv) for (const c of [tl,tr,br,bl]) c[1] = 1 - c[1];

    // unit square -> quad (Heckbert): x' = (a u + b v + c)/(g u + h v + 1)
    const [x0,y0] = tl, [x1,y1] = tr, [x2,y2] = br, [x3,y3] = bl;
    const sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    let a, b, c, d, e, f, g, h;
    if (Math.abs(sx) < 1e-9 && Math.abs(sy) < 1e-9) {
      a = x1 - x0; b = x3 - x0; c = x0; d = y1 - y0; e = y3 - y0; f = y0; g = 0; h = 0;
    } else {
      const dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
      const det = dx1 * dy2 - dx2 * dy1;
      g = (sx * dy2 - dx2 * sy) / det;
      h = (dx1 * sy - sx * dy1) / det;
      a = x1 - x0 + g * x1; b = x3 - x0 + h * x3; c = x0;
      d = y1 - y0 + g * y1; e = y3 - y0 + h * y3; f = y0;
    }
    // same map in pixels: conjugate by the stage size
    const m = [a, d * H / W, 0, g / W,
               b * W / H, e, 0, h / H,
               0, 0, 1, 0,
               c * W, f * H, 0, 1];
    return 'matrix3d(' + m.map(v => (Math.abs(v) < 1e-9 ? 0 : +v.toFixed(7))).join(',') + ')';
  },

  attach(p){
    const el = p && p.elements;
    if (!el || !el.container || !el.wrapper) return;
    const container = el.container;
    if (container.querySelector('.nomad-stage')) return;

    const stage = document.createElement('div');
    stage.className = 'nomad-stage';
    const surface = document.createElement('div');
    surface.className = 'nomad-surface';
    container.insertBefore(stage, el.wrapper);
    stage.appendChild(surface);
    surface.appendChild(el.wrapper);
    if (el.captions) surface.appendChild(el.captions);

    const T = this.TEXT;
    const id = 'nomad-proj-' + Math.random().toString(36).slice(2, 8);
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'plyr__controls__item plyr__control nomad-proj-btn';
    btn.setAttribute('aria-label', T.button);
    btn.setAttribute('aria-expanded', 'false');
    btn.setAttribute('aria-controls', id);
    btn.innerHTML = this.ICON;

    const panel = document.createElement('div');
    panel.className = 'nomad-proj';
    panel.id = id;
    panel.hidden = true;
    panel.innerHTML =
      '<div class="nomad-proj-row">' +
        '<button type="button" class="nomad-proj-toggle" data-k="fh" aria-pressed="false"></button>' +
        '<button type="button" class="nomad-proj-toggle" data-k="fv" aria-pressed="false"></button>' +
      '</div>' +
      '<label class="nomad-proj-slider"><span></span><input type="range" min="-' + this.MAX + '" max="' + this.MAX + '" step="1" data-k="kv"></label>' +
      '<label class="nomad-proj-slider"><span></span><input type="range" min="-' + this.MAX + '" max="' + this.MAX + '" step="1" data-k="kh"></label>' +
      '<button type="button" class="nomad-proj-reset"></button>';
    const q = sel => panel.querySelector(sel);
    q('[data-k="fh"]').textContent = T.flipH;
    q('[data-k="fv"]').textContent = T.flipV;
    q('label:nth-of-type(1) span').textContent = T.keyV;
    q('label:nth-of-type(2) span').textContent = T.keyH;
    q('.nomad-proj-reset').textContent = T.reset;

    const controls = el.controls;
    if (controls) {
      const fs = el.buttons && el.buttons.fullscreen;
      controls.insertBefore(btn, (fs && fs.parentNode === controls) ? fs : null);
    }
    container.appendChild(panel);

    const openPanel = (open) => {
      panel.hidden = !open;
      btn.setAttribute('aria-expanded', open ? 'true' : 'false');
      // a portrait phone player is shorter than the panel, so lift the container's clip
      // while it is open, the way Plyr does for its own settings menu. The stage keeps
      // clipping the transformed picture regardless.
      container.classList.toggle('nomad-proj-open', open);
      if (open) { try { p.toggleControls(true); } catch(e){} }
    };
    btn.addEventListener('click', (e) => { e.stopPropagation(); openPanel(panel.hidden); });
    panel.addEventListener('click', e => e.stopPropagation());
    panel.addEventListener('input', (e) => {
      const k = e.target && e.target.dataset && e.target.dataset.k;
      if (k === 'kv' || k === 'kh') this.set({ [k]: +e.target.value });
      try { p.toggleControls(true); } catch(err){}
    });
    q('[data-k="fh"]').addEventListener('click', () => this.set({ fh: !this.state().fh }));
    q('[data-k="fv"]').addEventListener('click', () => this.set({ fv: !this.state().fv }));
    q('.nomad-proj-reset').addEventListener('click', () => this.reset());
    p.on('controlshidden', () => openPanel(false));
    container.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !panel.hidden) openPanel(false); });

    const apply = () => {
      const s = this.state();
      const W = stage.clientWidth, H = stage.clientHeight;
      const on = !this.isNeutral(s);
      surface.style.transform = (on && W && H) ? this.matrix(s, W, H) : '';
      btn.classList.toggle('nomad-proj-on', on);
      container.classList.toggle('nomad-proj-active', on);
      if (H) container.style.setProperty('--nomad-cap', Math.max(16, Math.round(H * this.CAPTION)) + 'px');
      q('[data-k="fh"]').setAttribute('aria-pressed', s.fh ? 'true' : 'false');
      q('[data-k="fv"]').setAttribute('aria-pressed', s.fv ? 'true' : 'false');
      if (document.activeElement !== q('[data-k="kv"]')) q('[data-k="kv"]').value = s.kv;
      if (document.activeElement !== q('[data-k="kh"]')) q('[data-k="kh"]').value = s.kh;
    };
    // the matrix depends on the stage size: fullscreen, rotation and resizes re-apply it
    if (window.ResizeObserver) new ResizeObserver(() => apply()).observe(stage);
    else window.addEventListener('resize', apply);
    p.on('enterfullscreen', () => setTimeout(apply, 60));
    p.on('exitfullscreen', () => setTimeout(apply, 60));
    p.on('ready', apply);
    this._apply.push(apply);
    apply();
  }
};

/* Cover images for the library grids. With loading="lazy" Chrome opens dozens of cover
   requests at once on a big library. AsyncTCP has about sixteen sockets, so covers get
   refused or come late and fight whoever is streaming. Here an image loads only
   near the viewport, at most LIMIT in flight (without IntersectionObserver, just the queue). */
const NomadImages = {
  LIMIT: 3,
  // A cover the device turns away (503, "retry shortly") is tried again after 1, 2, 4
  // and 8 s. The retry gets its own
  // query parameter because some browsers will not re-request an identical src.
  RETRIES: 4,
  _q: [], _active: 0, _io: null,

  observe(img, src){
    if (!img || !src) return;
    img.dataset.src = src;
    if (!this._io && window.IntersectionObserver) {
      this._io = new IntersectionObserver(entries => {
        for (const e of entries) {
          if (!e.isIntersecting) continue;
          this._io.unobserve(e.target);
          this._enqueue(e.target);
        }
      }, { rootMargin: '400px 0px' });
    }
    if (this._io) this._io.observe(img); else this._enqueue(img);
  },

  _enqueue(img){
    if (!img.dataset.src || img.dataset.queued) return;
    img.dataset.queued = '1';
    this._q.push(img);
    this._pump();
  },

  _pump(){
    while (this._active < this.LIMIT && this._q.length) {
      const img = this._q.shift();
      const src = img.dataset.src;
      if (!src || !img.isConnected) { delete img.dataset.queued; continue; }
      this._active++;
      const free = () => { this._active = Math.max(0, this._active - 1); this._pump(); };
      const onLoad = () => { img.removeEventListener('error', onError); free(); };
      const onError = () => {
        img.removeEventListener('load', onLoad);
        free();
        const tries = +(img.dataset.tries || 0);
        if (tries >= this.RETRIES || !img.isConnected) return;
        img.dataset.tries = tries + 1;
        setTimeout(() => {
          const base = src.replace(/[?&]r=\d+$/, '');
          img.dataset.src = base + (base.includes('?') ? '&' : '?') + 'r=' + (tries + 1);
          this._enqueue(img);
        }, 1000 << tries);
      };
      img.addEventListener('load', onLoad, { once: true });
      img.addEventListener('error', onError, { once: true });
      delete img.dataset.src;
      delete img.dataset.queued;
      img.src = src;
    }
  }
};

/* Shared upload runner with a full-screen progress modal. One file at a time, each sent
   in 4 MB slices into a part file on the device that is renamed into place once complete,
   so a dropped connection, a phone that slept or a reload carries on from where it
   stopped instead of starting over (/upload-status in the firmware). A file already on
   the card is skipped, a different file with the same name becomes "name (2)", and one
   file failing doesn't stop the rest. */
const NomadUpload = {
  SLICE: 4 * 1024 * 1024,
  RETRIES: 12,                 // waits of 1, 2, 4, 8 then 15 s: about 2.5 minutes with no progress
  _built: false,
  _xhr: null,
  _stop: false,

  _ensure(){
    if (this._built) return;
    this._built = true;
    document.body.insertAdjacentHTML('beforeend', `
<style>
  .nu-overlay{position:fixed;inset:0;background:rgba(0,0,0,.6);z-index:7000;display:none;
    align-items:center;justify-content:center;padding:20px}
  .nu-overlay.open{display:flex}
  .nu-card{background:var(--card,#fff);color:var(--text,#111);border:1px solid var(--line,#ddd);
    border-radius:12px;max-width:420px;width:100%;padding:20px;text-align:center;
    box-shadow:0 12px 40px rgba(0,0,0,.35)}
  .nu-title{font-weight:700;font-size:1.05rem;margin-bottom:.35rem}
  .nu-file{font-size:.9rem;color:var(--muted,#777);margin-bottom:.75rem;overflow:hidden;
    text-overflow:ellipsis;white-space:nowrap}
  .nu-bar-wrap{background:var(--bg,#eee);border:1px solid var(--line,#ddd);border-radius:999px;
    height:14px;overflow:hidden;margin-bottom:.5rem}
  .nu-bar{height:100%;width:0%;background:var(--primary,#0b62d6);border-radius:999px;
    transition:width .15s linear}
  .nu-count{font-size:.8rem;color:var(--muted,#777)}
  .nu-note{font-size:.75rem;color:var(--muted,#777);margin-top:.6rem}
  .nu-error{color:#c0392b;font-size:.85rem;margin-top:.5rem;word-break:break-word;text-align:left;
    max-height:9em;overflow:auto}
  .nu-actions{display:flex;gap:8px;justify-content:center;margin-top:.8rem}
  .nu-actions button{display:none;border-radius:8px;padding:9px 20px;cursor:pointer;
    border:1px solid var(--primary,#0b62d6)}
  .nu-actions .nu-main{background:var(--primary,#0b62d6);color:var(--color-white,#fff)}
  .nu-actions .nu-quiet{background:transparent;color:var(--primary,#0b62d6)}
</style>
<div class="nu-overlay" id="nuOverlay">
  <div class="nu-card">
    <div class="nu-title" id="nuTitle">Uploading</div>
    <div class="nu-file" id="nuFile"></div>
    <div class="nu-bar-wrap"><div class="nu-bar" id="nuBar"></div></div>
    <div class="nu-count" id="nuCount"></div>
    <div class="nu-note" id="nuNote"></div>
    <div class="nu-error" id="nuError"></div>
    <div class="nu-actions">
      <button class="nu-quiet" id="nuStop">Stop</button>
      <button class="nu-main" id="nuRetry">Try again</button>
      <button class="nu-quiet" id="nuClose">Close</button>
    </div>
  </div>
</div>`);
    document.getElementById('nuStop').addEventListener('click', () => {
      this._stop = true;
      if (this._xhr) this._xhr.abort();
    });
  },

  _el(id){ return document.getElementById(id); },

  _headers(){
    // an admin logged in on this tab may upload what visitors cannot (pages, scripts)
    try {
      const t = sessionStorage.getItem('nomad_admin_token') || localStorage.getItem('nomad_admin_token');   // kept sign-ins live in localStorage
      return t ? { 'X-Admin-Token': t } : {};
    } catch (e) { return {}; }
  },

  _size(n){
    return n >= 1e9 ? (n / 1e9).toFixed(1) + ' GB' : n >= 1e6 ? (n / 1e6).toFixed(1) + ' MB' : Math.ceil(n / 1e3) + ' KB';
  },

  _numbered(name, n){
    const dot = name.lastIndexOf('.');
    return dot > 0 ? name.slice(0, dot) + ' (' + n + ')' + name.slice(dot) : name + ' (' + n + ')';
  },

  _wait(ms){ return new Promise(r => setTimeout(r, ms)); },

  _fail(msg, how){ const e = new Error(msg); e.how = how; return e; },

  // what the device has under this name: {size, part, free}, -1 for none or unknown.
  // null: it can't say (no answer, or firmware from before resumable uploads).
  async _status(path, total){
    try {
      const r = await fetch('/upload-status?path=' + encodeURIComponent(path) + '&total=' + total,
                            { cache: 'no-store', credentials: 'same-origin', headers: this._headers() });
      if (r.status === 404) return { legacy: true };
      if (r.status === 401 || r.status === 403) {
        let j = {}; try { j = await r.json(); } catch (e) {}
        throw this._fail(r.status === 401 ? 'That folder needs the admin password.' : (j.error || 'Uploads are turned off on Nomad.'), 'all');
      }
      if (!r.ok) return null;
      return await r.json();
    } catch (e) {
      if (e.how) throw e;
      return null;
    }
  },

  // one POST: the bytes [offset, end) of file, or the whole file when total is 0
  _post(file, dir, name, offset, end, total, onSent){
    return new Promise((resolve) => {
      const xhr = new XMLHttpRequest();
      this._xhr = xhr;
      const q = total ? '?dir=' + encodeURIComponent(dir) + '&total=' + total + '&offset=' + offset : '';
      xhr.open('POST', '/upload' + q, true);
      xhr.withCredentials = true;
      for (const [k, v] of Object.entries(this._headers())) xhr.setRequestHeader(k, v);
      xhr.timeout = 180000;                      // a 4 MB slice in three minutes is 23 KB/s
      const fd = new FormData();
      fd.append('dir', dir);                     // fields must precede the file part
      fd.append('file', total ? file.slice(offset, end) : file, name);
      xhr.upload.onprogress = (e) => { if (e.lengthComputable && e.total) onSent(Math.min(1, e.loaded / e.total) * (end - offset)); };
      xhr.onload = () => {
        let json = {};
        try { json = JSON.parse(xhr.responseText) || {}; } catch (e) {}
        resolve({ status: xhr.status, json });
      };
      xhr.onerror = xhr.ontimeout = xhr.onabort = () => resolve({ status: 0, json: {} });
      xhr.send(fd);
    });
  },

  // one file onto the card. Resolves 'done' or 'skipped', throws with .how = 'file' (this
  // file failed), 'all' (no file will go: uploads off, password needed) or 'stop'.
  async _one(file, dir, show){
    const base = dir === '/' ? '' : dir.replace(/\/+$/, '');
    let name = file.name, n = 1;
    for (;;) {
      // 1. where it goes, and how much of it is already there
      let st = await this._status(base + '/' + name, file.size);
      if (st && st.legacy) return this._legacy(file, dir, show);
      let offset = 0;
      if (st) {
        if (st.size >= 0) {
          if (st.size === file.size) return 'skipped';          // the same file, already there
          name = this._numbered(file.name, ++n);                 // a different one by that name
          continue;
        }
        if (st.part > 0 && st.part <= file.size) offset = st.part;
        if (st.free >= 0 && file.size - offset > st.free) {
          throw this._fail('The card is full. It has ' + this._size(st.free) + ' free.', 'file');
        }
      }
      // 2. the bytes, a slice at a time, picking up from whatever arrived
      let misses = 0, renamed = false;
      while (!renamed) {
        if (this._stop) throw this._fail('Stopped.', 'stop');
        const end = Math.min(file.size, offset + this.SLICE);
        show(offset, misses ? 'Connection lost. Trying again (' + misses + ')' : '');
        const r = await this._post(file, dir, name, offset, end, file.size, (sent) => show(offset + sent));
        const j = r.json;
        if (this._stop) {
          show(offset, 'Stopping\u2026');
          await this._cancel(base + '/' + name, file.size);
          throw this._fail('Stopped.', 'stop');
        }
        if (r.status === 200 && j.status === 'partial') { offset = j.have; misses = 0; continue; }
        if (r.status === 200) return 'done';
        if (r.status === 409 && j.error === 'offset') { offset = j.have || 0; continue; }
        if (r.status === 409 && /exist/i.test(j.error || '')) { name = this._numbered(file.name, ++n); renamed = true; continue; }
        if (r.status === 507) throw this._fail('The card is full.', 'file');
        if (r.status === 401) throw this._fail('That folder needs the admin password.', 'all');
        if (r.status === 403) throw this._fail(j.error || 'Uploads are turned off on Nomad.', 'all');
        if (r.status === 400) throw this._fail('Nomad refused this file name.', 'file');
        // lost connection, the card busy (503), the same file coming from another phone
        // (409 busy): wait, ask how much arrived, carry on
        if (++misses > this.RETRIES) throw this._fail('Connection to Nomad lost. Try again to carry on from where it stopped.', 'file');
        show(offset, 'Connection lost. Trying again (' + misses + ')');
        await this._wait(Math.min(15000, 1000 * 2 ** (misses - 1)));
        st = await this._status(base + '/' + name, file.size);
        if (st && st.size === file.size) return 'done';          // it finished just as the answer was lost
        if (st && st.part >= 0) offset = st.part;
      }
    }
  },

  // a stopped file's part would only sit on the card. The device may still be finishing
  // the slice that was cut off ("busy"), so ask again for a few seconds.
  async _cancel(path, total){
    for (let i = 0; i < 5; i++) {
      try {
        const r = await fetch('/upload-cancel', { method: 'POST', headers: this._headers(),
          body: new URLSearchParams({ path, total: String(total) }) });
        if (r.status !== 409 && r.status !== 503) return;
      } catch (e) {}
      await this._wait(1000);
    }
  },

  // firmware from before resumable uploads: the whole file in one request
  async _legacy(file, dir, show){
    const r = await this._post(file, dir, file.name, 0, file.size, 0, (sent) => show(sent));
    if (r.status >= 200 && r.status < 300) return 'done';
    if (r.status === 409) throw this._fail('A file by this name is already on the card.', 'file');
    if (r.status === 401) throw this._fail('That folder needs the admin password.', 'all');
    if (r.status === 403) throw this._fail(r.json.error || 'Uploads are turned off on Nomad.', 'all');
    throw this._fail(r.status ? 'Nomad refused the upload (HTTP ' + r.status + ').' : 'The connection to Nomad was lost.', 'file');
  },

  /* uploads `files` into `dir`, one at a time, resolves with how many were added */
  async run(files, dir){
    this._ensure();
    const [title, fileEl, bar, count, note, error, stopBtn, retryBtn, closeBtn] =
      ['nuTitle','nuFile','nuBar','nuCount','nuNote','nuError','nuStop','nuRetry','nuClose'].map(id => this._el(id));
    const ov = this._el('nuOverlay');
    ov.classList.add('open');
    // leaving the page stops the upload (what arrived is kept for next time)
    const guard = (e) => { e.preventDefault(); e.returnValue = ''; };
    window.addEventListener('beforeunload', guard);

    let added = 0, skipped = 0, todo = Array.from(files);
    const finished = new Set();
    try {
      for (;;) {
        this._stop = false;
        const failed = [];
        let stopped = false;
        const totalBytes = todo.reduce((a, f) => a + f.size, 0);
        let doneBytes = 0;
        title.textContent = 'Uploading';
        error.textContent = '';
        note.textContent = 'Keep this page open until the upload finishes.';
        stopBtn.style.display = 'inline-block';
        retryBtn.style.display = closeBtn.style.display = 'none';
        for (let i = 0; i < todo.length; i++) {
          const f = todo[i];
          fileEl.textContent = f.name;
          const show = (at, msg) => {
            const all = totalBytes ? (doneBytes + at) / totalBytes : 1;
            bar.style.width = Math.round(all * 100) + '%';
            count.textContent = 'File ' + (i + 1) + ' of ' + todo.length + ' · ' +
              this._size(doneBytes + at) + ' of ' + this._size(totalBytes);
            note.textContent = msg || 'Keep this page open until the upload finishes.';
          };
          show(0);
          try {
            const r = await this._one(f, dir, show);
            if (r === 'done') added++; else skipped++;
            finished.add(f);
          } catch (e) {
            if (e.how === 'stop') { stopped = true; break; }
            failed.push({ file: f, msg: e.message });
            if (e.how === 'all') { for (const rest of todo.slice(i + 1)) failed.push({ file: rest, msg: '' }); break; }
          }
          doneBytes += f.size;
        }
        stopBtn.style.display = 'none';
        fileEl.textContent = [added && added + ' uploaded', skipped && skipped + ' already on the card']
          .filter(Boolean).join(', ') || (stopped ? '' : 'Nothing uploaded');
        count.textContent = '';
        note.textContent = '';
        if (!failed.length && !stopped) {
          title.textContent = 'Upload complete';
          bar.style.width = '100%';
          await this._wait(1200);
          ov.classList.remove('open');
          return added;
        }
        title.textContent = stopped ? 'Upload stopped' : failed.length === todo.length ? 'Upload failed' : "Some files didn't upload";
        const shown = failed.filter(x => x.msg).slice(0, 6);
        error.textContent = shown.map(x => x.file.name + ': ' + x.msg).join('\n') +
          (failed.length > shown.length ? '\n' + (failed.length - shown.length) + ' more not uploaded.' : '');
        error.style.whiteSpace = 'pre-line';
        retryBtn.style.display = 'inline-block';
        closeBtn.style.display = 'inline-block';
        const choice = await new Promise((res) => {
          retryBtn.onclick = () => res('retry');
          closeBtn.onclick = () => res('close');
        });
        if (choice === 'close') { ov.classList.remove('open'); return added; }
        // the ones that didn't make it. a part already on the card is carried on from
        todo = todo.filter(f => !finished.has(f));
      }
    } finally {
      window.removeEventListener('beforeunload', guard);
    }
  }
};

