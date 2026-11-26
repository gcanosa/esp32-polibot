// IRC networks, their report channels, and the guarded channels
const MAX_NETWORKS = 4;

const CHANNEL_HELP = 'Channel names cannot contain spaces, commas, quotes or backslashes.';

const withHash = c => /^[#&]/.test(c) ? c : '#' + c;

const validChannel = c => !/[\s,'"<>`\\]/.test(c);

const networks = () => currentConfig.irc?.networks || [];

let editingNetwork; // name shown in the IRC editor; null while adding a new one

let shownChannels = ''; // what the editor's channel field was filled with

function renderNetworkSelectors() {
  if (editingNetwork === undefined || (editingNetwork !== null && !networks().some(n => n.name === editingNetwork))) {
    editingNetwork = networks()[0]?.name ?? null;
  }
  const options = networks().map(n => `<option value="${esc(n.name)}">${esc(n.name)} (${esc(n.host)})</option>`).join('');
  const editor = document.getElementById('irc-net-select');
  editor.innerHTML = options + (editingNetwork === null ? '<option value="">(new network)</option>' : '');
  editor.value = editingNetwork ?? '';
  for (const id of ['console-network', 'new-channel-network']) {
    const select = document.getElementById(id);
    const previous = select.value;
    select.innerHTML = options;
    if (networks().some(n => n.name === previous)) select.value = previous;
  }
  fillNetworkEditor();
}

function fillNetworkEditor() {
  const net = networks().find(n => n.name === editingNetwork)
    || { port: 6667, nick: 'PoliBOT', ident: 'polibot', realname: 'ESP32-S3 Guardian Sentry' };
  const set = (id, value) => { document.getElementById(id).value = value; };
  set('irc-name', net.name ?? '');
  set('irc-host', net.host ?? '');
  set('irc-port', net.port ?? 6667);
  set('irc-ssl', String(net.ssl ?? false));
  set('irc-ssl-verify', String(net.ssl_verify ?? true));
  set('irc-nick', net.nick ?? 'PoliBOT');
  set('irc-alt-nick', net.alt_nick ?? '');
  set('irc-ident', net.ident ?? 'polibot');
  set('irc-realname', net.realname ?? '');
  set('irc-account', net.account ?? '');
  set('irc-nickserv', net.nickserv_password ?? '');
  shownChannels = (net.channels || []).join(', ');
  set('irc-channels', shownChannels);
  set('irc-prefix', currentConfig.irc?.cmd_prefix || '!');
  set('irc-bot-mode', String(net.bot_mode ?? false));
  set('irc-report', net.report_channel ?? '');
  set('irc-report-key', net.report_key ?? '');
  set('irc-quiet', net.quiet_mode ?? '+q {mask}');
  set('irc-ops', net.ops_command ?? 'PRIVMSG ChanServ :OP {channel}');
  set('irc-invite', net.invite_command ?? 'PRIVMSG ChanServ :INVITE {channel}');
  set('irc-unban', net.unban_command ?? 'PRIVMSG ChanServ :UNBAN {channel}');
  set('irc-perform', (net.perform || []).join('\n'));
  showReportStatus();
  document.getElementById('irc-name').readOnly = editingNetwork !== null; // channel rules are stored by network name
  document.getElementById('irc-delete').disabled = editingNetwork === null;
  document.getElementById('irc-add').disabled = networks().length >= MAX_NETWORKS;
}

function selectNetwork(name) {
  editingNetwork = name || null;
  renderNetworkSelectors();
}

function newNetwork() {
  editingNetwork = null;
  renderNetworkSelectors();
  document.getElementById('irc-name').focus();
}

// Changes a fresh copy of the config and posts only the IRC section; the bot applies it at once
async function updateIrc(change) {
  const fresh = await fetch('/api/config', { headers: getAuthHeader() });
  if (!fresh.ok) { alert('Could not read the current configuration.'); return false; }
  const irc = (await fresh.json()).irc;
  irc.networks = irc.networks || [];
  if (change(irc) === false) return false;
  const res = await fetch('/api/config', { method: 'POST', headers: authJson(), body: JSON.stringify({ irc }) });
  if (!res.ok) {
    const data = await res.json().catch(() => ({}));
    alert(data.message || 'Failed to save. Administrator permission is required.');
    return false;
  }
  await loadConfig();
  setTimeout(async () => { await fetchStatus(); renderChannelsUI(); }, 2500);
  return true;
}

async function saveNetwork() {
  const val = id => document.getElementById(id).value.trim();
  const net = {
    name: val('irc-name'), host: val('irc-host'), port: parseInt(val('irc-port'), 10),
    ssl: val('irc-ssl') === 'true', ssl_verify: val('irc-ssl-verify') === 'true',
    nick: val('irc-nick') || 'PoliBOT', alt_nick: val('irc-alt-nick'), ident: val('irc-ident') || 'polibot', realname: val('irc-realname'),
    account: val('irc-account'),
    bot_mode: val('irc-bot-mode') === 'true',
    nickserv_password: document.getElementById('irc-nickserv').value,
    channels: val('irc-channels').split(',').map(s => s.trim()).filter(Boolean).map(withHash),
    report_channel: val('irc-report') ? withHash(val('irc-report')) : '',
    report_key: document.getElementById('irc-report-key').value,
    quiet_mode: val('irc-quiet'), ops_command: val('irc-ops'), invite_command: val('irc-invite'), unban_command: val('irc-unban'),
    perform: val('irc-perform').split('\n').map(s => s.trim()).filter(Boolean)
  };
  if (!/^[A-Za-z0-9_.-]{1,32}$/.test(net.name)) return alert('Network name: 1-32 letters, digits, ".", "-" or "_".');
  if (!net.host) return alert('Server host is required.');
  if (!(net.port >= 1 && net.port <= 65535)) return alert('Port must be between 1 and 65535.');
  if (!net.channels.every(validChannel)) return alert(CHANNEL_HELP);
  if (net.report_channel && !validChannel(net.report_channel)) return alert(CHANNEL_HELP);
  if (net.report_channel && !net.report_key) return alert('Choose a password for the report channel.');
  const isNew = editingNetwork === null;
  // An untouched channel field leaves the device's list alone: it may have changed since (!join, !part)
  if (!isNew && val('irc-channels') === shownChannels.trim()) delete net.channels;
  const prefix = val('irc-prefix') || '!';
  const ok = await updateIrc(irc => {
    const at = irc.networks.findIndex(n => n.name.toLowerCase() === net.name.toLowerCase());
    if (isNew && at >= 0) { alert('A network with that name already exists.'); return false; }
    if (at >= 0) irc.networks[at] = { ...irc.networks[at], ...net };
    else irc.networks.push(net);
    irc.cmd_prefix = prefix;
  });
  if (ok) {
    editingNetwork = net.name;
    renderNetworkSelectors();
    showToast(`Saved network ${net.name}`);
  }
}

async function deleteNetwork() {
  const name = editingNetwork;
  if (name === null || !confirm(`Delete network ${name}? The bot disconnects from it.`)) return;
  if (await updateIrc(irc => { irc.networks = irc.networks.filter(n => n.name !== name); })) showToast(`Deleted network ${name}`);
}

function renderChannelsUI() {
  const container = document.getElementById('channels-container');
  container.innerHTML = '';

  const chanSettings = currentConfig.guardian?.channel_settings || {};
  const statusChans = latestStatus.channels_detail || [];

  if (!networks().some(n => (n.channels || []).length)) {
    container.innerHTML = '<div style="color:var(--dim)">No channels configured. Add one above.</div>';
    return;
  }

  networks().forEach(net => (net.channels || []).forEach(ch => {
    const chLower = ch.toLowerCase();
    const key = `${net.name.toLowerCase()}/${chLower}`;
    const id = name => `${name}-${esc(key)}`;
    const stat = statusChans.find(c => c.network === net.name && c.name.toLowerCase() === chLower) || {};
    const setting = chanSettings[key] || chanSettings[chLower] || {
      dynamic_limit: { enabled: true, offset: 3, min_limit: 5 },
      entrymsg: { enabled: false, notice: true, message: "" }, // as on the device: nothing saved, nothing sent
      badwords: [],
      lockdown: false
    };
    const voice = splitMinutes(setting.timed_voice?.minutes ?? 10);

    const password = channelKey(net, ch);
    const isLocked = stat.lockdown || setting.lockdown;
    const card = document.createElement('div');
    card.className = 'chan-card';
    card.dataset.key = key;
    card.innerHTML = `
      <div class="chan-header">
        <div>
          <strong style="font-size:1.15rem; color:var(--accent-ink);">${esc(ch)}</strong>
          <span class="pill pill-purple">${esc(net.name)}</span>
          <span class="pill ${stat.users ? 'pill-green' : ''}">Users: ${stat.users ?? 0}</span>
          <span class="pill">Active +l: ${stat.active_limit ?? 'None'}</span>
          ${stat.op === 0 ? '<span class="pill pill-red">Not operator</span>' : stat.op === 1 ? '<span class="pill pill-green">Operator</span>' : ''}
          ${isLocked ? '<span class="pill pill-red">LOCKED (+mi)</span>' : ''}
        </div>
        <div class="btn-row">
          ${isLocked ?
            `<button class="btn btn-primary btn-sm" data-icon="unlock" data-act="unlock">Unlock</button>` :
            `<button class="btn btn-red btn-sm" data-icon="lock" data-act="lock">Lockdown (+mi)</button>`
          }
          <button class="btn btn-sm" data-icon="zap" data-act="sync">Sync +l</button>
          <button class="btn btn-red btn-sm ch-remove" data-icon="trash" data-act="remove">Remove</button>
        </div>
      </div>

      <div class="form-grid" style="margin-top:12px;">
        <div class="form-group">
          <label>Dynamic Limit (+l)</label>
          <select id="${id('ch-dyn-en')}" class="form-control">
            <option value="true" ${setting.dynamic_limit?.enabled ? 'selected' : ''}>Enabled (Auto-follow users)</option>
            <option value="false" ${!setting.dynamic_limit?.enabled ? 'selected' : ''}>Disabled</option>
          </select>
        </div>
        <div class="form-group">
          <label>Free Slots Ahead (+l offset)</label>
          <input type="number" id="${id('ch-dyn-offset')}" class="form-control" value="${Number(setting.dynamic_limit?.offset ?? 3)}">
        </div>
        <div class="form-group">
          <label>Min Limit Floor</label>
          <input type="number" id="${id('ch-dyn-min')}" class="form-control" value="${Number(setting.dynamic_limit?.min_limit ?? 5)}">
        </div>
      </div>

      <div class="form-grid">
        <div class="form-group">
          <label for="${id('ch-key')}">Channel Password (+k, empty = none)</label>
          <input type="password" id="${id('ch-key')}" class="form-control" value="${esc(password)}" maxlength="50" autocomplete="new-password">
        </div>
        <div class="form-group">
          <label>Entry Welcome / Rules Notice</label>
          <select id="${id('ch-entry-en')}" class="form-control">
            <option value="true" ${setting.entrymsg?.enabled ? 'selected' : ''}>Enabled (Send on join)</option>
            <option value="false" ${!setting.entrymsg?.enabled ? 'selected' : ''}>Disabled</option>
          </select>
        </div>
        <div class="form-group">
          <label>Send Entry Message As</label>
          <select id="${id('ch-entry-notice')}" class="form-control">
            <option value="true" ${setting.entrymsg?.notice !== false ? 'selected' : ''}>Notice</option>
            <option value="false" ${setting.entrymsg?.notice === false ? 'selected' : ''}>Private message</option>
          </select>
        </div>
        <div class="form-group">
          <label for="${id('ch-auto-op')}">Auto-Op Admins on Join</label>
          <select id="${id('ch-auto-op')}" class="form-control">
            <option value="default" ${typeof setting.auto_op !== 'boolean' ? 'selected' : ''}>As set under Guardian Rules</option>
            <option value="on" ${setting.auto_op === true ? 'selected' : ''}>Enabled here</option>
            <option value="off" ${setting.auto_op === false ? 'selected' : ''}>Disabled here</option>
          </select>
        </div>
        <div class="form-group">
          <label for="${id('ch-voice-en')}">Timed Voice (+v after a while)</label>
          <select id="${id('ch-voice-en')}" class="form-control">
            <option value="true" ${setting.timed_voice?.enabled ? 'selected' : ''}>Enabled</option>
            <option value="false" ${!setting.timed_voice?.enabled ? 'selected' : ''}>Disabled</option>
          </select>
        </div>
        <div class="form-group">
          <label for="${id('ch-voice-after')}">Voice After Being Here For</label>
          <div class="console-input-row" style="margin: 0;">
            <input type="number" id="${id('ch-voice-after')}" class="form-control" min="1" value="${voice.count}">
            <select id="${id('ch-voice-unit')}" class="form-control" style="width:auto;" aria-label="Unit">
              ${TIME_UNITS.map(([name, size]) => `<option value="${size}" ${voice.unit === size ? 'selected' : ''}>${name}</option>`).join('')}
            </select>
          </div>
        </div>
        <div class="form-group" style="grid-column: 1 / -1;">
          <label>Forbidden Words in This Channel (comma separated)</label>
          <input type="text" id="${id('ch-words')}" class="form-control" value="${esc((setting.badwords || []).join(', '))}">
        </div>
        <div class="form-group" style="grid-column: 1 / -1;">
          <label>Entry Message</label>
          <input type="text" id="${id('ch-entry-msg')}" class="form-control" value="${esc(setting.entrymsg?.message)}" placeholder="Welcome to ${esc(ch)}! Please follow channel rules.">
        </div>
      </div>

      <button class="btn btn-primary" style="margin-top:6px;" data-icon="save" data-act="save">Save ${esc(ch)} Settings</button>
      <div class="access"></div>
    `;
    const on = (act, handler) => card.querySelector(`[data-act="${act}"]`)?.addEventListener('click', handler);
    on('lock', () => triggerLockdown(net.name, ch));
    on('unlock', () => triggerUnlock(net.name, ch));
    on('sync', () => triggerSyncLimit(net.name, ch));
    on('remove', () => removeChannel(net.name, ch));
    on('save', () => saveSingleChannelConfig(net.name, ch, setting));
    renderAccess(card.querySelector('.access'), net.name, ch);
    container.appendChild(card);
  }));
}

// The stored (masked) password of a channel; names are compared without case
function channelKey(net, ch) {
  const found = Object.entries(net.channel_keys || {}).find(([name]) => name.toLowerCase() === ch.toLowerCase());
  return found ? found[1] : '';
}

function setChannelKey(net, ch, key) {
  net.channel_keys = Object.fromEntries(Object.entries(net.channel_keys || {}).filter(([name]) => name.toLowerCase() !== ch.toLowerCase()));
  if (key) net.channel_keys[ch.toLowerCase()] = key;
}

// A masked password is sent back under the name it was read with, so the device finds the stored one
async function saveChannelKey(network, ch, key) {
  return updateIrc(irc => {
    const net = irc.networks.find(n => n.name === network);
    if (!net) { alert('That network no longer exists.'); return false; }
    setChannelKey(net, ch, key);
  });
}

async function saveChannels(network, change, also) {
  return updateIrc(irc => {
    const net = irc.networks.find(n => n.name === network);
    if (!net) { alert('That network no longer exists.'); return false; }
    net.channels = change(net.channels || []);
    also?.(net);
    if (editingNetwork === network) {
      document.getElementById('irc-channels').value = net.channels.join(', '); // so an overlapping "Save" cannot restore the old list
    }
  });
}

async function addChannel() {
  const input = document.getElementById('new-channel');
  const network = document.getElementById('new-channel-network').value;
  if (!input.value.trim()) return;
  if (!network) return alert('Add a network first (IRC Settings).');
  const ch = withHash(input.value.trim());
  if (!validChannel(ch)) return alert(CHANNEL_HELP);
  const same = c => c.toLowerCase() === ch.toLowerCase();
  const keyInput = document.getElementById('new-channel-key');
  const key = keyInput.value;
  if (/[\s,]/.test(key) || key.startsWith(':')) return alert('A channel password cannot contain spaces or commas.');
  if (await saveChannels(network, list => list.some(same) ? list : [...list, ch], net => { if (key) setChannelKey(net, ch, key); })) {
    input.value = '';
    keyInput.value = '';
    showToast(`Joining ${ch} on ${network}`);
  }
}

async function removeChannel(network, ch) {
  if (!confirm(`Remove ${ch} from ${network}? The bot will leave the channel.`)) return;
  if (await saveChannels(network, list => list.filter(c => c !== ch), net => setChannelKey(net, ch, ''))) showToast(`Left ${ch}`);
}

async function channelAction(network, channel, action, extra = {}) {
  const res = await fetch('/api/action', { method: 'POST', headers: authJson(), body: JSON.stringify({ action, network, channel, ...extra }) });
  if (!res.ok) {
    const data = await res.json().catch(() => ({}));
    alert(data.message || 'Request failed.');
  }
  return res.ok;
}

// Re-reads the stored channel rules so the cards show what the device really has
async function refreshChannels() {
  const res = await fetch('/api/config', { headers: getAuthHeader() });
  if (res.status === 401) return sessionExpired();
  if (res.ok) {
    const fresh = await res.json();
    currentConfig.guardian = fresh.guardian;
    currentConfig.irc = fresh.irc;
  }
  await fetchStatus();
  renderChannelsUI();
}

async function saveSingleChannelConfig(network, ch, current) {
  const key = `${network.toLowerCase()}/${ch.toLowerCase()}`;
  const field = name => document.getElementById(`${name}-${key}`);
  // lockdown has its own buttons, so it is never sent from here
  const config = {
    dynamic_limit: {
      enabled: field('ch-dyn-en').value === 'true',
      offset: parseInt(field('ch-dyn-offset').value, 10),
      min_limit: parseInt(field('ch-dyn-min').value, 10)
    },
    entrymsg: { enabled: field('ch-entry-en').value === 'true', notice: field('ch-entry-notice').value === 'true', message: field('ch-entry-msg').value },
    badwords: splitList(field('ch-words').value),
    auto_op: { default: null, on: true, off: false }[field('ch-auto-op').value],
    timed_voice: { enabled: field('ch-voice-en').value === 'true', minutes: Number(field('ch-voice-after').value) * Number(field('ch-voice-unit').value) }
  };
  const minutes = config.timed_voice.minutes;
  if (!Number.isInteger(minutes) || minutes < 1 || minutes > 525600) return alert('Timed voice: enter a whole number, from 1 minute to 365 days.');
  const channelPassword = field('ch-key').value;
  const net = networks().find(n => n.name === network);
  const keyChanged = net && channelPassword !== channelKey(net, ch);
  if (keyChanged && (/[\s,]/.test(channelPassword) || channelPassword.startsWith(':'))) return alert('A channel password cannot contain spaces or commas.');
  if (!await channelAction(network, ch, 'save_chan_config', { config })) return;
  Object.assign(current, config);
  currentConfig.guardian = currentConfig.guardian || {};
  currentConfig.guardian.channel_settings = currentConfig.guardian.channel_settings || {};
  currentConfig.guardian.channel_settings[key] = current;
  if (keyChanged && !await saveChannelKey(network, ch, channelPassword)) return;
  showToast(`Saved ${ch} settings!`);
  fetchStatus();
}

async function triggerLockdown(network, ch) {
  if (!confirm(`Trigger Emergency Lockdown on ${ch}? Channel will be set to +mi (moderated/invite-only).`)) return;
  if (await channelAction(network, ch, 'chan_lockdown')) showToast(`Lockdown applied to ${ch}`);
  refreshChannels();
}

async function triggerUnlock(network, ch) {
  if (await channelAction(network, ch, 'chan_unlock')) showToast(`Lockdown released on ${ch}`);
  refreshChannels();
}

async function triggerSyncLimit(network, ch) {
  if (await channelAction(network, ch, 'chan_limit_update')) showToast(`Dynamic limit updated on ${ch}`);
  refreshChannels();
}

// How the report channel and bot mode of the network being edited are doing
function showReportStatus() {
  const box = document.getElementById('report-status');
  const saved = networks().find(n => n.name === editingNetwork);
  const live = (latestStatus.networks || []).find(n => n.name === editingNetwork);
  document.getElementById('bot-mode-status').innerText = saved?.bot_mode ? `Status: ${live?.bot_mode || 'unknown'}` : '';
  document.getElementById('account-status').innerText = saved ? `Recognising users by services account: ${live?.accounts || 'unknown'}.` : '';
  box.innerText = !saved?.report_channel ? 'Status: no report channel set for this network.'
    : `Status of ${saved.report_channel}: ${live?.report || 'unknown'}`;
}
