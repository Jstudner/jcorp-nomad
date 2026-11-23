/* nomad-coords.js - positions as people write them down: decimal degrees, degrees minutes
   seconds, UTM, and MGRS (the US National Grid is the same grid, used by search and
   rescue, FEMA and the military). WGS84 throughout.

     NomadCoords.format(lat, lon, 'dd' | 'dms' | 'utm' | 'mgrs')
     NomadCoords.parse(text)  -> {lat, lon} or null   (any of the four)
*/
(function (root) {
  'use strict';

  const A = 6378137, F = 1 / 298.257223563, K0 = 0.9996;
  const E2 = F * (2 - F), EP2 = E2 / (1 - E2);
  const R = Math.PI / 180;
  const BANDS = 'CDEFGHJKLMNPQRSTUVWX';                 // 8° bands from 80°S, X is 12°

  function zoneOf(lat, lon) {
    let z = Math.floor((lon + 180) / 6) + 1;
    if (lat >= 56 && lat < 64 && lon >= 3 && lon < 12) z = 32;            // Norway
    if (lat >= 72 && lat < 84) {                                          // Svalbard
      if (lon >= 0 && lon < 9) z = 31; else if (lon >= 9 && lon < 21) z = 33;
      else if (lon >= 21 && lon < 33) z = 35; else if (lon >= 33 && lon < 42) z = 37;
    }
    return Math.min(60, Math.max(1, z));
  }
  const bandOf = (lat) => BANDS[Math.min(19, Math.max(0, Math.floor((lat + 80) / 8)))];

  // meridian arc length from the equator to latitude phi
  function arc(phi) {
    const e4 = E2 * E2, e6 = e4 * E2;
    return A * ((1 - E2 / 4 - 3 * e4 / 64 - 5 * e6 / 256) * phi - (3 * E2 / 8 + 3 * e4 / 32 + 45 * e6 / 1024) * Math.sin(2 * phi)
      + (15 * e4 / 256 + 45 * e6 / 1024) * Math.sin(4 * phi) - (35 * e6 / 3072) * Math.sin(6 * phi));
  }

  function toUTM(lat, lon, forceZone) {
    const zone = forceZone || zoneOf(lat, lon);
    const phi = lat * R, lam = lon * R, lam0 = ((zone - 1) * 6 - 180 + 3) * R;
    const N = A / Math.sqrt(1 - E2 * Math.sin(phi) ** 2), T = Math.tan(phi) ** 2, C = EP2 * Math.cos(phi) ** 2;
    const a = Math.cos(phi) * (lam - lam0), M = arc(phi);
    const e = K0 * N * (a + (1 - T + C) * a ** 3 / 6 + (5 - 18 * T + T * T + 72 * C - 58 * EP2) * a ** 5 / 120) + 500000;
    let n = K0 * (M + N * Math.tan(phi) * (a * a / 2 + (5 - T + 9 * C + 4 * C * C) * a ** 4 / 24
      + (61 - 58 * T + T * T + 600 * C - 330 * EP2) * a ** 6 / 720));
    if (lat < 0) n += 10000000;
    return { zone, band: bandOf(lat), e, n, south: lat < 0 };
  }

  function fromUTM(zone, south, e, n) {
    const x = e - 500000, y = south ? n - 10000000 : n;
    const M = y / K0, mu = M / (A * (1 - E2 / 4 - 3 * E2 * E2 / 64 - 5 * E2 ** 3 / 256));
    const e1 = (1 - Math.sqrt(1 - E2)) / (1 + Math.sqrt(1 - E2));
    const phi1 = mu + (3 * e1 / 2 - 27 * e1 ** 3 / 32) * Math.sin(2 * mu) + (21 * e1 * e1 / 16 - 55 * e1 ** 4 / 32) * Math.sin(4 * mu)
      + (151 * e1 ** 3 / 96) * Math.sin(6 * mu) + (1097 * e1 ** 4 / 512) * Math.sin(8 * mu);
    const N1 = A / Math.sqrt(1 - E2 * Math.sin(phi1) ** 2), T1 = Math.tan(phi1) ** 2, C1 = EP2 * Math.cos(phi1) ** 2;
    const R1 = A * (1 - E2) / (1 - E2 * Math.sin(phi1) ** 2) ** 1.5, D = x / (N1 * K0);
    const lat = phi1 - (N1 * Math.tan(phi1) / R1) * (D * D / 2 - (5 + 3 * T1 + 10 * C1 - 4 * C1 * C1 - 9 * EP2) * D ** 4 / 24
      + (61 + 90 * T1 + 298 * C1 + 45 * T1 * T1 - 252 * EP2 - 3 * C1 * C1) * D ** 6 / 720);
    const lon = (D - (1 + 2 * T1 + C1) * D ** 3 / 6 + (5 - 2 * C1 + 28 * T1 - 3 * C1 * C1 + 8 * EP2 + 24 * T1 * T1) * D ** 5 / 120) / Math.cos(phi1);
    return { lat: lat / R, lon: ((zone - 1) * 6 - 180 + 3) + lon / R };
  }

  // MGRS 100 km squares: column letters cycle in three sets by zone, rows in two
  const COLS = ['ABCDEFGH', 'JKLMNPQR', 'STUVWXYZ'], ROWS = 'ABCDEFGHJKLMNPQRSTUV';
  function toMGRS(lat, lon, digits = 5) {
    if (lat < -80 || lat >= 84) return null;                              // the poles use UPS
    const u = toUTM(lat, lon);
    const col = COLS[(u.zone - 1) % 3][Math.floor(u.e / 100000) - 1];
    const row = ROWS[(Math.floor(u.n / 100000) + (u.zone % 2 === 0 ? 5 : 0)) % 20];
    const d = 10 ** (5 - digits), pad = (v) => String(Math.floor((v % 100000) / d)).padStart(digits, '0');
    return `${u.zone}${u.band} ${col}${row} ${pad(u.e)} ${pad(u.n)}`;
  }
  function fromMGRS(zone, band, col, row, east, north) {
    const ci = COLS[(zone - 1) % 3].indexOf(col), ri = ROWS.indexOf(row);
    if (ci < 0 || ri < 0) return null;
    const e = (ci + 1) * 100000 + east;
    let n = ((ri - (zone % 2 === 0 ? 5 : 0) + 20) % 20) * 100000 + north;
    // the row letters repeat every 2,000 km: take the cycle that lands in the band
    const bi = BANDS.indexOf(band), south = bi < 10;
    const bandLat = -80 + bi * 8;
    const minN = toUTM(bandLat, (zone - 1) * 6 - 180 + 3, zone).n;
    while (n < minN - 100000) n += 2000000;
    return fromUTM(zone, south, e, n);
  }

  function dms(v, pos, neg) {
    const h = v < 0 ? neg : pos, a = Math.abs(v), d = Math.floor(a), mf = (a - d) * 60, m = Math.floor(mf), s = (mf - m) * 60;
    return `${d}°${String(m).padStart(2, '0')}'${s.toFixed(1).padStart(4, '0')}"${h}`;
  }

  function format(lat, lon, fmt) {
    if (fmt === 'dms') return dms(lat, 'N', 'S') + ' ' + dms(lon, 'E', 'W');
    if (fmt === 'utm') {
      const u = toUTM(lat, lon);
      return `${u.zone}${u.band} ${Math.round(u.e)}E ${Math.round(u.n)}N`;
    }
    if (fmt === 'mgrs') return toMGRS(lat, lon) || lat.toFixed(5) + ', ' + lon.toFixed(5);
    return lat.toFixed(5) + ', ' + lon.toFixed(5);
  }

  function parse(text) {
    const t = String(text).trim().toUpperCase();
    let m = /^(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)$/.exec(t);                       // 47.6, -122.3
    if (m) { const a = +m[1], b = +m[2]; return Math.abs(a) <= 90 && Math.abs(b) <= 180 ? { lat: a, lon: b } : null; }
    m = /^(\d+)°\s*(\d+)['′]\s*([\d.]+)["″]?\s*([NS])[\s,]*(\d+)°\s*(\d+)['′]\s*([\d.]+)["″]?\s*([EW])$/.exec(t);
    if (m) {
      const lat = (+m[1] + m[2] / 60 + m[3] / 3600) * (m[4] === 'S' ? -1 : 1);
      const lon = (+m[5] + m[6] / 60 + m[7] / 3600) * (m[8] === 'W' ? -1 : 1);
      return { lat, lon };
    }
    m = /^(\d{1,2})([C-HJ-NP-X])\s*([A-HJ-NP-Z])([A-HJ-NP-V])\s*(\d+)\s*(\d*)$/.exec(t);    // 10T EM 50200 72700
    if (m) {
      let east = m[5], north = m[6];
      if (!north) { if (east.length % 2) return null; north = east.slice(east.length / 2); east = east.slice(0, east.length / 2); }
      if (east.length !== north.length || east.length > 5) return null;
      const scale = 10 ** (5 - east.length);
      return fromMGRS(+m[1], m[2], m[3], m[4], +east * scale, +north * scale);
    }
    m = /^(\d{1,2})([C-HJ-NP-X])\s+(\d{3,7}(?:\.\d+)?)\s*E?\s+(\d{1,8}(?:\.\d+)?)\s*N?$/.exec(t);  // 10T 550200E 5272700N
    if (m) return fromUTM(+m[1], m[2] < 'N', +m[3], +m[4]);
    return null;
  }

  const api = { format, parse, toUTM, fromUTM, toMGRS };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.NomadCoords = api;
})(typeof self !== 'undefined' ? self : this);
