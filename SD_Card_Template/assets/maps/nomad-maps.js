/* nomad-maps.js - the Maps page: every map pack on the card as one map, with search,
   directions and turn-by-turn, offline.

   Packs live in /Maps/<id>/ with a manifest.json (Nomad-Tools build_map_pack.py): a
   vector map (.pmtiles), and optionally routing (nomad-route.js) and places. "All maps"
   takes each tile from the most detailed pack that has it, so overviews and state packs
   read as one map. Older raster regions (a tiles/ folder of images) open as a layer.

   Big files go through /media?file=, which honours Range on every firmware. The plain
   path doesn't on older firmware. */
import * as maplibregl from './maplibre-gl.mjs';

const $ = (id) => document.getElementById(id);
const NR = window.NomadRoute;
const icon = (name, size) => (window.NomadIcons ? window.NomadIcons.svg(name, size) : '');
const store = {
  get(k, d) { try { const v = localStorage.getItem('nomad-maps-' + k); return v == null ? d : JSON.parse(v); } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem('nomad-maps-' + k, JSON.stringify(v)); } catch (e) {} },
};
const media = (path) => '/media?file=' + encodeURIComponent(path);
const abs = (path) => location.origin + path;

const prefs = Object.assign({ flavor: 'auto', labels: true, pois: true, buildings: true, outlines: false,
  units: 'mi', voice: true, raster: [], pack: 'all', mode: 'drive', route: 'fast', trails: true, walk: 'fast', coords: 'dd' },
  store.get('prefs', {}));
const savePrefs = () => store.set('prefs', prefs);

function fmtDist(m) {
  if (prefs.units === 'km') return m < 950 ? Math.round(m / 10) * 10 + ' m' : (m / 1000).toFixed(m < 9500 ? 1 : 0) + ' km';
  const ft = m * 3.28084, mi = m / 1609.344;
  return mi < 0.1 ? Math.max(10, Math.round(ft / 10) * 10) + ' ft' : mi.toFixed(mi < 9.5 ? 1 : 0) + ' mi';
}
function fmtTime(s) {
  const m = Math.round(s / 60);
  if (m < 1) return '1 min';
  return m < 60 ? m + ' min' : Math.floor(m / 60) + ' h ' + (m % 60) + ' min';
}
// positions in the format chosen in Layers: lat/lon, degrees minutes seconds, UTM or MGRS
function fmtLL(lat, lon) { return window.NomadCoords ? NomadCoords.format(lat, lon, prefs.coords) : lat.toFixed(5) + ', ' + lon.toFixed(5); }
function esc(s) { return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

let toastT = null;
function toast(msg, ms = 2600) {
  const t = $('toast');
  if (!msg) { t.classList.remove('show'); return; }
  t.textContent = msg; t.classList.add('show');
  clearTimeout(toastT); if (ms) toastT = setTimeout(() => t.classList.remove('show'), ms);
}

function whyFailed(e) {
  const m = String((e && e.message) || e || '');
  if (/busy/i.test(m)) return "Nomad's card is busy. Try again in a moment.";
  if (e instanceof TypeError || /fetch|network|load failed/i.test(m)) return "Lost touch with Nomad. Check this phone is still on Nomad's WiFi, then try again.";
  if (/HTTP 404/.test(m)) return 'A map file is missing from the card. Copy the map pack again with Nomad-Tools.';
  if (/HTTP 5\d\d/.test(m)) return "Nomad couldn't read the card. Try again, or restart Nomad.";
  return 'Something went wrong (' + esc(m) + '). Try again.';
}
const errBox = (msg, retryId) => `<div class="err"><span class="ic">${icon('warning', 18)}</span><div>${msg}${retryId ? `<div><button class="btn" id="${retryId}">Try again</button></div>` : ''}</div></div>`;
// a sheet's title row, with its own close button so nothing sits on top of anything else
const head = (title, sub, close = true) => `<div class="sh"><div class="t"><h2>${title}</h2>${sub ? `<div class="sub">${sub}</div>` : ''}</div>`
  + (close ? `<button class="ib x" data-close aria-label="Close">${icon('x', 20)}</button>` : '') + '</div>';

let packs = [];          // every pack: {id, name, base, man, pm?, router-ready?}
let rasters = [];        // older tile-folder regions
async function fetchJSON(url, tries = 4) {
  let wait = 400;
  for (let i = 0; i < tries; i++) {
    try {
      const r = await fetch(url, { cache: 'no-cache' });
      if (r.ok) return await r.json();
      if (r.status !== 503) throw new Error('HTTP ' + r.status);
    } catch (e) { if (i === tries - 1) throw e; }
    await new Promise((res) => setTimeout(res, wait)); wait *= 1.8;
  }
  throw new Error('busy');
}
async function loadPacks() {
  const list = (await fetchJSON('/api/maps-list')).regions || [];
  const mans = await Promise.all(list.map((r) => fetchJSON(r.manifest).then((m) => [r, m]).catch(() => null)));
  for (const it of mans) {
    if (!it) continue;
    const [r, m] = it, base = r.manifest.replace(/manifest\.json$/, '');
    if (m.type === 'pack') packs.push({ id: m.id || r.id, name: m.name || r.id, base, man: m });
    else if (m.tilePath) rasters.push({ id: m.id || r.id, name: m.name || r.id, base, man: m });
  }
  // most detailed first, then smallest area: that is the order a tile is looked for in
  const area = (b) => (b[2] - b[0]) * (b[3] - b[1]);
  packs.sort((a, b) => ((b.man.maxZoom || 0) - (a.man.maxZoom || 0)) || (area(a.man.bounds) - area(b.man.bounds)));
  for (const p of packs) {
    if (p.man.map) p.pm = new pmtiles.PMTiles(new pmtiles.FetchSource(abs(media(p.base + p.man.map.file))));
    if (p.man.cover) p.cover = new Set(p.man.cover.tiles.map(([x, y]) => x + '/' + y));
    // shared blocks: a state lists the grid squares of street-level map it uses. a square
    // on a border is one file both neighbours list (Nomad-Tools map_catalog.py)
    const bl = p.man.blocks;
    if (bl) {
      // packs can cut their blocks to different depths (street level 14, town level 12).
      // a block holds the deepest any pack listing it asked for
      blockSpec = { zoom: bl.zoom, minZoom: bl.minZoom, maxZoom: Math.max(bl.maxZoom, blockSpec ? blockSpec.maxZoom : 0), dir: bl.dir };
      for (const [x, y] of bl.tiles) {
        const key = x + '/' + y;
        const b = blocks.get(key) || { path: `${bl.dir}/${bl.zoom}/${x}/${y}.pmtiles`, packs: new Set(), pm: null, maxZoom: 0 };
        b.packs.add(p.id); b.maxZoom = Math.max(b.maxZoom, bl.maxZoom); blocks.set(key, b);
      }
    }
  }
  const missing = new Set();
  for (const p of packs) for (const r of p.man.requires || []) if (!packs.some((q) => q.id === r)) missing.add(r);
  if (missing.size) setTimeout(() => toast(`Some maps need the ${[...missing].join(', ')} pack${missing.size > 1 ? 's' : ''} too. Add ${missing.size > 1 ? 'them' : 'it'} with Nomad-Tools.`, 5000), 1500);
}
const blocks = new Map();                 // 'x/y' on blockSpec.zoom's grid -> {path, packs, pm}
let blockSpec = null;
// one pack chosen: it and the packs it builds on (a state shows the world and US overview around it)
function activePacks() {
  if (prefs.pack === 'all') return packs;
  const want = new Set([prefs.pack]);
  for (const id of [...want]) for (const r of (packs.find((p) => p.id === id) || { man: {} }).man.requires || []) want.add(r);
  return packs.filter((p) => want.has(p.id));
}
// one pack chosen: the packs under it fill in only the zooms above it, so the chosen pack is
// the detail you see. A world archive can carry street level for a whole country.
function packMaxZoom(p) {
  const z = p.man.maxZoom || 0;
  if (prefs.pack === 'all' || p.id === prefs.pack) return z;
  const sel = packs.find((q) => q.id === prefs.pack);
  const top = sel ? (sel.man.blocks ? sel.man.blocks.minZoom : sel.man.minZoom) || 0 : 0;
  return top ? Math.min(z, top - 1) : z;
}
// the block holding tile z/x/y (z at or past the block grid's zoom), if a chosen pack has it
function blockAt(z, x, y) {
  if (!blockSpec || z < blockSpec.minZoom || z > blockSpec.maxZoom) return null;
  const k = z - blockSpec.zoom, b = blocks.get((x >> k) + '/' + (y >> k));
  if (!b || z > b.maxZoom || (prefs.pack !== 'all' && !b.packs.has(prefs.pack))) return null;
  if (!b.pm) b.pm = new pmtiles.PMTiles(new pmtiles.FetchSource(abs(media(b.path))));
  return b;
}
function lonLatTile(lng, lat, z) {
  const n = 2 ** z, r = lat * Math.PI / 180;
  return [Math.floor((lng + 180) / 360 * n), Math.floor((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2 * n)];
}
// a pack with a cover grid only has tiles in those squares (its bounds can run around the world)
function covers(p, lng, lat) {
  if (!p.cover) return true;
  const [x, y] = lonLatTile(lng, lat, p.man.cover.zoom);
  return p.cover.has(x + '/' + y);
}

/* one tile from the best pack that has it */
function tileBox(z, x, y) {
  const n = 2 ** z, lon = (xx) => xx / n * 360 - 180;
  const lat = (yy) => Math.atan(Math.sinh(Math.PI * (1 - 2 * yy / n))) * 180 / Math.PI;
  return [lon(x), lat(y + 1), lon(x + 1), lat(y)];
}
const hits = (b, t) => !(t[2] < b[0] || t[0] > b[2] || t[3] < b[1] || t[1] > b[3]);
async function tileData(z, x, y, signal) {
  const tb = tileBox(z, x, y);
  const b = blockAt(z, x, y);
  if (b) {
    try {
      const r = await b.pm.getZxy(z, x, y, signal);
      if (r && r.data) return r.data;
    } catch (e) { if (e.name === 'AbortError') throw e; }
  }
  for (const p of activePacks()) {
    if (!p.pm || z < (p.man.minZoom || 0) || z > packMaxZoom(p) || !hits(p.man.bounds, tb)) continue;
    try {
      const r = await p.pm.getZxy(z, x, y, signal);
      if (r && r.data) return r.data;
    } catch (e) { if (e.name === 'AbortError') throw e; }
  }
  return null;
}
// past the deepest zoom a spot has (a continent overview stops at 9), the tile is made from
// its parent a level or two up: the same map, closer (nomad-overzoom.js)
const OVERZOOM = 2;
maplibregl.addProtocol('nomad', async (params, ctl) => {
  const m = /nomad:\/\/tiles\/(\d+)\/(\d+)\/(\d+)/.exec(params.url);
  const z = +m[1], x = +m[2], y = +m[3], signal = ctl && ctl.signal;
  const data = await tileData(z, x, y, signal);
  if (data) return { data };
  for (let dz = 1; dz <= OVERZOOM && z - dz >= 0 && window.NomadOverzoom; dz++) {
    const up = await tileData(z - dz, x >> dz, y >> dz, signal);
    if (up) return { data: NomadOverzoom.child(new Uint8Array(up), dz, x - ((x >> dz) << dz), y - ((y >> dz) << dz)) };
  }
  return { data: new Uint8Array(0) };
});

/* the deepest zoom any active pack has here, beyond it the map would go blank */
function detailAt(lng, lat) {
  let best = 0;
  const inside = (b) => lng >= b[0] && lng <= b[2] && lat >= b[1] && lat <= b[3];
  // a pack with a cover grid has its full depth only there, elsewhere what cover.elsewhere says
  // (a world archive: the lower 48 to 14, the rest to 8), or nothing (an overview)
  for (const p of activePacks()) {
    if (!p.pm || !inside(p.man.bounds)) continue;
    const z = covers(p, lng, lat) ? p.man.maxZoom : (p.man.cover && p.man.cover.elsewhere) || 0;
    best = Math.max(best, Math.min(z, packMaxZoom(p)));
  }
  if (blockSpec) {
    const [x, y] = lonLatTile(lng, lat, blockSpec.zoom);
    const b = blocks.get(x + '/' + y);
    if (b && (prefs.pack === 'all' || b.packs.has(prefs.pack))) best = Math.max(best, b.maxZoom);
  }
  for (const r of rasters) if (prefs.raster.includes(r.id) && inside(r.man.bounds)) best = Math.max(best, r.man.maxZoom || 14);
  return best;
}

// dark when Nomad is (its own switch, body.dark, the same on every page), not when the phone is
function isDark() {
  if (prefs.flavor === 'dark' || prefs.flavor === 'black') return true;
  if (prefs.flavor !== 'auto') return false;
  return document.body.classList.contains('dark') || document.documentElement.classList.contains('dark');
}
// a colour from Nomad's theme (master.css, with the admin panel's theme over it)
function themeColor(name, fallback) {
  const v = getComputedStyle(document.body).getPropertyValue(name).trim();
  return v || fallback;
}
function flavorName() { return prefs.flavor === 'auto' ? (isDark() ? 'dark' : 'light') : prefs.flavor; }
function buildStyle() {
  const fl = flavorName();
  const style = {
    version: 8,
    glyphs: abs('/assets/maps/fonts/{fontstack}/{range}.pbf'),
    sprite: abs('/assets/maps/sprites/v4/' + (fl === 'dark' || fl === 'black' ? 'dark' : 'light')),
    sources: {}, layers: [],
  };
  const vec = activePacks().filter((p) => p.pm || p.man.blocks);
  if (vec.length) {
    style.sources.nomad = { type: 'vector', tiles: ['nomad://tiles/{z}/{x}/{y}'], minzoom: 0,
      maxzoom: Math.max(...vec.map((p) => p.man.blocks ? p.man.blocks.maxZoom : p.man.maxZoom)),
      attribution: '<a href="https://protomaps.com">Protomaps</a> © <a href="https://openstreetmap.org/copyright">OpenStreetMap</a>' };
    let layers = basemaps.layers('nomad', basemaps.namedFlavor(fl), { lang: 'en' });
    if (!prefs.labels) layers = layers.filter((l) => l.type !== 'symbol' || l.id === 'pois');
    if (!prefs.pois) layers = layers.filter((l) => l.id !== 'pois');
    if (!prefs.buildings) layers = layers.filter((l) => !/building/.test(l.id));
    style.layers = layers;
  } else {
    style.layers = [{ id: 'background', type: 'background', paint: { 'background-color': themeColor('--bg', isDark() ? '#071226' : '#f4f4f9') } }];
  }
  // older raster regions the user switched on, under the labels
  // least detailed first, so a city's own map draws over the country overview
  const firstSymbol = style.layers.findIndex((l) => l.type === 'symbol');
  let at = firstSymbol < 0 ? style.layers.length : firstSymbol;
  for (const r of rasters.slice().sort((a, b) => (a.man.maxZoom || 14) - (b.man.maxZoom || 14))) {
    if (!prefs.raster.includes(r.id)) continue;
    style.sources['r-' + r.id] = { type: 'raster', tileSize: 256, bounds: r.man.bounds,
      minzoom: r.man.minZoom || 0, maxzoom: r.man.maxZoom || 14,
      tiles: [abs(r.base + r.man.tilePath)], attribution: r.man.attribution || '' };
    style.layers.splice(at++, 0, { id: 'r-' + r.id, type: 'raster', source: 'r-' + r.id, paint: { 'raster-opacity': 1 } });
  }
  return style;
}

let map;
function addOverlays() {
  if (!map.getSource('route')) {
    map.addSource('route', { type: 'geojson', data: emptyFC() });
    map.addLayer({ id: 'route-casing', type: 'line', source: 'route', layout: { 'line-join': 'round', 'line-cap': 'round' },
      paint: { 'line-color': themeColor('--primary-dark', '#0056b3'), 'line-width': ['interpolate', ['linear'], ['zoom'], 6, 5, 14, 10, 18, 16] } });
    map.addLayer({ id: 'route-line', type: 'line', source: 'route', layout: { 'line-join': 'round', 'line-cap': 'round' },
      paint: { 'line-color': themeColor('--primary', '#007bff'), 'line-width': ['interpolate', ['linear'], ['zoom'], 6, 3, 14, 7, 18, 12] } });
  }
  if (!map.getSource('areas')) {
    map.addSource('areas', { type: 'geojson', data: areasFC() });
    map.addLayer({ id: 'areas', type: 'line', source: 'areas', layout: { visibility: prefs.outlines ? 'visible' : 'none' },
      paint: { 'line-color': themeColor('--danger', '#dc3545'), 'line-width': 1.5, 'line-dasharray': [3, 2] } });
  }
  if (!map.getSource('reach')) {
    map.addSource('reach', { type: 'geojson', data: emptyFC() });
    map.addLayer({ id: 'reach-line', type: 'line', source: 'reach', layout: { 'line-join': 'round', 'line-cap': 'round' },
      paint: { 'line-color': ['interpolate', ['linear'], ['get', 'f'], 0, '#1a9850', 0.5, '#f2c12e', 1, '#d73027'],
        'line-width': ['interpolate', ['linear'], ['zoom'], 8, 1.5, 14, 4], 'line-opacity': 0.85 } },
    map.getLayer('route-casing') ? 'route-casing' : undefined);
  }
  if (!map.getSource('trails')) {
    map.addSource('trails', { type: 'geojson', data: emptyFC() });
    const before = map.getLayer('route-casing') ? 'route-casing' : undefined;
    map.addLayer({ id: 'trails-line', type: 'line', source: 'trails', minzoom: 9,
      layout: { 'line-join': 'round', visibility: prefs.trails ? 'visible' : 'none' },
      paint: { 'line-color': '#b8641c', 'line-width': ['interpolate', ['linear'], ['zoom'], 9, 1.2, 14, 2.6], 'line-dasharray': [2, 1.2] } }, before);
    map.addLayer({ id: 'trails-label', type: 'symbol', source: 'trails', minzoom: 12,
      layout: { 'symbol-placement': 'line', 'text-field': ['get', 'name'], 'text-size': 11, 'text-font': ['Noto Sans Italic'],
        visibility: prefs.trails ? 'visible' : 'none' },
      paint: { 'text-color': isDark() ? '#e0a46a' : '#8a4a12', 'text-halo-color': isDark() ? '#101418' : '#fff', 'text-halo-width': 1.4 } }, before);
    trailsShown.clear();
    loadTrails();
  }
  if (routeGeo) map.getSource('route').setData(routeGeo);
}
const emptyFC = () => ({ type: 'FeatureCollection', features: [] });
function areasFC() {
  return { type: 'FeatureCollection', features: packs.map((p) => {
    const [w, s, e, n] = p.man.bounds;
    return { type: 'Feature', properties: { name: p.name }, geometry: { type: 'LineString', coordinates: [[w, s], [e, s], [e, n], [w, n], [w, s]] } };
  }) };
}
function applyStyle() {
  map.setStyle(buildStyle(), { diff: false });
  map.once('styledata', addOverlays);
  clampZoom();
}
/* Past the deepest zoom the packs have here, the map would go blank: ease back to it.
   Done after the move rather than with setMaxZoom, which would also stop a flight from
   the overview into a state that does have the detail. */
let clampWarned = 0;
function zoomLimitAt(lng, lat) {
  if (!(packs.some((p) => p.pm || p.man.blocks) || prefs.raster.length)) return 22;
  const d = detailAt(lng, lat);
  // a little past the data (OVERZOOM): closer, at the same detail
  return d >= 14 ? 22 : d ? d + OVERZOOM + 0.5 : 3;
}
function clampZoom() {
  if (!map || !(packs.some((p) => p.pm || p.man.blocks) || prefs.raster.length)) return;
  const c = map.getCenter();
  const limit = zoomLimitAt(c.lng, c.lat);
  if (map.getZoom() > limit + 0.01) {
    map.easeTo({ zoom: limit, duration: 300 });
    if (Date.now() - clampWarned > 4000) { clampWarned = Date.now(); toast('More detail here needs a map pack for this area.'); }
  }
}

function startMap() {
  const last = store.get('view', null);
  const fit = boundsOf(activePacks());
  map = new maplibregl.Map({
    container: 'map', style: buildStyle(), attributionControl: { compact: true },
    center: last ? last.c : fit ? [(fit[0] + fit[2]) / 2, (fit[1] + fit[3]) / 2] : [-98, 39],
    zoom: last ? last.z : 3, maxPitch: 60, dragRotate: true, hash: false,
  });
  map.addControl(new maplibregl.NavigationControl({ visualizePitch: true }), 'top-right');
  map.addControl(new maplibregl.ScaleControl({ unit: prefs.units === 'km' ? 'metric' : 'imperial' }), 'bottom-left');
  map.on('load', () => { addOverlays(); if (!last && fit) map.fitBounds([[fit[0], fit[1]], [fit[2], fit[3]]], { padding: 30, duration: 0 }); clampZoom(); });
  map.on('moveend', () => { clampZoom(); loadTrails(); const c = map.getCenter(); store.set('view', { c: [c.lng, c.lat], z: map.getZoom() }); });
  map.on('click', onMapClick);
  followTheme();
  // a few icons the style names are not in the sprite sheet: draw nothing rather than warn
  const blank = { width: 1, height: 1, data: new Uint8Array(4) };
  if (map.setMissingStyleImageResolver) map.setMissingStyleImageResolver((id) => { if (!map.hasImage(id)) map.addImage(id, blank); });
  else map.on('styleimagemissing', (e) => { if (!map.hasImage(e.id)) map.addImage(e.id, blank); });
  map.on('dragstart', () => { if (follow) setFollow(false); if (nav) nav.free = true; closeResults(); });
  // a phone short of memory can drop the map's drawing. it comes back by itself, and the
  // turn banner and voice carry on meanwhile
  map.on('webglcontextlost', () => toast('Redrawing the map…', 4000));
  map.on('webglcontextrestored', () => toast(''));
  map.on('error', (e) => { const m = (e && e.error && e.error.message) || ''; if (!/40[34]|AbortError|Not Found/i.test(m)) console.warn('map:', m); });
}
// text on the theme colour: white like the rest of Nomad, unless white is hard to read on it
// (a dark theme's primary is often light, and the turn banner has to read at a glance)
function setOnPrimary() {
  const c = document.createElement('canvas').getContext('2d');
  c.fillStyle = themeColor('--primary', '#007bff');
  const m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})/i.exec(c.fillStyle);
  if (!m) return;
  const lin = (h) => { const v = parseInt(h, 16) / 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; };
  const L = 0.2126 * lin(m[1]) + 0.7152 * lin(m[2]) + 0.0722 * lin(m[3]);
  document.body.style.setProperty('--on-primary', 1.05 / (L + 0.05) >= 3 ? '#ffffff' : '#111111');
}
// the card's theme (/.system-theme.json) lands after the map has drawn, and dark mode is a
// class on body: when either changes, recolour the route, and redraw the map if it follows Nomad
function followTheme() {
  let dark = isDark(), t = null;
  const again = () => {
    clearTimeout(t);
    t = setTimeout(() => {
      setOnPrimary();
      if (!map) return;
      if (map.getLayer('route-line')) {
        map.setPaintProperty('route-line', 'line-color', themeColor('--primary', '#007bff'));
        map.setPaintProperty('route-casing', 'line-color', themeColor('--primary-dark', '#0056b3'));
      }
      if (prefs.flavor === 'auto' && isDark() !== dark) { dark = isDark(); applyStyle(); }
    }, 100);
  };
  new MutationObserver(again).observe(document.head, { childList: true, subtree: true, characterData: true });
  new MutationObserver(again).observe(document.body, { attributes: true, attributeFilter: ['class'] });
}
function boundsOf(list) {
  const bs = list.map((p) => p.man.bounds).concat(list === packs ? [] : []);
  if (!bs.length) return rasters.length ? rasters[0].man.bounds : null;
  // the smallest pack is where the detail is, open there rather than on the world
  return bs.slice().sort((a, b) => (a[2] - a[0]) * (a[3] - a[1]) - (b[2] - b[0]) * (b[3] - b[1]))[0];
}

