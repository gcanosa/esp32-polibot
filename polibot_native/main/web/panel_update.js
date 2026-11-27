// Online updates: what the device found out about newer releases, and the notice that there is one
function showUpdateBanner(version) {
  document.getElementById('update-banner').hidden = !version;
  document.getElementById('update-banner-text').innerText = version ? `Version ${version} is available.` : '';
}

const openUpdates = () => document.getElementById('tab-ota-button').click();

function timeAgo(seconds) {
  if (seconds < 90) return 'just now';
  if (seconds < 5400) return `${Math.round(seconds / 60)} minutes ago`;
  return `${Math.round(seconds / 3600)} hours ago`;
}

function showUpdateState(u) {
  const checked = u.checked_ago >= 0 ? ` Checked ${timeAgo(u.checked_ago)}.` : '';
  document.getElementById('update-state').innerText = u.checking ? 'Checking...' : u.message + checked;
  const offer = u.available;
  document.getElementById('update-offer').hidden = !offer;
  document.getElementById('update-notes').innerText = offer
    ? `Version ${offer.version} (${Math.round(offer.size / 1024)} KB), you run ${u.current}. ${offer.notes}` : '';
  showUpdateBanner(offer?.version);
}

async function fetchUpdates() {
  try {
    const res = await fetch('/api/update', { headers: getAuthHeader() });
    if (res.ok) return await res.json();
  } catch (e) {
    console.error(e);
  }
  return null;
}

// When the tab is opened: the state and the settings. Later refreshes leave what is being typed alone.
async function loadUpdates() {
  const u = await fetchUpdates();
  if (!u) {
    document.getElementById('update-state').innerText = 'Online updates require an owner account.';
    return;
  }
  document.getElementById('update-auto').checked = u.auto_check;
  document.getElementById('update-hours').value = u.check_hours;
  showUpdateState(u);
}

async function postUpdate(body) {
  const res = await fetch('/api/update', { method: 'POST', headers: authJson(), body: JSON.stringify(body) });
  if (res.status === 401) { sessionExpired(); return false; }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) { alert(data.message || 'Request failed.'); return false; }
  return data.message || true;
}

async function checkForUpdate() {
  if (!await postUpdate({ action: 'check' })) return;
  for (let tries = 0; tries < 40; tries++) {
    const u = await fetchUpdates();
    if (u) showUpdateState(u);
    if (u && !u.checking) return;
    await sleep(1000);
  }
}

async function saveUpdateSettings() {
  const hours = Number(document.getElementById('update-hours').value);
  if (!Number.isInteger(hours) || hours < 1 || hours > 720) return alert('Enter a whole number of hours, from 1 to 720.');
  const message = await postUpdate({ auto_check: document.getElementById('update-auto').checked, check_hours: hours });
  if (message) showToast(message);
}

async function installUpdate() {
  if (!confirm('Download and install the new version? The device will reboot. Settings are kept.')) return;
  setOtaBusy(true);
  setOta('Starting download...', 0);
  if (await postUpdate({ action: 'install' })) return pollOta();
  setOtaBusy(false);
  setOta('Idle', 0);
}
