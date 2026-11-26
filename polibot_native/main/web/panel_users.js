// User accounts and passwords
async function changePassword() {
  const fields = ['pw-current', 'pw-new', 'pw-repeat'].map(id => document.getElementById(id));
  const [current, password, repeat] = fields.map(f => f.value);
  if (!current || !password) return alert('Enter your current password and a new one.');
  if (password !== repeat) return alert('The new passwords do not match.');
  const res = await fetch('/api/password', { method: 'POST', headers: authJson(), body: JSON.stringify({ current, password }) });
  if (res.status === 401) return sessionExpired();
  const data = await res.json().catch(() => ({}));
  if (!res.ok) return alert(data.message || 'Could not change the password.');
  fields.forEach(f => f.value = '');
  showToast(data.message);
}

async function loadUsers() {
  try {
    const res = await fetch('/api/users', { headers: getAuthHeader() });
    if (!res.ok) return;
    const users = await res.json();
    const container = document.getElementById('user-list-container');
    container.innerHTML = '';
    if (users.length === 0) {
      container.innerHTML = '<div style="color:var(--dim)">No user accounts registered.</div>';
      return;
    }
    users.forEach(u => {
      const item = document.createElement('div');
      item.className = 'user-item';
      let chRolesStr = '';
      if (u.channel_roles && Object.keys(u.channel_roles).length > 0) {
        chRolesStr = ' | Per-Channel: ' + Object.entries(u.channel_roles).map(([c, r]) => `<span class="pill">${esc(c)}: ${esc(r.toUpperCase())}</span>`).join(' ');
      }
      item.innerHTML = `
        <div>
          <strong style="font-size:1.05rem; color:var(--text);">${esc(u.username)}</strong>
          <span class="pill pill-purple" style="margin-left:6px;">${esc(u.global_role.toUpperCase())}</span>
          ${chRolesStr}
          <div style="font-size:0.78rem; color:var(--dim); margin-top:4px;">
            Hostmasks: <code>${esc((u.hostmasks || []).join(', '))}</code>
            ${u.accounts?.length ? `<br>Services accounts: <code>${esc(u.accounts.join(', '))}</code>` : ''}
          </div>
        </div>
        <div class="btn-row">
          <button class="btn btn-sm" data-icon="edit" data-act="edit">Edit</button>
          <button class="btn btn-red btn-sm" data-icon="trash" data-act="delete">Delete</button>
        </div>
      `;
      item.querySelector('[data-act="edit"]').addEventListener('click', () => populateEditUser(u));
      item.querySelector('[data-act="delete"]').addEventListener('click', () => deleteUser(u.username));
      container.appendChild(item);
    });
  } catch(e) {
    console.error(e);
  }
}

function populateEditUser(u) {
  document.getElementById('usr-name').value = u.username;
  document.getElementById('usr-pwd').value = '';
  document.getElementById('usr-global-role').value = u.global_role;
  document.getElementById('usr-chan-roles').value = Object.entries(u.channel_roles || {}).map(([c, r]) => `${c}: ${r}`).join(', ');
  document.getElementById('usr-hostmasks').value = (u.hostmasks || []).join(', ');
  document.getElementById('usr-accounts').value = (u.accounts || []).join(', ');
  document.getElementById('usr-pwd').placeholder = 'Leave blank to keep current password';
}

async function saveUserAccount() {
  const username = document.getElementById('usr-name').value.trim();
  const password = document.getElementById('usr-pwd').value;
  const global_role = document.getElementById('usr-global-role').value;
  const chanRolesRaw = document.getElementById('usr-chan-roles').value.trim();
  const hostmasksRaw = document.getElementById('usr-hostmasks').value.trim();

  if (!username) return alert('Username is required.');

  const channel_roles = {};
  if (chanRolesRaw) {
    chanRolesRaw.split(',').forEach(item => {
      if (item.includes(':')) {
        const [c, r] = item.split(':');
        if (c.trim() && r.trim()) {
          channel_roles[c.trim().toLowerCase()] = r.trim().toLowerCase();
        }
      }
    });
  }

  const hostmasks = hostmasksRaw ? hostmasksRaw.split(',').map(s=>s.trim()).filter(Boolean) : ['*!*@*'];

  const payload = {
    action: 'save',
    username: username,
    password: password || undefined,
    global_role: global_role,
    channel_roles: channel_roles,
    hostmasks: hostmasks,
    accounts: splitList(document.getElementById('usr-accounts').value)
  };

  const res = await fetch('/api/users', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...getAuthHeader() },
    body: JSON.stringify(payload)
  });
  const data = await res.json();
  if (res.ok) {
    showToast(data.message || 'User saved successfully.');
    document.getElementById('usr-name').value = '';
    document.getElementById('usr-pwd').value = '';
    document.getElementById('usr-chan-roles').value = '';
    document.getElementById('usr-hostmasks').value = '*!*@*';
    document.getElementById('usr-accounts').value = '';
    loadUsers();
  } else {
    alert(data.message || 'Failed to save user.');
  }
}

async function deleteUser(username) {
  if (!confirm(`Are you sure you want to delete user '${username}'?`)) return;
  const res = await fetch('/api/users', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...getAuthHeader() },
    body: JSON.stringify({ action: 'delete', username: username })
  });
  const data = await res.json();
  if (res.ok) {
    showToast(data.message || 'User deleted.');
    loadUsers();
  } else {
    alert(data.message || 'Error deleting user.');
  }
}