const COORD_FMTS = [['dd', 'Lat/Lon'], ['dms', 'D° M′ S″'], ['utm', 'UTM'], ['mgrs', 'MGRS']];
const FLAVORS = [['auto', 'Nomad'], ['light', 'Light'], ['dark', 'Dark'], ['white', 'White'], ['grayscale', 'Gray'], ['black', 'Black']];
function renderPanel() {
  const P = $('panel');
  if (prefs.pack !== 'all' && !packs.some((p) => p.id === prefs.pack)) prefs.pack = 'all';
  const mb = (n) => (n / 1e6 >= 1000 ? (n / 1e9).toFixed(1) + ' GB' : Math.round(n / 1e6) + ' MB');
  P.innerHTML = `
    ${packs.length > 1 ? `<h3>Maps shown</h3><select id="packSel" aria-label="Maps shown"><option value="all">All maps</option>${packs.map((p) => `<option value="${esc(p.id)}" ${prefs.pack === p.id ? 'selected' : ''}>${esc(p.name)}</option>`).join('')}</select>` : ''}
    <h3>Map style</h3>
    <div class="swatches">${FLAVORS.map(([k, n]) => `<button data-fl="${k}" class="${prefs.flavor === k ? 'on' : ''}">${n}</button>`).join('')}</div>
    <h3>Show</h3>
    <label class="toggle">Labels <input type="checkbox" data-p="labels" ${prefs.labels ? 'checked' : ''}></label>
    <label class="toggle">Places and shops <input type="checkbox" data-p="pois" ${prefs.pois ? 'checked' : ''}></label>
    <label class="toggle">Buildings <input type="checkbox" data-p="buildings" ${prefs.buildings ? 'checked' : ''}></label>
    <label class="toggle">Hiking trails <input type="checkbox" data-p="trails" ${prefs.trails ? 'checked' : ''}></label>
    <label class="toggle">Map pack edges <input type="checkbox" data-p="outlines" ${prefs.outlines ? 'checked' : ''}></label>
    ${rasters.length ? '<h3>Image maps</h3>' + rasters.map((r) => `<label class="toggle">${esc(r.name)} <input type="checkbox" data-r="${esc(r.id)}" ${prefs.raster.includes(r.id) ? 'checked' : ''}></label>`).join('') : ''}
    <h3>Units</h3>
    <div class="seg"><button data-u="mi" class="${prefs.units === 'mi' ? 'on' : ''}">Miles</button><button data-u="km" class="${prefs.units === 'km' ? 'on' : ''}">Kilometers</button></div>
    <h3>Coordinates</h3>
    <div class="seg">${COORD_FMTS.map(([k, n]) => `<button data-cf="${k}" class="${prefs.coords === k ? 'on' : ''}">${n}</button>`).join('')}</div>
    <h3>On this card</h3>
    <div class="areas">${packs.map((p) => `<div><span>${esc(p.name)}${p.man.routing ? ' · directions' : ''}</span><span>${mb(p.man.sizeBytes || 0)}</span></div>`).join('') || '<div><span>No map packs</span><span></span></div>'}</div>
    <div class="foot">Add areas in Nomad-Tools › Download map packs</div>`;
  if ($('packSel')) $('packSel').onchange = () => {
    prefs.pack = $('packSel').value; savePrefs(); resetRouter(); applyStyle();
    const b = boundsOf(activePacks());
    if (b && prefs.pack !== 'all') map.fitBounds([[b[0], b[1]], [b[2], b[3]]], { padding: 30 });
  };
  P.querySelectorAll('[data-fl]').forEach((b) => b.onclick = () => { prefs.flavor = b.dataset.fl; savePrefs(); renderPanel(); applyStyle(); });
  P.querySelectorAll('[data-p]').forEach((c) => c.onchange = () => {
    prefs[c.dataset.p] = c.checked; savePrefs();
    if (c.dataset.p === 'outlines') map.setLayoutProperty('areas', 'visibility', c.checked ? 'visible' : 'none');
    else if (c.dataset.p === 'trails') showTrails();
    else applyStyle();
  });
  P.querySelectorAll('[data-r]').forEach((c) => c.onchange = () => {
    prefs.raster = c.checked ? prefs.raster.concat(c.dataset.r) : prefs.raster.filter((x) => x !== c.dataset.r);
    savePrefs(); applyStyle();
  });
  P.querySelectorAll('[data-u]').forEach((b) => b.onclick = () => { prefs.units = b.dataset.u; savePrefs(); location.reload(); });
  P.querySelectorAll('[data-cf]').forEach((b) => b.onclick = () => { prefs.coords = b.dataset.cf; savePrefs(); renderPanel(); });
}
$('layersBtn').onclick = (e) => { e.stopPropagation(); closeResults(); renderPanel(); $('panel').classList.toggle('open'); $('layersBtn').classList.toggle('on'); };
document.addEventListener('click', (e) => {
  if (!$('panel').contains(e.target) && !$('layersBtn').contains(e.target)) { $('panel').classList.remove('open'); $('layersBtn').classList.remove('on'); }
});

