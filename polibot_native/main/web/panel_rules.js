// Protection rules and bot behaviour, generated from one description per setting, and the auto-kick list
// ---------------- Rules and behaviour settings ----------------
const ACTIONS = [['warn', 'Warn'], ['kick', 'Kick'], ['ban', 'Kick & Ban'], ['quiet', 'Mute'], ['escalate', 'Escalate (see below)'], ['report', 'Only report it']];

// the filters of the "spam" section, which start switched off
const spamAction = (key, label, values) => pick(key, label, [['off', 'Off'], ...ACTIONS.filter(([value]) => values.includes(value))], 'off');
const MESSAGE_ACTIONS = ['warn', 'kick', 'ban', 'quiet', 'escalate', 'report'];

const bool = (key, label, fallback = true) => ({ key, label, type: 'bool', fallback });

const int = (key, label, min, max, fallback) => ({ key, label, type: 'int', min, max, fallback });

const text = (key, label, fallback = '') => ({ key, label, type: 'text', fallback });

const list = (key, label) => ({ key, label, type: 'list', fallback: [] });

const pick = (key, label, options, fallback) => ({ key, label, type: 'pick', options, fallback });

// One card per entry; `section` is the part of config.json it saves ('' = the top level).
// Fallbacks matter for the older rules only: the device sends the value in effect for the rest.
const FORMS = {
  'guardian-forms': [
    { title: 'Anti-Flood', section: 'guardian', fields: [
      bool('anti_flood.enabled', 'Anti-Flood Protection'), int('anti_flood.max_lines', 'Max Lines per Time Window', 1, 100, 4),
      int('anti_flood.time_window', 'Time Window (seconds)', 1, 600, 4), pick('anti_flood.action', 'Action', ACTIONS, 'kick'),
      text('anti_flood.kick_reason', 'Reason Shown', 'Flood protection triggered')] },
    { title: 'Excessive Capitals', section: 'guardian', fields: [
      bool('anti_caps.enabled', 'Capitals Filter', false), int('anti_caps.min_length', 'Shortest Line Checked (letters)', 1, 400, 15),
      int('anti_caps.caps_threshold', 'Capitals Allowed Below (%)', 1, 100, 75), pick('anti_caps.action', 'Action', ACTIONS, 'warn'),
      text('anti_caps.kick_reason', 'Reason Shown', 'Excessive caps detected')] },
    { title: 'Forbidden Words', section: 'guardian', fields: [
      bool('anti_badwords.enabled', 'Word Filter'), list('anti_badwords.words', 'Words for All Channels (comma separated)'),
      pick('anti_badwords.action', 'Action', ACTIONS, 'kick'), text('anti_badwords.kick_reason', 'Reason Shown', 'Disallowed word')] },
    { title: 'Links and Channel Adverts', section: 'spam', note: 'Web addresses, and the names of other channels, from people without a staff role.', fields: [
      spamAction('links_action', 'When Somebody Advertises', MESSAGE_ACTIONS), bool('links_channels', 'Other Channel Names Count Too'),
      bool('links_spare_voiced', 'Spare Voiced Users'), text('links_allowed', 'Allowed Domains (comma separated)'), text('links_reason', 'Reason Shown')] },
    { title: 'Mass Highlight', section: 'spam', fields: [
      spamAction('highlight_action', 'When One Line Names Many Present', MESSAGE_ACTIONS), int('highlight_nicks', 'Nicknames that Are Too Many', 2, 50),
      text('highlight_reason', 'Reason Shown')] },
    { title: 'Nickname Change Flood', section: 'spam', fields: [
      spamAction('nick_action', 'Action', ['kick', 'ban', 'quiet', 'escalate', 'report']), int('nick_changes', 'Changes Allowed', 1, 50),
      int('nick_seconds', 'Within (seconds)', 5, 600), text('nick_reason', 'Reason Shown')] },
    { title: 'Join and Part Flood', section: 'spam', note: 'Counts one user\'s joins, parts and quits in a channel; acts when they join once too often.', fields: [
      spamAction('presence_action', 'Action', ['kick', 'ban', 'quiet', 'report']), int('presence_count', 'Joins and Parts Allowed', 2, 50),
      int('presence_seconds', 'Within (seconds)', 5, 600), int('presence_minutes', 'Ban or Mute Length (minutes, 0 = until lifted)', 0, 525600),
      text('presence_reason', 'Reason Shown')] },
    { title: 'Bans, Mutes and Escalation', section: 'moderation', fields: [
      bool('exempt_channel_ops', 'Spare Channel Operators from Filters'),
      pick('mask_style', 'Ban a Nick By', [['host', 'Host (*!*@host)'], ['userhost', 'User and host (*!user@host)'], ['nick', 'Nickname only (easy to evade)']]),
      int('ban_minutes', 'Ban Length (minutes, 0 = until lifted)', 0, 525600), int('quiet_minutes', 'Mute Length (minutes, 0 = until lifted)', 0, 525600),
      text('escalation_steps', 'Escalation Steps (warn, quiet, kick, ban)'), int('strike_reset_minutes', 'Forget Offences After (minutes)', 1, 1440),
      int('escalate_quiet_minutes', 'Escalation Mute (minutes)', 0, 525600), int('escalate_ban_minutes', 'Escalation Ban (minutes)', 0, 525600)] },
    { title: 'Raid Protection', section: 'raid', fields: [
      pick('join_action', 'When Many Join at Once', [['off', 'Do nothing'], ['report', 'Only report it'], ['lockdown', 'Lock the channel down']]),
      int('join_count', 'Joins that Make a Raid', 2, 100), int('join_seconds', 'Within (seconds)', 1, 600),
      text('lockdown_modes', 'Lockdown Modes'), int('unlock_after_seconds', 'End Automatic Lockdown After (seconds, 0 = never)', 0, 86400),
      pick('clone_action', 'Same Text from Several Users', [['off', 'Do nothing'], ['report', 'Only report it'], ['kick', 'Kick'], ['quiet', 'Mute'], ['ban', 'Kick & Ban'], ['escalate', 'Escalate'], ['lockdown', 'Lock the channel down']]),
      int('clone_users', 'Users Needed', 2, 50), int('clone_seconds', 'Within (seconds)', 1, 600), int('clone_min_length', 'Shortest Text Counted', 1, 400)] },
    { title: 'Operators', section: 'guardian', note: 'Each channel can override this, and has its own auto-op, half-op and voice lists, under Channels.', fields: [bool('auto_op_admins', 'Auto-Op Admins on Join')] },
  ],
  'bot-forms': [
    { title: 'General', section: '', fields: [text('device_name', 'Device Name', 'PoliBOT-Sentry')] },
    { title: 'Connection (all networks)', section: 'irc', fields: [
      text('cmd_prefix', 'Command Prefix', '!'), int('reconnect_delay', 'First Reconnect Delay (seconds)', 1, 3600, 5),
      int('max_reconnect_delay', 'Longest Reconnect Delay (seconds)', 1, 3600, 60), int('ping_timeout', 'Ping Timeout (seconds)', 20, 3600, 120)] },
    { title: 'Operator Status and Rejoining', section: 'ops', note: 'The commands sent to the services are set per network under IRC Settings.', fields: [
      bool('request', 'Ask for Operator Status When Missing'), int('retry_seconds', 'Ask Again Every (seconds)', 30, 86400),
      int('rejoin_delay', 'First Rejoin Delay (seconds)', 1, 600), int('rejoin_max_delay', 'Longest Rejoin Delay (seconds)', 5, 86400)] },
    { title: 'Floating Channel Limit (+l)', section: 'channel_limit', note: 'The free places and the lowest limit are set per channel under Channels.', fields: [
      int('interval_seconds', 'Adjust the Limit Every (seconds)', 5, 600), int('grace_seconds', 'No Limit After Joining a Channel (seconds)', 0, 3600),
      int('slack', 'Extra Places on a Raise', 0, 50), int('lower_after_seconds', 'Lower It Only After (seconds too high)', 0, 86400)] },
    { title: 'Rate Limits and Sign-In', section: 'limits', note: 'Pacing keeps the server from disconnecting the bot. Command limits apply to people without a staff role.', fields: [
      int('send_burst', 'Lines Sent at Once', 1, 20), int('send_interval_ms', 'Then One Line Every (ms, 0 = no pacing)', 0, 10000),
      int('command_count', 'Commands per Person', 1, 100), int('command_seconds', 'Within (seconds)', 1, 600),
      int('command_total', 'Commands from Everyone Together', 1, 120), int('login_attempts', 'Failed Sign-Ins Allowed', 1, 50),
      int('login_window_minutes', 'Counted Over (minutes)', 1, 1440), int('login_block_minutes', 'Then Paused For (minutes)', 1, 1440),
      int('masskick_max', 'Most Kicks per Masskick', 1, 90),
      int('session_minutes', 'Sign Out of This Panel When Idle (minutes)', 1, 1440)] },
    { title: 'Help', section: 'help', note: 'Staff always get their own list by asking in a private message.', fields: [
      pick('reply', 'Help Asked in a Channel', [['channel', 'Answer in the channel (general commands only)'], ['notice', 'Answer by notice, to the asker only']], 'channel')] },
    { title: 'Timed Voice', section: 'timed_voice', note: 'Switched on and timed per channel under Channels. Who earned voice is saved at the Seen interval, a loss of voice at once.', fields: [
      int('max_remembered', 'People Remembered (all channels)', 10, 2000), int('keep_days', 'Forget After (days away)', 1, 365)] },
    { title: 'Seen', section: 'seen', fields: [
      bool('enabled', 'The seen Command'), bool('save_message', 'Remember What Was Said'),
      int('max_entries', 'People Remembered', 10, 500), int('keep_days', 'Forget After (days)', 1, 365),
      int('save_minutes', 'Save to Flash Every (minutes)', 1, 1440)] },
    { title: 'Tell', section: 'tell', fields: [
      bool('enabled', 'The tell Command'),
      pick('delivery', 'Deliver Messages', [['private', 'In a private message'], ['notice', 'In a notice'], ['channel', 'In the channel']]),
      int('per_recipient', 'Waiting per Recipient', 1, 20), int('per_sender', 'Waiting per Sender', 1, 50),
      int('max_total', 'Waiting in Total', 1, 150), int('keep_days', 'Discard After (days)', 1, 365)] },
    { title: 'Topic and Mode Locks', section: 'locks', note: 'What is locked is set per channel under Channels.', fields: [
      int('reverts_per_minute', 'Most Times Put Back per Minute and Channel', 1, 20)] },
    { title: 'Reliability', section: 'health', note: 'A device whose IRC connection, main loop or web panel hangs restarts by itself; the event log keeps what led to it.', fields: [
      int('stuck_minutes', 'Restart When Unresponsive For (minutes, 0 = never)', 0, 60)] },
    { title: 'Alerts Outside IRC', section: 'alerts', note: 'Sent to a webhook or an ntfy topic, for what the report channel cannot tell. A saved address reads as ********.',
      extra: { label: 'Send Test Alert', icon: 'send', call: 'testAlert()' }, fields: [
      text('url', 'Address (https://..., empty = off)'),
      pick('format', 'Send As', [['text', 'Plain text (ntfy)'], ['json', 'JSON with "text" and "content"']], 'text'),
      int('offline_minutes', 'A Network Away For (minutes, 0 = never)', 0, 1440), bool('on_fault', 'Restarts After a Fault'),
      bool('on_low_memory', 'Low Memory (below the limit set under Report Channel)')] },
    { title: 'Reverse Proxy', section: 'network',
      note: 'If the panel is reached through your own proxy (Caddy, nginx, ...), enter the address the device sees it connect from: sign-in reports and the lockout then count each visitor. Empty = none.',
      fields: [text('trusted_proxies', 'Trusted Proxy Addresses (comma separated)')] },
    { title: 'Local Name', section: 'network', note: 'The panel also opens as http://<name>.local/ on your own network. Empty = off.', fields: [
      text('mdns_name', 'Name')] },
    { title: 'What Goes to the Report Channel', section: 'report', note: 'The channel and its password are set per network under IRC Settings.', fields: [
      bool('moderation', 'Moderation: kicks, bans, commands used'), bool('raid', 'Raids and Lockdowns'),
      bool('security', 'Sign-Ins and Command Floods'), bool('channel', 'Operator Status and Joining Problems'),
      bool('system', 'Settings Changes, Restarts and Low Memory'), int('max_per_minute', 'Most Reports per Minute', 1, 120),
      int('low_memory_kb', 'Warn When Free Internal RAM Drops Below (KB, 0 = never)', 0, 300)] },
  ]
};

