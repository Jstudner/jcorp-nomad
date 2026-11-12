// <!-- Version 6 -->

// trust badge next to the SD usage %. statTrusted is only true once a real scan
// has confirmed the fast stat figure (see the firmware's reconcileStatTrust)
function renderSdTrustBadge(statTrusted) {
  const badge = document.getElementById('sd-trust-badge');
  if (!badge) return;
  badge.style.display = 'inline-block';
  if (statTrusted) {
    badge.textContent = 'Verified';
    badge.style.background = 'var(--success)';
    badge.style.color = '#fff';
  } else {
    badge.textContent = 'Estimate';
    badge.style.background = 'var(--warning)';
    badge.style.color = '#212529';
  }
}

function renderSdUsage(total, used, statTrusted) {
  document.getElementById('sd-total').textContent = `of ${(total/1e9).toFixed(2)} GB`;
  document.getElementById('sd-used').textContent = `${(used/1e9).toFixed(2)} GB used`;
  const percent = total > 0 ? Math.round((used / total) * 100) : 0;
  document.getElementById('sd-bar').style.width = `${percent}%`;
  document.getElementById('sd-percent').textContent = `${percent}%`;
  const stripCard = document.getElementById('strip-card');
  if (stripCard) stripCard.textContent = `${percent}% full`;
  renderSdTrustBadge(statTrusted);
}

// Fetch SD card information (cached values - near-instant, safe to call often)
async function fetchSD() {
  try {
    const res = await fetch('/sdinfo');
    const { total, used, statTrusted } = await res.json();
    renderSdUsage(total, used, statTrusted);
  } catch (e) {
    console.error('Failed to fetch SD info:', e);
  }
}

function renderSdBreakdown(entries) {
  const table = document.getElementById('sd-breakdown-table');
  const body = document.getElementById('sd-breakdown-body');
  if (!table || !body) return;
  if (!entries || entries.length === 0) {
    table.style.display = 'none';
    return;
  }
  body.innerHTML = '';
  entries.forEach(e => {
    const tr = document.createElement('tr');
    for (const v of [e.dir, `${(e.bytes / 1e9).toFixed(2)} GB`, e.files]) {
      const td = document.createElement('td');
      td.textContent = v;
      tr.appendChild(td);
    }
    body.appendChild(tr);
  });
  table.style.display = '';
}

async function fetchSdBreakdown() {
  try {
    const res = await fetch('/api/sd-breakdown');
    const data = await res.json();
    renderSdBreakdown(data.breakdown);
    return data.scanning;
  } catch (e) {
    console.error('Failed to fetch SD breakdown:', e);
    return false;
  }
}

// full-card walk. the exact "how full is my card" answer - slow (10-20 min), also
// builds the folder-by-folder breakdown
async function triggerSDScan() {
  const msg = document.getElementById('sd-scan-msg');

  try {
    const res = await adminFetch('/api/sd-scan', { method: 'POST' });
    const data = await res.json().catch(() => ({}));

    if (res.status === 429) {
      if (msg) msg.textContent = data.message || 'A full scan ran recently. Try again later.';
      return;
    }
    if (res.status === 409) {
      // blocked because indexing or another scan is running, not an error
      if (msg) msg.textContent = data.error || 'Busy. Try again when the current scan finishes.';
      return;
    }
    if (!res.ok) {
      alert(data.error || "Couldn't start the scan.");
      return;
    }

    if (msg) msg.textContent = 'Card scan started.';

    // Poll until the scan finishes, then show results.
    const poll = async () => {
      const stillScanning = await fetchSdBreakdown();
      await fetchSD();
      if (stillScanning) {
        setTimeout(poll, 3000);
      } else if (msg) {
        msg.textContent = 'Scan complete.';
      }
    };
    setTimeout(poll, 2000);
  } catch (e) {
    console.error('SD scan error:', e);
    alert("Couldn't start the card scan.");
  }
}

// Check videos: reads every video's headers (never the video) and counts what will play
// badly. The file browser marks the same files with the same dot colours.

// [key, label, level]; level is the file browser's dot: bad won't play, warn some devices, slow
const VIDEO_CHECK_ROWS = [
  ['unreadable', "Can't read", 'bad'],
  ['oldVideo', "Won't play (old format)", 'bad'],
  ['highProfile', "Won't play (10-bit)", 'bad'],
  ['avi', "Won't play (AVI)", 'bad'],
  ['mkv', 'No iPhone (MKV)', 'warn'],
  ['dolby', 'No sound in Chrome', 'warn'],
  ['hevc', 'Not every browser (HEVC)', 'warn'],
  ['av1', 'Not older iPhones (AV1)', 'warn'],
  ['fragmented', "Won't seek in Firefox", 'warn'],
  ['indexEnd', 'Slow to start (index at end)', 'slow'],
  ['bigIndex', 'Slow to start (large index)', 'slow'],
  ['heavy', 'Heavy to stream', 'slow'],
  ['uhd', '4K', 'slow'],
];

function renderVideoCheck(d) {
  const msg = document.getElementById('video-check-msg');
  if (!msg) return;
  if (d.state === 'running') {
    msg.textContent = d.total ? `Scanning ${d.done.toLocaleString()} of ${d.total.toLocaleString()}` : 'Scanning';
    return;
  }
  if (d.state !== 'done' || !d.videos) { msg.textContent = ''; return; }
  const rows = VIDEO_CHECK_ROWS.filter(([k]) => d.counts && d.counts[k]);
  const issues = rows.reduce((n, [k]) => n + d.counts[k], 0);
  msg.innerHTML = '';
  const head = document.createElement('div');
  head.className = 'video-check-head';
  head.textContent = `${d.videos.toLocaleString()} videos, ` +
    (issues ? `${issues.toLocaleString()} ${issues === 1 ? 'issue' : 'issues'}.` : 'no issues.');
  msg.appendChild(head);
  if (!rows.length) return;
  const list = document.createElement('ul');
  list.className = 'video-check-list';
  for (const [k, label, level] of rows) {
    const li = document.createElement('li');
    const dot = document.createElement('span');
    dot.className = 'video-check-dot ' + level;
    const name = document.createElement('span');
    name.textContent = label;
    const n = document.createElement('span');
    n.className = 'video-check-n';
    n.textContent = d.counts[k].toLocaleString();
    li.append(dot, name, n);
    list.appendChild(li);
  }
  msg.appendChild(list);
}

// follow a running scan, then show what it found
async function followVideoCheck() {
  const btn = document.getElementById('video-check-btn');
  let d = {};
  try { d = await (await fetch('/api/video-check', { cache: 'no-store' })).json(); } catch (e) {}
  renderVideoCheck(d);
  if (btn) btn.disabled = d.state === 'running';
  if (d.state === 'running') setTimeout(followVideoCheck, 2000);
}

async function triggerVideoCheck() {
  const ok = confirm(
    "Check every video for files that won't play in some browsers " +
    'or are slow to start?\n\n' +
    "Nothing on the card is changed. Videos with issues get a dot in the file browser, " +
    'and Nomad-Tools can fix most of them.\n\n' +
    'On a big card this takes about a minute.'
  );
  if (!ok) return;
  const msg = document.getElementById('video-check-msg');
  const res = await adminFetch('/api/video-check', { method: 'POST' });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) { if (msg) msg.textContent = data.error || "Couldn't start the check."; return; }
  followVideoCheck();
}

followVideoCheck();

// Library index scan scope (indexing, not disk usage)

function onIndexScanScopeChange() {
  const scope = document.getElementById('index-scan-scope').value;
  const dirWrap = document.getElementById('index-scan-dir-wrap');
  if (!dirWrap) return;
  dirWrap.style.display = (scope === 'folder') ? '' : 'none';
  if (scope === 'folder') {
    populateIndexScanDirDropdown();
  }
}

// fills the "specific folder" dropdown from the real top-level listing, so "scan just
// Movies" works for any folder, not just the 6 known buckets
let indexScanDirDropdownLoaded = false;
async function populateIndexScanDirDropdown() {
  if (indexScanDirDropdownLoaded) return;
  const select = document.getElementById('index-scan-dir');
  if (!select) return;
  try {
    const res = await fetch('/listfiles?dir=/');
    const entries = await res.json();
    select.innerHTML = '';
    entries
      .filter(e => e.isDir)
      .map(e => e.name.replace(/\/$/, ''))
      .filter(name => !name.startsWith('.'))
      .sort((a, b) => a.localeCompare(b))
      .forEach(name => {
        const opt = document.createElement('option');
        opt.value = '/' + name;
        opt.textContent = name;
        select.appendChild(opt);
      });
    indexScanDirDropdownLoaded = true;
  } catch (e) {
    console.error('Failed to load folder list:', e);
  }
}

// polls /scan-status. /generate-media returns as soon as the index is queued and the
// worker only sets indexingInProgress a moment later, so wait for it to start, then finish
function fetchScanStatus() {
  return fetch('/scan-status').then(r => r.ok ? r.json() : null).catch(() => null);
}

async function waitForIndexingToFinish() {
  // phase 1: wait for the queued index to actually start
  const startDeadline = Date.now() + 30000;
  for (;;) {
    await new Promise(r => setTimeout(r, 1000));
    const data = await fetchScanStatus();
    if (!data) { if (Date.now() > startDeadline) return; continue; }
    if (data.indexingInProgress || data.indexingTasksActive) break; // running
    // not running and not queued: done or never started. wait out the deadline in case the worker is slow
    if (!data.requestIndexing && Date.now() > startDeadline) return;
  }
  // phase 2: wait for the run (and any queued follow-up) to drain

  for (;;) {
    await new Promise(r => setTimeout(r, 2000));
    const data = await fetchScanStatus();
    if (!data) return;
    if (!(data.indexingInProgress || data.indexingTasksActive || data.requestIndexing)) return;
  }
}

async function triggerIndexScan() {
  const scope = document.getElementById('index-scan-scope').value;
  const withSd = document.getElementById('index-scan-with-sd').checked;
  const msg = document.getElementById('generate-msg');

  try {
    if (scope === 'folder') {
      const dir = document.getElementById('index-scan-dir').value;
      if (!dir) {
        alert('Choose a folder first');
        return;
      }
      if (msg) msg.textContent = `Starting scan of ${dir}...`;
      const res = await adminFetch(`/api/reindex?path=${encodeURIComponent(dir)}`, { method: 'POST' });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) {
        alert(data.error || "Couldn't start the scan.");
        return;
      }
      if (msg) msg.textContent = `Scan of ${dir} started. Progress is under Logs.`;
    } else {
      await generateMediaJson();
    }

    if (withSd) {
      // full SD scan, only AFTER indexing finishes. never run both at once, they fight
      // over the SD and the heap
      await waitForIndexingToFinish();
      if (msg) msg.textContent = 'Index done. Starting the card scan…';
      await triggerSDScan();
    } else {
      setTimeout(() => { if (msg) msg.textContent = ''; }, 5000);
    }
  } catch (e) {
    console.error('Failed to start index scan:', e);
    if (msg) msg.textContent = "Couldn't start the scan.";
  }
}
// Theme management
const THEME_KEY = 'nomad_dark_mode';

function applyThemeFlag(dark) {
  try {
    dark = !!dark;
    document.body.classList.toggle('dark', dark);
    
    const root = document.documentElement.style;
    if (dark) {
      root.setProperty('--bg', '#121212');
      root.setProperty('--text', '#e9ecef');
      root.setProperty('--muted', '#adb5bd');
      root.setProperty('--card-bg', '#1e1e1e');
      root.setProperty('--card-border', '#343a40');
      root.setProperty('--header-text', '#ffffff');
    } else {
      root.setProperty('--bg', '#f8f9fa');
      root.setProperty('--text', '#212529');
      root.setProperty('--muted', '#6c757d');
      root.setProperty('--card-bg', '#ffffff');
      root.setProperty('--card-border', '#dee2e6');
      root.setProperty('--header-text', '#ffffff');
    }
  } catch (e) {
    console.warn('applyThemeFlag failed', e);
  }
}

function initTheme() {
  applyThemeFlag(window.nomadIsDark ? nomadIsDark() : false);
}

// Initialize theme
if (document.readyState === 'loading') {
  window.addEventListener('DOMContentLoaded', initTheme);
} else {
  initTheme();
}

window.addEventListener('nomad-dark', (ev) => applyThemeFlag(ev.detail));
// Sync theme across tabs
window.addEventListener('storage', (ev) => {
  if (ev.key === THEME_KEY) {
    const dark = ev.newValue === 'true';
    applyThemeFlag(dark);
  }
});

/* SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 for the admin sign-in, done by hand because
   crypto.subtle is https-only and Nomad serves plain http. */