const placeIdx = new Map();     // pack id -> index.json
const placeChunks = new Map();  // pack id + prefix -> lines
const fold = (s) => s.toLowerCase().normalize('NFKD').replace(/[̀-ͯ]/g, '');
/* The index splits common word starts into longer ones so no keystroke reads much: use
   the longest start that fits the word. 'more' means the word is too short for its slice. */
async function placeLines(p, word) {
  if (!placeIdx.has(p.id)) placeIdx.set(p.id, fetchJSON(p.base + p.man.places.index).then((ix) => {
    ix.keys = Object.keys(ix.prefixes).sort((a, b) => b.length - a.length); return ix;
  }).catch(() => null));
  const idx = await placeIdx.get(p.id);
  if (!idx) { placeIdx.delete(p.id); return { fail: true }; }
  const prefix = idx.keys.find((k) => word.startsWith(k));
  if (!prefix) return idx.keys.some((k) => k.startsWith(word)) ? { more: true } : [];
  const key = p.id + '/' + prefix;
  if (!placeChunks.has(key)) {
    const r = idx.prefixes[prefix];
    placeChunks.set(key, !r ? Promise.resolve([]) : fetch(media(p.base + p.man.places.data), { headers: { Range: `bytes=${r[0]}-${r[0] + r[1] - 1}` } })
      .then((res) => res.text()).then((t) => t.split('\n').filter(Boolean).map((l) => {
        const [w, name, lat, lon, kind, cat] = l.split('\t');
        return { w, name, lat: +lat, lon: +lon, kind: idx.kinds[+kind], cat: idx.cats[+cat], pack: p.name };
      })).catch(() => { placeChunks.delete(key); return { fail: true }; }));
  }
  return placeChunks.get(key);
}
function parseCoords(q) {
  if (window.NomadCoords) return NomadCoords.parse(q);
  const m = /^\s*(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)\s*$/.exec(q);
  if (!m) return null;
  const a = +m[1], b = +m[2];
  return Math.abs(a) <= 90 && Math.abs(b) <= 180 ? { lat: a, lon: b } : null;
}
/* One list under the search box or a directions field: waypoints (yours, so first), then
   coordinates, addresses and places. With nothing typed: your waypoints and recent places,
   and in a directions field also your location and "choose on the map". */
let resultsFor = null;               // 'q' (the search box), 'from' or 'to' (the directions card)
const recent = {
  get: () => store.get('recent', []),
  add(r) {
    if (r.wp || r.action) return;
    const l = recent.get().filter((x) => !(x.name === r.name && Math.abs(x.lat - r.lat) < 1e-5 && Math.abs(x.lon - r.lon) < 1e-5));
    l.unshift({ name: r.name, lat: r.lat, lon: r.lon, kind: r.kind || '' }); store.set('recent', l.slice(0, 6));
  },
};
function hereLL() {
  if (me) return { lat: me.lat, lon: me.lon };
  if (map) { const c = map.getCenter(); return { lat: c.lat, lon: c.lng }; }
  return null;
}
// every word typed starts a word of the name, or the name holds what was typed
function nameMatches(name, q) {
  const nf = fold(name), fq = fold(q).trim(), parts = nf.split(/[^a-z0-9]+/);
  return nf.includes(fq) || fq.split(/[^a-z0-9]+/).filter(Boolean).every((w) => parts.some((x) => x.startsWith(w)));
}
function waypointsFor(q) {
  const c = hereLL();
  return store.get('pins', []).filter((p) => !q || nameMatches(p.name, q))
    .map((p) => ({ ...p, kind: 'waypoint', wp: true, d: c ? NR.hav(c.lat, c.lon, p.lat, p.lon) : 0 })).sort((a, b) => a.d - b.d);
}
/* Categories: what people type for each kind of place Nearby knows (osm_extras.py
   ESSENTIALS). "hospital", "gas", "nearest water", "camping" list the closest ones, from
   the same small reads Nearby makes (and keeps). */
const CAT_WORDS = {
  hospital: ['hospital', 'hospitals', 'er', 'emergency room', 'emergency'],
  clinic: ['clinic', 'clinics', 'doctor', 'doctors', 'urgent care', 'medical'],
  pharmacy: ['pharmacy', 'pharmacies', 'drugstore', 'chemist', 'medicine'],
  water: ['water', 'drinking water', 'spring', 'water tap'],
  shelter: ['shelter', 'shelters', 'hut', 'refuge'],
  food: ['food', 'grocery', 'groceries', 'supermarket', 'store', 'convenience store'],
  fuel: ['fuel', 'gas', 'gas station', 'petrol', 'diesel'],
  police: ['police', 'police station', 'sheriff'],
  fire: ['fire station', 'fire stations', 'fire department'],
  ranger: ['ranger', 'ranger station'],
  aed: ['aed', 'defibrillator'],
  phone: ['emergency phone', 'sos phone'],
  camp: ['camp', 'campsite', 'campsites', 'campground', 'camping', 'rv park'],
  school: ['school', 'schools'],
  helipad: ['helipad', 'heliport', 'landing site'],
  boat: ['boat ramp', 'boat launch', 'slipway'],
  toilets: ['toilet', 'toilets', 'restroom', 'restrooms', 'bathroom', 'washroom'],
  peak: ['peak', 'peaks', 'summit', 'mountain top'],
  pass: ['mountain pass', 'saddle'],
};
const QUICK_CATS = [['hospital', 'Hospital'], ['water', 'Water'], ['shelter', 'Shelter'], ['fuel', 'Fuel'],
  ['food', 'Food'], ['pharmacy', 'Pharmacy'], ['camp', 'Campsite'], ['toilets', 'Toilets']];
function catOf(q) {
  const f = fold(q).trim().replace(/\s+/g, ' ').replace(/^(nearest|closest|nearby|near me) /, '').replace(/ (near me|nearby|near here)$/, '');
  for (const [k, ws] of Object.entries(CAT_WORDS)) if (ws.includes(f)) return k;
  if (f.length >= 4) for (const [k, ws] of Object.entries(CAT_WORDS)) if (ws.some((w) => w.startsWith(f))) return k;   // "hosp"
  return null;
}
async function categoryNear(key, n = 6) {
  const at = whereFrom();
  const { cats, found } = await nearby(at.lat, at.lon, key);
  const i = cats.findIndex((c) => c[0] === key);
  if (i < 0) return null;
  const from = at.from === 'you' ? 'you' : at.from;
  return { at, title: cats[i][1], list: (found.get(i) || []).slice(0, n).map((x) => ({
    name: x.name || 'Unnamed ' + (ONE_OF[key] || 'place'), lat: x.lat, lon: x.lon, icon: 'nearby', kind: ONE_OF[key] || key,
    sub: `${ONE_OF[key] || key} · ${fmtDist(x.d)} ${COMPASS8[Math.round(bearingOf([at.lat, at.lon], [x.lat, x.lon]) / 45) % 8]} of ${from}` })) };
}
async function findPlaces(q) {
  const coord = parseCoords(q);
  if (coord) return { list: [{ name: fmtLL(coord.lat, coord.lon), lat: coord.lat, lon: coord.lon, kind: 'coordinates', icon: 'locate' }] };
  const cat = catOf(q);
  const near = cat && activePacks().some((p) => p.man.essentials) ? await categoryNear(cat).catch(() => null) : null;
  if (!near) return placesNamed(q);
  // the closest of the kind first, then places whose names match
  const rest = await placesNamed(q);
  const list = near.list.concat(rest.list.filter((r) => !near.list.some((x) => samePt(x, r)))).slice(0, 14);
  const where = near.at.from === 'you' ? 'you' : near.at.from;
  return { list, note: near.list.length ? '' : `No ${near.title.toLowerCase()} on these maps within about 40 km of ${esc(where)}.` };
}
async function placesNamed(q) {
  const out = waypointsFor(q).slice(0, 5);
  const words = fold(q).split(/[^a-z0-9]+/).filter((w) => w.length >= 2);
  if (!words.length) return { list: out, note: out.length ? '' : 'Keep typing…' };
  const src = activePacks().filter((p) => p.man.places);
  if (!src.length) return { list: out, note: out.length ? '' : 'No place search on these maps: add an area pack with Nomad-Tools. Waypoints and coordinates (like 47.6, -122.3) still work.' };
  const first = words[0];
  const [got, addrs] = await Promise.all([Promise.all(src.map((p) => placeLines(p, first))), addressSearch(q).catch(() => [])]);
  const failed = got.some((g) => g && g.fail), more = got.some((g) => g && g.more), lists = got.filter(Array.isArray);
  const c = map.getCenter(), seen = new Set(), found = [...addrs];
  const fq = fold(q).trim();
  for (const list of lists) for (const r of list) {
    if (!r.w.startsWith(first)) continue;
    const nf = fold(r.name);
    if (!words.slice(1).every((w) => nf.split(/[^a-z0-9]+/).some((x) => x.startsWith(w)))) continue;
    const key = r.name + '|' + r.lat.toFixed(3) + r.lon.toFixed(3);
    if (seen.has(key)) continue;
    seen.add(key);
    const d = NR.hav(c.lat, c.lng, r.lat, r.lon);
    // the name itself, then names that start with what was typed, then towns, landmarks
    // and parks over shops, then near ones
    const catRank = r.cat === 'town' ? 0 : (r.cat === 'fun' || r.cat === 'park' || r.cat === 'civic') ? 1 : 2;
    r.score = (nf === fq ? 0 : nf.startsWith(fq) ? 1 : 2) * 1e9 + catRank * 1e8 + Math.min(d, 9e7);
    found.push(r);
  }
  found.sort((a, b) => (a.score || 0) - (b.score || 0));
  const list = out.concat(found).slice(0, 12);
  let note = '';
  if (failed) note = "Lost touch with Nomad while searching. Check you are still on its WiFi.";
  else if (!list.length) note = more ? 'Keep typing…'
    : `Nothing called “${esc(q.trim())}” on these maps. Try part of the name, a town, an address like “1200 Main St”, or coordinates.`;
  return { list, note };
}
function suggestions(which) {
  const items = [];
  if (which) {
    items.push({ name: 'Your location', icon: 'locate', action: 'me', sub: '' });
    items.push({ name: 'Choose on the map', icon: 'pin', action: 'map', sub: '' });
  }
  if (activePacks().some((p) => p.man.essentials)) items.push({ head: 'Find the nearest' }, { cats: true });
  const w = waypointsFor('');
  if (w.length) items.push({ head: 'Your waypoints' }, ...w.slice(0, 8));
  const rc = recent.get();
  if (rc.length) items.push({ head: 'Recent' }, ...rc.map((r) => ({ ...r, icon: 'refresh' })));
  return items;
}
const ICON_OF = { waypoint: 'pin', address: 'home', coordinates: 'locate' };
function subOf(r) {
  if (r.sub != null) return r.sub;
  // map data sometimes calls a thing just "yes", that says nothing
  const c = hereLL(), bits = [r.kind && !/^(yes|no|unknown)$/i.test(r.kind) ? r.kind.replace(/_/g, ' ') : '', r.pack].filter(Boolean);
  if (c && r.lat != null) bits.push(fmtDist(NR.hav(c.lat, c.lon, r.lat, r.lon)));
  return bits.join(' · ');
}
function renderResults(items, note) {
  const box = $('results');
  box.innerHTML = items.map((r, i) => r.head ? `<h4>${esc(r.head)}</h4>`
    : r.cats ? `<div class="qcats">${QUICK_CATS.map(([, n]) => `<button data-cat="${esc(n)}">${icon('nearby', 15)}${esc(n)}</button>`).join('')}</div>`
    : `<button data-i="${i}"><span class="ic">${icon(r.icon || ICON_OF[r.kind] || 'search', 18)}</span><span class="tx"><span>${esc(r.name)}</span><small>${esc(subOf(r))}</small></span></button>`).join('')
    + (note ? `<div class="none">${note}</div>` : '');
  box.classList.toggle('open', !!box.innerHTML);
  box.scrollTop = 0;
  box.querySelectorAll('button[data-i]').forEach((b) => b.onclick = () => pickResult(items[+b.dataset.i]));
  box.querySelectorAll('button[data-cat]').forEach((b) => b.onclick = () => {
    const which = resultsFor || 'q', el = $(which === 'from' ? 'pf' : which === 'to' ? 'pt' : 'q');
    el.value = b.dataset.cat; if (which === 'q') $('qClear').hidden = false;
    searchInto(which, b.dataset.cat);
  });
}
function closeResults() { $('results').classList.remove('open'); resultsFor = null; }
let searchSeq = 0;
async function searchInto(which, q) {
  const seq = ++searchSeq;
  resultsFor = which;
  if (!q.trim()) { renderResults(suggestions(which === 'q' ? null : which)); return; }
  const { list, note } = await findPlaces(q).catch((e) => ({ list: [], note: whyFailed(e) }));
  if (seq !== searchSeq || resultsFor !== which) return;
  renderResults(list, note);
}
async function pickResult(r) {
  const which = resultsFor;
  closeResults();
  if (which === 'from' || which === 'to') {
    $(which === 'from' ? 'pf' : 'pt').blur();
    if (r.action === 'map') { startPick(which); return; }
    if (r.action === 'me') {
      const p = await locateOnce();
      if (p) setPlanPoint(which, myLoc(p)); else fillFields();
      return;
    }
    recent.add(r);
    setPlanPoint(which, { name: r.name, lat: r.lat, lon: r.lon });
    return;
  }
  $('q').blur(); $('q').value = r.name; $('qClear').hidden = false;
  recent.add(r);
  map.flyTo({ center: [r.lon, r.lat], zoom: Math.max(map.getZoom(), r.cat === 'town' ? 12 : 16), essential: true });
  showPlace({ name: r.name, lat: r.lat, lon: r.lon, sub: r.wp ? 'waypoint' : r.kind });
}
function wireSearchInput(el, which) {
  let t = null;
  el.addEventListener('focus', () => {
    $('panel').classList.remove('open');
    if (which !== 'q') el.select();
    // a field showing the point already chosen opens on the suggestions, not a search for it
    const cur = which === 'q' ? '' : (plan[which] && plan[which].name) || '';
    searchInto(which, el.value === cur ? '' : el.value);
  });
  el.addEventListener('input', () => {
    if (which === 'q') $('qClear').hidden = !el.value;
    clearTimeout(t); t = setTimeout(() => searchInto(which, el.value), 200);
  });
  el.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { const b = $('results').querySelector('button[data-i]'); if (b) b.click(); }
    if (e.key === 'Escape') { closeResults(); el.blur(); }
  });
  // leaving a directions field without choosing puts back what it had
  if (which !== 'q') el.addEventListener('blur', () => setTimeout(() => { if (document.activeElement !== el) fillFields(); }, 250));
}
wireSearchInput($('q'), 'q');
$('results').addEventListener('mousedown', (e) => e.preventDefault());   // keep the field focused while clicking
$('qClear').onclick = () => { $('q').value = ''; $('qClear').hidden = true; closeResults(); if (placeMarker) { placeMarker.remove(); placeMarker = null; } };

