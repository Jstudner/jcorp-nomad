// <!-- Version 1 -->
/* zim-sw.js - the archive reader's full mode: a service worker that answers /zimview/
 * straight out of a ZIM, as kiwix-js does in ServiceWorker mode. Pages open at a real
 * address, /zimview/<archive>/<namespace>/<path> (e.g. /zimview/4/C/Paris), so their own
 * stylesheets, scripts, relative links and videos load. That covers what the script-less
 * reader can't: web-app archives (Gutenberg, TED, PhET, video channels) and Zimit copies.
 *
 * Service workers need a secure page (https, localhost, or Chrome's "treat as secure"
 * setting), so archive.html uses this only where the browser allows it.
 * ZIM reading is nomad-zim.js, the same code the page uses, all on the phone. */
'use strict';
importScripts('/assets/nomad-zim.js', '/assets/xzwasm.min.js', '/assets/fzstd.min.js');
// a page asks for its stylesheet, scripts and images all at once. three reads at a time
// (the firmware gives each reader its own file handle) is a lot faster than one. more
// than about four at once and the Nomad starts turning reads away
NomadZim.sharedQueue.concurrency = 3;
// the online demo has no /media, its server hands out the archive files with ranges itself
if (new URL(self.location.href).searchParams.has('direct')) NomadZim.NomadArchiveIndex.VIA = '';

const decompressors = {
  zstd: async (data) => fzstd.decompress(data),
  xz: async (data) => {
    const stream = new Blob([data]).stream();
    return new Uint8Array(await new Response(new xzwasm.XzReadableStream(stream)).arrayBuffer());
  },
};

let indexP = null;
function getIndex() {
  if (!indexP) {
    const idx = new NomadZim.NomadArchiveIndex('/Archive/.nomad-zim');
    indexP = idx.load().then(() => idx).catch((e) => { indexP = null; throw e; });
  }
  return indexP;
}

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));

// firmware that answers /zimview/ itself (NomadZimServe) is faster than this worker:
// unpacking on the Nomad beats pulling every compressed cluster over WiFi. A phone that
// registered the worker before keeps it, so it steps aside there. Asked once per start
let nomadServesP = null;
function nomadServes() {
  if (!nomadServesP) nomadServesP = fetch('/api/zimserve', { cache: 'no-store' }).then((r) => r.ok ? r.json() : null)
    .then((j) => !!(j && j.v >= 1)).catch(() => false);
  return nomadServesP;
}

self.addEventListener('fetch', (e) => {
  const u = new URL(e.request.url);
  if (u.origin !== self.location.origin || !u.pathname.startsWith('/zimview/')) return;
  e.respondWith(nomadServes().then((yes) => yes ? fetch(e.request) : serve(u, e.request)));
});

