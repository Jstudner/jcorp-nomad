// <!-- Version 5 -->
/* nomad-xlate.js - fetch model files for offline translation off the card.
 *
 * transformers.js wants to fetch model files as plain URLs. Two things stop that
 * working on a Nomad:
 *
 *   1. A plain GET under /Translate/** falls through to the firmware's onNotFound
 *      whole-file response, which ignores Range and streams the entire file. A
 *      57 MB decoder as one un-resumable stream wedges the async web server.
 *      Reads go through GET /media?file=<urlencoded path> instead, which maps to
 *      handleRangeRequest and honours Range. Same rule as nomad-zim.js.
 *
 *   2. http://192.168.4.1 is not a secure context, so the Cache API is absent and
 *      transformers.js's own caching silently no-ops. IndexedDB is available on
 *      insecure origins, so the cache is built here instead.
 *
 * So: intercept fetch, pull each file as a series of Range reads, store the chunks
 * in IndexedDB as they land, and answer from IndexedDB after that. Chunk level
 * storage is what makes a download resumable, a drop at 90% costs one chunk.
 *
 * Requests are issued one at a time on purpose. handleRangeRequest keeps a single
 * reused file handle per path, so two in-flight requests to the same file seek each
 * other's handle out from under them. Same reason nomad-zim.js pins FetchQueue to 1.
 */