/* What area packs carry beside roads (Nomad-Tools osm_extras.py). */
// a byte range of a card file, retried while the card is busy
async function rangeBytes(file, off, len) {
  let wait = 300;
  for (let i = 0; i < 6; i++) {
    const res = await fetch(media(file), { headers: { Range: `bytes=${off}-${off + len - 1}` }, cache: 'no-store' });
    if (res.status === 206) return new Uint8Array(await res.arrayBuffer());
    if (res.status === 200) return new Uint8Array(await res.arrayBuffer()).slice(off, off + len);
    await new Promise((ok) => setTimeout(ok, wait)); wait = Math.min(wait * 1.8, 3000);
  }
  throw new Error('the card stayed busy');
}
const utf8 = new TextDecoder();
const extraIdx = new Map(), extraChunk = new Map();
function extraIndex(p, sub) {
  const key = p.id + '/' + sub;
  if (!extraIdx.has(key)) extraIdx.set(key, fetchJSON(p.base + p.man[sub].index).catch(() => null));
  return extraIdx.get(key);
}
function extraRange(p, sub, file, off, len) {
  const key = p.id + '/' + sub + '/' + off;
  if (!extraChunk.has(key)) extraChunk.set(key, rangeBytes(p.base + p.man[sub][file], off, len).catch(() => null));
  return extraChunk.get(key);
}
const cellKey = (lat, lon, size, di = 0, dj = 0) => `${Math.floor(lat / size) + di}_${Math.floor(lon / size) + dj}`;

/* Nearby: the closest hospitals, water, shelter, fuel... around you (or the map centre),
   named or not. Reads the ~10 km squares around the spot, wider if they hold little. */
async function nearby(lat, lon, want = null) {
  const src = activePacks().filter((p) => p.man.essentials);
  const found = new Map();                     // category -> items
  let cats = null;
  for (const ring of [2, 4]) {
    await Promise.all(src.map(async (p) => {
      const ix = await extraIndex(p, 'essentials');
      if (!ix) return;
      cats = cats || ix.cats;
      const jobs = [];
      for (let di = -ring; di <= ring; di++) for (let dj = -ring; dj <= ring; dj++) {
        const r = ix.cells[cellKey(lat, lon, ix.cell, di, dj)];
        if (r) jobs.push(extraRange(p, 'essentials', 'data', r[0], r[1]));
      }
      for (const b of await Promise.all(jobs)) {
        if (!b) continue;
        for (const line of utf8.decode(b).split('\n')) {
          if (!line) continue;
          const [c, name, la, lo, ele] = line.split('\t');
          const key = la + ',' + lo + c;
          const list = found.get(+c) || []; found.set(+c, list);
          if (!list.some((x) => x.key === key)) list.push({ key, cat: +c, name, lat: +la, lon: +lo, ele: ele ? +ele : null, d: NR.hav(lat, lon, +la, +lo) });
        }
      }
    }));
    // enough to show. a search for one kind ("camping") keeps looking wider for that kind
    const wi = want != null && cats ? cats.findIndex((c) => c[0] === want) : -1;
    if (wi >= 0 ? (found.get(wi) || []).length >= 3 : [...found.values()].reduce((n, l) => n + l.length, 0) >= 30) break;
  }
  for (const l of found.values()) l.sort((a, b) => a.d - b.d);
  return { cats: cats || [], found };
}
const COMPASS8 = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
// what one of each category is called, for the ones with no name
const ONE_OF = { hospital: 'hospital', clinic: 'clinic', pharmacy: 'pharmacy', water: 'water point', shelter: 'shelter',
  food: 'food store', fuel: 'fuel station', police: 'police station', fire: 'fire station', ranger: 'ranger station',
  aed: 'defibrillator', phone: 'emergency phone', toilets: 'toilet', camp: 'campsite', school: 'school', helipad: 'helipad',
  boat: 'boat ramp', peak: 'peak', pass: 'pass' };
const fmtEle = (m) => prefs.units === 'km' ? Math.round(m).toLocaleString() + ' m' : Math.round(m * 3.28084).toLocaleString() + ' ft';
// where "from here" is: you, or the middle of the map
function whereFrom() {
  // a place or dropped pin open on screen is where you're asking about
  if (sheetCtx === 'place' && currentPlace) {
    const nm = currentPlace.name === 'Dropped pin' ? 'the dropped pin' : currentPlace.name;
    return { lat: currentPlace.lat, lon: currentPlace.lon, from: nm, name: nm };
  }
  if (me) return { lat: me.lat, lon: me.lon, from: 'you', name: 'where you are' };
  const c = map.getCenter();
  return { lat: c.lat, lon: c.lng, from: 'the middle of the map', name: 'the middle of the map' };
}
async function openNearby(atPlace) {
  const at = atPlace || whereFrom();
  const pins = waypointsFor('').map((x) => ({ ...x, d: NR.hav(at.lat, at.lon, x.lat, x.lon) })).sort((a, b) => a.d - b.d);
  const haveData = activePacks().some((p) => p.man.essentials);
  if (!haveData && !pins.length) {
    openSheet(head('Nearby') + '<div class="pnote">Nearby needs an area pack from Nomad-Tools.</div>', { ctx: 'list' });
    return;
  }
  openSheet(head('Nearby', 'Looking…'), { ctx: 'list' });
  let cats = [], found = new Map();
  try { if (haveData) ({ cats, found } = await nearby(at.lat, at.lon)); }
  catch (e) { openSheet(head('Nearby') + errBox(whyFailed(e), 'nRetry'), { ctx: 'list' }); $('nRetry').onclick = () => openNearby(at); return; }
  const dir = (x) => COMPASS8[Math.round(bearingOf([at.lat, at.lon], [x.lat, x.lon]) / 45) % 8];
  const label = (x) => x.wp ? x.name : (x.name || ('Unnamed ' + (ONE_OF[cats[x.cat] && cats[x.cat][0]] || 'place')))
    + (x.ele != null ? ' · ' + fmtEle(x.ele) : '');
  // your own waypoints first, then what the maps know, in the order that matters most
  const rows = (pins.length ? [['wp', 'Your waypoints', pins, 5]] : [])
    .concat(cats.map(([, title], i) => [i, title, found.get(i) || [], 3]).filter(([, , l]) => l.length));
  if (!rows.length) { openSheet(head('Nearby') + `<div class="pnote">Nothing on these maps within about 40 km of ${at.from}.</div>`, { ctx: 'list' }); return; }
  const shown = new Map();
  openSheet(head('Nearby', `Closest to ${at.from}, in a straight line`) + `<div class="list">
    ${rows.map(([i, title, l, n]) => `<h3>${esc(title)}</h3>` + l.slice(0, n).map((x, k) => { shown.set(i + '/' + k, x);
      return `<button class="it" data-k="${i}/${k}"><span>${esc(label(x))}</span><small>${fmtDist(x.d)} ${dir(x)}</small></button>`; }).join('')).join('')}</div>`, { ctx: 'list' });
  document.querySelectorAll('.list .it').forEach((b) => b.onclick = () => {
    const x = shown.get(b.dataset.k);
    map.flyTo({ center: [x.lon, x.lat], zoom: Math.max(map.getZoom(), 15) });
    showPlace({ name: label(x), lat: x.lat, lon: x.lon, sub: x.wp ? 'waypoint' : (cats[x.cat] ? cats[x.cat][1] : '') });
  });
}
function openWaypoints() {
  const at = whereFrom(), list = waypointsFor('');
  if (!list.length) {
    openSheet(head('Your waypoints') + '<div class="pnote">No waypoints yet</div>', { ctx: 'list' });
    return;
  }
  const dir = (x) => COMPASS8[Math.round(bearingOf([at.lat, at.lon], [x.lat, x.lon]) / 45) % 8];
  openSheet(head('Your waypoints', `${list.length} saved · distance from ${at.from}`) + `<div class="list">${list.map((x, i) =>
    `<button class="it" data-i="${i}"><span>${esc(x.name)}</span><small>${fmtDist(NR.hav(at.lat, at.lon, x.lat, x.lon))} ${dir(x)}</small></button>`).join('')}</div>`, { ctx: 'list' });
  document.querySelectorAll('.list .it').forEach((b) => b.onclick = () => {
    const x = list[+b.dataset.i];
    map.flyTo({ center: [x.lon, x.lat], zoom: Math.max(map.getZoom(), 15) });
    showPlace({ ...x, sub: 'waypoint' });
  });
}
document.querySelectorAll('[data-chip]').forEach((b) => b.onclick = () => {
  closeResults();
  const c = b.dataset.chip;
  if (c === 'directions') openPlan(null, null);
  else if (c === 'nearby') openNearby();
  else if (c === 'waypoints') openWaypoints();
  else if (c === 'reach') openReach(whereFrom());
});

/* Addresses: "1234 Main" finds the street (by the words of its name, like place search),
   then the number on it, or the nearest number on that street. */
const ADDR_STOP = new Set(['the', 'of', 'and', 'de', 'la', 'el', 'st', 'street', 'ave', 'avenue', 'rd', 'road', 'dr', 'drive',
  'n', 's', 'e', 'w', 'ne', 'nw', 'se', 'sw', 'north', 'south', 'east', 'west', 'way', 'ln', 'lane', 'blvd', 'boulevard',
  'ct', 'court', 'pl', 'place']);
