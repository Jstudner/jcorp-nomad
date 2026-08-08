// <!-- Version 4 -->

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
  document.getElementById('sd-total').textContent = `Total: ${(total/1e9).toFixed(2)} GB`;
  document.getElementById('sd-used').textContent = `Used: ${(used/1e9).toFixed(2)} GB`;
  const percent = total > 0 ? Math.round((used / total) * 100) : 0;
  document.getElementById('sd-bar').style.width = `${percent}%`;
  document.getElementById('sd-percent').textContent = `${percent}%`;
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
    tr.style.borderBottom = '1px solid var(--card-border)';

    const nameTd = document.createElement('td');
    nameTd.style.padding = '0.35rem 0.25rem';
    nameTd.textContent = e.dir;
    tr.appendChild(nameTd);

    const sizeTd = document.createElement('td');
    sizeTd.style.padding = '0.35rem 0.25rem';
    sizeTd.style.textAlign = 'right';
    sizeTd.textContent = `${(e.bytes / 1e9).toFixed(2)} GB`;
    tr.appendChild(sizeTd);

    const filesTd = document.createElement('td');
    filesTd.style.padding = '0.35rem 0.25rem';
    filesTd.style.textAlign = 'right';
    filesTd.textContent = e.files;
    tr.appendChild(filesTd);

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
      if (msg) msg.textContent = data.message || 'A full scan ran recently - please wait before running another.';
      return;
    }
    if (res.status === 409) {
      // blocked because indexing or another scan is running, not an error
      if (msg) msg.textContent = data.error || 'Busy - try again once the current operation finishes.';
      return;
    }
    if (!res.ok) {
      alert(data.error || 'Failed to start scan');
      return;
    }

    if (msg) msg.textContent = 'Full system scan started.';

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
    alert('Error starting SD scan');
  }
}

// ---------- Library Index scan scope (indexing, not disk usage) ----------

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

// polls /scan-status directly so it resolves promptly. two phases matter:
// /generate-media returns as soon as the index is QUEUED, the worker flips
// indexingInProgress a moment later. so wait for it to actually START, then finish -
// else we'd see both flags false and return early (the old "totals before index" bug)
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
    // not running and not queued -> done or never started. wait for the deadline so a slow worker still counts
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
        alert(data.error || 'Failed to start scan');
        return;
      }
      if (msg) msg.textContent = `Scan of ${dir} started. Check console for progress.`;
    } else {
      await generateMediaJson();
    }

    if (withSd) {
      // full SD scan, but only AFTER indexing finishes - never overlap them, thats
      // what (with the firmware's index/scan guard) keeps them off each other's SD/heap
      await waitForIndexingToFinish();
      if (msg) msg.textContent = 'Index complete - starting SD card scan...';
      await triggerSDScan();
    } else {
      setTimeout(() => { if (msg) msg.textContent = ''; }, 5000);
    }
  } catch (e) {
    console.error('Failed to start index scan:', e);
    if (msg) msg.textContent = 'Failed to start scan.';
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
  try {
    const saved = localStorage.getItem(THEME_KEY);
    const isDark = saved === 'true';
    applyThemeFlag(isDark);
  } catch (e) {
    applyThemeFlag(false);
  }
}

// Initialize theme
if (document.readyState === 'loading') {
  window.addEventListener('DOMContentLoaded', initTheme);
} else {
  initTheme();
}

// Sync theme across tabs
window.addEventListener('storage', (ev) => {
  if (ev.key === THEME_KEY) {
    const dark = ev.newValue === 'true';
    applyThemeFlag(dark);
  }
});

// SHA-256 helper
async function sha256Hex(str) {
  if (!window.crypto || !crypto.subtle) {
    console.warn('SubtleCrypto unavailable; sha256Hex will return raw string (less secure).');
    return str;
  }
  const enc = new TextEncoder();
  const data = enc.encode(str || '');
  const hash = await crypto.subtle.digest('SHA-256', data);
  const bytes = new Uint8Array(hash);
  return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('');
}

// session token from POST /auth/login, sent as X-Admin-Token on every state-changing
// request so the device enforces the password, not just this UI
let adminToken = sessionStorage.getItem('nomad_admin_token') || '';

function setAdminToken(token) {
  adminToken = token || '';
  if (adminToken) {
    sessionStorage.setItem('nomad_admin_token', adminToken);
  } else {
    sessionStorage.removeItem('nomad_admin_token');
  }
}