(function () {
  'use strict';

  var VIA = '/media?file=';

  /* 1 MiB matches the firmware's tuned MAX_REQ_BYTES on exFAT. Device benchmark
   * (zimbench.html): ~24 ms fixed overhead per request, throughput flattens at
   * ~625-784 KB/s by 256 KB. Bigger chunks only buy back request overhead: 114 MB
   * costs ~114 requests at 1 MiB versus ~456 at 256 KB. Past 1 MiB there is nothing
   * left to win and each retry gets more expensive. */
  var CHUNK = 1048576;

  var DB_NAME = 'nomad-xlate';
  var DB_VER = 1;
  var RETRIES = 4;

  var dbP = null;

  function openDB() {
    if (dbP) return dbP;
    dbP = new Promise(function (resolve, reject) {
      var req = indexedDB.open(DB_NAME, DB_VER);
      req.onupgradeneeded = function (e) {
        var db = e.target.result;
        if (!db.objectStoreNames.contains('chunks')) db.createObjectStore('chunks');
        if (!db.objectStoreNames.contains('meta')) db.createObjectStore('meta');
      };
      req.onsuccess = function () { resolve(req.result); };
      req.onerror = function () { reject(req.error); };
    });
    return dbP;
  }

  function idbGet(store, key) {
    return openDB().then(function (db) {
      return new Promise(function (resolve, reject) {
        var r = db.transaction(store, 'readonly').objectStore(store).get(key);
        r.onsuccess = function () { resolve(r.result); };
        r.onerror = function () { reject(r.error); };
      });
    });
  }

  function idbPut(store, key, val) {
    return openDB().then(function (db) {
      return new Promise(function (resolve, reject) {
        var tx = db.transaction(store, 'readwrite');
        tx.objectStore(store).put(val, key);
        tx.oncomplete = function () { resolve(); };
        tx.onerror = function () { reject(tx.error); };
      });
    });
  }

  // how many chunks of a file are already stored. chunk keys are "<path>#<index>", so
  // a bounded range over that prefix counts them without reading the data back
  function countChunks(path) {
    return openDB().then(function (db) {
      return new Promise(function (resolve, reject) {
        var range = IDBKeyRange.bound(path + '#', path + '#￿');
        var r = db.transaction('chunks', 'readonly').objectStore('chunks').count(range);
        r.onsuccess = function () { resolve(r.result); };
        r.onerror = function () { reject(r.error); };
      });
    });
  }

  function idbClear() {
    return openDB().then(function (db) {
      return new Promise(function (resolve, reject) {
        var tx = db.transaction(['chunks', 'meta'], 'readwrite');
        tx.objectStore('chunks').clear();
        tx.objectStore('meta').clear();
        tx.oncomplete = function () { resolve(); };
        tx.onerror = function () { reject(tx.error); };
      });
    });
  }

  /* ---------------- range reads ---------------- */

  // total file size out of a 206's Content-Range ("bytes a-b/TOTAL"). chunk 0 carries
  // this, so there is no separate size probe
  function totalFromResponse(res) {
    var cr = res.headers.get('Content-Range') || '';
    var m = /\/(\d+)\s*$/.exec(cr);
    if (m) return parseInt(m[1], 10);
    var cl = res.headers.get('Content-Length');
    if (cl) return parseInt(cl, 10);   // 200 response: body is the whole file
    return null;
  }

  /* Self-check on the one rule that can corrupt reads on the device: never two
   * in-flight requests for the same path (the firmware reuses one file handle per
   * path). Counted here rather than server-side, where keep-alive makes the request
   * window ambiguous. `samePathViolations` must stay 0. */
  var live = {};
  var diag = { maxPerPath: 0, maxPaths: 0, samePathViolations: 0, requests: 0 };

  function liveEnter(path) {
    diag.requests++;
    live[path] = (live[path] || 0) + 1;
    if (live[path] > diag.maxPerPath) diag.maxPerPath = live[path];
    if (live[path] > 1) {
      diag.samePathViolations++;
      console.error('[xlate] SAME-PATH OVERLAP on ' + path + ' - this corrupts the device handle');
    }
    var n = Object.keys(live).filter(function (k) { return live[k] > 0; }).length;
    if (n > diag.maxPaths) diag.maxPaths = n;
  }
  function liveLeave(path) {
    live[path]--;
    if (live[path] <= 0) delete live[path];
  }

  function rangeGet(path, start, end) {
    var url = VIA + encodeURIComponent(path);
    var attempt = 0;
    function go() {
      attempt++;
      var ctl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
      // A 1 MiB chunk at the device's ~0.6-0.8 MB/s needs well over a second;
      // 45 s leaves room for a slow card without hanging the UI forever.
      var timer = ctl ? setTimeout(function () { ctl.abort(); }, 45000) : null;
      liveEnter(path);
      return fetch(url, {
        headers: { 'Range': 'bytes=' + start + '-' + end },
        signal: ctl ? ctl.signal : undefined,
        cache: 'no-store'
      }).then(function (res) {
        if (timer) clearTimeout(timer);
        if (!res.ok && res.status !== 206) {
          liveLeave(path);
          throw new Error('HTTP ' + res.status + ' on ' + path);
        }
        // stays "in flight" until the body is drained, because the device is
        // still streaming it out of that same file handle until then
        return res.arrayBuffer().then(function (buf) {
          liveLeave(path);
          return { res: res, buf: buf };
        }, function (e) { liveLeave(path); throw e; });
      }).catch(function (err) {
        if (timer) clearTimeout(timer);
        if (attempt > RETRIES) throw err;
        // Back off a little: a failure here usually means the server is busy
        // with another read, not that the file is bad.
        return new Promise(function (r) { setTimeout(r, 250 * attempt); }).then(go);
      });
    }
    return go();
  }

  /* ---------------- per-file transfer, resumable ----------------
   *
   * Chunks within one file stay strictly sequential: handleRangeRequest keeps a
   * single reused handle per path, so two in-flight requests for the same file seek
   * each other's handle out from under them.
   *
   * Different files are fetched concurrently, up to FILE_CONCURRENCY. Separate paths
   * mean separate handles, and this is where the speed comes from: the device reads a
   * chunk off the card and then sends it over WiFi as two serial phases, so with one
   * request in flight the radio idles during every SD read and the card idles during
   * every send. A second file fills those gaps. Measured single-stream throughput was
   * ~439 KB/s against ~784 KB/s of raw SD read, and that gap is the un-overlapped send.
   *
   * Kept low on purpose. Each in-flight request is another async_tcp connection and
   * the web stack is the part most prone to wedging under load. 2 recovers most of the
   * overlap, 8 would be a gamble.
   */
  var FILE_CONCURRENCY = (typeof window !== 'undefined' &&
                          +window.NOMAD_XLATE_CONCURRENCY > 0)
    ? +window.NOMAD_XLATE_CONCURRENCY : 2;

  var active = 0;
  var waiters = [];
  var inflight = {};    // path -> promise, so one file is never pulled twice

  function acquire() {
    if (active < FILE_CONCURRENCY) { active++; return Promise.resolve(); }
    return new Promise(function (r) { waiters.push(r); });
  }
  function release() {
    var next = waiters.shift();
    if (next) next(); else active--;
  }

  function fetchFile(path, onProgress) {
    if (inflight[path]) return inflight[path];

    var p = idbGet('meta', path).then(function (meta) {
      if (meta && meta.done) {
        // count already-finished files toward the bar. without this a resume reports only
        // the still-missing bytes, so the bar starts near 0% and looks like a fresh start
        if (onProgress) onProgress(meta.size, meta.size, true);
        return meta;
      }
      return acquire().then(function () {
        return pullFile(path, meta, onProgress).then(function (m) {
          release(); return m;
        }, function (err) {
          release(); throw err;
        });
      });
    });

    inflight[path] = p;
    var clear = function () { delete inflight[path]; };
    p.then(clear, clear);
    return p;
  }

  function pullFile(path, meta, onProgress) {
    var m = meta && meta.size
      ? meta
      : null;   // size still unknown; chunk 0 will tell us

    function loop(i) {
      if (m && i >= m.chunks) {
        m.done = true;
        return idbPut('meta', path, m).then(function () { return m; });
      }
      var idx = i;
      var key = path + '#' + idx;
      // Skip chunks a previous attempt already stored - the resume path.
      return idbGet('chunks', key).then(function (have) {
        if (have && m) {
          if (onProgress) onProgress(Math.min((idx + 1) * CHUNK, m.size), m.size, true);
          return loop(i + 1);
        }
        var start = idx * CHUNK;
        var end = m ? Math.min(start + CHUNK, m.size) - 1 : start + CHUNK - 1;
        return rangeGet(path, start, end).then(function (got) {
          // First response of an unknown file also establishes its size.
          if (!m) {
            var size = totalFromResponse(got.res);
            if (!size) throw new Error('cannot determine size of ' + path);
            m = { size: size, chunkSize: CHUNK, chunks: Math.ceil(size / CHUNK) || 1, done: false };
          }
          var want = Math.min(start + CHUNK, m.size) - start;
          // A short body means the device cut the stream; retry rather than
          // silently cache a truncated chunk.
          if (got.buf.byteLength !== want) {
            throw new Error('short read on ' + path + ' chunk ' + idx +
                            ': got ' + got.buf.byteLength + ' want ' + want);
          }
          return idbPut('meta', path, m).then(function () {
            return idbPut('chunks', key, new Uint8Array(got.buf));
          }).then(function () {
            if (onProgress) onProgress(Math.min((idx + 1) * CHUNK, m.size), m.size, false);
            return loop(i + 1);
          });
        });
      });
    }
    return Promise.resolve().then(function () { return loop(0); });
  }

  function assemble(path) {
    return idbGet('meta', path).then(function (m) {
      if (!m || !m.done) throw new Error('not cached: ' + path);
      var out = new Uint8Array(m.size);
      var i = 0;
      function next() {
        if (i >= m.chunks) return out;
        var idx = i;
        return idbGet('chunks', path + '#' + idx).then(function (bytes) {
          if (!bytes) throw new Error('missing chunk ' + idx + ' of ' + path);
          out.set(bytes, idx * m.chunkSize);
          i++;
          return next();
        });
      }
      return Promise.resolve().then(next);
    });
  }

  /* ---------------- fetch interception ---------------- */

  var MIME = {
    onnx: 'application/octet-stream',
    wasm: 'application/wasm',
    json: 'application/json',
    js: 'text/javascript',
    txt: 'text/plain'
  };

  function mimeFor(path) {
    var m = /\.([a-z0-9]+)$/i.exec(path);
    return (m && MIME[m[1].toLowerCase()]) || 'application/octet-stream';
  }

  var origFetch = window.fetch.bind(window);
  var MANAGED = /^\/Translate\/(models|runtime)\//;
  var progressCb = null;

  function pathOf(url) {
    var u;
    try { u = new URL(url, location.href); } catch (e) { return null; }
    if (u.origin !== location.origin) return null;
    return decodeURIComponent(u.pathname);
  }

  function serve(path) {
    return fetchFile(path, function (done, total, cached) {
      if (progressCb) progressCb(path, done, total, cached);
    }).then(function () {
      return assemble(path);
    }).then(function (bytes) {
      return new Response(bytes, {
        status: 200,
        headers: {
          'Content-Type': mimeFor(path),
          'Content-Length': String(bytes.length)
        }
      });
    });
  }

  /* Offline guard.
   *
   * There is no internet on a Nomad, so any request that leaves this origin is a bug
   * that presents as a hang until it times out. transformers.js is configured with
   * allowRemoteModels=false, but that is one library's setting and only covers model
   * files. This blocks the whole class at the fetch layer and says so loudly.
   */
  function isOffOrigin(url) {
    try {
      var u = new URL(url, location.href);
      return u.origin !== location.origin && /^https?:$/.test(u.protocol);
    } catch (e) { return false; }
  }

  window.fetch = function (input, init) {
    var url = (typeof input === 'string') ? input
            : (input && input.url) ? input.url : String(input);

    if (isOffOrigin(url)) {
      console.error('[xlate] BLOCKED off-device request to ' + url +
                    ' - a Nomad has no internet; everything must come off the card');
      return Promise.reject(new Error('offline: refused off-device request to ' + url));
    }

    // Never intercept our own range reads - that would recurse forever.
    if (url.indexOf(VIA) === 0 || url.indexOf('/media?file=') !== -1) {
      return origFetch(input, init);
    }
    var path = pathOf(url);
    if (!path || !MANAGED.test(path)) return origFetch(input, init);
    return serve(path);
  };

  console.log('[xlate] active - model files read from the card via ' + VIA +
              ' in ' + (CHUNK / 1024) + ' KiB chunks, ' + FILE_CONCURRENCY +
              ' files at a time, cached in IndexedDB. Off-device requests are blocked.');

  /* ---------------- public API ---------------- */

  window.NomadXlate = {
    CHUNK_BYTES: CHUNK,
    FILE_CONCURRENCY: FILE_CONCURRENCY,

    // diag.samePathViolations must be 0; anything else means the device is
    // being asked to serve one file from two places at once.
    diag: diag,

    onProgress: function (cb) { progressCb = cb; },

    // Pull a whole file set up front so the UI can show one honest progress bar,
    // instead of transformers.js pulling them mid-load with no feedback.
    //
    // Every file is started at once, the FILE_CONCURRENCY semaphore inside fetchFile
    // decides how many actually run. Progress is aggregated across all of them, and
    // `expected` lets the bar be truthful before the not-yet-started sizes are known
    // (otherwise the total keeps growing and the percentage walks backwards).
    preload: function (paths, cb, expected) {
      var sizes = {};
      paths.forEach(function (p) { sizes[p] = { done: 0, total: 0 }; });
      function tick(p) {
        if (!cb) return;
        var done = 0, total = 0;
        Object.keys(sizes).forEach(function (k) {
          done += sizes[k].done; total += sizes[k].total;
        });
        cb(p, done, Math.max(total, expected || 0));
      }
      return Promise.all(paths.map(function (p) {
        return fetchFile(p, function (done, total) {
          sizes[p] = { done: done, total: total };
          tick(p);
        });
      })).then(function () {});
    },

    // How much of a file set is already on this device, without downloading.
    //
    // `have` counts stored chunks of half-finished files too. counting only completed
    // files makes an interrupted download at 90% report zero, and the UI then offers a
    // full re-download instead of a resume.
    status: function (paths) {
      var out = { cached: 0, have: 0, known: 0, files: {} };
      var i = 0;
      function next() {
        if (i >= paths.length) return out;
        var p = paths[i++];
        return idbGet('meta', p).then(function (m) {
          out.files[p] = m || null;
          if (!m) return next();
          out.known += m.size;
          if (m.done) { out.cached += m.size; out.have += m.size; return next(); }
          return countChunks(p).then(function (n) {
            // last chunk may be short; close enough for a progress figure
            out.have += Math.min(n * m.chunkSize, m.size);
            return next();
          });
        });
      }
      return Promise.resolve().then(next);
    },

    clearCache: function () { return idbClear(); }
  };
})();
