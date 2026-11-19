/* nomad-route.js - offline routing over NRG3 packs (Nomad-Tools route_pack.py).

   A pack is roads.idx (where every cell is) plus roads.bin (the cells), read by byte
   range: nothing loads until a search needs it. Three levels: level 0 holds every road
   in small cells, level 1 motorway to secondary, level 2 motorway and trunk. Short
   trips search level 0. Longer ones climb: out from both ends on small roads to the
   big ones, out on those to the interstates, across on the top level, and back down,
   so a trip across the country only reads a few MB.

   Vertices are OSM node ids, so several packs make one road network: where a search
   reaches a point more than one pack covers, every pack's cell there is loaded and
   intersections with the same id are the same intersection. A highways-only pack
   (levels 1 and 2 for a whole country) joins the same way, and where it is the only
   pack the route starts or ends on the nearest big road.

   Works in the browser and in Node (the speed tests run the same code):
     const r = new NomadRoute.Router(read)       read(pack, off, len) -> ArrayBuffer
     r.addPack('wa', idxArrayBuffer)             or, reading only the levels a trip needs:
     r.addPackLazy('wa', manifest.routing, (off, len) -> ArrayBuffer of roads.idx)
     const a = await r.snap(lat, lon, 'drive'), b = await r.snap(...)
     const res = await r.route(a, b, 'drive', progress => {})
     const back = await r.rejoin(here, res, legIndex, 'drive')   // off the route
*/
(function (root) {
  'use strict';

  const SCALE = 1e5;
  // km/h by road class (osm_graph.ROAD_CLASSES order)
  const CLASS_SPEED = [100, 90, 65, 55, 45, 40, 30, 12, 20, 40, 5, 5, 4, 15, 5, 20];
  const CLASS_NAMES = ['motorway', 'trunk', 'primary', 'secondary', 'tertiary', 'unclassified',
    'residential', 'living_street', 'service', 'road', 'footway', 'path', 'steps', 'cycleway',
    'pedestrian', 'track'];
  const LEVEL_MAX_CLASS = [15, 3, 1];   // every road; motorway..secondary; motorway, trunk
  const LEVELS = LEVEL_MAX_CLASS.length;
  const F_CAR_FWD = 16, F_CAR_BWD = 32, F_FOOT = 64;
  const WALK_MS = 1.4;
  const MAX_MS = 100 / 3.6;
  const LONG_M = 30000;          // straight-line distance past which level 1 takes the middle
  const TOP_M = 250000;          // ... and past which level 2 does
  const FALLBACK_M = 150000;     // longest trip tried on streets alone when the big roads fail
  // climbing a level: keep going this many seconds past the first road of the next level
  // up, and give up at the cap (then the trip stays a level lower)
  const CLIMB = [[90, 900], [600, 5400]];
  const HWEIGHT = 1.25;          // a little greedy: fewer cells read, routes within a few %
  const PARALLEL = 4;            // reads Nomad serves at once without refusing
  const REJOIN_M = 30000;        // off the route: look for a way back onto the next 30 km of it
  const REJOIN_S = 900;          // ... reached within 15 minutes, before planning afresh
  // junctions held in memory before a search gives up (~0.5 KB each: ~350 MB, what a
  // phone browser tab can spare); the longest trips in the US use about 450,000
  const MAX_NODES = 700000;

  function hav(la1, lo1, la2, lo2) {
    const R = 6371000, r = Math.PI / 180;
    const dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    const a = Math.sin(dla / 2) ** 2 + Math.cos(la1 * r) * Math.cos(la2 * r) * Math.sin(dlo / 2) ** 2;
    return 2 * R * Math.asin(Math.min(1, Math.sqrt(a)));
  }

  // binary heap of [key, value]
  class Heap {
    constructor() { this.k = []; this.v = []; }
    get size() { return this.k.length; }
    push(key, val) {
      const k = this.k, v = this.v; let i = k.length; k.push(key); v.push(val);
      while (i > 0) { const p = (i - 1) >> 1; if (k[p] <= key) break; k[i] = k[p]; v[i] = v[p]; i = p; }
      k[i] = key; v[i] = val;
    }
    peekKey() { return this.k[0]; }
    pop() {
      const k = this.k, v = this.v, topV = v[0], lastK = k.pop(), lastV = v.pop(), n = k.length;
      if (n) {
        let i = 0;
        for (;;) {
          let l = 2 * i + 1, m = i, mk = lastK;
          if (l < n && k[l] < mk) { m = l; mk = k[l]; }
          if (l + 1 < n && k[l + 1] < mk) { m = l + 1; mk = k[l + 1]; }
          if (m === i) break;
          k[i] = k[m]; v[i] = v[m]; i = m;
        }
        k[i] = lastK; v[i] = lastV;
      }
      return topV;
    }
    // values near the top, for read-ahead
    peekValues(n) { return this.v.slice(0, Math.min(n, this.v.length)); }
  }

  // index entries (29 bytes each) from `o` to the end of buf, appended to levels[]
  function parseEntries(buf, o, levels) {
    const dv = new DataView(buf), u8 = new Uint8Array(buf);
    for (; o + 29 <= buf.byteLength; o += 29) {
      const lv = u8[o];
      if (!levels[lv]) levels[lv] = [];
      levels[lv].push({ id: levels[lv].length, s: dv.getInt32(o + 1, true), w: dv.getInt32(o + 5, true),
        n: dv.getInt32(o + 9, true), e: dv.getInt32(o + 13, true),
        off: Number(dv.getBigUint64 ? dv.getBigUint64(o + 17, true) : (dv.getUint32(o + 17, true) + dv.getUint32(o + 21, true) * 4294967296)),
        len: dv.getUint32(o + 25, true) });
    }
    return levels;
  }
  function boxOf(cells) {
    let s = Infinity, w = Infinity, nn = -Infinity, e = -Infinity;
    for (const c of cells) { s = Math.min(s, c.s); w = Math.min(w, c.w); nn = Math.max(nn, c.n); e = Math.max(e, c.e); }
    return { s, w, n: nn, e };
  }
  function parseIndex(buf) {
    const u8 = new Uint8Array(buf);
    if (String.fromCharCode(u8[0], u8[1], u8[2], u8[3]) !== 'NRI3') throw new Error('not an NRG3 index');
    const levels = parseEntries(buf, 9, []);
    return { levels, box: boxOf(levels.flat().filter(Boolean)) };
  }

  // the cell whose quadtree box holds this point (boxes are half-open, as built)
  function cellAt(cells, la, lo) {
    for (const c of cells) if (la >= c.s && la < c.n && lo >= c.w && lo < c.e) return c;
    return null;
  }
  function cellsNear(cells, la, lo, pad) {
    const out = [];
    for (const c of cells) if (la >= c.s - pad && la < c.n + pad && lo >= c.w - pad && lo < c.e + pad) out.push(c);
    return out;
  }

  function parseCell(buf) {
    const u8 = new Uint8Array(buf);
    if (u8[0] !== 78 || u8[1] !== 67 || u8[2] !== 51) throw new Error('bad cell');
    const level = u8[3];
    let p = 4;
    // varints past 2^31 (OSM ids) need arithmetic, not bit ops
    function vi() {
      let r = 0, mul = 1, b;
      do { b = u8[p++]; r += (b & 0x7f) * mul; mul *= 128; } while (b & 0x80);
      return r;
    }
    function zz() { const v = vi(); return v % 2 ? -(v + 1) / 2 : v / 2; }
    const cid = vi(), V = vi(), F = vi(), E = vi(), N = vi();
    const ids = new Float64Array(V + F), lat = new Int32Array(V + F), lon = new Int32Array(V + F);
    let prev = 0;
    for (let i = 0; i < V; i++) { prev += vi(); ids[i] = prev; }
    let pla = 0, plo = 0;
    for (let i = 0; i < V; i++) { pla += zz(); plo += zz(); lat[i] = pla; lon[i] = plo; }
    prev = 0;
    for (let i = V; i < V + F; i++) { prev += vi(); ids[i] = prev; }
    pla = 0; plo = 0;
    for (let i = V; i < V + F; i++) { pla += zz(); plo += zz(); lat[i] = pla; lon[i] = plo; }
    for (let i = 0; i < F; i++) vi();                 // foreign cell ids: the point lookup covers it
    const names = new Array(N);
    const dec = typeof TextDecoder !== 'undefined' ? new TextDecoder() : null;
    for (let i = 0; i < N; i++) { const ln = vi(); names[i] = dec.decode(u8.subarray(p, p + ln)); p += ln; }
    const edges = new Array(E);
    for (let i = 0; i < E; i++) {
      const a = vi(), b = vi(), len = vi(), flags = u8[p++], nm = vi(), g = vi();
      // the shape stays packed in the cell until a route uses the edge: a cross-country
      // search holds hundreds of thousands of edges and draws a few thousand
      const gp = p;
      for (let k = 0; k < 2 * g; k++) { while (u8[p++] & 0x80); }
      edges[i] = { a, b, len, flags, name: nm ? names[nm - 1] : '', gp, gn: g };
    }
    return { level, cid, V, F, ids, lat, lon, edges, u8 };
  }

  // the road network, as far as it has been read
  class Graph {
    constructor(level) { this.level = level; this.nodes = new Map(); }
    node(id, la, lo) {
      let n = this.nodes.get(id);
      if (!n) { n = { id, lat: la, lon: lo, adj: [], ready: false }; this.nodes.set(id, n); }
      return n;
    }
    // a cell's edges go to its own vertices only; a crossing edge is in both cells
    addCell(c) {
      for (let i = 0; i < c.V + c.F; i++) this.node(c.ids[i], c.lat[i], c.lon[i]);
      for (const e of c.edges) {
        const A = this.nodes.get(c.ids[e.a]), B = this.nodes.get(c.ids[e.b]);
        const rec = { a: A, b: B, len: e.len, flags: e.flags, name: e.name, gb: c.u8, gp: e.gp, gn: e.gn };
        if (e.a < c.V) A.adj.push({ e: rec, fwd: true, to: B });
        if (e.b < c.V) B.adj.push({ e: rec, fwd: false, to: A });
      }
    }
  }

  function allowed(edgeRec, fwd, mode, reverse) {
    const f = edgeRec.flags;
    if (mode === 'walk') return (f & 15) !== 0;       // anywhere but a motorway
    if (f & F_FOOT) return false;
    // reverse: is travel along this adjacency's opposite direction legal (backward search)
    const goingFwd = reverse ? !fwd : fwd;
    return !!(f & (goingFwd ? F_CAR_FWD : F_CAR_BWD));
  }
  function edgeSecs(edgeRec, mode) {
    return mode === 'walk' ? edgeRec.len / WALK_MS : edgeRec.len / (CLASS_SPEED[edgeRec.flags & 15] / 3.6);
  }
  // polyline of an edge walked from `from`, in degrees
  // shape points of an edge from a to b, e5 pairs, unpacked from its cell
  function geomOf(e) {
    const u8 = e.gb, n = e.gn, out = new Array(2 * n);
    let p = e.gp, la = e.a.lat, lo = e.a.lon;
    const vi = () => { let r = 0, mul = 1, b; do { b = u8[p++]; r += (b & 0x7f) * mul; mul *= 128; } while (b & 0x80); return r; };
    for (let k = 0; k < n; k++) {
      const y = vi(), x = vi();
      la += y % 2 ? -(y + 1) / 2 : y / 2; lo += x % 2 ? -(x + 1) / 2 : x / 2;
      out[2 * k] = la; out[2 * k + 1] = lo;
    }
    return out;
  }
  function edgePts(edgeRec, fwd) {
    const g = geomOf(edgeRec), pts = [];
    const s = fwd ? edgeRec.a : edgeRec.b, t = fwd ? edgeRec.b : edgeRec.a;
    pts.push([s.lat / SCALE, s.lon / SCALE]);
    const n = g.length / 2;
    for (let k = 0; k < n; k++) { const j = fwd ? k : n - 1 - k; pts.push([g[2 * j] / SCALE, g[2 * j + 1] / SCALE]); }
    pts.push([t.lat / SCALE, t.lon / SCALE]);
    return pts;
  }

  const edgeKey = (e) => (e.a.id < e.b.id ? e.a.id + '-' + e.b.id : e.b.id + '-' + e.a.id);
  // walking that prefers trails: paths and tracks a little cheaper, big roads a lot dearer
  const TRAIL_FACTOR = [9, 4, 2.5, 1.8, 1.3, 1.1, 1, 1, 1, 1.2, 0.85, 0.7, 0.9, 0.85, 0.9, 0.75];
  const onLevel = (e, lv) => (e.flags & 15) <= LEVEL_MAX_CLASS[lv] && !(e.flags & F_FOOT);
  const touches = (n, lv) => n.adj.some(ad => onLevel(ad.e, lv));
  const startOf = (ad) => ad.fwd ? ad.e.a : ad.e.b;

  class Router {
    constructor(read) {
      this.read = read;               // async (packId, offset, length) -> ArrayBuffer
      this.packs = [];
      this.graphs = [];
      for (let lv = 0; lv < LEVELS; lv++) this.graphs.push(new Graph(lv));
      this.loaded = new Map();        // 'pack/level/cell' -> Promise
      this.stats = { cells: 0, bytes: 0, batches: 0 };
      this.tooBig = false;            // the last search stopped at MAX_NODES
      this.blocked = new Set();       // 'idA-idB' road pieces closed for this trip (blockAhead)
      this.backId = null;             // on a blocked piece: the end that is behind you
      // what a route is planned for: 'fast' (time), 'short' (distance), 'nohwy' (time,
      // keeping off motorways, the interstates and freeways, unless there is no other way.
      // the big US routes stay in, they're what you take when the interstate is jammed)
      this.prefer = 'fast';
    }
    // how much the search leans toward the goal: more when motorways cost extra, or a
    // cross-country trip off them spreads over half the country
    hweight() { return this.prefer === 'nohwy' ? 1.8 : HWEIGHT; }
    // what an edge costs the search; legs still show real driving time
    cost(e, mode) {
      if (this.blocked.size && this.blocked.has(edgeKey(e))) return Infinity;
      if (this.prefer === 'short') return e.len / (mode === 'walk' ? WALK_MS : MAX_MS);
      const t = edgeSecs(e, mode);
      if (this.prefer === 'trails' && mode === 'walk') return t * TRAIL_FACTOR[e.flags & 15];
      return this.prefer === 'nohwy' && mode === 'drive' && (e.flags & 15) === 0 ? t * 2 : t;
    }
    /* Road blocked: the route's next stretch (from leg `at`, about `meters` of it) is out
       for the rest of this trip, and the route may turn back the way it came. Memory only,
       a new trip or a reload starts clear. */
    blockAhead(legs, at, meters = 300) {
      let run = 0;
      for (let i = Math.max(0, at); i < legs.length && run < meters; i++) {
        const l = legs[i];
        if (l.from != null && l.to != null) this.blocked.add(l.from < l.to ? l.from + '-' + l.to : l.to + '-' + l.from);
        if (i === at && l.from != null) this.backId = l.from;
        run += l.dist;
      }
    }
    clearBlocks() { this.blocked.clear(); this.backId = null; }
    nodeCount() { return this.graphs.reduce((t, g) => t + g.nodes.size, 0); }
    // forget the roads read so far (each pack's index stays), before a new plan once
    // memory is full. Snaps made before this can't be used after it.
    trim() {
      this.graphs = [];
      for (let lv = 0; lv < LEVELS; lv++) this.graphs.push(new Graph(lv));
      this.loaded = new Map();
    }
    // a whole roads.idx, already read
    addPack(id, idxBuf) {
      const idx = parseIndex(idxBuf);
      const major = !idx.levels[0] || !idx.levels[0].length;
      this.packs.push({ id, box: idx.box, levels: idx.levels, major });
      return idx;
    }
    // only the levels a trip needs, read when it needs them: routing is the manifest's
    // "routing" (bounds in degrees, levels: {level: [offset, length]} in roads.idx)
    addPackLazy(id, routing, readIdx) {
      const [w, s, e, n] = routing.bounds;
      const pad = 0.01;
      const box = { s: Math.floor((s - pad) * SCALE), w: Math.floor((w - pad) * SCALE), n: Math.ceil((n + pad) * SCALE), e: Math.ceil((e + pad) * SCALE) };
      const ranges = routing.levels;
      this.packs.push({ id, box, levels: [], ranges, readIdx, pending: {}, major: !ranges['0'] });
    }
    // index entries for this level of every pack whose area holds one of the points
    async ensureIdx(level, points) {
      const jobs = [];
      for (const pk of this.packs) {
        if (pk.levels[level] || !pk.ranges || !pk.ranges[level]) continue;
        const b = pk.box;
        if (!points.some(([la, lo]) => la >= b.s && la < b.n && lo >= b.w && lo < b.e)) continue;
        if (!pk.pending[level]) {
          const [off, len] = pk.ranges[level];
          pk.pending[level] = Promise.resolve(pk.readIdx(off, len)).then(buf => {
            this.stats.bytes += len;
            const lv = parseEntries(buf, 0, []);
            pk.levels[level] = lv[level] || [];
          });
        }
        jobs.push(pk.pending[level]);
      }
      if (jobs.length) await Promise.all(jobs);
    }
    coversAt(level, la, lo) {
      const out = [];
      let majorHere = false;
      for (const pk of this.packs) {
        const b = pk.box;
        if (la < b.s || la >= b.n || lo < b.w || lo >= b.e) continue;
        const cells = pk.levels[level];
        if (!cells) continue;
        const c = cellAt(cells, la, lo);
        if (c) { out.push([pk, c]); if (pk.major) majorHere = true; }
      }
      // a highways-only pack and a state pack both have the big roads: read them once
      return majorHere && level > 0 ? out.filter(([pk]) => pk.major) : out;
    }
    loadCell(pk, level, c) {
      const key = pk.id + '/' + level + '/' + c.id;
      let p = this.loaded.get(key);
      if (!p) {
        p = Promise.resolve(this.read(pk.id, c.off, c.len)).then(buf => {
          this.stats.cells++; this.stats.bytes += c.len;
          this.graphs[level].addCell(parseCell(buf));
        });
        this.loaded.set(key, p);
      }
      return p;
    }
    // every pack's cell at these points, PARALLEL at a time
    async loadAt(level, points) {
      await this.ensureIdx(level, points);
      const want = [];
      const seen = new Set();
      for (const [la, lo] of points) {
        for (const [pk, c] of this.coversAt(level, la, lo)) {
          const key = pk.id + '/' + level + '/' + c.id;
          if (seen.has(key) || this.loaded.has(key)) continue;
          seen.add(key); want.push([pk, c]);
        }
      }
      for (let i = 0; i < want.length; i += PARALLEL) {
        this.stats.batches++;
        await Promise.all(want.slice(i, i + PARALLEL).map(([pk, c]) => this.loadCell(pk, level, c)));
      }
      // cells another call already started
      await Promise.all(points.flatMap(([la, lo]) => this.coversAt(level, la, lo)
        .map(([pk, c]) => this.loaded.get(pk.id + '/' + level + '/' + c.id))));
    }
    // a node's adjacency is complete once every covering pack's cell at it is in
    async ready(level, n, heap) {
      if (n.ready) return;
      const pts = [[n.lat, n.lon]];
      if (heap) {                     // read ahead: the next nodes the search will want
        for (const m of heap.peekValues(48)) if (!m.ready) pts.push([m.lat, m.lon]);
      }
      await this.loadAt(level, pts);
      n.ready = true;
      for (const m of heap ? heap.peekValues(48) : []) {
        if (!m.ready && this.coversAt(level, m.lat, m.lon).every(([pk, c]) =>
          this.loaded.has(pk.id + '/' + level + '/' + c.id))) m.ready = true;
      }
    }

    /* nearest road to a point that `mode` may use: a position on an edge. Street level
       where a pack has the streets. Where only a highways pack covers the point, the
       nearest big road (snap.level > 0, snap.dist says how far off it is).
       opts.place: a place chosen on the map or by search, not a position on the move: a
       street within ~150 m beats a motorway or trunk road (reachable only from a ramp, so a
       spot picked beside the freeway would route to the next exit and back). A GPS fix
       keeps the nearest road, since a phone on the freeway is on it. */
    async snap(lat, lon, mode, opts = {}) {
      let best = await this.snapOn(0, lat, lon, mode, 3000, 300, opts.place ? 150 : 0);
      if (mode === 'drive' && (!best || best.dist > 2000)) {
        // nearby first: in a city the big roads are close and their cells are many
        for (let lv = 1; lv < LEVELS && !(best && best.level); lv++) {
          for (const reach of [3000, 15000, 60000]) {
            const far = await this.snapOn(lv, lat, lon, mode, reach, reach / 2);
            if (far && far.dist < reach * 1.11) { if (!best || far.dist < best.dist) best = far; break; }
          }
        }
      }
      return best;
    }
    async snapOn(level, lat, lon, mode, reach, pad, fastPenalty = 0) {
      const la = Math.round(lat * SCALE), lo = Math.round(lon * SCALE);
      await this.ensureIdx(level, [[la, lo]]);
      const pts = [[la, lo]];
      for (const pk of this.packs) for (const c of cellsNear(pk.levels[level] || [], la, lo, pad)) pts.push([(c.s + c.n) / 2 | 0, (c.w + c.e) / 2 | 0]);
      await this.loadAt(level, pts);
      const kx = Math.cos(lat * Math.PI / 180);
      let best = null, bd = Infinity;
      const seen = new Set();
      for (const n of this.graphs[level].nodes.values()) {
        if (Math.abs(n.lat - la) > reach || Math.abs(n.lon - lo) > reach / kx) continue;
        for (const ad of n.adj) {
          const e = ad.e;
          const key = e.a.id + '-' + e.b.id + '-' + e.len;   // a border edge is decoded once per cell
          if (seen.has(key)) continue;
          seen.add(key);
          if (mode === 'walk' ? (e.flags & 15) === 0 : (e.flags & F_FOOT) || !(e.flags & (F_CAR_FWD | F_CAR_BWD))) continue;
          const pts = edgePts(e, true);
          let along = 0;
          for (let i = 0; i + 1 < pts.length; i++) {
            const [y1, x1] = pts[i], [y2, x2] = pts[i + 1];
            const dx = (x2 - x1) * kx, dy = y2 - y1, px = (lon - x1) * kx, py = lat - y1;
            const L2 = dx * dx + dy * dy;
            let t = L2 ? (px * dx + py * dy) / L2 : 0; t = Math.max(0, Math.min(1, t));
            const qx = x1 + (x2 - x1) * t, qy = y1 + (y2 - y1) * t;
            const d = hav(lat, lon, qy, qx), score = d + ((e.flags & 15) <= 1 ? fastPenalty : 0);
            const segLen = hav(y1, x1, y2, x2);
            if (score < bd) { bd = score; best = { e, lat: qy, lon: qx, along: along + segLen * t, i, t, dist: d }; }
            along += segLen;
          }
        }
      }
      if (!best) return null;
      // geometric length vs the stored length: scale so the parts add up
      const geoLen = edgePts(best.e, true).reduce((s, p, i, a) => i ? s + hav(a[i - 1][0], a[i - 1][1], p[0], p[1]) : 0, 0) || 1;
      best.fracA = Math.max(0, Math.min(1, best.along / geoLen));
      best.name = best.e.name;
      best.level = level;
      return best;
    }

    // Dijkstra/A* on one level. sources: [[node, cost]]; targets: Map node id -> extra cost
    // (minExtra: the smallest of those, when some are below zero). reverse: search
    // backwards (edges walked against their direction). Stops at the best target, or
    // when stop(cost) returns true (then returns everything settled).
    async search(level, mode, sources, targets, goal, opts = {}) {
      const heap = new Heap();
      const dist = new Map(), came = new Map(), done = new Set();
      const h = goal ? (n) => this.hweight() * hav(n.lat / SCALE, n.lon / SCALE, goal[0], goal[1]) / (mode === 'walk' ? WALK_MS : MAX_MS) : () => 0;
      for (const [n, c] of sources) { if (c < (dist.get(n) ?? Infinity)) { dist.set(n, c); heap.push(c + h(n), n); } }
      const minExtra = opts.minExtra || 0;
      let best = Infinity, bestNode = null, steps = 0;
      while (heap.size) {
        if (heap.peekKey() + minExtra >= best) break;
        const u = heap.pop();
        if (done.has(u)) continue;
        const du = dist.get(u);
        if (opts.stop && opts.stop(du, u, done)) break;
        done.add(u);
        if (targets && targets.has(u.id)) {
          const tot = du + targets.get(u.id);
          if (tot < best) { best = tot; bestNode = u; }
        }
        if (!u.ready) await this.ready(level, u, heap);
        if (++steps % 4096 === 0) {
          if (opts.progress) opts.progress(this.stats);
          if (this.nodeCount() > MAX_NODES) { this.tooBig = true; break; }
        }
        for (const ad of u.adj) {
          if (!allowed(ad.e, ad.fwd, mode, opts.reverse)) continue;
          const v = ad.to, nd = du + this.cost(ad.e, mode);
          if (nd < (dist.get(v) ?? Infinity)) { dist.set(v, nd); came.set(v, ad); heap.push(nd + h(v), v); }
        }
      }
      return { dist, came, done, best, bestNode, steps };
    }

    /* From both ends at once, a step from whichever side is behind, until they meet.
       Besides reading less on a long trip, it knows when there is no way through: a
       destination on its own network (Alaska from Seattle, with no Canada on the card)
       runs out of roads after a few thousand junctions, where a search from the start
       alone would read every road in the country first. */
    async searchBoth(level, mode, sources, targets, a, b, progress) {
      const mk = (srcs, goal, reverse) => {
        const sd = { heap: new Heap(), dist: new Map(), came: new Map(), done: new Set(), reverse,
          h: (n) => this.hweight() * hav(n.lat / SCALE, n.lon / SCALE, goal[0], goal[1]) / (mode === 'walk' ? WALK_MS : MAX_MS) };
        for (const [n, c] of srcs) if (c < (sd.dist.get(n) ?? Infinity)) { sd.dist.set(n, c); sd.heap.push(c + sd.h(n), n); }
        return sd;
      };
      const F = mk(sources, [b.lat, b.lon], false), B = mk(targets, [a.lat, a.lon], true);
      let mu = Infinity, meet = null, steps = 0;
      for (const [n, c] of F.dist) if (B.dist.has(n) && c + B.dist.get(n) < mu) { mu = c + B.dist.get(n); meet = n; }
      while (F.heap.size && B.heap.size) {
        if (Math.min(F.heap.peekKey(), B.heap.peekKey()) >= mu) break;
        // the side that is behind goes next, but neither runs far ahead of the other: a
        // side with nowhere to go has to get its turns to find that out
        let fwd = F.heap.peekKey() <= B.heap.peekKey();
        if (F.done.size > 2 * B.done.size + 2000) fwd = false;
        else if (B.done.size > 2 * F.done.size + 2000) fwd = true;
        const S = fwd ? F : B, O = fwd ? B : F;
        const u = S.heap.pop();
        if (S.done.has(u)) continue;
        S.done.add(u);
        if (!u.ready) await this.ready(level, u, S.heap);
        if (++steps % 4096 === 0) {
          if (progress) progress(this.stats);
          if (this.nodeCount() > MAX_NODES) { this.tooBig = true; return null; }
        }
        const du = S.dist.get(u);
        for (const ad of u.adj) {
          if (!allowed(ad.e, ad.fwd, mode, S.reverse)) continue;
          const v = ad.to, nd = du + this.cost(ad.e, mode);
          if (nd < (S.dist.get(v) ?? Infinity)) {
            S.dist.set(v, nd); S.came.set(v, ad); S.heap.push(nd + S.h(v), v);
            const ov = O.dist.get(v);
            if (ov !== undefined && nd + ov < mu) { mu = nd + ov; meet = v; }
          }
        }
      }
      return meet ? { F, B, meet, mu, steps } : null;
    }

    // path of adjacencies from a search's source to n (forward search)
    static trace(came, n) {
      const out = [];
      for (let ad = came.get(n); ad; ad = came.get(startOf(ad))) out.push(ad);
      return out.reverse();
    }
    // a backward search, from n toward its source, as adjacencies in driving order
    static traceBack(came, n) {
      const out = [];
      for (let ad = came.get(n); ad; ad = came.get(startOf(ad))) out.push({ e: ad.e, fwd: !ad.fwd, to: startOf(ad) });
      return out;
    }

    // costs of leaving a snapped point along its edge to the edge's ends
    edgeEnds(sn, mode, reverse) {
      const e = sn.e, out = [];
      const A = e.a, B = e.b;
      const secs = this.cost(e, mode);
      // to A means walking the edge backwards from the snap point (forward search), or
      // arriving at the snap point from A (reverse search) - the same legality check either way
      const toA = mode === 'walk' || (!(e.flags & F_FOOT) && (e.flags & (reverse ? F_CAR_FWD : F_CAR_BWD)));
      const toB = mode === 'walk' || (!(e.flags & F_FOOT) && (e.flags & (reverse ? F_CAR_BWD : F_CAR_FWD)));
      if (toA) out.push([A, secs * sn.fracA]);
      if (toB) out.push([B, secs * (1 - sn.fracA)]);
      if (this.blocked.size && this.blocked.has(edgeKey(e))) {
        // standing on a closed piece: only back the way you came (its cost stays finite)
        const t = edgeSecs(e, mode), both = [[A, t * sn.fracA], [B, t * (1 - sn.fracA)]];
        const back = both.filter(([n]) => n.id === this.backId);
        return back.length ? back : both;        // a closed piece that isn't the one you came in on
      }
      return out;
    }

    async route(a, b, mode, progress) {
      if (!a || !b) return null;
      this.stats = { cells: 0, bytes: 0, batches: 0 };
      this.tooBig = false; this.noWay = false;
      const t0 = Date.now();
      const straight = hav(a.lat, a.lon, b.lat, b.lon);
      const goal = [b.lat, b.lon];
      let parts = null, how = 'local';
      // both points on one edge, and the edge runs the right way
      if (a.e === b.e) {
        const fwd = b.fracA >= a.fracA;
        if (mode === 'walk' || (a.e.flags & (fwd ? F_CAR_FWD : F_CAR_BWD))) {
          return this.finish([{ partial: [a, b] }], mode, t0, 'same road');
        }
      }
      const low = Math.max(a.level || 0, b.level || 0);
      // on foot the top level is mostly motorways, so a long walk crosses on level 1
      if (straight > LONG_M || low > 0) {
        const top = mode === 'walk' ? Math.max(low, 1) : Math.max(low, straight > TOP_M ? 2 : 1);
        parts = await this.routeLong(a, b, top, progress, mode);
        if (parts) how = 'long';
      }
      // streets only, if the big roads didn't do it: never when the search from both ends
      // ran out of roads (there is no way), and only for a trip a street search can manage
      if (!parts && !low && !this.noWay && !this.tooBig && straight < FALLBACK_M) {
        const targets = new Map(this.edgeEnds(b, mode, true).map(([n, c]) => [n.id, c]));
        const r = await this.search(0, mode, this.edgeEnds(a, mode, false), targets, goal, { progress });
        if (!r.bestNode) return null;
        parts = [{ partial: [a, null] }, { adjs: Router.trace(r.came, r.bestNode) }, { partial: [null, b] }];
      }
      if (!parts) return null;
      return this.finish(parts, mode, t0, how);
    }

    /* Long trip: from each end, climb a level at a time (small roads out to the big ones,
       big roads out to the interstates), cross on the highest level both ends reached,
       then trace back down. A climb that finds nothing to climb to within its cap
       leaves the trip a level lower. */
    async routeLong(a, b, top, progress, mode = 'drive') {
      const slow = mode === 'walk' ? 8 : 1;   // the same reach on foot takes longer
      const climb = async (sn, reverse) => {
        const steps = [];             // per level: the search that climbed out of it
        let lv = sn.level || 0;
        let srcs = this.edgeEnds(sn, mode, reverse);
        const from = [];              // per level: [[node, cost]] the level starts from
        from[lv] = srcs;
        for (; lv < top; lv++) {
          const [extra, cap] = CLIMB[lv].map(x => x * slow);
          let first = Infinity;
          const stop = (cost, u) => {
            if (first === Infinity && u.ready && touches(u, lv + 1)) first = cost;
            return cost > Math.min(first + extra, cap);
          };
          const r = await this.search(lv, mode, srcs, null, null, { stop, progress, reverse });
          const up = [];
          for (const n of r.done) if (touches(n, lv + 1)) up.push(n);
          if (!up.length) break;
          await this.loadAt(lv + 1, up.map(n => [n.lat, n.lon]));
          const g = this.graphs[lv + 1], next = [];
          for (const n of up) { const m = g.nodes.get(n.id); if (m) next.push([m, r.dist.get(n)]); }
          if (!next.length) break;
          steps[lv] = r;
          from[lv + 1] = srcs = next;
        }
        return { steps, from, reached: from.length - 1 };
      };
      const A = await climb(a, false), B = await climb(b, true);
      // cross on the highest level both ends reached. If that network has no way through,
      // a level down may: Swiss and Austrian motorways never meet, the crossings between
      // them are main roads. Only when the big roads fail too is there no way.
      const reached = Math.min(A.reached, B.reached);
      const floor = Math.min(reached, Math.max(1, a.level || 0, b.level || 0));   // never all the way to streets
      let lv = reached, mid = null;
      for (; lv >= floor; lv--) {
        if (!A.from[lv] || !B.from[lv]) continue;
        mid = await this.searchBoth(lv, mode, A.from[lv], B.from[lv], a, b, progress);
        if (mid || this.tooBig) break;
      }
      if (!mid) { if (!this.tooBig) this.noWay = true; return null; }
      const fPart = Router.trace(mid.F.came, mid.meet), bPart = Router.traceBack(mid.B.came, mid.meet);
      const midAdjs = fPart.concat(bPart);
      const endNode = bPart.length ? bPart[bPart.length - 1].to : mid.meet;
      // back down the start's climb: each level's path ends where the next one began
      let head = [], id = fPart.length ? startOf(fPart[0]).id : mid.meet.id;
      for (let k = lv - 1; k >= (a.level || 0); k--) {
        const n = this.graphs[k].nodes.get(id);
        const adjs = n ? Router.trace(A.steps[k].came, n) : [];
        head = adjs.concat(head);
        if (adjs.length) id = startOf(adjs[0]).id;
      }
      let tail = [];
      id = endNode.id;
      for (let k = lv - 1; k >= (b.level || 0); k--) {
        const n = this.graphs[k].nodes.get(id);
        const adjs = n ? Router.traceBack(B.steps[k].came, n) : [];
        tail = tail.concat(adjs);
        if (adjs.length) id = adjs[adjs.length - 1].to.id;
      }
      this.lastLevel = lv;
      return [{ partial: [a, null] }, { adjs: head }, { adjs: midAdjs }, { adjs: tail }, { partial: [null, b] }];
    }

    /* How far can I get: every road reachable from `here` within `seconds`, as pieces of
       line with the time they are reached by. On foot, the small roads; by car, the small
       roads for the first ten minutes, then the big roads from wherever those reach. */
    async reach(here, mode, seconds, progress) {
      if (!here) return [];
      this.tooBig = false;
      const out = [];
      // by car, the streets a reach is read from: residential and smaller would be
      // thousands of pieces in a city and say nothing more about how far you get
      const minor = mode === 'drive' ? 6 : 99;
      // each level its own allowance; the big roads drawn end to end (it is a wide view)
      const collect = (r, level) => {
        let n = 0;
        for (const [v, ad] of r.came) {
          const t = r.dist.get(v);
          if (t > seconds || (ad.e.flags & 15) >= minor || ++n > (level ? 80000 : 25000)) continue;
          const pts = level ? [[ad.e.a.lat / SCALE, ad.e.a.lon / SCALE], [ad.e.b.lat / SCALE, ad.e.b.lon / SCALE]] : edgePts(ad.e, ad.fwd);
          out.push({ pts, t, level });
        }
      };
      const first = mode === 'drive' ? Math.min(seconds, 600) : seconds;
      const lv = here.level || 0;
      const r0 = await this.search(lv, mode, this.edgeEnds(here, mode, false), null, null, { stop: (c) => c > first, progress });
      collect(r0, lv);
      if (mode === 'drive' && seconds > first && lv === 0) {
        const up = [...r0.done].filter((n) => touches(n, 1));
        await this.loadAt(1, up.map((n) => [n.lat, n.lon]));
        const src = [];
        for (const n of up) { const m = this.graphs[1].nodes.get(n.id); if (m) src.push([m, r0.dist.get(n)]); }
        if (src.length) collect(await this.search(1, mode, src, null, null, { stop: (c) => c > seconds, progress }), 1);
      }
      return out;
    }

    /* Off the route: the quickest way from `here` back onto the part of the route ahead
       (from leg `at` on), counting the rest of the route from where it joins. Searches
       small roads near here only, so it answers in a few reads; null when there is no
       way back within REJOIN_S, and then the caller plans afresh. */
    async rejoin(here, res, at, mode, progress) {
      if (!here || !res) return null;
      this.stats = { cells: 0, bytes: 0, batches: 0 };
      const t0 = Date.now();
      const legs = res.legs;
      const after = new Array(legs.length + 1).fill(0);
      for (let i = legs.length - 1; i >= 0; i--) after[i] = after[i + 1] + legs[i].secs;
      const targets = new Map(), legOf = new Map();
      let ahead = 0, minExtra = Infinity;
      for (let i = Math.max(0, at); i < legs.length && ahead < REJOIN_M; i++) {
        ahead += legs[i].dist;
        const id = legs[i].to;
        if (id == null || targets.has(id)) continue;
        targets.set(id, after[i + 1]); legOf.set(id, i);
        minExtra = Math.min(minExtra, after[i + 1]);
      }
      if (!targets.size) return null;
      // the first point of the route reached is nearly always the one to take: past it,
      // the route itself is the quick way on. Look a minute further, for a better join.
      let hit = Infinity;
      const stop = (cost, u) => {
        if (hit === Infinity && targets.has(u.id)) hit = cost;
        return cost > REJOIN_S || cost > hit + 60;
      };
      const lv = here.level || 0;
      const r = await this.search(lv, mode, this.edgeEnds(here, mode, false), targets, null, { minExtra, stop, progress });
      if (!r.bestNode) return null;
      const back = this.finish([{ partial: [here, null] }, { adjs: Router.trace(r.came, r.bestNode) }], mode, t0, 'rejoin');
      const rest = legs.slice(legOf.get(r.bestNode.id) + 1);
      const out = back.legs.concat(rest);
      let dist = 0, secs = 0; for (const l of out) { dist += l.dist; secs += l.secs; }
      return { legs: out, dist, secs, how: 'rejoin', ms: Date.now() - t0, stats: { ...this.stats }, joinedAt: back.legs.length };
    }

    /* legs: {name, cls, dist, secs, pts, to}, what the turn list and the line are made
       of. `to` is the OSM id of the junction a leg ends at (what rejoin aims for) */
    finish(parts, mode, t0, how) {
      const legs = [];
      for (const p of parts) {
        if (p.adjs) for (const ad of p.adjs) legs.push({ name: ad.e.name, cls: ad.e.flags & 15, dist: ad.e.len, secs: edgeSecs(ad.e, mode), pts: edgePts(ad.e, ad.fwd), from: startOf(ad).id, to: ad.to.id });
      }
      // stitch the snapped ends onto the first and last legs
      const first = parts[0], last = parts[parts.length - 1];
      if (first.partial && first.partial[0] && first.partial[1]) {
        const [a, b] = first.partial;
        const len = Math.abs(b.fracA - a.fracA) * a.e.len;
        legs.push({ name: a.e.name, cls: a.e.flags & 15, dist: len, secs: edgeSecs(a.e, mode) * Math.abs(b.fracA - a.fracA), pts: [[a.lat, a.lon], [b.lat, b.lon]], to: null });
      } else {
        if (first.partial && first.partial[0]) {
          const a = first.partial[0];
          const startNode = legs.length ? legs[0].pts[0] : null;
          if (startNode) legs.unshift({ name: a.e.name, cls: a.e.flags & 15, dist: hav(a.lat, a.lon, startNode[0], startNode[1]), secs: 0, pts: [[a.lat, a.lon], startNode], to: null });
        }
        if (last.partial && last.partial[1]) {
          const b = last.partial[1];
          const endPt = legs.length ? legs[legs.length - 1].pts.slice(-1)[0] : null;
          if (endPt) legs.push({ name: b.e.name, cls: b.e.flags & 15, dist: hav(endPt[0], endPt[1], b.lat, b.lon), secs: 0, pts: [endPt, [b.lat, b.lon]], to: null });
        }
      }
      if (!legs.length && first.partial && first.partial[0] && last.partial && last.partial[1]) {
        const a = first.partial[0], b = last.partial[1];
        legs.push({ name: a.e.name, cls: a.e.flags & 15, dist: hav(a.lat, a.lon, b.lat, b.lon), secs: 0, pts: [[a.lat, a.lon], [b.lat, b.lon]], to: null });
      }
      for (const l of legs) if (!l.secs && l.dist) l.secs = mode === 'walk' ? l.dist / WALK_MS : l.dist / (CLASS_SPEED[l.cls] / 3.6);
      let dist = 0, secs = 0; for (const l of legs) { dist += l.dist; secs += l.secs; }
      const ends = { from: first.partial && first.partial[0], to: last.partial && last.partial[1] };
      return { legs, dist, secs, how, ms: Date.now() - t0, stats: { ...this.stats },
        // how far each end is from the road the route uses (big where only highways are on the card)
        offRoad: [ends.from ? ends.from.dist : 0, ends.to ? ends.to.dist : 0] };
    }
  }

  function bearing(a, b) {
    const r = Math.PI / 180, y = Math.sin((b[1] - a[1]) * r) * Math.cos(b[0] * r);
    const x = Math.cos(a[0] * r) * Math.sin(b[0] * r) - Math.sin(a[0] * r) * Math.cos(b[0] * r) * Math.cos((b[1] - a[1]) * r);
    return (Math.atan2(y, x) / r + 360) % 360;
  }
  const COMPASS = ['north', 'northeast', 'east', 'southeast', 'south', 'southwest', 'west', 'northwest'];
  function turnOf(delta) {
    const d = ((delta + 540) % 360) - 180;
    if (Math.abs(d) < 20) return ['straight', 'Continue on'];
    if (Math.abs(d) > 150) return ['uturn', 'Make a U-turn onto'];
    const side = d > 0 ? 'right' : 'left';
    if (Math.abs(d) < 45) return ['slight-' + side, 'Bear ' + side + ' onto'];
    if (Math.abs(d) > 110) return ['sharp-' + side, 'Turn sharp ' + side + ' onto'];
    return [side, 'Turn ' + side + ' onto'];
  }
  // legs -> steps where the road name changes, each with where it starts on the line.
  // Ramps are unnamed motorway pieces (links fold into their road's class). One that
  // carries straight on along a highway is part of it, one that turns off is an exit,
  // and one from a street is a ramp. Both take the name of the road they lead to.
  function steps(res) {
    const out = [], line = [];
    const major = (c) => c <= 1;
    for (let i = 0; i < res.legs.length; i++) {
      const lg = res.legs[i];
      const startIdx = line.length ? line.length - 1 : 0;
      for (let k = line.length ? 1 : 0; k < lg.pts.length; k++) line.push(lg.pts[k]);
      const prev = out[out.length - 1];
      const merge = () => { prev.dist += lg.dist; prev.secs += lg.secs; };
      let kind, verb;
      if (!prev) kind = 'depart';
      else {
        const pp = res.legs[i - 1].pts;
        [kind, verb] = turnOf(bearing(lg.pts[0], lg.pts[1] || lg.pts[0]) - bearing(pp[pp.length - 2] || pp[0], pp[pp.length - 1]));
      }
      if (prev) {
        if (lg.name && lg.name === prev.name) { merge(); continue; }
        if (prev.ramp && !prev.name) {             // still on the ramp, or reaching its road
          if (lg.name) prev.name = lg.name;
          merge(); continue;
        }
        if (!lg.name && major(lg.cls) && major(prev.cls) && kind === 'straight') { merge(); continue; }
        if (!lg.name && !major(lg.cls) && lg.dist < 60) { merge(); continue; }
      }
      const step = { kind, verb, name: lg.name, cls: lg.cls, dist: lg.dist, secs: lg.secs, at: startIdx, ll: lg.pts[0] };
      if (kind === 'depart') step.verb = 'Head ' + COMPASS[Math.round(bearing(lg.pts[0], lg.pts[1] || lg.pts[0]) / 45) % 8] + ' on';
      else if (!lg.name && major(lg.cls)) {
        step.ramp = true;
        step.verb = prev && major(prev.cls) ? 'Take the exit to' : 'Take the ramp onto';
        if (kind === 'straight') step.kind = prev && major(prev.cls) ? 'slight-right' : 'straight';
      }
      out.push(step);
    }
    // an "exit" that only led back onto the road it left was a bend, not an exit
    for (let i = out.length - 1; i >= 2; i--) {
      if (out[i - 1].ramp && out[i - 1].name === out[i - 2].name) {
        out[i - 2].dist += out[i - 1].dist; out[i - 2].secs += out[i - 1].secs; out.splice(i - 1, 1);
      }
    }
    for (let i = out.length - 1; i >= 1; i--) {
      if (out[i].name && out[i].name === out[i - 1].name) { out[i - 1].dist += out[i].dist; out[i - 1].secs += out[i].secs; out.splice(i, 1); }
    }
    for (const s of out) if (!s.name) s.name = s.ramp ? 'the highway' : 'an unnamed road';
    out.push({ kind: 'arrive', verb: 'Arrive', name: '', dist: 0, secs: 0, at: line.length - 1, ll: line[line.length - 1] });
    return { steps: out, line };
  }

  const api = { Router, steps, parseIndex, hav, SCALE, LEVELS };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.NomadRoute = api;
})(typeof self !== 'undefined' ? self : this);