const NomadCrypto = (() => {
  const K = new Int32Array([
    0x428a2f98, 0x71374491, -0x4a3f0431, -0x164a245b, 0x3956c25b, 0x59f111f1, -0x6dc07d5c, -0x54e3a12b,
    -0x27f85568, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, -0x7f214e02, -0x6423f959, -0x3e640e8c,
    -0x1b64963f, -0x1041b87a, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    -0x67c1aeae, -0x57ce3993, -0x4ffcd838, -0x40a68039, -0x391ff40d, -0x2a586eb9, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, -0x7e3d36d2, -0x6d8dd37b,
    -0x5d40175f, -0x57e599b5, -0x3db47490, -0x3893ae5d, -0x2e6d17e7, -0x2966f9dc, -0xbf1ca7b, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, -0x7b3787ec, -0x7338fdf8, -0x6f410006, -0x5baf9315, -0x41065c09, -0x398e870e]);
  const W = new Int32Array(64);
  function block(H, b, o) {
    for (let i = 0; i < 16; i++) W[i] = (b[o + 4 * i] << 24) | (b[o + 4 * i + 1] << 16) | (b[o + 4 * i + 2] << 8) | b[o + 4 * i + 3];
    for (let i = 16; i < 64; i++) {
      const x = W[i - 15], y = W[i - 2];
      W[i] = (((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3)) + W[i - 16] +
             (((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10)) + W[i - 7] | 0;
    }
    let a = H[0], b1 = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
    for (let i = 0; i < 64; i++) {
      const t1 = h + (((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7))) + ((e & f) ^ (~e & g)) + K[i] + W[i] | 0;
      const t2 = (((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10))) + ((a & b1) ^ (a & c) ^ (b1 & c)) | 0;
      h = g; g = f; f = e; e = d + t1 | 0; d = c; c = b1; b1 = a; a = t1 + t2 | 0;
    }
    H[0] = H[0] + a | 0; H[1] = H[1] + b1 | 0; H[2] = H[2] + c | 0; H[3] = H[3] + d | 0;
    H[4] = H[4] + e | 0; H[5] = H[5] + f | 0; H[6] = H[6] + g | 0; H[7] = H[7] + h | 0;
  }
  const IV = [0x6a09e667, -0x4498517b, 0x3c6ef372, -0x5ab00ac6, 0x510e527f, -0x64fa9774, 0x1f83d9ab, 0x5be0cd19];
  function sha256(msg) {
    const len = msg.length, total = ((len + 9 + 63) >> 6) << 6, b = new Uint8Array(total);
    b.set(msg); b[len] = 0x80;
    const bits = len * 8;
    b[total - 4] = bits >>> 24; b[total - 3] = bits >>> 16; b[total - 2] = bits >>> 8; b[total - 1] = bits;
    b[total - 5] = Math.floor(bits / 0x100000000);
    const H = new Int32Array(IV);
    for (let o = 0; o < total; o += 64) block(H, b, o);
    const out = new Uint8Array(32);
    for (let i = 0; i < 8; i++) { out[4 * i] = H[i] >>> 24; out[4 * i + 1] = H[i] >>> 16; out[4 * i + 2] = H[i] >>> 8; out[4 * i + 3] = H[i]; }
    return out;
  }
  function hmac(key, msg) {
    if (key.length > 64) key = sha256(key);
    const ip = new Uint8Array(64 + msg.length), op = new Uint8Array(96);
    for (let i = 0; i < 64; i++) { const k = key[i] || 0; ip[i] = k ^ 0x36; op[i] = k ^ 0x5c; }
    ip.set(msg, 64);
    op.set(sha256(ip), 64);
    return sha256(op);
  }
  // one 32-byte block (all the key needs); the HMAC's padded-key states are worked out once
  function pbkdf2(password, salt, iter) {
    let key = password;
    if (key.length > 64) key = sha256(key);
    const ipad = new Uint8Array(64), opad = new Uint8Array(64);
    for (let i = 0; i < 64; i++) { const k = key[i] || 0; ipad[i] = k ^ 0x36; opad[i] = k ^ 0x5c; }
    const Hi = new Int32Array(IV), Ho = new Int32Array(IV);
    block(Hi, ipad, 0); block(Ho, opad, 0);
    // a 32-byte message after the 64-byte pad: one more block, padded for 96 bytes
    const tail = new Uint8Array(64);
    tail[32] = 0x80; tail[62] = 0x03; tail[63] = 0x00;   // 96 * 8 = 768 bits = 0x300
    const H = new Int32Array(8), word = (h) => { const o = new Uint8Array(32); for (let i = 0; i < 8; i++) { o[4 * i] = h[i] >>> 24; o[4 * i + 1] = h[i] >>> 16; o[4 * i + 2] = h[i] >>> 8; o[4 * i + 3] = h[i]; } return o; };
    const step = (msg32) => {
      tail.set(msg32, 0);
      H.set(Hi); block(H, tail, 0); const inner = word(H);
      tail.set(inner, 0);
      H.set(Ho); block(H, tail, 0); return word(H);
    };
    // U1 = HMAC(P, salt || 00000001): the salt is not 32 bytes, so the general HMAC
    const s = new Uint8Array(salt.length + 4); s.set(salt); s[salt.length + 3] = 1;
    let u = hmac(password, s);
    const out = u.slice();
    for (let n = 1; n < iter; n++) { u = step(u); for (let i = 0; i < 32; i++) out[i] ^= u[i]; }
    return out;
  }
  const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  const unhex = (s) => new Uint8Array((s.match(/../g) || []).map((x) => parseInt(x, 16)));
  const utf8 = (s) => new TextEncoder().encode(s);
  return { sha256, hmac, pbkdf2, hex, unhex, utf8 };
})();

// session token from POST /auth/login, sent as X-Admin-Token on every state-changing
// request so the device enforces the password, not just this UI.
// localStorage when "Keep me signed in" is ticked (Nomad keeps it across restarts too),
// else sessionStorage.
let adminToken = (() => { try { return sessionStorage.getItem('nomad_admin_token') || localStorage.getItem('nomad_admin_token') || ''; } catch (e) { return ''; } })();

function setAdminToken(token, remember) {
  adminToken = token || '';
  try {
    sessionStorage.removeItem('nomad_admin_token');
    localStorage.removeItem('nomad_admin_token');
    if (adminToken) (remember ? localStorage : sessionStorage).setItem('nomad_admin_token', adminToken);
  } catch (e) {}
}
const tokenKept = () => { try { return !!localStorage.getItem('nomad_admin_token'); } catch (e) { return false; } };

// Sign in without sending the password: Nomad hands out a one-time challenge and gets back
// HMAC(key, challenge), key = PBKDF2 of the password with the card's salt. A card still on
// the old scheme takes the password once as typed and upgrades it.
async function signIn(password, remember) {
  const ch = await (await fetch('/auth/challenge', { cache: 'no-store' })).json();
  let body;
  if (ch.mode === 'v2') {
    const key = NomadCrypto.pbkdf2(NomadCrypto.utf8(password), NomadCrypto.unhex(ch.salt), ch.iter);
    body = { nonce: ch.nonce, proof: NomadCrypto.hex(NomadCrypto.hmac(key, NomadCrypto.unhex(ch.nonce))), remember };
  } else {
    body = { password, remember };
  }
  const res = await fetch('/auth/login', { method: 'POST', cache: 'no-store',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ body: JSON.stringify(body) }) });
  let j = {};
  try { j = await res.json(); } catch (e) {}
  return { status: res.status, token: j.token, wait: j.wait || +(res.headers.get('Retry-After') || 0) };
}

// use for any request that changes device state (restart, settings, LEDs). attaches
// the admin session token and re-shows the auth overlay if the server rejects it
async function adminFetch(url, opts = {}) {
  const headers = Object.assign({}, opts.headers || {});
  if (adminToken) headers['X-Admin-Token'] = adminToken;
  const res = await fetch(url, Object.assign({}, opts, { headers }));
  if (res.status === 401) {
    setAdminToken('');
    const overlay = document.getElementById('auth-overlay');
    if (overlay) overlay.classList.remove('hidden');
  }
  return res;
}

// Settings management
async function loadSettings() {
  console.log("loadSettings() fired");
  try {
    // authed: the firmware only reveals the wifi password to the admin
    const res = await adminFetch('/settings', { cache: 'no-store' });
    const s = await res.json();
    console.log("settings from backend:", s);

    // Update UI elements
    if (s.hasOwnProperty('adminPassword')) {
      const serverPw = s.adminPassword;
      if (serverPw === null || serverPw === '' || serverPw === 'null') {
        console.log('Admin password is disabled on server');
      } else {
        console.log('Admin password is set on server');
      }
    }

    // RGB settings
    if (s.rgbMode !== undefined) {
      ledMode = s.rgbMode;
      updateModeUI(ledMode);
    }
    if (s.rgbColor !== undefined) {
      document.getElementById('led-color').value = s.rgbColor;
    }

    // WiFi settings
    if (s.wifiSSID !== undefined) {
      document.getElementById('ssid').value = s.wifiSSID;
    }
    if (s.wifiPassword !== undefined && s.wifiPassword !== '__set__') {
      document.getElementById('wifi-password').value = s.wifiPassword;
    }

    // brightness 0-100, clamp it (old settings files can still hold 230)
    if (s.brightness !== undefined) {
      const b = Math.max(1, Math.min(100, parseInt(s.brightness, 10) || 100));
      document.getElementById('brightness').value = b;
      updateBrightnessLabel(b);
    }

    // Auto-generate
    if (s.autoGenerateMedia !== undefined) {
      isAutoGenerate = s.autoGenerateMedia;
      document.getElementById('auto-generate').checked = isAutoGenerate;
    }

    // Screen flip (180° rotation)
    if (s.flipScreen !== undefined) {
      document.getElementById('flip-screen').checked = !!s.flipScreen;
    }

    // TV support (DLNA), default on when the firmware doesn't report it
    const dlnaEl = document.getElementById('dlna-enabled');
    if (dlnaEl) dlnaEl.checked = (s.dlnaEnabled !== undefined) ? !!s.dlnaEnabled : true;

    // captive DNS mode (WiFi Mode popup checkbox), default targeted
    const dnsEl = document.getElementById('wifi-dns-catchall');
    if (dnsEl) dnsEl.checked = !!s.dnsCatchAll;

    // Check authentication
    if (typeof requireAdminAuth === 'function') {
      await requireAdminAuth(s);
    }

  } catch (e) {
    console.error('Failed to load settings:', e);
  }
}

async function saveSettings() {
  try {
    // hotspot name and password are only sent from updateWiFiSettings. sending them here
    // blanks the password if the page loaded before signing in
    const settings = {
      rgbMode: ledMode,
      rgbColor: document.getElementById('led-color').value,
      brightness: parseInt(document.getElementById('brightness').value),
      autoGenerateMedia: document.getElementById('auto-generate').checked,
      flipScreen: document.getElementById('flip-screen').checked,
      dlnaEnabled: document.getElementById('dlna-enabled') ? document.getElementById('dlna-enabled').checked : true
    };

    const res = await adminFetch('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({
        'body': JSON.stringify(settings)
      })
    });

    if (res.ok) {
      console.log('Settings saved successfully');
    } else {
      console.error('Failed to save settings');
    }
  } catch (e) {
    console.error('Error saving settings:', e);
  }
}

// RGB LED controls
let ledMode = 'off';

function updateModeUI(mode) {
  const buttons = {
    off: document.getElementById('mode-off'),
    solid: document.getElementById('mode-solid'),
    rainbow: document.getElementById('mode-rainbow')
  };

  // Reset all buttons
  Object.values(buttons).forEach(btn => {
    if (btn) {
      btn.classList.remove('btn-primary', 'btn-success', 'btn-secondary');
      btn.classList.add('btn-secondary');
    }
  });

  // the chosen one lit
  if (buttons[mode]) {
    buttons[mode].classList.remove('btn-secondary');
    buttons[mode].classList.add('btn-primary');
  }

  ledMode = mode;
}

async function sendModeToServer(mode) {
  try {
    if (mode === 'off') {
      await adminFetch('/led/onoff', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: false })
      });
    } else if (mode === 'solid') {
      await adminFetch('/led/onoff', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled: true })
      });
    } else if (mode === 'rainbow') {
      await adminFetch('/led/rainbow', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' }
      });
    }
  } catch (e) {
    console.error('Failed to send mode to server:', e);
  }
}

async function sendColorToServer(color) {
  try {
    await adminFetch('/led/color', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ color: color })
    });
  } catch (e) {
    console.error('Failed to send color to server:', e);
  }
}

// Admin password functions
// The key is derived here from the password and a fresh salt; only the key goes to Nomad
// (POST /auth/password). This device stays signed in, every other one signs in again.
async function postPassword(body) {
  return adminFetch('/auth/password', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ body: JSON.stringify(body) }) });
}
async function updateAdminPassword() {
  const newPw = document.getElementById('new-password').value;
  const confirmPw = document.getElementById('confirm-password').value;
  const msg = document.getElementById('password-msg');
  const say = (t) => { if (msg) msg.textContent = t; else alert(t); };
  if (newPw.length < 4) { say('Use at least 4 characters.'); return; }
  if (newPw !== confirmPw) { say("The two passwords don't match."); return; }
  try {
    const salt = crypto.getRandomValues(new Uint8Array(16)), iter = 10000;
    const key = NomadCrypto.pbkdf2(NomadCrypto.utf8(newPw), salt, iter);
    const keep = tokenKept() || !adminToken;
    let res = await postPassword({ salt: NomadCrypto.hex(salt), iter, key: NomadCrypto.hex(key), remember: keep });
    if (res.status === 404) {
      // older firmware without /auth/password: send it to /settings, it hashes it there
      res = await adminFetch('/settings', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ body: JSON.stringify({ adminPassword: newPw }) }) });
      if (res.ok) { setAdminToken(''); say('Password set. Sign in with it.'); location.reload(); return; }
    }
    if (!res.ok) { say(`Nomad refused (HTTP ${res.status}).`); return; }
    const j = await res.json().catch(() => ({}));
    if (j.token) setAdminToken(j.token, keep);
    document.getElementById('new-password').value = '';
    document.getElementById('confirm-password').value = '';
    say('Password set. Other devices will need to sign in again.');
    loadSettings();
    refreshSessionInfo();
  } catch (e) {
    console.error('Error updating admin password:', e);
    say("Couldn't reach Nomad.");
  }
}

// who is signed in, for the password card
async function refreshSessionInfo() {
  const el = document.getElementById('session-info');
  const outBtns = document.getElementById('signout-buttons');
  if (!el) return;
  try {
    const r = await fetch('/auth/sessions', { cache: 'no-store', headers: adminToken ? { 'X-Admin-Token': adminToken } : {} });
    if (!r.ok) { el.textContent = ''; if (outBtns) outBtns.hidden = true; return; }
    const j = await r.json();
    const chip = document.getElementById('account-btn');
    if (j.mode === 'none') {
      el.textContent = 'No password: anyone on Nomad\'s WiFi can change these settings.';
      if (outBtns) outBtns.hidden = true;
      if (chip) chip.hidden = true;
      return;
    }
    if (chip) { chip.hidden = false; chip.textContent = tokenKept() ? 'Signed in (kept)' : 'Signed in'; }
    el.textContent = `Password on. Signed in on ${j.signedIn} device${j.signedIn === 1 ? '' : 's'}` +
      (j.kept ? ` (${j.kept} kept signed in)` : '') + `. This device: ${tokenKept() ? 'kept signed in' : 'until this tab closes'}.`;
    if (outBtns) outBtns.hidden = false;
  } catch (e) { el.textContent = ''; }
}
async function signOut(all) {
  if (all && !confirm('Sign out every device, this one included?')) return;
  try {
    await fetch('/auth/logout', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-Admin-Token': adminToken },
      body: new URLSearchParams({ body: JSON.stringify({ all: !!all }) }) });
  } catch (e) {}
  setAdminToken('');
  location.reload();
}

