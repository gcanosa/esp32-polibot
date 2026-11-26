// WiFi and time, device actions, console, status LED, firmware update and backup
async function saveConfig() {
  // Apply the form on top of the device's current config, so changes made elsewhere (IRC, other tabs) survive
  const fresh = await fetch('/api/config', { headers: getAuthHeader() });
  if (!fresh.ok) return alert('Could not read the current configuration.');
  currentConfig = await fresh.json();
  currentConfig.wifi = currentConfig.wifi || {};

  currentConfig.wifi.ssid = document.getElementById('wifi-ssid').value;
  currentConfig.wifi.password = document.getElementById('wifi-password').value;
  currentConfig.wifi.ap_fallback = document.getElementById('wifi-ap-fallback').value === 'true';
  currentConfig.wifi.ap_ssid = document.getElementById('wifi-ap-ssid').value;
  currentConfig.wifi.ap_password = document.getElementById('wifi-ap-pw').value;

  if (!currentConfig.ntp) currentConfig.ntp = {};
  currentConfig.ntp.enabled = document.getElementById('ntp-enabled').value === 'true';
  currentConfig.ntp.server = document.getElementById('ntp-server').value.trim() || 'pool.ntp.org';
  currentConfig.ntp.timezone = document.getElementById('ntp-timezone').value.trim();
  currentConfig.ntp.location = document.getElementById('ntp-location').value.trim();

  // IRC networks, rules and behaviour are saved from their own tabs
  const { wifi, ntp } = currentConfig;
  const res = await fetch('/api/config', { method: 'POST', headers: authJson(), body: JSON.stringify({ wifi, ntp }) });
  if (res.ok) {
    showToast('Configuration saved & rehashed!');
    fetchStatus();
    renderChannelsUI();
  } else {
    const data = await res.json().catch(() => ({}));
    alert(data.message || 'Failed to save configuration. Check permissions.');
  }
  return res.ok;
}

async function rehashConfig() {
  await fetch('/api/action', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...getAuthHeader() },
    body: JSON.stringify({ action: 'rehash' })
  });
  showToast('Rehashed successfully!');
  fetchStatus();
}

async function reconnectIRC() {
  await fetch('/api/action', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...getAuthHeader() },
    body: JSON.stringify({ action: 'reconnect' })
  });
  showToast('Reconnection requested.');
  fetchStatus();
}

async function rebootESP() {
  if (confirm('Reboot the ESP32-S3 Sentry device?')) {
    await fetch('/api/action', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...getAuthHeader() },
      body: JSON.stringify({ action: 'reboot' })
    });
    alert('Device is rebooting. Web panel will reload shortly.');
  }
}

async function sendConsoleCmd() {
  const inp = document.getElementById('cmd-input');
  const val = inp.value.trim();
  if (!val) return;
  inp.value = '';
  await fetch('/api/action', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...getAuthHeader() },
    body: JSON.stringify({ action: 'send_irc', cmd: val, network: document.getElementById('console-network').value })
  });
  setTimeout(refreshLogs, 400);
}

document.getElementById('cmd-input').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') sendConsoleCmd();
});

const LED_TEXT = { no_wifi: 'no WiFi', no_irc: 'no IRC network connected', some_irc: 'some IRC networks connected', all_irc: 'all IRC networks connected' };

// Mirrors the LED on the board, with its pattern (led_colour in status_led.h); the browser does the blinking
const LED_USERS = { panel: ', web panel signed in', visitor: ', status page open' };
const stillLed = matchMedia('(prefers-reduced-motion: reduce)');
let ledState = null;

function drawLed() {
  if (!ledState) return;
  const cycle = ledState.period * 10;
  const at = Math.floor(Date.now() / 100) % cycle;
  const flash = ledState.web && at >= 7 && at + 2 < cycle && (at - 5) % 2 === 0;
  const colour = !ledState.on ? null : stillLed.matches || at < 5 ? ledState.colour : flash ? ledState.blink_colour : null;
  document.getElementById('led-dot').style.setProperty('--led', colour || 'var(--led-off)');
}

