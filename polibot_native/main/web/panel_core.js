// Sign-in, tabs, status and logs. Loaded last: it starts the panel.
let currentConfig = {};

let latestStatus = {};

let currentUser = null;

function getAuthHeader() {
  const token = localStorage.getItem('polibot_token') || '';
  return token ? { 'Authorization': 'Bearer ' + token } : {};
}

function switchTab(tabId) {
  document.querySelectorAll('.tab-content').forEach(el => el.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
  document.getElementById(tabId).classList.add('active');
  event.target.classList.add('active');
}

function showToast(msg) {
  const t = document.getElementById('toast');
  t.innerText = msg;
  t.style.display = 'block';
  setTimeout(() => { t.style.display = 'none'; }, 3000);
}

async function checkAuth() {
  const token = localStorage.getItem('polibot_token');
  if (!token) {
    document.getElementById('login-modal').style.display = 'flex';
    return false;
  }
  try {
    const res = await fetch('/api/me', { headers: getAuthHeader() });
    if (res.ok) {
      const me = await res.json();
      if (me.must_change_password) {
        askNewPassword('');
        return false;
      }
      currentUser = me;
      showProxyHint(me.proxy);
      document.getElementById('login-modal').style.display = 'none';
      const roleStr = currentUser.global_role.toUpperCase();
      document.getElementById('user-profile-badge').innerText = `${currentUser.username} · ${roleStr}`;
      return true;
    } else {
      localStorage.removeItem('polibot_token');
      document.getElementById('login-modal').style.display = 'flex';
      return false;
    }
  } catch(e) {
    document.getElementById('login-modal').style.display = 'flex';
    return false;
  }
}

// An account on the default password can do nothing else until it has another one
function askNewPassword(current) {
  currentUser = null;
  document.getElementById('login-modal').style.display = 'none';
  document.getElementById('password-modal').style.display = 'flex';
  document.getElementById('force-current').value = current;
  document.getElementById(current ? 'force-new' : 'force-current').focus();
}

async function forcePasswordChange() {
  const [current, password, repeat] = ['force-current', 'force-new', 'force-repeat'].map(id => document.getElementById(id).value);
  const fail = message => {
    const box = document.getElementById('force-err');
    box.innerText = message;
    box.style.display = 'block';
  };
  if (!current || !password) return fail('Enter your current password and a new one.');
  if (password !== repeat) return fail('The new passwords do not match.');
  const res = await fetch('/api/password', { method: 'POST', headers: authJson(), body: JSON.stringify({ current, password }) });
  if (res.status === 401) { document.getElementById('password-modal').style.display = 'none'; return sessionExpired(); }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) return fail(data.message || 'Could not change the password.');
  location.reload(); // signed in already, now without restrictions
}

// The device forgets sessions when it restarts, the password changes or they sit idle
function sessionExpired() {
  localStorage.removeItem('polibot_token');
  currentUser = null;
  document.getElementById('user-profile-badge').innerText = 'Not signed in';
  document.querySelectorAll('dialog').forEach(d => d.close()); // they would cover the sign-in form
  document.getElementById('login-modal').style.display = 'flex';
}

async function performLogin() {
  const u = document.getElementById('login-username').value.trim();
  const p = document.getElementById('login-password').value;
  const errBox = document.getElementById('login-err');
  errBox.style.display = 'none';
  if (!u || !p) {
    errBox.innerText = 'Please enter both username and password.';
    errBox.style.display = 'block';
    return;
  }
  try {
    const res = await fetch('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: u, password: p })
    });
    const data = await res.json();
    if (res.ok && data.token) {
      localStorage.setItem('polibot_token', data.token);
      if (data.must_change_password) return askNewPassword(p);
      document.getElementById('login-modal').style.display = 'none';
      currentUser = data.user;
      document.getElementById('user-profile-badge').innerText = `${currentUser.username} · ${currentUser.global_role.toUpperCase()}`;
      showToast('Logged in successfully!');
      initApp();
    } else {
      errBox.innerText = data.message || 'Login failed.';
      errBox.style.display = 'block';
    }
  } catch(e) {
    errBox.innerText = 'Connection error: ' + e;
    errBox.style.display = 'block';
  }
}

function performLogout() {
  const token = localStorage.getItem('polibot_token');
  if (token) {
    fetch('/api/logout', { method: 'POST', headers: getAuthHeader() });
  }
  localStorage.removeItem('polibot_token');
  location.reload();
}

// A refresh made by the panel itself does not count as use, so an unattended session still expires
const pollHeaders = poll => poll ? { ...getAuthHeader(), 'X-Poll': '1' } : getAuthHeader();

