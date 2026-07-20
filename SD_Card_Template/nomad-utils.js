// <!-- Version 4 -->
/* tiny shared helpers used across the frontend pages, dependency-free */

// one leading slash, no trailing slash, no double slashes, backslashes -> forward.
// most defensive of the old per-page copies (some missed doubled slashes)
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

/* Subtitle support. Browsers only render WebVTT in <track> elements, so
   buildTracks() converts .srt to VTT client-side and passes .vtt through. */
const NomadSubs = {
  _blobCache: new Map(),

  srtToVtt(text){
    let s = String(text).replace(/^﻿/, '').replace(/\r/g, '');
    s = s.replace(/(\d{2}:\d{2}:\d{2}),(\d{3})/g, '$1.$2');
    return 'WEBVTT\n\n' + s.trim() + '\n';
  },

  /* "Movie.en.srt" beside "Movie.mp4" -> "EN"; plain "Movie.srt" -> "Subtitles" */
  labelFor(subPath, videoPath, i){
    const base = (p) => String(p).split('/').pop().replace(/\.[^.]+$/, '');
    const vb = videoPath ? base(videoPath) : '';
    let rest = base(subPath);
    if (vb && rest.toLowerCase().startsWith(vb.toLowerCase())) rest = rest.slice(vb.length);
    rest = rest.replace(/^[ ._-]+/, '');
    if (rest && rest.length <= 12) return rest.toUpperCase();
    return i === 0 ? 'Subtitles' : 'Subtitles ' + (i + 1);
  },

  /* subs: [{path, url}] -> Plyr-style track list ({kind,label,srclang,src,default}) */
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
            const buf = await (await fetch(s.url)).arrayBuffer();
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
        src, default: out.length === 0 });
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
    try {
      const r = await fetch('/.system-ui.json?_=' + Date.now(), { cache: 'no-store' });
      this._cfg = r.ok ? (JSON.parse(await r.text()) || {}) : {};
    } catch(e){ this._cfg = {}; }
    return this._cfg;
  },
  async hideDownloads(...els){
    const cfg = await this.config();
    if (cfg.downloadsDisabled) {
      els.forEach(sel => {
        const el = typeof sel === 'string' ? document.getElementById(sel) : sel;
        if (el) el.style.display = 'none';
      });
      return true;
    }
    return false;
  }
};

/* Shared upload runner with a full-screen progress modal. One file at a
   time with breathing room between files, which keeps load off the device. */
const NomadUpload = {
  _built: false,

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
  .nu-error{color:#c0392b;font-size:.88rem;margin-top:.5rem;word-break:break-word}
  .nu-close{display:none;margin-top:.8rem;background:var(--primary,#0b62d6);
    color:var(--color-white,#fff);border:none;border-radius:8px;padding:9px 20px;cursor:pointer}
</style>
<div class="nu-overlay" id="nuOverlay">
  <div class="nu-card">
    <div class="nu-title" id="nuTitle">Uploading</div>
    <div class="nu-file" id="nuFile"></div>
    <div class="nu-bar-wrap"><div class="nu-bar" id="nuBar"></div></div>
    <div class="nu-count" id="nuCount"></div>
    <div class="nu-note" id="nuNote">Keep this page open until the upload finishes.</div>
    <div class="nu-error" id="nuError"></div>
    <button class="nu-close" id="nuClose">Close</button>
  </div>
</div>`);
    document.getElementById('nuClose').addEventListener('click', () => {
      document.getElementById('nuOverlay').classList.remove('open');
    });
  },

  _one(file, dir){
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/upload', true);
      xhr.withCredentials = true;
      const fd = new FormData();
      fd.append('dir', dir);                       // fields must precede the file part
      fd.append('file', file, file.name);
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          document.getElementById('nuBar').style.width = Math.round(e.loaded / e.total * 100) + '%';
        }
      };
      xhr.onload = () => (xhr.status >= 200 && xhr.status < 300)
        ? resolve() : reject(new Error('Device rejected the upload (HTTP ' + xhr.status + ')'));
      xhr.onerror = () => reject(new Error('Connection lost during upload'));
      xhr.send(fd);
    });
  },

  /* uploads `files` sequentially into `dir`; resolves with the success count */
  async run(files, dir){
    this._ensure();
    const ov = document.getElementById('nuOverlay');
    const [title, fileEl, bar, count, note, error, close] =
      ['nuTitle','nuFile','nuBar','nuCount','nuNote','nuError','nuClose'].map(id => document.getElementById(id));
    title.textContent = 'Uploading';
    error.textContent = '';
    close.style.display = 'none';
    note.style.display = '';
    ov.classList.add('open');

    let ok = 0;
    for (let i = 0; i < files.length; i++){
      fileEl.textContent = files[i].name;
      count.textContent = 'File ' + (i + 1) + ' of ' + files.length;
      bar.style.width = '0%';
      try {
        await this._one(files[i], dir);
        ok++;
        await new Promise(r => setTimeout(r, 400));   // let the device breathe between files
      } catch(e){
        title.textContent = 'Upload failed';
        error.textContent = files[i].name + ': ' + e.message +
          (ok ? ` (${ok} file${ok === 1 ? '' : 's'} uploaded before this one)` : '');
        note.style.display = 'none';
        close.style.display = 'inline-block';
        return ok;
      }
    }
    title.textContent = 'Upload complete';
    fileEl.textContent = ok + ' file' + (ok === 1 ? '' : 's') + ' uploaded.';
    bar.style.width = '100%';
    count.textContent = '';
    note.style.display = 'none';
    setTimeout(() => ov.classList.remove('open'), 1200);
    return ok;
  }
};
