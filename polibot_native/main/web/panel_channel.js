// One channel's members, bans and mutes, asked for only while its dialog is open
let openedChannel = null; // { network, channel }
let channelData = null;

// Highest first
const RANKS = [['q', 'Owner'], ['a', 'Admin'], ['o', 'Op'], ['h', 'Half-Op'], ['v', 'Voice']];

const channelDialog = () => document.getElementById('channel-dialog');

// The toast would be hidden behind the dialog, so results are shown inside it
const channelMessage = text => document.getElementById('channel-message').innerText = text;

function openChannel(network, channel) {
  openedChannel = { network, channel };
  channelData = null;
  document.getElementById('channel-title').innerText = channel;
  document.getElementById('member-search').value = '';
  channelMessage('');
  renderChannel();
  channelDialog().showModal();
  loadChannel();
}

// The answer, or null once what went wrong has been shown
async function channelRequest(body) {
  const res = await fetch('/api/action', { method: 'POST', headers: authJson(), body: JSON.stringify({ ...openedChannel, ...body }) });
  if (res.status === 401) { sessionExpired(); return null; }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) { channelMessage(data.message || 'The request was refused.'); return null; }
  return data;
}

async function loadChannel() {
  const asked = openedChannel;
  const data = await channelRequest({ action: 'channel_view' });
  if (data && asked === openedChannel) {
    channelData = data;
    renderChannel();
  }
}

function renderChannel() {
  const d = channelData;
  document.getElementById('channel-info').innerText = !d ? 'Loading…'
    : !d.joined ? `The bot is not in this channel (${openedChannel.network}).`
    : `${openedChannel.network} · ${d.members.length} present · the bot is ${d.op === 1 ? 'an operator' : 'not an operator'}${d.topic ? ' · Topic: ' + d.topic : ''}`;
  renderBans();
  renderMembers();
}

const timeLeft = s => s === 0 ? 'Until lifted' : s < 0 ? 'Timed'
  : s >= 86400 ? `${Math.floor(s / 86400)}d ${Math.floor(s % 86400 / 3600)}h`
  : s >= 3600 ? `${Math.floor(s / 3600)}h ${Math.floor(s % 3600 / 60)}m` : `${Math.ceil(s / 60)}m`;

function renderBans() {
  const rows = document.getElementById('ban-rows');
  const bans = channelData?.bans || [];
  rows.innerHTML = bans.map((b, i) => `<tr>
    <td><span class="pill ${b.mute ? 'pill-orange' : 'pill-red'}">${b.mute ? 'Mute' : 'Ban'}</span> <code>${esc(b.mode)}</code></td>
    <td data-label="Set against">${esc(b.nick) || '—'}</td>
    <td data-label="Time left">${timeLeft(b.left)}</td>
    <td class="row-actions"><button type="button" class="btn btn-sm" data-icon="unlock" data-ban="${i}">Lift</button></td>
  </tr>`).join('') || `<tr><td colspan="4">${channelData ? 'No bans or mutes on record.' : 'Loading…'}</td></tr>`;
  rows.querySelectorAll('button[data-ban]').forEach(button => button.addEventListener('click', () => liftBan(bans[button.dataset.ban])));
}

async function liftBan(ban) {
  if (!confirm(`Lift ${ban.mode}?`)) return;
  const data = await channelRequest({ action: 'ban_lift', mode: ban.mode });
  if (!data) return;
  channelMessage(data.message);
  loadChannel();
}

// ponytail: no paging; a very large channel is slow to draw, add it if that matters
function renderMembers() {
  const query = document.getElementById('member-search').value.trim().toLowerCase();
  const all = channelData?.members || [];
  const rank = m => {
    const at = RANKS.findIndex(([mode]) => m.modes.includes(mode));
    return at < 0 ? RANKS.length : at;
  };
  const shown = all.filter(m => `${m.nick} ${m.host}`.toLowerCase().includes(query)).sort((a, b) => rank(a) - rank(b) || a.nick.localeCompare(b.nick));
  const act = (i, command, label, red = false) => `<button type="button" class="btn btn-sm${red ? ' btn-red' : ''}" data-member="${i}" data-do="${command}">${label}</button>`;
  const rows = document.getElementById('member-rows');
  rows.innerHTML = shown.map((m, i) => `<tr>
    <td><strong>${esc(m.nick)}</strong></td>
    <td data-label="Status">${RANKS.filter(([mode]) => m.modes.includes(mode)).map(([, name]) => `<span class="pill pill-green">${name}</span>`).join('') || '—'}</td>
    <td data-label="Host"><code>${esc(m.host) || '—'}</code></td>
    <td class="row-actions">
      ${m.modes.includes('o') ? act(i, 'deop', 'Deop') : act(i, 'op', 'Op')}
      ${m.modes.includes('v') ? act(i, 'devoice', 'Devoice') : act(i, 'voice', 'Voice')}
      ${act(i, 'kick', 'Kick', true)}
      ${act(i, 'kickban', 'Ban', true)}
    </td>
  </tr>`).join('') || `<tr><td colspan="4">${channelData ? 'Nobody to show.' : 'Loading…'}</td></tr>`;
  rows.querySelectorAll('button[data-do]').forEach(button => button.addEventListener('click',
    () => memberAction(button.dataset.do, shown[button.dataset.member].nick)));
  document.getElementById('member-count').innerText = channelData ? `${shown.length} of ${all.length} members` : '';
}

// The same commands as on IRC, so the same permissions apply
async function memberAction(command, nick) {
  let line = `!${command} ${openedChannel.channel} ${nick}`;
  if (command === 'kick' || command === 'kickban') {
    const reason = prompt(`${command === 'kick' ? 'Kick' : 'Ban and kick'} ${nick} from ${openedChannel.channel}. Reason (may be empty):`, '');
    if (reason === null) return;
    if (reason.trim()) line += ' ' + reason.trim();
  }
  const data = await channelRequest({ action: 'send_irc', cmd: line });
  if (!data) return;
  channelMessage(data.message || 'Sent.');
  setTimeout(loadChannel, 1500); // once the server has answered
}