const splitList = value => value.split(',').map(s => s.trim()).filter(Boolean);

const fieldId = (card, field) => `set-${card.section || 'top'}-${field.key}`;

const readPath = (obj, path) => path.split('.').reduce((o, k) => o?.[k], obj);

function writePath(obj, path, value) {
  const keys = path.split('.');
  const last = keys.pop();
  keys.reduce((o, k) => (o[k] = o[k] || {}), obj)[last] = value;
}

function fieldHtml(card, field) {
  const id = fieldId(card, field);
  const options = field.type === 'bool' ? [['true', 'Enabled'], ['false', 'Disabled']] : field.options;
  const input = options
    ? `<select id="${id}" class="form-control">${options.map(([v, l]) => `<option value="${esc(v)}">${esc(l)}</option>`).join('')}</select>`
    : field.type === 'int' ? `<input type="number" id="${id}" class="form-control" min="${field.min}" max="${field.max}">`
    : `<input type="text" id="${id}" class="form-control">`;
  return `<div class="form-group"><label for="${id}">${esc(field.label)}</label>${input}</div>`;
}

function renderForms() {
  for (const [target, cards] of Object.entries(FORMS)) {
    document.getElementById(target).innerHTML = cards.map((card, i) => `
      <div class="card">
        <h3 style="margin-bottom: 12px;">${esc(card.title)}</h3>
        ${card.note ? `<p style="font-size:0.82rem; color:var(--dim); margin:-6px 0 14px;">${esc(card.note)}</p>` : ''}
        <div class="form-grid">${card.fields.map(f => fieldHtml(card, f)).join('')}</div>
        <div class="btn-row">
          <button class="btn btn-primary" data-icon="save" onclick="saveCard('${target}', ${i})">Save ${esc(card.title)}</button>
          ${card.extra ? `<button class="btn" data-icon="${card.extra.icon}" onclick="${card.extra.call}">${esc(card.extra.label)}</button>` : ''}
        </div>
      </div>`).join('');
  }
}