async function addressSearch(q) {
  const m = /^\s*(\d+[a-z]?(?:[-\/]\d+[a-z]?)?)\s+(.+)$/i.exec(q);
  if (!m) return [];
  const num = m[1].toLowerCase(), all = fold(m[2]).split(/[^a-z0-9]+/).filter((w) => w.length >= 2);
  const words = all.filter((w) => !ADDR_STOP.has(w));
  const key = (words.length ? words : all).slice().sort((a, b) => b.length - a.length)[0];
  if (!key) return [];
  const c = map.getCenter(), streets = [];
  await Promise.all(activePacks().filter((p) => p.man.addresses).map(async (p) => {
    const ix = await extraIndex(p, 'addresses');
    if (!ix) return;
    ix.keys = ix.keys || Object.keys(ix.prefixes).sort((a, b) => b.length - a.length);
    const pre = ix.keys.find((k) => key.startsWith(k));
    if (!pre) return;
    const r = ix.prefixes[pre], b = await extraRange(p, 'addresses', 'streets', r[0], r[1]);
    if (!b) return;
    for (const line of utf8.decode(b).split('\n')) {
      if (!line) continue;
      const [w, st, city, la, lo, off, len] = line.split('\t');
      if (!w.startsWith(key)) continue;
      const parts = fold(st).split(/[^a-z0-9]+/);
      if (!words.every((x) => parts.some((y) => y.startsWith(x)))) continue;
      streets.push({ p, st, city, lat: +la, lon: +lo, off: +off, len: +len, d: NR.hav(c.lat, c.lng, +la, +lo) });
    }
  }));
  streets.sort((a, b) => a.d - b.d);
  const out = [], seen = new Set();
  for (const s of streets.slice(0, 8)) {
    if (seen.has(s.st + s.city + s.off)) continue;
    seen.add(s.st + s.city + s.off);
    const b = await extraRange(s.p, 'addresses', 'numbers', s.off, s.len);
    if (!b) continue;
    let i = 0;
    const vi = () => { let r = 0, mul = 1, x; do { x = b[i++]; r += (x & 127) * mul; mul *= 128; } while (x & 128); return r; };
    const zz = () => { const v = vi(); return v % 2 ? -(v + 1) / 2 : v / 2; };
    const n = vi(), clat = Math.round(s.lat * 1e5), clon = Math.round(s.lon * 1e5);
    let best = null;
    const want = parseInt(num, 10);
    for (let k = 0; k < n; k++) {
      const ln = vi(), hn = utf8.decode(b.subarray(i, i + ln)); i += ln;
      const la = (clat + zz()) / 1e5, lo = (clon + zz()) / 1e5;
      const exact = hn.toLowerCase() === num, gap = Math.abs((parseInt(hn, 10) || 0) - want);
      const score = exact ? -1 : gap;
      if (!best || score < best.score) best = { hn, la, lo, score };
    }
    // the same number on the same street, from a part of it with its town given and a part without
    if (best && out.some((o) => o.hn === best.hn && o.st === s.st && NR.hav(o.lat, o.lon, best.la, best.lo) < 300)) continue;
    if (best) out.push({ hn: best.hn, st: s.st, name: `${best.hn} ${s.st}${s.city ? ', ' + s.city : ''}`, lat: best.la, lon: best.lo,
      kind: best.score === -1 ? 'address' : `nearest number to ${num}`, pack: s.p.name, score: (best.score === -1 ? 0 : 5e8) + s.d });
  }
  return out;
}

/* Trails: named hiking routes, drawn over the map from about zoom 9, a ~25 km square at a
   time as they come into view. */
const trailsShown = new Set(), trailFeatures = [];
let trailBusy = false;
function showTrails() {
  for (const id of ['trails-line', 'trails-label']) if (map.getLayer(id)) map.setLayoutProperty(id, 'visibility', prefs.trails ? 'visible' : 'none');
  if (prefs.trails) loadTrails();
}
async function loadTrails() {
  if (!prefs.trails || trailBusy || !map || map.getZoom() < 9 || !map.getSource('trails')) return;
  trailBusy = true;
  try {
    const b = map.getBounds(), src = activePacks().filter((p) => p.man.trails);
    let added = 0;
    for (const p of src) {
      const ix = await extraIndex(p, 'trails');
      if (!ix) continue;
      const s = ix.cell, jobs = [];
      for (let la = Math.floor(b.getSouth() / s) - 1; la <= Math.floor(b.getNorth() / s); la++) {
        for (let lo = Math.floor(b.getWest() / s) - 1; lo <= Math.floor(b.getEast() / s); lo++) {
          const key = p.id + '/' + la + '_' + lo, r = ix.cells[la + '_' + lo];
          if (!r || trailsShown.has(key) || jobs.length > 40) continue;
          trailsShown.add(key);
          jobs.push(extraRange(p, 'trails', 'data', r[0], r[1]).then((u8) => ({ u8, names: ix.names })));
        }
      }
      for (const { u8, names } of await Promise.all(jobs)) {
        if (!u8) continue;
        let i = 0;
        const vi = () => { let r = 0, mul = 1, x; do { x = u8[i++]; r += (x & 127) * mul; mul *= 128; } while (x & 128); return r; };
        const zz = () => { const v = vi(); return v % 2 ? -(v + 1) / 2 : v / 2; };
        for (let n = vi(); n > 0; n--) {
          const name = names[vi()] || '', kind = u8[i++], np = vi(), coords = [];
          let la = 0, lo = 0;
          for (let k = 0; k < np; k++) { la += zz(); lo += zz(); coords.push([lo / 1e5, la / 1e5]); }
          trailFeatures.push({ type: 'Feature', properties: { name, kind }, geometry: { type: 'LineString', coordinates: coords } });
          added++;
        }
      }
    }
    if (added) map.getSource('trails').setData({ type: 'FeatureCollection', features: trailFeatures });
  } catch (e) { /* the map still works without trails */ }
  trailBusy = false;
}

/* Keep a copy of the map and directions for outside Nomad's range. */
const stamp = () => new Date().toISOString().slice(0, 16).replace(/[:T]/g, '-');
function download(name, blob) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob); a.download = name; document.body.appendChild(a); a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 2000);
}
// the map as drawn (the route line included), from inside a frame so the canvas still holds it
function saveMapImage() {
  map.once('render', () => map.getCanvas().toBlob((b) => (b ? download(`map-${stamp()}.png`, b) : toast("Couldn't save the map image.")), 'image/png'));
  map.triggerRepaint();
}
// the turn list as plain text, each turn with its position in the format chosen in Layers
function saveDirections() {
  const r = plan.result;
  if (!r) return;
  const lines = [`Directions${plan.from ? ' from ' + plan.from.name : ''}${plan.to ? ' to ' + plan.to.name : ''}`,
    `${fmtTime(r.res.secs)}, ${fmtDist(r.res.dist)}, ${plan.mode === 'walk' ? 'on foot' : 'by car'}. Saved ${new Date().toLocaleString()}.`, ''];
  r.st.steps.forEach((st, i) => lines.push(`${String(i + 1).padStart(3)}. ${st.verb} ${st.name}${st.dist ? ' (' + fmtDist(st.dist) + ')' : ''}`
    + `\n       ${fmtLL(st.ll[0], st.ll[1])}`));
  download(`directions-${stamp()}.txt`, new Blob([lines.join('\n') + '\n'], { type: 'text/plain' }));
}

function openReach(p, mins) {
  openSheet(head('How far can I get', 'from ' + esc(p.name)) + `
    <div class="row scroll"><div class="seg"><button data-rm="drive" class="${plan.mode === 'drive' ? 'on' : ''}" aria-label="Drive">${icon('car', 18)}</button><button data-rm="walk" class="${plan.mode === 'walk' ? 'on' : ''}" aria-label="Walk">${icon('walk', 18)}</button></div>
      <div class="seg">${[15, 30, 60].map((m) => `<button data-min="${m}" class="${m === mins ? 'on' : ''}">${m} min</button>`).join('')}</div></div>
    <div class="pnote" id="reachOut">Green is close, red is near the limit.</div>`, { ctx: 'reach' });
  document.querySelectorAll('[data-rm]').forEach((b) => b.onclick = () => { plan.mode = prefs.mode = b.dataset.rm; savePrefs(); openReach(p, mins); if (mins) runReach(p, mins); });
  document.querySelectorAll('[data-min]').forEach((b) => b.onclick = () => { openReach(p, +b.dataset.min); runReach(p, +b.dataset.min); });
}
async function runReach(p, mins) {
  const out = $('reachOut');
  const R = await getRouter().catch(() => null);
  if (!R) { out.innerHTML = 'These maps have no directions. Add an area pack with Nomad-Tools.'; return; }
  if (R.nodeCount() > 150000) R.trim();
  R.prefer = plan.mode === 'drive' ? 'fast' : (prefs.walk === 'trails' ? 'trails' : 'fast');
  out.textContent = 'Reading roads…';
  const tick = setInterval(() => { if ($('reachOut') === out) out.textContent = `Reading roads… ${(R.stats.bytes / 1048576).toFixed(1)} MB`; }, 300);
  try {
    R.stats = { cells: 0, bytes: 0, batches: 0 };
    const here = await R.snap(p.lat, p.lon, plan.mode);
    if (!here) { out.textContent = 'No road near here on the maps with directions. Try from a spot on or near a road.'; return; }
    const segs = await R.reach(here, plan.mode, mins * 60);
    if ($('reachOut') !== out) return;              // the sheet moved on
    const secs = mins * 60;
    let far = 0, w = 180, s = 90, e = -180, n = -90;
    const features = segs.map((g) => {
      for (const [la, lo] of g.pts) { far = Math.max(far, NR.hav(p.lat, p.lon, la, lo)); w = Math.min(w, lo); e = Math.max(e, lo); s = Math.min(s, la); n = Math.max(n, la); }
      return { type: 'Feature', properties: { f: g.t / secs }, geometry: { type: 'LineString', coordinates: g.pts.map(([la, lo]) => [lo, la]) } };
    });
    map.getSource('reach').setData({ type: 'FeatureCollection', features });
    if (features.length) map.fitBounds([[w, s], [e, n]], { padding: mapPadding(30), duration: 600 });
    out.innerHTML = `Up to <b>${fmtDist(far)}</b> ${plan.mode === 'walk' ? 'on foot' : 'by car'} in ${mins} min, at normal speeds. <a href="#" class="lnk" id="reachClear">Clear</a>`;
    $('reachClear').onclick = (ev) => { ev.preventDefault(); map.getSource('reach').setData(emptyFC()); openReach(p); };
  } catch (err) {
    out.innerHTML = errBox(whyFailed(err));
  } finally { clearInterval(tick); }
}

/* The sheet (place card, plan, navigation): its first part always shows, anything in a
   .more shows when it is pulled up. */
let sheetCtx = '';
function openSheet(html, { min = false, ctx = '' } = {}) {
  const S = $('sheet');
  if (sheetCtx === 'reach' && ctx !== 'reach' && map && map.getSource('reach')) map.getSource('reach').setData(emptyFC());
  $('sheetBody').innerHTML = html; sheetCtx = ctx;
  S.classList.add('open'); S.classList.toggle('min', min); S.classList.toggle('nomore', !S.querySelector('.more'));
  S.querySelectorAll('[data-close]').forEach((b) => b.onclick = closeSheet);
  if (window.NomadIcons) NomadIcons.fill(S);
  $('sheetBody').scrollTop = 0;
  sheetH();
  backSync();
}
function sheetH() {
  const S = $('sheet');
  document.documentElement.style.setProperty('--sheet-h', (S.classList.contains('open') ? S.offsetHeight : 0) + 'px');
}
function setSheetMin(on) { $('sheet').classList.toggle('min', on); sheetH(); }
function closeSheet() {
  const was = sheetCtx;
  $('sheet').classList.remove('open'); sheetCtx = ''; sheetH();
  if (was === 'reach' && map.getSource('reach')) map.getSource('reach').setData(emptyFC());
  if (placeMarker) { placeMarker.remove(); placeMarker = null; }
  if (nav) stopNav();
  else if (stepping) { stopPreview(); renderPlan(); }
  else if (document.body.classList.contains('planning') && was !== 'plan') renderPlan();
  backSync();
}
/* The phone's Back closes what is open instead of leaving Maps: a sheet, then directions.
   One history entry (NomadBack, nomad-utils.js) stands for "something is open" and is
   put back after each Back while anything still is. */
const Back = typeof NomadBack !== 'undefined' ? NomadBack : null;
let backQueued = false;
function backSync() {
  if (!Back || backQueued) return;
  backQueued = true;
  // once the current change has settled: a sheet swapped for another is still open
  queueMicrotask(() => {
    backQueued = false;
    if ($('sheet').classList.contains('open') || planning()) Back.open('maps', onBack);
    else Back.done('maps');
  });
}
function onBack() {
  const open = $('sheet').classList.contains('open');
  if (nav || stepping) { if (nav) stopNav(); else stopPreview(); if (planning()) renderPlan(); else closeSheet(); }   // as Exit does
  else if (open && sheetCtx !== 'plan') closeSheet();
  else if (planning()) closePlan();
  else if (open) closeSheet();
  backSync();
}
if (window.ResizeObserver) new ResizeObserver(sheetH).observe($('sheet'));
// the handle: tap to toggle, drag up for more, down for less
(function () {
  const g = $('grip'); let y0 = null;
  g.addEventListener('pointerdown', (e) => { y0 = e.clientY; g.setPointerCapture(e.pointerId); });
  g.addEventListener('pointerup', (e) => {
    if (y0 == null) return;
    const dy = e.clientY - y0; y0 = null;
    if (!$('sheet').querySelector('.more')) { if (dy > 40 && sheetCtx !== 'plan' && sheetCtx !== 'nav') closeSheet(); return; }
    setSheetMin(dy > 25 ? true : dy < -25 ? false : !$('sheet').classList.contains('min'));
  });
})();
// room the floating parts take, so the map centres what matters in what is left
function mapPadding(extra = 30) {
  const top = document.body.classList.contains('navigating') ? $('banner').offsetHeight + 16 : $('top').getBoundingClientRect().bottom;
  // (also while stepping through a preview, which uses the same banner)
  return { top: top + extra, bottom: ($('sheet').classList.contains('open') ? $('sheet').offsetHeight : 0) + extra, left: extra, right: extra };
}
function topH() { document.documentElement.style.setProperty('--top-h', Math.round($('top').getBoundingClientRect().bottom) + 'px'); }
if (window.ResizeObserver) {
  new ResizeObserver(topH).observe($('top'));
  new ResizeObserver(() => document.documentElement.style.setProperty('--banner-h', $('banner').offsetHeight + 'px')).observe($('banner'));
}

