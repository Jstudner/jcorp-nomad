/* nomad-zim.js - pure-JS ZIM reader for Nomad.
 * The ESP32 only serves raw byte ranges (same handler as movies), all the ZIM
 * smarts (header parse, title lookups, cluster decompress) run here in the browser.
 * Things that matter on-device:
 *  - reads go through an LRU block cache
 *  - one shared fetch queue so a search cant flood the ESP32 with sockets
 *  - 503 (SD busy / indexing) is retried, its an expected transient not an error
 *  - split ZIMs (.zimaa/.zimab, FAT32's 4GB limit) are handled by HttpSource
 *  - title search compares raw UTF-8 bytes (case handled by probing a few variants)
 * Runs in the browser (window.NomadZim) and Node (module.exports) so the shipped
 * code can be tested on a PC. Decompressors (xz/zstd) are injected by the host page. */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.NomadZim = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var ZIM_MAGIC = 0x44D495A;
  var COMP_NONE = 0, COMP_NONE2 = 1, COMP_XZ = 4, COMP_ZSTD = 5;
  // 8KB reads, kept ABOVE the firmware's 2048-byte probe threshold on purpose -
  // the probe path frees its heap buffer immediately and corrupts under rapid tiny
  // reads, so anything >2048 takes the safe streaming path instead
  var BLOCK_SIZE = 8192;
  var BLOCK_CACHE_MAX = 128;      // ~1MB of index blocks. big enough that one article's
                                  // whole lookup set stays cached, so later images need zero device reads
  var CLUSTER_CACHE_MAX = 4;      // decompressed clusters (can be ~2MB each)
  var DIRENT_CACHE_MAX = 512;
  var MAX_CLUSTER_BYTES = 96 * 1024 * 1024;  // big enough for a ZIM-embedded video/epub blob's cluster

  var textEncoder = new TextEncoder();
  var textDecoder = new TextDecoder('utf-8');

  /* ---------------- byte helpers ---------------- */

  function readU16(b, o) { return b[o] | (b[o + 1] << 8); }
  function readU32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }
  function readU64(b, o) { return readU32(b, o + 4) * 0x100000000 + readU32(b, o); }

  // Lexicographic compare of two Uint8Arrays (ZIM sort order).
  function cmpBytes(a, b) {
    var n = Math.min(a.length, b.length);
    for (var i = 0; i < n; i++) {
      if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return a.length === b.length ? 0 : (a.length < b.length ? -1 : 1);
  }

  function startsWithBytes(hay, prefix) {
    if (hay.length < prefix.length) return false;
    for (var i = 0; i < prefix.length; i++) if (hay[i] !== prefix[i]) return false;
    return true;
  }

  // NUL-terminated string starting at `start`; returns {str, bytes, end}.
  // end === -1 means no terminator inside the buffer (caller must re-read bigger).
  function readCStr(buf, start) {
    var end = start;
    while (end < buf.length && buf[end] !== 0) end++;
    if (end >= buf.length) return { str: null, bytes: null, end: -1 };
    var bytes = buf.slice(start, end);
    return { str: textDecoder.decode(bytes), bytes: bytes, end: end + 1 };
  }

  /* ---------------- shared fetch queue ---------------- */

  function FetchQueue(concurrency) {
    this.concurrency = concurrency || 2;
    this.active = 0;
    this.waiting = [];
  }
  FetchQueue.prototype.run = function (fn) {
    var self = this;
    return new Promise(function (resolve, reject) {
      var task = function () {
        self.active++;
        Promise.resolve().then(fn).then(
          function (v) { self.active--; self._next(); resolve(v); },
          function (e) { self.active--; self._next(); reject(e); }
        );
      };
      if (self.active < self.concurrency) task();
      else self.waiting.push(task);
    });
  };
  FetchQueue.prototype._next = function () {
    if (this.waiting.length && this.active < this.concurrency) {
      this.waiting.shift()();
    }
  };

  // concurrency 1 is REQUIRED - the firmware streams ZIM ranges from one reused file
  // handle, two in-flight requests seek it under each other and crash the device.
  // serialize every ZIM read
  var sharedQueue = new FetchQueue(1);

  /* ---------------- HTTP byte source (split-aware) ---------------- */

  // parts: [{path:'/Archive/foo.zimaa', size:123}, ...] in order.
  // opts.via: read prefix. on-device this MUST be '/media?file=' - fetching a .zim by
  // its own URL hits the whole-file fallback that ignores Range (streams the whole 2GB =
  // "loads forever"). pass '' only for a PC test server that honors Range directly.
  function HttpSource(parts, opts) {
    opts = opts || {};
    if (!parts || !parts.length) throw new Error('HttpSource: no parts');
    this.parts = parts;
    this.size = 0;
    for (var i = 0; i < parts.length; i++) this.size += parts[i].size;
    this.queue = opts.queue || sharedQueue;
    this.maxRetries = opts.maxRetries != null ? opts.maxRetries : 4;
    this.via = (opts.via != null) ? opts.via
             : (typeof NomadArchiveIndex !== 'undefined' && typeof NomadArchiveIndex.VIA === 'string')
               ? NomadArchiveIndex.VIA : '/media?file=';
  }

  HttpSource.prototype._fetchRange = function (path, start, end) {
    var self = this;
    var url = self.via ? self.via + encodeURIComponent(path) : path;
    var attempt = 0;
    var doFetch = function () {
      attempt++;
      var big = (end - start + 1) > 65536;
      var controller = (typeof AbortController !== 'undefined') ? new AbortController() : null;
      var timer = controller ? setTimeout(function () { controller.abort(); }, big ? 45000 : 12000) : null;
      return fetch(url, {
        headers: { 'Range': 'bytes=' + start + '-' + end },
        cache: 'no-store',
        signal: controller ? controller.signal : undefined
      }).then(function (resp) {
        if (timer) clearTimeout(timer);
        if (resp.status === 206 || resp.status === 200) {
          // a body that dies mid-read (device truncated it) is retryable like a 503
          return resp.arrayBuffer().then(
            function (ab) { return new Uint8Array(ab); },
            function (bodyErr) { bodyErr.retryable = true; throw bodyErr; }
          );
        }
        var retryable = resp.status === 503 || resp.status === 429;
        var err = new Error('Range ' + start + '-' + end + ' of ' + path + ' failed: HTTP ' + resp.status);
        err.retryable = retryable;
        throw err;
      }, function (netErr) {
        if (timer) clearTimeout(timer);
        netErr.retryable = true;
        throw netErr;
      }).catch(function (err) {
        if (err.retryable && attempt < self.maxRetries) {
          return new Promise(function (r) { setTimeout(r, 300 * attempt); }).then(doFetch);
        }
        throw err;
      });
    };
    return this.queue.run(doFetch);
  };

  // cap any one range request. a huge read keeps the ESP32 flat-out for seconds, so
  // split into sequential capped reads with a breather. chunks go forward-only in a
  // part so the reused file handle only seeks forward (cheap on FAT32)
  HttpSource.MAX_REQ_BYTES = 256 * 1024;
  HttpSource.CHUNK_BREATHER_MS = 25;

  HttpSource.prototype.read = function (offset, length) {
    var self = this;
    if (offset >= this.size) return Promise.resolve(new Uint8Array(0));
    if (offset + length > this.size) length = this.size - offset;

    // map the read onto parts, split anything over MAX_REQ_BYTES into capped requests
    var partStart = 0;
    var chunks = [];
    var CAP = HttpSource.MAX_REQ_BYTES;
    for (var i = 0; i < this.parts.length && length > 0; i++) {
      var p = this.parts[i];
      var partEnd = partStart + p.size;
      if (offset < partEnd && offset + length > partStart) {
        var local = Math.max(0, offset - partStart);
        var take = Math.min(p.size - local, offset + length - Math.max(offset, partStart));
        for (var at = local; at < local + take; at += CAP) {
          var end = Math.min(at + CAP, local + take) - 1;
          chunks.push({ path: p.path, start: at, end: end });
        }
      }
      partStart = partEnd;
    }
    if (chunks.length === 1) {
      return this._fetchRange(chunks[0].path, chunks[0].start, chunks[0].end);
    }
    // Multi-chunk read: fetch sequentially (breather between chunks) and stitch.
    var out = new Uint8Array(length);
    var written = 0;
    var chain = Promise.resolve();
    chunks.forEach(function (s, idx) {
      chain = chain.then(function () {
        var pause = idx > 0
          ? new Promise(function (r) { setTimeout(r, HttpSource.CHUNK_BREATHER_MS); })
          : Promise.resolve();
        return pause.then(function () {
          return self._fetchRange(s.path, s.start, s.end);
        }).then(function (chunk) {
          out.set(chunk, written);
          written += chunk.length;
        });
      });
    });
    return chain.then(function () { return written === length ? out : out.slice(0, written); });
  };

  /* ---------------- small LRU map ---------------- */

  function lruSet(map, key, value, max) {
    if (map.has(key)) map.delete(key);
    map.set(key, value);
    if (map.size > max) map.delete(map.keys().next().value);
  }
  function lruGet(map, key) {
    if (!map.has(key)) return undefined;
    var v = map.get(key);
    map.delete(key);
    map.set(key, v);
    return v;
  }

  /* ---------------- ZimArchive ---------------- */

  // source: {size, read(offset,len)->Promise<Uint8Array>}
  // opts.decompressors: { xz: (u8)->Promise<u8>|u8, zstd: (u8)->Promise<u8>|u8 }
  function ZimArchive(source, opts) {
    opts = opts || {};
    this.src = source;
    this.decompressors = opts.decompressors || {};
    this.header = null;
    this.mimeTypes = [];
    this._blocks = new Map();     // aligned block cache
    this._dirents = new Map();    // dirent-by-offset cache
    this._clusters = new Map();   // decompressed cluster cache
    this._titleIndex = null;      // {mode:'v1-raw'|'v1-mem'|'header', ...}
    this._inflightBlocks = new Map();
  }

  /* ----- cached small reads ----- */

  // Read `length` bytes at `offset` through the aligned block cache.
  // Only for small index-ish reads; clusters go direct.
  ZimArchive.prototype._readAt = function (offset, length) {
    var self = this;
    var firstBlock = Math.floor(offset / BLOCK_SIZE);
    var lastBlock = Math.floor((offset + length - 1) / BLOCK_SIZE);
    var blockNums = [];
    for (var b = firstBlock; b <= lastBlock; b++) blockNums.push(b);
    return Promise.all(blockNums.map(function (bn) { return self._getBlock(bn); }))
      .then(function (blocks) {
        var out = new Uint8Array(length);
        var written = 0;
        for (var i = 0; i < blocks.length; i++) {
          var bn = blockNums[i];
          var blockBase = bn * BLOCK_SIZE;
          var from = Math.max(offset, blockBase) - blockBase;
          var to = Math.min(offset + length, blockBase + BLOCK_SIZE) - blockBase;
          var slice = blocks[i].subarray(from, Math.min(to, blocks[i].length));
          out.set(slice, written);
          written += slice.length;
          if (slice.length < to - from) break; // hit EOF
        }
        return written === length ? out : out.slice(0, written);
      });
  };

  ZimArchive.prototype._getBlock = function (blockNum) {
    var cached = lruGet(this._blocks, blockNum);
    if (cached) return Promise.resolve(cached);
    var inflight = this._inflightBlocks.get(blockNum);
    if (inflight) return inflight;
    var self = this;
    var p = this.src.read(blockNum * BLOCK_SIZE, BLOCK_SIZE).then(function (buf) {
      self._inflightBlocks.delete(blockNum);
      lruSet(self._blocks, blockNum, buf, BLOCK_CACHE_MAX);
      return buf;
    }, function (err) {
      self._inflightBlocks.delete(blockNum);
      throw err;
    });
    this._inflightBlocks.set(blockNum, p);
    return p;
  };

  /* ----- open / header ----- */

  // full open: header + mime list + title index. needed for live title search.
  // on-device we avoid this for big ZIMs, see openHeaderOnly() below
  ZimArchive.prototype.open = function () {
    var self = this;
    return this.openHeaderOnly().then(function () {
      return self._readMimeTypes();
    }).then(function () {
      return self._initTitleIndex();
    }).then(function () {
      return self.header;
    });
  };

  // header-only open: one 80-byte read, enough to fetch an article by (cluster, blob)
  // via getContentAt(). the cheap path the preindexed reader uses - it already knows
  // where the article is, so opening one only touches the ZIM for the cluster read
  ZimArchive.prototype.openHeaderOnly = function () {
    var self = this;
    if (self.header) return Promise.resolve(self.header);
    return this._readAt(0, 80).then(function (buf) {
      if (buf.length < 80 || readU32(buf, 0) !== ZIM_MAGIC) {
        throw new Error('Not a ZIM file (bad magic)');
      }
      self.header = {
        majorVersion: readU16(buf, 4),
        minorVersion: readU16(buf, 6),
        entryCount: readU32(buf, 24),
        clusterCount: readU32(buf, 28),
        urlPtrPos: readU64(buf, 32),
        titlePtrPos: readU64(buf, 40),
        clusterPtrPos: readU64(buf, 48),
        mimeListPos: readU64(buf, 56),
        mainPage: readU32(buf, 64),
        layoutPage: readU32(buf, 68),
        checksumPos: readU64(buf, 72)
      };
      return self.header;
    });
  };

  ZimArchive.prototype._readMimeTypes = function () {
    var self = this;
    return this._readAt(this.header.mimeListPos, 2048).then(function (buf) {
      self.mimeTypes = [];
      var pos = 0;
      while (pos < buf.length) {
        var r = readCStr(buf, pos);
        if (r.end < 0 || r.str === '') break; // empty string terminates the list
        self.mimeTypes.push(r.str);
        pos = r.end;
      }
    });
  };

  /* ----- dirents ----- */

  ZimArchive.prototype._readDirent = function (offset) {
    var cached = lruGet(this._dirents, offset);
    if (cached) return Promise.resolve(cached);
    var self = this;
    var parse = function (buf) {
      var mimeIdx = readU16(buf, 0);
      var isRedirect = mimeIdx === 0xFFFF;
      var base = isRedirect ? 12 : 16;
      var url = readCStr(buf, base);
      if (url.end < 0) return null;
      var title = readCStr(buf, url.end);
      if (title.end < 0) return null;
      var e = {
        offset: offset,
        mimeIdx: mimeIdx,
        namespace: String.fromCharCode(buf[3]),
        isRedirect: isRedirect,
        url: url.str,
        urlBytes: url.bytes,
        title: title.str !== '' ? title.str : url.str,
        titleBytes: title.bytes.length ? title.bytes : url.bytes
      };
      if (isRedirect) {
        e.redirectTarget = readU32(buf, 8);
      } else {
        e.clusterNumber = readU32(buf, 8);
        e.blobNumber = readU32(buf, 12);
        e.mimeType = self.mimeTypes[mimeIdx] || 'application/octet-stream';
      }
      return e;
    };
    return this._readAt(offset, 512).then(function (buf) {
      var e = parse(buf);
      if (e) return e;
      // Very long url+title: retry with a bigger window.
      return self._readAt(offset, 2048).then(function (big) {
        var e2 = parse(big);
        if (!e2) throw new Error('Unparseable dirent at ' + offset);
        return e2;
      });
    }).then(function (e) {
      lruSet(self._dirents, offset, e, DIRENT_CACHE_MAX);
      return e;
    });
  };

  ZimArchive.prototype._direntByUrlIndex = function (i) {
    var self = this;
    return this._readAt(this.header.urlPtrPos + i * 8, 8).then(function (buf) {
      return self._readDirent(readU64(buf, 0));
    });
  };

  ZimArchive.prototype.resolveRedirect = function (entry, depth) {
    depth = depth || 0;
    if (!entry.isRedirect || depth > 8) return Promise.resolve(entry);
    var self = this;
    return this._direntByUrlIndex(entry.redirectTarget).then(function (t) {
      return self.resolveRedirect(t, depth + 1);
    });
  };

  /* ----- URL lookup (binary search over URL pointer list) ----- */

  // findByUrl('C', 'Albert_Einstein') -> dirent or null
  ZimArchive.prototype.findByUrl = function (ns, url) {
    var self = this;
    var nsCode = ns.charCodeAt(0);
    var urlBytes = textEncoder.encode(url);
    var lo = 0, hi = this.header.entryCount - 1;
    var step = function () {
      if (lo > hi) return null;
      var mid = (lo + hi) >> 1;
      return self._direntByUrlIndex(mid).then(function (e) {
        var eNs = e.namespace.charCodeAt(0);
        var c = eNs < nsCode ? -1 : eNs > nsCode ? 1 : cmpBytes(e.urlBytes, urlBytes);
        if (c === 0) return e;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
        return step();
      });
    };
    return step();
  };

  /* ----- blob / cluster access ----- */

  // Locate a blob's raw file position if (and only if) its cluster is
  // uncompressed. Returns {offset, size} or null if the cluster is compressed.
  ZimArchive.prototype._locateRawBlob = function (clusterNumber, blobNumber) {
    var self = this;
    return this._clusterExtent(clusterNumber).then(function (ext) {
      return self._readAt(ext.offset, 1).then(function (cb) {
        var comp = cb[0] & 0x0F;
        if (comp !== COMP_NONE && comp !== COMP_NONE2) return null;
        var offsetSize = (cb[0] & 0x10) ? 8 : 4;
        var tableAt = ext.offset + 1 + blobNumber * offsetSize;
        return self._readAt(tableAt, offsetSize * 2).then(function (tb) {
          var start = offsetSize === 8 ? readU64(tb, 0) : readU32(tb, 0);
          var end = offsetSize === 8 ? readU64(tb, 8) : readU32(tb, 4);
          return { offset: ext.offset + 1 + start, size: end - start };
        });
      });
    });
  };

  ZimArchive.prototype._clusterExtent = function (clusterNumber) {
    var self = this;
    return this._readAt(this.header.clusterPtrPos + clusterNumber * 8, 16).then(function (buf) {
      var off = readU64(buf, 0);
      var next = (clusterNumber + 1 < self.header.clusterCount)
        ? readU64(buf, 8)
        : (self.header.checksumPos || self.src.size);
      return { offset: off, size: next - off };
    });
  };

  ZimArchive.prototype._readCluster = function (clusterNumber) {
    var cached = lruGet(this._clusters, clusterNumber);
    if (cached) return Promise.resolve(cached);
    var self = this;
    return this._clusterExtent(clusterNumber).then(function (ext) {
      if (ext.size > MAX_CLUSTER_BYTES) throw new Error('Cluster too large: ' + ext.size);
      return self.src.read(ext.offset, ext.size);
    }).then(function (raw) {
      var compByte = raw[0];
      var comp = compByte & 0x0F;
      var offsetSize = (compByte & 0x10) ? 8 : 4;
      var body = raw.subarray(1);
      var dataP;
      if (comp === COMP_NONE || comp === COMP_NONE2) {
        dataP = Promise.resolve(body);
      } else if (comp === COMP_XZ) {
        if (!self.decompressors.xz) throw new Error('XZ decompressor not available');
        dataP = Promise.resolve(self.decompressors.xz(body));
      } else if (comp === COMP_ZSTD) {
        if (!self.decompressors.zstd) throw new Error('Zstd decompressor not available');
        dataP = Promise.resolve(self.decompressors.zstd(body));
      } else {
        throw new Error('Unknown cluster compression: ' + comp);
      }
      return dataP.then(function (data) {
        var result = { data: data, offsetSize: offsetSize };
        lruSet(self._clusters, clusterNumber, result, CLUSTER_CACHE_MAX);
        return result;
      });
    });
  };

  ZimArchive.prototype.readBlob = function (clusterNumber, blobNumber) {
    var self = this;
    var fromCluster = function (cl) {
      var os = cl.offsetSize;
      var start, end;
      if (os === 8) {
        start = readU64(cl.data, blobNumber * 8);
        end = readU64(cl.data, (blobNumber + 1) * 8);
      } else {
        start = readU32(cl.data, blobNumber * 4);
        end = readU32(cl.data, (blobNumber + 1) * 4);
      }
      return cl.data.slice(start, end);
    };
    // Cluster already decompressed and cached: slice it for free.
    var cached = lruGet(this._clusters, clusterNumber);
    if (cached) return Promise.resolve(fromCluster(cached));
    // uncompressed cluster: read ONLY this blob's bytes. images live in uncompressed
    // ~2MB clusters, pulling the whole thing for a ~10KB thumb was ~200x the data and
    // rebooted the ESP32. compressed clusters (text) still need the full extent (small)
    return this._locateRawBlob(clusterNumber, blobNumber).then(function (raw) {
      if (raw) return self.src.read(raw.offset, raw.size);
      return self._readCluster(clusterNumber).then(fromCluster);
    });
  };

  // entry (possibly redirect) -> {data, mimeType, entry}
  ZimArchive.prototype.getContent = function (entry) {
    var self = this;
    return this.resolveRedirect(entry).then(function (e) {
      if (e.isRedirect) throw new Error('Unresolvable redirect: ' + entry.url);
      return self.readBlob(e.clusterNumber, e.blobNumber).then(function (data) {
        return { data: data, mimeType: e.mimeType, entry: e };
      });
    });
  };

  // read an article straight from a known cluster/blob - no lookup, no binary search.
  // the preindexed search stores (cluster, blob) per article, so opening one is just a
  // pointer read + cluster read + decompress. the ZIM is touched only for that article
  ZimArchive.prototype.getContentAt = function (clusterNumber, blobNumber, mimeType) {
    return this.readBlob(clusterNumber, blobNumber).then(function (data) {
      return { data: data, mimeType: mimeType || 'text/html', entry: null };
    });
  };

  /* ----- title index ----- */

  // prefer X/listing/titleOrdered/v1 (modern ZIMs, random-access in place if
  // uncompressed), else fall back to the header title pointer list
  ZimArchive.prototype._initTitleIndex = function () {
    var self = this;
    return this.findByUrl('X', 'listing/titleOrdered/v1').then(function (e) {
      if (!e) return null;
      return self.resolveRedirect(e).then(function (r) {
        if (r.isRedirect) return null;
        return self._locateRawBlob(r.clusterNumber, r.blobNumber).then(function (raw) {
          if (raw) {
            return { mode: 'v1-raw', offset: raw.offset, count: Math.floor(raw.size / 4) };
          }
          return self.readBlob(r.clusterNumber, r.blobNumber).then(function (data) {
            return { mode: 'v1-mem', data: data, count: Math.floor(data.length / 4) };
          });
        });
      });
    }).then(function (idx) {
      if (idx) { self._titleIndex = idx; return; }
      self._titleIndex = self._headerTitleIndex();
    }).catch(function () {
      self._titleIndex = self._headerTitleIndex();
    });
  };

  // Header title pointer list fallback (pre-v1-listing ZIMs). Modern files
  // set titlePtrPos to 0xFFFFFFFFFFFFFFFF ("absent"); guard against using it.
  ZimArchive.prototype._headerTitleIndex = function () {
    var p = this.header.titlePtrPos;
    if (!p || p + this.header.entryCount * 4 > this.src.size) {
      return { mode: 'none', count: 0 };
    }
    return { mode: 'header', offset: p, count: this.header.entryCount };
  };

  // i-th entry (in title order) -> dirent
  ZimArchive.prototype._direntByTitleIndex = function (i) {
    var self = this;
    var ti = this._titleIndex;
    if (ti.mode === 'v1-mem') {
      return this._direntByUrlIndex(readU32(ti.data, i * 4));
    }
    return this._readAt(ti.offset + i * 4, 4).then(function (buf) {
      return self._direntByUrlIndex(readU32(buf, 0));
    });
  };

  // Sort key of the i-th title-ordered entry, as bytes.
  // v1 lists sort by title alone; the header list sorts by (ns, title).
  ZimArchive.prototype._titleKeyAt = function (i) {
    var headerMode = this._titleIndex.mode === 'header';
    return this._direntByTitleIndex(i).then(function (e) {
      if (!headerMode) return { key: e.titleBytes, entry: e };
      var key = new Uint8Array(e.titleBytes.length + 1);
      key[0] = e.namespace.charCodeAt(0);
      key.set(e.titleBytes, 1);
      return { key: key, entry: e };
    });
  };

  // First index whose title-key >= target (classic lower bound).
  ZimArchive.prototype._titleLowerBound = function (targetBytes) {
    var self = this;
    var lo = 0, hi = this._titleIndex.count;
    var step = function () {
      if (lo >= hi) return lo;
      var mid = (lo + hi) >> 1;
      return self._titleKeyAt(mid).then(function (r) {
        if (cmpBytes(r.key, targetBytes) < 0) lo = mid + 1; else hi = mid;
        return step();
      });
    };
    return step();
  };

  // Case variants to probe, since ZIM title order is case-sensitive.
  function caseVariants(q) {
    var out = [];
    var push = function (s) { if (s && out.indexOf(s) < 0) out.push(s); };
    push(q);
    push(q.charAt(0).toUpperCase() + q.slice(1));
    push(q.charAt(0).toUpperCase() + q.slice(1).toLowerCase());
    // Title Case Every Word ("albert einstein" -> "Albert Einstein")
    push(q.replace(/(^|\s)(\S)/g, function (m, sp, c) { return sp + c.toUpperCase(); }).replace(/(\S)(\S*)/g, function (m, h, t) { return h + t.toLowerCase(); }));
    push(q.toLowerCase());
    push(q.toUpperCase());
    return out;
  }

  // only tiny archives (e.g. a 7-video TED collection) get a full in-memory title
  // table - it's one dirent read PER title, way too slow over the network for anything
  // bigger. keep this low, raising it brings back multi-second scans
  var SMALL_ARCHIVE_MAX_TITLES = 256;

  ZimArchive.prototype.isSmall = function () {
    return this._titleIndex && this._titleIndex.mode !== 'none' &&
           this._titleIndex.count <= SMALL_ARCHIVE_MAX_TITLES;
  };

  // loads the full title table (tiny archives only). fires reads together so the
  // queue pipelines them instead of one round trip per title. cached, safe to recall
  ZimArchive.prototype._loadAllTitles = function () {
    if (this._allTitles) return this._allTitles;
    var self = this;
    var n = this._titleIndex.count;
    var idxs = [];
    for (var i = 0; i < n; i++) idxs.push(i);
    this._allTitles = Promise.all(idxs.map(function (i) {
      return self._direntByTitleIndex(i).then(function (e) {
        return { title: e.title, url: e.url, namespace: e.namespace, entry: e };
      });
    })).catch(function (err) {
      self._allTitles = null; // allow retry after transient failures
      throw err;
    });
    return this._allTitles;
  };

  // pre-warm hook: for tiny archives pull the substring table now, off the search path.
  // no-op for large archives
  ZimArchive.prototype.prewarm = function () {
    if (this.isSmall()) return this._loadAllTitles().catch(function () {});
    return Promise.resolve();
  };

  // Title prefix search. Returns [{title, url, namespace, entry}].
  // scanCap bounds the forward scan per variant so a miss stays cheap.
  ZimArchive.prototype.suggest = function (query, limit, scanCap) {
    limit = limit || 8;
    scanCap = scanCap || 48;
    var self = this;
    var q = (query || '').trim();
    if (!q || !this.header) return Promise.resolve([]);
    if (this._titleIndex.mode === 'none') return Promise.resolve([]);

    if (this.isSmall()) {
      var needle = q.toLowerCase();
      return this._loadAllTitles().then(function (all) {
        var starts = [], contains = [];
        for (var i = 0; i < all.length; i++) {
          var t = all[i];
          if (t.namespace !== 'A' && t.namespace !== 'C') continue;
          var lt = t.title.toLowerCase();
          var idx = lt.indexOf(needle);
          if (idx === 0) starts.push(t);
          else if (idx > 0) contains.push(t);
          if (starts.length >= limit) break;
        }
        return starts.concat(contains).slice(0, limit);
      });
    }

    var headerMode = this._titleIndex.mode === 'header';
    // In header-list mode there is a namespace byte in front of each key.
    // 'A' = old-namespace articles, 'C' = new shared content namespace.
    var nsPrefixes = headerMode ? ['A', 'C'] : [''];

    var results = [];
    var seen = {};
    var variants = caseVariants(q);

    var runProbe = function (vi, ni) {
      if (results.length >= limit) return Promise.resolve();
      if (vi >= variants.length) return Promise.resolve();
      if (ni >= nsPrefixes.length) return runProbe(vi + 1, 0);
      var prefixBytes = textEncoder.encode(nsPrefixes[ni] + variants[vi]);
      return self._titleLowerBound(prefixBytes).then(function (idx) {
        var scanned = 0;
        var scan = function (i) {
          if (i >= self._titleIndex.count || results.length >= limit || scanned >= scanCap) {
            return Promise.resolve();
          }
          scanned++;
          return self._titleKeyAt(i).then(function (r) {
            if (!startsWithBytes(r.key, prefixBytes)) return Promise.resolve(); // left the prefix range
            var e = r.entry;
            // Header mode walks all namespaces; only article-ish ones matter.
            if (!headerMode || e.namespace === 'A' || e.namespace === 'C') {
              var k = e.namespace + '/' + e.url;
              if (!seen[k]) {
                seen[k] = true;
                results.push({ title: e.title, url: e.url, namespace: e.namespace, entry: e });
              }
            }
            return scan(i + 1);
          });
        };
        return scan(idx);
      }).then(function () { return runProbe(vi, ni + 1); });
    };

    return runProbe(0, 0).then(function () { return results; });
  };

  /* ----- convenience ----- */

  ZimArchive.prototype.getMainPage = function () {
    var self = this;
    if (this.header.mainPage !== 0xFFFFFFFF) {
      return this._direntByUrlIndex(this.header.mainPage).then(function (e) {
        return self.resolveRedirect(e);
      });
    }
    return this.findByUrl('W', 'mainPage').then(function (e) {
      if (e) return self.resolveRedirect(e);
      return self.findByUrl('C', 'index.html');
    });
  };

  // Metadata entries live in namespace 'M' (Title, Description, Language...).
  ZimArchive.prototype.getMetadata = function (name) {
    var self = this;
    return this.findByUrl('M', name).then(function (e) {
      if (!e) return null;
      return self.getContent(e).then(function (c) {
        return textDecoder.decode(c.data);
      });
    }).catch(function () { return null; });
  };

  // The standard 48x48 card icon, as a blob URL (browser) or bytes (node).
  ZimArchive.prototype.getIllustration = function () {
    var self = this;
    return this.findByUrl('M', 'Illustration_48x48@1').then(function (e) {
      if (!e) return null;
      return self.getContent(e).then(function (c) { return c; });
    }).catch(function () { return null; });
  };

  // Find an entry by its full path like 'C/Albert_Einstein' or 'A/foo.html'.
  ZimArchive.prototype.getEntryByPath = function (path) {
    var slash = path.indexOf('/');
    if (slash < 1) return this.findByUrl('C', path);
    return this.findByUrl(path.substring(0, slash), path.substring(slash + 1));
  };

  /* ---------------- archive list helper ---------------- */

  // Fetches the device's advanced-content manifest. Returns [] on failure.
  function fetchArchiveList(url) {
    return fetch(url || '/api/archive-list', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (j) {
        if (j && Array.isArray(j.archives)) return j.archives;
        if (Array.isArray(j)) return j;
        return [];
      })
      .catch(function () { return []; });
  }

  /* ---------------- preindexed search (NomadArchiveIndex) ---------------- */
  //
  // searches the sidecar index built on a PC by nomad-manager's zim_indexer.js.
  // the device never binary-searches a multi-GB ZIM - a query is the skip table
  // (fetched once, in RAM) + ONE small range read of words.dat. opening a result
  // reads only that article's cluster.
  //   words.dat : sorted  word \t title \t archiveIdx \t cluster \t blob
  //   words.skp : { step, keys: [[word, byteOffset], ...] }
  //   manifest.json : { version, archives:[...] }

  function NomadArchiveIndex(baseUrl) {
    this.base = baseUrl || '/Archive/.nomad-zim';
    // route index reads through /media?file= (honors Range). fetching words.dat by
    // its own URL hits the whole-file fallback that ignores Range = minutes-long hang
    this.via = (typeof NomadArchiveIndex.VIA === 'string') ? NomadArchiveIndex.VIA : '/media?file=';
    this.manifest = null;
    this.skip = null;
    this._loadP = null;
  }

  NomadArchiveIndex.prototype._url = function (name) {
    var p = this.base + '/' + name;
    return this.via ? this.via + encodeURIComponent(p) : p;
  };

  // fetch a URL under one timeout - the timer has to survive until the BODY is read,
  // or a huge streamed body never trips it. returns { ok, status, text(), json() }
  NomadArchiveIndex.prototype._fetch = function (url, range, timeoutMs) {
    // same sharedQueue as ZIM reads so a search and an image are never in flight at
    // once - the firmware keeps one archive handle and they'd fight over it
    return sharedQueue.run(function () {
      var ctl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
      var timer = ctl ? setTimeout(function () { ctl.abort(); }, timeoutMs || 20000) : null;
      var opts = { cache: 'no-store', signal: ctl ? ctl.signal : undefined };
      if (range) opts.headers = { 'Range': range };
      return fetch(url, opts).then(function (r) {
        return r.text().then(function (t) {           // read body under the same timeout
          if (timer) clearTimeout(timer);
          return { ok: r.ok, status: r.status, body: t };
        });
      }, function (e) { if (timer) clearTimeout(timer); throw e; });
    });
  };

  NomadArchiveIndex.prototype.load = function () {
    if (this._loadP) return this._loadP;
    var self = this;
    var parse = function (res) { try { return (res.ok || res.status === 200) ? JSON.parse(res.body) : null; } catch (e) { return null; } };
    // sharded (v2, full Wikipedia): manifest only, shards fetched lazily so we never
    // hold a multi-MB skip table in RAM. legacy (v1): grab the one words.skp up front
    this._shardCache = {};
    this._loadP = self._fetch(self._url('manifest.json')).then(parse).then(function (mani) {
      self.manifest = mani;
      if (!self.manifest) throw new Error('archive index not found');
      self.sharded = !!self.manifest.sharded;
      if (self.sharded) return null;
      return self._fetch(self._url('words.skp')).then(parse);
    }).then(function (skp) {
      if (!self.sharded) {
        self.skip = skp;
        if (!self.skip || !self.skip.keys) throw new Error('archive index not found');
      }
      return self;
    }).catch(function (e) {
      self._loadP = null; // allow retry
      throw e;
    });
    return this._loadP;
  };

  NomadArchiveIndex.prototype.isReady = function () { return !!(this.manifest && (this.sharded || this.skip)); };
  NomadArchiveIndex.prototype.archive = function (idx) {
    return this.manifest && this.manifest.archives ? this.manifest.archives[idx] : null;
  };

  // resolve a word's shard (its first char) to a { keys, datUrl, datSize } handle.
  // sharded = lazily fetch/cache words-<c>.skp; legacy = one virtual shard over the whole
  // words.dat so both formats share the same scanning code. null if no such shard
  NomadArchiveIndex.prototype._getShard = function (ch) {
    var self = this;
    if (!this.sharded) {
      return Promise.resolve({ keys: (self.skip && self.skip.keys) || [], datUrl: self._url('words.dat'), datSize: (self.manifest && self.manifest.wordsBytes) || 0 });
    }
    var sh = self.manifest.shards && self.manifest.shards[ch];
    if (!sh) return Promise.resolve(null);
    if (self._shardCache[ch]) return self._shardCache[ch];
    var parse = function (res) { try { return (res.ok || res.status === 200) ? JSON.parse(res.body) : null; } catch (e) { return null; } };
    var p = self._fetch(self._url(sh.skp)).then(parse).then(function (skp) {
      if (!skp || !skp.keys) throw new Error('shard skip not found: ' + ch);
      return { keys: skp.keys, datUrl: self._url(sh.dat), datSize: sh.bytes || 0 };
    });
    self._shardCache[ch] = p;
    p.catch(function () { delete self._shardCache[ch]; });
    return p;
  };

  // Ordered shard alphabet (matches the indexer's LC_ALL=C sort: digits < letters).
  var SHARD_ALPHABET = '0123456789abcdefghijklmnopqrstuvwxyz';

  // Largest skip key <= target within a given shard's keys (binary search in RAM).
  NomadArchiveIndex.prototype._floorIn = function (ks, target) {
    var lo = 0, hi = ks.length - 1, at = 0;
    while (lo <= hi) {
      var m = (lo + hi) >> 1;
      if (ks[m][0] <= target) { at = m; lo = m + 1; } else hi = m - 1;
    }
    return at;
  };

  // query -> [{title, archiveId, archiveIdx, cluster, blob}]. matches the first word
  // as a sorted prefix, then for multi-word queries keeps titles containing all words
  NomadArchiveIndex.prototype.search = function (query, limit) {
    limit = limit || 12;
    var self = this;
    var q = (query || '').trim().toLowerCase();
    if (q.length < 2) return Promise.resolve([]);
    var qWords = q.split(/[^a-z0-9]+/).filter(function (w) { return w.length >= 2; });
    if (!qWords.length) return Promise.resolve([]);
    // single word matches that word; multi-word matches the full query as a title
    // prefix, so "world war" hits "world war ii" not any title with "war"
    var multi = qWords.length > 1;
    var normQuery = qWords.join(' ');
    var primary = multi ? normQuery : qWords[0];

    // pull the prefix range into a candidate pool then rank (exact, starts-with, shortest).
    // each chunk is scanned fully before reading more so a late exact match isnt cut off.
    // CAP bounds a very common prefix
    var MAX_WINDOWS = 4;
    var CAP = 3000;

    return this.load().then(function () {
      // anything starting with the query's first char lives in one shard, so a
      // search only touches that shard
      return self._getShard(primary.charAt(0));
    }).then(function (shard) {
      if (!shard || !shard.keys.length) return [];
      var ks = shard.keys;
      var at = self._floorIn(ks, primary);
      var datSize = shard.datSize;
      var cands = [];

      function readWindow(w, budget) {
        if (w >= ks.length || budget <= 0 || cands.length >= CAP) return Promise.resolve();
        var start = ks[w][1];
        var end = (w + 1 < ks.length) ? ks[w + 1][1] : datSize;
        if (end <= start) return Promise.resolve();
        return self._fetch(shard.datUrl, 'bytes=' + start + '-' + (end - 1), 20000).then(function (r) {
          return (r.ok || r.status === 206 || r.status === 200) ? r.body : '';
        }).then(function (text) {
          var lines = text.split('\n');
          var passedPrefix = false;
          for (var i = 0; i < lines.length; i++) {
            var line = lines[i];
            if (!line) continue;
            var f = line.split('\t');
            var word = f[0];
            if (!word) continue;
            if (word < primary) continue;                 // not there yet
            if (word.indexOf(primary) !== 0) { passedPrefix = true; break; } // past the prefix
            // keep all matches, dedup AFTER ranking - redirects point at the same
            // article and we want the best title to win, not whichever sorts first
            cands.push({ title: f[1], archiveIdx: +f[2], cluster: +f[3], blob: +f[4] });
            if (cands.length >= CAP) return;
          }
          if (!passedPrefix) return readWindow(w + 1, budget - 1);
        });
      }

      // Score: exact title (0) < starts-with-query (1) < contains-word (2),
      // then shorter title. Lower is better.
      var scoreOf = function (c) {
        var t = c.title.toLowerCase();
        var s = (t === q) ? 0 : (t.indexOf(q) === 0 ? 1 : 2);
        return s * 100000 + Math.min(99999, c.title.length);
      };

      return readWindow(at, MAX_WINDOWS).then(function () {
        // Group by archive, rank + dedup (best title per article) within each.
        var groups = {};
        for (var i = 0; i < cands.length; i++) {
          var c = cands[i];
          (groups[c.archiveIdx] = groups[c.archiveIdx] || []).push(c);
        }
        var glist = [];
        Object.keys(groups).forEach(function (aIdx) {
          var list = groups[aIdx];
          list.sort(function (a, b) { return scoreOf(a) - scoreOf(b); });
          var seen = {}, deduped = [];
          for (var j = 0; j < list.length; j++) {
            var key = list[j].cluster + '|' + list[j].blob;
            if (seen[key]) continue;
            seen[key] = true;
            deduped.push(list[j]);
          }
          glist.push({ aIdx: +aIdx, list: deduped });
        });
        // Order libraries by how good their best hit is (most relevant leads).
        glist.sort(function (a, b) { return scoreOf(a.list[0]) - scoreOf(b.list[0]); });

        // Round-robin across libraries so EVERY library with a match is
        // represented - otherwise Wikipedia's size buries the smaller ZIMs.
        var out = [], pos = {}, remaining = true;
        while (out.length < limit && remaining) {
          remaining = false;
          for (var g = 0; g < glist.length && out.length < limit; g++) {
            var grp = glist[g], p = pos[grp.aIdx] || 0;
            if (p < grp.list.length) {
              var c2 = grp.list[p];
              var arc = self.archive(c2.archiveIdx);
              out.push({ title: c2.title, archiveIdx: c2.archiveIdx,
                         archiveId: arc ? arc.id : String(c2.archiveIdx),
                         cluster: c2.cluster, blob: c2.blob });
              pos[grp.aIdx] = p + 1;
              if (pos[grp.aIdx] < grp.list.length) remaining = true;
            }
          }
        }
        return out;
      });
    });
  };

  function normKey(s) { return (s || '').toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim(); }

  // list every article whose title is in [from, to], optionally one archive. backs the
  // ZIM "index" pages (Wikipedia's "A - Aeolic Greek" alphabetical bands). uses canonical
  // title rows (word key == normalized title) so it returns real titles, not word matches.
  // opts: {archiveIdx, limit}
  NomadArchiveIndex.prototype.listRange = function (from, to, opts) {
    opts = opts || {};
    var limit = opts.limit || 200;
    var self = this;
    var lo = normKey(from), hi = normKey(to);
    if (!lo) return Promise.resolve([]);
    if (hi && hi < lo) { var t = lo; lo = hi; hi = t; }
    var wantArc = (opts.archiveIdx != null) ? +opts.archiveIdx : null;

    var out = [], seen = {};

    // Scan one shard for canonical-title rows whose key is in [lo, hi].
    function scanShard(shard) {
      var ks = shard.keys, datSize = shard.datSize, MAX_WINDOWS = 60;
      var at = self._floorIn(ks, lo);
      function readWindow(w, budget) {
        if (w >= ks.length || budget <= 0 || out.length >= limit) return Promise.resolve();
        var start = ks[w][1], end = (w + 1 < ks.length) ? ks[w + 1][1] : datSize;
        if (end <= start) return Promise.resolve();
        return self._fetch(shard.datUrl, 'bytes=' + start + '-' + (end - 1), 20000).then(function (r) {
          return (r.ok || r.status === 206 || r.status === 200) ? r.body : '';
        }).then(function (text) {
          var lines = text.split('\n'), pastEnd = false;
          for (var i = 0; i < lines.length; i++) {
            var line = lines[i]; if (!line) continue;
            var f = line.split('\t'); var key = f[0]; if (!key) continue;
            if (key < lo) continue;
            if (hi && key > hi) { pastEnd = true; break; }
            if (wantArc != null && +f[2] !== wantArc) continue;
            if (normKey(f[1]) !== key) continue;         // canonical title rows only
            var dk = f[2] + '|' + f[3] + '|' + f[4];
            if (seen[dk]) continue; seen[dk] = true;
            var arc = self.archive(+f[2]);
            out.push({ title: f[1], archiveIdx: +f[2], archiveId: arc ? arc.id : String(f[2]), cluster: +f[3], blob: +f[4] });
            if (out.length >= limit) return;
          }
          if (!pastEnd) return readWindow(w + 1, budget - 1);
        });
      }
      return readWindow(at, MAX_WINDOWS);
    }

    return this.load().then(function () {
      // the band can cross first-char boundaries, so walk every shard from lo's char
      // to hi's (legacy index has one virtual shard so the loop just runs once)
      var loc = lo.charAt(0), hic = (hi ? hi.charAt(0) : loc);
      var chars;
      if (!self.sharded) { chars = [loc]; }
      else {
        chars = [];
        for (var i = 0; i < SHARD_ALPHABET.length; i++) {
          var c = SHARD_ALPHABET[i];
          if (c >= loc && c <= hic) chars.push(c);
        }
      }
      var idx = 0;
      function nextShard() {
        if (idx >= chars.length || out.length >= limit) return Promise.resolve();
        return self._getShard(chars[idx++]).then(function (shard) {
          if (shard && shard.keys.length) return scanShard(shard);
        }).then(nextShard);
      }
      return nextShard();
    }).then(function () {
      out.sort(function (a, b) { return normKey(a.title) < normKey(b.title) ? -1 : 1; });
      return out;
    });
  };

  // read an archive's browse list (title \t cluster \t blob), written by the indexer
  // for browsable-sized archives. backs the reader's own book/talk list
  NomadArchiveIndex.prototype.listArchive = function (archiveIdx) {
    var self = this;
    return this.load().then(function () {
      var arc = self.archive(archiveIdx);
      if (!arc || !arc.hasList) return [];
      return self._fetch(self._url(archiveIdx + '.list'), null, 20000).then(function (r) {
        if (!(r.ok || r.status === 200 || r.status === 206)) return [];
        var out = [], lines = r.body.split('\n');
        for (var i = 0; i < lines.length; i++) {
          if (!lines[i]) continue;
          var f = lines[i].split('\t');
          if (f.length < 3) continue;
          out.push({ title: f[0], archiveIdx: +archiveIdx, archiveId: arc.id, cluster: +f[1], blob: +f[2] });
        }
        return out;
      });
    });
  };

  // Cache/return the opened ZimArchive for an archive (header-only). Used by
  // openArticle and by resource resolution (video/epub).
  NomadArchiveIndex.prototype._zim = function (archiveIdx, decompressors) {
    var self = this;
    return this.load().then(function () {
      var arc = self.archive(archiveIdx);
      if (!arc) throw new Error('unknown archive ' + archiveIdx);
      self._zims = self._zims || {};
      var zim = self._zims[archiveIdx];
      if (!zim) {
        zim = new ZimArchive(new HttpSource(arc.parts, { via: self.via }), { decompressors: decompressors });
        self._zims[archiveIdx] = zim;
      }
      return zim.openHeaderOnly().then(function () { return zim; });
    });
  };

  // open a preindexed article: reads only its cluster. hit = {archiveIdx, cluster, blob}.
  // returns { data, mimeType }, caches one ZimArchive per archive
  NomadArchiveIndex.prototype.openArticle = function (hit, decompressors) {
    return this._zim(hit.archiveIdx, decompressors).then(function (zim) {
      return zim.getContentAt(hit.cluster, hit.blob, 'text/html');
    });
  };

  // resolve a resource URL inside an archive and return its bytes - the path for
  // playing a ZIM video or reading a ZIM epub (one findByUrl + a cluster read).
  // only runs on a deliberate click. baseUrl = article's url, resourceUrl = the ref in its HTML
  NomadArchiveIndex.prototype.openResource = function (archiveIdx, baseUrl, resourceUrl, decompressors) {
    var self = this;
    return this._zim(archiveIdx, decompressors).then(function (zim) {
      // Candidate paths, most-specific first: resolved-against-article, raw ref,
      // decoded, and basename - covers root-relative and subdir article layouts.
      var raw = (resourceUrl || '').split('#')[0].split('?')[0];
      var cands = [resolveZimPath(baseUrl, resourceUrl), raw.replace(/^\.?\//, '')];
      try { cands.push(decodeURIComponent(cands[0])); } catch (e) {}
      var bn = raw.substring(raw.lastIndexOf('/') + 1);
      if (bn) cands.push(bn);
      // dedup
      var uniq = []; cands.forEach(function (c) { if (c && uniq.indexOf(c) < 0) uniq.push(c); });

      var nss = ['C', 'A', 'I', '-'];
      var tryOne = function (ci, ni) {
        if (ci >= uniq.length) return Promise.resolve(null);
        if (ni >= nss.length) return tryOne(ci + 1, 0);
        return zim.findByUrl(nss[ni], uniq[ci]).then(function (e) {
          if (e) return e;
          return tryOne(ci, ni + 1);
        });
      };
      return tryOne(0, 0).then(function (e) {
        if (!e) throw new Error('resource not found: ' + uniq.join(' , '));
        return zim.getContent(e).then(function (c) {
          if (!c.mimeType || c.mimeType === 'application/octet-stream') c.mimeType = mimeFromUrl(uniq[0]) || c.mimeType;
          return c;
        });
      });
    });
  };

  function mimeFromUrl(u) {
    u = (u || '').toLowerCase();
    if (/\.webm$/.test(u)) return 'video/webm';
    if (/\.mp4$/.test(u)) return 'video/mp4';
    if (/\.m4v$/.test(u)) return 'video/x-m4v';
    if (/\.ogv$/.test(u)) return 'video/ogg';
    if (/\.mp3$/.test(u)) return 'audio/mpeg';
    if (/\.epub$/.test(u)) return 'application/epub+zip';
    if (/\.pdf$/.test(u)) return 'application/pdf';
    if (/\.vtt$/.test(u)) return 'text/vtt';
    if (/\.(jpe?g)$/.test(u)) return 'image/jpeg';
    if (/\.png$/.test(u)) return 'image/png';
    if (/\.webp$/.test(u)) return 'image/webp';
    return null;
  }

  // Resolve a relative resource URL against an article's own path, ZIM-style
  // (paths are relative to the article's directory; handle ./ and ../).
  function resolveZimPath(baseUrl, rel) {
    rel = (rel || '').split('#')[0].split('?')[0];
    if (rel.charAt(0) === '/') return rel.replace(/^\/+/, '');
    var baseDir = (baseUrl || '');
    var slash = baseDir.lastIndexOf('/');
    baseDir = slash >= 0 ? baseDir.substring(0, slash + 1) : '';
    var path = baseDir + rel;
    var parts = path.split('/'), stack = [];
    for (var i = 0; i < parts.length; i++) {
      var p = parts[i];
      if (p === '.' || p === '') continue;
      if (p === '..') { stack.pop(); continue; }
      stack.push(p);
    }
    return stack.join('/');
  }

  return {
    ZimArchive: ZimArchive,
    HttpSource: HttpSource,
    FetchQueue: FetchQueue,
    sharedQueue: sharedQueue,
    fetchArchiveList: fetchArchiveList,
    NomadArchiveIndex: NomadArchiveIndex,
    _internals: { cmpBytes: cmpBytes, startsWithBytes: startsWithBytes, caseVariants: caseVariants, readU32: readU32, readU64: readU64 }
  };
});
