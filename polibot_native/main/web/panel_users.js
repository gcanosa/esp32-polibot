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

// Highest first: the order of the role pickers and of the counts
const ROLES = { owner: { name: 'Owner', level: 100 }, admin: { name: 'Admin', level: 75 }, mod: { name: 'Mod', level: 50 }, user: { name: 'User', level: 10 } };

let userList = null; // null: not loaded, or this account may not see it
let userSort = { by: 'role', reverse: false };
let editingUser = null; // null while adding

const roleKey = role => {
  const key = String(role).toLowerCase();
  return key === 'moderator' ? 'mod' : Object.hasOwn(ROLES, key) ? key : 'user';
};

const roleBadge = (role, text) => `<span class="role role-${roleKey(role)}">${esc(text ?? ROLES[roleKey(role)].name)}</span>`;

const roleOptions = selected => Object.entries(ROLES)
  .map(([key, role]) => `<option value="${key}"${key === selected ? ' selected' : ''}>${role.name}</option>`).join('');

const usersDialog = () => document.getElementById('users-dialog');

async function loadUsers() {
  try {
    const res = await fetch('/api/users', { headers: getAuthHeader() });
    if (res.status === 401) return sessionExpired();
    userList = res.ok ? await res.json() : null;
    renderUserSummary();
    renderUsers();
  } catch(e) {
    console.error(e);
  }
}

// The tab itself only counts the accounts; they are managed in the dialog
function renderUserSummary() {
  document.getElementById('users-open').disabled = !userList;
  document.getElementById('user-summary').innerHTML = !userList ? 'Only administrators can see and manage the accounts.'
    : Object.keys(ROLES).map(key => {
      const count = userList.filter(u => roleKey(u.global_role) === key).length;
      return count ? roleBadge(key, `${count} × ${ROLES[key].name}`) : '';
    }).join(' ');
}

function openUsers() {
  document.getElementById('users-search').value = '';
  document.getElementById('users-role-filter').value = '';
  showUserList();
  usersDialog().showModal();
  loadUsers();
}

// The toast would be hidden behind the dialog, so results are shown inside it
function showUserList(message = '') {
  document.getElementById('users-form').hidden = true;
  document.getElementById('users-list').hidden = false;
  document.getElementById('users-message').innerText = message;
}

function sortUsers(by) {
  userSort = { by, reverse: userSort.by === by && !userSort.reverse };
  renderUsers();
}

// How the bot knows this account on IRC without being told a password
function recognisedBy(u) {
  return [
    ...(u.accounts || []).map(a => `<span class="pill pill-green" title="Signed in to services">${esc(a)}</span>`),
    ...(u.trusted_hostmasks || []).map(m => `<span class="pill pill-orange" title="Trusted host: no password">${esc(m)}</span>`)
  ].join(' ') || '<span class="pill pill-purple">Password only</span>';
}

// ponytail: no paging, a device holds few accounts; add it if the list ever gets long
function renderUsers() {
  const query = document.getElementById('users-search').value.trim().toLowerCase();
  const only = document.getElementById('users-role-filter').value;
  const level = u => ROLES[roleKey(u.global_role)].level;
  const rolesOf = u => [u.global_role, ...Object.values(u.channel_roles || {})].map(roleKey);
  const wordsOf = u => [u.username, ...Object.keys(u.channel_roles || {}), ...(u.hostmasks || []), ...(u.accounts || [])].join(' ').toLowerCase();
  const shown = (userList || [])
    .filter(u => (!only || rolesOf(u).includes(only)) && wordsOf(u).includes(query))
    .sort((a, b) => ((userSort.by === 'role' && level(b) - level(a)) || a.username.localeCompare(b.username)) * (userSort.reverse ? -1 : 1));

  document.querySelectorAll('#users-dialog th[data-sort]').forEach(th => {
    // roles start with the highest, names with A
    const descending = (th.dataset.sort === 'role') !== userSort.reverse;
    if (th.dataset.sort === userSort.by) th.setAttribute('aria-sort', descending ? 'descending' : 'ascending');
    else th.removeAttribute('aria-sort');
  });

  const iAmOwner = roleKey(currentUser?.global_role) === 'owner';
  const rows = document.getElementById('users-rows');
  rows.innerHTML = shown.map(u => {
    const owner = roleKey(u.global_role) === 'owner';
    const name = esc(u.username);
    const channels = Object.entries(u.channel_roles || {}).map(([c, r]) => roleBadge(r, `${c} · ${ROLES[roleKey(r)].name}`)).join(' ');
    return `<tr>
      <td><span class="avatar role-${roleKey(u.global_role)}" aria-hidden="true">${esc(u.username.charAt(0).toUpperCase())}</span><strong>${name}</strong></td>
      <td data-label="Role">${roleBadge(u.global_role)}</td>
      <td data-label="Channel roles">${channels || '—'}</td>
      <td data-label="Recognised by">${recognisedBy(u)}</td>
      <td class="users-actions">
        <button class="btn btn-sm" data-icon="edit" data-act="edit" data-user="${name}" aria-label="Edit ${name}"
          title="${owner && !iAmOwner ? 'Only an owner can change an owner' : 'Edit'}"${owner && !iAmOwner ? ' disabled' : ''}></button>
        <button class="btn btn-red btn-sm" data-icon="trash" data-act="delete" data-user="${name}" aria-label="Delete ${name}"
          title="${owner ? 'Owner accounts cannot be deleted' : 'Delete'}"${owner ? ' disabled' : ''}></button>
      </td>
    </tr>`;
  }).join('') || `<tr><td colspan="5">${userList ? 'No matching users.' : 'Loading…'}</td></tr>`;
  rows.querySelectorAll('button[data-user]').forEach(button => button.addEventListener('click', () => {
    const user = userList.find(u => u.username === button.dataset.user);
    if (button.dataset.act === 'edit') openUserForm(user);
    else deleteUser(user.username);
  }));
  document.getElementById('users-count').innerText = userList ? `${shown.length} of ${userList.length} users` : '';
}