let placeMarker = null, currentPlace = null;
// kind: '' (a place), 'wp' (your waypoint), 'start'. colours come from the theme (maps.html)
function marker(lat, lon, kind, el) {
  const d = el || document.createElement('div');
  if (!el) d.className = 'pin' + (kind ? ' ' + kind : '');
  return new maplibregl.Marker({ element: d }).setLngLat([lon, lat]).addTo(map);
}
const samePt = (a, b) => Math.abs(a.lat - b.lat) < 1e-6 && Math.abs(a.lon - b.lon) < 1e-6;
function showPlace(p) {
  if (placeMarker) placeMarker.remove();
  placeMarker = marker(p.lat, p.lon, '');
  const pins = store.get('pins', []), wp = pins.find((x) => samePt(x, p));
  const name = wp ? wp.name : p.name;
  const planning = document.body.classList.contains('planning');
  const sub = [wp ? 'Waypoint' : p.sub, fmtLL(p.lat, p.lon), me ? fmtDist(NR.hav(me.lat, me.lon, p.lat, p.lon)) + ' away' : ''].filter(Boolean).map(esc).join(' · ');
  const pt = { name, lat: p.lat, lon: p.lon };
  currentPlace = { name: wp ? name : p.name, lat: p.lat, lon: p.lon };
  openSheet(`<div id="plHead">${head(esc(name), sub)}</div>
    <div class="row scroll">${planning
      ? `<button class="btn primary" id="toHere">Set as destination</button><button class="btn" id="fromHere">Set as start</button>`
      : `<button class="btn primary" id="goHere"><span data-icon="directions"></span>Directions</button><button class="btn" id="fromHere">Start</button>`}
    <button class="btn" id="pinIt"><span data-icon="${wp ? 'check' : 'pin'}"></span>${wp ? 'Saved' : 'Save'}</button>
    ${wp ? '<button class="btn" id="renameIt"><span data-icon="pencil"></span>Rename</button>' : ''}
    <button class="btn" id="nearHere" aria-label="What's nearby"><span data-icon="nearby"></span>Nearby</button>
    <button class="btn" id="reachHere" aria-label="How far can I get from here"><span data-icon="timer"></span>How far</button></div>`, { ctx: 'place' });
  $('nearHere').onclick = () => openNearby(whereFrom());
  if (planning) {
    $('toHere').onclick = () => setPlanPoint('to', pt);
    $('fromHere').onclick = () => setPlanPoint('from', pt);
  } else {
    $('goHere').onclick = () => openPlan(null, pt);
    $('fromHere').onclick = () => openPlan(pt, null);
  }
  $('reachHere').onclick = () => openReach(pt);
  $('pinIt').onclick = () => {
    let list = store.get('pins', []);
    if (wp) { list = list.filter((x) => !samePt(x, p)); toast(`Removed “${wp.name}” from your waypoints.`); }
    else {
      // the next number nobody has: after a delete, list.length + 1 can already be taken
      let n = list.length + 1;
      while (list.some((x) => x.name === 'Waypoint ' + n)) n++;
      const nm = !p.name || p.name === 'Dropped pin' ? 'Waypoint ' + n : p.name;
      list.push({ name: nm, lat: p.lat, lon: p.lon });
      toast(`Saved as “${nm}”`, 3200);
    }
    store.set('pins', list); drawPins(); showPlace({ ...p, name: wp ? wp.name : p.name });
  };
  if ($('renameIt')) $('renameIt').onclick = () => {
    $('plHead').innerHTML = `<div class="rename"><input id="rnIn" value="${esc(name)}" aria-label="Waypoint name" maxlength="60" enterkeyhint="done"><button class="btn primary" id="rnOk">Save</button></div>`;
    const inp = $('rnIn'); inp.focus(); inp.select();
    const done = () => {
      const v = inp.value.trim(); if (!v) return;
      const list = store.get('pins', []).map((x) => samePt(x, p) ? { ...x, name: v } : x);
      store.set('pins', list); drawPins(); showPlace({ ...p, name: v });
    };
    $('rnOk').onclick = done; inp.onkeydown = (e) => { if (e.key === 'Enter') done(); if (e.key === 'Escape') showPlace(p); };
  };
}
let pinMarkers = [];
function drawPins() {
  pinMarkers.forEach((m) => m.remove());
  pinMarkers = store.get('pins', []).map((p) => {
    const m = marker(p.lat, p.lon, 'wp');
    m.getElement().title = p.name;
    m.getElement().addEventListener('click', (e) => { e.stopPropagation(); closeResults(); if (!nav) showPlace({ ...p, sub: 'waypoint' }); });
    return m;
  });
}
function onMapClick(e) {
  if (nav || stepping) return;
  if ($('results').classList.contains('open')) { closeResults(); document.activeElement && document.activeElement.blur(); return; }
  const lat = e.lngLat.lat, lon = e.lngLat.lng;
  // a named feature under the finger beats a bare coordinate
  const f = map.queryRenderedFeatures([[e.point.x - 6, e.point.y - 6], [e.point.x + 6, e.point.y + 6]])
    .find((x) => x.properties && x.properties.name && x.layer.type === 'symbol');
  if (pickFor) { const which = pickFor; cancelPick(); setPlanPoint(which, { name: f ? f.properties.name : fmtLL(lat, lon), lat, lon }); return; }
  if (f) showPlace({ name: f.properties.name, lat, lon, sub: (f.properties.kind || '').replace(/_/g, ' ') });
  else showPlace({ name: 'Dropped pin', lat, lon });
}

let router = null;
function resetRouter() { router = null; }
async function getRouter() {
  if (router) return router;
  const rp = activePacks().filter((p) => p.man.routing);
  if (!rp.length) return null;
  const byId = new Map(rp.map((p) => [p.id, p]));
  const ranged = async (file, off, len) => {
    let wait = 300;
    for (let i = 0; i < 6; i++) {
      const res = await fetch(media(file), { headers: { Range: `bytes=${off}-${off + len - 1}` }, cache: 'no-store' });
      if (res.status === 206) return res.arrayBuffer();
      if (res.status === 200) return (await res.arrayBuffer()).slice(off, off + len);
      await new Promise((ok) => setTimeout(ok, wait)); wait = Math.min(wait * 1.8, 3000);
    }
    throw new Error('the card stayed busy');
  };
  const r = new NR.Router((id, off, len) => { const p = byId.get(id); return ranged(p.base + p.man.routing.data, off, len); });
  await Promise.all(rp.map(async (p) => {
    // newer packs say where each level of the index is: read only what a trip needs
    if (p.man.routing.levels && p.man.routing.bounds) {
      r.addPackLazy(p.id, p.man.routing, (off, len) => ranged(p.base + p.man.routing.index, off, len));
      return;
    }
    const res = await fetch(media(p.base + p.man.routing.index), { cache: 'no-cache' });
    if (!res.ok) throw new Error('routing index for ' + p.name + ': HTTP ' + res.status);
    r.addPack(p.id, await res.arrayBuffer());
  }));
  router = r;
  return r;
}

/* Directions: start and destination in the card at the top, the route's summary, Start and
   turns in the sheet. */
const plan = { from: null, to: null, mode: prefs.mode, result: null };
let pickFor = null, routeGeo = null, startM = null, endM = null;
const myLoc = (p) => ({ name: 'Your location', lat: p.lat, lon: p.lon, gps: true });
const planning = () => document.body.classList.contains('planning');
function fillFields() {
  if (document.activeElement !== $('pf')) $('pf').value = plan.from ? plan.from.name : '';
  if (document.activeElement !== $('pt')) $('pt').value = plan.to ? plan.to.name : '';
}
function openPlan(from, to) {
  closeResults(); cancelPick(); $('panel').classList.remove('open');
  if (placeMarker) { placeMarker.remove(); placeMarker = null; }
  if (!planning()) { plan.from = null; plan.to = null; plan.result = null; }
  plan.from = from || plan.from || (me ? myLoc(me) : null);
  plan.to = to || plan.to;
  document.body.classList.add('planning');
  backSync();
  fillFields();
  if (plan.from && plan.to) computeRoute();
  else { renderPlan(); searchInto(plan.from ? 'to' : 'from', ''); }
}
function closePlan() {
  closeResults(); cancelPick(); routeSeq++;
  document.body.classList.remove('planning');
  plan.result = null; routeGeo = null;
  if (map.getSource('route')) map.getSource('route').setData(emptyFC());
  if (startM) { startM.remove(); startM = null; } if (endM) { endM.remove(); endM = null; }
  if (sheetCtx === 'plan' || sheetCtx === 'place') { $('sheet').classList.remove('open'); sheetCtx = ''; sheetH(); }
  backSync();
}
function setPlanPoint(which, p) {
  if (placeMarker) { placeMarker.remove(); placeMarker = null; }
  plan[which] = p; plan.result = null; fillFields();
  if (plan.from && plan.to) { closeResults(); computeRoute(); }
  else { renderPlan(); searchInto(which === 'from' ? 'to' : 'from', ''); }
}
function startPick(which) {
  pickFor = which; closeResults();
  $('pickWhich').textContent = which === 'from' ? 'start' : 'destination';
  $('pickHint').hidden = false;
  setSheetMin(true);
}
function cancelPick() { pickFor = null; $('pickHint').hidden = true; }
$('pickCancel').onclick = () => { const w = pickFor; cancelPick(); if (w) searchInto(w, ''); };
$('dirBack').onclick = closePlan;
$('pSwap').onclick = () => { [plan.from, plan.to] = [plan.to, plan.from]; plan.result = null; fillFields(); if (plan.from && plan.to) computeRoute(); else renderPlan(); };
wireSearchInput($('pf'), 'from');
wireSearchInput($('pt'), 'to');
function renderPlan(extra = '') {
  const r = plan.result;
  let out = extra;
  if (!out) out = r ? routeSummary(r)
    : `<div class="pnote">${!plan.from && !plan.to ? 'Choose a start and a destination'
      : !plan.from ? 'Choose a start' : 'Choose a destination'}</div>`;
  openSheet(`<div class="row scroll" style="margin-top:0"><div class="seg"><button data-m="drive" class="${plan.mode === 'drive' ? 'on' : ''}" aria-label="Drive" title="Drive">${icon('car', 18)}</button><button data-m="walk" class="${plan.mode === 'walk' ? 'on' : ''}" aria-label="Walk" title="Walk">${icon('walk', 18)}</button></div>
    ${plan.mode === 'drive' ? `<div class="seg" role="group" aria-label="Route">${ROUTE_OPTS.map(([k, label]) =>
      `<button data-r="${k}" class="${prefs.route === k ? 'on' : ''}" title="${label}">${ROUTE_SHORT[k] || label}</button>`).join('')}</div>`
    : `<div class="seg" role="group" aria-label="Walk">${WALK_OPTS.map(([k, label]) =>
      `<button data-w="${k}" class="${prefs.walk === k ? 'on' : ''}">${label}</button>`).join('')}</div>`}</div>
    <div id="pOut">${out}</div>`, { ctx: 'plan', min: !!r });
  const again = () => { renderPlan(); if (plan.from && plan.to) computeRoute(); };
  document.querySelectorAll('[data-m]').forEach((b) => b.onclick = () => { plan.mode = prefs.mode = b.dataset.m; savePrefs(); again(); });
  document.querySelectorAll('[data-r]').forEach((b) => b.onclick = () => { prefs.route = b.dataset.r; savePrefs(); again(); });
  document.querySelectorAll('[data-w]').forEach((b) => b.onclick = () => { prefs.walk = b.dataset.w; savePrefs(); again(); });
  if ($('saveMap')) $('saveMap').onclick = saveMapImage;
  if ($('saveSteps')) $('saveSteps').onclick = saveDirections;
  if ($('pGo')) $('pGo').onclick = startNav;
  if ($('pSteps')) $('pSteps').onclick = () => setSheetMin(!$('sheet').classList.contains('min'));
  if ($('pRetry')) $('pRetry').onclick = () => computeRoute();
  document.querySelectorAll('.steps li').forEach((li) => li.onclick = () => {
    const s = plan.result.st.steps[+li.dataset.i]; setSheetMin(true); map.flyTo({ center: [s.ll[1], s.ll[0]], zoom: 16 });
  });
}
// what a drive is planned for (nomad-route.js Router.prefer)
const ROUTE_OPTS = [['fast', 'Fastest'], ['short', 'Shortest'], ['nohwy', 'No highways']];
const ROUTE_SHORT = { nohwy: 'No hwys' };
const WALK_OPTS = [['fast', 'Quickest'], ['trails', 'Prefer trails']];
const STEP_ICON = { depart: 'navigate', straight: 'arrow-up', left: 'turn-left', right: 'turn-right', 'slight-left': 'slight-left',
  'slight-right': 'slight-right', 'sharp-left': 'turn-left', 'sharp-right': 'turn-right', uturn: 'uturn', arrive: 'flag' };