function showLed(led) {
  if (!led) return;
  ledState = led;
  const dot = document.getElementById('led-dot');
  dot.title = `Status LED${led.on ? '' : ' (off)'}: ${LED_TEXT[led.state] || ''}${LED_USERS[led.web] || ''}`;
  dot.setAttribute('aria-label', dot.title);
  drawLed();
}
setInterval(drawLed, 100);

// ---------------- Settings backup ----------------
async function downloadBackup() {
  const res = await fetch('/api/backup', { headers: getAuthHeader() });
  if (res.status === 401) return sessionExpired();
  if (!res.ok) return alert('Only an owner can download the settings.');
  const link = document.createElement('a');
  link.href = URL.createObjectURL(await res.blob());
  link.download = `polibot-backup-${new Date().toISOString().slice(0, 10)}.json`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 1000);
}

async function restoreBackup() {
  const file = document.getElementById('restore-file').files[0];
  if (!file) return alert('Choose a backup file first.');
  let backup;
  try { backup = JSON.parse(await file.text()); } catch (e) { return alert('That file is not a settings backup.'); }
  if (!confirm(`Replace all settings and accounts with '${file.name}'? The device restarts.`)) return;
  const keep_wifi = document.getElementById('restore-keep-wifi').checked;
  const res = await fetch('/api/restore', { method: 'POST', headers: authJson(), body: JSON.stringify({ backup, keep_wifi }) });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) return alert(data.message || 'Could not restore the settings.');
  setOtaBusy(true);
  waitForReboot();
}

// ---------------- Status LED settings ----------------
const LED_FIELDS = ['no_wifi', 'no_irc', 'some_irc', 'all_irc', 'panel', 'visitor'];

let ledDefaults = null;

function fillLedSettings(values) {
  if (!values) return;
  LED_FIELDS.forEach(k => document.getElementById('led-' + k).value = values[k]);
  document.getElementById('led-enabled').value = String(values.enabled);
  document.getElementById('led-period').value = values.period;
  document.getElementById('led-brightness').value = values.brightness;
  document.getElementById('led-brightness-value').value = values.brightness;
}

async function loadLedSettings() {
  const res = await fetch('/api/led', { headers: getAuthHeader() });
  if (res.status === 401) return sessionExpired();
  if (!res.ok) return;
  const settings = await res.json();
  ledDefaults = settings.defaults;
  fillLedSettings(settings);
}

async function saveLedSettings() {
  const body = {
    enabled: document.getElementById('led-enabled').value === 'true',
    period: Number(document.getElementById('led-period').value),
    brightness: Number(document.getElementById('led-brightness').value)
  };
  LED_FIELDS.forEach(k => body[k] = document.getElementById('led-' + k).value);
  const res = await fetch('/api/led', { method: 'POST', headers: authJson(), body: JSON.stringify(body) });
  if (res.status === 401) return sessionExpired();
  const data = await res.json().catch(() => ({}));
  if (!res.ok) return alert(data.message || 'Could not save the LED settings.');
  showToast(data.message);
  fetchStatus();
}

// ---------------- Firmware update (OTA) ----------------
let otaBusy = false; // pauses background polling while the device is flashing

const sleep = ms => new Promise(r => setTimeout(r, ms));

function setOta(message, percent) {
  document.getElementById('ota-status').innerText = message;
  if (percent !== undefined) document.getElementById('ota-progress').value = percent;
}

function setOtaBusy(busy) {
  otaBusy = busy;
  document.querySelectorAll('.ota-btn').forEach(b => b.disabled = busy);
}