// The sign-in screen shows the version and the LED; both are public
async function showDeviceInfo() {
  try {
    const info = await (await fetch('/api/system', { cache: 'no-store' })).json();
    document.querySelectorAll('.app-version').forEach(el => el.innerText = info.version ? 'v' + info.version : '');
    showLed(info.led);
  } catch (e) {
    console.error(e);
  }
}

async function fetchStatus(poll = false) {
  try {
    const res = await fetch('/api/status', { headers: pollHeaders(poll) });
    if (res.status === 401) return sessionExpired();
    latestStatus = await res.json();
    const nets = latestStatus.networks || [];
    const up = nets.filter(n => n.connected).length;
    document.getElementById('stat-irc').innerHTML = up && up === nets.length ? '<span class="status-online">● ONLINE</span>'
      : up ? `<span class="status-online">◐ ${up}/${nets.length} ONLINE</span>` : '<span class="status-offline">○ OFFLINE</span>';
    document.getElementById('stat-server').innerText = nets.map(n => `${n.connected ? '●' : '○'} ${n.name}: ${n.host}:${n.port} (${n.nick})`).join('\n') || 'No networks configured';
    document.querySelectorAll('.app-version').forEach(el => el.innerText = latestStatus.version ? 'v' + latestStatus.version : '');
    const wifi = latestStatus.wifi || {};
    document.getElementById('wifi-status').innerText = (wifi.connected ? `Connected. IP address: ${wifi.ip}` : 'Not connected to a WiFi network.')
      + (wifi.ap_active ? ' The setup access point is open at 192.168.4.1.' : '');
    showLed(latestStatus.led);
    showUpdateBanner(latestStatus.update);
    showReportStatus();
    document.getElementById('stat-uptime').innerText = latestStatus.uptime;
    document.getElementById('stat-ram').innerText = latestStatus.free_ram;
    if (latestStatus.current_time) {
      document.getElementById('stat-time').innerText = latestStatus.current_time;
      const syncBadge = latestStatus.ntp_synced ? '● NTP Synced' : '○ NTP Syncing...';
      document.getElementById('stat-tz').innerText = (latestStatus.location || '') + ' (' + syncBadge + ')';
    }
    
    const several = nets.length > 1;
    document.getElementById('stat-channels').innerHTML = (latestStatus.channels_detail || [])
      .map(c => `<span class="pill">${several ? esc(c.network) + ' ' : ''}${esc(c.name)}</span>`).join('')
      || '<span style="color:var(--dim)">None</span>';
  } catch(e) {
    console.error(e);
  }
}

async function loadConfig() {
  try {
    const res = await fetch('/api/config', { headers: getAuthHeader() });
    if (res.status === 401) return sessionExpired();
    currentConfig = await res.json();
    
    renderNetworkSelectors();

    loadForms();
    renderAkicks();

    // WiFi
    const w = currentConfig.wifi || {};
    document.getElementById('wifi-ssid').value = w.ssid || '';
    document.getElementById('wifi-password').value = w.password || '';
    document.getElementById('wifi-ap-fallback').value = String(w.ap_fallback ?? true);
    document.getElementById('wifi-ap-ssid').value = w.ap_ssid || '';
    document.getElementById('wifi-ap-pw').value = w.ap_password || '';

    // NTP
    const n = currentConfig.ntp || {};
    document.getElementById('ntp-enabled').value = String(n.enabled ?? true);
    document.getElementById('ntp-server').value = n.server || 'pool.ntp.org';
    document.getElementById('ntp-timezone').value = n.timezone || 'CET-1CEST,M3.5.0,M10.5.0/3';
    document.getElementById('ntp-location').value = n.location || 'Europe/Madrid';

    renderChannelsUI();
    loadUsers();
    loadLedSettings();
  } catch(e) {
    console.error(e);
  }
}

const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const authJson = () => ({ 'Content-Type': 'application/json', ...getAuthHeader() });

async function initApp() {
  showDeviceInfo();
  const authed = await checkAuth();
  if (authed) {
    fetchStatus();
    refreshLogs();
    loadConfig();
  }
}

// nothing is asked while nobody can see the page
const polling = () => currentUser && !otaBusy && !document.hidden;

setInterval(() => { if (polling()) fetchStatus(true); }, 3000);

setInterval(() => { if (polling()) refreshLogs(true); }, 2500);

document.addEventListener('visibilitychange', () => { if (polling()) { fetchStatus(true); refreshLogs(true); } });

renderForms();

initApp();