const NAMESPACES = /^[A-Z-]$/;
async function serve(u, request) {
  try {
    const m = /^\/zimview\/(\d+)\/?(.*)$/.exec(u.pathname);
    if (!m) return notFound('That address is not in an archive.');
    const archiveIdx = +m[1];
    let path = m[2];
    try { path = decodeURIComponent(path); } catch (e) { /* keep it as it came */ }
    const idx = await getIndex();
    const zim = await idx._zim(archiveIdx, decompressors);

    // the archive's own address: on to its main page
    if (!path) {
      const main = await zim.getMainPage();
      if (!main) return notFound('This archive has no main page.');
      return redirect(archiveIdx, main.namespace + '/' + main.url, u);
    }

    // "<namespace>/<url>". a relative link that climbed out of its namespace ("../x"
    // from the top of C/) lands without one, and means content (C, or A on old ZIMs)
    const slash = path.indexOf('/');
    let ns = slash > 0 ? path.slice(0, slash) : '';
    let rest = slash > 0 ? path.slice(slash + 1) : path;
    // a Zimit copy stores a page that had a query string ("list?page=2") under its whole
    // address, so with a query that is tried first
    let q = '';
    try { q = decodeURIComponent(u.search); } catch (e) { q = u.search; }
    const tries = [];
    if (q && NAMESPACES.test(ns)) tries.push([ns, rest + q, true]);
    if (q) tries.push(['C', path + q, true]);
    if (NAMESPACES.test(ns)) tries.push([ns, rest]);
    tries.push(['C', path], ['A', path], ['I', path], ['-', path]);
    let entry = null, at = null, withQuery = false;
    for (const [n, p, wq] of tries) {
      entry = await zim.findByUrl(n, p);
      if (!entry && p.endsWith('/')) entry = await zim.findByUrl(n, p + 'index.html');   // a directory page
      if (entry) { at = n + '/' + p; withQuery = !!wq; break; }
    }
    if (!entry) return notFound('This page is not in the archive: ' + path);
    if (entry.isRedirect) {
      const target = await zim.resolveRedirect(entry);
      if (target && (target.namespace + '/' + target.url) !== at) return redirect(archiveIdx, target.namespace + '/' + target.url, u);
      entry = target || entry;
    }
    if (at !== ns + '/' + rest + (withQuery ? q : '')) return redirect(archiveIdx, at, u, withQuery);   // so its own relative links resolve

    // a video or other big file sits in an uncompressed cluster: answer each range
    // straight from the card. reading a whole talk (tens of MB) first is a minute of nothing
    const raw = await zim._locateRawBlob(entry.clusterNumber, entry.blobNumber);
    if (raw && raw.size > CARD_CHUNK) return fromCard(zim, raw, entry.mimeType || mimeOf(entry.url) || 'application/octet-stream', request);

    const content = await zim.getContent(entry);
    let type = content.mimeType || mimeOf(entry.url) || 'application/octet-stream';
    if (/^text\/|javascript|json|xml|svg/.test(type) && !/charset=/i.test(type)) type += '; charset=utf-8';
    const isPage = /^text\/html|xhtml/.test(type);
    return withRange(isPage ? withFrameScript(content.data) : content.data, type, request, isPage);
  } catch (err) {
    return new Response(page('Couldn\'t read this from the archive',
      'Nomad may be busy or out of range. Go back and try again. (' + esc(String(err && err.message || err)) + ')'),
      { status: 503, headers: { 'Content-Type': 'text/html; charset=utf-8' } });
  }
}

function redirect(archiveIdx, nsPath, u, queryInPath) {
  const loc = '/zimview/' + archiveIdx + '/' + nsPath.split('/').map(encodeURIComponent).join('/') + (queryInPath ? '' : u.search) + u.hash;
  return new Response(null, { status: 302, headers: { Location: loc } });
}

// every page gets /assets/zim-frame.js first thing in its <head> (the same idea as the
// script kiwix-js puts into each page): it keeps "_top" links inside the reader and stops
// links out to the web. bytes in, bytes out, so the page's own text encoding is untouched
const FRAME_TAG = new TextEncoder().encode('<script src="/assets/zim-frame.js"></script>');
function withFrameScript(data) {
  if (data.length > 1 && ((data[0] === 0xFF && data[1] === 0xFE) || (data[0] === 0xFE && data[1] === 0xFF))) return data;   // UTF-16
  const start = new TextDecoder('latin1').decode(data.subarray(0, Math.min(data.length, 16384)));
  const m = /<head(\s[^>]*)?>/i.exec(start) || /<html(\s[^>]*)?>/i.exec(start) || /<!doctype[^>]*>/i.exec(start);
  const at = m ? m.index + m[0].length : 0;
  const out = new Uint8Array(data.length + FRAME_TAG.length);
  out.set(data.subarray(0, at)); out.set(FRAME_TAG, at); out.set(data.subarray(at), at + FRAME_TAG.length);
  return out;
}