function addChannelRole(channel = '', role = 'mod') {
  const row = document.createElement('div');
  row.className = 'console-input-row chan-role';
  row.innerHTML = `<input type="text" class="form-control" list="usr-channel-list" placeholder="#channel or Network/#channel" aria-label="Channel" value="${esc(channel)}">
    <select class="form-control" aria-label="Role in that channel">${roleOptions(roleKey(role))}</select>
    <button type="button" class="btn btn-red btn-sm" data-icon="trash" aria-label="Remove this channel role" title="Remove"></button>`;
  row.querySelector('button').addEventListener('click', () => row.remove());
  document.getElementById('usr-chan-roles').appendChild(row);
}

// Every field is set each time, so nothing is left over from the account edited before
function openUserForm(user) {
  editingUser = user?.username ?? null;
  const field = id => document.getElementById(id);
  field('users-list').hidden = true;
  field('users-form').hidden = false;
  field('users-message').innerText = '';
  field('users-form-title').innerText = user ? `Edit ${user.username}` : 'Add User';
  field('usr-name').value = editingUser || '';
  field('usr-name').readOnly = !!user;
  field('usr-pwd').value = '';
  field('usr-pwd').placeholder = user ? 'Blank keeps the current one' : 'Required, no spaces';
  field('usr-global-role').innerHTML = roleOptions(roleKey(user?.global_role));
  field('usr-hostmasks').value = (user?.hostmasks || ['*!*@*']).join(', ');
  field('usr-accounts').value = (user?.accounts || []).join(', ');
  field('usr-channel-list').innerHTML = [...new Set(networks().flatMap(n => n.channels || []))].map(c => `<option value="${esc(c)}"></option>`).join('');
  field('usr-chan-roles').innerHTML = '';
  Object.entries(user?.channel_roles || {}).forEach(([c, r]) => addChannelRole(c, r));
  field(user ? 'usr-pwd' : 'usr-name').focus();
}

async function postUser(payload) {
  const res = await fetch('/api/users', { method: 'POST', headers: authJson(), body: JSON.stringify(payload) });
  if (res.status === 401) { sessionExpired(); return false; }
  const data = await res.json().catch(() => ({}));
  if (!res.ok) { alert(data.message || 'The change was refused.'); return false; }
  showUserList(data.message);
  await loadUsers();
  return true;
}

async function saveUserAccount() {
  const username = document.getElementById('usr-name').value.trim();
  const password = document.getElementById('usr-pwd').value;
  if (!username) return alert('Username is required.');

  if (!editingUser) {
    if (!password) return alert('A password is required for a new account.');
    await loadUsers(); // it may have been added from IRC meanwhile
    if (!userList) return alert('Could not read the accounts.');
    // the device would overwrite it
    if (userList.some(u => u.username.toLowerCase() === username.toLowerCase())) return alert(`'${username}' already exists. Edit that account instead.`);
  }

  const channel_roles = {};
  for (const row of document.querySelectorAll('.chan-role')) {
    const channel = row.querySelector('input').value.trim().toLowerCase();
    if (!/^([^/\s,]+\/)?[#&][^\s,]+$/.test(channel)) {
      return alert('Each channel role needs a channel, such as #polibot or SekurNet/#polibot. Fill it in or remove the row.');
    }
    if (channel in channel_roles) return alert(`${channel} is listed twice.`);
    channel_roles[channel] = row.querySelector('select').value;
  }

  const hostmasks = splitList(document.getElementById('usr-hostmasks').value);
  await postUser({
    action: 'save',
    username,
    password: password || undefined,
    global_role: document.getElementById('usr-global-role').value,
    channel_roles,
    hostmasks: hostmasks.length ? hostmasks : ['*!*@*'],
    accounts: splitList(document.getElementById('usr-accounts').value)
  });
}

async function deleteUser(username) {
  if (confirm(`Are you sure you want to delete user '${username}'?`)) await postUser({ action: 'delete', username });
}