// WiFi settings
// Hotspot name and password, saved on their own. WPA2 needs 8-63 characters; empty means
// an open hotspot, which needs a confirm.
async function updateWiFiSettings() {
  const ssid = document.getElementById('ssid').value.trim();
  const wifiPassword = document.getElementById('wifi-password').value;
  const msg = document.getElementById('wifi-msg');
  const say = (t) => { if (msg) msg.textContent = t; else alert(t); };
  if (!ssid || ssid.length > 32) { say('The network name needs 1 to 32 characters.'); return; }
  if (wifiPassword && (wifiPassword.length < 8 || wifiPassword.length > 63)) { say('A WiFi password needs 8 to 63 characters.'); return; }
  const open = !wifiPassword;
  if (open && !confirm('Leave the WiFi password empty? Anyone nearby will be able to join Nomad\'s network.')) return;
  const body = { wifiSSID: ssid };
  if (open) { body.wifiPassword = ''; body.wifiOpen = true; } else body.wifiPassword = wifiPassword;
  try {
    const res = await adminFetch('/settings', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ body: JSON.stringify(body) }) });
    say(res.ok ? 'Saved. Nomad uses it from its next restart.' : `Nomad refused (HTTP ${res.status}).`);
  } catch (e) { say("Couldn't reach Nomad."); }
}

// Brightness control
function setBrightness(val) {
  const brightnessValue = parseInt(val);
  return adminFetch('/brightness', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      'body': JSON.stringify({ value: brightnessValue })
    })
  }).catch(e => console.error('Failed to set brightness:', e));
}

function updateBrightnessLabel(val) {
  const pct = Math.max(0, Math.min(100, parseInt(val, 10) || 0));
  document.getElementById('brightness-percent').textContent = `${pct}%`;
  // paint the used part blue via the element's own background so it repaints while dragging
  const slider = document.getElementById('brightness');
  if (slider) {
    slider.style.background =
      `linear-gradient(to right, var(--primary) 0 ${pct}%, var(--card-border) ${pct}% 100%)`;
  }
}

// Media generation
async function generateMediaJson() {
  try {
    const msg = document.getElementById('generate-msg');
    msg.textContent = 'Starting…';
    const res = await adminFetch('/generate-media', { method: 'POST' });
    // show what Nomad actually answered, busy and refused aren't "started"
    msg.textContent = res.ok ? 'Indexing started.'
      : res.status === 409 ? 'An index or card scan is already running.'
      : res.status === 401 ? 'Sign in first.'
      : `Nomad didn't start it (HTTP ${res.status}).`;
    if (res.ok) setTimeout(() => { msg.textContent = ''; }, 8000);
  } catch (e) {
    console.error('Failed to generate media:', e);
    document.getElementById('generate-msg').textContent = "Couldn't start the scan.";
  }
}

// Auto-generate toggle
let isAutoGenerate = false;

// Console functionality
/* The log: Nomad's entries (/console-logs, the last 50) merged with this page's notes.
   Lines are set as text, never HTML: messages carry guest-chosen file names. Nomad stamps
   entries with its uptime, so they show as "5 min ago". */
let serverLog = [], serverNow = 0, serverAt = 0;   // entries, the Nomad's uptime when fetched, and our clock then
let consoleLines = [];                              // this page's notes: { msg, type, at }
const maxConsoleLines = 30;

function addConsoleLog(message, type = 'info') {
  consoleLines.push({ msg: String(message), type, at: Date.now() });
  if (consoleLines.length > maxConsoleLines) consoleLines = consoleLines.slice(-maxConsoleLines);
  renderLog();
}
function updateConsoleDisplay() { renderLog(); }   // kept: older code calls it

function agoText(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 45) return 'just now';
  if (s < 3600) return Math.round(s / 60) + ' min ago';
  if (s < 86400) return Math.round(s / 3600) + ' h ago';
  return Math.round(s / 86400) + ' d ago';
}
function logLines() {
  const nowServer = serverNow + (Date.now() - serverAt);
  const lines = serverLog.map(l => {
    const msg = String(l.message || '');
    const media = /^\[?[\d:APM\s]*\]?\s*Playing:|\[Media\]$/.test(msg);
    return { msg, type: media ? 'media' : (l.type || 'info'), when: serverNow ? agoText(nowServer - (l.timestamp || 0)) : '' };
  });
  return lines.concat(consoleLines.map(l => ({ msg: l.msg, type: l.type, when: agoText(Date.now() - l.at) })));
}
function renderLog(force) {
  const box = document.getElementById('console-output');
  if (!box || (!force && !box.offsetParent)) return;           // not on screen: nothing to draw
  const filter = (document.getElementById('log-filter') || {}).value || 'all';
  const follow = !document.getElementById('log-follow') || document.getElementById('log-follow').checked;
  const keep = filter === 'problems' ? (l) => l.type === 'error' || l.type === 'warning'
             : filter === 'media' ? (l) => l.type === 'media' : () => true;
  const frag = document.createDocumentFragment();
  const shown = logLines().filter(keep);
  for (const l of shown) {
    const row = document.createElement('div');
    row.className = 'ln ' + l.type;
    if (l.when && !/^\[/.test(l.msg)) { const t = document.createElement('span'); t.className = 't'; t.textContent = l.when; row.appendChild(t); }
    row.appendChild(document.createTextNode(l.msg));
    frag.appendChild(row);
  }
  if (!shown.length) { const row = document.createElement('div'); row.className = 'ln'; row.textContent = 'Nothing to show.'; frag.appendChild(row); }
  box.replaceChildren(frag);
  if (follow) box.scrollTop = box.scrollHeight;
}
function copyLog() {
  const text = logLines().map(l => (l.when ? l.when + '  ' : '') + l.msg).join('\n');
  // navigator.clipboard is https-only, so use a hidden textarea and execCommand('copy')
  const ta = document.createElement('textarea');
  ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
  document.body.appendChild(ta); ta.select();
  let ok = false;
  try { ok = document.execCommand('copy'); } catch (e) {}
  ta.remove();
  addConsoleLog(ok ? 'Log copied.' : 'Copying is blocked here: long-press the log to select it.', ok ? 'success' : 'warning');
}
// the icon-only buttons (data-icon) get their icon
document.addEventListener('DOMContentLoaded', () => {
  if (window.NomadIcons) document.querySelectorAll('[data-icon]').forEach((b) => { b.innerHTML = NomadIcons.svg(b.dataset.icon, 17); });
});

document.addEventListener('DOMContentLoaded', () => {
  const f = document.getElementById('log-filter');
  if (f) f.addEventListener('change', () => renderLog(true));
});

// true while a scan/index is running - the adaptive poll below speeds up when it is
let indexingActive = false;

async function refreshConsole() {
  try {
    // Fetch scan status
    const res = await fetch('/scan-status');
    if (res.ok) {
      const data = await res.json();

      // Update status display
      document.getElementById('scan-status-text').textContent = data.status || 'Idle';
      document.getElementById('scan-mode-text').textContent = data.mode || '—';
      document.getElementById('scan-queue-depth').textContent = data.queueDepth || '0';

      // Update scanning animation
      const statusElement = document.getElementById('scan-status-text');
      if (data.status && data.status.toLowerCase().includes('scanning')) {
        statusElement.classList.add('scanning');
      } else {
        statusElement.classList.remove('scanning');
      }

      // Library Index progress bar
      indexingActive = !!(data.indexingInProgress || data.indexingTasksActive);
      const stripIndex = document.getElementById('strip-index');
      if (stripIndex) stripIndex.textContent = indexingActive
        ? (data.totalBuckets > 0 ? `${data.progressPercent}%` : 'Working…') : (data.status || 'Idle');
      const wrap = document.getElementById('index-progress-wrap');
      const text = document.getElementById('index-progress-text');
      const bar = document.getElementById('index-progress-bar');
      if (wrap && text && bar) {
        if (indexingActive) {
          wrap.style.display = '';
          const label = data.currentPath ? data.currentPath.replace(/^\//, '') || 'root' : 'library';
          if (data.totalBuckets > 0) {
            text.textContent = `Indexing: ${label} (${data.currentBucketNum}/${data.totalBuckets}) ${data.progressPercent}%`;
            bar.style.width = `${data.progressPercent}%`;
          } else {
            text.textContent = `Updating index: ${label}`;
            bar.style.width = '100%';
          }
        } else {
          wrap.style.display = 'none';
        }
      }
    }

    // Fetch console logs from backend
    await fetchConsoleLogs();

  } catch (e) {
    const timestamp = new Date().toLocaleTimeString();
    addConsoleLog(`[${timestamp}] Failed to fetch system status: ${e.message}`, 'error');
  }
}

async function fetchConsoleLogs() {
  try {
    const res = await adminFetch('/console-logs');   // admin-only: it names files and networks
    if (!res.ok) return;
    const data = await res.json();
    serverLog = Array.isArray(data.logs) ? data.logs : [];
    serverNow = +data.now || 0;
    serverAt = Date.now();
    renderLog();
  } catch (e) {
    // the next poll tries again
  }
}


// Admin bar functionality
async function fetchAdminBar() {
  try {
    // authed: the firmware only reveals the wifi password to the admin
    const res = await adminFetch('/admin-status');
    const data = await res.json();

    document.getElementById('bar-ssid').textContent = data.ssid || '—';

    // in WiFi Mode (joined a home network) the hotspot password means nothing,
    // show the address the router gave it instead
    const passBadge = document.getElementById('bar-wifi-pass');
    const passLabel = passBadge ? passBadge.parentElement.querySelector('.label') : null;
    if (data.wifiMode === 'sta') {
      if (passLabel) passLabel.textContent = 'Address';
      passBadge.textContent = data.ip || '—';
    } else {
      if (passLabel) passLabel.textContent = 'Password';
      passBadge.textContent = (data.wifiPassword && data.wifiPassword !== '__set__') ? data.wifiPassword : '••••••';
    }

    document.getElementById('bar-users').textContent =
      typeof data.users === 'number' ? `${data.users}` : '0';

  } catch (e) {
    console.error('Could not load admin bar:', e);
  }
}

// WiFi Mode popup: joins Nomad to an existing network instead of running the hotspot.
// All state lives on the device (/api/wifi-mode); this popup is just a remote control.
let wifiScanTimer = null;
let wifiStoredPasswordSet = false;

function wifiSetStatus(text, cls) {
  const box = document.getElementById('wifi-status');
  const txt = document.getElementById('wifi-status-text');
  if (!box || !txt) return;
  box.classList.remove('online', 'warn');
  if (cls) box.classList.add(cls);
  txt.textContent = text;
}

async function openWifiModal() {
  const overlay = document.getElementById('wifi-overlay');
  overlay.classList.remove('hidden');
  wifiSetStatus('Loading…', '');
  try {
    const res = await adminFetch('/api/wifi-mode', { cache: 'no-store' });
    if (res.status === 404) {
      wifiSetStatus('This firmware has no WiFi Mode. Update the firmware first.', 'warn');
      document.getElementById('wifi-connect-btn').disabled = true;
      document.getElementById('wifi-disable-btn').disabled = true;
      return;
    }
    if (!res.ok) throw new Error('status ' + res.status);
    const s = await res.json();

    document.getElementById('wifi-connect-btn').disabled = false;
    document.getElementById('wifi-disable-btn').disabled = false;
    wifiStoredPasswordSet = !!s.staPasswordSet;

    const ssidEl = document.getElementById('wifi-sta-ssid');
    if (!ssidEl.value && s.staSSID) ssidEl.value = s.staSSID;
    document.getElementById('wifi-sta-persist').checked = !!s.staPersist;
    const passEl = document.getElementById('wifi-sta-pass');
    passEl.placeholder = wifiStoredPasswordSet ? '(saved, leave empty to keep it)' : 'Leave empty for open networks';

    if (s.mode === 'sta') {
      wifiSetStatus(`Connected to "${s.staSSID}", ${s.ip}/${s.cidr || '?'}${typeof s.rssi === 'number' ? ` (${s.rssi} dBm)` : ''}`, 'online');
    } else if (s.staArmed) {
      wifiSetStatus(`Hotspot mode, will try to join "${s.staSSID}" on the next restart`, 'warn');
    } else if (s.lastError) {
      wifiSetStatus(`Hotspot mode, the last attempt to join failed: ${s.lastError}`, 'warn');
    } else {
      wifiSetStatus(`Hotspot mode ("${s.hotspotSSID}"), ${s.ip}`, '');
    }
  } catch (e) {
    console.error('WiFi mode status failed:', e);
    wifiSetStatus("Couldn't read the WiFi status from Nomad.", 'warn');
  }
}

function closeWifiModal() {
  document.getElementById('wifi-overlay').classList.add('hidden');
  if (wifiScanTimer) { clearTimeout(wifiScanTimer); wifiScanTimer = null; }
}

async function wifiPollScan() {
  const listEl = document.getElementById('wifi-scan-list');
  const scanBtn = document.getElementById('wifi-scan-btn');
  try {
    const res = await adminFetch('/api/wifi-scan', { cache: 'no-store' });
    if (!res.ok) throw new Error('status ' + res.status);
    const data = await res.json();
    if (data.status === 'scanning') {
      wifiScanTimer = setTimeout(wifiPollScan, 1200);
      return;
    }
    scanBtn.disabled = false;
    scanBtn.textContent = 'Scan for networks';
    listEl.innerHTML = '';
    const nets = data.networks || [];
    if (!nets.length) {
      listEl.innerHTML = '<div class="wifi-net-row" style="cursor:default;">No networks found</div>';
    }
    nets.forEach(n => {
      const row = document.createElement('button');
      row.type = 'button';
      row.className = 'wifi-net-row';
      const name = document.createElement('span');
      name.textContent = n.ssid;
      const meta = document.createElement('span');
      meta.className = 'meta';
      meta.innerHTML = `${n.secure ? NomadIcons.svg('lock') + ' ' : ''}${Number(n.rssi)} dBm`;
      row.appendChild(name);
      row.appendChild(meta);
      row.addEventListener('click', () => {
        document.getElementById('wifi-sta-ssid').value = n.ssid;
        document.getElementById('wifi-sta-pass').focus();
      });
      listEl.appendChild(row);
    });
    listEl.classList.remove('hidden');
  } catch (e) {
    console.error('WiFi scan failed:', e);
    scanBtn.disabled = false;
    scanBtn.textContent = 'Scan for networks';
    addConsoleLog('WiFi scan failed', 'error');
  }
}

async function wifiStartScan() {
  const scanBtn = document.getElementById('wifi-scan-btn');
  scanBtn.disabled = true;
  scanBtn.textContent = 'Scanning… (playback may pause a moment)';
  wifiPollScan();
}

async function wifiConnect() {
  const ssid = document.getElementById('wifi-sta-ssid').value.trim();
  const pass = document.getElementById('wifi-sta-pass').value;
  const persist = document.getElementById('wifi-sta-persist').checked;

  if (!ssid) { alert('Enter or pick a network name first.'); return; }
  if (pass && (pass.length < 8 || pass.length > 63)) {
    alert('WiFi passwords are 8-63 characters. Leave the field empty for an open network' +
          (wifiStoredPasswordSet ? ' or to keep the saved password.' : '.'));
    return;
  }

  const ok = confirm(
    `Nomad restarts and joins "${ssid}".\n\n` +
    `Its hotspot is off in WiFi Mode. Connect this device to "${ssid}" ` +
    `and open http://nomad.local. Nomad's screen shows the exact address.\n\n` +
    (persist
      ? 'Auto-reconnect is on: Nomad rejoins on every boot. To get the hotspot back, hold the side button while it boots, or use this popup.\n\n'
      : 'Auto-reconnect is off: unplug and replug Nomad to get the hotspot back.\n\n') +
    'If the network can\'t be reached, Nomad goes back to hotspot mode.\n\nContinue?'
  );
  if (!ok) return;

  const body = { action: 'connect', ssid: ssid, persist: persist };
  // empty field + a stored password = keep it (the device never echoes it back)
  if (pass || !wifiStoredPasswordSet) body.password = pass;

  try {
    const res = await adminFetch('/api/wifi-mode', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ body: JSON.stringify(body) })
    });
    const data = await res.json().catch(() => ({}));
    if (res.ok) {
      addConsoleLog(`WiFi Mode: restarting to join "${ssid}"`, 'info');
      alert(`Nomad is restarting and joining "${ssid}".\n\nSwitch this device to that network, then open http://nomad.local or the address on Nomad's screen.`);
      closeWifiModal();
    } else {
      alert("Couldn't switch WiFi Mode: " + (data.error || ('HTTP ' + res.status)));
    }
  } catch (e) {
    addConsoleLog('WiFi mode change failed to send', 'error');
    alert("Couldn't reach Nomad.");
  }
}

