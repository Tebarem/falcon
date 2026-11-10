// Falcon launcher page. Talks to engine/editor/launcher.cpp through window.falcon (JSON).
'use strict';

window.addEventListener('error', e => {
  if (window.falcon && window.falcon.postMessage) {
    window.falcon.postMessage(JSON.stringify({ type: 'ui.error', message: String(e.message), source: `${e.filename}:${e.lineno}` }));
  }
});

const $ = selector => document.querySelector(selector);
const $$ = selector => [...document.querySelectorAll(selector)];

function send(type, payload = {}) {
  if (window.falcon && typeof window.falcon.postMessage === 'function') window.falcon.postMessage(JSON.stringify({ type, ...payload }));
}

function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key === 'html') node.innerHTML = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else node.setAttribute(key, value === true ? '' : value);
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

// 16x16 line icons (the editor's, ui/editor/icons.js).
const ICONS = {
  plus: '<path d="M8 3v10M3 8h10"/>',
  folder: '<path d="M2 4.5a1 1 0 0 1 1-1h3l1.5 1.5H13a1 1 0 0 1 1 1V12a1 1 0 0 1-1 1H3a1 1 0 0 1-1-1z"/>',
  scene: '<path d="M2.5 4a1 1 0 0 1 1-1h9a1 1 0 0 1 1 1v8a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1z"/><path d="M2.5 10.5l3.2-3 2.6 2.4 1.7-1.6 3.5 3"/>',
  close: '<path d="M4 4l8 8M12 4l-8 8"/>',
  winMin: '<path d="M3.5 8h9" stroke-width="1"/>',
  winMax: '<rect x="3.5" y="3.5" width="9" height="9" stroke-width="1"/>',
  winRestore: '<rect x="3.5" y="5.5" width="7" height="7" stroke-width="1"/><path d="M5.5 5.5v-2h7v7h-2" stroke-width="1"/>',
  winClose: '<path d="M4 4l8 8M12 4l-8 8" stroke-width="1"/>',
};
const icon = name => `<svg class="icon" viewBox="0 0 16 16" aria-hidden="true">${ICONS[name] || ''}</svg>`;
function applyIcons(root = document) {
  root.querySelectorAll('[data-icon]').forEach(n => {
    if (!n.dataset.iconDone) {
      n.insertAdjacentHTML('afterbegin', icon(n.dataset.icon));
      n.dataset.iconDone = '1';
    }
  });
}

let state = { recent: [], defaultLocation: '', busy: false, error: '' };

// ---------------------------------------------------------------- window chrome

// The window is borderless: the bar is its title bar (the engine drags the window from it,
// except over the buttons).
function reportChrome() {
  const rect = n => { const b = n.getBoundingClientRect(); return { x: b.left, y: b.top, width: b.width, height: b.height }; };
  send('ui.chrome', { titlebar: rect($('#titlebar')), controls: [rect($('#window-buttons'))] });
}
new ResizeObserver(reportChrome).observe($('#titlebar'));
$('#win-min').addEventListener('click', () => send('ui.window', { action: 'minimize' }));
$('#win-max').addEventListener('click', () => send('ui.window', { action: 'maximize' }));
$('#win-close').addEventListener('click', () => send('ui.window', { action: 'close' }));
function setMaximized(maximized) {
  const b = $('#win-max');
  b.title = maximized ? 'Restore' : 'Maximize';
  b.innerHTML = icon(maximized ? 'winRestore' : 'winMax');
}

// ---------------------------------------------------------------- projects

function ago(seconds) {
  const s = Date.now() / 1000 - seconds;
  if (s < 90) return 'just now';
  if (s < 3600) return `${Math.round(s / 60)} minutes ago`;
  if (s < 86400 * 1.5) return `${Math.round(s / 3600)} hours ago`;
  if (s < 86400 * 45) return `${Math.round(s / 86400)} days ago`;
  return new Date(seconds * 1000).toLocaleDateString();
}

