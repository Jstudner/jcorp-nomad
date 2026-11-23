/* nomad-overzoom.js - a deeper map tile made from a shallower one.

   Where the card has a map only to zoom 9 (a continent overview) but the map is set up to
   zoom 14 (blocks elsewhere), MapLibre asks for zoom-10 and -11 tiles there and would get
   nothing. This makes them from the zoom-9 tile instead: the shapes scaled up and moved so
   the child's corner is at 0,0, and anything that lands wholly outside the child dropped.
   No new detail, no new reads: the same roads and towns, closer.

   Works on Mapbox Vector Tile bytes directly: layers, keys, values and tags are copied as
   they are, only each feature's geometry is rewritten.

     NomadOverzoom.child(parentBytes, dz, cx, cy)   cx, cy: the child's place in the parent,
                                                    0 .. 2^dz - 1 across and down
*/
(function (root) {
  'use strict';

  function Reader(buf) { this.b = buf; this.p = 0; }
  Reader.prototype.varint = function () {
    let r = 0, mul = 1, b;
    do { b = this.b[this.p++]; r += (b & 0x7f) * mul; mul *= 128; } while (b & 0x80);
    return r;
  };
  Reader.prototype.skip = function (wire) {
    if (wire === 0) this.varint();
    else if (wire === 2) { const n = this.varint(); this.p += n; }
    else if (wire === 1) this.p += 8;
    else if (wire === 5) this.p += 4;
    else throw new Error('wire type ' + wire);
  };

  function Writer() { this.a = []; }
  Writer.prototype.varint = function (v) {
    while (v >= 0x80) { this.a.push((v & 0x7f) | 0x80); v = Math.floor(v / 128); }
    this.a.push(v);
  };
  Writer.prototype.bytes = function (u8) { for (let i = 0; i < u8.length; i++) this.a.push(u8[i]); };
  Writer.prototype.field = function (tag, wire, payload) {        // length-delimited
    this.varint(tag * 8 + wire); this.varint(payload.length); this.bytes(payload);
  };
  Writer.prototype.out = function () { return Uint8Array.from(this.a); };

  const zz = (n) => (n >>> 1) ^ -(n & 1);
  const unzz = (n) => (n << 1) ^ (n >> 31);

  // geometry commands -> scaled, shifted commands; null when the shape is wholly outside
  function moveGeometry(cmds, s, ox, oy, lo, hi) {
    const out = [];
    let x = 0, y = 0, px = 0, py = 0, i = 0;
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    while (i < cmds.length) {
      const c = cmds[i++], id = c & 7, n = c >> 3;
      out.push(c);
      if (id === 7) continue;                                       // ClosePath
      for (let k = 0; k < n; k++) {
        x += zz(cmds[i++]); y += zz(cmds[i++]);
        const nx = x * s - ox, ny = y * s - oy;
        out.push(unzz(nx - px), unzz(ny - py));
        px = nx; py = ny;
        if (nx < minX) minX = nx; if (nx > maxX) maxX = nx;
        if (ny < minY) minY = ny; if (ny > maxY) maxY = ny;
      }
    }
    if (maxX < lo || minX > hi || maxY < lo || minY > hi) return null;
    return out;
  }

  function packed(nums) { const w = new Writer(); for (const n of nums) w.varint(n >>> 0); return w.out(); }

  function moveFeature(u8, s, ox, oy, lo, hi) {
    const r = new Reader(u8), w = new Writer();
    let geom = null;
    const rest = [];
    while (r.p < u8.length) {
      const key = r.varint(), tag = key >> 3, wire = key & 7, start = r.p;
      if (tag === 4 && wire === 2) {
        const n = r.varint(), end = r.p + n, cmds = [];
        while (r.p < end) cmds.push(r.varint());
        geom = moveGeometry(cmds, s, ox, oy, lo, hi);
        if (!geom) return null;
      } else {
        r.skip(wire);
        rest.push([key, u8.subarray(start, r.p)]);
      }
    }
    for (const [key, raw] of rest) { w.varint(key); w.bytes(raw); }
    if (geom) w.field(4, 2, packed(geom));
    return w.out();
  }

  function moveLayer(u8, dz, cx, cy) {
    // the extent first: it can come after the features
    let extent = 4096;
    const scan = new Reader(u8);
    while (scan.p < u8.length) {
      const key = scan.varint();
      if ((key >> 3) === 5 && (key & 7) === 0) extent = scan.varint(); else scan.skip(key & 7);
    }
    const s = 1 << dz, ox = cx * extent, oy = cy * extent, buf = extent / 8;
    const r = new Reader(u8), w = new Writer();
    let kept = 0;
    while (r.p < u8.length) {
      const key = r.varint(), tag = key >> 3, wire = key & 7, start = r.p;
      if (tag === 2 && wire === 2) {
        const n = r.varint(), f = moveFeature(u8.subarray(r.p, r.p + n), s, ox, oy, -buf, extent + buf);
        r.p += n;
        if (f) { w.field(2, 2, f); kept++; }
      } else {
        r.skip(wire);
        w.varint(key); w.bytes(u8.subarray(start, r.p));
      }
    }
    return kept ? w.out() : null;
  }

  function child(parent, dz, cx, cy) {
    const u8 = parent instanceof Uint8Array ? parent : new Uint8Array(parent);
    const r = new Reader(u8), w = new Writer();
    while (r.p < u8.length) {
      const key = r.varint(), tag = key >> 3, wire = key & 7;
      if (tag === 3 && wire === 2) {
        const n = r.varint(), layer = moveLayer(u8.subarray(r.p, r.p + n), dz, cx, cy);
        r.p += n;
        if (layer) w.field(3, 2, layer);
      } else {
        const start = r.p; r.skip(wire); w.varint(key); w.bytes(u8.subarray(start, r.p));
      }
    }
    return w.out();
  }

  const api = { child };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.NomadOverzoom = api;
})(typeof self !== 'undefined' ? self : this);