async function wifiDisable() {
  const ok = confirm('Return Nomad to hotspot mode?\n\nIf it is on your home network, it restarts with the hotspot on.');
  if (!ok) return;
  try {
    const res = await adminFetch('/api/wifi-mode', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ body: JSON.stringify({ action: 'disable' }) })
    });
    const data = await res.json().catch(() => ({}));
    if (res.ok) {
      if (data.status === 'restarting') {
        addConsoleLog('WiFi Mode off, restarting in hotspot mode', 'info');
        alert('Nomad is restarting in hotspot mode. Reconnect to its WiFi in a moment.');
        closeWifiModal();
      } else {
        addConsoleLog('WiFi Mode disabled', 'info');
        wifiSetStatus('Hotspot mode, auto-reconnect is off', '');
      }
    } else {
      alert("Couldn't switch WiFi Mode: " + (data.error || ('HTTP ' + res.status)));
    }
  } catch (e) {
    alert("Couldn't reach Nomad.");
  }
}

// Temperature monitoring
let showFahrenheit = false;

async function updateTemp() {
  try {
    const res = await fetch('/cpu-temp');
    const data = await res.json();
    const temp = data.temperature || 0;
    
    let displayTemp = temp;
    let unit = "°C";
    
    if (showFahrenheit) {
      displayTemp = (temp * 9/5) + 32;
      unit = "°F";
    }

    const tempBtn = document.getElementById('cpu-temp');
    tempBtn.innerHTML = `${NomadIcons.svg('thermometer')} ${displayTemp.toFixed(1)} ${unit}`;

    // Color coding based on temperature
    tempBtn.classList.remove('btn-success', 'btn-warning', 'btn-danger', 'btn-secondary');
    if (temp < 60) tempBtn.classList.add('btn-success');
    else if (temp < 70) tempBtn.classList.add('btn-warning');
    else if (temp < 75) tempBtn.classList.add('btn-warning');
    else tempBtn.classList.add('btn-danger');

  } catch (e) {
    console.warn('Failed to fetch temp:', e);
  }
}

// Authentication system
async function requireAdminAuth(passedSettings) {
  const settings = passedSettings;
  const overlay = document.getElementById('auth-overlay');
  const passwordInput = document.getElementById('auth-password');
  const submitBtn = document.getElementById('auth-submit');
  const errorDiv = document.getElementById('auth-error');

  if (!overlay || !passwordInput || !submitBtn) {
    console.warn('requireAdminAuth: overlay or controls not present in DOM. Skipping auth overlay.');
    return;
  }

  // Check if password is disabled
  if (!settings || !settings.hasOwnProperty('adminPassword') ||
      settings.adminPassword === null || settings.adminPassword === '' || settings.adminPassword === 'null') {
    console.debug('requireAdminAuth: admin password explicitly disabled on server, skipping auth.');
    overlay.classList.add('hidden');
    return;
  }

  // a saved sign-in: ask the Nomad whether it still stands (a restart ends a sign-in that
  // wasn't kept, a password change ends them all)
  if (adminToken) {
    try {
      const r = await fetch('/auth/sessions', { cache: 'no-store', headers: { 'X-Admin-Token': adminToken } });
      if (r.ok || r.status === 404) { overlay.classList.add('hidden'); return; }   // 404: firmware from before /auth/sessions
    } catch (e) { overlay.classList.add('hidden'); return; }                       // can't tell: let adminFetch find out
    setAdminToken('');
  }

  overlay.classList.remove('hidden');
  passwordInput.focus();
}

// the sign-in box's buttons, wired once at start-up, so a session that runs out later
// (adminFetch shows the box on a 401) still has a working box
function attachAuthHandlers() {
  const overlay = document.getElementById('auth-overlay');
  const passwordInput = document.getElementById('auth-password');
  const submitBtn = document.getElementById('auth-submit');
  const errorDiv = document.getElementById('auth-error');
  const keepEl = document.getElementById('auth-keep');
  if (!overlay || !submitBtn || submitBtn._authHandlerAttached) return;
  let lockTimer = null;
  const showError = (msg) => { if (errorDiv) { errorDiv.textContent = msg; errorDiv.classList.remove('hidden'); } };
  const lockFor = (secs) => {
    clearInterval(lockTimer);
    let left = Math.max(1, secs | 0);
    submitBtn.disabled = true;
    const tick = () => {
      if (left <= 0) { clearInterval(lockTimer); submitBtn.disabled = false; showError('Try again now.'); return; }
      showError(`Too many wrong passwords. Try again in ${left} s.`);
      left--;
    };
    tick();
    lockTimer = setInterval(tick, 1000);
  };
  {
    const authenticate = async () => {
      const inputPw = passwordInput.value;
      if (!inputPw || submitBtn.disabled) return;
      submitBtn.disabled = true;
      const label = submitBtn.textContent;
      submitBtn.textContent = 'Checking…';
      try {
        const keep = !keepEl || keepEl.checked;
        let r = await signIn(inputPw, keep);
        // a password pasted with a stray space at either end
        if (r.status === 401 && inputPw.trim() !== inputPw) r = await signIn(inputPw.trim(), keep);
        submitBtn.textContent = label;
        submitBtn.disabled = false;
        if (r.status === 200 && r.token) {
          setAdminToken(r.token, keep);
          overlay.classList.add('hidden');
          if (errorDiv) errorDiv.classList.add('hidden');
          passwordInput.value = '';
          // signed in: what was held back before (the hotspot password) loads now
          loadSettings();
          if (typeof refreshSessionInfo === 'function') refreshSessionInfo();
        } else if (r.status === 429) {
          passwordInput.value = '';
          lockFor(r.wait || 30);
        } else {
          showError(r.status === 401 ? 'Wrong password.' : `Nomad didn't answer (HTTP ${r.status}).`);
          passwordInput.value = '';
          passwordInput.focus();
        }
      } catch (e) {
        submitBtn.textContent = label;
        submitBtn.disabled = false;
        console.error('Authentication error:', e);
        showError("Couldn't reach Nomad. Check you're on its WiFi.");
      }
    };

    submitBtn.onclick = authenticate;
    passwordInput.onkeydown = (e) => {
      if (e.key === 'Enter') authenticate();
    };
    submitBtn._authHandlerAttached = true;
  }
}
// Initialize everything when DOM is ready
document.addEventListener('DOMContentLoaded', async () => {
  // Initialize console
  addConsoleLog('Admin panel initialized', 'info');
  attachAuthHandlers();
  
  // Load settings and authenticate
  await loadSettings();
  refreshSessionInfo();
  NomadUpdate.init();   // draws the Update card only on home WiFi with the server reachable
  
  // initial data (refreshConsole is kicked off by the adaptive poll loop below)
  fetchSD();
  fetchSdBreakdown(); // shows whatever was cached from the last scan, if any
  fetchAdminBar();
  updateTemp();

  // Set up RGB controls
  const colorPicker = document.getElementById('led-color');
  const modeButtons = {
    off: document.getElementById('mode-off'),
    solid: document.getElementById('mode-solid'),
    rainbow: document.getElementById('mode-rainbow')
  };

  // Color picker event
  if (colorPicker) {
    colorPicker.addEventListener('input', () => {
      if (ledMode === 'solid') {
        sendColorToServer(colorPicker.value);
      }
    });
  }

  // Mode button events
  if (modeButtons.off) {
    modeButtons.off.addEventListener('click', async () => {
      updateModeUI('off');
      await sendModeToServer('off');
      await saveSettings();
    });
  }

  if (modeButtons.solid) {
    modeButtons.solid.addEventListener('click', async () => {
      updateModeUI('solid');
      await sendModeToServer('solid');
      await sendColorToServer(colorPicker.value);
      await saveSettings();
    });
  }

  if (modeButtons.rainbow) {
    modeButtons.rainbow.addEventListener('click', async () => {
      updateModeUI('rainbow');
      await sendModeToServer('rainbow');
      await saveSettings();
    });
  }

  // Action button events
  const restartBtn = document.getElementById('btn-restart');
  if (restartBtn) {
    restartBtn.addEventListener('click', async () => {
      if (!confirm('Restart Nomad?')) return;
      try {
        await adminFetch('/restart', { method: 'POST' });
        addConsoleLog('Restart command sent', 'info');
        alert('Nomad is restarting. Reconnect in a moment.');
      } catch {
        addConsoleLog('Nomad disconnected, restarting', 'warning');
        alert('Nomad disconnected to restart. Reconnect in a moment.');
      }
    });
  }

  const shutdownBtn = document.getElementById('btn-shutdown');
  if (shutdownBtn) {
    shutdownBtn.addEventListener('click', async () => {
      const ok = confirm('Shut down Nomad?\n\nThe card is unmounted first, then Nomad goes into deep sleep.');
      if (!ok) return;

      try {
        const res = await adminFetch('/shutdown', { method: 'POST' });
        if (res.ok) {
          addConsoleLog('Safe shutdown initiated', 'info');
          alert('Shutting down. Nomad powers off in a moment.');
        } else {
          addConsoleLog('Shutdown command failed', 'error');
          alert("Nomad didn't shut down. Try again.");
        }
      } catch (err) {
        addConsoleLog('Nomad disconnected, shutting down', 'warning');
        alert('Nomad disconnected. It is probably shutting down.');
      }
    });
  }

  const usbBtn = document.getElementById('btn-usbmode');
  if (usbBtn) {
    usbBtn.addEventListener('click', async () => {
      const ok = confirm(
        'Restart Nomad in USB mode?\n\n' +
        'The card can take over a minute to show up on the computer. ' +
        'To leave USB mode, unplug and replug Nomad.'
      );
      if (!ok) return;
      try {
        await adminFetch('/enterUsb', { method: 'POST' });
        addConsoleLog('USB mode command sent', 'info');
        alert('Nomad is restarting in USB mode.');
      } catch {
        addConsoleLog('Nomad disconnected, USB mode starting', 'warning');
        alert('Nomad disconnected. Check your computer in about a minute.');
      }
    });
  }

  const flashBtn = document.getElementById('btn-flashmode');
  if (flashBtn) {
    flashBtn.addEventListener('click', async () => {
      const ok = confirm('Restart Nomad in flash mode for a firmware update?');
      if (!ok) return;
      try {
        await adminFetch('/flash-mode', { method: 'POST' });
        addConsoleLog('Flash mode command sent', 'info');
        alert('Nomad is restarting in flash mode.');
      } catch {
        addConsoleLog('Nomad disconnected, flash mode starting', 'error');
        alert('Nomad disconnected. Flash mode is probably starting.');
      }
    });
  }

  // WiFi Mode popup
  const wifiBtn = document.getElementById('btn-wifimode');
  if (wifiBtn) wifiBtn.addEventListener('click', openWifiModal);
  const wifiCloseBtn = document.getElementById('wifi-close-btn');
  if (wifiCloseBtn) wifiCloseBtn.addEventListener('click', closeWifiModal);
  const wifiScanBtn = document.getElementById('wifi-scan-btn');
  if (wifiScanBtn) wifiScanBtn.addEventListener('click', wifiStartScan);
  const wifiConnectBtn = document.getElementById('wifi-connect-btn');
  if (wifiConnectBtn) wifiConnectBtn.addEventListener('click', wifiConnect);
  const wifiDisableBtn = document.getElementById('wifi-disable-btn');
  if (wifiDisableBtn) wifiDisableBtn.addEventListener('click', wifiDisable);
  // click outside the panel closes the popup, like a modal should
  const wifiOverlay = document.getElementById('wifi-overlay');
  if (wifiOverlay) wifiOverlay.addEventListener('click', (e) => {
    if (e.target === wifiOverlay) closeWifiModal();
  });
  // captive DNS mode saves immediately and applies live, no restart
  const dnsCatchEl = document.getElementById('wifi-dns-catchall');
  if (dnsCatchEl) dnsCatchEl.addEventListener('change', async () => {
    try {
      await adminFetch('/settings', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ body: JSON.stringify({ dnsCatchAll: dnsCatchEl.checked }) })
      });
      addConsoleLog('Captive DNS: ' + (dnsCatchEl.checked ? 'catch-all (legacy)' : 'targeted'), 'info');
    } catch (e) {
      addConsoleLog('Failed to save DNS mode', 'error');
    }
  });

  // Temperature click to toggle units
  const tempBtn = document.getElementById('cpu-temp');
  if (tempBtn) {
    tempBtn.addEventListener('click', () => {
      showFahrenheit = !showFahrenheit;
      updateTemp();
    });
  }

  // Auto-generate toggle
  const autoToggle = document.getElementById('auto-generate');
  if (autoToggle) {
    autoToggle.addEventListener('change', () => {
      isAutoGenerate = autoToggle.checked;
      saveSettings();
      addConsoleLog(`Check for new files on boot ${isAutoGenerate ? 'enabled' : 'disabled'}`, 'info');
    });
  }

  // Screen flip toggle - firmware applies the rotation live on save
  const flipToggle = document.getElementById('flip-screen');
  if (flipToggle) {
    flipToggle.addEventListener('change', () => {
      saveSettings();
      addConsoleLog(`Screen flipped ${flipToggle.checked ? '180° (upside-down mount)' : 'back to normal'}`, 'info');
    });
  }

  // TV support toggle - firmware starts/stops SSDP announcements on save.
  // TVs that already cached the entry can take a minute to drop it.
  const dlnaToggle = document.getElementById('dlna-enabled');
  if (dlnaToggle) {
    dlnaToggle.addEventListener('change', () => {
      saveSettings();
      const msg = document.getElementById('dlna-msg');
      if (msg) msg.textContent = dlnaToggle.checked
        ? 'On. TVs on this WiFi list "Nomad" as a media server within a minute or two.'
        : 'Off. TVs may keep a stale entry until they refresh their source list.';
      addConsoleLog(`TV support (DLNA) ${dlnaToggle.checked ? 'enabled' : 'disabled'}`, 'info');
    });
  }

  // Disable password button
  const disablePasswordBtn = document.getElementById('btn-disable-password');
  if (disablePasswordBtn) {
    disablePasswordBtn.addEventListener('click', async () => {
      if (!confirm('Turn the admin password off? Anyone on Nomad\'s WiFi will be able to change its settings.')) return;
      try {
        let res = await postPassword({ disable: true });
        if (res.status === 404) {
          res = await adminFetch('/settings', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: new URLSearchParams({ body: JSON.stringify({ adminPassword: '' }) }) });
        }
        if (res.ok) { setAdminToken(''); location.reload(); }
        else alert(`Nomad refused (HTTP ${res.status}).`);
      } catch (e) {
        console.error('Error disabling admin password:', e);
        alert("Couldn't reach Nomad.");
      }
    });
  }

  // LCD toggle button
  // Separate from the slider, the slider's minimum is 1 so it can't turn the screen off.
  const lcdToggleBtn = document.getElementById('lcd-toggle');
  if (lcdToggleBtn) {
    let lcdOff = false;
    try { lcdOff = localStorage.getItem('lcd_off') === '1'; } catch (e) {}
    const label = () => { lcdToggleBtn.textContent = lcdOff ? 'Turn on' : 'Turn off'; };
    label();
    lcdToggleBtn.addEventListener('click', async () => {
      const brightnessElement = document.getElementById('brightness');
      try {
        if (lcdOff) {
          const stored = parseInt(localStorage.getItem('lcd_brightness') || '', 10);
          const level = stored > 0 ? stored : (parseInt(brightnessElement.value, 10) || 100);
          await setBrightness(level);
          brightnessElement.value = level;
          updateBrightnessLabel(level);
          lcdOff = false;
        } else {
          localStorage.setItem('lcd_brightness', brightnessElement.value);
          await setBrightness(0);
          lcdOff = true;
        }
        localStorage.setItem('lcd_off', lcdOff ? '1' : '0');
      } catch (e) {
        console.error('Failed to toggle LCD:', e);
      }
      label();
    });
    // moving the slider turns the screen on again
    document.getElementById('brightness').addEventListener('change', () => { lcdOff = false; try { localStorage.setItem('lcd_off', '0'); } catch (e) {} label(); });
  }

  // Set up periodic updates
  setInterval(fetchAdminBar, 30000); // Every 30 seconds
  setInterval(updateTemp, 6000); // Every 6 seconds
  setInterval(fetchSD, 60000); // Every minute

  // adaptive poll: every 2s while indexing so the bar looks live, 10s when idle.
  // self-reschedules instead of setInterval so it reacts to the last poll
  (async function pollConsoleLoop() {
    await refreshConsole();
    setTimeout(pollConsoleLoop, indexingActive ? 2000 : 10000);
  })();
});

