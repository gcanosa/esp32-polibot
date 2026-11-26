// The event log: what each line is about, newest first, one colour per kind, and clearing it
const LOG_KINDS = { problem: 'Problem', moderation: 'Moderation', security: 'Security', channel: 'Channel', connection: 'Connection', command: 'Console', system: 'System' };
const LOG_PROBLEM = /fail|error|invalid|refus|reject|lost|disconnected|timed? ?out|cannot|could not|abort|closed by|unreadable|not enough|no usable|not connected|did not answer|low memory|dropp|crash|brownout|watchdog/i;
let logLines = [];
let shownLogs = '';

// "[network] [report] [mod] text" gives where it came from, what it is about and the text itself
function parseLog({ time, msg }) {
  let source = 'device', text = msg;
  const from = text.match(/^\[([^\]]+)\] ([\s\S]*)$/);
  if (from) [, source, text] = from;
  const report = text.match(/^\[report\] (?:\[(\w+)\] )?([\s\S]*)$/);
  const tag = report?.[1];
  if (report) text = report[2];
  const kind = /^(Web Console|Reply:)/.test(text) ? 'command'
    : tag === 'mod' || tag === 'raid' ? 'moderation'
    : tag === 'security' ? 'security'
    : LOG_PROBLEM.test(text) ? 'problem'
    : /SASL|NickServ|signed in|sign-in|authenticated/i.test(text) ? 'security'
    : tag === 'channel' ? 'channel'
    : /connect|joined network|nickname|known as|wifi|socket/i.test(text) ? 'connection'
    : /channel|operator/i.test(text) ? 'channel'
    : 'system';
  return { time, source, kind, text };
}

// Before the clock is set the device counts seconds since it started
const logTime = t => t > 1700000000 ? new Date(t * 1000).toLocaleTimeString([], { hour12: false }) : `+${t}s`;

function logRow(line) {
  const row = document.createElement('div');
  row.className = 'log-line log-' + line.kind;
  for (const [part, value] of [['time', logTime(line.time)], ['source', line.source], ['kind', LOG_KINDS[line.kind]], ['text', line.text]]) {
    const cell = document.createElement('span');
    cell.className = 'log-' + part;
    cell.textContent = value; // text from IRC is never read as HTML
    row.appendChild(cell);
  }
  return row;
}

function renderLogs() {
  const box = document.getElementById('terminal');
  const only = document.getElementById('log-filter').value;
  const rows = logLines.map(parseLog).filter(l => !only || l.kind === only).reverse().map(logRow);
  // newest is on top: someone reading older lines stays on them when new ones arrive
  const reading = box.scrollTop > 0;
  const fromEnd = box.scrollHeight - box.scrollTop;
  box.replaceChildren(...rows);
  if (!rows.length) box.textContent = 'No lines to show.';
  box.scrollTop = reading ? box.scrollHeight - fromEnd : 0;
}

async function refreshLogs(poll = false) {
  try {
    const res = await fetch('/api/logs', { headers: pollHeaders(poll) });
    if (res.status === 401) return sessionExpired();
    const lines = await res.json();
    const received = JSON.stringify(lines);
    if (received === shownLogs) return; // nothing new: leave a selection being copied alone
    shownLogs = received;
    logLines = lines;
    renderLogs();
  } catch(e) {
    console.error(e);
  }
}

async function clearLogs() {
  const res = await fetch('/api/action', { method: 'POST', headers: authJson(), body: JSON.stringify({ action: 'clear_logs' }) });
  if (res.status === 401) return sessionExpired();
  if (!res.ok) return alert('Clearing the event log needs the admin role.');
  refreshLogs();
}
