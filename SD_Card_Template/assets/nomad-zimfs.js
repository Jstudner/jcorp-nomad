// <!-- Version 4 -->
/* nomad-zimfs.js - serve a StreetZim map's data straight out of the .zim.
 *
 * Extracting a StreetZim to real files multiplies its size (8.9 GiB zim ->
 * 41 GiB across 2.87M files) and takes hours to write. The viewer fetches
 * everything as plain relative URLs, so this wraps fetch() and resolves those
 * URLs to ZIM entries instead, reading only the bytes needed. The card holds
 * the .zim plus a ~33 MB app shell.
 *
 * Only bulk data comes from the zim; the shell is extracted as real files so
 * the page boots before any of this is ready.
 *
 * Load fzstd.min.js, xzwasm.min.js and nomad-zim.js first, and set
 * window.NOMAD_ZIMFS = { zim: '/Maps/<id>/<file>.zim' } before this script.
 */
(function () {
  'use strict';

  var cfg = window.NOMAD_ZIMFS || {};
  if (!cfg.zim) return;  // not a zim-backed region; leave fetch alone

  // An absent shim looks exactly like a broken map: every tile 404s from the
  // file server. Say so at load and name the missing dependency.
  var missing = [];
  if (!window.NomadZim) missing.push('nomad-zim.js');
  if (!window.fzstd) missing.push('fzstd.min.js');
  if (missing.length) {
    console.error('[zimfs] NOT ACTIVE - missing ' + missing.join(', ') +
                  '. Tiles will 404 from the file server. Copy these into /assets/ on the card.');
    return;
  }
  console.log('[zimfs] active - serving bulk map data from ' + cfg.zim);

  // directories that live inside the zim rather than on the card
  var BULK = /(^|\/)(tiles|satellite|terrain|search|search-data|category-index|routing-data)\//;

  var MIME = {
    pbf: 'application/x-protobuf',
    json: 'application/json',
    html: 'text/html; charset=utf-8',
    avif: 'image/avif',
    webp: 'image/webp',
    png: 'image/png',
    jpg: 'image/jpeg',
    jpeg: 'image/jpeg',
    bin: 'application/octet-stream',
    js: 'text/javascript',
    css: 'text/css'
  };

  function mimeFor(path) {
    var m = /\.([a-z0-9]+)$/i.exec(path);
    return (m && MIME[m[1].toLowerCase()]) || 'application/octet-stream';
  }

  var origFetch = window.fetch.bind(window);
  var zimP = null;
  var stats = { served: 0, misses: 0, bytes: 0, byDir: {} };

  function tally(path, bytes, miss) {
    var dir = path.split('/')[0] + '/';
    if (!stats.byDir[dir]) stats.byDir[dir] = { n: 0, bytes: 0 };
    stats.byDir[dir].n++;
    stats.byDir[dir].bytes += bytes || 0;
    if (miss) stats.misses++; else stats.served++;
    stats.bytes += bytes || 0;
  }

  function decompressors() {
    var d = {};
    if (window.fzstd && window.fzstd.decompress) {
      d.zstd = function (u8) { return window.fzstd.decompress(u8); };
    }
    // global is `xzwasm`, matching archive.html, not XzReadableStream
    if (window.xzwasm && window.xzwasm.XzReadableStream) {
      d.xz = function (u8) {
        return new Response(new window.xzwasm.XzReadableStream(new Response(u8).body))
          .arrayBuffer().then(function (b) { return new Uint8Array(b); });
      };
    }
    return d;
  }

  // HttpSource needs the part size up front; the installer records it so we
  // skip a HEAD on every load, but ask for it if missing.
  function zimSize() {
    if (cfg.size > 0) return Promise.resolve(cfg.size);
    return origFetch('/media?file=' + encodeURIComponent(cfg.zim), { method: 'HEAD' })
      .then(function (r) { return parseInt(r.headers.get('Content-Length') || '0', 10); });
  }

  function openZim() {
    if (zimP) return zimP;
    zimP = zimSize().then(function (size) {
      if (!size) throw new Error('could not determine zim size');
      // range reads must go through /media?file=, which honours Range headers
      var src = new window.NomadZim.HttpSource([{ path: cfg.zim, size: size }], {});
      var z = new window.NomadZim.ZimArchive(src, { decompressors: decompressors() });
      return z.open().then(function () { return z; });
    });
    return zimP;
  }

  // "http://host/Maps/x/tiles/1/2/3.pbf" or "tiles/1/2/3.pbf" -> "tiles/1/2/3.pbf"
  function toZimPath(url) {
    var s = String(url);
    if (/^[a-z]+:\/\//i.test(s) && s.indexOf(location.origin) !== 0) return null;
    if (s.indexOf(location.origin) === 0) s = s.slice(location.origin.length);
    s = s.split('#')[0].split('?')[0];
    // never touch the zim's own range reads - that would recurse forever
    if (s === '/media' || s.indexOf('/media?') === 0) return null;
    if (s === cfg.zim) return null;
    var m = BULK.exec(s);
    if (!m) return null;
    return s.slice(m.index + (m[1] ? m[1].length : 0));
  }

  function notFound(path) {
    return new Response('not in zim: ' + path, { status: 404, statusText: 'Not Found' });
  }

  function serve(path) {
    return openZim().then(function (z) {
      return z.findByUrl('C', path).then(function (e) {
        if (!e) { tally(path, 0, true); return notFound(path); }
        if (e.redirectIndex !== undefined && e.clusterNumber === undefined) {
          return z.resolveRedirect(e).then(function (t) {
            if (!t) return notFound(path);
            return z.getContentAt(t.clusterNumber, t.blobNumber).then(toResponse(path));
          });
        }
        return z.getContentAt(e.clusterNumber, e.blobNumber).then(toResponse(path));
      });
    }).catch(function (err) {
      return new Response('zim read failed: ' + (err && err.message), {
        status: 500, statusText: 'ZIM error'
      });
    });
  }

  function toResponse(path) {
    return function (data) {
      var bytes = data && data.length !== undefined ? data
                : (data && data.data ? data.data : new Uint8Array(0));
      tally(path, bytes.length, false);
      // copy into a standalone buffer: the cluster cache may reuse its own
      return new Response(new Uint8Array(bytes).buffer, {
        status: 200,
        headers: { 'Content-Type': mimeFor(path), 'Content-Length': String(bytes.length) }
      });
    };
  }

  window.fetch = function (input, init) {
    var url = (typeof input === 'string') ? input
            : (input && input.url) ? input.url : String(input);
    var path = toZimPath(url);
    if (!path) return origFetch(input, init);
    return serve(path);
  };

  window.NomadZimFS = {
    zimPath: cfg.zim,
    ready: function () { return openZim(); },
    // exposed for diagnostics from the console
    resolve: function (p) { return openZim().then(function (z) { return z.findByUrl('C', p); }); },
    // live counters, for seeing whether a slow map is over-fetching
    stats: stats,
    report: function () {
      var byDir = Object.keys(stats.byDir).map(function (k) {
        return '  ' + k + ': ' + stats.byDir[k].n + ' reqs, ' +
               (stats.byDir[k].bytes / 1048576).toFixed(2) + ' MB';
      }).join('\n');
      return 'served ' + stats.served + ' requests, ' + stats.misses + ' not-in-zim, ' +
             (stats.bytes / 1048576).toFixed(2) + ' MB delivered\n' + byDir;
    }
  };
})();