function loadForms() {
  Object.values(FORMS).flat().forEach(card => {
    const values = card.section ? currentConfig[card.section] : currentConfig;
    card.fields.forEach(field => {
      const value = readPath(values, field.key) ?? field.fallback ?? field.options?.[0][0] ?? '';
      document.getElementById(fieldId(card, field)).value = field.type === 'list' ? (value || []).join(', ') : String(value);
    });
  });
}

// Writes the card's values over a fresh copy of its section, so changes made elsewhere survive
async function saveCard(target, index) {
  const card = FORMS[target][index];
  const values = [];
  for (const field of card.fields) {
    const raw = document.getElementById(fieldId(card, field)).value;
    let value = raw;
    if (field.type === 'bool') value = raw === 'true';
    if (field.type === 'list') value = splitList(raw);
    if (field.type === 'int') {
      value = Number(raw);
      if (!Number.isInteger(value) || raw === '') return alert(`${field.label}: enter a whole number from ${field.min} to ${field.max}.`);
      value = Math.min(Math.max(value, field.min), field.max);
    }
    values.push([field.key, value]);
  }
  if (card.section === 'irc') {
    if (await updateIrc(irc => values.forEach(([k, v]) => writePath(irc, k, v)))) showToast(`Saved ${card.title}`);
    return;
  }
  const fresh = await fetch('/api/config', { headers: getAuthHeader() });
  if (fresh.status === 401) return sessionExpired();
  if (!fresh.ok) return alert('Could not read the current configuration.');
  const config = await fresh.json();
  const part = card.section ? (config[card.section] || {}) : {};
  values.forEach(([k, v]) => writePath(part, k, v));
  if (await postConfig(card.section ? { [card.section]: part } : part)) showToast(`Saved ${card.title}`);
}

