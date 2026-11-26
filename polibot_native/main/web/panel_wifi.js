// The list of Wi-Fi networks nearby: one look each time Scan Now is pressed; picking one fills the form
const SCAN_ANSWERS = 10; // a look takes a couple of seconds: its result is asked for once a second, at most this often

const wifiSignal = rssi => rssi >= -60 ? 'excellent' : rssi >= -70 ? 'good' : rssi >= -80 ? 'fair' : 'weak';

function pickWifi(network) {
  const name = document.getElementById('wifi-ssid');
  const password = document.getElementById('wifi-password');
  const saved = currentConfig.wifi || {};
  name.value = network.ssid;
  // the masked password stands for the stored one, which belongs to the saved network only
  password.value = network.ssid === saved.ssid ? saved.password || '' : '';
  password.placeholder = network.secure ? 'Password of ' + network.ssid : 'Open network: leave empty';
  markWifi();
  if (network.secure && !password.value) password.focus();
}

function markWifi() {
  const chosen = document.getElementById('wifi-ssid').value;
  document.querySelectorAll('#wifi-networks .btn').forEach(b => b.setAttribute('aria-pressed', String(b.dataset.ssid === chosen)));
}

function wifiRow(network) {
  const row = document.createElement('button');
  row.type = 'button';
  row.className = 'btn';
  row.dataset.ssid = network.ssid;
  row.dataset.icon = network.secure ? 'lock' : 'wifi';
  const name = document.createElement('span');
  name.textContent = network.ssid; // names come from anybody's router: never read as HTML
  const detail = document.createElement('small');
  detail.textContent = `${wifiSignal(network.rssi)} · ${network.rssi} dBm${network.secure ? '' : ' · open'}`;
  row.append(name, detail);
  row.addEventListener('click', () => pickWifi(network));
  return row;
}

function showNetworks(networks) {
  const list = document.getElementById('wifi-networks');
  list.replaceChildren(...networks.map(wifiRow));
  markWifi();
  return networks.length ? `Found ${networks.length}. Not there? Press Scan Now again, or type the name.`
    : 'No networks found (this device uses 2.4 GHz only). Press Scan Now again, or type the name.';
}

async function scanOnce() {
  let res = await fetch('/api/wifi/scan', { method: 'POST', headers: getAuthHeader() });
  for (let answers = 1; ; answers++) {
    if (res.status === 401) return sessionExpired();
    if (res.status === 403) return 'Listing networks needs the admin role. You can still type the name.';
    const scan = await res.json();
    if (scan.problem) return `${scan.problem} Try again in a moment, or type the name.`;
    if (!scan.scanning) return showNetworks(scan.networks);
    if (answers >= SCAN_ANSWERS) return 'No result yet. Press Scan Now again.';
    await sleep(1000);
    res = await fetch('/api/wifi/scan', { headers: pollHeaders(true) });
  }
}

async function scanWifi() {
  const button = document.getElementById('wifi-scan');
  const status = document.getElementById('wifi-scan-status');
  button.disabled = true;
  status.textContent = 'Looking…';
  try {
    status.textContent = await scanOnce() || '';
  } catch(e) {
    status.textContent = 'The device did not answer. Press Scan Now again.';
  } finally {
    button.disabled = false;
  }
}

document.getElementById('wifi-ssid').addEventListener('input', markWifi);
