/* Version 4 - Nomad 3D model viewer.

   Parses STL / OBJ / 3MF and draws them with raw WebGL. No three.js: everything on
   the Nomad works off the card with no internet, and a general purpose 3D engine is
   a megabyte of code to do what a part preview needs about three hundred lines for.

   Meshes come out as flat triangle soup, no index buffer. Printable models are
   faceted anyway (STL has one normal per facet) so there is nothing to gain by
   welding vertices, and skipping it keeps parsing linear.

   Usage:
     const mesh = await NomadModel.load('/Workshop/bracket.stl', p => {...});
     const view = new NomadModel.Viewer(canvasEl);
     view.setMesh(mesh);
*/
(function (global) {
  'use strict';

  // past this the vertex arrays need more memory than a phone browser will reliably
  // hand out. better to say so than to be killed mid-parse with no explanation
  const MAX_TRIANGLES = 1200000;

  /* ---- small vector / matrix helpers (column-major, as WebGL wants) ---- */

  function mat4Perspective(fovy, aspect, near, far) {
    const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
    return new Float32Array([
      f / aspect, 0, 0, 0,
      0, f, 0, 0,
      0, 0, (far + near) * nf, -1,
      0, 0, 2 * far * near * nf, 0
    ]);
  }

  function mat4LookAt(eye, center, up) {
    let zx = eye[0] - center[0], zy = eye[1] - center[1], zz = eye[2] - center[2];
    let len = Math.hypot(zx, zy, zz) || 1;
    zx /= len; zy /= len; zz /= len;

    let xx = up[1] * zz - up[2] * zy, xy = up[2] * zx - up[0] * zz, xz = up[0] * zy - up[1] * zx;
    len = Math.hypot(xx, xy, xz);
    if (!len) { xx = 1; xy = 0; xz = 0; } else { xx /= len; xy /= len; xz /= len; }

    const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;

    return new Float32Array([
      xx, yx, zx, 0,
      xy, yy, zy, 0,
      xz, yz, zz, 0,
      -(xx * eye[0] + xy * eye[1] + xz * eye[2]),
      -(yx * eye[0] + yy * eye[1] + yz * eye[2]),
      -(zx * eye[0] + zy * eye[1] + zz * eye[2]),
      1
    ]);
  }

  /* ---- mesh assembly ---- */

  /* Growable triangle sink. Parsers push raw vertices, this works out the bounding
     box as it goes and fills in a face normal for any triangle whose format did not
     supply one (most OBJ files, and every degenerate facet in the wild). */
  function MeshBuilder(estimatedTriangles) {
    const cap = Math.max(3, (estimatedTriangles || 1024) * 3);
    this.pos = new Float32Array(cap * 3);
    this.nrm = new Float32Array(cap * 3);
    this.count = 0;                                   // vertices written
    this.min = [Infinity, Infinity, Infinity];
    this.max = [-Infinity, -Infinity, -Infinity];
  }

  MeshBuilder.prototype._grow = function (need) {
    if ((this.count + need) * 3 <= this.pos.length) return;
    let cap = this.pos.length;
    while (cap < (this.count + need) * 3) cap *= 2;
    const p = new Float32Array(cap), n = new Float32Array(cap);
    p.set(this.pos); n.set(this.nrm);
    this.pos = p; this.nrm = n;
  };

  /* one triangle. nx/ny/nz optional - omitted means "work it out from the
     winding", which is what STL asks for when its stored normal is zero */
  MeshBuilder.prototype.tri = function (ax, ay, az, bx, by, bz, cx, cy, cz, nx, ny, nz) {
    if (this.count / 3 >= MAX_TRIANGLES) return false;
    this._grow(3);

    if (nx === undefined || (nx === 0 && ny === 0 && nz === 0)) {
      const ux = bx - ax, uy = by - ay, uz = bz - az;
      const vx = cx - ax, vy = cy - ay, vz = cz - az;
      nx = uy * vz - uz * vy; ny = uz * vx - ux * vz; nz = ux * vy - uy * vx;
      const l = Math.hypot(nx, ny, nz);
      if (l > 0) { nx /= l; ny /= l; nz /= l; } else { nx = 0; ny = 0; nz = 1; }
    }

    const o = this.count * 3;
    const p = this.pos, n = this.nrm;
    p[o] = ax; p[o + 1] = ay; p[o + 2] = az;
    p[o + 3] = bx; p[o + 4] = by; p[o + 5] = bz;
    p[o + 6] = cx; p[o + 7] = cy; p[o + 8] = cz;
    for (let i = 0; i < 3; i++) { n[o + i * 3] = nx; n[o + i * 3 + 1] = ny; n[o + i * 3 + 2] = nz; }
    this.count += 3;

    const mn = this.min, mx = this.max;
    if (ax < mn[0]) mn[0] = ax; if (ax > mx[0]) mx[0] = ax;
    if (ay < mn[1]) mn[1] = ay; if (ay > mx[1]) mx[1] = ay;
    if (az < mn[2]) mn[2] = az; if (az > mx[2]) mx[2] = az;
    if (bx < mn[0]) mn[0] = bx; if (bx > mx[0]) mx[0] = bx;
    if (by < mn[1]) mn[1] = by; if (by > mx[1]) mx[1] = by;
    if (bz < mn[2]) mn[2] = bz; if (bz > mx[2]) mx[2] = bz;
    if (cx < mn[0]) mn[0] = cx; if (cx > mx[0]) mx[0] = cx;
    if (cy < mn[1]) mn[1] = cy; if (cy > mx[1]) mx[1] = cy;
    if (cz < mn[2]) mn[2] = cz; if (cz > mx[2]) mx[2] = cz;
    return true;
  };

  /* Bake the model to a unit-ish size centred on the origin so the camera can use one
     set of distances for a 4 mm screw and a 300 mm helmet. Real dimensions are kept
     for display, those are what someone printing it cares about. */
  MeshBuilder.prototype.finish = function (units) {
    const n = this.count;
    if (!n) throw new Error('no triangles found');
    const mn = this.min, mx = this.max;
    const size = [mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]];
    const ctr = [(mx[0] + mn[0]) / 2, (mx[1] + mn[1]) / 2, (mx[2] + mn[2]) / 2];
    const radius = Math.max(1e-6, Math.hypot(size[0], size[1], size[2]) / 2);

    const pos = this.pos.subarray(0, n * 3);
    for (let i = 0; i < n * 3; i += 3) {
      pos[i]     = (pos[i]     - ctr[0]) / radius;
      pos[i + 1] = (pos[i + 1] - ctr[1]) / radius;
      pos[i + 2] = (pos[i + 2] - ctr[2]) / radius;
    }

    return {
      positions: pos,
      normals: this.nrm.subarray(0, n * 3),
      triangles: n / 3,
      size: size,
      units: units || 'mm',
      truncated: n / 3 >= MAX_TRIANGLES
    };
  };

  /* ---- STL ---- */

  /* Binary or ASCII? In a binary STL the triangle count at byte 80 has to account for
     the file's exact length, which is decisive whenever it holds. Plenty of binary
     files also start with the word "solid", so the leading text is only consulted
     when the arithmetic does not settle it. */
  function stlIsBinary(buf) {
    if (buf.byteLength < 84) return false;
    const tris = new DataView(buf).getUint32(80, true);
    if (84 + tris * 50 === buf.byteLength) return true;

    const head = new Uint8Array(buf, 0, Math.min(buf.byteLength, 512));
    let txt = '';
    for (let i = 0; i < head.length; i++) txt += String.fromCharCode(head[i]);
    if (/^\s*solid/.test(txt) && /\bfacet\b|\bendsolid\b/i.test(txt)) return false;
    return true;
  }

  function parseSTLBinary(buf) {
    const dv = new DataView(buf);
    const tris = dv.getUint32(80, true);
    const mb = new MeshBuilder(Math.min(tris, MAX_TRIANGLES));
    let off = 84;
    for (let i = 0; i < tris; i++) {
      if (off + 50 > buf.byteLength) break;                 // truncated file, keep what we have
      const nx = dv.getFloat32(off, true), ny = dv.getFloat32(off + 4, true), nz = dv.getFloat32(off + 8, true);
      if (!mb.tri(
        dv.getFloat32(off + 12, true), dv.getFloat32(off + 16, true), dv.getFloat32(off + 20, true),
        dv.getFloat32(off + 24, true), dv.getFloat32(off + 28, true), dv.getFloat32(off + 32, true),
        dv.getFloat32(off + 36, true), dv.getFloat32(off + 40, true), dv.getFloat32(off + 44, true),
        nx, ny, nz)) break;
      off += 50;
    }
    return mb.finish('mm');
  }

  function parseSTLAscii(text) {
    const mb = new MeshBuilder(2048);
    // one pass over the numbers, rather than a regex per facet: same result,
    // and it doesn't fall over on the many hand-written variants of the format
    const facetRe = /facet[\s\S]*?endfacet/gi;
    const numRe = /-?[\d.]+(?:[eE][-+]?\d+)?/g;
    let m;
    while ((m = facetRe.exec(text))) {
      const nums = m[0].match(numRe);
      if (!nums || nums.length < 12) continue;
      const v = nums.map(Number);
      // normal is nums[0..2], then three vertices
      if (!mb.tri(v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[0], v[1], v[2])) break;
    }
    return mb.finish('mm');
  }

  function parseSTL(buf) {
    if (stlIsBinary(buf)) return parseSTLBinary(buf);
    return parseSTLAscii(new TextDecoder().decode(buf));
  }

  /* ---- OBJ ---- */

  function parseOBJ(text) {
    const vx = [], vn = [];
    const mb = new MeshBuilder(4096);

    for (const rawLine of text.split(/\r?\n/)) {
      const line = rawLine.trim();
      if (!line || line.charCodeAt(0) === 35) continue;       // '#'
      const sp = line.indexOf(' ');
      if (sp < 0) continue;
      const tag = line.slice(0, sp);

      if (tag === 'v') {
        const p = line.slice(sp + 1).trim().split(/\s+/);
        vx.push(+p[0], +p[1], +p[2]);
      } else if (tag === 'vn') {
        const p = line.slice(sp + 1).trim().split(/\s+/);
        vn.push(+p[0], +p[1], +p[2]);
      } else if (tag === 'f') {
        const parts = line.slice(sp + 1).trim().split(/\s+/);
        if (parts.length < 3) continue;

        // "v", "v/vt", "v//vn", "v/vt/vn"; indices are 1-based and may be
        // negative (counting back from the end of the list so far)
        const idx = parts.map(tok => {
          const bits = tok.split('/');
          let vi = parseInt(bits[0], 10);
          let ni = bits.length > 2 && bits[2] ? parseInt(bits[2], 10) : NaN;
          if (vi < 0) vi = vx.length / 3 + vi + 1;
          if (ni < 0) ni = vn.length / 3 + ni + 1;
          return { v: (vi - 1) * 3, n: isNaN(ni) ? -1 : (ni - 1) * 3 };
        });

        // fan-triangulate anything with more than three corners
        for (let i = 1; i + 1 < idx.length; i++) {
          const a = idx[0], b = idx[i], c = idx[i + 1];
          if (a.v < 0 || b.v < 0 || c.v < 0) continue;
          let nx, ny, nz;
          if (a.n >= 0 && a.n + 2 < vn.length) { nx = vn[a.n]; ny = vn[a.n + 1]; nz = vn[a.n + 2]; }
          if (!mb.tri(
            vx[a.v], vx[a.v + 1], vx[a.v + 2],
            vx[b.v], vx[b.v + 1], vx[b.v + 2],
            vx[c.v], vx[c.v + 1], vx[c.v + 2],
            nx, ny, nz)) return mb.finish('units');
        }
      }
    }
    return mb.finish('units');
  }

  /* ---- 3MF ---- */

  function loadScriptOnce(src) {
    loadScriptOnce._p = loadScriptOnce._p || {};
    if (!loadScriptOnce._p[src]) {
      loadScriptOnce._p[src] = new Promise((res, rej) => {
        const s = document.createElement('script');
        s.src = src; s.async = true;
        s.onload = res;
        s.onerror = () => { delete loadScriptOnce._p[src]; rej(new Error('failed to load ' + src)); };
        document.head.appendChild(s);
      });
    }
    return loadScriptOnce._p[src];
  }

  /* A 3MF is a zip with an XML mesh inside. Objects are placed on the plate by
     <build><item>, each with an optional 4x3 row-major transform, which is what puts a
     multi-part plate together instead of stacking every part on the origin. Nested
     <components> are not followed, those objects still draw at their own origin. */
  async function parse3MF(buf) {
    if (!global.unzipit) await loadScriptOnce('/assets/unzipit.min.js');
    if (!global.unzipit) throw new Error('3MF support needs /assets/unzipit.min.js on the card');

    const { entries } = await global.unzipit.unzip(buf);
    const names = Object.keys(entries);
    const modelName = names.find(n => /^3d\/3dmodel\.model$/i.test(n)) ||
                      names.find(n => /\.model$/i.test(n));
    if (!modelName) throw new Error('no 3D model inside this 3MF');

    const xml = await entries[modelName].text();
    const doc = new DOMParser().parseFromString(xml, 'application/xml');
    if (doc.querySelector('parsererror')) throw new Error('3MF model XML is malformed');

    const unit = (doc.documentElement.getAttribute('unit') || 'millimeter').toLowerCase();
    const units = unit.startsWith('milli') ? 'mm' : unit.startsWith('cent') ? 'cm'
                : unit.startsWith('inch') ? 'in' : unit.startsWith('meter') ? 'm' : unit;

    const objects = new Map();                     // id -> {verts:Float64Array, tris:Int32Array}
    for (const obj of doc.getElementsByTagName('object')) {
      const mesh = obj.getElementsByTagName('mesh')[0];
      if (!mesh) continue;
      const vEls = mesh.getElementsByTagName('vertex');
      const tEls = mesh.getElementsByTagName('triangle');
      const verts = new Float64Array(vEls.length * 3);
      for (let i = 0; i < vEls.length; i++) {
        verts[i * 3]     = parseFloat(vEls[i].getAttribute('x')) || 0;
        verts[i * 3 + 1] = parseFloat(vEls[i].getAttribute('y')) || 0;
        verts[i * 3 + 2] = parseFloat(vEls[i].getAttribute('z')) || 0;
      }
      const tris = new Int32Array(tEls.length * 3);
      for (let i = 0; i < tEls.length; i++) {
        tris[i * 3]     = parseInt(tEls[i].getAttribute('v1'), 10);
        tris[i * 3 + 1] = parseInt(tEls[i].getAttribute('v2'), 10);
        tris[i * 3 + 2] = parseInt(tEls[i].getAttribute('v3'), 10);
      }
      objects.set(obj.getAttribute('id'), { verts, tris });
    }
    if (!objects.size) throw new Error('3MF contains no mesh objects');

    // row-vector convention: v' = v * M + t
    const applyXf = (m, x, y, z) => m
      ? [x * m[0] + y * m[3] + z * m[6] + m[9],
         x * m[1] + y * m[4] + z * m[7] + m[10],
         x * m[2] + y * m[5] + z * m[8] + m[11]]
      : [x, y, z];

    let total = 0;
    for (const o of objects.values()) total += o.tris.length / 3;
    const mb = new MeshBuilder(Math.min(total, MAX_TRIANGLES));

    const emit = (obj, m) => {
      const v = obj.verts, t = obj.tris;
      for (let i = 0; i < t.length; i += 3) {
        const a = t[i] * 3, b = t[i + 1] * 3, c = t[i + 2] * 3;
        if (a < 0 || b < 0 || c < 0 || a + 2 >= v.length || b + 2 >= v.length || c + 2 >= v.length) continue;
        const A = applyXf(m, v[a], v[a + 1], v[a + 2]);
        const B = applyXf(m, v[b], v[b + 1], v[b + 2]);
        const C = applyXf(m, v[c], v[c + 1], v[c + 2]);
        if (!mb.tri(A[0], A[1], A[2], B[0], B[1], B[2], C[0], C[1], C[2])) return false;
      }
      return true;
    };

    const items = doc.getElementsByTagName('item');
    let placed = false;
    for (const item of items) {
      const obj = objects.get(item.getAttribute('objectid'));
      if (!obj) continue;
      const xf = item.getAttribute('transform');
      const m = xf ? xf.trim().split(/\s+/).map(Number) : null;
      placed = true;
      if (!emit(obj, m && m.length >= 12 ? m : null)) break;
    }
    // no build section (or it referenced nothing we have): draw everything as-is
    if (!placed) for (const obj of objects.values()) if (!emit(obj, null)) break;

    return mb.finish(units);
  }

  /* ---- loading ---- */

  const SUPPORTED = ['stl', 'obj', '3mf'];

  function extOf(path) {
    const m = String(path).toLowerCase().match(/\.([a-z0-9]+)$/);
    return m ? m[1] : '';
  }

  /* Fetch with byte progress where the server gives a length. Model files are the
     biggest thing this page reads off the card, and over the Nomad's own wifi a 30 MB
     STL is a real wait. a silent one looks like a hang. */
  async function fetchBuffer(url, onProgress) {
    // the caller passes a ready URL (workshop uses /media?file=<encoded>);
    // re-encoding here turned the escapes into %25 and made every load a 404
    const res = await fetch(url, { cache: 'no-store' });
    if (!res.ok) throw new Error('could not read the file (HTTP ' + res.status + ')');

    const total = +res.headers.get('Content-Length') || 0;
    if (!res.body || !total || !res.body.getReader) return await res.arrayBuffer();

    const reader = res.body.getReader();
    const chunks = [];
    let got = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      got += value.length;
      if (onProgress) onProgress(got, total);
    }
    const out = new Uint8Array(got);
    let o = 0;
    for (const c of chunks) { out.set(c, o); o += c.length; }
    return out.buffer;
  }

  async function load(url, onProgress) {
    const ext = extOf(url);
    if (SUPPORTED.indexOf(ext) < 0) throw new Error('no viewer for .' + ext + ' files');
    const buf = await fetchBuffer(url, onProgress);
    if (ext === 'stl') return parseSTL(buf);
    if (ext === 'obj') return parseOBJ(new TextDecoder().decode(buf));
    return await parse3MF(buf);
  }

  /* ---- viewer ---- */

  const VERT_SRC = `
    attribute vec3 aPos;
    attribute vec3 aNormal;
    uniform mat4 uProj;
    uniform mat4 uView;
    varying vec3 vNormal;
    varying vec3 vEye;
    void main() {
      vec4 p = uView * vec4(aPos, 1.0);
      vEye = p.xyz;
      vNormal = mat3(uView) * aNormal;
      gl_Position = uProj * p;
    }`;

  /* Two soft lights plus a rim, deliberately matte. A printable part is read for its
     shape, so the shading is there to make edges and overhangs legible rather than to
     look like a render. gl_FrontFacing flips the normal on back faces because inverted
     facets are common in downloaded STLs and an unlit black patch reads as a hole. */
  const FRAG_SRC = `
    precision mediump float;
    varying vec3 vNormal;
    varying vec3 vEye;
    uniform vec3 uColor;
    void main() {
      vec3 n = normalize(vNormal);
      if (!gl_FrontFacing) n = -n;
      vec3 key  = normalize(vec3( 0.35,  0.65, 0.70));
      vec3 fill = normalize(vec3(-0.60, -0.25, 0.45));
      float d = max(dot(n, key), 0.0) * 0.72 + max(dot(n, fill), 0.0) * 0.26;
      vec3 v = normalize(-vEye);
      float rim = pow(1.0 - max(dot(n, v), 0.0), 2.5) * 0.22;
      gl_FragColor = vec4(uColor * (0.34 + d) + rim, 1.0);
    }`;

  function compile(gl, type, src) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(s);
      gl.deleteShader(s);
      throw new Error('shader failed to compile: ' + log);
    }
    return s;
  }

  function Viewer(canvas, opts) {
    opts = opts || {};
    const gl = canvas.getContext('webgl', { antialias: true, alpha: false, preserveDrawingBuffer: false })
            || canvas.getContext('experimental-webgl');
    if (!gl) throw new Error('this browser has no WebGL');

    this.canvas = canvas;
    this.gl = gl;
    this.mesh = null;
    this.color = opts.color || [0.42, 0.60, 0.86];
    this.background = opts.background || [0.08, 0.09, 0.11];

    // orbit state: azimuth, elevation, distance in normalized model radii
    this.az = Math.PI * 0.25;
    this.el = Math.PI * 0.18;
    this.dist = 3.1;
    this.pan = [0, 0];
    this._raf = null;

    const prog = gl.createProgram();
    gl.attachShader(prog, compile(gl, gl.VERTEX_SHADER, VERT_SRC));
    gl.attachShader(prog, compile(gl, gl.FRAGMENT_SHADER, FRAG_SRC));
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error('shader link failed: ' + gl.getProgramInfoLog(prog));
    gl.useProgram(prog);

    this.prog = prog;
    this.loc = {
      pos:    gl.getAttribLocation(prog, 'aPos'),
      normal: gl.getAttribLocation(prog, 'aNormal'),
      proj:   gl.getUniformLocation(prog, 'uProj'),
      view:   gl.getUniformLocation(prog, 'uView'),
      color:  gl.getUniformLocation(prog, 'uColor')
    };
    this.posBuf = gl.createBuffer();
    this.nrmBuf = gl.createBuffer();

    // depth only. Back-face culling stays off on purpose - see FRAG_SRC: models
    // with inverted facets would otherwise come out full of holes.
    gl.enable(gl.DEPTH_TEST);

    this._bindInput();
    this._onResize = () => { this.render(); };
    window.addEventListener('resize', this._onResize);
  }

  Viewer.prototype.setMesh = function (mesh) {
    const gl = this.gl;
    this.mesh = mesh;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.posBuf);
    gl.bufferData(gl.ARRAY_BUFFER, mesh.positions, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.nrmBuf);
    gl.bufferData(gl.ARRAY_BUFFER, mesh.normals, gl.STATIC_DRAW);
    this.resetView();
  };

  Viewer.prototype.resetView = function () {
    this.az = Math.PI * 0.25;
    this.el = Math.PI * 0.18;
    this.dist = 3.1;
    this.pan = [0, 0];
    this.render();
  };

  Viewer.prototype.setColor = function (rgb) { this.color = rgb; this.render(); };

  Viewer.prototype.render = function () {
    if (this._raf) return;
    this._raf = requestAnimationFrame(() => {
      this._raf = null;
      this._draw();
    });
  };

  Viewer.prototype._draw = function () {
    const gl = this.gl, canvas = this.canvas;

    // match the drawing buffer to the element's real pixel size, capped so a
    // hi-dpi phone doesn't try to shade four times the pixels it can afford
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(canvas.clientWidth * dpr));
    const h = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    gl.viewport(0, 0, w, h);

    gl.clearColor(this.background[0], this.background[1], this.background[2], 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (!this.mesh) return;

    const eye = [
      this.dist * Math.cos(this.el) * Math.sin(this.az),
      this.dist * Math.sin(this.el),
      this.dist * Math.cos(this.el) * Math.cos(this.az)
    ];
    const center = [this.pan[0], this.pan[1], 0];
    eye[0] += this.pan[0];
    eye[1] += this.pan[1];

    const proj = mat4Perspective(35 * Math.PI / 180, w / h, 0.05, 100);
    const view = mat4LookAt(eye, center, [0, 1, 0]);

    gl.useProgram(this.prog);
    gl.uniformMatrix4fv(this.loc.proj, false, proj);
    gl.uniformMatrix4fv(this.loc.view, false, view);
    gl.uniform3fv(this.loc.color, this.color);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.posBuf);
    gl.enableVertexAttribArray(this.loc.pos);
    gl.vertexAttribPointer(this.loc.pos, 3, gl.FLOAT, false, 0, 0);

    gl.bindBuffer(gl.ARRAY_BUFFER, this.nrmBuf);
    gl.enableVertexAttribArray(this.loc.normal);
    gl.vertexAttribPointer(this.loc.normal, 3, gl.FLOAT, false, 0, 0);

    gl.drawArrays(gl.TRIANGLES, 0, this.mesh.triangles * 3);
  };

  /* Drag to orbit, wheel or pinch to zoom, two fingers or right-drag to pan. Pointer
     events cover mouse and touch together, and touch-action:none on the canvas is what
     stops a drag from scrolling the page instead. */
  Viewer.prototype._bindInput = function () {
    const c = this.canvas, self = this;
    const active = new Map();
    let lastPinch = 0, mode = null;

    c.style.touchAction = 'none';

    const center = () => {
      const pts = [...active.values()];
      return [pts.reduce((s, p) => s + p.x, 0) / pts.length, pts.reduce((s, p) => s + p.y, 0) / pts.length];
    };
    const spread = () => {
      const pts = [...active.values()];
      return pts.length < 2 ? 0 : Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y);
    };

    c.addEventListener('pointerdown', e => {
      c.setPointerCapture(e.pointerId);
      active.set(e.pointerId, { x: e.clientX, y: e.clientY });
      mode = (e.button === 2 || e.shiftKey) ? 'pan' : 'orbit';
      if (active.size === 2) { mode = 'pinch'; lastPinch = spread(); }
    });

    c.addEventListener('pointermove', e => {
      const prev = active.get(e.pointerId);
      if (!prev) return;
      const prevCenter = center(), prevSpread = spread();
      active.set(e.pointerId, { x: e.clientX, y: e.clientY });

      if (active.size >= 2) {
        const s = spread();
        if (prevSpread > 0 && s > 0) self.dist = Math.min(20, Math.max(1.2, self.dist * (prevSpread / s)));
        const nc = center();
        self.pan[0] -= (nc[0] - prevCenter[0]) / c.clientHeight * self.dist;
        self.pan[1] += (nc[1] - prevCenter[1]) / c.clientHeight * self.dist;
      } else if (mode === 'pan') {
        self.pan[0] -= (e.clientX - prev.x) / c.clientHeight * self.dist;
        self.pan[1] += (e.clientY - prev.y) / c.clientHeight * self.dist;
      } else {
        self.az -= (e.clientX - prev.x) * 0.008;
        self.el += (e.clientY - prev.y) * 0.008;
        const lim = Math.PI / 2 - 0.02;
        self.el = Math.max(-lim, Math.min(lim, self.el));
      }
      self.render();
    });

    const release = e => {
      active.delete(e.pointerId);
      if (active.size < 2) mode = active.size ? 'orbit' : null;
    };
    c.addEventListener('pointerup', release);
    c.addEventListener('pointercancel', release);
    c.addEventListener('contextmenu', e => e.preventDefault());

    c.addEventListener('wheel', e => {
      e.preventDefault();
      self.dist = Math.min(20, Math.max(1.2, self.dist * (e.deltaY > 0 ? 1.12 : 0.89)));
      self.render();
    }, { passive: false });
  };

  Viewer.prototype.destroy = function () {
    window.removeEventListener('resize', this._onResize);
    if (this._raf) cancelAnimationFrame(this._raf);
    const gl = this.gl;
    try {
      gl.deleteBuffer(this.posBuf);
      gl.deleteBuffer(this.nrmBuf);
      gl.deleteProgram(this.prog);
      // free the drawing buffer now rather than waiting for GC. a few large models opened
      // in a row will otherwise hit the browser's WebGL context limit and every later
      // canvas silently fails to initialise
      const ext = gl.getExtension('WEBGL_lose_context');
      if (ext) ext.loseContext();
    } catch (e) {}
    this.mesh = null;
  };

  global.NomadModel = {
    load, parseSTL, parseOBJ, parse3MF, Viewer,
    SUPPORTED, MAX_TRIANGLES, extOf
  };
})(window);