// Use for any request that changes device state (restart, settings, LEDs, etc).
// Attaches the admin session token and re-shows the auth overlay if the server
// rejects it (e.g. the device rebooted and the token no longer exists).
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
    const res = await fetch('/settings', { cache: 'no-store' });
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
    if (s.wifiPassword !== undefined) {
      document.getElementById('wifi-password').value = s.wifiPassword;
    }

    // brightness 0-100, clamp a stale value (old default was 230 -> showed 230%)
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
    const wifiPassword = document.getElementById('wifi-password').value;
    
    // Validate WiFi password length
    if (wifiPassword && wifiPassword.length < 8) {
      alert('WiFi password must be at least 8 characters long for captive portal compatibility.');
      return;
    }
    
    const settings = {
      rgbMode: ledMode,
      rgbColor: document.getElementById('led-color').value,
      wifiSSID: document.getElementById('ssid').value,
      wifiPassword: wifiPassword,
      brightness: parseInt(document.getElementById('brightness').value),
      autoGenerateMedia: document.getElementById('auto-generate').checked,
      flipScreen: document.getElementById('flip-screen').checked
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

  // Highlight active button
  if (buttons[mode]) {
    buttons[mode].classList.remove('btn-secondary');
    if (mode === 'off') buttons[mode].classList.add('btn-secondary');
    else if (mode === 'solid') buttons[mode].classList.add('btn-primary');
    else if (mode === 'rainbow') buttons[mode].classList.add('btn-success');
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
async function updateAdminPassword() {
  const newPw = document.getElementById('new-password').value;
  const confirmPw = document.getElementById('confirm-password').value;

  if (!newPw) {
    alert('Please enter a password');
    return;
  }

  if (newPw !== confirmPw) {
    alert('Passwords do not match');
    return;
  }

  try {
    const hashedPw = await sha256Hex(newPw);
    const settingsUpdate = {
      adminPassword: hashedPw
    };

    const res = await adminFetch('/settings', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({
        'body': JSON.stringify(settingsUpdate)
      })
    });

    if (res.ok) {
      alert('Admin password updated successfully');
      document.getElementById('new-password').value = '';
      document.getElementById('confirm-password').value = '';
      // server drops the token when the password changes, so drop ours and re-auth
      setAdminToken('');
      await loadSettings();
    } else {
      alert('Failed to update admin password');
    }
  } catch (e) {
    console.error('Error updating admin password:', e);
    alert('Error updating admin password');
  }
}

// WiFi settings
function updateWiFiSettings() {
  const wifiPassword = document.getElementById('wifi-password').value;
  
  // Validate WiFi password length
  if (wifiPassword && wifiPassword.length < 8) {
    alert('WiFi password must be at least 8 characters long for captive portal compatibility.');
    return;
  }
  
  saveSettings();
  alert('WiFi settings updated. Changes will take effect after restart.');
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
    document.getElementById('generate-msg').textContent = 'Starting full scan...';
    await adminFetch('/generate-media', { method: 'POST' });
    document.getElementById('generate-msg').textContent = 'Full scan initiated. Check console for progress.';
    setTimeout(() => {
      document.getElementById('generate-msg').textContent = '';
    }, 5000);
  } catch (e) {
    console.error('Failed to generate media:', e);
    document.getElementById('generate-msg').textContent = 'Failed to start scan.';
  }
}

// Auto-generate toggle
let isAutoGenerate = false;

// Console functionality
let consoleLines = [];
const maxConsoleLines = 100;

function addConsoleLog(message, type = 'info') {
  const timestamp = new Date().toLocaleTimeString();
  const line = `[${timestamp}] ${message}`;
  
  consoleLines.push({ line, type, timestamp: Date.now() });
  
  // Keep only the last maxConsoleLines
  if (consoleLines.length > maxConsoleLines) {
    consoleLines = consoleLines.slice(-maxConsoleLines);
  }
  
  updateConsoleDisplay();
}

function updateConsoleDisplay() {
  const console = document.getElementById('console-output');
  if (!console) return;

  console.innerHTML = consoleLines.map(({ line, type }) => {
    let color = '#00ff00'; // Default green
    let icon = '';

    if (type === 'error') {
      color = '#ff4444';
      icon = '❌ ';
    } else if (type === 'warning') {
      color = '#ffaa00';
      icon = '⚠️ ';
    } else if (type === 'info') {
      color = '#00aaff';
      icon = 'ℹ️ ';
    } else if (type === 'success') {
      color = '#00ff88';
      icon = '✅ ';
    } else if (type === 'system') {
      color = '#88aaff';
      icon = '🔧 ';
    }

    return `<div style="color: ${color}; margin-bottom: 2px;">${icon}${line}</div>`;
  }).join('');

  // Auto-scroll to bottom
  console.scrollTop = console.scrollHeight;
}

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
    addConsoleLog(`[${timestamp}] ❌ Failed to fetch system status: ${e.message}`, 'error');
  }
}