async function testAlert() {
  const res = await fetch('/api/action', { method: 'POST', headers: authJson(), body: JSON.stringify({ action: 'test_alert' }) });
  if (res.status === 401) return sessionExpired();
  const data = await res.json().catch(() => ({}));
  if (!res.ok) return alert(data.message || 'Could not send a test alert.');
  showToast(data.message);
}

// The device says when a request came through a proxy it does not trust
let seenProxy = '';

function showProxyHint(address) {
  seenProxy = address || '';
  document.getElementById('proxy-banner').hidden = !seenProxy;
  document.getElementById('proxy-banner-text').innerText = seenProxy ? `You seem to be connecting through a proxy at ${seenProxy}.` : '';
}

async function trustProxy() {
  const address = seenProxy;
  if (!confirm(`Trust ${address}? Only if it is your own proxy: whatever it says about visitors' addresses will be believed.`)) return;
  const res = await fetch('/api/config', { headers: getAuthHeader() });
  if (res.status === 401) return sessionExpired();
  if (!res.ok) return alert('Could not read the current configuration.');
  const network = (await res.json()).network || {};
  const list = (network.trusted_proxies || '').split(/[, ]+/).filter(a => a && a !== address);
  network.trusted_proxies = [...list, address].join(', ');
  if (!await postConfig({ network })) return;
  showProxyHint('');
  showToast(`Trusting the proxy at ${address}`);
}