// Global functions for HTML onclick handlers
window.updateAdminPassword = updateAdminPassword;
window.updateWiFiSettings = updateWiFiSettings;
window.setBrightness = setBrightness;
window.updateBrightnessLabel = updateBrightnessLabel;
window.generateMediaJson = generateMediaJson;
window.refreshConsole = refreshConsole;

// Also expose utility/init functions that may be called from HTML or externally
window.fetchSD = fetchSD;
window.applyThemeFlag = applyThemeFlag;
window.initTheme = initTheme;


/* Sections, one at a time. The shown one is in the address (#pages) so a reload comes
   back to it. The file browser and theme editor load the first time they open. */
function showSection(sec) {
  const tabs = [...document.querySelectorAll('#adm-tabs button')];
  if (!tabs.some(t => t.dataset.sec === sec)) sec = 'overview';
  tabs.forEach(t => t.classList.toggle('active', t.dataset.sec === sec));
  document.querySelectorAll('.adm-sections > [data-sec]').forEach(c => c.classList.toggle('shown', c.dataset.sec === sec));
  for (const f of document.querySelectorAll('.adm-sections > [data-sec="' + sec + '"] iframe[data-src]')) {
    if (!f.getAttribute('src')) f.src = f.dataset.src;
  }
  try { history.replaceState(null, '', '#' + sec); localStorage.setItem('nomad_admin_section', sec); } catch (e) {}
  const active = tabs.find(t => t.dataset.sec === sec);
  if (active && active.scrollIntoView) active.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  if (sec === 'logs') renderLog(true);
  // what is playing: asked for only while it is on screen
  if (sec === 'activity') Activity.start(); else Activity.stop();
  if (sec === 'overview' && typeof Usage !== 'undefined' && Usage.data) Usage.load();   // fresh counts each look
  if (typeof Support !== 'undefined' && (sec === 'device' || !Support.loaded)) Support.load();   // after a sign-in too
  // the file browser, built into the page the first time Files opens
  if (sec === 'files' && window.NomadFiles && !showSection._files) {
    showSection._files = NomadFiles.mount(document.getElementById('nf-host'), { path: '/' });
  }
}
// the theme editor says how tall it is, and its frame takes that height: no scroll inside
window.addEventListener('message', (ev) => {
  if (ev.origin !== location.origin || !ev.data) return;
  const f = document.getElementById('theme-iframe');
  if (!f || ev.source !== f.contentWindow) return;
  if (ev.data.type === 'nomad-theme-height') f.style.height = Math.max(300, Math.ceil(+ev.data.height || 0)) + 'px';
  // saved in the editor: apply it here at once (the editor already updated the shared copy)
  if (ev.data.type === 'nomad-theme-saved' && window.ThemeManager) {
    if ('colors' in ev.data) ThemeManager.applyThemeColors(ev.data.colors);
    if ('mode' in ev.data) ThemeManager.applyDarkMode(ThemeManager.getDarkMode());
  }
});
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('#adm-tabs button').forEach(t => t.addEventListener('click', () => showSection(t.dataset.sec)));
  let start = location.hash.slice(1);
  if (!start) { try { start = localStorage.getItem('nomad_admin_section') || ''; } catch (e) {} }
  showSection(start || 'overview');
});
window.addEventListener('hashchange', () => showSection(location.hash.slice(1)));

/* UI configuration (/.system-ui.json), owned by the firmware via GET/POST /api/ui-config.
   The POST is admin-only and writes on a background task so saving can't stall the web server. */

const UI_CFG_FILE = '/.system-ui.json';
const UI_PAGES = [
  ['movies',  'Movies'],  ['shows',    'Shows'],    ['music', 'Music'],
  ['books',   'Books'],   ['gallery',  'Gallery'],  ['files', 'Files'],
  ['games',   'Games'],   ['maps',     'Maps'],     ['archive', 'Archive'],
  ['chat',    'Chat'],    ['translate','Translate'],
  ['cookbook','Cookbook'], ['workshop', 'Workshop'],
];
let uiCfg = { hiddenPages: [], downloadsDisabled: false, uploadsDisabled: false, dlOff: [], ulOff: [], motd: '' };
// which pages have downloads and uploads to turn off
const DL_PAGES = ['movies', 'shows', 'music', 'books', 'files', 'archive', 'workshop'];
const UL_PAGES = ['gallery', 'files', 'cookbook'];

async function loadUiConfig() {
  try {
    // firmware endpoint first (RAM copy, always current), file as fallback
    // so this page still works against an older firmware
    let r = await fetch('/api/ui-config?_=' + Date.now(), { cache: 'no-store' });
    let txt = r.ok ? await r.text() : '';
    let parsed = null;
    try { parsed = JSON.parse(txt); } catch (e) { parsed = null; }
    if (!parsed) {
      r = await fetch(UI_CFG_FILE + '?_=' + Date.now(), { cache: 'no-store' });
      if (r.ok) { try { parsed = JSON.parse(await r.text()); } catch (e) {} }
    }
    if (parsed) uiCfg = Object.assign(uiCfg, parsed);
  } catch (e) {}
  ['hiddenPages', 'dlOff', 'ulOff'].forEach(k => { if (!Array.isArray(uiCfg[k])) uiCfg[k] = []; });
  renderPerms();
  document.getElementById('ui-motd').value = uiCfg.motd || '';
}

// one row a page: show it, its downloads, its uploads (a box only where the page has them)
function renderPerms() {
  const body = document.getElementById('ui-pages');
  if (!body) return;
  const box = (kind, key, on, label) =>
    `<input type="checkbox" class="perm" data-kind="${kind}" data-page="${key}" ${on ? 'checked' : ''} aria-label="${label}">`;
  body.innerHTML = UI_PAGES.map(([key, label]) => `<tr>
      <td>${label}</td>
      <td>${box('show', key, !uiCfg.hiddenPages.includes(key), label + ': show on the menu')}</td>
      <td>${DL_PAGES.includes(key) ? box('dl', key, !uiCfg.dlOff.includes(key), label + ': downloads') : '<span class="na">&ndash;</span>'}</td>
      <td>${UL_PAGES.includes(key) ? box('ul', key, !uiCfg.ulOff.includes(key), label + ': uploads') : '<span class="na">&ndash;</span>'}</td>
    </tr>`).join('');
  const dl = document.getElementById('ui-downloads'), ul = document.getElementById('ui-uploads');
  dl.checked = !uiCfg.downloadsDisabled;
  ul.checked = !uiCfg.uploadsDisabled;
  paintPermMasters();
  if (!body._wired) {
    body._wired = true;
    body.addEventListener('change', savePerms);
    dl.addEventListener('change', savePerms);
    ul.addEventListener('change', savePerms);
  }
}
// with a column's master off, its page boxes say so (greyed) but keep their own setting
function paintPermMasters() {
  const dlOn = document.getElementById('ui-downloads').checked, ulOn = document.getElementById('ui-uploads').checked;
  document.querySelectorAll('#ui-pages input[data-kind="dl"]').forEach(i => { i.disabled = !dlOn; });
  document.querySelectorAll('#ui-pages input[data-kind="ul"]').forEach(i => { i.disabled = !ulOn; });
}
let _permTimer = null;
function savePerms() {
  const off = (kind) => [...document.querySelectorAll(`#ui-pages input[data-kind="${kind}"]`)].filter(i => !i.checked).map(i => i.dataset.page);
  uiCfg.hiddenPages = off('show');
  uiCfg.dlOff = off('dl');
  uiCfg.ulOff = off('ul');
  uiCfg.downloadsDisabled = !document.getElementById('ui-downloads').checked;
  uiCfg.uploadsDisabled = !document.getElementById('ui-uploads').checked;
  paintPermMasters();
  const msg = document.getElementById('ui-pages-msg');
  if (msg) msg.textContent = 'Saving…';
  clearTimeout(_permTimer);
  _permTimer = setTimeout(() => saveUiConfig('ui-pages-msg'), 500);   // several taps, one save
}
// kept: older code calls it
function savePageToggles() { savePerms(); }

function saveMotd() {
  uiCfg.motd = document.getElementById('ui-motd').value.trim().slice(0, 120);
  saveUiConfig('ui-community-msg');
}

async function saveUiConfig(msgId) {
  const msg = document.getElementById(msgId);
  const payload = JSON.stringify({ v: 1, hiddenPages: uiCfg.hiddenPages,
    downloadsDisabled: !!uiCfg.downloadsDisabled, uploadsDisabled: !!uiCfg.uploadsDisabled,
    dlOff: uiCfg.dlOff || [], ulOff: uiCfg.ulOff || [], motd: uiCfg.motd || '' });
  try {
    const res = await adminFetch('/api/ui-config', { method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ body: payload }) });
    if (msg) msg.textContent = res.ok ? 'Saved. Pages pick it up when they next load.' : 'Not saved (HTTP ' + res.status + ').';
  } catch (e) {
    if (msg) msg.textContent = "Couldn't save: " + e.message;
  }
}