async function fetchConsoleLogs() {
  try {
    const res = await fetch('/console-logs');
    if (res.ok) {
      const data = await res.json();

      // Clear existing logs and add new ones
      consoleLines = [];

      if (data.logs && data.logs.length > 0) {
        data.logs.forEach(log => {
          // Convert timestamp to readable format
          const date = new Date(log.timestamp);
          const timeStr = date.toLocaleTimeString();
          const formattedMessage = log.message && log.message.startsWith('[') ?
            log.message : `[${timeStr}] ${log.message || ''}`;

          consoleLines.push({
            line: formattedMessage,
            type: log.type || 'info',
            timestamp: log.timestamp
          });
        });
      } else {
        // Show default message if no logs
        const timeStr = new Date().toLocaleTimeString();
        consoleLines.push({
          line: `[${timeStr}] System ready. Monitoring operations...`,
          type: 'info',
          timestamp: Date.now()
        });
      }

      updateConsoleDisplay();
    }
  } catch (e) {
    // Silently handle errors to avoid spam
  }
}

async function fetchSystemInfo() {
  try {
    const timestamp = new Date().toLocaleTimeString();

    // Fetch admin status for additional info
    const adminRes = await fetch('/admin-status');
    if (adminRes.ok) {
      const adminData = await adminRes.json();

      // Log connection info if users changed
      if (!window.lastUserCount || window.lastUserCount !== adminData.users) {
        const userChange = window.lastUserCount ?
          (adminData.users > window.lastUserCount ? 'connected' : 'disconnected') : 'detected';
        addConsoleLog(`[${timestamp}] 👥 User ${userChange}: ${adminData.users} active connection${adminData.users !== 1 ? 's' : ''}`, 'info');
        window.lastUserCount = adminData.users;
      }
    }

    // Fetch CPU temperature
    const tempRes = await fetch('/cpu-temp');
    if (tempRes.ok) {
      const tempData = await tempRes.json();

      // Log temperature warnings
      if (tempData.temp > 70) {
        addConsoleLog(`[${timestamp}] 🌡️ High CPU temperature detected: ${tempData.temp}°C`, 'warning');
      } else if (!window.lastTempLog || Date.now() - window.lastTempLog > 60000) {
        // Log temperature every minute
        addConsoleLog(`[${timestamp}] 🌡️ CPU temperature: ${tempData.temp}°C`, 'system');
        window.lastTempLog = Date.now();
      }
    }

  } catch (e) {
    // Silently handle errors for additional info to avoid spam
  }
}