function renderList() {
  const list = $('#list');
  if (!state.recent.length) {
    list.replaceChildren(el('div', { class: 'empty' },
      el('div', {}, 'No projects yet.'),
      el('button', { class: 'button primary', onclick: openNewDialog, html: icon('plus') }, 'New project'),
      el('div', { class: 'hint' }, 'or open one you have with Open.')));
    return;
  }
  list.replaceChildren(...state.recent.map(p => el('div', {
    class: 'project' + (p.exists ? '' : ' missing'),
    title: p.exists ? `Open ${p.dir}` : `${p.dir} is gone`,
    onclick: () => { if (p.exists) send('launcher.open', { path: p.dir }); },
  },
    el('div', { class: 'badge' }, (p.name || '?').slice(0, 1).toUpperCase()),
    el('div', {}, el('div', { class: 'name' }, p.name), el('div', { class: 'path' }, p.dir)),
    el('div', { class: 'when' }, p.exists ? ago(p.opened) : 'Missing'),
    el('button', { class: 'forget', title: 'Remove from the list (the folder stays)', html: icon('close'),
      onclick: e => { e.stopPropagation(); send('launcher.forget', { path: p.dir }); } }))));
}

function render() {
  renderList();
  const err = $('#error');
  const dialogOpen = !$('#new-dialog').hidden;
  err.hidden = !state.error || dialogOpen;
  err.textContent = state.error;
  const newErr = $('#new-error');
  newErr.hidden = !state.error || !dialogOpen;
  newErr.textContent = state.error;
  $('#busy').hidden = !state.busy;
  $('#busy-text').textContent = state.busyText || 'Working…';
  if (!$('#new-location').value) $('#new-location').value = state.defaultLocation;
  updateNewPath();
}

// ---------------------------------------------------------------- new project

function updateNewPath() {
  const name = $('#new-name').value.trim(), loc = $('#new-location').value.trim();
  $('#new-path').textContent = name && loc ? `Creates ${loc.replace(/[\\/]+$/, '')}/${name}` : '';
}
function openNewDialog() {
  state.error = '';
  $('#new-dialog').hidden = false;
  render();
  $('#new-name').focus();
  $('#new-name').select();
}
function closeNewDialog() {
  $('#new-dialog').hidden = true;
  state.error = '';
  render();
}
$('#btn-new').addEventListener('click', openNewDialog);
$('#btn-cancel').addEventListener('click', closeNewDialog);
$('#btn-open').addEventListener('click', () => send('launcher.browse', { purpose: 'open' }));
$('#btn-browse').addEventListener('click', () => send('launcher.browse', { purpose: 'location' }));
$('#new-name').addEventListener('input', updateNewPath);
$('#new-location').addEventListener('input', updateNewPath);
$('#btn-create').addEventListener('click', () => {
  send('launcher.create', { name: $('#new-name').value.trim(), location: $('#new-location').value.trim() });
});
document.addEventListener('keydown', e => {
  if ($('#new-dialog').hidden) return;
  if (e.key === 'Escape') closeNewDialog();
  if (e.key === 'Enter') $('#btn-create').click();
});

// ---------------------------------------------------------------- engine messages

function handle(m) {
  switch (m.type) {
    case 'launcher.state':
      state = m;
      render();
      break;
    case 'launcher.picked':
      if (m.purpose === 'location') {
        $('#new-location').value = m.path;
        updateNewPath();
      }
      break;
    case 'ui.window': setMaximized(!!m.maximized); break;
    case 'launcher.probe': {  // tests: where test.mouse aims
      const n = $(m.at);
      const b = n && n.getBoundingClientRect();
      send('launcher.probed', {
        at: m.at, rect: b ? { x: b.left, y: b.top, width: b.width, height: b.height } : null,
        value: n && 'value' in n ? n.value : null, focused: !!n && document.activeElement === n,
        dialog: !$('#new-dialog').hidden,
      });
      break;
    }
    default: break;
  }
}
window.falcon.onMessage = text => {
  let p;
  try { p = JSON.parse(text); } catch { return; }
  for (const m of p.type === 'ui.batch' ? p.messages : [p]) handle(m);
};

// The page is the whole window: it takes the pointer (there is no game viewport under it), and
// the keyboard's text input while a field has focus.
const isTextField = n => n && (n.tagName === 'INPUT' || n.tagName === 'TEXTAREA');
document.addEventListener('focusin', e => { if (isTextField(e.target)) send('ui.input', { keyboard: true }); });
document.addEventListener('focusout', e => { if (isTextField(e.target)) send('ui.input', { keyboard: false }); });
send('ui.input', { pointer: true });

applyIcons();
render();
reportChrome();
send('ui.window', { action: 'query' });
send('launcher.ready');
