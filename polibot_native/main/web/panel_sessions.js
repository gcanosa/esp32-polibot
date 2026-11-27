// Who is signed in, on IRC and in this panel, and signing them out
function openSessions() {
  showUsersView('users-sessions');
  document.getElementById('users-message').innerText = '';
  loadSessions();
}

async function loadSessions() {
  const res = await fetch('/api/sessions', { headers: getAuthHeader() });
  if (res.status === 401) return sessionExpired();
  const data = await res.json().catch(() => null);
  if (!res.ok || !data) return document.getElementById('users-message').innerText = data?.message || 'Could not read who is signed in.';
  renderSessions(data);
}

const idleTime = seconds => seconds < 0 ? '—' : seconds < 60 ? 'Now' : seconds < 3600 ? `${Math.floor(seconds / 60)}m` : `${Math.floor(seconds / 3600)}h ${Math.floor(seconds % 3600 / 60)}m`;

function renderSessions(list) {
  const rows = document.getElementById('session-rows');
  rows.innerHTML = list.map((s, i) => `<tr>
    <td><strong>${esc(s.user)}</strong>${s.current ? ' <span class="pill pill-green">This browser</span>' : ''}</td>
    <td data-label="Where"><span class="pill ${s.network ? '' : 'pill-purple'}">${esc(s.network || 'Web panel')}</span></td>
    <td data-label="Signed in as">${s.nick ? esc(s.nick) + ' ' : ''}<code>${esc(s.from)}</code></td>
    <td data-label="Idle">${idleTime(s.idle)}</td>
    <td class="row-actions"><button type="button" class="btn btn-red btn-sm" data-icon="signout" data-session="${i}">Sign Out</button></td>
  </tr>`).join('') || '<tr><td colspan="5">Nobody is signed in.</td></tr>';
  rows.querySelectorAll('button[data-session]').forEach(button => button.addEventListener('click', () => endSession(list[button.dataset.session])));
}

async function endSession(session) {
  if (!confirm(session.current ? 'Sign out of this panel?' : `Sign ${session.user} out?`)) return;
  const res = await fetch('/api/sessions', { method: 'POST', headers: authJson(), body: JSON.stringify({ id: session.id }) });
  if (res.status === 401 || (res.ok && session.current)) return sessionExpired();
  const data = await res.json().catch(() => ({}));
  if (!res.ok) return alert(data.message || 'Could not sign them out.');
  document.getElementById('users-message').innerText = data.message;
  loadSessions();
}