// Admin bar functionality
async function fetchAdminBar() {
  try {
    const res = await fetch('/admin-status');
    const data = await res.json();

    document.getElementById('bar-ssid').textContent = data.ssid || '—';
    document.getElementById('bar-wifi-pass').textContent = data.wifiPassword || '—';
    document.getElementById('bar-users').textContent =
      typeof data.users === 'number' ? `${data.users}` : '0';

  } catch (e) {
    console.error('Could not load admin bar:', e);
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
    tempBtn.textContent = `🌡️ ${displayTemp.toFixed(1)} ${unit}`;

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
    console.debug('requireAdminAuth: admin password explicitly disabled on server — skipping auth.');
    overlay.classList.add('hidden');
    return;
  }

  // a cached token means a real server-verified session (only the server can validate it)
  if (adminToken) {
    console.debug('requireAdminAuth: cached session token present, skipping auth overlay.');
    overlay.classList.add('hidden');
    return;
  }

  // Show auth overlay if password is set and no valid session token
  console.debug('requireAdminAuth: admin password is set — showing auth overlay.');
  overlay.classList.remove('hidden');
  passwordInput.focus();

  // Handle authentication - only attach handlers once
  if (!submitBtn._authHandlerAttached) {
    const authenticate = async () => {
      const inputPw = passwordInput.value.trim();
      if (!inputPw) return;

      try {
        const hashedInput = await sha256Hex(inputPw);
        const res = await fetch('/auth/login', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({ 'body': JSON.stringify({ hash: hashedInput }) })
        });

        if (res.ok) {
          // Success - the server verified the password hash and issued a session token.
          const data = await res.json();
          setAdminToken(data.token);
          overlay.classList.add('hidden');
          if (errorDiv) errorDiv.classList.add('hidden');
          passwordInput.value = '';
          console.debug('requireAdminAuth: authentication success, overlay hidden.');
        } else {
          // Failure
          if (errorDiv) errorDiv.classList.remove('hidden');
          passwordInput.value = '';
          passwordInput.focus();
        }
      } catch (e) {
        console.error('Authentication error:', e);
        if (errorDiv) errorDiv.classList.remove('hidden');
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
  
  // Load settings and authenticate
  await loadSettings();
  
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
      if (!confirm('⚠️ Are you sure you want to restart the device?')) return;
      try {
        await adminFetch('/restart', { method: 'POST' });
        addConsoleLog('Restart command sent', 'info');
        alert('Restart command sent. The device will reboot shortly.');
      } catch {
        addConsoleLog('Device disconnected - restart in progress', 'warning');
        alert('Device Disconnected, Please Reconnect (Successful Reboot)');
      }
    });
  }

  const shutdownBtn = document.getElementById('btn-shutdown');
  if (shutdownBtn) {
    shutdownBtn.addEventListener('click', async () => {
      const ok = confirm('⚠️ Shut down Nomad safely?\n\nThis will unmount the SD card and enter deep sleep.');
      if (!ok) return;

      try {
        const res = await adminFetch('/shutdown', { method: 'POST' });
        if (res.ok) {
          addConsoleLog('Safe shutdown initiated', 'info');
          alert('Safe shutdown initiated.\nNomad will power down shortly.');
        } else {
          addConsoleLog('Shutdown command failed', 'error');
          alert('Shutdown completed successfully.');
        }
      } catch (err) {
        addConsoleLog('Device disconnected - shutdown in progress', 'warning');
        alert('Device disconnected — shutdown likely in progress.');
      }
    });
  }

  const usbBtn = document.getElementById('btn-usbmode');
  if (usbBtn) {
    usbBtn.addEventListener('click', async () => {
      const ok = confirm(
        '⚠️ This will restart the device into USB Transfer mode. It can take more than 60 seconds to mount\n\n' +
        'To exit USB mode, you must unplug and re-plug the device.\n\n' +
        'Proceed?'
      );
      if (!ok) return;
      try {
        await adminFetch('/enterUsb', { method: 'POST' });
        addConsoleLog('USB mode command sent', 'info');
        alert('USB-mode command sent. The device will reboot into USB MSC mode.');
      } catch {
        addConsoleLog('Web server closed - USB mode starting', 'warning');
        alert('Web server closed. Please check your computer in ~60 seconds.');
      }
    });
  }

  const flashBtn = document.getElementById('btn-flashmode');
  if (flashBtn) {
    flashBtn.addEventListener('click', async () => {
      const ok = confirm('⚠️ Enter flash mode? This will restart the device for firmware updates.');
      if (!ok) return;
      try {
        await adminFetch('/flash-mode', { method: 'POST' });
        addConsoleLog('Flash mode command sent', 'info');
        alert('Flash mode activated. Device will restart for firmware updates.');
      } catch {
        addConsoleLog('Flash mode activation failed', 'error');
        alert('Flash mode activated successfully. Device restarting...');
      }
    });
  }

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

  // Disable password button
  const disablePasswordBtn = document.getElementById('btn-disable-password');
  if (disablePasswordBtn) {
    disablePasswordBtn.addEventListener('click', async () => {
      if (!confirm('Are you sure you want to disable the admin password?')) return;
      
      try {
        const settingsUpdate = {
          adminPassword: ""
        };

        const res = await adminFetch('/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: new URLSearchParams({
            'body': JSON.stringify(settingsUpdate)
          })
        });

        if (res.ok) {
          alert('Admin password disabled successfully');
          setAdminToken('');
          location.reload();
        } else {
          alert('Failed to disable admin password');
        }
      } catch (e) {
        console.error('Error disabling admin password:', e);
        alert('Error disabling admin password');
      }
    });
  }

  // LCD toggle button
  const lcdToggleBtn = document.getElementById('lcd-toggle');
  if (lcdToggleBtn) {
    lcdToggleBtn.addEventListener('click', async () => {
      try {
        // Get current brightness value
        const brightnessElement = document.getElementById('brightness');
        const currentBrightness = parseInt(brightnessElement.value);

        // If brightness is 0, turn it back on to previous value or default
        if (currentBrightness === 0) {
          const stored = localStorage.getItem('lcd_brightness');
          const newBrightness = stored !== null ? parseInt(stored) : 100;
          await setBrightness(newBrightness);
          brightnessElement.value = newBrightness;
          updateBrightnessLabel(newBrightness);
        } else {
          // Turn off by setting brightness to 0 (store current)
          localStorage.setItem('lcd_brightness', currentBrightness);
          await setBrightness(0);
          brightnessElement.value = 0;
          updateBrightnessLabel(0);
        }
      } catch (e) {
        console.error('Failed to toggle LCD:', e);
      }
    });
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


function checkScreenSize() {
  const fileBrowserSection = document.querySelector('.file-browser-section');
  if (fileBrowserSection) {
    if (window.innerWidth <= 768) {
      fileBrowserSection.style.display = 'none';
    } else {
      fileBrowserSection.style.display = 'block';
    }
  }
}

window.addEventListener('resize', checkScreenSize);
window.addEventListener('load', checkScreenSize);