// smaller files come out of the archive whole, a range of one is cut from the bytes in hand.
// An archive page runs its own scripts at the Nomad's address (as in Kiwix's reader).
// They may only talk to the archive itself: not the Nomad's admin, upload or delete calls.
function pagePolicy() {
  const o = self.location.origin;
  return "connect-src " + o + "/zimview/ blob: data:; form-action " + o + "/zimview/; base-uri " + o + "/zimview/";
}
function withRange(data, type, request, isPage) {
  const total = data.byteLength;
  const range = request.headers.get('Range');
  const m = range && /bytes=(\d*)-(\d*)/.exec(range);
  const base = { 'Content-Type': type, 'Accept-Ranges': 'bytes', 'Cache-Control': 'no-store' };
  if (isPage) base['Content-Security-Policy'] = pagePolicy();
  if (!m || (!m[1] && !m[2])) return new Response(data, { status: 200, headers: Object.assign({ 'Content-Length': String(total) }, base) });
  let start = m[1] === '' ? Math.max(0, total - +m[2]) : +m[1];
  let end = m[1] === '' || m[2] === '' ? total - 1 : Math.min(+m[2], total - 1);
  if (start >= total || start > end) return new Response(null, { status: 416, headers: { 'Content-Range': 'bytes */' + total } });
  return new Response(data.slice(start, end + 1), { status: 206, headers: Object.assign({
    'Content-Range': 'bytes ' + start + '-' + end + '/' + total, 'Content-Length': String(end - start + 1) }, base) });
}

// one range answer at most this big, a player asks again for the next piece
const CARD_CHUNK = 1024 * 1024;
function fromCard(zim, raw, type, request) {
  const total = raw.size;
  const m = /bytes=(\d*)-(\d*)/.exec(request.headers.get('Range') || '');
  const base = { 'Content-Type': type, 'Accept-Ranges': 'bytes', 'Cache-Control': 'no-store' };
  if (!m || (!m[1] && !m[2])) {
    // no range (a download): the whole file, read from the card as it goes out
    let at = 0;
    const body = new ReadableStream({ async pull(c) {
      if (at >= total) { c.close(); return; }
      const n = Math.min(CARD_CHUNK, total - at);
      c.enqueue(await zim.src.read(raw.offset + at, n)); at += n;
    } });
    return new Response(body, { status: 200, headers: Object.assign({ 'Content-Length': String(total) }, base) });
  }
  const start = m[1] === '' ? Math.max(0, total - +m[2]) : +m[1];
  let end = m[1] === '' || m[2] === '' ? total - 1 : Math.min(+m[2], total - 1);
  if (start >= total || start > end) return new Response(null, { status: 416, headers: { 'Content-Range': 'bytes */' + total } });
  end = Math.min(end, start + CARD_CHUNK - 1);
  return zim.src.read(raw.offset + start, end - start + 1).then((data) => new Response(data, { status: 206, headers: Object.assign({
    'Content-Range': 'bytes ' + start + '-' + end + '/' + total, 'Content-Length': String(data.byteLength) }, base) }));
}

const MIME = { html: 'text/html', htm: 'text/html', css: 'text/css', js: 'text/javascript', mjs: 'text/javascript', json: 'application/json',
  svg: 'image/svg+xml', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', webp: 'image/webp', avif: 'image/avif',
  ico: 'image/x-icon', woff: 'font/woff', woff2: 'font/woff2', ttf: 'font/ttf', otf: 'font/otf', mp4: 'video/mp4', webm: 'video/webm',
  ogv: 'video/ogg', mp3: 'audio/mpeg', ogg: 'audio/ogg', vtt: 'text/vtt', pdf: 'application/pdf', epub: 'application/epub+zip', txt: 'text/plain', xml: 'text/xml' };
function mimeOf(url) { const m = /\.([a-z0-9]+)$/i.exec(url || ''); return m ? MIME[m[1].toLowerCase()] : null; }
function esc(s) { return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
function page(title, text) {
  return '<!DOCTYPE html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<body style="font-family:system-ui,sans-serif;padding:24px;line-height:1.5;color:#333"><h2 style="font-size:1.2rem">' + esc(title) + '</h2><p>' + text + '</p></body>';
}
function notFound(text) {
  return new Response(page('Not in this archive', esc(text)), { status: 404, headers: { 'Content-Type': 'text/html; charset=utf-8' } });
}
