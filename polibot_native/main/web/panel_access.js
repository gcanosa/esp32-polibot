// A channel's auto-op, half-op and voice lists, and the units of the timed voice
const TIME_UNITS = [['Minutes', 1], ['Hours', 60], ['Days', 1440]];

const ACCESS_LISTS = [['o', 'Auto-Op'], ['h', 'Auto Half-Op'], ['v', 'Auto-Voice']];

// 120 -> 2 hours: the largest unit that divides it
function splitMinutes(minutes) {
  const unit = [1440, 60].find(size => minutes % size === 0) || 1;
  return { count: minutes / unit, unit };
}

function renderAccess(box, network, channel) {
  const same = (a, b) => String(a).toLowerCase() === String(b).toLowerCase();
  const entries = (currentConfig.guardian?.access || []).filter(e => same(e.network, network) && same(e.channel, channel));
  box.innerHTML = `
    <h4 style="font-size: 1rem; margin: 14px 0 4px;">Auto-Op, Half-Op and Voice Lists</h4>
    <p style="font-size: 0.82rem; color: var(--dim); margin: 4px 0 10px;">
      For this channel only. Enter a hostmask (<code>*!*@host.example</code>), a services account (<code>alice</code>), an account of this bot (<code>user:alice</code>), or the nickname of somebody there: it is stored as their account, else their hostmask. A nickname alone never counts.
    </p>
    ${ACCESS_LISTS.map(([mode, title]) => entries.filter(e => e.mode === mode).map(e => `
      <div class="user-item">
        <div><span class="pill pill-purple">${title}</span> <code>${esc(e.who)}</code></div>
        <button class="btn btn-red btn-sm" data-icon="trash" data-mode="${mode}" data-who="${esc(e.who)}">Remove</button>
      </div>`).join('')).join('') || '<div style="color:var(--dim); margin-bottom:10px;">Nobody is listed.</div>'}
    <div class="console-input-row" style="margin: 10px 0 0;">
      <select class="form-control" style="width:auto;" aria-label="List">${ACCESS_LISTS.map(([mode, title]) => `<option value="${mode}">${title}</option>`).join('')}</select>
      <input type="text" class="form-control" maxlength="95" placeholder="account, hostmask or nick" aria-label="Who to add" autocomplete="off">
      <button class="btn btn-primary" data-icon="plus" data-add>Add</button>
    </div>`;
  box.querySelectorAll('button[data-who]').forEach(button => button.addEventListener('click',
    () => changeAccess('access_del', network, channel, button.dataset.mode, button.dataset.who)));
  const input = box.querySelector('input');
  const add = () => input.value.trim() && changeAccess('access_add', network, channel, box.querySelector('select').value, input.value.trim());
  box.querySelector('[data-add]').addEventListener('click', add);
  input.addEventListener('keydown', event => { if (event.key === 'Enter') add(); });
}

async function changeAccess(action, network, channel, mode, who) {
  const res = await fetch('/api/action', { method: 'POST', headers: authJson(), body: JSON.stringify({ action, network, channel, mode, who }) });
  if (res.status === 401) return sessionExpired();
  const data = await res.json().catch(() => ({}));
  if (!res.ok) return alert(data.message || 'Request failed.');
  showToast(data.message);
  await refreshChannels();
}