async function loadOtaStatus() {
  const info = document.getElementById('ota-info');
  try {
    const res = await fetch('/api/ota/status', { headers: getAuthHeader() });
    if (!res.ok) {
      info.innerText = 'Firmware updates require an owner account.';
      return null;
    }
    const s = await res.json();
    info.innerText = `Version ${s.app_version} (built ${s.compile_time})\nRunning: ${s.running_partition} | Next update goes to: ${s.next_partition} (max ${Math.floor(s.max_image_size / 1024)} KB)`;
    if (s.in_progress && !otaBusy) pollOta(); // started elsewhere (IRC or another tab)
    return s;
  } catch(e) {
    return null;
  }
}

async function waitForReboot() {
  setOta('Rebooting... this page reloads when the device is back (you will need to sign in again).', 100);
  await sleep(8000);
  for (;;) {
    try {
      const res = await fetch('/api/system', { cache: 'no-store', signal: AbortSignal.timeout(3000) });
      if (res.ok) break;
    } catch(e) {}
    await sleep(2000);
  }
  location.reload();
}

function otaUpload() {
  const file = document.getElementById('ota-file').files[0];
  if (!file) return alert('Choose a .bin firmware file first.');
  if (!confirm(`Flash '${file.name}' (${Math.round(file.size / 1024)} KB)? The device will reboot.`)) return;

  setOtaBusy(true);
  setOta('Preparing flash...', 0);
  const xhr = new XMLHttpRequest(); // fetch() cannot report upload progress
  xhr.open('POST', '/api/ota/upload');
  Object.entries(getAuthHeader()).forEach(([k, v]) => xhr.setRequestHeader(k, v));
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = e => {
    const pct = Math.floor(e.loaded * 100 / e.total);
    setOta(pct < 100 ? `Uploading & flashing: ${pct}%` : 'Verifying image...', pct);
  };
  xhr.onload = () => {
    let msg = 'HTTP ' + xhr.status;
    try { msg = JSON.parse(xhr.responseText).message || msg; } catch(e) {}
    if (xhr.status === 200) return waitForReboot();
    setOtaBusy(false);
    setOta('Update failed: ' + msg, 0);
  };
  xhr.onerror = () => {
    setOtaBusy(false);
    setOta('Update failed: the device closed the connection (check the image, your login, and the logs).', 0);
  };
  xhr.send(file);
}

async function otaFromUrl() {
  const url = document.getElementById('ota-url').value.trim();
  if (!url) return alert('Enter a firmware URL first.');
  if (!confirm(`Download and flash firmware from ${url}? The device will reboot.`)) return;

  setOtaBusy(true);
  setOta('Starting download...', 0);
  try {
    const res = await fetch('/api/ota/url', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...getAuthHeader() },
      body: JSON.stringify({ url: url })
    });
    const data = await res.json();
    if (!res.ok) throw new Error(data.message || 'Request rejected.');
    pollOta();
  } catch(e) {
    setOtaBusy(false);
    setOta('Update failed: ' + e.message, 0);
  }
}

async function pollOta() {
  setOtaBusy(true);
  let failures = 0;
  while (failures < 5) {
    await sleep(1000);
    const s = await loadOtaStatus();
    if (!s) { failures++; continue; }
    failures = 0;
    setOta(s.status_message, s.progress_percent);
    if (s.requires_reboot) return waitForReboot();
    if (!s.in_progress) return setOtaBusy(false);
  }
  setOtaBusy(false);
  setOta('Lost contact with the device. Reload the page to check the result.');
}

function onTimezonePresetChange() {
  const sel = document.getElementById('ntp-preset').value;
  if (sel !== 'custom') {
    const parts = sel.split('|');
    document.getElementById('ntp-location').value = parts[0];
    document.getElementById('ntp-timezone').value = parts[1];
  }
}

async function saveAndSyncNtp() {
  if (!await saveConfig()) return;
  alert('NTP & Timezone settings saved! Synchronizing...');
  setTimeout(fetchStatus, 1500);
}