/* delete every file in a community dotfolder (chat / whiteboard state) */
async function clearCommunityDir(dir, label, confirmed = false) {
  const msg = document.getElementById('ui-community-msg');
  if (!confirmed && !confirm(`Clear the ${label} for everyone? This cannot be undone.`)) return;
  let n = 0;
  try {
    const res = await fetch(`/listfiles?dir=${encodeURIComponent(dir)}&_=${Date.now()}`, { cache: 'no-store' });
    if (res.ok) {
      const arr = JSON.parse(await res.text());
      for (const f of (Array.isArray(arr) ? arr : [])) {
        if (f.isDir) continue;
        const ok = await fetch('/delete', { method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({ filename: `${dir}/${f.name}` }) });
        if (ok.ok) n++;
      }
    }
  } catch (e) {}
  if (msg) msg.textContent = `${label} cleared (${n} file${n === 1 ? '' : 's'} removed).`;
}
// Nomad keeps chat in memory too (NomadChat.h), so deleting the files alone isn't enough:
// its own clear empties both. Older firmware has only the files.
async function clearChatHistory() {
  const msg = document.getElementById('ui-community-msg');
  if (!confirm('Clear the chat history for everyone? This cannot be undone.')) return;
  try {
    const r = await adminFetch('/api/chat/clear', { method: 'POST' });
    if (r.ok) { if (msg) msg.textContent = 'Chat history cleared.'; return; }
    if (r.status !== 404) { if (msg) msg.textContent = `Nomad didn't clear the chat (HTTP ${r.status}).`; return; }
  } catch (e) {}
  return clearCommunityDir('/.chat', 'chat history', true);
}
const clearWhiteboard  = () => clearCommunityDir('/.whiteboard', 'live whiteboard');

loadUiConfig();

/* Library statistics, counted here from the /.system-index NDJSON files: bucket indexes
   hold top-level rows, <Bucket>__<name>.nested.ndjson each series' or album's files. */
const STAT_EXTS = {
  video: ['.mp4', '.mkv', '.webm', '.avi', '.mov', '.m4v'],
  audio: ['.mp3', '.m4a', '.m4b', '.flac', '.wav', '.ogg', '.aac', '.opus'],
  book:  ['.pdf', '.epub', '.cbz', '.cbr', '.azw3', '.mobi'],
  image: ['.jpg', '.jpeg', '.png', '.webp', '.gif', '.bmp'],
};
function statExt(name) {
  const m = String(name).toLowerCase().match(/\.[a-z0-9]+$/);
  return m ? m[0] : '';
}

async function computeLibraryStats() {
  const wrap = document.getElementById('lib-stats');
  if (!wrap) return;
  try {
    const listRes = await fetch(`/listfiles?dir=${encodeURIComponent('/.system-index')}&_=${Date.now()}`, { cache: 'no-store' });
    if (!listRes.ok) throw new Error('no index dir');
    const files = (JSON.parse(await listRes.text()) || [])
      .filter(f => !f.isDir && /\.ndjson$/.test(f.name)).map(f => f.name);

    // group index files by bucket
    const byBucket = {};
    for (const n of files) {
      const bucket = n.split(/\.|__/)[0];
      (byBucket[bucket] = byBucket[bucket] || []).push(n);
    }
    const rowsOf = async (name) => {
      try {
        const r = await fetch(`/.system-index/${encodeURIComponent(name)}?_=${Date.now()}`, { cache: 'no-store' });
        if (!r.ok) return [];
        return (await r.text()).split('\n').slice(1).filter(Boolean).map(l => {
          try { return JSON.parse(l); } catch (e) { return null; }
        }).filter(Boolean);
      } catch (e) { return []; }
    };
    // one at a time with a pause: the device serves these off the SD inside its network
    // task, and parallel index fetches can starve its watchdog
    const breather = () => new Promise(r => setTimeout(r, 150));
    const bucketRows = async (bucket) => {
      const out = [];
      for (const name of (byBucket[bucket] || [])) {
        out.push(...await rowsOf(name));
        await breather();
      }
      return out;
    };
    const count = (rows, exts) => rows.filter(r => r.t === 'f' && exts.includes(statExt(r.n))).length;

    const results = [];
    for (const b of ['Movies', 'Shows', 'Music', 'Books', 'Gallery', 'Files', 'Games']) {
      results.push(await bucketRows(b));
    }
    const [movies, shows, music, books, gallery, filesRows, games] = results;
    const topDirs = (rows, bucket) => rows.filter(r =>
      r.t === 'd' && (r.p.match(/\//g) || []).length === 2).length;

    const stats = [
      ['Movies',    count(movies, STAT_EXTS.video)],
      ['Shows',     (byBucket['Shows'] || []).filter(n => n.includes('__')).length || topDirs(shows, 'Shows')],
      ['Episodes',  count(shows, STAT_EXTS.video)],
      ['Albums',    topDirs(music, 'Music')],
      ['Songs',     count(music, STAT_EXTS.audio)],
      ['Books',     count(books, STAT_EXTS.book)],
      ['Audiobooks',count(books, STAT_EXTS.audio)],
      ['Photos',    count(gallery, STAT_EXTS.image)],
      ['Files',     filesRows.filter(r => r.t === 'f').length],
      ['Games',     games.filter(r => r.t === 'f' && !STAT_EXTS.image.includes(statExt(r.n))).length],
    ].filter(([, v]) => v > 0);

    wrap.innerHTML = stats.length ? stats.map(([label, v]) => `
      <div class="adm-tile"><b>${v.toLocaleString()}</b><span>${label}</span></div>`).join('')
      : '<div class="adm-tiles-note">No library index yet. Run a scan from Library.</div>';
  } catch (e) {
    wrap.innerHTML = '<div class="adm-tiles-note">No library counts yet. Run a scan from Library.</div>';
  }
}
// delayed start so the admin page's initial settings/status fetches finish first
setTimeout(computeLibraryStats, 2500);

// Interface updates. The Update card shows only on home WiFi with the update server
// reachable; in hotspot mode nothing is fetched from outside. The phone downloads each file
// and stages it (/api/update/stage, kept only if size and SHA-256 match the manifest), then
// /api/update/commit swaps them all in at once. Device side: NomadUpdate.h; server side:
// tools/ui-manifest.py.
const NomadUpdate = {
  el: {},
  manifest: null,
  manifestUrl: '',
  pending: [],

  async init() {
    const $ = id => document.getElementById(id);
    this.el = { card: $('update-card'), summary: $('update-summary'), btn: $('update-btn'),
                list: $('update-list'), progress: $('update-progress'), bar: $('update-bar') };
    if (!this.el.card) return;
    let env;
    try { env = await (await fetch('/api/update/env', { cache: 'no-store' })).json(); } catch (e) { return; }
    // ?update=test shows the card in hotspot mode, against ?manifest=, on bench builds only
    const qs = new URLSearchParams(location.search);
    const test = env.testMode && qs.get('update') === 'test';
    if (env.mode !== 'station' && !test) return;
    this.manifestUrl = (test && qs.get('manifest')) || env.manifest;

    try {
      const ctl = new AbortController();
      const t = setTimeout(() => ctl.abort(), 6000);
      const r = await fetch(this.manifestUrl, { cache: 'no-store', signal: ctl.signal });
      clearTimeout(t);
      if (!r.ok) return;
      this.manifest = await r.json();
    } catch (e) { return; }   // no internet: stay out of the way
    if (!this.manifest || this.manifest.format !== 1 || !Array.isArray(this.manifest.files)) return;

    this.show();
    const m = this.manifest;
    if (this.olderThan(env.firmware, m.minFirmware)) {
      this.say(`Build ${m.build} needs firmware ${m.minFirmware} · Nomad has ${env.firmware} · `,
               'https://nomadflash.jcorptech.net', 'Flash it');
      return;
    }
    this.say('Checking files…');
    const local = await this.localHashes(m.files.map(f => f.path));
    if (!local) return;
    this.pending = m.files.filter(f => !local[f.path] || local[f.path][1] !== f.sha256)
                          .map(f => Object.assign({ isNew: !local[f.path] }, f));
    this.render();
  },

  show() { this.el.card.hidden = false; },

  say(text, href, linkText) {
    const s = this.el.summary;
    s.textContent = text;
    if (href) {
      const a = document.createElement('a');
      a.href = href; a.target = '_blank'; a.rel = 'noopener'; a.textContent = linkText;
      s.appendChild(a);
    }
  },

  olderThan(a, b) {
    const pa = String(a || '0').split(/[.+-]/).map(n => parseInt(n, 10) || 0);
    const pb = String(b || '0').split(/[.+-]/).map(n => parseInt(n, 10) || 0);
    for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
      if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) < (pb[i] || 0);
    }
    return false;
  },

  size(n) {
    return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB';
  },

  async localHashes(paths) {
    const r = await adminFetch('/api/update/hash', { method: 'POST', headers: { 'Content-Type': 'text/plain' },
                                                     body: paths.join('\n') });
    if (r.status === 401) { this.say('Sign in to check for updates'); return null; }
    if (!r.ok) { this.say("Couldn't check files (" + r.status + ')'); return null; }
    for (let i = 0; i < 600; i++) {
      await new Promise(res => setTimeout(res, 500));
      const j = await (await fetch('/api/update/hash', { cache: 'no-store' })).json();
      if (j.state === 'done') return j.files;
      if (j.state === 'error') break;
      if (j.total) this.say(`Checking files… ${j.done} of ${j.total}`);
    }
    this.say("Couldn't check files");
    return null;
  },

  render() {
    const { list, btn } = this.el;
    list.innerHTML = '';
    const m = this.manifest;
    if (!this.pending.length) {
      this.say(`Up to date · ${m.build}`);
      btn.hidden = true;
      return;
    }
    const changed = this.pending.filter(f => !f.isNew).length;
    const added = this.pending.length - changed;
    const bytes = this.pending.reduce((n, f) => n + f.size, 0);
    this.say([changed && `${changed} changed`, added && `${added} new`, this.size(bytes), m.build]
             .filter(Boolean).join(' · '));
    for (const f of this.pending) {
      const li = document.createElement('li');
      if (f.isNew) li.className = 'is-new';
      const name = document.createElement('span');
      name.textContent = f.path.replace(/^\//, '');
      const sz = document.createElement('span');
      sz.textContent = this.size(f.size);
      li.append(name, sz);
      list.appendChild(li);
    }
    btn.hidden = false;
    btn.disabled = false;
    btn.textContent = `Update ${this.pending.length} file${this.pending.length === 1 ? '' : 's'}`;
    btn.onclick = () => this.run();
  },

  fileUrl(path) {
    // base: where the files live relative to the manifest (the flasher points it at the
    // template it already stages for the SD card tab); files/ is ui-manifest.py's layout
    const base = new URL(this.manifest.base || 'files/', this.manifestUrl);
    return new URL(path.replace(/^\//, '').split('/').map(encodeURIComponent).join('/'), base).href;
  },

  async run() {
    const { btn, progress, bar } = this.el;
    btn.disabled = true;
    progress.hidden = false;
    const files = this.pending;
    const total = files.reduce((n, f) => n + f.size, 0);
    let done = 0;
    for (let i = 0; i < files.length; i++) {
      const f = files[i];
      this.say(`Updating ${i + 1} of ${files.length} · ${f.path.replace(/^\//, '')}`);
      let ok = false;
      try {
        const r = await fetch(this.fileUrl(f.path), { cache: 'no-store' });
        const blob = r.ok ? await r.blob() : null;
        if (blob && blob.size === f.size) {
          const q = `path=${encodeURIComponent(f.path)}&size=${f.size}&sha256=${f.sha256}`;
          const s = await adminFetch('/api/update/stage?' + q, { method: 'POST',
            headers: { 'Content-Type': 'application/octet-stream' }, body: blob });
          ok = s.ok;
        }
      } catch (e) { ok = false; }
      if (!ok) {
        await adminFetch('/api/update/abort', { method: 'POST' }).catch(() => {});
        progress.hidden = true;
        this.say(`Update stopped at ${f.path.replace(/^\//, '')} · nothing was changed`);
        btn.disabled = false;
        btn.textContent = 'Try again';
        return;
      }
      done += f.size;
      bar.style.width = Math.round(done * 100 / total) + '%';
    }
    const c = await adminFetch('/api/update/commit', { method: 'POST' });
    if (!c.ok) {
      progress.hidden = true;
      this.say("Some files couldn't be replaced · see Logs");
      return;
    }
    // Scripts and styles are cached for 10 minutes and a reload does not revalidate them,
    // so refresh each replaced file in the cache first, or the page comes back half old.
    await Promise.all(files.map(f => fetch(f.path, { cache: 'reload' }).catch(() => {})));
    this.say(`Updated to ${this.manifest.build} · reloading`);
    setTimeout(() => location.reload(), 800);
  }
};

/* Activity: what is playing (GET /api/activity), polled every 2 s only while the section
   is open. A row is one device on one file; NomadActivity.h keeps them. */
const Activity = {
  timer: 0,
  fmt(b) { return b >= 1e9 ? (b / 1e9).toFixed(2) + ' GB' : b >= 1e6 ? Math.round(b / 1e6) + ' MB' : Math.max(1, Math.round(b / 1e3)) + ' KB'; },
  dur(s) { return s < 60 ? s + ' s' : s < 3600 ? Math.round(s / 60) + ' min' : Math.floor(s / 3600) + ' h ' + Math.round((s % 3600) / 60) + ' min'; },
  esc(v) { return String(v).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); },
  start() { if (this.timer) return; this.tick(); this.timer = setInterval(() => { if (!document.hidden) this.tick(); }, 2000); },
  stop() { clearInterval(this.timer); this.timer = 0; },
  async tick() {
    const list = document.getElementById('act-list');
    let d;
    try {
      const r = await adminFetch('/api/activity', { cache: 'no-store' });
      // older firmware answers any address it doesn't know with its start page, under a 200
      const old = r.status === 404 || !/json/.test(r.headers.get('content-type') || '');
      if (!r.ok || old) {
        list.innerHTML = `<div class="adm-tiles-note">${old ? "This firmware has no Activity. Update the firmware." : "Nomad didn't answer (HTTP " + r.status + ').'}</div>`;
        if (old) this.stop();
        return;
      }
      d = await r.json();
    } catch (e) { return; }   // a missed look; the next one is two seconds away
    const rows = (d.rows || []).sort((a, b) => (b.live - a.live) || (a.ago - b.ago));
    const playing = rows.filter(r => r.live).length;
    document.getElementById('act-count').textContent = playing ? `${playing} going` : '';
    if (!rows.length) { list.innerHTML = '<div class="adm-tiles-note">Nothing playing right now.</div>'; return; }
    list.innerHTML = rows.map(r => {
      const parts = r.path.split('/').filter(Boolean);
      const title = (parts.pop() || '').replace(/\.[^.]+$/, '');
      const pct = r.size ? Math.min(100, Math.round(r.at / r.size * 100)) : 0;
      const state = r.stopped != null ? ['stopped', 'Stopped']
        : r.live ? ['on', r.dl ? 'Downloading' : 'Playing']
        : r.ago < 90 ? ['idle', 'Paused'] : ['idle', 'Idle ' + this.dur(r.ago)];
      const meta = [`${this.fmt(r.at)} of ${this.fmt(r.size)} (${pct}%)`];
      if (r.live && r.rate) meta.push((r.rate / 1e6).toFixed(1) + ' MB/s');
      meta.push('for ' + this.dur(r.since));
      if (r.stopped != null) meta.push('blocked ' + this.dur(r.stopped) + ' more');
      return `<div class="act-row" data-id="${r.id}">
        <div class="act-title" title="${this.esc(r.path)}">${this.esc(title)}</div>
        <span class="act-state ${state[0]}">${state[1]}</span>
        <div class="act-sub">${this.esc(parts.join(' / '))} · ${this.esc(r.dev)} ${this.esc(r.ip)}</div>
        <div class="act-bar"><i style="width:${pct}%"></i></div>
        <div class="act-meta">${meta.map(m => `<span>${this.esc(m)}</span>`).join('')}</div>
        <div class="act-actions">${r.stopped != null
          ? `<button class="btn btn-secondary" data-act="allow">Allow again</button>`
          : `<button class="btn btn-danger" data-act="stop">Stop</button>`}</div>
      </div>`;
    }).join('');
  },
  async act(id, what, title) {
    const msg = document.getElementById('act-msg');
    if (what === 'stop' && !confirm(`Stop ${title}? That device can't open it again for 30 minutes unless you allow it.`)) return;
    try {
      const r = await adminFetch('/api/activity/' + what, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ id: String(id) }) });
      const j = await r.json().catch(() => ({}));
      msg.textContent = r.ok ? (what === 'stop' ? 'Stopped.' : 'Allowed again.') : (j.error || 'HTTP ' + r.status);
    } catch (e) { msg.textContent = "Couldn't reach Nomad."; }
    this.tick();
  },
};
document.addEventListener('click', (ev) => {
  const b = ev.target.closest('#act-list [data-act]');
  if (!b) return;
  const row = b.closest('.act-row');
  Activity.act(row.dataset.id, b.dataset.act, row.querySelector('.act-title').textContent);
});