async function postConfig(body) {
  const res = await fetch('/api/config', { method: 'POST', headers: authJson(), body: JSON.stringify(body) });
  if (res.status === 401) { sessionExpired(); return false; }
  if (!res.ok) {
    const data = await res.json().catch(() => ({}));
    alert(data.message || 'Failed to save. Administrator permission is required.');
    return false;
  }
  await loadConfig();
  return true;
}

// ---------------- Auto-kick list ----------------
function renderAkicks() {
  const select = document.getElementById('akick-network');
  const previous = select.value;
  select.innerHTML = '<option value="">All networks</option>' + networks().map(n => `<option value="${esc(n.name)}">${esc(n.name)}</option>`).join('');
  select.value = networks().some(n => n.name === previous) ? previous : '';
  const entries = currentConfig.guardian?.akick || [];
  const container = document.getElementById('akick-list');
  container.innerHTML = entries.length ? '' : '<div style="color:var(--dim); margin-bottom:14px;">The list is empty.</div>';
  entries.forEach(entry => {
    const item = document.createElement('div');
    item.className = 'user-item';
    item.innerHTML = `<div><code>${esc(entry.mask)}</code> <span class="pill pill-purple">${esc(entry.network || 'all networks')}</span>
      <span class="pill">${esc(entry.channel || 'all channels')}</span> ${esc(entry.reason)}</div>
      <button class="btn btn-red btn-sm" data-icon="trash">Remove</button>`;
    item.querySelector('button').addEventListener('click', () => changeAkicks(all =>
      all.filter(a => !(a.mask === entry.mask && (a.network || '') === (entry.network || '') && (a.channel || '') === (entry.channel || '')))));
    container.appendChild(item);
  });
}

async function changeAkicks(change) {
  const fresh = await fetch('/api/config', { headers: getAuthHeader() });
  if (fresh.status === 401) return sessionExpired();
  if (!fresh.ok) return alert('Could not read the current configuration.');
  const guardian = (await fresh.json()).guardian || {};
  guardian.akick = change(guardian.akick || []);
  return postConfig({ guardian });
}

async function addAkick() {
  const val = id => document.getElementById(id).value.trim();
  let mask = val('akick-mask');
  if (!mask) return;
  if (!mask.includes('!')) mask = mask.includes('@') ? '*!' + mask : mask + '!*@*';
  if (/\s/.test(mask) || !/[^*?!@.~]/.test(mask)) return alert('The mask must name a nick or a host, without spaces.');
  const entry = { network: val('akick-network'), channel: val('akick-channel') ? withHash(val('akick-channel')) : '', mask, reason: val('akick-reason') };
  if (await changeAkicks(all => [...all, entry])) {
    document.getElementById('akick-mask').value = '';
    document.getElementById('akick-reason').value = '';
    showToast(`Added ${mask}`);
  }
}