// where only a highways pack covers an end, the route starts or stops on the nearest big road
function offRoadNote(res) {
  const [a, b] = res.offRoad || [0, 0], far = 1000;
  const parts = [];
  if (a > far) parts.push(`starts on the nearest big road, ${fmtDist(a)} from the start`);
  if (b > far) parts.push(`ends on the nearest big road, ${fmtDist(b)} from the destination`);
  return parts.length ? `<div class="pnote">No street map on this card there. The route ${parts.join(' and ')}.</div>` : '';
}
function routeSummary(r) {
  const via = r.res.legs.filter((l) => l.name).sort((a, b) => b.dist - a.dist)[0];
  return `<div class="summary"><div class="t"><b>${fmtTime(r.res.secs)}</b>${fmtDist(r.res.dist)}${via ? '<br>via ' + esc(via.name) : ''}</div>
      <button class="btn primary" id="pGo"><span data-icon="navigate"></span>${plan.from && !plan.from.gps && !SIM ? 'Preview' : 'Start'}</button></div>
    ${offRoadNote(r.res)}
    <div class="row" style="margin-top:6px"><a href="#" class="lnk" id="pSteps" onclick="return false">${r.st.steps.length} steps</a></div>
    <div class="more">
      <ol class="steps">${r.st.steps.map((s, i) => `<li data-i="${i}"><span class="ic">${icon(STEP_ICON[s.kind] || 'arrow-up', 18)}</span>
        <span>${esc(s.verb)} ${esc(s.name)}</span><span class="d">${s.dist ? fmtDist(s.dist) : ''}</span></li>`).join('')}</ol>
      <div class="row"><button class="btn" id="saveMap"><span data-icon="download"></span>Save map image</button><button class="btn" id="saveSteps"><span data-icon="download"></span>Save directions</button></div>
    </div>`;
}
let routeSeq = 0;
async function computeRoute(fromOverride) {
  const seq = ++routeSeq, f = fromOverride || plan.from, t = plan.to;
  plan.result = null;
  const say = (html) => { if (!nav && seq === routeSeq && planning()) renderPlan(html); };
  say('<div class="pnote" id="rp">Finding a route…</div>');
  let R;
  try { R = await getRouter(); } catch (e) { say(errBox(whyFailed(e), 'pRetry')); return null; }
  if (!R) { say('<div class="pnote">These maps have no directions. Add an area pack with Nomad-Tools.</div>'); return null; }
  const t0 = performance.now();
  R.prefer = plan.mode === 'drive' ? prefs.route : (prefs.walk === 'trails' ? 'trails' : 'fast');
  const tick = setInterval(() => {
    const el = $('rp'); if (!el) return;
    const long = performance.now() - t0 > 6000 ? ' · a long trip can take a minute' : '';
    el.textContent = `Finding a route… ${(R.stats.bytes / 1048576).toFixed(1)} MB${long}`;
  }, 250);
  try {
    if (R.nodeCount() > 150000) R.trim();          // the last trip's roads: room for this one
    const a = await R.snap(f.lat, f.lon, plan.mode, { place: !f.gps }), b = await R.snap(t.lat, t.lon, plan.mode, { place: true });
    if (seq !== routeSeq) return null;
    if (!a || !b) {
      say(errBox(`${!a ? 'The start' : 'The destination'} is ${plan.mode === 'walk' ? 'not near a road or path' : 'not near a road'} on the maps with directions. Choose a spot on or near a road.`));
      return null;
    }
    const res = await R.route(a, b, plan.mode);
    if (seq !== routeSeq) return null;
    if (!res) {
      const why = R.tooBig ? 'This trip is too long to plan in one go on this phone. Plan to a town partway there, then on from it.'
        : R.noWay ? 'No road joins those places on the maps on this card (an island, or a gap between map packs).'
          : `No ${plan.mode === 'walk' ? 'walking' : 'driving'} route between those points.${plan.mode === 'drive' && prefs.route === 'nohwy' ? ' Try allowing highways.' : ''}`;
      say(errBox(why));
      return null;
    }
    plan.result = { res, st: NR.steps(res), ms: performance.now() - t0 };
    drawRoute(plan.result, !nav);
    if (!nav && planning()) renderPlan();
    return plan.result;
  } catch (e) {
    say(errBox(whyFailed(e), 'pRetry'));
    return null;
  } finally { clearInterval(tick); }
}
function drawRoute(r, fit) {
  const line = r.st.line.map(([la, lo]) => [lo, la]);
  routeGeo = { type: 'FeatureCollection', features: [{ type: 'Feature', properties: {}, geometry: { type: 'LineString', coordinates: line } }] };
  if (map.getSource('route')) map.getSource('route').setData(routeGeo);
  if (startM) startM.remove(); if (endM) endM.remove();
  startM = marker(line[0][1], line[0][0], 'start');
  endM = marker(line[line.length - 1][1], line[line.length - 1][0], '');
  if (fit) {
    let w = 180, s = 90, e = -180, n = -90;
    for (const [lo, la] of line) { w = Math.min(w, lo); e = Math.max(e, lo); s = Math.min(s, la); n = Math.max(n, la); }
    requestAnimationFrame(() => map.fitBounds([[w, s], [e, n]], { padding: mapPadding(40), duration: 600 }));
  }
}

let me = null, meMarker = null, watchId = null, follow = false;
const SIM = new URLSearchParams(location.search).has('sim');
/* Location needs a page the browser trusts. Chrome on Android can be told to trust this
   address. iPhones only share location with a secure site, which Nomad's pages aren't. */
const IOS = /iPhone|iPad|iPod/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
function copyText(t, btn) {
  const done = () => { btn.textContent = 'Copied'; setTimeout(() => { btn.textContent = 'Copy'; }, 1500); };
  if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(t).then(done, () => {});
  else {                                   // plain pages have no clipboard API
    const ta = document.createElement('textarea'); ta.value = t; ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta); ta.select();
    try { if (document.execCommand('copy')) done(); } catch (e) {}
    ta.remove();
  }
}
function gpsOk() {
  if (SIM) return true;
  if (window.isSecureContext && navigator.geolocation) return true;
  for (const id of ['gpsIphone', 'gpsChrome']) $(id).hidden = true;
  $('gpsOrigin').textContent = location.origin;
  $('gpsOriginCopy').onclick = (e) => copyText(location.origin, e.target);
  document.querySelectorAll('#gpsModal [data-copy]').forEach((b) => b.onclick = () => copyText(b.dataset.copy, b));
  $('gpsModal').classList.add('open');
  $('gpsModal').scrollTop = 0;
  $(IOS ? 'gpsIphone' : 'gpsChrome').hidden = false;
  return false;
}
// you on the map: the marker turns with the map, so the pointer shows your heading whatever
// way the map is turned (navigation turns it to the road ahead when the phone has no heading)
function pointMe(deg) {
  if (!meMarker) return;
  const el = meMarker.getElement();
  el.classList.toggle('heading', deg != null && !isNaN(deg));
  if (deg != null && !isNaN(deg)) meMarker.setRotation(deg);
}
function onFix(p) {
  me = p;
  if (!meMarker) {
    const el = document.createElement('div'); el.className = 'me';
    el.innerHTML = '<div class="dotc"></div><svg class="arrow" viewBox="0 0 36 36" aria-hidden="true"><path d="M18 3 L29 31 L18 25 L7 31 Z" fill="currentColor" stroke="#fff" stroke-width="2.5" stroke-linejoin="round"/></svg>';
    meMarker = new maplibregl.Marker({ element: el, rotationAlignment: 'map', pitchAlignment: 'map' }).setLngLat([p.lon, p.lat]).addTo(map);
  }
  meMarker.setLngLat([p.lon, p.lat]);
  pointMe(p.heading != null && (p.speed == null || p.speed > 0.5) ? p.heading : null);
  if (nav) navUpdate(p);
  else if (follow) map.easeTo({ center: [p.lon, p.lat], duration: 500 });
}
const GPS_WHY = {
  1: 'Location is blocked for this page. Allow it in the browser: tap the icon left of the address, then Permissions or Site settings, then Location.',
  2: "This phone can't work out where it is. Check Location is on in the phone's settings.",
  3: 'Still waiting for a GPS fix. It can take a minute, and is quicker outdoors.',
};
let gpsSaid = 0;
function startWatch() {
  if (watchId != null || SIM) return;
  watchId = navigator.geolocation.watchPosition((pos) => onFix({ lat: pos.coords.latitude, lon: pos.coords.longitude,
    acc: pos.coords.accuracy, heading: pos.coords.heading, speed: pos.coords.speed, t: pos.timestamp }),
  (err) => {
    if (Date.now() - gpsSaid > 15000) { gpsSaid = Date.now(); toast(GPS_WHY[err.code] || GPS_WHY[3], 6000); }
    if (err.code === 1) { navigator.geolocation.clearWatch(watchId); watchId = null; }
  },
  { enableHighAccuracy: true, maximumAge: 1000, timeout: 20000 });
}
function locateOnce() {
  if (!gpsOk()) return Promise.resolve(null);
  if (me) return Promise.resolve(me);
  startWatch();
  toast('Finding your location…', 0);
  return new Promise((res) => {
    let n = 0; const t = setInterval(() => {
      if (me || ++n > 60 || watchId == null) {
        clearInterval(t);
        if (me) toast(''); else if (watchId != null) toast(GPS_WHY[3], 5000);
        res(me);
      }
    }, 250);
  });
}
function setFollow(on) { follow = on; $('locBtn').classList.toggle('on', on); }
$('locBtn').onclick = async () => {
  if (nav) { nav.free = false; setFollow(true); if (me) navUpdate(me); return; }
  const p = await locateOnce(); if (!p) return;
  setFollow(true); map.flyTo({ center: [p.lon, p.lat], zoom: Math.max(map.getZoom(), 15) });
};

let nav = null;
function lineMeasure(line) {
  const cum = [0];
  for (let i = 1; i < line.length; i++) cum.push(cum[i - 1] + NR.hav(line[i - 1][0], line[i - 1][1], line[i][0], line[i][1]));
  return cum;
}
// nearest point on the route to p: segment index, metres along, metres off
function project(line, cum, p, from) {
  const kx = Math.cos(p.lat * Math.PI / 180);
  let best = { i: from, along: cum[from], off: Infinity };
  const end = Math.min(line.length - 1, from + 400);
  for (let i = Math.max(0, from - 20); i < end; i++) {
    const [y1, x1] = line[i], [y2, x2] = line[i + 1];
    const dx = (x2 - x1) * kx, dy = y2 - y1, L2 = dx * dx + dy * dy;
    let t = L2 ? (((p.lon - x1) * kx) * dx + (p.lat - y1) * dy) / L2 : 0; t = Math.max(0, Math.min(1, t));
    const qy = y1 + (y2 - y1) * t, qx = x1 + (x2 - x1) * t, d = NR.hav(p.lat, p.lon, qy, qx);
    if (d < best.off) best = { i, along: cum[i] + (cum[i + 1] - cum[i]) * t, off: d, ll: [qy, qx] };
  }
  return best;
}
function speak(text) {
  if (!prefs.voice || !window.speechSynthesis) return;
  try { speechSynthesis.cancel(); const u = new SpeechSynthesisUtterance(text); u.rate = 1.02; speechSynthesis.speak(u); } catch (e) {}
}
async function startNav() {
  if (!plan.result) return;
  // a start that isn't where you are: nothing to follow, so step through the turns instead
  if (!plan.from.gps && !SIM) { startPreview(); return; }
  nav = { ...navRoute(plan.result), said: new Set(), rerouting: false, lastReroute: 0, free: false };
  // a long trip outlives the page: if it reloads on the way, offer to carry on
  if (plan.to) store.set('trip', { to: plan.to, mode: plan.mode, t: Date.now() });
  closeResults(); setFollow(true);
  $('banner').classList.add('open');
  document.body.classList.add('navigating');
  openSheet(`<div class="navbar"><div class="eta" id="nEtaBox"><b id="nRemain">${fmtTime(nav.r.res.secs)}</b><span id="nEta">${fmtDist(nav.r.res.dist)}</span></div>
      <button class="ib" id="nVoice" aria-label="${prefs.voice ? 'Mute voice' : 'Voice on'}">${icon(prefs.voice ? 'volume' : 'volume-off', 22)}</button>
      <button class="btn danger" id="nEnd">Exit</button></div>
    <div class="more">
      <div class="pnote" id="nDirect"></div><div class="pnote" id="nPos"></div>
      <div class="row"><button class="btn" id="nBlock"><span data-icon="warning"></span>Road blocked ahead</button></div>
    </div>`, { ctx: 'nav', min: true });
  $('nEnd').onclick = () => { stopNav(); renderPlan(); };
  $('nEtaBox').onclick = () => setSheetMin(!$('sheet').classList.contains('min'));
  $('nBlock').onclick = () => { setSheetMin(true); roadBlocked(); };
  $('nVoice').onclick = () => { prefs.voice = !prefs.voice; savePrefs(); $('nVoice').innerHTML = icon(prefs.voice ? 'volume' : 'volume-off', 22); };
  const s0 = nav.r.st.steps[0];
  speak(`${s0.verb} ${s0.name}`);
  if (SIM) simulate(); else startWatch();
  if (me) navUpdate(me);
  else map.flyTo({ center: [nav.line[0][1], nav.line[0][0]], zoom: 16 });
}
/* Preview: a route planned from somewhere other than where you are (or with no GPS). The
   turns are stepped through by hand, Prev and Next, the map turned to each one. */