/* Card health (GET /api/storage-health) */
const CardHealth = {
  MAKERS: { 1: 'Panasonic', 2: 'Toshiba / Kioxia', 3: 'SanDisk', 0x1b: 'Samsung', 0x1d: 'ADATA', 0x27: 'Phison', 0x28: 'Lexar',
    0x31: 'Silicon Power', 0x41: 'Kingston', 0x74: 'Transcend', 0x76: 'Patriot', 0x82: 'Sony', 0x9c: 'Angelbird', 0xad: 'Longsys' },
  loaded: false,
  async load() {
    const sum = document.getElementById('hl-summary');
    try {
      const r = await adminFetch('/api/storage-health', { cache: 'no-store' });
      // older firmware answers any address it doesn't know with its start page, under a 200
      if (r.status === 404 || (r.ok && !/json/.test(r.headers.get('content-type') || ''))) {
        sum.textContent = "This firmware doesn't have card health yet."; document.getElementById('hl-check').hidden = true; return null;
      }
      if (!r.ok) { sum.textContent = "Nomad didn't answer (HTTP " + r.status + ').'; return null; }
      const d = await r.json();
      this.render(d);
      this.loaded = true;
      return d;
    } catch (e) { sum.textContent = "Couldn't reach Nomad."; return null; }
  },
  render(d) {
    const b = d.boot || {}, c = d.check || {}, card = d.card || {};
    const failed = (b.readErrors || 0) + (b.writeErrors || 0) + (c.state === 'done' ? c.errors || 0 : 0);
    const slow = (b.slow || 0) + (c.state === 'done' ? c.slow || 0 : 0);
    const badge = document.getElementById('hl-badge');
    const [cls, word, line] = failed ? ['bad', 'Problems', `${failed} read${failed === 1 ? '' : 's'} or write${failed === 1 ? '' : 's'} failed. Copy anything you can't replace off the card, it may be wearing out.`]
      : slow > 5 ? ['warn', 'Slow', 'Some reads were slow. Fine for one film, a few at once may stutter.']
      : ['good', 'Good', c.state === 'done' ? 'No failed reads, and the last check came back clean.' : 'No failed reads since Nomad started.'];
    badge.className = 'adm-badge ' + cls; badge.textContent = word; badge.hidden = false;
    document.getElementById('hl-summary').textContent = line;
    const gb = (n) => (n / 1e9).toFixed(n >= 1e11 ? 0 : 1) + ' GB';
    const facts = [];
    if (card.bytes) {
      const maker = this.MAKERS[card.mid] || ('maker ' + card.mid);
      facts.push(['Card', `${maker} ${String(card.name || '').trim()}, made ${card.made}`]);
      facts.push(['Size', `${gb(card.bytes)} ${card.fs || ''}`]);
      facts.push(['Link', `${card.bus}-bit at ${Math.round(card.mhz)} MHz`]);
    }
    const mb = (n) => n >= 1024 ? (n / 1024).toFixed(1) + ' GB' : (n || 0) + ' MB';
    facts.push(['Since start', `${(b.reads || 0).toLocaleString()} reads (${mb(b.mbRead)}), ${b.readErrors || 0} failed, ${b.slow || 0} slow` +
      (b.worstMs ? `, worst ${b.worstMs} ms` : '')]);
    if (b.writes) facts.push(['Writes', `${b.writes.toLocaleString()} (${mb(b.mbWritten)}), ${b.writeErrors || 0} failed`]);
    // the speed only means something with reads the size a film uses; short of memory the check reads 4 KB at a time
    if (c.state === 'done') facts.push(['Last check', `${c.done} reads, ${c.errors} failed, ${c.slow} slow` + (c.chunkKb >= 16 ? `, ${(+c.mbps).toFixed(1)} MB/s` : '') +
      (c.ago >= 0 ? `, ${Activity.dur(c.ago)} ago` : '')]);
    document.getElementById('hl-facts').innerHTML = facts.map(([k, v]) => `<dt>${Activity.esc(k)}</dt><dd>${Activity.esc(v)}</dd>`).join('');
    const btn = document.getElementById('hl-check');
    btn.disabled = c.state === 'running';
    btn.textContent = c.state === 'running' ? `Checking ${c.done} of ${c.total}` : 'Check the card';
  },
  async check() {
    const msg = document.getElementById('hl-msg');
    msg.textContent = '';
    try {
      const r = await adminFetch('/api/storage-health/check', { method: 'POST' });
      if (!r.ok && r.status !== 409) { const j = await r.json().catch(() => ({})); msg.textContent = j.error || 'HTTP ' + r.status; return; }
    } catch (e) { msg.textContent = "Couldn't reach Nomad."; return; }
    for (let i = 0; i < 90; i++) {
      await new Promise(res => setTimeout(res, 1000));
      const d = await this.load();
      if (d && d.check && d.check.state !== 'running') break;
    }
  },
};
document.addEventListener('DOMContentLoaded', () => {
  const b = document.getElementById('hl-check');
  if (b) b.addEventListener('click', () => CardHealth.check());
  setTimeout(() => { if (!CardHealth.loaded) CardHealth.load(); }, 1500);
});

/* Backup: settings to a file and back */
const BACKUP_SETTINGS = ['wifiSSID', 'wifiPassword', 'rgbMode', 'rgbColor', 'brightness', 'autoGenerateMedia', 'flipScreen', 'dlnaEnabled', 'dnsCatchAll'];
async function backupDownload() {
  const msg = document.getElementById('bk-msg');
  try {
    const s = await (await adminFetch('/settings', { cache: 'no-store' })).json();
    if (s.wifiPassword === '__set__') { msg.textContent = 'Sign in first, so the hotspot password can go in the backup.'; return; }
    const ui = await (await fetch('/api/ui-config?_=' + Date.now(), { cache: 'no-store' })).json();
    const theme = await ThemeManager.fetchCard();
    const settings = {};
    for (const k of BACKUP_SETTINGS) if (k in s) settings[k] = s[k];
    const file = { nomadBackup: 1, saved: new Date().toISOString(), settings, pages: ui, theme: theme || null };
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(file, null, 2)], { type: 'application/json' }));
    a.download = 'nomad-settings-' + new Date().toISOString().slice(0, 10) + '.json';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    msg.textContent = 'Saved. It has the hotspot password in it, so keep it somewhere private.';
  } catch (e) { msg.textContent = "Couldn't make the backup: " + e.message; }
}
async function backupRestore(f) {
  const msg = document.getElementById('bk-msg');
  let b;
  try { b = JSON.parse(await f.text()); } catch (e) { msg.textContent = "That file isn't a Nomad backup."; return; }
  if (!b || b.nomadBackup !== 1 || typeof b.settings !== 'object') { msg.textContent = "That file isn't a Nomad backup."; return; }
  const when = b.saved ? new Date(b.saved).toLocaleString() : 'an unknown date';
  if (!confirm(`Restore the settings saved ${when}? This replaces the hotspot, pages, theme, screen and lights settings on Nomad.`)) return;
  const done = [], failed = [];
  const post = (url, body) => adminFetch(url, { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({ body: JSON.stringify(body) }) });
  try {
    const s = {};
    for (const k of BACKUP_SETTINGS) if (k in b.settings) s[k] = b.settings[k];
    if (s.wifiPassword === '') s.wifiOpen = true;
    ((await post('/settings', s)).ok ? done : failed).push('settings');
    if (b.pages && typeof b.pages === 'object') ((await post('/api/ui-config', b.pages)).ok ? done : failed).push('pages');
    const t = await ThemeManager.writeCard(b.theme && typeof b.theme === 'object' ? b.theme : null);
    (t.ok ? done : failed).push('theme');
  } catch (e) { failed.push('the rest (' + e.message + ')'); }
  msg.textContent = failed.length ? `Restored ${done.join(', ') || 'nothing'}. Not restored: ${failed.join(', ')}.`
    : 'Restored. Hotspot changes take effect after a restart.';
  try { loadSettings(); loadUiConfig(); ThemeManager.sync(); } catch (e) {}
}
document.addEventListener('DOMContentLoaded', () => {
  const save = document.getElementById('bk-save'), load = document.getElementById('bk-load'), file = document.getElementById('bk-file');
  if (!save) return;
  save.addEventListener('click', backupDownload);
  load.addEventListener('click', () => file.click());
  file.addEventListener('change', () => { const f = file.files && file.files[0]; file.value = ''; if (f) backupRestore(f); });
});

/* Support file and safe mode (NomadSupport.h). /api/support has the build, chip, memory,
   start reason and stored crash; the rest comes from routes other sections use. Passwords
   never go in it. */
const SUPPORT_SECRET = /pass|psk|secret|token|key|hash|pin/i;
const Support = {
  data: null, loaded: false,
  async load() {
    try {
      const r = await adminFetch('/api/support', { cache: 'no-store' });
      if (!r.ok) return;
      this.data = await r.json();
      this.loaded = true;
      this.render();
    } catch (e) {}
  },
  crashText(c) {
    if (!c) return 'None kept';
    return `In ${c.task || 'a task'}, ${c.thisStart ? 'ended the last run' : 'from an earlier run'}` + (c.sameBuild ? '' : ' (older firmware)');
  },
  render() {
    const d = this.data;
    if (!d) return;
    const crash = document.getElementById('sup-crash');
    if (crash) crash.textContent = this.crashText(d.crash);
    const forget = document.getElementById('sup-forget');
    if (forget) forget.hidden = !d.crash;
    const banner = document.getElementById('safe-banner');
    if (banner) {
      banner.hidden = !(d.start && d.start.safe);
      const why = document.getElementById('safe-why');
      if (why && d.start) why.textContent = `It crashed ${d.start.inARow} times in a row` +
        (d.crash && d.crash.task ? `, last in ${d.crash.task}` : '') + '. Indexing and TV support are off until it starts normally.';
    }
  },
  async get(url) {
    try {
      const r = await adminFetch(url, { cache: 'no-store' });
      return r.ok ? await r.json() : { error: 'HTTP ' + r.status };
    } catch (e) { return { error: e.message }; }
  },
  scrub(o) {
    if (Array.isArray(o)) return o.map(v => this.scrub(v));
    if (!o || typeof o !== 'object') return o;
    const out = {};
    // true/false (adminPasswordSet) says whether one is set, not what it is: kept
    for (const [k, v] of Object.entries(o)) out[k] = SUPPORT_SECRET.test(k) && typeof v !== 'boolean' ? '(removed)' : this.scrub(v);
    return out;
  },
  async save(msgId) {
    const msg = document.getElementById(msgId || 'sup-msg');
    if (msg) msg.textContent = 'Gathering…';
    // one at a time: Nomad may be busy serving films while this runs
    const support = await this.get('/api/support');
    const firmware = await this.get('/api/firmware');
    const card = await this.get('/api/storage-health');
    const log = await this.get('/console-logs');
    const settings = this.scrub(await this.get('/settings'));
    const pages = await this.get('/api/ui-config?_=' + Date.now());
    const usage = await this.get('/api/usage');
    if (usage && usage.top) delete usage.top;   // counts only, not which files people played
    // log times are uptime: say how long before the file was made, easier to read than ms
    if (log && Array.isArray(log.logs) && log.now) {
      log.logs = log.logs.map(l => ({ ago: Math.round((log.now - l.timestamp) / 1000) + 's', type: l.type, message: l.message }));
    }
    const file = { nomadSupport: 1, saved: new Date().toISOString(), browser: navigator.userAgent,
      support, firmware, card, log, settings, pages, usage };
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(file, null, 2)], { type: 'application/json' }));
    a.download = `nomad-support-${(support && support.version) || 'x'}-${new Date().toISOString().slice(0, 10)}.json`;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    if (msg) msg.textContent = 'Saved. Passwords are taken out. The log names files and networks, so send it only to someone you trust.';
  },
  async forget() {
    if (!confirm('Forget the crash Nomad kept? Save a support file first if someone may need it.')) return;
    const r = await adminFetch('/api/support/forget-crash', { method: 'POST' });
    document.getElementById('sup-msg').textContent = r.ok ? 'Forgotten.' : "Couldn't forget it.";
    this.load();
  },
  async normal() {
    const b = document.getElementById('safe-normal');
    b.disabled = true; b.textContent = 'Restarting…';
    try { await adminFetch('/api/support/normal', { method: 'POST' }); } catch (e) {}
    // back when it answers again
    const t0 = Date.now();
    const poll = async () => {
      try { if ((await fetch('/api/firmware', { cache: 'no-store' })).status) { if (Date.now() - t0 > 8000) return location.reload(); } } catch (e) {}
      if (Date.now() - t0 < 90000) setTimeout(poll, 2000); else location.reload();
    };
    setTimeout(poll, 4000);
  },
};
document.addEventListener('DOMContentLoaded', () => {
  const on = (id, fn) => { const el = document.getElementById(id); if (el) el.addEventListener('click', fn); };
  on('sup-save', () => Support.save('sup-msg'));
  on('sup-forget', () => Support.forget());
  on('safe-save', () => Support.save('sup-msg'));
  on('safe-normal', () => Support.normal());
  setTimeout(() => { if (!Support.loaded) Support.load(); }, 1200);
});

/* Firmware over the air or from a file (NomadFirmware.h). Nomad writes the spare slot,
   checks the file (signed by nomadflash, for this board, newer unless allowed) and only
   then offers the restart. */
const Firmware = {
  data: null,
  el: (id) => document.getElementById(id),
  async load() {
    try {
      const r = await adminFetch('/api/firmware', { cache: 'no-store' });
      if (!r.ok || !/json/.test(r.headers.get('content-type') || '')) {
        this.el('fw-now').textContent = "This firmware can't update itself yet. Update it once over USB from nomadflash.";
        this.el('fw-actions').hidden = true;
        return null;
      }
      this.data = await r.json();
      this.render(this.data);
      return this.data;
    } catch (e) { return null; }
  },
  render(d) {
    this.el('fw-now').textContent = `Running ${d.version} for ${d.board}, ${d.channel} channel.`;
    const last = this.el('fw-last');
    last.textContent = d.proving ? 'Checking this firmware works after the update. Give it a minute.'
      : d.last && d.last.result === 'ok' ? `Updated to ${d.last.version}.`
      : d.last && d.last.result === 'back' ? `Tried ${d.last.version}, but ${d.last.why || 'it didn\'t start'}, so Nomad went back to ${d.version}.`
      : '';
    const pull = this.el('fw-pull'), pick = this.el('fw-pick');
    const busy = ['receiving', 'pulling', 'checking'].includes(d.state);
    pull.disabled = pick.disabled = busy || !d.signed || !d.spare || d.proving;
    pull.disabled = pull.disabled || !d.canPull;
    pull.title = d.canPull ? '' : 'Needs WiFi Mode, on a network with internet';
    const msg = this.el('fw-msg');
    if (!d.signed) msg.textContent = "This build has no update key, so it can only be updated over USB.";
    else if (!d.spare) msg.textContent = "Nomad has no spare firmware slot. Update it once over USB from nomadflash.";
    else if (d.state === 'failed') msg.textContent = d.error || 'The update stopped.';
    else if (d.state === 'idle' && d.error) msg.textContent = d.error;
    else if (!busy && d.state !== 'ready') msg.textContent = '';
    const prog = this.el('fw-progress');
    prog.hidden = !busy;
    if (busy) {
      const pct = d.total ? Math.round(d.written / d.total * 100) : 0;
      this.el('fw-bar').style.width = pct + '%';
      this.el('fw-progress-text').textContent = d.state === 'checking' ? 'Checking the firmware…'
        : d.state === 'pulling' && !d.total ? 'Asking nomadflash…'
        : `${d.state === 'pulling' ? 'Downloading' : 'Writing'} ${(d.written / 1e6).toFixed(1)} of ${(d.total / 1e6).toFixed(1)} MB`;
    }
    const ready = this.el('fw-ready');
    ready.hidden = d.state !== 'ready';
    if (d.ready) this.el('fw-ready-text').textContent = `${d.ready.version} for ${d.ready.board} is checked and ready.`;
  },
  async poll() {
    for (let i = 0; i < 600; i++) {
      await new Promise((res) => setTimeout(res, 1000));
      const d = await this.load();
      if (d && !['receiving', 'pulling', 'checking'].includes(d.state)) return d;
    }
  },
  async pull() {
    if (!confirm('Check nomadflash for newer firmware? Films pause while it downloads, a minute or two. Nothing changes until you restart into it.')) return;
    const older = this.el('fw-older').checked ? '?older=1' : '';
    const r = await adminFetch('/api/firmware/pull' + older, { method: 'POST' });
    if (!r.ok) { const j = await r.json().catch(() => ({})); this.el('fw-msg').textContent = j.error || 'HTTP ' + r.status; return; }
    this.el('fw-msg').textContent = '';
    await this.poll();
  },
  async upload(file) {
    const msg = this.el('fw-msg');
    if (!/\.nomadfw$/i.test(file.name)) { msg.textContent = 'Pick a .nomadfw file from nomadflash (the .bin files are for USB flashing).'; return; }
    const older = this.el('fw-older').checked ? '&older=1' : '';
    let r = await adminFetch(`/api/firmware/begin?size=${file.size}${older}`, { method: 'POST' });
    let j = await r.json().catch(() => ({}));
    if (!r.ok) { msg.textContent = j.error || 'HTTP ' + r.status; return; }
    msg.textContent = '';
    const SLICE = 32 * 1024;
    let at = 0, tries = 0;
    this.render(Object.assign({}, this.data, { state: 'receiving', total: file.size, written: 0 }));
    while (at < file.size) {
      try {
        r = await adminFetch(`/api/firmware/stage?offset=${at}`, { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' },
          body: file.slice(at, Math.min(file.size, at + SLICE)) });
        j = await r.json().catch(() => ({}));
      } catch (e) { r = { ok: false, status: 0 }; j = {}; }
      if (r.ok) { at = j.have; tries = 0; }
      else if (r.status === 409 && j.error === 'offset') at = j.have;            // carry on where it has got to
      else if ((r.status === 503 || r.status === 0) && tries++ < 40) { await new Promise((res) => setTimeout(res, 300)); continue; }
      else break;                                                                 // failed: Nomad says why
      this.render(Object.assign({}, this.data, { state: 'receiving', total: file.size, written: at }));
    }
    await this.poll();
  },
  async apply() {
    const d = this.data || {};
    if (!d.ready) return;
    if (!confirm(`Restart into ${d.ready.version}? Everything playing stops and Nomad is gone for about a minute. If the new firmware doesn't start properly it goes back to ${d.version} by itself.`)) return;
    const r = await adminFetch('/api/firmware/apply', { method: 'POST' });
    if (!r.ok) { const j = await r.json().catch(() => ({})); this.el('fw-msg').textContent = j.error || 'HTTP ' + r.status; return; }
    this.el('fw-ready').hidden = true;
    this.el('fw-msg').textContent = 'Restarting. This page reconnects when Nomad is back.';
    await new Promise((res) => setTimeout(res, 8000));
    for (let i = 0; i < 60; i++) {
      const back = await this.load();
      if (back) { this.el('fw-msg').textContent = ''; return; }
      await new Promise((res) => setTimeout(res, 3000));
    }
  },
  async discard() {
    await adminFetch('/api/firmware/abort', { method: 'POST' });
    this.load();
  },
};
document.addEventListener('DOMContentLoaded', () => {
  if (!document.getElementById('fw-card')) return;
  Firmware.el('fw-pull').addEventListener('click', () => Firmware.pull());
  Firmware.el('fw-pick').addEventListener('click', () => Firmware.el('fw-file').click());
  Firmware.el('fw-file').addEventListener('change', () => { const f = Firmware.el('fw-file').files[0]; Firmware.el('fw-file').value = ''; if (f) Firmware.upload(f); });
  Firmware.el('fw-apply').addEventListener('click', () => Firmware.apply());
  Firmware.el('fw-discard').addEventListener('click', () => Firmware.discard());
  setTimeout(() => Firmware.load(), 1200);
});

/* WiFi QR code (assets/nomad-qr.js, loaded on first open). Admin only: /settings shows
   the hotspot password only to the admin. A second code opens the pages for phones
   already on the WiFi. */
const WifiQR = {
  which: 'wifi', ssid: '', pass: '',
  async load() {
    if (window.NomadQR) return;
    await new Promise((res, rej) => { const s = document.createElement('script'); s.src = '/assets/nomad-qr.js?v=1'; s.onload = res; s.onerror = rej; document.head.appendChild(s); });
  },
  draw() {
    const page = location.origin + '/';
    const text = this.which === 'wifi' ? NomadQR.wifi(this.ssid, this.pass) : page;
    NomadQR.draw(document.getElementById('qr-canvas'), text, { ecl: 'M', scale: 10, margin: 2 });
    document.getElementById('qr-title').textContent = this.which === 'wifi' ? 'Join the WiFi' : 'Open the pages';
    document.querySelector('.qr-cap').textContent = this.which === 'wifi' ? "Point a phone's camera at this to join."
      : `For phones already on the WiFi: opens ${page}`;
    document.getElementById('qr-show-wifi').className = 'btn ' + (this.which === 'wifi' ? 'btn-primary' : 'btn-secondary');
    document.getElementById('qr-show-page').className = 'btn ' + (this.which === 'page' ? 'btn-primary' : 'btn-secondary');
  },
  async open() {
    const msg = document.getElementById('wifi-msg');
    try {
      const s = await (await adminFetch('/settings', { cache: 'no-store' })).json();
      if (s.wifiPassword === '__set__') { msg.textContent = 'Sign in first, so the code can carry the password.'; return; }
      this.ssid = s.wifiSSID || ''; this.pass = s.wifiPassword || '';
      await this.load();
    } catch (e) { msg.textContent = "Couldn't make the code: " + e.message; return; }
    document.getElementById('qr-ssid').textContent = this.ssid;
    document.getElementById('qr-pass').textContent = this.pass || 'none (open network)';
    this.which = 'wifi';
    this.draw();
    document.getElementById('qr-pop').hidden = false;
  },
  close() {
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    document.getElementById('qr-pop').hidden = true;
  },
};
document.addEventListener('DOMContentLoaded', () => {
  const pop = document.getElementById('qr-pop');
  if (!pop) return;
  document.getElementById('qr-open').addEventListener('click', () => WifiQR.open());
  document.getElementById('qr-close').addEventListener('click', () => WifiQR.close());
  pop.addEventListener('click', (e) => { if (e.target === pop) WifiQR.close(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !pop.hidden) WifiQR.close(); });
  document.getElementById('qr-show-wifi').addEventListener('click', () => { WifiQR.which = 'wifi'; WifiQR.draw(); });
  document.getElementById('qr-show-page').addEventListener('click', () => { WifiQR.which = 'page'; WifiQR.draw(); });
  document.getElementById('qr-full').addEventListener('click', () => {
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
    else if (pop.requestFullscreen) pop.requestFullscreen().catch(() => {});
  });
});

/* What got used (GET /api/usage, NomadUsage.h): counters since this start or all time. */
const Usage = {
  data: null, view: 'now',
  NAMES: { movies: 'Movies', shows: 'Shows', music: 'Music', books: 'Books', gallery: 'Gallery', files: 'Files', games: 'Games',
    maps: 'Maps', archive: 'Archive', translate: 'Translate', cookbook: 'Cookbook', workshop: 'Workshop', chat: 'Chat' },
  size(b) { return b >= 1e9 ? (b / 1e9).toFixed(1) + ' GB' : b >= 1e6 ? Math.round(b / 1e6) + ' MB' : Math.round(b / 1e3) + ' KB'; },
  async load() {
    try {
      const r = await adminFetch('/api/usage', { cache: 'no-store' });
      if (!r.ok || !/json/.test(r.headers.get('content-type') || '')) {
        document.getElementById('use-tiles').innerHTML = `<div class="adm-tiles-note">This firmware doesn't count use yet.</div>`;
        return;
      }
      this.data = await r.json();
      this.draw();
    } catch (e) {}
  },
  draw() {
    const d = this.data; if (!d) return;
    const v = this.view === 'all' ? d.all : d.now;
    document.querySelectorAll('.use-when .btn').forEach((b) => { b.className = 'btn ' + (b.dataset.v === this.view ? 'btn-primary' : 'btn-secondary'); });
    const tiles = [[v.peak, 'busiest'], [v.plays.toLocaleString(), v.plays === 1 ? 'play' : 'plays'], [this.size(v.bytes), 'sent']];
    if (this.view === 'now') tiles.push([d.now.phones, 'on now']);
    else tiles.push([d.all.boots, d.all.boots === 1 ? 'start' : 'starts']);
    document.getElementById('use-tiles').innerHTML = tiles.map(([n, l]) => `<div class="adm-tile"><b>${Activity.esc(n)}</b><span>${Activity.esc(l)}</span></div>`).join('');
    // most played and pages opened are all-time counts
    const top = (d.top || []).slice(0, 5);
    document.getElementById('use-top').innerHTML = top.length ? '<h3>Most played</h3>' + top.map(([p, n]) =>
      `<div class="use-row"><span title="${Activity.esc(p)}">${Activity.esc(p.split('/').pop().replace(/\.[^.]+$/, ''))}</span><b>${n}</b></div>`).join('') : '';
    const pages = Object.entries(d.pages || {}).filter(([, n]) => n > 0).sort((a, b) => b[1] - a[1]).slice(0, 6);
    const most = pages.length ? pages[0][1] : 1;
    document.getElementById('use-pages').innerHTML = pages.length ? '<h3>Pages opened</h3>' + pages.map(([k, n]) =>
      `<div class="use-bar"><span>${Activity.esc(this.NAMES[k] || k)}</span><i style="width:${Math.max(2, Math.round(n / most * 100))}%"></i><b>${n}</b></div>`).join('') : '';
  },
};
document.addEventListener('DOMContentLoaded', () => {
  if (!document.getElementById('use-card')) return;
  document.querySelectorAll('.use-when .btn').forEach((b) => b.addEventListener('click', () => { Usage.view = b.dataset.v; Usage.draw(); }));
  document.getElementById('use-reset').addEventListener('click', async () => {
    if (!confirm('Start counting from zero? Every total and the most played list are cleared.')) return;
    await adminFetch('/api/usage/reset', { method: 'POST' });
    Usage.load();
  });
  setTimeout(() => Usage.load(), 1800);
});