let stepping = null;
function startPreview() {
  const r = plan.result; if (!r) return;
  const steps = r.st.steps, line = r.st.line;
  closeResults(); setFollow(false);
  stepping = { i: 0, m: null };
  const el = document.createElement('div'); el.className = 'me heading';
  el.innerHTML = '<div class="dotc"></div><svg class="arrow" viewBox="0 0 36 36" aria-hidden="true"><path d="M18 3 L29 31 L18 25 L7 31 Z" fill="currentColor" stroke="#fff" stroke-width="2.5" stroke-linejoin="round"/></svg>';
  stepping.m = new maplibregl.Marker({ element: el, rotationAlignment: 'map', pitchAlignment: 'map' }).setLngLat([line[0][1], line[0][0]]).addTo(map);
  document.body.classList.add('navigating');
  $('banner').classList.add('open');
  openSheet(`<div class="navbar"><div class="eta"><b id="sCount" class="small"></b><span id="sLeft"></span></div>
      <button class="btn step" id="sPrev" aria-label="Previous turn" title="Previous turn">${icon('chevron-left', 22)}</button>
      <button class="btn primary step" id="sNext" aria-label="Next turn" title="Next turn">${icon('chevron-right', 22)}</button>
      <button class="btn danger" id="sEnd">Exit</button></div>`, { ctx: 'nav' });
  const show = (say) => {
    const i = stepping.i, s = steps[i], nx = steps[i + 1];
    $('bIc').innerHTML = icon(STEP_ICON[s.kind] || 'arrow-up', 40);
    $('bDist').textContent = s.kind === 'arrive' ? 'Arrive' : s.dist ? fmtDist(s.dist) : '';
    $('bWhat').textContent = s.kind === 'arrive' ? (plan.to ? plan.to.name : '') : `${s.verb} ${s.name}`.trim();
    $('bThen').textContent = nx ? `Then ${nx.verb.toLowerCase()} ${nx.name}`.trim() : '';
    let d = 0, t = 0; for (let k = i; k < steps.length; k++) { d += steps[k].dist || 0; t += steps[k].secs || 0; }
    $('sCount').textContent = `Turn ${i + 1} of ${steps.length}`;
    $('sLeft').textContent = s.kind === 'arrive' ? 'the end of the route' : `${fmtDist(d)} · ${fmtTime(t)} from here`;
    $('sPrev').disabled = i === 0; $('sNext').disabled = i === steps.length - 1;
    const at = s.ll, ahead = line[Math.min(line.length - 1, s.at + 3)];
    const brg = s.kind === 'arrive' ? map.getBearing() : bearingOf(at, ahead);
    stepping.m.setLngLat([at[1], at[0]]).setRotation(brg);
    map.easeTo({ center: [at[1], at[0]], bearing: brg, pitch: 0, zoom: Math.min(17, zoomLimitAt(at[1], at[0])), duration: 700, padding: mapPadding(40) });
    if (say) speak(s.kind === 'arrive' ? 'Arrive' : `${s.verb} ${s.name}`);
  };
  $('sPrev').onclick = () => { if (stepping.i > 0) { stepping.i--; show(true); } };
  $('sNext').onclick = () => { if (stepping.i < steps.length - 1) { stepping.i++; show(true); } };
  $('sEnd').onclick = () => { stopPreview(); renderPlan(); };
  show(false);
  toast(`Preview from ${plan.from.name}. For live directions, start from Your location.`, 4500);
}
function stopPreview() {
  if (!stepping) return;
  stepping.m.remove(); stepping = null;
  $('banner').classList.remove('open'); document.body.classList.remove('navigating');
  map.easeTo({ pitch: 0, bearing: 0, duration: 400 });
}
function stopNav() {
  if (router) router.clearBlocks();           // closures last a trip, no longer
  nav = null; store.set('trip', null); $('banner').classList.remove('open'); document.body.classList.remove('navigating');
  if (simTimer) { clearInterval(simTimer); simTimer = null; }
  setFollow(false);
  map.easeTo({ pitch: 0, bearing: 0, duration: 400 });
}
function navUpdate(p) {
  const N = nav; if (!N || N.rerouting) return;
  const pr = project(N.line, N.cum, p, N.idx);
  N.idx = pr.i;
  const total = N.cum[N.cum.length - 1], left = Math.max(0, total - pr.along);
  // off the line for a while: plan again from here
  const tol = Math.max(45, (p.acc || 0) * 1.5);
  N.off = pr.off > tol ? N.off + 1 : 0;
  if (N.off >= 3 && Date.now() - N.lastReroute > 8000) { reroute(p); return; }
  if (left < 25) { arrive(); return; }
  const steps = N.r.st.steps;
  let k = steps.findIndex((s) => N.cum[s.at] > pr.along + 3);
  if (k < 0) k = steps.length - 1;
  const st = steps[k], dist = N.cum[st.at] - pr.along, next = steps[k + 1];
  $('bIc').innerHTML = icon(STEP_ICON[st.kind] || 'arrow-up', 40);
  $('bDist').textContent = fmtDist(dist);
  $('bWhat').textContent = st.kind === 'arrive' ? 'Arrive' : `${st.verb} ${st.name}`;
  $('bThen').textContent = next && dist < 400 ? `Then ${next.verb.toLowerCase()} ${next.name}` : '';
  // say each turn three times at most: early, close, now
  const fast = (p.speed || 0) > 18;
  for (const [mark, lead] of [[fast ? 1600 : 400, 'In ' + fmtDist(fast ? 1600 : 400) + ', '], [fast ? 400 : 120, ''], [25, 'Now ']]) {
    const key = k + '/' + mark;
    if (dist <= mark && dist > mark * 0.4 && !N.said.has(key)) {
      N.said.add(key);
      speak(lead + (st.kind === 'arrive' ? 'you will arrive' : `${st.verb.toLowerCase()} ${st.name}`));
      break;
    }
  }
  // straight to the destination, and where you are: what still helps off the roads
  if (plan.to && $('nDirect')) {
    const d = NR.hav(p.lat, p.lon, plan.to.lat, plan.to.lon), dir = COMPASS[Math.round(bearingOf([p.lat, p.lon], [plan.to.lat, plan.to.lon]) / 45) % 8];
    $('nDirect').textContent = `${plan.to.name}: ${fmtDist(d)} ${dir} in a straight line`;
    $('nPos').textContent = `You are at ${fmtLL(p.lat, p.lon)}`;
  }
  const frac = total ? left / total : 0;
  if ($('nRemain')) $('nRemain').textContent = fmtTime(N.r.res.secs * frac);
  if ($('nEta')) $('nEta').textContent = fmtDist(left) + ' · arrive ' + new Date(Date.now() + N.r.res.secs * frac * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
  // the map follows, pointing the way you are going
  const ahead = N.line[Math.min(N.line.length - 1, pr.i + 3)];
  const brg = p.heading != null && (p.speed || 0) > 2 ? p.heading : bearingOf(pr.ll || [p.lat, p.lon], ahead);
  pointMe(brg);
  if (N.free) return;                          // looking around: the location button comes back
  // no deeper than the maps here go (a fight with clampZoom reloads tiles every fix), and
  // flat where only an overview covers it: tilted, it would load out to the horizon
  const lim = zoomLimitAt(p.lon, p.lat);
  // you sit low on the screen, the road ahead above you, the turn banner clear
  const pad = mapPadding(0), h = map.getContainer().clientHeight;
  map.easeTo({ center: [p.lon, p.lat], bearing: brg, pitch: lim >= 14 ? 45 : 0, zoom: Math.min(fast ? 15 : 17, lim), duration: 900,
    padding: { top: Math.max(pad.top, h * 0.45), bottom: pad.bottom + 20, left: 0, right: 0 } });
}
const COMPASS = ['north', 'northeast', 'east', 'southeast', 'south', 'southwest', 'west', 'northwest'];
function bearingOf(a, b) {
  const r = Math.PI / 180, y = Math.sin((b[1] - a[1]) * r) * Math.cos(b[0] * r);
  const x = Math.cos(a[0] * r) * Math.sin(b[0] * r) - Math.sin(a[0] * r) * Math.cos(b[0] * r) * Math.cos((b[1] - a[1]) * r);
  return (Math.atan2(y, x) / r + 360) % 360;
}
// what navigation follows: the line, distances along it, and which leg each point is in
function navRoute(r) {
  const legEnd = [];
  let n = 0;
  r.res.legs.forEach((l, i) => { n += l.pts.length - (i ? 1 : 0); legEnd.push(n - 1); });
  return { r, line: r.st.line, cum: lineMeasure(r.st.line), legEnd, idx: 0, off: 0 };
}
/* Off the route: first look for the quickest way back onto it from here (a few reads of
   the roads nearby, the usual case after a missed turn). Only if there is none, plan the
   whole trip again from here. */
async function reroute(p) {
  const N = nav; N.rerouting = true; N.lastReroute = Date.now();
  $('bWhat').textContent = 'Finding a new route…'; $('bDist').textContent = ''; $('bThen').textContent = '';
  speak('Rerouting');
  let r = null;
  try {
    const R = await getRouter(), t0 = performance.now();
    const here = R && await R.snap(p.lat, p.lon, plan.mode);
    let leg = N.legEnd.findIndex((e) => e >= N.idx);
    if (leg < 0) leg = N.legEnd.length - 1;
    const back = here && await R.rejoin(here, N.r.res, leg, plan.mode);
    if (back && nav) {
      r = { res: back, st: NR.steps(back), ms: performance.now() - t0 };
      plan.result = r; drawRoute(r, false);
    }
  } catch (e) { r = null; }
  if (!r && nav) r = await computeRoute({ name: 'Your location', lat: p.lat, lon: p.lon, gps: true });
  if (!nav) return;
  if (r) Object.assign(nav, { ...navRoute(r), said: new Set() });
  else outOfReach();
  nav.rerouting = false;
}
/* Road blocked ahead: the next stretch of the route is closed for this trip, and the route
   finds a way around it (back the way you came if it must). Nothing is kept after the trip. */
async function roadBlocked() {
  const N = nav; if (!N || N.rerouting) return;
  const R = await getRouter().catch(() => null); if (!R) return;
  let leg = N.legEnd.findIndex((e) => e >= N.idx);
  if (leg < 0) leg = N.legEnd.length - 1;
  R.blockAhead(N.r.res.legs, leg, 300);
  speak('Finding a way around');
  toast('Road blocked for this trip. Finding a way around…', 3000);
  N.lastReroute = 0;
  await reroute(me || { lat: N.line[N.idx][0], lon: N.line[N.idx][1] });
}
// Nomad is out of reach: what is on screen still works, a new plan has to wait
function outOfReach() {
  if (!nav) return;
  $('bWhat').textContent = "Out of Nomad's range: keep to the route shown";
  $('bThen').textContent = "Rerouting resumes on Nomad's WiFi";
  toast("Out of Nomad's range", 5000);
}
function arrive() {
  const short = nav && nav.r.res.offRoad && nav.r.res.offRoad[1] > 1000 ? nav.r.res.offRoad[1] : 0;
  $('bIc').innerHTML = icon('flag', 40); $('bDist').textContent = short ? 'End of route' : 'Arrived';
  $('bWhat').textContent = short ? `${plan.to ? plan.to.name : 'The destination'} is ${fmtDist(short)} from here` : (plan.to ? plan.to.name : '');
  $('bThen').textContent = '';
  speak(short ? `This is the end of the route. Your destination is ${fmtDist(short)} away, past the roads on this card.` : 'You have arrived');
  const n = nav; nav = null; store.set('trip', null);
  if (simTimer) { clearInterval(simTimer); simTimer = null; }
  setTimeout(() => { if (!nav && n) { $('banner').classList.remove('open'); document.body.classList.remove('navigating'); setFollow(false); if (planning()) renderPlan(); } }, 4000);
}

/* ?sim drives the route at road speed, for trying navigation without moving (and for tests).
   ?sim=off wanders 80 m off the route a third of the way along, to show a reroute. */
let simTimer = null;
function simulate() {
  const N = nav, line = N.line, cum = N.cum, speed = plan.mode === 'walk' ? 1.4 : 22;
  const wander = new URLSearchParams(location.search).get('sim') === 'off';
  let s = 0, t0 = Date.now(), wandered = false;
  simTimer = setInterval(() => {
    if (!nav) { clearInterval(simTimer); simTimer = null; return; }
    s = Math.min(cum[cum.length - 1], s + speed * 4);       // 4x real time
    let i = 0; while (i < cum.length - 2 && cum[i + 1] < s) i++;
    const t = cum[i + 1] > cum[i] ? (s - cum[i]) / (cum[i + 1] - cum[i]) : 0;
    let lat = line[i][0] + (line[i + 1][0] - line[i][0]) * t, lon = line[i][1] + (line[i + 1][1] - line[i][1]) * t;
    if (wander && !wandered && s > cum[cum.length - 1] / 3) { lat += 0.0008; if (nav.off >= 3) wandered = true; }
    onFix({ lat, lon, acc: 8, heading: bearingOf(line[i], line[i + 1]), speed, t: Date.now() - t0 });
  }, 1000);
}

function offerResume(trip) {
  openSheet(head(`Carry on to ${esc(trip.to.name)}?`, '') + `
    <div class="row"><button class="btn primary" id="rGo"><span data-icon="navigate"></span>Carry on</button><button class="btn" id="rNo">No thanks</button></div>`, { ctx: 'resume' });
  $('rNo').onclick = () => { store.set('trip', null); closeSheet(); };
  $('rGo').onclick = async () => {
    const p = await locateOnce(); if (!p) return;
    plan.mode = trip.mode || plan.mode;
    document.body.classList.add('planning');
    plan.from = myLoc(p); plan.to = trip.to; fillFields();
    if (await computeRoute()) startNav();
  };
}

function showEmpty(html) { const E = $('empty'); E.style.display = 'flex'; E.innerHTML = html; }
(async function boot() {
  if (window.NomadIcons) NomadIcons.fill(document);
  setOnPrimary();
  topH();
  const hv = /#v=(-?[\d.]+),(-?[\d.]+),([\d.]+)/.exec(location.hash);
  if (hv) { store.set('view', { c: [+hv[1], +hv[2]], z: +hv[3] }); history.replaceState(null, '', location.pathname + location.search); }
  try { await loadPacks(); }
  catch (e) {
    showEmpty(`<b>Can't load the maps</b><span>${whyFailed(e)}</span><button class="btn primary" onclick="location.reload()">Try again</button><a class="btn" href="/menu">Back to the menu</a>`);
    return;
  }
  if (!packs.length && !rasters.length) {
    showEmpty('<b>No maps on this card yet</b><span>Add map packs with Nomad-Tools.</span><a class="btn" href="/menu">Back to the menu</a>');
    return;
  }
  if (!packs.length && rasters.length && !prefs.raster.length) prefs.raster = rasters.map((r) => r.id);
  startMap();
  map.once('load', drawPins);
  const trip = store.get('trip', null);
  if (trip && trip.to && Date.now() - trip.t < 2 * 86400e3) offerResume(trip);
  window.nomadMaps = { map, packs, plan, openPlan, setPlanPoint, closePlan, showPlace, findPlaces, computeRoute, startNav, getRouter, prefs, onFix,
    get nav() { return nav; }, get stepping() { return stepping; }, packMaxZoom, detailAt };   // for tests
})();
