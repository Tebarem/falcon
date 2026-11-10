// Falcon editor page. Talks to engine/editor/editor_bridge.cpp through window.falcon (JSON).
// The engine owns the scene view (tools Q/W/E/R, X, Z, F, camera); this page owns panels,
// menus and every Ctrl shortcut. Keys reach both unless a text field has focus.
'use strict';

// Script errors go to the engine log (and from there to the Console).
window.addEventListener('error', e => {
  if (window.falcon && window.falcon.postMessage) {
    window.falcon.postMessage(JSON.stringify({ type: 'ui.error', message: String(e.message), source: `${e.filename}:${e.lineno}` }));
  }
});

try {  // the whole page: report what breaks it (CEF masks script errors as "Script error.")

// ---------------------------------------------------------------- basics

const $ = selector => document.querySelector(selector);
const $$ = selector => [...document.querySelectorAll(selector)];

// CEF 151's off-screen renderer crashes (a CHECK, exit 0xC0000409; the page goes dead) when the
// page creates a <button> at runtime or gives a <label> a title. So: clickable things are
// <span role="button">, and tooltips go on a wrapping element, never on a <label>.
function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === 'class') node.className = value;
    else if (key === 'html') node.innerHTML = value;
    else if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else if (key in node && typeof value !== 'string') node[key] = value;
    else node.setAttribute(key, value === true ? '' : value);
  }
  for (const child of children.flat()) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

function send(type, payload = {}) {
  if (window.falcon && typeof window.falcon.postMessage === 'function') {
    window.falcon.postMessage(JSON.stringify({ type, ...payload }));
  }
}

function setText(node, text) {
  if (node.textContent !== text) node.textContent = text;  // unchanged text: no repaint
}

// Page preferences live in the engine's editor prefs (cef-lite runs without a disk cache, so
// localStorage would not survive a restart). They arrive with editor.prefs on connect.
let pagePrefs = {};
let prefsLoaded = false;  // (none with --no-prefs: tests)
const store = {
  get(key, fallback) { return key in pagePrefs ? pagePrefs[key] : fallback; },
  set(key, value) {
    pagePrefs[key] = value;
    send('editor.prefs.set', { key, value });
  },
};

function fmt(value, digits = 3) {
  if (!Number.isFinite(value)) return '0';
  const s = Number(value).toFixed(digits);
  return s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s;
}

const isTextField = node => !!node && (
  (node.tagName === 'INPUT' && !['checkbox', 'range', 'button'].includes(node.type)) ||
  node.tagName === 'TEXTAREA' || node.tagName === 'SELECT' || node.isContentEditable);

// ---------------------------------------------------------------- state

const state = {
  scene: '', entities: [], entityCount: 0, selectedEntity: 0, inspector: null,
  playing: false, paused: false, canUndo: false, canRedo: false, dirty: false,
  tools: { tool: 'move', local: false, pivotCenter: false, grid: true, gameView: false, snapMove: 0.25, snapRotate: 15, snapScale: 0.1 },
  settings: null, assets: [], terrain: null,
};
const ui = {
  search: '', collapsedRoot: false, collapsed: new Set(), seenSkeletons: new Set(), renaming: 0, maximizeOnPlay: store.get('maxplay', false),
  showStats: store.get('stats', false), logs: [], logFilter: { info: true, warning: true, error: true },
  logCollapse: store.get('collapse', false), logSearch: '', selectedLog: -1, lastStats: null,
};
const entityById = new Map();
const selected = () => state.selectedEntity ? entityById.get(state.selectedEntity) : null;

// ---------------------------------------------------------------- layout: viewport, input capture

const viewport = $('#viewport');
let lastLayout = '';
function reportLayout() {
  const r = viewport.getBoundingClientRect();
  const rect = n => { const b = n.getBoundingClientRect(); return { x: b.left, y: b.top, width: b.width, height: b.height }; };
  const exclude = $$('#viewport .overlay').filter(n => !n.hidden && n.offsetParent).map(rect);
  // Other views the engine draws (the Animator's preview: [data-view] elements shown).
  const views = $$('[data-view]').filter(n => n.offsetParent && n.getBoundingClientRect().width > 0)
    .map(n => ({ name: n.dataset.view, ...rect(n) }));
  const layout = { viewport: { x: r.left, y: r.top, width: r.width, height: r.height }, exclude, scene: !viewport.hidden, views };
  const key = JSON.stringify(layout);
  if (key !== lastLayout) {
    lastLayout = key;
    send('ui.layout', layout);
  }
}
new ResizeObserver(reportLayout).observe(viewport);
window.addEventListener('resize', reportLayout);

// While a menu or dialog is open, the page takes all pointer input (a click outside closes it
// instead of reaching the scene); while a text field has focus, the keyboard is the page's.
let pointerCaptured = false;
function capturePointer(on) {
  if (on !== pointerCaptured) {
    pointerCaptured = on;
    send('ui.input', { pointer: on });
  }
}
document.addEventListener('focusin', e => { if (isTextField(e.target)) send('ui.input', { keyboard: true }); });
document.addEventListener('focusout', e => { if (isTextField(e.target)) send('ui.input', { keyboard: false }); });
// Double-click selects a whole field (Chromium would select one word), ready to type over.
document.addEventListener('dblclick', e => {
  const t = e.target;
  if (t.tagName === 'INPUT' && isTextField(t)) t.select();
});

// While a mouse button is held, panels that rebuild on engine state wait for the release:
// rebuilding mid-drag would replace the slider (or field) under the mouse and end the drag.
let pressTarget = null;
const deferredRenders = new Set();
const pressedIn = root => !!pressTarget && root.contains(pressTarget);
const deferRender = render => deferredRenders.add(render);
document.addEventListener('mousedown', e => { pressTarget = e.target; }, true);
document.addEventListener('mouseup', () => {
  pressTarget = null;
  const renders = [...deferredRenders];
  deferredRenders.clear();
  for (const render of renders) render();
}, true);

// ---------------------------------------------------------------- window chrome

// The window is borderless: this bar is its title bar. The engine drags the window from it
// (and gives Windows snap / double-click maximize), except over the controls listed here.
const appBar = $('.app-toolbar');
let lastChrome = '';
function reportChrome() {
  const rect = n => { const b = n.getBoundingClientRect(); return { x: b.left, y: b.top, width: b.width, height: b.height }; };
  const controls = [...$$('#menubar > button'), $('.transport'), $('#window-buttons'), ...$$('.app-right .pill')].map(rect);
  const chrome = { titlebar: rect(appBar), controls };
  const key = JSON.stringify(chrome);
  if (key !== lastChrome) {
    lastChrome = key;
    send('ui.chrome', chrome);
  }
}
new ResizeObserver(reportChrome).observe(appBar);
window.addEventListener('resize', reportChrome);
$('#win-min').addEventListener('click', () => send('ui.window', { action: 'minimize' }));
$('#win-max').addEventListener('click', () => send('ui.window', { action: 'maximize' }));
$('#win-close').addEventListener('click', () => send('ui.window', { action: 'close' }));
function setMaximized(maximized) {
  const b = $('#win-max');
  b.title = maximized ? 'Restore' : 'Maximize';
  b.innerHTML = icon(maximized ? 'winRestore' : 'winMax');
}

// ---------------------------------------------------------------- splitters and docks

const workspace = $('#workspace');
const sizes = Object.assign({ left: 260, right: 330, bottom: 230 }, store.get('sizes', {}));
function applySizes() {
  workspace.style.setProperty('--left', sizes.left + 'px');
  workspace.style.setProperty('--right', sizes.right + 'px');
  workspace.style.setProperty('--bottom', sizes.bottom + 'px');
}
applySizes();
$$('.splitter').forEach(splitter => {
  splitter.addEventListener('mousedown', down => {
    down.preventDefault();
    const which = splitter.dataset.split;
    const start = { x: down.clientX, y: down.clientY, size: sizes[which] };
    splitter.classList.add('dragging');
    const move = e => {
      const d = which === 'bottom' ? start.y - e.clientY : which === 'right' ? start.x - e.clientX : e.clientX - start.x;
      const max = which === 'bottom' ? window.innerHeight - 200 : window.innerWidth - 420;
      sizes[which] = Math.max(140, Math.min(max, start.size + d));
      applySizes();
    };
    const up = () => {
      splitter.classList.remove('dragging');
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      store.set('sizes', sizes);
    };
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
});

function showTab(dock, tab) {
  const root = typeof dock === 'string' ? $(`[data-dock="${dock}"]`) : dock;
  root.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.tab === tab));
  root.querySelectorAll('.dock-body').forEach(b => { b.hidden = b.dataset.body !== tab; });
  if (root.dataset.dock === 'scene') {
    const game = tab === 'game', animator = tab === 'animator';
    $('#scene-toolbar').hidden = game || animator;
    $('#game-toolbar').hidden = !game;
    viewport.hidden = animator;
    $('#animator').hidden = !animator;
    Animator.shown(animator);
    $('#tools-overlay').hidden = game;
    $('#stats-overlay').hidden = !(game && ui.showStats);
    $('#game-hud').classList.toggle('beside-tools', !game);
    if (!animator && state.tools.gameView !== game) sendTools({ gameView: game });
    reportLayout();
  }
  if (root.dataset.dock === 'bottom') {
    Profiler.shown(tab === 'profiler');
    NetworkPanel.shown(tab === 'network');
    Assets.shown(tab === 'project');
  }
  if (tab !== 'animator') store.set('tab.' + root.dataset.dock, tab);  // (the Animator opens with a graph)
}
$$('.dock').forEach(dock => {
  dock.querySelectorAll('.tab').forEach(t => t.addEventListener('click', () => showTab(dock, t.dataset.tab)));
});
function restoreTabs() {
  $$('.dock').forEach(dock => {
    const saved = store.get('tab.' + dock.dataset.dock, null);
    if (saved && dock.querySelector(`.tab[data-tab="${saved}"]`)) showTab(dock, saved);
  });
}

let hoveredDock = null;
document.addEventListener('mousemove', e => { hoveredDock = e.target.closest ? e.target.closest('.dock') : null; }, true);
function toggleMaximize(dock = hoveredDock || $('#dock-scene')) {
  const on = !dock.classList.contains('maximized');
  $$('.dock.maximized').forEach(d => d.classList.remove('maximized'));
  dock.classList.toggle('maximized', on);
  document.body.classList.toggle('dock-maximized', on);
  reportLayout();
}
$('#btn-maximize-scene').addEventListener('click', () => toggleMaximize($('#dock-scene')));

// ---------------------------------------------------------------- menus

let openMenuState = null;  // {node, anchor, subs: [submenu nodes, by depth]}
function closeMenu() {
  if (!openMenuState) return;
  openMenuState.node.remove();
  openMenuState.subs.forEach(n => n.remove());
  if (openMenuState.anchor) openMenuState.anchor.classList.remove('open');
  openMenuState = null;
  capturePointer(!!$('.modal-back'));
}
function closeSubmenus(depth) {  // keeps submenus above `depth` (1 = the first level)
  while (openMenuState && openMenuState.subs.length >= depth) {
    const n = openMenuState.subs.pop();
    if (n.owner) n.owner.classList.remove('open');
    n.remove();
  }
}
function placeMenu(node, x, y, flipX = null) {
  document.body.append(node);
  const r = node.getBoundingClientRect();
  if (flipX !== null && x + r.width > window.innerWidth - 4) x = flipX - r.width;  // no room on the right: open leftwards
  node.style.left = Math.max(4, Math.min(x, window.innerWidth - r.width - 4)) + 'px';
  node.style.top = Math.max(4, Math.min(y, window.innerHeight - r.height - 4)) + 'px';
}
// items: {label, shortcut, action, disabled, checked, icon, value (data-value, for tests), submenu: [items]},
// '-' for a separator or {heading} for a section's title. A submenu opens beside its item on hover or click.
function menuNode(items, depth) {
  const node = el('div', { class: 'menu' });
  for (const item of items) {
    if (item === '-') { node.append(el('div', { class: 'menu-sep' })); continue; }
    if (item.heading) { node.append(el('div', { class: 'menu-heading' }, item.heading)); continue; }
    const disabled = typeof item.disabled === 'function' ? item.disabled() : item.disabled;
    const checked = typeof item.checked === 'function' ? item.checked() : item.checked;
    const sub = item.submenu;
    const row = el('div', { class: 'menu-item' + (disabled ? ' disabled' : '') + (sub ? ' has-sub' : '') + (checked ? ' checked' : ''), 'data-value': item.value },
      el('span', { html: checked ? '✓' : item.icon ? icon(item.icon) : '' }),
      el('span', {}, item.label),
      sub ? el('span', { class: 'sub-arrow', html: icon('chevRight') }) : el('kbd', {}, item.shortcut || ''));
    const openSub = () => {
      if (!openMenuState || (openMenuState.subs[depth] && openMenuState.subs[depth].owner === row)) return;
      closeSubmenus(depth + 1);
      const child = menuNode(sub, depth + 1);
      child.owner = row;
      row.classList.add('open');
      const r = row.getBoundingClientRect();
      placeMenu(child, r.right + 2, r.top - 4, r.left - 2);
      openMenuState.subs.push(child);
    };
    row.addEventListener('mousedown', e => e.stopPropagation());
    row.addEventListener('mouseenter', () => { if (sub && !disabled) openSub(); else closeSubmenus(depth + 1); });
    row.addEventListener('click', () => {
      if (disabled) return;
      if (sub) { openSub(); return; }
      closeMenu();
      item.action();
    });
    node.append(row);
  }
  return node;
}
function openMenu(items, x, y, anchor = null) {
  closeMenu();
  const node = menuNode(items, 0);
  placeMenu(node, x, y);
  if (anchor) anchor.classList.add('open');
  openMenuState = { node, anchor, subs: [] };
  capturePointer(true);
}
document.addEventListener('mousedown', e => {
  if (openMenuState && !openMenuState.node.contains(e.target) && !openMenuState.subs.some(n => n.contains(e.target))) closeMenu();
});

function menuBelow(button, items) {
  const r = button.getBoundingClientRect();
  openMenu(items, r.left, r.bottom + 2, button);
}

// A <select>'s list is a native popup widget, which the off-screen browser never draws (cef-lite
// has no popup surface), so every select opens this page's menu instead: a pick sets its value
// and fires 'change' as the native list would. The keyboard (arrows) still works natively.
document.addEventListener('mousedown', e => {
  const sel = e.button === 0 && e.target.closest ? e.target.closest('select') : null;
  if (!sel || sel.disabled) return;
  e.preventDefault();  // no native popup
  e.stopPropagation();  // (the outside-click handler above would close the menu again)
  sel.focus();
  if (openMenuState && openMenuState.anchor === sel) { closeMenu(); return; }
  menuBelow(sel, [...sel.options].map(o => ({
    label: o.textContent, checked: o.selected, disabled: o.disabled,
    action: () => {
      if (sel.value === o.value) return;
      sel.value = o.value;
      sel.dispatchEvent(new Event('change', { bubbles: true }));
    },
  })));
  const menu = openMenuState.node;
  menu.classList.add('select-menu');
  menu.style.minWidth = sel.getBoundingClientRect().width + 'px';
  // A long list: a search field on top (typing filters, Enter picks the first match).
  if (sel.options.length > 12) {
    const search = el('input', { class: 'field menu-search', placeholder: 'Search…', spellcheck: 'false' });
    const rows = [...menu.querySelectorAll('.menu-item')];
    search.addEventListener('input', () => {
      const q = search.value.trim().toLowerCase();
      for (const row of rows) row.hidden = !!q && !row.textContent.toLowerCase().includes(q);
    });
    search.addEventListener('keydown', e => {
      if (e.key !== 'Enter') return;
      const first = rows.find(row => !row.hidden && !row.classList.contains('disabled'));
      if (first) first.click();
    });
    menu.prepend(search);
    setTimeout(() => search.focus());
  }
  // It scrolls, below the field or above it (whichever has room), the current choice in view.
  const r = sel.getBoundingClientRect();
  const below = window.innerHeight - r.bottom - 6, above = r.top - 6;
  const up = below < Math.min(menu.scrollHeight, 240) && above > below;
  menu.style.maxHeight = Math.max(120, Math.min(480, up ? above : below)) + 'px';
  const h = menu.getBoundingClientRect().height;
  menu.style.top = Math.max(4, up ? r.top - 2 - h : r.bottom + 2) + 'px';
  const current = menu.querySelector('.menu-item.checked');
  if (current) menu.scrollTop = current.offsetTop - (menu.clientHeight - current.offsetHeight) / 2;
}, true);

// The create menu (GameObject menu, the Hierarchy's + button and right-click), Unity's layout.
// `parent`: create under that entity (the row menu's Create Child).
const SHAPES = [['cube', 'Cube'], ['sphere', 'Sphere'], ['capsule', 'Capsule'], ['cylinder', 'Cylinder'], ['plane', 'Plane'],
  ['quad', 'Quad'], ['cone', 'Cone']];
function createMenu(parent = 0) {
  return [
    { label: 'Create Empty', icon: 'empty', shortcut: parent ? '' : 'Ctrl+Shift+N', action: () => cmd.createEmpty(parent) },
    '-',
    { label: '3D Object', icon: 'cube', submenu: SHAPES.map(([p, label]) => ({ label, icon: p, action: () => cmd.create(p, parent) })) },
    { label: 'Level', icon: 'levelTools', value: 'Level', submenu: [  // editable meshes and the level tools' create commands
      { label: 'Level Box', icon: 'cube', value: 'Level Box', action: () => cmd.createLevel('box', parent) },
      { label: 'Level Plane', icon: 'plane', value: 'Level Plane', action: () => cmd.createLevel('plane', parent) },
      ...(window.levelTools ? window.levelTools.createItems() : []),
    ] },
    { label: 'Light', icon: 'light', submenu: [
      { label: 'Directional Light', icon: 'sun', action: () => cmd.createLight('directional', parent) },
      { label: 'Point Light', icon: 'light', action: () => cmd.createLight('point', parent) },
      { label: 'Spot Light', icon: 'spot', action: () => cmd.createLight('spot', parent) },
    ] },
    { label: 'Camera', icon: 'camera', action: () => cmd.createCamera(parent) },
    { label: 'Effects', icon: 'sparkle', submenu: [
      { label: 'Particle Effect', icon: 'sparkle', action: () => send('editor.entity.create', { kind: 'empty', name: 'Particle Effect', vfx: 'effect', parent }) },
      { label: 'Decal', icon: 'decal', action: () => send('editor.entity.create', { kind: 'empty', name: 'Decal', vfx: 'decal', parent }) },
    ] },
    { label: 'Water', icon: 'water', submenu: [
      { label: 'Lake', icon: 'water', action: () => cmd.createWater('lake', parent) },
      { label: 'Pond', icon: 'water', action: () => cmd.createWater('pond', parent) },
      { label: 'Pool', icon: 'water', action: () => cmd.createWater('pool', parent) },
      { label: 'Ocean', icon: 'water', action: () => cmd.createWater('ocean', parent) },
      { label: 'River', icon: 'water', action: () => cmd.createWater('river', parent) },
      { label: 'Tank', icon: 'water', action: () => cmd.createWater('volume', parent) },
    ] },
    // (the terrain has no parent: top-level menus only)
    ...(parent ? [] : ['-', { label: 'Terrain', icon: 'mountain', action: () => showTab('inspector', 'terrain') }]),
  ];
}

// ---------------------------------------------------------------- commands and shortcuts

// Another scene of the project (or the launcher) is another editor: with unsaved changes, ask
// first (Save / Don't save / Cancel); `run(save)` sends the request.
function withSaveChoice(run) {
  const unsaved = state.dirty || (state.terrain && state.terrain.dirty);
  if (!unsaved) return run(false);
  dialog(`Save the changes to ${state.scene}?`, [el('div', { class: 'modal-text' }, 'Your changes are lost if you don\'t save them.')], [
    { label: 'Cancel' },
    { label: 'Don\'t save', action: () => run(false) },
    { label: 'Save', primary: true, action: () => run(true) },
  ]);
}

// A small dialog: a title, body nodes, buttons {label, primary, action} (each closes it).
function dialog(title, body, buttons) {
  closeMenu();
  closeShortcuts();
  const back = el('div', { class: 'modal-back', onmousedown: e => { if (e.target === back) closeShortcuts(); } },
    el('div', { class: 'modal dialog' },
      el('div', { class: 'modal-head' }, title),
      el('div', { class: 'dialog-body' }, body),
      el('div', { class: 'modal-actions' }, buttons.map(b => el('span', {
        class: 'tbtn wide' + (b.primary ? ' primary' : ''), role: 'button',
        onclick: () => { closeShortcuts(); if (b.action) b.action(); },
      }, b.label)))));
  document.body.append(back);
  capturePointer(true);
  const first = back.querySelector('input');
  if (first) { first.focus(); first.select(); }
}

const sceneLabel = path => path.replace(/^Scenes\//, '').replace(/\.fpak$/, '');

// Editor windows of editor code ([EditorWindow]): a library's (or the project's) HTML page in a
// frame of a floating window. The page talks to its C# side through this page: its messages
// (falcon-panel.js: postMessage) go to the engine as editor.window.message, the C# side's
// (editor.window.send) to the page; the editor's colours go to it when it loads.
const editorWindows = new Map();  // menu -> { node, frame }
const THEME_VARS = ['--app', '--panel', '--header', '--toolbar', '--list', '--field', '--field-hover', '--field-line', '--button', '--button-hover',
  '--hover', '--select', '--line', '--line-soft', '--text', '--text-dim', '--text-faint', '--text-strong', '--accent', '--warn', '--error'];
function postTheme(frame) {
  const cs = getComputedStyle(document.documentElement);
  const vars = {};
  for (const v of THEME_VARS) vars[v] = cs.getPropertyValue(v).trim();
  if (frame.contentWindow) frame.contentWindow.postMessage({ falcon: 'theme', vars }, '*');
}
function openEditorWindow(w) {
  const open = editorWindows.get(w.menu);
  if (open) { document.body.append(open.node); return; }  // (to the front)
  const frame = el('iframe', { class: 'ext-frame', src: w.url, title: w.title });
  const close = el('span', { class: 'tbtn ext-close', role: 'button', html: icon('close'), title: 'Close' });
  const head = el('div', { class: 'ext-head' }, el('span', { class: 'ext-title' }, w.title), el('span', { class: 'spacer' }), close);
  const node = el('div', { class: 'ext-window', 'data-window': w.menu }, head, frame);
  const n = editorWindows.size;
  node.style.width = (w.width || 360) + 'px';
  node.style.height = (w.height || 420) + 32 + 'px';
  node.style.left = Math.max(20, innerWidth / 2 - (w.width || 360) / 2 + n * 24) + 'px';
  node.style.top = Math.max(40, innerHeight / 2 - ((w.height || 420) + 32) / 2 + n * 24) + 'px';
  head.addEventListener('mousedown', down => {  // (dragged by its title bar)
    if (down.button !== 0 || down.target.closest('.ext-close')) return;
    down.preventDefault();
    const x0 = down.clientX - node.offsetLeft, y0 = down.clientY - node.offsetTop;
    frame.style.pointerEvents = 'none';
    const move = e => { node.style.left = (e.clientX - x0) + 'px'; node.style.top = Math.max(0, e.clientY - y0) + 'px'; };
    const up = () => { removeEventListener('mousemove', move); removeEventListener('mouseup', up); frame.style.pointerEvents = ''; };
    addEventListener('mousemove', move);
    addEventListener('mouseup', up);
  });
  close.addEventListener('click', () => closeEditorWindow(w.menu));
  frame.addEventListener('load', () => postTheme(frame));
  document.body.append(node);
  editorWindows.set(w.menu, { node, frame });
  send('editor.window.open', { window: w.menu });
}
function closeEditorWindow(menu) {
  const open = editorWindows.get(menu);
  if (!open) return;
  open.node.remove();
  editorWindows.delete(menu);
  send('editor.window.close', { window: menu });
}
addEventListener('message', e => {
  const gameUi = document.getElementById('game-ui');
  if (e.data && e.data.falcon === 'ui' && gameUi && e.source === gameUi.contentWindow) {  // (the game's page: Ui.TryReceive)
    send('editor.gameui.message', { data: String(e.data.data) });
    return;
  }
  if (!e.data || e.data.falcon !== 'panel') return;
  for (const [menu, open] of editorWindows)
    if (e.source === open.frame.contentWindow)
      send('editor.window.message', { window: menu, data: JSON.stringify(e.data.data === undefined ? null : e.data.data) });
});

// The Library Manager (File > Libraries…): the project's libraries (Libraries/<name>: code built
// into the game, editor tools, UI, assets), each trusted or not, on or off; add one from a folder,
// a zip or a git repository, make a new one, export one as a zip to share, remove one.
let librariesNode = null;
function librariesDialog() {
  const data = state.libraries || { libraries: [] };
  const row = l => {
    const parts = [l.code && 'code', l.editor && 'editor tools', l.ui && 'UI', l.assets && 'assets'].filter(Boolean).join(', ');
    const status = l.error ? l.error : !l.enabled ? 'Off' : l.code && !l.trusted ? 'Not trusted: its code is not built' : 'On';
    const buttons = [];
    if (l.code && !l.trusted)
      buttons.push(actionButton('Trust', () => dialog('Trust ' + l.name + '?', [el('div', { class: 'modal-text' },
        'Its code will be built into the game and run with full access to your computer, as your own scripts are. Only trust code you know.')],
      [{ label: 'Cancel', action: () => cmd.libraries() }, { label: 'Trust', primary: true, action: () => send('editor.libraries.trust', { name: l.name, trust: true }) }])));
    buttons.push(actionButton(l.enabled ? 'Turn Off' : 'Turn On', () => send('editor.libraries.enable', { name: l.name, enabled: !l.enabled })));
    buttons.push(actionButton('Export', () => send('editor.libraries.export', { name: l.name })));
    buttons.push(actionButton('Remove', () => dialog('Remove ' + l.name + '?', [el('div', { class: 'modal-text' },
      'Its folder (Libraries/' + l.name + ') is deleted. Entities using its components keep their values, with a warning.')],
    [{ label: 'Cancel', action: () => cmd.libraries() }, { label: 'Remove', primary: true, action: () => send('editor.libraries.remove', { name: l.name }) }])));
    return el('div', { class: 'library-row' + (l.error ? ' bad' : ''), 'data-library': l.name },
      el('div', { class: 'library-head' }, el('span', { class: 'library-name' }, l.name), el('span', { class: 'library-version' }, l.version),
        el('span', { class: 'spacer' }), el('span', { class: 'library-status' }, status)),
      l.description ? el('div', { class: 'modal-text' }, l.description) : null,
      el('div', { class: 'library-meta' }, [parts || 'empty', l.author && 'by ' + l.author, l.source && 'from ' + (l.ref || l.source)].filter(Boolean).join(' · ')),
      el('div', { class: 'library-actions' }, buttons));
  };
  const list = el('div', { class: 'library-list' }, data.libraries.length ? data.libraries.map(row)
    : el('div', { class: 'modal-text' }, 'No libraries yet: add one, or make one to share your own tools.'));
  dialog('Libraries', [list, data.installing ? el('div', { class: 'modal-text' }, 'Adding a library…') : null].filter(Boolean), [
    { label: 'New…', action: () => cmd.newLibrary() },
    { label: 'Add Folder…', action: () => cmd.addLibrary('folder') },
    { label: 'Add Zip…', action: () => cmd.addLibrary('zip') },
    { label: 'Add from Git…', action: () => cmd.addLibrary('git') },
    { label: 'Close', primary: true, action: () => { librariesNode = null; } },
  ]);
  librariesNode = document.querySelector('.modal-back:last-of-type');
}

const manySelected = () => (state.selection || []).length > 1;
const cmd = {
  libraries: () => { send('editor.libraries.list'); librariesDialog(); },
  newLibrary: () => {
    const name = el('input', { class: 'field', value: 'MyLibrary', spellcheck: 'false' });
    const create = () => send('editor.libraries.create', { name: name.value.trim() });
    name.addEventListener('keydown', e => { if (e.key === 'Enter') { closeShortcuts(); create(); } });
    dialog('New library', [el('div', { class: 'modal-text' }, 'Libraries/<name>: Code (C# built into the game), Editor, UI and Assets, and a library.json. Export it to share it.'), name],
      [{ label: 'Cancel', action: () => cmd.libraries() }, { label: 'Create', primary: true, action: create }]);
  },
  // Adding one trusts its code: said first (a dialog asks for the folder or the zip after).
  addLibrary: source => {
    const url = el('input', { class: 'field', placeholder: 'https://github.com/someone/library.git', spellcheck: 'false' });
    const ref = el('input', { class: 'field', placeholder: 'branch or tag (optional)', spellcheck: 'false' });
    const add = () => send('editor.libraries.add', source === 'git' ? { source, path: url.value.trim(), ref: ref.value.trim() } : { source });
    dialog('Add a library', [el('div', { class: 'modal-text' },
      'A library\'s code is built into the game and runs with full access to your computer, as your own scripts are. Only add libraries from people you trust.'),
    ...(source === 'git' ? [url, ref] : [])], [{ label: 'Cancel', action: () => cmd.libraries() }, { label: 'Add', primary: true, action: add }]);
  },
  save: () => Animator.active() ? Animator.save() : send('editor.scene.save'),  // (the Animator's graph while it is open)
  newScene: (folder = 'Scenes') => {
    const name = el('input', { class: 'field', value: 'New Scene', spellcheck: 'false' });
    const create = () => withSaveChoice(save => send('editor.project.newScene', { name: name.value.trim(), folder, save }));
    name.addEventListener('keydown', e => { if (e.key === 'Enter') { closeShortcuts(); create(); } });
    dialog('New scene', [el('div', { class: 'modal-text' }, `An empty scene in ${folder || 'the project\'s folder'}. The editor reopens on it.`), name], [
      { label: 'Cancel' },
      { label: 'Create', primary: true, action: create },
    ]);
  },
  openScene: scene => withSaveChoice(save => send('editor.project.openScene', { scene, save })),
  openLauncher: () => withSaveChoice(save => send('editor.project.openLauncher', { save })),
  reload: () => send('editor.scene.reload'),
  undo: () => Animator.active() ? Animator.undo() : send('editor.undo'),  // (the Animator's graph while it is open)
  redo: () => Animator.active() ? Animator.redo() : send('editor.redo'),
  // (Several selected: no id, so the engine takes every one of them, one undo step.)
  duplicate: () => { if (state.selectedEntity) send('editor.entity.duplicate', manySelected() ? {} : { id: state.selectedEntity }); },
  remove: () => {
    if (state.selectedEntity) send('editor.entity.delete', manySelected() ? {} : { id: state.selectedEntity });
    else if (state.terrain?.scatter?.picked) send('editor.terrain.scatter.exclude');
  },
  rename: () => { if (state.selectedEntity) startRename(state.selectedEntity); },
  frame: () => { if (state.selectedEntity) send('editor.frame', { id: state.selectedEntity }); },
  deselect: () => state.tools.tool === 'brush' ? sendTools({ tool: 'move' }) : send('editor.select', { id: 0, escape: true }),
  create: (primitive, parent = 0) => send('editor.entity.create', { kind: 'primitive', primitive, parent }),
  createEmpty: (parent = 0) => send('editor.entity.create', { kind: 'empty', parent }),
  createLight: (light, parent = 0) => send('editor.entity.create', { kind: 'light', light, parent }),
  createCamera: (parent = 0) => send('editor.entity.create', { kind: 'camera', parent }),
  createWater: (water, parent = 0) => send('editor.entity.create', { kind: 'water', primitive: water, parent }),
  createLevel: (shape, parent = 0) => send('editor.entity.create', { kind: 'level', shape, parent }),
  // (Waiting for the scripts' build to play: pressing it again calls Play off.)
  play: () => send(state.playing || (state.scripts && state.scripts.playPending) ? 'editor.stop' : 'editor.play'),
  buildScripts: () => send('editor.scripts.build'),
  buildGame: () => send('editor.game.build'),
  // SteamPipe: the built game to Steam by steamcmd, in a window of its own where you sign in.
  uploadToSteam: () => {
    const user = el('input', { class: 'field', placeholder: 'your Steamworks account name', spellcheck: 'false', value: store.get('steam.user') || '' });
    const description = el('input', { class: 'field', placeholder: 'what this build is (optional)', spellcheck: 'false' });
    const branch = el('input', { class: 'field', placeholder: 'branch to set live (optional)', spellcheck: 'false', value: (state.settings || {}).steamBranch || '' });
    const upload = () => {
      store.set('steam.user', user.value.trim());
      send('editor.steam.upload', { user: user.value.trim(), description: description.value.trim(), branch: branch.value.trim() });
    };
    dialog('Upload to Steam', [el('div', { class: 'modal-text' },
      `The last Build Game goes to App ID ${(state.settings || {}).steamAppId}'s depot through SteamPipe. steamcmd opens in a window of its own: ` +
      'sign in there with your password and Steam Guard code (they never pass through the editor). Then set the build live on Steamworks, or name a branch here.'),
      user, description, branch], [{ label: 'Cancel' }, { label: 'Upload', primary: true, action: upload }]);
  },
  // Settings saved with the project (project.falcon), for the editor's view and the game: applied
  // as they change.
  projectSettings: () => {
    const defaults = { lodPixels: 0.5, cullPixels: 4, terrainPixels: 1, detailDistance: 10, audioMaster: 1, audioSfx: 1, audioPhysics: 1, audioAmbience: 1, audioMusic: 1,
      audioUi: 1, audioHrtf: true, audioReflections: 2, audioReverbType: 2, audioReverb: 1, audioPhysicsSounds: true, audioWaterSounds: true, audioVariation: true, netPreset: 'competitive' };
    const body = () => [
      el('div', { class: 'modal-text' }, 'Saved with the project, for the editor and the game alike. Changes show at once.'),
      el('div', { class: 'section-title' }, 'Level of detail'),
      tip(sliderProp('Full detail within (m)', 'detailDistance', 0, 200, 0.5, 1),
        'Nearer than this to the camera, meshes and terrain keep their full detail and nothing is left out for being small. Beyond it, the limits below apply. 0: they apply everywhere.'),
      tip(sliderProp('LOD error (px)', 'lodPixels', 0, 8, 0.05),
        'How far simplified geometry may stray from the full mesh on screen, in pixels. Lower keeps more detail at a distance (more triangles); 0 draws the full meshes.'),
      tip(sliderProp('Cull size (px)', 'cullPixels', 0, 32, 0.5),
        'Objects smaller than this on screen (their radius, in pixels) are not drawn; glowing ones always are. 0 draws everything.'),
      tip(sliderProp('Terrain error (px)', 'terrainPixels', 0.1, 16, 0.05),
        "How far the terrain's coarser patches may stray from its full heights on screen, in pixels. Lower keeps more terrain detail at a distance."),
      el('div', { class: 'section-title' }, 'Audio'),
      tip(sliderProp('Master volume', 'audioMaster', 0, 2, 0.01), 'Everything the game plays (1: as mixed)'),
      ...[['audioSfx', 'Effects'], ['audioPhysics', 'Physics'], ['audioAmbience', 'Ambience'], ['audioMusic', 'Music'], ['audioUi', 'UI']]
        .map(([key, label]) => tip(sliderProp(label, key, 0, 2, 0.01), `The ${label.toLowerCase()} bus: sounds set to play on it`)),
      tip(toggleProp('HRTF (3D on headphones)', 'audioHrtf'),
        `Steam Audio places 3D sounds around the head (best on headphones); off, they are panned left and right. Now: ${state.settings.audioSpatializer || 'off'}`),
      tip(selectProp('Reflections', 'audioReflections', [['0', 'Off'], ['1', 'Low'], ['2', 'Medium'], ['3', 'High']]),
        'Echoes and reverb from the scene\'s geometry and acoustic materials, simulated by Steam Audio on a thread of its own. Higher: more rays and bounces, a longer tail, more sounds with echoes of their own'),
      tip(sliderProp('Reverb level', 'audioReverb', 0, 2, 0.01), 'How loud the reflections and reverb are'),
      tip(toggleProp('Physics sounds', 'audioPhysicsSounds'), 'Impacts, scrapes and rolls from what the physics simulates (on the physics bus)'),
      tip(toggleProp('Water sounds', 'audioWaterSounds'), 'Splashes, sloshing, and the world muffled under water'),
      tip(toggleProp('Sound variation', 'audioVariation'),
        "Every one-shot sound plays a little different each time (about ±1 dB and ±0.5 semitone; with several clips, never the same one twice running) unless it varies on its own. Loops and music are left as they are; a sound's or an Audio Source's Vary turns it off."),
      tip(selectProp('Reverb type', 'audioReverbType', [['0', 'Convolution'], ['1', 'Parametric'], ['2', 'Hybrid']]),
        'Convolution: the simulated echoes exactly; parametric: a smooth tail of the right length; hybrid: convolution, then a parametric tail (cheaper). Takes effect at the next start'),
      el('div', { class: 'section-title' }, 'Navigation'),
      tip(sliderProp('Agent height (m)', 'navAgentHeight', 0.2, 4, 0.05), 'The navigation mesh is baked for an agent this tall: no way under anything lower'),
      tip(sliderProp('Agent radius (m)', 'navAgentRadius', 0.05, 2, 0.01), 'How far from walls and edges it keeps'),
      tip(sliderProp('Step height (m)', 'navAgentClimb', 0, 2, 0.01), 'The highest step it walks up'),
      tip(sliderProp('Max slope (°)', 'navMaxSlope', 0, 89, 1, 0), 'Steeper ground is not walkable'),
      tip(sliderProp('Cell size (m)', 'navCellSize', 0.05, 1, 0.01), 'How finely it is baked (smaller: closer to the level, slower)'),
      tip(toggleProp('Bake as you build', 'navAutoBake'), 'Once a scene has one, its navigation mesh is baked again a moment after the level changes'),
      el('div', { class: 'section-title' }, 'Network'),
      tip(selectTextProp('Preset', 'netPreset', [['competitive', 'Competitive (64 players, 30 snapshots/s)'], ['large', 'Large battle (1,000 players, 20/s)'],
        ['mmo', 'MMO / survival (2,000 players, 10/s)'], ['rollback', 'Rollback (2-8 players, inputs only)'], ['custom', 'Custom']], () => setTimeout(cmd.projectSettings, 0)),
        'Sets the values below for a kind of game; changing one makes it Custom'),
      tip(selectTextProp('Mode', 'netMode', [['authoritative', 'Authoritative (the host simulates; clients predict their own)'], ['rollback', 'Rollback (everyone simulates everything from inputs)']]),
        "Authoritative: the host's simulation is the truth, sent to clients (any number of players, late join). Rollback: every machine simulates the whole world, physics too, from everyone's inputs, and rolls back for late ones (2-8 players, tiny bandwidth)"),
      tip(sliderProp('Snapshots a second', 'netSendRate', 1, 60, 1, 0), "How often the host sends clients the world's changes"),
      tip(sliderProp('Bandwidth a client (kbit/s)', 'netBandwidth', 16, 2048, 16, 0), "Each client's budget: nearest objects first; the rest wait their turn"),
      tip(sliderProp('Max players', 'netMaxPlayers', 0, 2000, 1, 0), 'Clients beyond this are refused (0: no limit)'),
      tip(sliderProp('Interpolation delay (ms)', 'netInterpolation', 0, 500, 10, 0), "Others' objects are shown this far in the past, between two snapshots"),
      tip(sliderProp('Input delay (ticks)', 'netInputDelay', 0, 8, 1, 0), 'Rollback mode: an input is built this many ticks before its tick (more: fewer rollbacks, felt as delay)'),
      tip(sliderProp('Rollback window (ticks)', 'netRollbackWindow', 4, 120, 1, 0), 'Rollback mode: how far back a late input can roll the world'),
      tip(sliderProp('Port', 'netPort', 1024, 65535, 1, 0), "A host's or dedicated server's default port"),
      el('div', { class: 'section-title' }, 'Steam'),
      tip(numberFieldProp('App ID', 'steamAppId'), "The game's Steam App ID (480: Valve's Spacewar, for development; 0: no Steam). The game starts Steam for it"),
      tip(numberFieldProp('Depot ID', 'steamDepotId'), "The Windows depot uploads go to (0: the App ID + 1, Steamworks' default)"),
      tip(textFieldProp('Branch to set live', 'steamBranch'), 'An upload is set live on this branch ("beta", say; empty: none, you set it live on Steamworks)'),
      tip(toggleProp('Start through Steam', 'steamRestart'), 'A shipped game started outside Steam starts again through it (the overlay, DRM, launch options)'),
    ];
    const holder = el('div', { class: 'project-settings' }, body());
    dialog('Project settings', [holder], [
      { label: 'Defaults', action: () => {
        for (const [k, v] of Object.entries(defaults)) setSetting(k, v);
        setTimeout(cmd.projectSettings, 0);  // (reopened with the values)
      } },
      { label: 'Close', primary: true },
    ]);
  },
  pause: () => send('editor.pause'),
  step: () => send('editor.step'),
  tool: tool => sendTools({ tool }),
  focusSearch: () => { showTab('hierarchy', 'hierarchy'); $('#hier-search').focus(); $('#hier-search').select(); },
  toggleVisible: () => { const e = selected(); if (e && state.inspector) send('editor.render.set', { id: e.id, visible: !state.inspector.render.visible }); },
  shortcuts: () => openShortcuts(),
  resetLayout: () => { Object.assign(sizes, { left: 260, right: 330, bottom: 230 }); applySizes(); store.set('sizes', sizes); },
};

// One table drives the handlers, the menus' hints and Help > Keyboard Shortcuts. `engine`
// entries are handled by the engine's scene view (listed for reference only).
const SHORTCUTS = [
  { group: 'File', keys: 'Ctrl+S', label: 'Save scene edits', run: cmd.save },
  { group: 'File', keys: 'Ctrl+B', label: 'Build scripts', run: cmd.buildScripts },
  { group: 'Edit', keys: 'Ctrl+Z', label: 'Undo', run: cmd.undo },
  { group: 'Edit', keys: 'Ctrl+Y', label: 'Redo', run: cmd.redo },
  { group: 'Edit', keys: 'Ctrl+Shift+Z', label: 'Redo', run: cmd.redo, hidden: true },
  { group: 'Edit', keys: 'Ctrl+D', label: 'Duplicate', run: cmd.duplicate },
  { group: 'Edit', keys: 'Ctrl+G', label: 'Group (under a new empty)', run: () => send('editor.group') },
  { group: 'Edit', keys: 'Ctrl+Shift+G', label: 'Ungroup', run: () => send('editor.ungroup') },
  { group: 'Edit', keys: 'Delete', label: 'Delete', run: cmd.remove },
  { group: 'Edit', keys: 'F2', label: 'Rename', run: cmd.rename },
  { group: 'Edit', keys: 'Escape', label: 'Clear selection (with the brush on: stop brushing)', run: cmd.deselect },
  { group: 'Edit', keys: 'Ctrl+F', label: 'Search hierarchy', run: cmd.focusSearch },
  { group: 'Edit', keys: 'Alt+Shift+A', label: 'Toggle visibility', run: cmd.toggleVisible },
  { group: 'GameObject', keys: 'Ctrl+Shift+N', label: 'Create empty', run: () => cmd.createEmpty() },
  { group: 'Play', keys: 'Ctrl+P', label: 'Play / Stop', run: cmd.play },
  { group: 'Play', keys: 'Ctrl+Shift+P', label: 'Pause', run: cmd.pause },
  { group: 'Play', keys: 'Ctrl+Alt+P', label: 'Step one frame', run: cmd.step },
  { group: 'Window', keys: 'Shift+Space', label: 'Maximize panel under the mouse', run: () => toggleMaximize() },
  { group: 'Window', keys: 'Ctrl+1', label: 'Scene', run: () => showTab('scene', 'scene') },
  { group: 'Window', keys: 'Ctrl+2', label: 'Game', run: () => showTab('scene', 'game') },
  { group: 'Window', keys: 'Ctrl+3', label: 'Inspector', run: () => showTab('inspector', 'inspector') },
  { group: 'Window', keys: 'Ctrl+4', label: 'Hierarchy', run: () => $('#tree').focus() },
  { group: 'Window', keys: 'Ctrl+5', label: 'Project', run: () => showTab('bottom', 'project') },
  { group: 'Window', keys: 'Ctrl+Shift+C', label: 'Console', run: () => showTab('bottom', 'console') },
  { group: 'Window', keys: 'Ctrl+7', label: 'Profiler', run: () => showTab('bottom', 'profiler') },
  { group: 'Window', keys: 'Ctrl+L', label: 'Lighting', run: () => showTab('inspector', 'lighting') },
  { group: 'Window', keys: 'Ctrl+Shift+L', label: 'Level Tools', run: () => window.levelTools && window.levelTools.toggleWindow() },
  { group: 'Help', keys: 'Ctrl+/', label: 'Keyboard shortcuts', run: cmd.shortcuts },
  { group: 'Level design', keys: 'Ctrl+Shift+L', label: 'The Level window (every level command, in sections)', engine: true },
  { group: 'Level design', keys: '1 / 2 / 3 / 4', label: 'Objects / vertices / edges / faces (the mode bar)', engine: true },
  { group: 'Level design', keys: 'Shift+1 / 2 / 3 / 4 / 5', label: 'Chord menu: the mode\'s numbered ring (5: world and tools); Shift held, 1–6 a group then a command (Edit > Chord Menus…)', engine: true },
  { group: 'Level design', keys: 'Tab', label: 'Element mode wheel (objects, vertices, edges, faces)', engine: true },
  { group: 'Level design', keys: 'Shift+A', label: 'Create menu at the mouse (shapes, walls, rooms, doors, cuts, volumes, splines)', engine: true },
  { group: 'Level design', keys: 'Alt+Q', label: 'Operations wheel (the mode\'s; More…: all of them)', engine: true },
  { group: 'Level design', keys: 'E / I / B / J / M / Alt+R', label: 'Extrude / inset / bevel / connect / merge / loop cut (modal: X Y Z lock, type a number, Esc)', engine: true },
  { group: 'Level design', keys: 'X / P / Alt+F / Alt+N', label: 'Delete / detach / fill / flip normals', engine: true },
  { group: 'Level design', keys: 'L / Shift+L / = / -', label: 'Select loop / ring / grow / shrink', engine: true },
  { group: 'Level design', keys: 'Shift+S', label: 'Snapping wheel; V: onto vertices; Ctrl+Shift: onto surfaces', engine: true },
  { group: 'Level design', keys: 'Shift+P / Shift+B', label: 'Paint menu / paint tool (layers, colours, decals, damage, scatter)', engine: true },
  { group: 'Level design', keys: '[ / ]', label: 'Brush smaller / bigger', engine: true },
  { group: 'Level design', keys: 'Alt+C / Alt+V', label: 'Pick the material under the mouse / put it on the chosen faces', engine: true },
  { group: 'Level design', keys: 'U / Shift+U / Alt+U / Alt+T / T', label: 'Unwrap / UV menu / UV editor / trim sheets / the last trim again', engine: true },
  { group: 'Level design', keys: 'Shift+M', label: 'Modifiers menu (mirror, array, bevel, follow spline...)', engine: true },
  { group: 'Level design', keys: 'Shift+G / Ctrl+G', label: 'Arrange menu (align, distribute, snap together) / group', engine: true },
  { group: 'Level design', keys: 'End', label: 'Drop the selection to the ground', engine: true },
  { group: 'Level design', keys: 'Ctrl+Shift+1..9 / Alt+1..9', label: 'Place a prefab shelf piece at the mouse / a palette material on the faces', engine: true },
  { group: 'Level design', keys: 'Alt+Z', label: 'Draw mode wheel (lit, unlit, wireframe...)', engine: true },
  { group: 'Level design', keys: 'Shift+V / Numpad 5 / 1 3 7', label: 'View menu / orthographic / front, right, top (Ctrl: the other side)', engine: true },
  { group: 'Level design', keys: 'F3 / Ctrl+Space / Shift+R', label: 'Command palette / again', engine: true },
  { group: 'Scene view tools', keys: 'Q', label: 'View tool (click to select)', engine: true },
  { group: 'Scene view tools', keys: 'W', label: 'Move tool', engine: true },
  { group: 'Scene view tools', keys: 'E', label: 'Rotate tool', engine: true },
  { group: 'Scene view tools', keys: 'R', label: 'Scale tool', engine: true },
  { group: 'Scene view tools', keys: 'X', label: 'Toggle Global / Local', engine: true },
  { group: 'Scene view tools', keys: 'Z', label: 'Toggle Pivot / Center', engine: true },
  { group: 'Scene view tools', keys: 'F', label: 'Frame selected (or double-click in Hierarchy)', engine: true },
  { group: 'Scene view tools', keys: 'Ctrl (drag)', label: 'Snap move / rotate / scale', engine: true },
  { group: 'Scene view tools', keys: 'Esc (drag)', label: 'Cancel the drag', engine: true },
  { group: 'Scene view camera', keys: 'Right-drag', label: 'Look around', engine: true },
  { group: 'Scene view camera', keys: 'Right-drag + WASD/QE', label: 'Fly (Shift: faster, wheel: speed)', engine: true },
  { group: 'Scene view camera', keys: 'Alt + left-drag', label: 'Orbit the pivot', engine: true },
  { group: 'Scene view camera', keys: 'Middle-drag', label: 'Pan', engine: true },
  { group: 'Scene view camera', keys: 'Wheel / Alt + right-drag', label: 'Zoom', engine: true },
  { group: 'Scene view camera', keys: 'Click an axis', label: 'Orientation gizmo: view along it', engine: true },
  { group: 'Hierarchy', keys: 'Up / Down, Home / End', label: 'Select previous / next / first / last' },
  { group: 'Hierarchy', keys: 'Double-click', label: 'Frame in the scene view' },
  { group: 'Project', keys: 'Enter / Backspace', label: 'Open the item / go up a folder' },
  { group: 'Project', keys: 'Arrows, Home / End, typing', label: 'Move through the items' },
  { group: 'Project', keys: 'F2 / Ctrl+D / Delete', label: 'Rename in place / duplicate / delete' },
  { group: 'Project', keys: 'Ctrl+Shift+N', label: 'New folder' },
  { group: 'Project', keys: 'Alt+Left / Alt+Right', label: 'Back / forward' },
  { group: 'Project', keys: 'Drag onto a folder', label: 'Move the items there' },
  { group: 'Inspector', keys: 'Drag a label', label: 'Scrub the value (Shift x10, Alt x0.1)' },
  { group: 'Inspector', keys: '2*3, +0.5', label: 'Arithmetic in number fields (leading operator = relative)' },
];
const shortcutFor = run => (SHORTCUTS.find(s => s.run === run && !s.hidden) || {}).keys;

function keyName(e) {
  const parts = [];
  if (e.ctrlKey || e.metaKey) parts.push('Ctrl');
  if (e.altKey) parts.push('Alt');
  if (e.shiftKey) parts.push('Shift');
  let k = e.key;
  if (k === ' ') k = 'Space';
  // (the digit row by its key, on any layout: Shift+2 is "Shift+2", not "Shift+@")
  else if (e.keyCode >= 48 && e.keyCode <= 57) k = String(e.keyCode - 48);
  else if (k.length === 1) k = k.toUpperCase();
  if (!['Control', 'Alt', 'Shift', 'Meta'].includes(k)) parts.push(k);
  return parts.join('+');
}

document.addEventListener('keydown', e => {
  const typing = isTextField(document.activeElement);
  if (e.key === 'Escape' && openMenuState) { closeMenu(); return; }
  if (e.key === 'Escape' && ui.picking) { ui.picking = null; renderInspector(); return; }
  if (e.key === 'Escape' && $('.modal-back')) { closeShortcuts(); return; }
  const name = keyName(e);
  if (typing) {
    // A text field keeps its own keys (editing, its undo); only other Ctrl shortcuts get through.
    if (e.key === 'Escape') document.activeElement.blur();
    if (!(e.ctrlKey || e.metaKey) || ['Ctrl+Z', 'Ctrl+Y', 'Ctrl+Shift+Z', 'Ctrl+A', 'Ctrl+C', 'Ctrl+V', 'Ctrl+X'].includes(name)) return;
  }
  // The Project panel's own keys while it has focus (arrows, Enter, F2, Delete, typing...).
  if (!typing && Assets.focused() && Assets.key(e, name)) {
    e.preventDefault();
    return;
  }
  const sc = SHORTCUTS.find(s => s.run && !s.engine && s.keys === name);
  if (sc) {
    e.preventDefault();
    try {
      sc.run();
    } catch (err) {  // window.onerror only gets an opaque "Script error." for file:// pages
      send('ui.error', { message: `shortcut ${name}: ${err && err.message}`, source: String(err && err.stack) });
    }
    return;
  }
  if (document.activeElement === $('#tree')) treeKey(e);
});

// ---------------------------------------------------------------- menubar

const MENUS = {
  file: () => {
    const project = state.projectInfo;
    const scenes = project ? project.scenes : [];
    return [
      { label: 'New Scene…', action: () => cmd.newScene(), disabled: () => !project || state.playing },
      { label: 'Open Scene', disabled: () => !project || state.playing, submenu: scenes.map(s => ({
        label: sceneLabel(s) + (s === project.startupScene ? '  (startup)' : ''), checked: s === project.current,
        action: () => { if (s !== project.current) cmd.openScene(s); },
      })) },
      { label: 'Save Scene', shortcut: 'Ctrl+S', action: cmd.save, disabled: () => state.playing },
      { label: 'Set as Startup Scene', action: () => send('editor.project.setStartup', {}),
        disabled: () => !project || project.current === project.startupScene },
      '-',
      { label: 'Project Settings…', value: 'project-settings', action: cmd.projectSettings, disabled: () => !project },
      '-',
      { label: state.scripts && state.scripts.state === 'none' ? 'Create Scripts' : 'Build Scripts', shortcut: 'Ctrl+B',
        action: cmd.buildScripts, disabled: () => !project || (state.scripts && state.scripts.state === 'building') },
      { label: 'Build Game', action: cmd.buildGame,
        disabled: () => !project || !state.scripts || state.scripts.state === 'none' || state.playing },
      { label: 'Upload to Steam…', action: cmd.uploadToSteam,
        disabled: () => !project || !(state.settings && state.settings.steamAppId) || state.playing },
      { label: 'Libraries…', icon: 'folder', action: cmd.libraries, disabled: () => !project },
      { label: 'Show Scripts Folder', icon: 'reveal', action: () => send('editor.assets.reveal', { path: 'Scripts' }),
        disabled: () => !project || !state.scripts || state.scripts.state === 'none' },
      '-',
      { label: 'Open Project…', action: cmd.openLauncher, disabled: () => !project || state.playing },
      { label: 'Reload From Engine', action: cmd.reload },
    ];
  },
  edit: () => [
    { label: 'Undo', shortcut: 'Ctrl+Z', action: cmd.undo, disabled: () => !state.canUndo },
    { label: 'Redo', shortcut: 'Ctrl+Y', action: cmd.redo, disabled: () => !state.canRedo },
    '-',
    { label: 'Duplicate', shortcut: 'Ctrl+D', action: cmd.duplicate, disabled: () => !state.selectedEntity },
    { label: 'Rename', shortcut: 'F2', action: cmd.rename, disabled: () => !state.selectedEntity },
    { label: 'Delete', shortcut: 'Delete', action: cmd.remove, disabled: () => !state.selectedEntity },
    '-',
    { label: 'Frame Selected', shortcut: 'F', action: cmd.frame, disabled: () => !state.selectedEntity },
    { label: 'Select None', shortcut: 'Esc', action: cmd.deselect },
    '-',
    { label: 'Command Palette…', shortcut: 'F3', action: () => window.levelTools && window.levelTools.openPalette() },
    { label: 'Keymap…', action: () => window.levelTools && window.levelTools.openKeymap() },
    { label: 'Chord Menus…', action: () => window.levelTools && window.levelTools.openChordEditor() },
    '-',
    { label: state.playing ? 'Stop' : 'Play', shortcut: 'Ctrl+P', action: cmd.play },
    { label: 'Pause', shortcut: 'Ctrl+Shift+P', action: cmd.pause, disabled: () => !state.playing, checked: () => state.paused },
    { label: 'Step', shortcut: 'Ctrl+Alt+P', action: cmd.step },
  ],
  gameobject: () => [
    ...createMenu(),
    '-',
    { label: 'Duplicate', shortcut: 'Ctrl+D', action: cmd.duplicate, disabled: () => !state.selectedEntity },
    { label: 'Frame Selected', shortcut: 'F', action: cmd.frame, disabled: () => !state.selectedEntity },
  ],
  // Editor code's menu items (libraries' Editor/, Scripts/Editor/: [EditorMenu]): "Tools/A/B" as
  // nested submenus; run by the game's code in the editor.
  tools: () => {
    const code = window.codeTools || {};
    const menus = [...(code.menus || []).filter(m => !m.builtin),  // (the level tools': toolbar, wheels, palette)
      ...(code.windows || []).map(w => ({ path: w.menu, window: w }))];
    if (!menus.length) return [{ label: 'No tools yet: libraries and Scripts/Editor/ add them', disabled: true },
      '-', { label: 'Libraries…', action: () => cmd.libraries(), disabled: () => !state.projectInfo }];
    const root = [];
    for (const m of menus) {
      const parts = m.path.split('/').filter(Boolean);
      if (parts[0] === 'Tools' && parts.length > 1) parts.shift();
      let level = root;
      parts.slice(0, -1).forEach(p => {
        let sub = level.find(i => i.label === p && i.submenu);
        if (!sub) level.push(sub = { label: p, submenu: [] });
        level = sub.submenu;
      });
      level.push({ label: parts[parts.length - 1], shortcut: m.shortcut,
        action: () => m.window ? openEditorWindow(m.window) : send('editor.tools.run', { menu: m.path }) });
    }
    return root;
  },
  window: () => [
    { label: 'Scene', shortcut: 'Ctrl+1', action: () => showTab('scene', 'scene') },
    { label: 'Game', shortcut: 'Ctrl+2', action: () => showTab('scene', 'game') },
    { label: 'Inspector', shortcut: 'Ctrl+3', action: () => showTab('inspector', 'inspector') },
    { label: 'Lighting', shortcut: 'Ctrl+L', action: () => showTab('inspector', 'lighting') },
    { label: 'Audio', action: () => showTab('inspector', 'audio') },
    { label: 'Project', shortcut: 'Ctrl+5', action: () => showTab('bottom', 'project') },
    { label: 'Console', shortcut: 'Ctrl+Shift+C', action: () => showTab('bottom', 'console') },
    { label: 'Profiler', shortcut: 'Ctrl+7', action: () => showTab('bottom', 'profiler') },
    { label: 'Level Tools', icon: 'levelTools', shortcut: 'Ctrl+Shift+L', action: () => window.levelTools && window.levelTools.toggleWindow(),
      checked: () => !!(window.levelTools && window.levelTools.windowOpen()) },
    '-',
    { label: 'Maximize Scene View', shortcut: 'Shift+Space', action: () => toggleMaximize($('#dock-scene')) },
    { label: 'Reset Layout', action: cmd.resetLayout },
  ],
  help: () => [
    { label: 'Keyboard Shortcuts', icon: 'keyboard', shortcut: 'Ctrl+/', action: cmd.shortcuts },
    '-',
    { label: 'Level Starter Content', icon: 'sample', value: 'Level Starter Content',
      submenu: window.levelTools && window.levelTools.starterItems().length ? window.levelTools.starterItems() : [{ label: 'Once the editor code is built', disabled: true }] },
  ],
};
$$('#menubar [data-menu]').forEach(button => {
  button.addEventListener('mousedown', e => {
    e.stopPropagation();
    if (openMenuState && openMenuState.anchor === button) { closeMenu(); return; }
    menuBelow(button, MENUS[button.dataset.menu]());
  });
  button.addEventListener('mouseenter', () => {
    if (openMenuState && openMenuState.anchor && openMenuState.anchor !== button && openMenuState.anchor.dataset.menu) {
      menuBelow(button, MENUS[button.dataset.menu]());
    }
  });
});

$('#btn-play').addEventListener('click', cmd.play);
$('#btn-pause').addEventListener('click', cmd.pause);
$('#btn-step').addEventListener('click', cmd.step);
$('#btn-create').addEventListener('mousedown', e => { e.stopPropagation(); menuBelow(e.currentTarget, createMenu()); });

// ---------------------------------------------------------------- shortcuts dialog

function openShortcuts() {
  closeMenu();
  const groups = new Map();
  for (const s of SHORTCUTS.filter(s => !s.hidden)) {
    if (!groups.has(s.group)) groups.set(s.group, []);
    groups.get(s.group).push(s);
  }
  const body = el('div', { class: 'modal-body' });
  for (const [group, list] of groups) {
    body.append(el('div', { class: 'sc-group' }, el('h4', {}, group),
      list.map(s => el('div', { class: 'sc' }, el('span', {}, s.label), el('kbd', {}, s.keys)))));
  }
  const back = el('div', { class: 'modal-back', onmousedown: e => { if (e.target === back) closeShortcuts(); } },
    el('div', { class: 'modal' },
      el('div', { class: 'modal-head', html: icon('keyboard') }, 'Keyboard shortcuts', el('span', { class: 'spacer' }),
        el('span', { class: 'tbtn', role: 'button', html: icon('close'), onclick: closeShortcuts })),
      body));
  document.body.append(back);
  capturePointer(true);
}
function closeShortcuts() {
  const back = $('.modal-back');
  if (back) back.remove();
  capturePointer(!!openMenuState);
}

// ---------------------------------------------------------------- hierarchy (virtualized)

const ROW = 20;
const tree = $('#tree');
const treeSpacer = $('#tree-spacer');
let rows = [];  // [{scene: true} | entity]

// The entity's icon: its primitive's shape, a character, a mesh, or an empty.
// Prefab commands for an entity: an instance's (Select / Apply / Revert / Unpack), or Save as Prefab.
function prefabMenu(r) {
  if (r.prefab) {
    return [
      { label: 'Select Prefab Asset', icon: 'prefab', action: () => Assets.reveal(r.prefabPath) },
      { label: 'Apply to Prefab', action: () => send('editor.prefab.apply', { id: r.id }) },
      { label: 'Revert to Prefab', action: () => send('editor.prefab.revert', { id: r.id }) },
      { label: 'Unpack Prefab', action: () => send('editor.prefab.unpack', { id: r.id }) },
    ];
  }
  return [{ label: 'Save as Prefab', icon: 'prefab', disabled: r.kind === 'mesh', action: () => send('editor.prefab.create', { id: r.id, folder: Assets.folder() }) }];
}

function entityIcon(e) {
  if (!e) return 'cube';
  if (e.kind === 'empty') return 'empty';
  if (e.kind === 'light') return e.primitive === 'spot' ? 'spot' : e.primitive === 'directional' ? 'sun' : 'light';
  if (e.kind === 'camera') return 'camera';
  if (e.kind === 'water') return 'water';
  if (e.kind === 'bone') return e.bone === -1 ? 'skeleton' : 'bone';
  if (e.kind === 'primitive') return ICONS[e.primitive] ? e.primitive : 'cube';
  return e.instance ? 'character' : 'cube';
}

// Rows in tree order (children under their parent, indented), skipping collapsed subtrees. A
// search lists the matches flat.
function rebuildRows() {
  const q = ui.search.trim().toLowerCase();
  rows = [{ scene: true, name: state.scene || 'Scene' }];
  if (q) {
    rows.push(...state.entities.filter(e => `${e.name} ${e.id}`.toLowerCase().includes(q)).map(e => ({ ...e, depth: 0, kids: 0 })));
  } else if (!ui.collapsedRoot) {
    const children = new Map();
    for (const e of state.entities) {
      const p = entityById.has(e.parent) ? e.parent : 0;
      if (!children.has(p)) children.set(p, []);
      children.get(p).push(e);
    }
    // A Skeleton (Expand Bones) starts folded: a character has a hundred bones or more.
    for (const e of state.entities)
      if (e.kind === 'bone' && e.bone === -1 && !ui.seenSkeletons.has(e.id)) {
        ui.seenSkeletons.add(e.id);
        ui.collapsed.add(e.id);
      }
    const walk = (parent, depth) => {
      for (const e of children.get(parent) || []) {
        const kids = (children.get(e.id) || []).length;
        rows.push({ ...e, depth, kids });
        if (kids && !ui.collapsed.has(e.id)) walk(e.id, depth + 1);
      }
    };
    walk(0, 0);
  }
  treeSpacer.style.height = rows.length * ROW + 'px';
  renderTree();
}

function renderTree() {
  const top = tree.scrollTop, height = tree.clientHeight;
  const first = Math.max(0, Math.floor(top / ROW) - 4);
  const last = Math.min(rows.length, Math.ceil((top + height) / ROW) + 4);
  const frag = document.createDocumentFragment();
  for (let i = first; i < last; ++i) {
    const r = rows[i];
    const row = el('div', { class: 'row' });
    row.style.top = i * ROW + 'px';
    if (r.scene) {
      row.classList.add('scene-row');
      row.append(el('span', { class: 'eye' }), el('span', { class: 'arrow', html: icon(ui.collapsedRoot ? 'chevRight' : 'chev') }),
        el('span', { class: 'ico', html: icon('scene') }), el('span', { class: 'name' }, r.name));
      row.addEventListener('click', () => { ui.collapsedRoot = !ui.collapsedRoot; rebuildRows(); });
    } else {
      const hidden = r.hidden;
      row.dataset.id = r.id;
      row.dataset.name = r.name || '';
      row.classList.toggle('selected', r.id === state.selectedEntity || (state.selection || []).includes(r.id));
      row.classList.toggle('hidden-entity', hidden);
      const eye = el('span', { class: 'eye' + (hidden ? ' off' : ''), html: icon(hidden ? 'eyeOff' : 'eye'), title: 'Visibility' });
      eye.addEventListener('click', e => { e.stopPropagation(); send('editor.render.set', { id: r.id, visible: hidden }); });
      const arrow = el('span', { class: 'arrow', html: r.kids ? icon(ui.collapsed.has(r.id) ? 'chevRight' : 'chev') : '' });
      arrow.style.marginLeft = (r.depth || 0) * 14 + 'px';
      if (r.kids) arrow.addEventListener('mousedown', e => { e.stopPropagation(); toggleCollapsed(r.id); });
      row.classList.toggle('prefab', !!r.prefab);
      row.append(eye, arrow, el('span', { class: 'ico' + (r.instance ? ' char' : ''), html: icon(r.prefab === 'root' ? 'prefab' : entityIcon(r)) }));
      if (ui.renaming === r.id) {
        const input = el('input', { class: 'field rename', value: r.name });
        const finish = commit => {
          if (ui.renaming !== r.id) return;
          ui.renaming = 0;
          if (commit && input.value.trim() && input.value !== r.name) send('editor.entity.rename', { id: r.id, name: input.value.trim() });
          renderTree();
          tree.focus();
        };
        input.addEventListener('keydown', e => { if (e.key === 'Enter') finish(true); else if (e.key === 'Escape') finish(false); e.stopPropagation(); });
        input.addEventListener('blur', () => finish(true));
        row.append(input);
        setTimeout(() => { input.focus(); input.select(); });
      } else {
        row.append(el('span', { class: 'name' }, r.name || `Entity ${r.id}`));
        if (r.overridden) row.append(el('span', { class: 'ovr-dot', title: 'Changed from its prefab (Apply or Revert)' }));
        if (r.clipped) row.append(el('span', { class: 'clip-badge', html: icon('warning'), title: 'Cut without caps: the mesh isn\'t closed, so its cuts are clipped open' }));
      }
    }
    frag.append(row);
  }
  treeSpacer.replaceChildren(frag);
}

function toggleCollapsed(id, collapse = !ui.collapsed.has(id)) {
  if (collapse) ui.collapsed.add(id); else ui.collapsed.delete(id);
  rebuildRows();
}

// Row input is handled on the tree, not per row: pressing a row selects it, the engine answers
// with new state and the rows are rebuilt, so the release (and a double-click's second click,
// and the context menu) land on a different node than the press.
const rowEntity = e => {
  const row = e.target.closest('.row[data-id]');
  return row ? state.entities.find(x => x.id === +row.dataset.id) : null;
};
// Dragging a row onto another makes it that row's child; onto the scene row, a root again.
let rowDrag = null;  // {id, x, y, active}
tree.addEventListener('mousedown', e => {
  Assets.stopInspecting();  // (clicking the selected entity again shows it again)
  const r = rowEntity(e);
  if (ui.picking && e.button === 0) {  // an Animator's entity parameter waiting for one (its pick button)
    const pick = ui.picking;
    ui.picking = null;
    if (r) send('editor.animator.set', { id: pick.id, entities: { [pick.param]: r.id } });
    renderInspector();
    e.preventDefault();
    return;
  }
  if (r && e.button === 0 && e.ctrlKey) {
    send('editor.select', { id: r.id, mode: 'toggle' });
    return;
  }
  if (r && e.button === 0 && e.shiftKey && state.selectedEntity) {  // the rows from the last one to this one
    const from = rows.findIndex(x => x.id === state.selectedEntity), to = rows.findIndex(x => x.id === r.id);
    if (from >= 0 && to >= 0) {
      const ids = rows.slice(Math.min(from, to), Math.max(from, to) + 1).filter(x => x.id && !x.scene).map(x => x.id);
      if (from > to) ids.reverse();  // (the clicked row ends up the primary one: last)
      send('editor.select', { ids, mode: 'add' });
      return;
    }
  }
  // (A right click on one of several selected rows keeps them: its menu acts on all of them.)
  const inMany = r && manySelected() && state.selection.includes(r.id);
  if (r && !(e.button === 2 && inMany) && (r.id !== state.selectedEntity || manySelected())) send('editor.select', { id: r.id });
  if (r && e.button === 0 && !e.target.closest('input, .eye, .arrow')) rowDrag = { id: r.id, x: e.clientX, y: e.clientY, active: false };
});
const dropRow = e => e.target.closest && e.target.closest('#tree .row');
document.addEventListener('mousemove', e => {
  if (!rowDrag) return;
  if (!rowDrag.active && Math.hypot(e.clientX - rowDrag.x, e.clientY - rowDrag.y) > 4) rowDrag.active = true;
  if (!rowDrag.active) return;
  $$('#tree .row.drop').forEach(r => r.classList.remove('drop'));
  const row = dropRow(e);
  if (row && +row.dataset.id !== rowDrag.id) row.classList.add('drop');
});
document.addEventListener('mouseup', e => {
  if (!rowDrag) return;
  const drag = rowDrag;
  rowDrag = null;
  $$('#tree .row.drop').forEach(r => r.classList.remove('drop'));
  const under = drag.active && document.elementFromPoint(e.clientX, e.clientY);
  const folder = under && under.closest('#assets [data-drop]');
  if (folder) {  // onto a folder of the Project panel: a prefab of it (and its children)
    send('editor.prefab.create', { id: drag.id, folder: folder.dataset.drop || 'Assets' });
    return;
  }
  const row = drag.active && dropRow(e);
  if (!row) return;
  const parent = row.classList.contains('scene-row') ? 0 : +row.dataset.id;
  if (parent !== drag.id) {
    if (parent) ui.collapsed.delete(parent);
    send('editor.entity.parent', { id: drag.id, parent });
  }
});
tree.addEventListener('dblclick', e => {
  const r = rowEntity(e);
  if (r && !e.target.closest('input, .eye')) send('editor.frame', { id: r.id });
});
tree.addEventListener('contextmenu', e => {
  e.preventDefault();
  const r = rowEntity(e);
  if (!r) {  // empty space: create, like Unity's hierarchy
    openMenu(createMenu(), e.clientX, e.clientY);
    return;
  }
  const many = manySelected() && state.selection.includes(r.id);
  if (!many) send('editor.select', { id: r.id });
  if (many) {  // several selected: what acts on all of them
    const n = state.selection.length;
    openMenu([
      { label: 'Frame', shortcut: 'F', action: () => send('editor.frame', {}) },
      { label: `Duplicate ${n}`, shortcut: 'Ctrl+D', action: () => send('editor.entity.duplicate', {}) },
      '-',
      { label: `Delete ${n}`, shortcut: 'Delete', action: () => send('editor.entity.delete', {}) },
    ], e.clientX, e.clientY);
    return;
  }
  if (r.kind === 'bone') {  // (driven by the animation: things hang under it, it is not edited)
    openMenu([
      { label: 'Create Child', icon: 'plus', submenu: createMenu(r.id) },
      '-',
      { label: 'Frame', shortcut: 'F', action: () => send('editor.frame', { id: r.id }) },
    ], e.clientX, e.clientY);
    return;
  }
  // A character (a skinned mesh in it or right below): its skeleton as entities to attach things to.
  const character = r.instance || state.entities.some(c => c.parent === r.id && c.instance);
  openMenu([
    { label: 'Create Child', icon: 'plus', submenu: createMenu(r.id) },
    ...(character ? [r.bones ? { label: 'Collapse Bones', icon: 'bone', action: () => send('editor.entity.bones', { id: r.id, expand: false }) }
      : { label: 'Expand Bones', icon: 'bone', action: () => send('editor.entity.bones', { id: r.id, expand: true }) }] : []),
    '-',
    { label: 'Frame', shortcut: 'F', action: () => send('editor.frame', { id: r.id }) },
    { label: 'Rename', shortcut: 'F2', action: () => startRename(r.id) },
    { label: 'Duplicate', shortcut: 'Ctrl+D', action: () => send('editor.entity.duplicate', { id: r.id }) },
    { label: r.hidden ? 'Show' : 'Hide', action: () => send('editor.render.set', { id: r.id, visible: r.hidden }) },
    '-',
    { label: 'Delete', shortcut: 'Delete', action: () => send('editor.entity.delete', { id: r.id }) },
    '-',
    ...prefabMenu(r),
  ], e.clientX, e.clientY);
});
tree.addEventListener('scroll', () => requestAnimationFrame(renderTree));
new ResizeObserver(() => renderTree()).observe(tree);
$('#hier-search').addEventListener('input', e => { ui.search = e.target.value; rebuildRows(); });
$('#hier-search').addEventListener('keydown', e => {
  if (e.key === 'Escape') { e.target.value = ''; ui.search = ''; rebuildRows(); }
  if (e.key === 'ArrowDown') { tree.focus(); treeKey(e); }
});

function revealSelected() {
  let i = rows.findIndex(r => r.id === state.selectedEntity);
  if (i < 0 && entityById.has(state.selectedEntity) && !ui.search) {  // inside a collapsed parent: open the way
    for (let p = entityById.get(state.selectedEntity).parent; p; p = (entityById.get(p) || {}).parent) ui.collapsed.delete(p);
    ui.collapsedRoot = false;
    rebuildRows();
    i = rows.findIndex(r => r.id === state.selectedEntity);
  }
  if (i < 0 || tree.clientHeight === 0) return;  // not laid out yet
  const y = i * ROW;
  if (y < tree.scrollTop) tree.scrollTop = y;
  else if (y + ROW > tree.scrollTop + tree.clientHeight) tree.scrollTop = y + ROW - tree.clientHeight;
}

function treeKey(e) {
  // Left / right: collapse or open the selected row, else go to its parent / first child.
  if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
    const r = rows.find(x => x.id === state.selectedEntity);
    if (!r) return;
    e.preventDefault();
    const open = r.kids && !ui.collapsed.has(r.id);
    if (e.key === 'ArrowRight' && r.kids && !open) toggleCollapsed(r.id, false);
    else if (e.key === 'ArrowRight' && open) send('editor.select', { id: rows[rows.indexOf(r) + 1].id });
    else if (e.key === 'ArrowLeft' && open) toggleCollapsed(r.id, true);
    else if (e.key === 'ArrowLeft' && r.parent && entityById.has(r.parent)) send('editor.select', { id: r.parent });
    return;
  }
  if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp' && e.key !== 'Home' && e.key !== 'End') return;
  e.preventDefault();
  const ids = rows.filter(r => !r.scene).map(r => r.id);
  if (!ids.length) return;
  let i = ids.indexOf(state.selectedEntity);
  if (e.key === 'Home') i = 0;
  else if (e.key === 'End') i = ids.length - 1;
  else i = i < 0 ? 0 : Math.max(0, Math.min(ids.length - 1, i + (e.key === 'ArrowDown' ? 1 : -1)));
  send('editor.select', { id: ids[i] });
}

function startRename(id) {
  showTab('hierarchy', 'hierarchy');
  ui.renaming = id;
  revealSelected();
  renderTree();
}

// ---------------------------------------------------------------- scene view toolbar and overlays

function sendTools(changes) {
  Object.assign(state.tools, changes);
  send('editor.tool.set', changes);
  renderTools();
}

const DRAW_MODES = [
  [0, 'Shaded'], [9, 'Albedo'], [2, 'Normals'], [11, 'Shadows'], [12, 'Indirect Light'], [13, 'Reflections'], [10, 'Light Count'],
  [1, 'Clusters'], [3, 'Instances'], [4, 'Triangles'], [14, 'Ray Traced'], [15, 'Probes'], [5, 'LOD Level'], [6, 'Occlusion Pass'], [7, 'HiZ'], [8, 'Texture Mip'],
];

// The mode bar's chip: the selected mesh's name (objects), or how many elements are chosen.
function modeCount(element, n) {
  if (element === 'object') return (selected() || {}).name || '';
  const [one, many] = { vertex: ['vertex', 'vertices'], edge: ['edge', 'edges'], face: ['face', 'faces'] }[element] || ['', ''];
  return n ? `${n} ${n === 1 ? one : many}` : `Click ${many} to choose them`;
}

let lastTool = null;
function renderTools() {
  const t = state.tools;
  $$('#tools-overlay [data-tool]').forEach(b => b.classList.toggle('on', b.dataset.tool === t.tool));
  // The mode bar: a level mesh's element modes, shown while one is selected (or a mode other than
  // objects is on), with what is chosen.
  const elements = !!t.levelSelected || (t.element && t.element !== 'object');
  const element = t.element || 'object';
  $('#mode-bar').hidden = !elements || !!state.playing;
  $$('#mode-bar [data-element]').forEach(b => b.classList.toggle('on', b.dataset.element === element));
  setText($('#mode-count'), modeCount(element, t.elementCount || 0));
  // Picking the brush (B, the toolbar) brings up the Terrain tab, where its settings are.
  if (lastTool !== null && lastTool !== t.tool && (t.tool === 'brush' || lastTool === 'brush')) {
    if (t.tool === 'brush') showTab('inspector', 'terrain');
    renderTerrain();
  }
  lastTool = t.tool;
  renderBrushPill();
  $('#btn-pivot').innerHTML = `${t.pivotCenter ? 'Center' : 'Pivot'}`;
  $('#btn-space').innerHTML = `${t.local ? 'Local' : 'Global'}`;
  $('#btn-grid').classList.toggle('on', t.grid);
  for (const [id, key] of [['#snap-move', 'snapMove'], ['#snap-rotate', 'snapRotate']]) {
    if (document.activeElement !== $(id)) $(id).value = fmt(t[key], 3);
  }
  const s = state.settings;
  if (s) {
    const mode = DRAW_MODES.find(m => m[0] === s.debugView);
    $('#btn-draw').innerHTML = `${mode ? mode[1] : 'Shaded'} ${icon('chev', 'chev')}`;
    if (document.activeElement !== $('#cam-speed')) $('#cam-speed').value = fmt(s.cameraSpeed, 2);
  }
  $('#btn-maxplay').classList.toggle('on', ui.maximizeOnPlay);
  $('#btn-stats').classList.toggle('on', ui.showStats);
  if (window.levelTools) window.levelTools.render();  // (the level toolbar and key hints follow the mode)
}
$$('#tools-overlay [data-tool]').forEach(b => b.addEventListener('click', () => cmd.tool(b.dataset.tool)));
$$('#mode-bar [data-element]').forEach(b => b.addEventListener('click', () => sendTools({ element: b.dataset.element })));

let brushPillKey = '';
function renderBrushPill() {
  const pill = $('#brush-pill');
  const t = state.terrain;
  const on = state.tools.tool === 'brush' && !state.tools.gameView;
  const b = t && t.brush;
  const painted = b && b.op === 'scatter' ? ((t.scatter && t.scatter.layers) || [])[b.scatterLayer] : null;
  const key = on ? JSON.stringify([!!(t && t.exists), b && b.op, b && b.radius, b && b.strength, painted && painted.name]) : '';
  if (key === brushPillKey) return;
  brushPillKey = key;
  pill.hidden = !on;
  if (on) {
    const stop = el('span', { class: 'tbtn pill-stop', role: 'button', title: 'Back to the Move tool (Esc)', onclick: () => sendTools({ tool: 'move' }) }, 'Stop (Esc)');
    if (!t || !t.exists) {
      pill.classList.add('warn');
      pill.replaceChildren(el('span', { class: 'pill-icon', html: icon('brush') }),
        el('span', { class: 'pill-text' }, el('b', {}, 'Brush on'), ' · this scene has no terrain to sculpt'),
        el('span', { class: 'tbtn pill-stop', role: 'button', onclick: () => showTab('inspector', 'terrain') }, 'Create one'), stop);
    } else {
      pill.classList.remove('warn');
      const op = (BRUSH_OPS.find(o => o[0] === b.op) || [b.op, b.op])[1];
      pill.replaceChildren(el('span', { class: 'pill-icon', html: icon('brush') }),
        el('span', { class: 'pill-text' }, el('b', {}, `Brush: ${op}${painted ? ' · ' + painted.name : ''}`),
          ` · ${fmt(b.radius, 1)} m` + (painted ? '' : ` · ${Math.round(b.strength * 100)}%`)),
        el('span', { class: 'pill-keys' }, painted ? '[ ] size · Shift erases' : '[ ] size · Shift inverts · Ctrl smooths'), stop);
    }
  }
  reportLayout();
}
$('#btn-pivot').addEventListener('click', () => sendTools({ pivotCenter: !state.tools.pivotCenter }));
$('#btn-space').addEventListener('click', () => sendTools({ local: !state.tools.local }));
$('#btn-grid').addEventListener('click', () => sendTools({ grid: !state.tools.grid }));
$('#btn-snap').addEventListener('mousedown', e => {
  e.stopPropagation();
  menuBelow(e.currentTarget, [
    { label: 'Move 0.1', action: () => sendTools({ snapMove: 0.1 }) },
    { label: 'Move 0.25', action: () => sendTools({ snapMove: 0.25 }) },
    { label: 'Move 1', action: () => sendTools({ snapMove: 1 }) },
    '-',
    { label: 'Rotate 5°', action: () => sendTools({ snapRotate: 5 }) },
    { label: 'Rotate 15°', action: () => sendTools({ snapRotate: 15 }) },
    { label: 'Rotate 45°', action: () => sendTools({ snapRotate: 45 }) },
    '-',
    { label: 'Scale 0.1', action: () => sendTools({ snapScale: 0.1 }) },
    { label: 'Scale 0.25', action: () => sendTools({ snapScale: 0.25 }) },
  ]);
});
for (const [id, key] of [['#snap-move', 'snapMove'], ['#snap-rotate', 'snapRotate']]) {
  $(id).addEventListener('change', e => { const v = parseFloat(e.target.value); if (v >= 0) sendTools({ [key]: v }); });
  $(id).addEventListener('keydown', e => { if (e.key === 'Enter') e.target.blur(); });
}
$('#btn-draw').addEventListener('mousedown', e => {
  e.stopPropagation();
  menuBelow(e.currentTarget, DRAW_MODES.map(([id, label], i) => [
    ...(i === 5 ? ['-'] : []),
    { label, checked: () => state.settings && state.settings.debugView === id, action: () => setSetting('debugView', id) },
  ]).flat());
});
$('#cam-speed').addEventListener('change', e => { const v = parseFloat(e.target.value); if (v > 0) setSetting('cameraSpeed', v); });
$('#cam-speed').addEventListener('keydown', e => { if (e.key === 'Enter') e.target.blur(); });
$('#btn-maxplay').addEventListener('click', () => { ui.maximizeOnPlay = !ui.maximizeOnPlay; store.set('maxplay', ui.maximizeOnPlay); renderTools(); applyPlayLayout(); });
$('#btn-stats').addEventListener('click', () => {
  ui.showStats = !ui.showStats;
  store.set('stats', ui.showStats);
  $('#stats-overlay').hidden = !ui.showStats || !state.tools.gameView;
  renderTools();
  renderStatsOverlay();
  reportLayout();
});

function applyPlayLayout() {
  document.body.classList.toggle('maximize-play', ui.maximizeOnPlay && state.playing);
  reportLayout();
}

// ---------------------------------------------------------------- inspector

const inspector = $('#inspector');
let collapsed = new Set(store.get('collapsed', []));

// Sliders paint their fill from --pct (editor.css): on input, when added, and when code moves them.
function paintRange(r) {
  const span = (+r.max - +r.min) || 1;
  r.style.setProperty('--pct', `${Math.min(100, Math.max(0, (+r.value - +r.min) / span * 100))}%`);
}
function setRange(r, v) {
  r.value = v;
  paintRange(r);
}
document.addEventListener('input', e => { if (e.target.matches && e.target.matches('input[type=range]')) paintRange(e.target); }, true);
new MutationObserver(records => {
  for (const m of records)
    for (const n of m.addedNodes)
      if (n.querySelectorAll) {
        if (n.matches('input[type=range]')) paintRange(n);
        n.querySelectorAll('input[type=range]').forEach(paintRange);
      }
}).observe(document.body, { childList: true, subtree: true });

// A tooltip on a row (on the row, not its label: see the CEF notes at the top).
function tip(node, text) {
  node.setAttribute('title', text);
  return node;
}

function component(title, key, body, extra = null) {
  const node = el('section', { class: 'component' + (collapsed.has(key) ? ' collapsed' : ''), 'data-component': key });
  const head = el('div', { class: 'comp-head' }, el('span', { class: 'arrow', html: icon('chev') }), extra, el('span', {}, title), el('span', { class: 'spacer' }));
  head.addEventListener('click', e => {
    if (e.target.closest('input, .more')) return;
    node.classList.toggle('collapsed');
    if (node.classList.contains('collapsed')) collapsed.add(key); else collapsed.delete(key);
    store.set('collapsed', [...collapsed]);
  });
  node.append(head, el('div', { class: 'comp-body' }, body));
  return { node, head };
}

// A number field whose label scrubs the value (drag left/right; Shift x10, Alt x0.1).
function scrub(label, get, onPreview, onCommit, step = 0.01) {
  label.addEventListener('mousedown', down => {
    if (down.button !== 0) return;
    down.preventDefault();
    const start = get(), x0 = down.clientX;
    let value = start, moved = false;
    const move = e => {
      const k = e.shiftKey ? 10 : e.altKey ? 0.1 : 1;
      value = start + (e.clientX - x0) * step * k;
      moved = true;
      onPreview(value);
    };
    const up = () => {
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      if (moved) onCommit(value);
    };
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
}

function numberInput(value, onCommit, digits = 3) {
  const input = el('input', { class: 'field', value: fmt(value, digits), inputmode: 'decimal' });
  input.addEventListener('keydown', e => { if (e.key === 'Enter') input.blur(); if (e.key === 'Escape') { input.value = fmt(value, digits); input.blur(); } });
  input.addEventListener('change', () => {
    // Arithmetic like Unity: "2*3", "(1+2)/4"; a leading operator is relative ("+0.5", "*2").
    let text = input.value.trim();
    if (/^[+*/]/.test(text)) text = `${value}${text}`;
    const v = evalExpr(text);
    if (Number.isFinite(v)) onCommit(v); else input.value = fmt(value, digits);
  });
  return input;
}

// + - * / and parentheses over numbers (the page's CSP rules out eval).
function evalExpr(text) {
  const tokens = text.match(/\d*\.?\d+(?:[eE][+-]?\d+)?|[-+*/()]/g);
  if (!tokens || tokens.join('') !== text.replace(/\s+/g, '')) return NaN;
  let i = 0;
  const peek = () => tokens[i];
  const factor = () => {
    const t = tokens[i++];
    if (t === '-') return -factor();
    if (t === '+') return factor();
    if (t === '(') { const v = sum(); if (tokens[i++] !== ')') return NaN; return v; }
    return Number(t);
  };
  const product = () => {
    let v = factor();
    while (peek() === '*' || peek() === '/') v = tokens[i++] === '*' ? v * factor() : v / factor();
    return v;
  };
  const sum = () => {
    let v = product();
    while (peek() === '+' || peek() === '-') v = tokens[i++] === '+' ? v + product() : v - product();
    return v;
  };
  const v = sum();
  return i === tokens.length ? v : NaN;
}

// A bone of an expanded skeleton (or its Skeleton group): where the animation has it, read only,
// and how to attach things to it.
function boneInspector(ins, kind) {
  const t = ins.transform;
  const v3 = (label, v, digits) => el('div', { class: 'prop' }, el('label', {}, label), el('span', { class: 'value' }, v.map(x => x.toFixed(digits)).join('  ')));
  const header = el('div', { class: 'ins-header' }, el('span', { class: 'big-ico', html: icon(kind) }),
    el('span', { class: 'name-field static' }, ins.name),
    el('div', { class: 'ins-sub' }, el('span', {}, ins.bone >= 0 ? `Bone ${ins.bone} of ${ins.boneOf}` : `${ins.boneOf}'s skeleton`), el('span', {}, ins.scene)));
  return [header, component(ins.bone >= 0 ? 'Bone' : 'Skeleton', 'bone', [
    v3('Position', t.position, 3), v3('Rotation', t.rotation, 1), v3('Scale', t.scale, 3),
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {},
      'Follows the animation. Drop entities onto it in the Hierarchy, or create a child, to attach them. Collapse Bones on the character puts the skeleton away.')),
    el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Create Empty Child', () => cmd.createEmpty(ins.id))),
  ], el('span', { html: icon(kind) })).node];
}

function transformComponent(ins) {
  const t = ins.transform;
  const current = { position: [...t.position], rotation: [...t.rotation], scale: [...t.scale] };
  const payload = () => ({ id: ins.id, position: current.position, rotation: current.rotation, scale: current.scale });
  const vecRow = (label, key, step) => {
    const vec = el('div', { class: 'vec' });
    ['X', 'Y', 'Z'].forEach((axis, i) => {
      const lab = el('label', {}, axis);
      const input = numberInput(current[key][i], v => { current[key][i] = v; send('editor.transform.set', payload()); });
      scrub(lab, () => current[key][i],
        v => { current[key][i] = v; input.value = fmt(v); send('editor.transform.preview', payload()); },
        v => { current[key][i] = v; send('editor.transform.set', payload()); }, step);
      vec.append(el('div', {}, lab, input));
    });
    const name = el('label', { class: 'scrub' }, label);
    return el('div', { class: 'prop' }, name, vec);
  };
  const more = el('span', { class: 'more', role: 'button', html: icon('more'), title: 'More' });
  const rows = [vecRow('Position', 'position', 0.01), vecRow('Rotation', 'rotation', 0.5), vecRow('Scale', 'scale', 0.005)];
  const c = component('Transform', 'transform', rows, el('span', { html: icon('move') }));
  if (t.local) tip(c.head, `Relative to the parent, ${(entityById.get(ins.parent) || {}).name || 'its parent'}`);
  c.head.append(more);
  more.addEventListener('mousedown', e => {
    e.stopPropagation();
    const r = more.getBoundingClientRect();
    openMenu([
      { label: 'Reset Position', action: () => { current.position = [0, 0, 0]; send('editor.transform.set', payload()); } },
      { label: 'Reset Rotation', action: () => { current.rotation = [0, 0, 0]; send('editor.transform.set', payload()); } },
      { label: 'Reset Scale', action: () => { current.scale = [1, 1, 1]; send('editor.transform.set', payload()); } },
      '-',
      { label: 'Copy Values', action: () => { ui.clipboard = JSON.parse(JSON.stringify(current)); } },
      { label: 'Paste Values', disabled: () => !ui.clipboard, action: () => { Object.assign(current, JSON.parse(JSON.stringify(ui.clipboard))); send('editor.transform.set', payload()); } },
    ], r.left - 150, r.bottom + 2);
  });
  return c.node;
}

// ---------------------------------------------------------------- colour field

// Linear RGB (what the engine stores) <-> sRGB 0..1 (what the eye picks).
const toSrgb = c => { c = Math.min(1, Math.max(0, c)); return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055; };
const toLinear = c => c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
function hsvToRgb(h, s, v) {
  const f = n => { const k = (n + h * 6) % 6; return v - v * s * Math.max(0, Math.min(k, 4 - k, 1)); };
  return [f(5), f(3), f(1)];
}
function rgbToHsv([r, g, b]) {
  const max = Math.max(r, g, b), d = max - Math.min(r, g, b);
  const h = d === 0 ? 0 : max === r ? ((g - b) / d + 6) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h / 6, max === 0 ? 0 : d / max, max];
}
const cssColor = srgb => `rgb(${srgb.map(c => Math.round(c * 255)).join(',')})`;
const hexOf = srgb => '#' + srgb.map(c => Math.round(c * 255).toString(16).padStart(2, '0')).join('');

let colorPop = null;  // the open picker: {node, close}
function closeColorPop() { if (colorPop) colorPop.close(); }
// Another popover shown as the colour picker is (one at a time; a click outside or Escape closes
// it): under `anchor`, or over it near the bottom of the window.
function openPopover(anchor, node) {
  closeColorPop();
  document.body.append(node);
  const fr = anchor.getBoundingClientRect(), pr = node.getBoundingClientRect();
  node.style.left = Math.max(4, Math.min(fr.left, window.innerWidth - pr.width - 4)) + 'px';
  node.style.top = (fr.bottom + 4 + pr.height > window.innerHeight ? Math.max(4, fr.top - pr.height - 4) : fr.bottom + 4) + 'px';
  capturePointer(true);
  colorPop = { node, anchor, close: () => { node.remove(); colorPop = null; capturePointer(!!openMenuState || !!$('.modal-back')); } };
}
const popoverFor = anchor => !!colorPop && colorPop.anchor === anchor;
document.addEventListener('mousedown', e => {
  if (colorPop && !colorPop.node.contains(e.target) && !colorPop.anchor.contains(e.target)) closeColorPop();
});
document.addEventListener('keydown', e => { if (e.key === 'Escape' && colorPop) { closeColorPop(); e.stopPropagation(); } }, true);

// A colour field: the colour itself, full width. Clicking it opens a picker under it: a hue /
// saturation wheel, a brightness bar, and the breakdown (R G B 0..255, hex). `linear`: [r, g, b];
// onPreview while dragging, onCommit on release or an edited value (one undo step each).
function colorField(linear, onPreview, onCommit) {
  let srgb = linear.map(toSrgb);
  const field = el('div', { class: 'color-field', role: 'button' });
  const paintField = () => { field.style.background = cssColor(srgb); };
  paintField();
  const emit = (fn) => fn(srgb.map(toLinear));
  field.addEventListener('mousedown', e => {
    e.stopPropagation();
    if (colorPop && colorPop.anchor === field) { closeColorPop(); return; }
    closeColorPop();
    openPicker();
  });

  function openPicker() {
    const SIZE = 168, R0 = SIZE / 2 - 2;
    let [h, s, v] = rgbToHsv(srgb);
    const wheel = el('canvas', { class: 'cp-wheel', width: SIZE, height: SIZE });
    const bar = el('div', { class: 'cp-bar' });
    const barThumb = el('span', { class: 'cp-bar-thumb' });
    bar.append(barThumb);
    const mark = el('span', { class: 'cp-mark' });
    const wheelBox = el('div', { class: 'cp-wheel-box' }, wheel, mark);
    const preview = el('span', { class: 'cp-preview' });
    const fields = ['R', 'G', 'B'].map((ch, i) => {
      const input = numberInput(Math.round(srgb[i] * 255), x => { srgb[i] = Math.min(255, Math.max(0, x)) / 255; fromRgb(); emit(onCommit); }, 0);
      return { input, row: el('div', { class: 'cp-field' }, el('span', { class: 'cp-ch' }, ch), input) };
    });
    const hex = el('input', { class: 'field cp-hex', value: hexOf(srgb), spellcheck: 'false' });
    hex.addEventListener('keydown', e => { if (e.key === 'Enter') hex.blur(); e.stopPropagation(); });
    hex.addEventListener('change', () => {
      const m = /^#?([0-9a-f]{6})$/i.exec(hex.value.trim());
      if (!m) { hex.value = hexOf(srgb); return; }
      srgb = [0, 2, 4].map(k => parseInt(m[1].slice(k, k + 2), 16) / 255);
      fromRgb();
      emit(onCommit);
    });
    const node = el('div', { class: 'color-pop' }, wheelBox, bar,
      el('div', { class: 'cp-breakdown' }, preview, ...fields.map(f => f.row), el('div', { class: 'cp-field' }, el('span', { class: 'cp-ch' }, '#'), hex)));

    // The wheel: hue around, saturation outwards, at full brightness (the bar dims it).
    const ctx = wheel.getContext('2d');
    const img = ctx.createImageData(SIZE, SIZE);
    for (let y = 0; y < SIZE; ++y)
      for (let x = 0; x < SIZE; ++x) {
        const dx = x + 0.5 - SIZE / 2, dy = y + 0.5 - SIZE / 2, r = Math.hypot(dx, dy);
        const o = (y * SIZE + x) * 4;
        if (r > R0 + 1) continue;
        const [cr, cg, cb] = hsvToRgb((Math.atan2(-dy, dx) / (2 * Math.PI) + 1) % 1, Math.min(1, r / R0), 1);
        img.data[o] = cr * 255; img.data[o + 1] = cg * 255; img.data[o + 2] = cb * 255;
        img.data[o + 3] = 255 * Math.min(1, R0 + 1 - r);  // antialiased rim
      }
    ctx.putImageData(img, 0, 0);

    const show = () => {
      const a = h * 2 * Math.PI;
      mark.style.left = SIZE / 2 + Math.cos(a) * s * R0 + 'px';
      mark.style.top = SIZE / 2 - Math.sin(a) * s * R0 + 'px';
      wheel.style.filter = `brightness(${0.25 + 0.75 * v})`;
      bar.style.background = `linear-gradient(to right, #000, ${cssColor(hsvToRgb(h, s, 1))})`;
      barThumb.style.left = v * 100 + '%';
      preview.style.background = cssColor(srgb);
      fields.forEach((f, i) => { if (document.activeElement !== f.input) f.input.value = String(Math.round(srgb[i] * 255)); });
      if (document.activeElement !== hex) hex.value = hexOf(srgb);
      paintField();
    };
    const fromRgb = () => { [h, s, v] = rgbToHsv(srgb); show(); };
    const fromHsv = () => { srgb = hsvToRgb(h, s, v); show(); };

    // Dragging on the wheel or the bar previews; the release commits.
    const drag = (target, set) => target.addEventListener('mousedown', e => {
      e.preventDefault();
      e.stopPropagation();
      set(e);
      emit(onPreview);
      const move = ev => { set(ev); emit(onPreview); };
      const up = ev => {
        set(ev);
        document.removeEventListener('mousemove', move);
        document.removeEventListener('mouseup', up);
        emit(onCommit);
      };
      document.addEventListener('mousemove', move);
      document.addEventListener('mouseup', up);
    });
    drag(wheelBox, e => {
      const r = wheel.getBoundingClientRect();
      const dx = e.clientX - (r.left + r.width / 2), dy = e.clientY - (r.top + r.height / 2);
      h = (Math.atan2(-dy, dx) / (2 * Math.PI) + 1) % 1;
      s = Math.min(1, Math.hypot(dx, dy) / R0);
      fromHsv();
    });
    drag(bar, e => {
      const r = bar.getBoundingClientRect();
      v = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width));
      fromHsv();
    });

    document.body.append(node);
    const fr = field.getBoundingClientRect(), pr = node.getBoundingClientRect();
    node.style.left = Math.max(4, Math.min(fr.right - pr.width, window.innerWidth - pr.width - 4)) + 'px';
    node.style.top = (fr.bottom + 4 + pr.height > window.innerHeight ? fr.top - pr.height - 4 : fr.bottom + 4) + 'px';
    field.classList.add('open');
    show();
    capturePointer(true);
    colorPop = {
      node, anchor: field,
      close: () => { node.remove(); field.classList.remove('open'); colorPop = null; capturePointer(!!openMenuState || !!$('.modal-back')); },
    };
  }
  return field;
}

// A point or spot light, edited like Unity's Light component (sliders preview while dragged;
// releasing them is one undo step).
function lightComponent(ins) {
  const L = { ...ins.light, color: [...ins.light.color] };
  const payload = () => {  // "type" is the message's own key: the light's goes as lightType
    const { type, ...rest } = L;
    return { id: ins.id, lightType: type, ...rest };
  };
  const commit = () => send('editor.light.set', payload());
  const preview = () => send('editor.light.preview', payload());
  const slider = (label, key, min, max, step, digits = 2) => {
    const clamp = v => Math.min(max, Math.max(min, v));
    const range = el('input', { type: 'range', min, max, step, value: L[key] });
    const input = numberInput(L[key], v => { L[key] = clamp(v); setRange(range, L[key]); commit(); }, digits);
    range.addEventListener('input', () => { L[key] = +range.value; input.value = fmt(L[key], digits); preview(); });
    range.addEventListener('change', commit);
    const lab = el('label', { class: 'scrub' }, label);
    scrub(lab, () => L[key], v => { L[key] = clamp(v); setRange(range, L[key]); input.value = fmt(L[key], digits); preview(); },
      v => { L[key] = clamp(v); commit(); }, (max - min) / 300);
    return el('div', { class: 'prop' }, lab, el('div', { class: 'slider-row' }, range, input));
  };
  const type = el('select', { class: 'field' }, [['directional', 'Directional'], ['point', 'Point'], ['spot', 'Spot']]
    .map(([v, t]) => el('option', { value: v, selected: L.type === v }, t)));
  type.addEventListener('change', () => { L.type = type.value; commit(); });
  // Colour: the colour itself; a click opens the picker (no native one in the off-screen browser).
  // Only the colour goes: the picker outlives this render (the engine keeps the other values).
  const color = colorField(L.color, c => { L.color = c; send('editor.light.preview', { id: ins.id, color: c }); },
    c => { L.color = c; send('editor.light.set', { id: ins.id, color: c }); });
  const shadows = el('input', { type: 'checkbox', class: 'check', checked: L.shadows });
  shadows.addEventListener('change', () => { L.shadows = shadows.checked; commit(); });
  const rows = [
    el('div', { class: 'prop' }, el('label', {}, 'Type'), type),
    el('div', { class: 'prop' }, el('label', {}, 'Color'), color),
  ];
  if (L.type === 'directional') {
    // The sun: rotate it to move the sun; its shadows (the clipmap) are always on.
    rows.push(tip(slider('Intensity', 'intensity', 0, 20, 0.01, 2), 'Illuminance; the default sun is 3.2'));
  } else {
    rows.push(tip(slider('Intensity', 'intensity', 0, 200, 0.1, 1), 'Candela'),
      tip(slider('Range', 'range', 0, 100, 0.1, 1), '0: no limit (it reaches as far as its light shows, from its intensity); more: its light stops there'));
    if (L.type === 'spot')
      rows.push(tip(slider('Spot Angle', 'outer', 1, 89, 0.5, 1), 'Half angle of the cone, degrees'),
        tip(slider('Inner Angle', 'inner', 0, 89, 0.5, 1), 'Half angle where the falloff starts, degrees'));
    rows.push(tip(el('div', { class: 'prop' }, el('label', {}, 'Cast Shadows'), shadows), 'Up to 32 lights cast shadows'));
  }
  return component('Light', 'light', rows, el('span', { html: icon(L.type === 'spot' ? 'spot' : L.type === 'directional' ? 'sun' : 'light') }));
}

// An outline's or a river's points (body-local x, z; a river's also its height): edited as
// numbers, added after the last or removed.
function pointsEditor(W, set) {
  const pts = [];
  for (let i = 0; i + 1 < (W.points || []).length; i += 2) pts.push([W.points[i], W.points[i + 1]]);
  const heights = (W.heights || []).slice();
  const river = W.shape === 'river';
  const commit = () => set({ points: pts.flat(), ...(river ? { heights: pts.map((_, i) => heights[i] || 0) } : {}) });
  const rows = pts.map((p, i) => {
    const field = (value, onSet) => numberInput(value, v => { onSet(v); commit(); }, 2);
    return el('div', { class: 'prop point-row' }, el('label', {}, `Point ${i + 1}`),
      el('div', { class: 'row-buttons' },
        field(p[0], v => { p[0] = v; }), field(p[1], v => { p[1] = v; }),
        ...(river ? [field(heights[i] || 0, v => { heights[i] = v; })] : []),
        el('span', { class: 'reset-field', role: 'button', title: 'Remove the point', onclick: () => {
          if (pts.length > (river ? 2 : 3)) { pts.splice(i, 1); heights.splice(i, 1); commit(); }
        } }, '\u2715')));
  });
  const add = actionButton('Add Point', () => {
    const a = pts[pts.length - 1], b = pts[pts.length - 2] || [0, 0];
    pts.push([a[0] + (a[0] - b[0]), a[1] + (a[1] - b[1])]);
    heights.push(heights[heights.length - 1] || 0);
    commit();
  });
  return el('div', { class: 'points' },
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, river ? 'x, z and height of each point, upstream first (metres, from the water\'s origin)' : 'x, z of each corner (metres, from the water\'s origin)')),
    ...rows, el('div', { class: 'prop' }, el('label', {}, ''), add));
}

// A body of water: its shape and size, its water material, and every field of that material
// (from the engine's schema, grouped), each an override on this body when it differs from the
// material's own value (marked, with a button back to the material's). Sliders preview while
// dragged; releasing them is one undo step.
function waterComponents(ins) {
  const W = ins.water, mat = ins.waterMaterial || {}, base = ins.waterBase || {};
  const set = (fields, live) => send(live ? 'editor.water.preview' : 'editor.water.set', { id: ins.id, ...fields });
  // A slider that previews while dragged and commits (one undo step) on release or a typed value.
  const slider = (label, get, preview, commit, min, max, step, digits) => {
    let v = get();
    const row = valueSlider(label, () => v, x => { v = x; preview(x); }, min, max, step, digits);
    row.querySelector('input[type=range]').addEventListener('change', () => commit(v));
    row.querySelector('.scrub').addEventListener('mouseup', () => commit(v));
    const typed = row.querySelector('input:not([type=range])');
    if (typed) typed.addEventListener('change', () => commit(v));
    return row;
  };
  const sizeSlider = (label, key, min, max, digits = 2) =>
    slider(label, () => W[key], x => set({ [key]: x }, true), x => set({ [key]: x }, false), min, max, 0.05, digits);
  const shapes = [['rect', 'Rectangle'], ['circle', 'Circle'], ['polygon', 'Outline'], ['river', 'River'], ['box', 'Tank (box)'], ['ocean', 'Ocean (endless)']];
  const isFile = !W.material.startsWith('builtin:');
  const overridden = Object.keys(W.overrides || {}).length;
  const materials = (ins.waterMaterials || []).map(m => [m.value, m.label + (m.value.startsWith('builtin:') ? '' : ' (asset)')]);
  const saveAs = () => {
    const name = el('input', { class: 'field', value: (mat.name || 'Water').replace(/[^\w -]/g, ''), spellcheck: 'false' });
    const folder = (typeof Assets !== 'undefined' && Assets.folder && Assets.folder()) || 'Assets';
    const go = () => send('editor.water.saveAsset', { id: ins.id, path: `${folder}/${name.value.trim() || 'Water'}.fwater` });
    name.addEventListener('keydown', e => { if (e.key === 'Enter') { closeShortcuts(); go(); } });
    dialog('Save water material', [el('div', { class: 'modal-text' }, `This water's look as a material in ${folder}, shared by any water that uses it.`), name],
      [{ label: 'Cancel' }, { label: 'Save', primary: true, action: go }]);
  };
  const body = [
    choice('Shape', shapes, W.shape, v => set({ shape: v })),
    sizeSlider(W.shape === 'circle' ? 'Diameter' : 'Width', 'width', 0.5, 500, 1),
    ...(W.shape === 'circle' ? [] : [sizeSlider('Length', 'length', 0.5, 500, 1)]),
    tip(sizeSlider('Depth', 'depth', 0, 100, 2), 'Below the surface: a tank\'s height, and how deep the water is where nothing is under it'),
    ...((W.shape === 'polygon' || W.shape === 'river') ? [pointsEditor(W, set)] : []),
    choice('Material', materials, W.material, v => set({ material: v })),
    el('div', { class: 'prop' }, el('label', {}, ''), el('div', { class: 'row-buttons' },
      actionButton('Save as Asset', saveAs),
      ...(isFile && overridden ? [actionButton('Apply to Asset', () => send('editor.water.applyAsset', { id: ins.id }))] : []),
      ...(overridden ? [actionButton(`Reset ${overridden} override${overridden > 1 ? 's' : ''}`, () => set({ resetAll: true }))] : []))),
  ];
  const sections = [component('Water', 'water', body, el('span', { html: icon('water') })).node];
  // The material's fields, by group.
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const groups = new Map();
  for (const f of ins.waterSchema || []) {
    if (!groups.has(f.group)) groups.set(f.group, []);
    const field = (value, live) => set({ field: f.key, value }, live);
    let row;
    if (f.kind === 'number') {
      const digits = f.step >= 1 ? 0 : f.step >= 0.1 ? 1 : f.step >= 0.01 ? 2 : 3;
      row = slider(f.label, () => mat[f.key], x => field(x, true), x => field(x, false), f.min, f.max, f.step, digits);
    } else if (f.kind === 'color') {
      row = el('div', { class: 'prop' }, el('label', {}, f.label), colorField(mat[f.key], c => field(c, true), c => field(c, false)));
    } else if (f.kind === 'bool') {
      const box = el('input', { type: 'checkbox', class: 'check', checked: !!mat[f.key] });
      box.addEventListener('change', () => field(box.checked, false));
      row = el('div', { class: 'prop' }, el('label', {}, f.label), box);
    } else if (f.kind === 'sound') {
      row = el('div', { class: 'prop', 'data-water-sound': f.key }, el('label', {}, f.label), Assets.soundPicker(mat[f.key] || '', p => field(p, false), 'Synthesized'));
    } else {
      row = choice(f.label, f.choices.map(c => [c, c[0].toUpperCase() + c.slice(1)]), mat[f.key], v => field(v, false));
    }
    if (!same(mat[f.key], base[f.key])) {
      row.classList.add('overridden');
      const label = row.querySelector('label');
      if (label) label.append(el('span', { class: 'reset-field', role: 'button', title: 'Back to the material\'s value',
        onclick: e => { e.stopPropagation(); send('editor.water.set', { id: ins.id, field: f.key }); } }, '\u21ba'));
    }
    groups.get(f.group).push(f.tip ? tip(row, f.tip) : row);
  }
  for (const [group, rows] of groups)
    sections.push(component(`Water: ${group}`, 'water.' + group, rows).node);
  return sections;
}

// A camera: field of view, near clip and whether it is the main one (Play, the Game view and
// the game look through it).
function cameraComponent(ins) {
  const C = { ...ins.camera };
  const payload = () => ({ id: ins.id, ...C });
  const commit = () => send('editor.camera.set', payload());
  const preview = () => send('editor.camera.preview', payload());
  const range = el('input', { type: 'range', min: 1, max: 179, step: 0.5, value: C.fov });
  const fov = numberInput(C.fov, v => { C.fov = Math.min(179, Math.max(1, v)); setRange(range, C.fov); commit(); }, 1);
  range.addEventListener('input', () => { C.fov = +range.value; fov.value = fmt(C.fov, 1); preview(); });
  range.addEventListener('change', commit);
  const fovLabel = el('label', { class: 'scrub' }, 'Field of View');
  scrub(fovLabel, () => C.fov, v => { C.fov = Math.min(179, Math.max(1, v)); setRange(range, C.fov); fov.value = fmt(C.fov, 1); preview(); },
    v => { C.fov = Math.min(179, Math.max(1, v)); commit(); }, 0.5);
  const near = numberInput(C.near, v => { C.near = Math.max(0.001, v); commit(); }, 3);
  const main = el('input', { type: 'checkbox', class: 'check', checked: C.main });
  main.addEventListener('change', () => { C.main = main.checked; commit(); });
  return component('Camera', 'camera', [
    tip(el('div', { class: 'prop' }, fovLabel, el('div', { class: 'slider-row' }, range, fov)), 'Vertical field of view, degrees'),
    el('div', { class: 'prop' }, el('label', {}, 'Near Clip'), near),
    tip(el('div', { class: 'prop' }, el('label', {}, 'Main Camera'), main), 'Play, the Game view and the game look through the main camera'),
  ], el('span', { html: icon('camera') }));
}

// "MeshInstance" -> "Mesh Instance", "gpuSlot" -> "Gpu Slot".
const prettify = name => name.replace(/^.*\./, '').replace(/([a-z0-9])([A-Z])/g, '$1 $2').replace(/^./, c => c.toUpperCase());

const COMPONENT_ICONS = { MeshInstance: 'cube', RigidBody: 'sphere' };

// A component without its own editor: its header (Mesh Instance's carries the visibility
// checkbox, as on Unity's renderers). Engine-internal fields (GPU slots, mesh indices) are not
// shown: nothing in a component is read-only; the header's tooltip names its mesh.
function ecsComponent(ins, c) {
  const rows = [];
  let enable = null;
  if (c.name === 'MeshInstance') {
    enable = el('input', { type: 'checkbox', class: 'check', checked: ins.render.visible, title: 'Visible' });
    enable.addEventListener('change', () => send('editor.render.set', { id: ins.id, visible: enable.checked }));
  }
  const head = el('span', { html: icon(COMPONENT_ICONS[c.name] || 'data') });
  const section = component(prettify(c.name), 'ecs.' + c.name, rows, enable ? [head, enable] : head);
  section.node.classList.add('empty-body');
  if (c.name === 'MeshInstance' && ins.render) tip(section.head, ins.render.path);
  return section.node;
}

// ---------------------------------------------------------------- add / remove component

// What Add Component can put on an entity (the engine lists which, as "addable"): its label,
// icon and the words a search also finds it by.
const ADDABLE_COMPONENTS = {
  Light: { label: 'Light', icon: 'light', words: 'point spot directional sun lamp rendering' },
  CameraComponent: { label: 'Camera', icon: 'camera', words: 'view main game rendering' },
  PhysicsBody: { label: 'Rigidbody', icon: 'rigidbody', words: 'physics body mass gravity dynamic kinematic' },
  BoxCollider: { label: 'Box Collider', icon: 'cube', words: 'physics collision shape' },
  SphereCollider: { label: 'Sphere Collider', icon: 'sphere', words: 'physics collision shape ball' },
  CapsuleCollider: { label: 'Capsule Collider', icon: 'capsule', words: 'physics collision shape character pill' },
  MeshCollider: { label: 'Mesh Collider', icon: 'model', words: 'physics collision shape' },
  AudioSource: { label: 'Audio Source', icon: 'audio', words: 'sound clip play music speaker emitter' },
  AudioListener: { label: 'Audio Listener', icon: 'listener', words: 'ears hear microphone sound' },
  Animator: { label: 'Animator', icon: 'character', words: 'animation graph skeleton skinned character state machine' },
  ParticleEffect: { label: 'Particle Effect', icon: 'sparkle', words: 'particles vfx effect fx smoke fire sparks system' },
  ParticleSphereEmitter: { label: 'Sphere Emitter', icon: 'sphere', words: 'particles emitter spawn burst rate' },
  ParticleBoxEmitter: { label: 'Box Emitter', icon: 'cube', words: 'particles emitter spawn burst rate volume' },
  ParticleConeEmitter: { label: 'Cone Emitter', icon: 'cone', words: 'particles emitter spawn burst rate spray' },
  ParticleRingEmitter: { label: 'Ring Emitter', icon: 'ring', words: 'particles emitter spawn burst rate circle shockwave' },
  ParticleSpriteRenderer: { label: 'Sprite Renderer', icon: 'image', words: 'particles renderer billboard sprite flipbook sheet' },
  ParticleTrailRenderer: { label: 'Trail Renderer', icon: 'trail', words: 'particles renderer ribbon tracer streak' },
  ParticleLightRenderer: { label: 'Light Renderer', icon: 'light', words: 'particles renderer light flash glow' },
  ParticleModelRenderer: { label: 'Model Renderer', icon: 'cube', words: 'particles renderer mesh model 3d debris shell casing chunks' },
  ParticleAttractor: { label: 'Particle Attractor', icon: 'target', words: 'particles controller pull magnet force' },
  ParticleNoise: { label: 'Particle Noise', icon: 'wave', words: 'particles controller turbulence curl swirl wind' },
  ParticleScript: { label: 'Particle Script', icon: 'code', words: 'particles controller c# script code' },
  Decal: { label: 'Decal', icon: 'decal', words: 'decal projector bullet hole parallax stain mark' },
};

// Three numbers in a row (a collider's center, size), each dragged by its axis label like the
// Transform's; `commit` gets the three (one undo step).
function vec3Row(label, values, commit, step = 0.01) {
  const v = [...values];
  const vec = el('div', { class: 'vec' });
  ['X', 'Y', 'Z'].forEach((axis, i) => {
    const lab = el('label', {}, axis);
    const input = numberInput(v[i], x => { v[i] = x; commit(v); });
    scrub(lab, () => v[i], x => { v[i] = x; input.value = fmt(x); }, x => { v[i] = x; commit(v); }, step);
    vec.append(el('div', {}, lab, input));
  });
  return el('div', { class: 'prop' }, el('label', { class: 'scrub' }, label), vec);
}

// A number field whose label scrubs it; `commit` on release or a typed value (one undo step).
function numberRow(label, value, commit, step = 0.01, min = -Infinity, digits = 3) {
  let v = value;
  const clamp = x => Math.max(min, x);
  const input = numberInput(v, x => { v = clamp(x); commit(v); }, digits);
  const lab = el('label', { class: 'scrub' }, label);
  scrub(lab, () => v, x => { v = clamp(x); input.value = fmt(v, digits); }, x => { v = clamp(x); commit(v); }, step);
  return el('div', { class: 'prop' }, lab, input);
}

// Unity's Rigidbody: simulated while the game runs (Play); Stop puts the entity back.
function rigidbodyComponent(ins) {
  const B = ins.body;
  const set = fields => send('editor.body.set', { id: ins.id, ...fields });
  const kinematic = el('input', { type: 'checkbox', class: 'check', checked: B.kinematic });
  kinematic.addEventListener('change', () => set({ kinematic: kinematic.checked }));
  return component('Rigidbody', 'body', [
    tip(numberRow('Mass', B.mass, v => set({ mass: v }), 0.05, 0.0001), 'kg'),
    tip(numberRow('Linear Damping', B.linearDamping, v => set({ linearDamping: v }), 0.01, 0), 'Slows its movement'),
    tip(numberRow('Angular Damping', B.angularDamping, v => set({ angularDamping: v }), 0.01, 0), 'Slows its turning'),
    tip(numberRow('Gravity Scale', B.gravityScale, v => set({ gravityScale: v }), 0.01), '0: it floats; 1: it falls as anything does'),
    tip(el('div', { class: 'prop' }, el('label', {}, 'Is Kinematic'), kinematic),
      'Moved by scripts or animation, not by physics: it pushes others, nothing pushes it'),
  ], el('span', { html: icon('rigidbody') }));
}

// A Box, Sphere, Capsule or Mesh Collider (in the entity's space, scaled with it). Without a
// Rigidbody it is static. Its layer, the layers it ignores, and whether it is a trigger.
function colliderComponent(ins) {
  const C = ins.collider;
  const set = fields => send('editor.collider.set', { id: ins.id, ...fields });
  const rows = [];
  if (C.shape !== 'mesh') rows.push(vec3Row('Center', C.center, v => set({ center: v })));
  if (C.shape === 'box') rows.push(vec3Row('Size', C.size, v => set({ size: v })));
  if (C.shape === 'sphere') rows.push(numberRow('Radius', C.size[0], v => set({ size: [v, v, v] }), 0.01, 0.001));
  if (C.shape === 'capsule')
    rows.push(numberRow('Radius', C.size[0], v => set({ size: [v, C.size[1], v] }), 0.01, 0.001),
      tip(numberRow('Height', C.size[1], v => set({ size: [C.size[0], v, C.size[0]] }), 0.01, 0.001), 'Along its Y, ends included'));
  if (C.shape === 'mesh')
    rows.push(el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'The mesh\'s collider; on a Rigidbody, its bounds as a box')));
  rows.push(tip(numberRow('Friction', C.friction, v => set({ friction: v }), 0.01, 0), '0: ice; 0.6: wood'),
    tip(numberRow('Bounciness', C.bounciness, v => set({ bounciness: Math.min(1, v) }), 0.01, 0), '0: no bounce; 1: keeps all its speed'));
  rows.push(tip(numberRow('Layer', C.layer, v => set({ layer: Math.max(0, Math.min(31, Math.round(v))) }), 1, 0, 0),
    'Its collision layer, 0 to 31 (everything else is on 0)'));
  const ignore = el('input', { type: 'text', value: (C.ignore || []).join(', '), placeholder: 'none' });
  ignore.addEventListener('change', () => set({
    ignore: ignore.value.split(/[\s,]+/).filter(t => t !== '').map(Number).filter(n => Number.isInteger(n) && n >= 0 && n < 32),
  }));
  rows.push(tip(el('div', { class: 'prop' }, el('label', {}, 'Ignores Layers'), ignore), 'Layers it passes through: 2, 5'));
  const trigger = el('input', { type: 'checkbox', class: 'check', checked: !!C.trigger });
  trigger.addEventListener('change', () => set({ trigger: trigger.checked }));
  rows.push(tip(el('div', { class: 'prop' }, el('label', {}, 'Is Trigger'), trigger),
    'Collides with nothing; scripts hear what comes into it and goes out (Physics.Triggers)'));
  const title = { box: 'Box Collider', sphere: 'Sphere Collider', mesh: 'Mesh Collider', capsule: 'Capsule Collider' }[C.shape];
  return component(title, 'collider', rows,
    el('span', { html: icon({ box: 'cube', sphere: 'sphere', mesh: 'model', capsule: 'capsule' }[C.shape]) }));
}

// The level tools' components (level/level_meshes.h): a cut volume, a parametric wall or room, an
// opening in a wall. Each change is one undo step (the selected entity's).
function textRow(label, value, commit, placeholder = '') {
  const input = el('input', { type: 'text', class: 'field', value: value || '', placeholder });
  input.addEventListener('change', () => commit(input.value.trim()));
  return el('div', { class: 'prop' }, el('label', {}, label), input);
}
// A material: the Project's materials in a menu (a click), or one dropped on it from the Project panel.
function materialRow(label, value, commit, noneLabel = 'Default') {
  return el('div', { class: 'prop' }, el('label', {}, label), Assets.materialPick(value || '', commit, { 'data-material-row': label }, noneLabel));
}
function checkRow(label, value, commit) {
  const box = el('input', { type: 'checkbox', class: 'check', checked: !!value });
  box.addEventListener('change', () => commit(box.checked));
  return el('div', { class: 'prop' }, el('label', {}, label), box);
}
function cutComponent(ins) {
  const C = ins.cut;
  const set = fields => send('editor.level.cut', fields);
  const rows = [
    choice('Shape', [['box', 'Box'], ['cylinder', 'Cylinder'], ['sphere', 'Sphere'], ['mesh', 'Its level mesh'], ['damage', 'Damage']], C.shape,
      v => set({ shape: v })),
    tip(checkRow('Intersect', C.intersect, v => set({ intersect: v })), 'Keep only what is inside it (else take it away)'),
    tip(materialRow('Cap Material', C.cap, v => set({ cap: v }), "The mesh's own"), 'The faces it makes'),
  ];
  if (C.shape === 'damage')
    rows.push(tip(numberRow('Seed', C.seed, v => set({ seed: Math.max(0, Math.round(v)) }), 1, 0, 0), 'Its lumps: another seed, another break'),
      tip(numberRow('Roughness', C.roughness, v => set({ roughness: Math.min(0.9, v) }), 0.01, 0), 'How far its lumps reach (of its radius)'),
      el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Another Break', () => set({ seed: Math.floor(Math.random() * 1e9) }))));
  return component('Cut Volume', 'cut', rows, el('span', { html: icon(C.shape === 'damage' || C.shape === 'sphere' ? 'sphere' : C.shape === 'cylinder' ? 'cylinder' : 'cube') }));
}
function wallComponent(ins) {
  const W = ins.wall;
  const set = fields => send('editor.level.wall', fields);
  return component(W.closed ? 'Room' : 'Wall', 'wall', [
    tip(numberRow('Height', W.height, v => set({ height: v }), 0.05, 0.01), 'metres'),
    tip(numberRow('Thickness', W.thickness, v => set({ thickness: v }), 0.01, 0.01), 'metres'),
    choice('Path Along', [['0', 'Its middle'], ['1', 'Its left face'], ['2', 'Its right face']], String(W.align), v => set({ align: +v })),
    tip(checkRow('Closed', W.closed, v => set({ closed: v })), 'Round back to its first point: a room'),
    W.closed ? checkRow('Floor', W.floor, v => set({ floor: v })) : null,
    W.closed ? checkRow('Ceiling', W.ceiling, v => set({ ceiling: v })) : null,
    W.closed ? tip(numberRow('Slab', W.slab, v => set({ slab: v }), 0.01, 0.01), 'The floor and ceiling\'s thickness') : null,
    materialRow('Side A', W.sideA, v => set({ sideA: v })),
    materialRow('Side B', W.sideB, v => set({ sideB: v })),
    materialRow('Top', W.top, v => set({ top: v })),
    W.closed ? materialRow('Floor Material', W.floorMaterial, v => set({ floorMaterial: v })) : null,
    W.closed ? materialRow('Ceiling Material', W.ceilingMaterial, v => set({ ceilingMaterial: v })) : null,
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, `${W.points.length} points`)),
    el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Convert to Mesh', () => send('editor.level.convert', {}))),
  ].filter(Boolean), el('span', { html: icon('cube') }));
}
// A roof (level/roofs.h LevelRoof): its style, height, pitch, overhang and materials, over the outline
// it was made from (Create/Roof over a selected room).
function roofComponent(ins) {
  const R = ins.roof;
  const set = fields => send('editor.level.roof', fields);
  return component('Roof', 'roof', [
    choice('Style', [['flat', 'Flat'], ['shed', 'Shed'], ['gable', 'Gable'], ['hip', 'Hip'], ['pyramid', 'Pyramid']], R.style, v => set({ style: v })),
    tip(numberRow('Base', R.base, v => set({ base: v }), 0.05, -1000), 'Its eaves\' height (metres)'),
    R.style !== 'flat' ? tip(numberRow('Pitch', R.pitch, v => set({ pitch: v }), 1, 1, 0), 'degrees') : null,
    tip(numberRow('Overhang', R.overhang, v => set({ overhang: v }), 0.05, 0), 'Past its outline, all round (metres)'),
    R.style === 'flat' ? tip(numberRow('Thickness', R.thickness, v => set({ thickness: v }), 0.01, 0.01), 'metres') : null,
    materialRow('Material', R.material, v => set({ material: v })),
    materialRow('Underside', R.under, v => set({ under: v })),
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, `${R.outline.length} corners`)),
    el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Convert to Mesh', () => send('editor.level.convert', {}))),
  ].filter(Boolean), el('span', { html: icon('roof') }));
}
// A spline (level/splines.h LevelSpline): closed, smooth or corners, what it is lofted into. Its
// points are edited in the view with the Spline tool.
function splineComponent(ins) {
  const S = ins.spline, L = S.loft || { profile: '' };
  const set = fields => send('editor.level.spline', fields);
  const rows = [
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, `${S.points.length} point${S.points.length === 1 ? '' : 's'}`)),
    tip(checkRow('Closed', S.closed, v => set({ closed: v })), 'Round back to its first point (3 points at least)'),
    el('div', { class: 'prop' }, el('label', {}, 'Points'), el('span', { class: 'modifier-buttons' },
      tip(actionButton('All Smooth', () => set({ corners: false })), 'The curve runs smoothly through every point'),
      tip(actionButton('All Corners', () => set({ corners: true })), 'Straight from point to point'),
      tip(actionButton('Edit', () => send('editor.tool.set', { tool: 'spline' })),
        'The Spline tool: click to add, drag to move, Ctrl+click to put one in between; over a point X takes it out, V makes it a corner; C closes it'))),
    choice('Loft', [['', 'None'], ['road', 'Road'], ['kerb', 'Kerb'], ['wall', 'Wall'], ['rail', 'Rail'], ['pipe', 'Pipe']], L.profile || '',
      v => set({ profile: v })),
    el('div', { class: 'prop' }, el('label', {}, 'Damage'), el('span', { class: 'modifier-buttons' },
      ...[[0, 'Holes', 'Lumpy holes along it'], [1, 'Chips', 'Small chips along it (along an edge)'], [2, 'Breaks', 'Bigger broken pieces along it']].map(([k, text, help]) =>
        tip(actionButton(text, () => { send('editor.tool.set', { paintDamage: k }); send('editor.level.damage', {}); }),
          `${help}, cut into the level meshes it runs over (one undo step). Their size, roughness, spacing, randomness, density, rubble and inside: the Damage brush's (its paint panel)`)))),
  ];
  if (L.profile)
    rows.push(tip(numberRow(L.profile === 'pipe' ? 'Diameter' : 'Width', L.width, v => set({ width: v }), 0.01, 0.001), 'metres'),
      L.profile === 'pipe' ? numberRow('Sides', L.sides, v => set({ sides: Math.round(v) }), 1, 3, 0)
        : tip(numberRow('Height', L.height, v => set({ height: v }), 0.01, 0.001), 'metres'),
      tip(numberRow('Step', L.step, v => set({ step: v }), 0.05, 0.02), 'How finely it follows the curve (metres)'),
      tip(numberRow('UV Length', L.uvLength, v => set({ uvLength: v }), 0.1, 0), 'Metres a texture tile runs along it (0: its width)'),
      materialRow('Material', L.material, v => set({ material: v })));
  return component('Spline', 'spline', rows, el('span', { html: icon('edge') }));
}

// The modifier stack (level/modifiers.h LevelModifiers): each modifier's fields, on or off, its place
// in the stack, Apply (the first one, into the mesh), Remove; Add at the end. Each change one undo step.
const modifierKinds = [['mirror', 'Mirror'], ['array', 'Array'], ['array:radial', 'Radial Array'], ['array:grid', 'Grid Array'],
  ['solidify', 'Solidify'], ['bevel', 'Bevel'], ['deform:bend', 'Bend'], ['deform:twist', 'Twist'], ['deform:taper', 'Taper'],
  ['curve:bend', 'Follow Spline'], ['curve:copies', 'Copies Along Spline']];
function modifierTitle(m) {
  if (m.kind === 'mirror') return 'Mirror ' + 'XYZ'[m.axis || 0];
  if (m.kind === 'array') return m.mode === 'radial' ? 'Radial Array' : m.mode === 'grid' ? 'Grid Array' : 'Array';
  if (m.kind === 'deform') return { bend: 'Bend', twist: 'Twist', taper: 'Taper' }[m.type] || 'Deform';
  if (m.kind === 'curve') return m.follow === 'copies' ? 'Copies Along Spline' : 'Follow Spline';
  if (m.kind === 'script') return (m.class || '').split('.').pop();
  return { solidify: 'Solidify', bevel: 'Bevel' }[m.kind] || m.kind;
}
function addModifier(value) {
  const [kind, variant] = value.split(':');
  send('editor.level.modifier', { op: 'add', kind, ...(kind === 'array' && variant ? { mode: variant } : {}), ...(kind === 'deform' ? { deform: variant } : {}),
    ...(kind === 'curve' ? { follow: variant } : {}), ...(kind === 'script' ? { class: variant } : {}) });
}
function modifiersComponent(ins) {
  const set = (index, fields) => send('editor.level.modifier', { op: 'set', index, ...fields });
  const act = (op, index, extra = {}) => send('editor.level.modifier', { op, index, ...extra });
  const axes = [['0', 'X'], ['1', 'Y'], ['2', 'Z']];
  const rows = [];
  ins.modifiers.forEach((m, k) => {
    const on = el('input', { type: 'checkbox', class: 'check', checked: m.enabled !== false, title: 'On (off: skipped)' });
    on.addEventListener('change', () => set(k, { enabled: on.checked }));
    rows.push(el('div', { class: 'prop modifier-head', 'data-modifier': String(k) }, el('label', {}, on, ' ', modifierTitle(m)),
      el('span', { class: 'modifier-buttons' },
        k > 0 ? tip(actionButton('Up', () => act('up', k)), 'Earlier in the stack') : null,
        k + 1 < ins.modifiers.length ? tip(actionButton('Down', () => act('down', k)), 'Later in the stack') : null,
        k === 0 ? tip(actionButton('Apply', () => act('apply', k)), 'Into the mesh itself (the first in the stack)') : null,
        actionButton('Remove', () => act('remove', k)))));
    if (m.kind === 'mirror') {
      rows.push(choice('Across', axes, String(m.axis), v => set(k, { axis: +v })),
        tip(numberRow('Offset', m.offset, v => set(k, { offset: v }), 0.05), 'Where the mirror is on its axis (metres)'),
        tip(checkRow('Weld', m.weld, v => set(k, { weld: v })), 'The seam joined (the faces on it gone)'));
    } else if (m.kind === 'array') {
      rows.push(choice('Mode', [['linear', 'In a row'], ['radial', 'Around'], ['grid', 'Grid']], m.mode, v => set(k, { mode: v })));
      if (m.mode === 'grid')
        rows.push(vec3Row('Count', m.count, v => set(k, { count: v.map(x => Math.max(1, Math.round(x))) }), 1));
      else
        rows.push(numberRow('Count', m.count[0], v => set(k, { count: [Math.max(1, Math.round(v)), m.count[1], m.count[2]] }), 1, 1, 0));
      if (m.mode === 'radial')
        rows.push(choice('About', axes, String(m.axis), v => set(k, { axis: +v })),
          tip(numberRow('Angle', m.angle, v => set(k, { angle: v }), 1), 'Degrees they spread over (360: all around)'),
          tip(numberRow('Radius', m.radius, v => set(k, { radius: v }), 0.05), 'How far out from the axis (metres)'));
      else
        rows.push(tip(vec3Row('Step', m.step, v => set(k, { step: v }), 0.05), m.relative ? 'Each copy\'s move, times its size' : 'Each copy\'s move (metres)'),
          tip(checkRow('Relative', m.relative, v => set(k, { relative: v })), 'Steps measured in its size'));
      rows.push(tip(vec3Row('Jitter', m.jitter, v => set(k, { jitter: v }), 0.01), 'At random per copy: move (metres), turn (degrees), scale (a fraction)'),
        numberRow('Seed', m.seed, v => set(k, { seed: Math.max(0, Math.round(v)) }), 1, 0, 0),
        tip(checkRow('Weld', m.weld, v => set(k, { weld: v })), 'Copies that touch joined (the faces between them gone)'));
    } else if (m.kind === 'solidify') {
      rows.push(tip(numberRow('Thickness', m.thickness, v => set(k, { thickness: v }), 0.01), 'metres'),
        tip(numberRow('Offset', m.offset, v => set(k, { offset: Math.max(-1, Math.min(1, v)) }), 0.1, -1), '-1: inward, 0: centred, 1: outward'));
    } else if (m.kind === 'bevel') {
      rows.push(tip(numberRow('Width', m.width, v => set(k, { width: v }), 0.005, 0), 'metres'),
        tip(numberRow('Sharp', m.sharp, v => set(k, { sharp: v }), 1, 0), 'Edges whose faces turn by more than this (degrees)'),
        tip(numberRow('Segments', m.segments ?? 1, v => set(k, { segments: Math.max(1, Math.min(32, Math.round(v))) }), 1, 1, 0),
          'Faces across each edge: 1 is a flat cut, more make it round'),
        tip(choice('Curve', [['1', 'Round'], ['0', 'Flat'], ['-1', 'Curved in']], String(Math.round(m.curve ?? 1)),
          v => set(k, { curve: +v, ...((m.segments ?? 1) < 2 && +v !== 0 ? { segments: 4 } : {}) })), 'The shape across the edge (with 2 or more segments)'));
    } else if (m.kind === 'deform') {
      rows.push(choice('Type', [['bend', 'Bend'], ['twist', 'Twist'], ['taper', 'Taper']], m.type, v => set(k, { deform: v })),
        choice('Along', axes, String(m.axis), v => set(k, { axis: +v })),
        m.type === 'taper' ? tip(numberRow('Factor', m.factor, v => set(k, { factor: v }), 0.05, 0), 'Its far end\'s size (1: as it is)')
          : tip(numberRow('Angle', m.angle, v => set(k, { angle: v }), 1), 'Degrees'));
    } else if (m.kind === 'script') {  // the game's C# one: its [Property] members
      const type = (ins.scriptModifiers || []).find(t => t.name === m.class);
      if (!type)
        rows.push(el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'The game has no ' + m.class + ' (renamed, removed, or not built yet?)')));
      else
        rows.push(...propertyRows(type.properties, { ...(type.defaults || {}), ...(m.settings || {}) }, v => set(k, { settings: v }), ins.id));
    } else if (m.kind === 'curve') {
      rows.push(tip(choice('Follow', [['bend', 'Bent along it'], ['copies', 'Copies along it']], m.follow, v => set(k, { follow: v })),
        'The spline it is under (select a spline with it to put it there)'),
        tip(choice('Length Along', axes, String(m.axis), v => set(k, { axis: +v })), 'Its axis that runs along the curve'));
      if (m.follow === 'copies')
        rows.push(tip(numberRow('Spacing', m.spacing, v => set(k, { spacing: v }), 0.1, 0), 'Metres apart (0: its own length)'),
          tip(checkRow('Turn', m.align, v => set(k, { align: v })), 'Turned with the curve'));
      else
        rows.push(tip(checkRow('Fit', m.fit, v => set(k, { fit: v })), 'Copies end to end, stretched to fill it'));
    }
  });
  const pick = el('select', { class: 'field', 'data-add-modifier': '' }, el('option', { value: '' }, 'Add Modifier...'),
    ...modifierKinds.map(([v, t]) => el('option', { value: v }, t)),
    ...(ins.scriptModifiers || []).map(t => el('option', { value: 'script:' + t.name }, t.label + ' (C#)')));
  pick.addEventListener('change', () => { if (pick.value) addModifier(pick.value); });
  rows.push(el('div', { class: 'prop' }, el('label', {}, ''), pick),
    ins.modifiers.length > 1 ? el('div', { class: 'prop' }, el('label', {}, ''), tip(actionButton('Apply All', () => act('apply', 0, { all: true })), 'The whole stack into the mesh')) : null);
  return component('Modifiers', 'modifiers', rows.filter(Boolean), el('span', { html: icon('copy') }));
}

// A level mesh's surface materials (level/poly_mesh.h: three at most, painted over its faces' own),
// each with what it is used for (editor.level.surface): painted on with the brush, or made live along
// the sharp edges, in the corners (stains) or down the sides (streaks), kept up as you build. The
// Inspector's Surface section and the paint panel's list; `brush` (the paint panel): the one the brush
// paints, `onBrush(k)` picks it.
const SURFACE_USES = [
  ['paint', 'Paint', 'Where you paint it on with the brush'],
  ['edges', 'Edge wear', 'Along the sharp outside edges (chipped, worn); kept up as you cut and edit the mesh'],
  ['grime', 'Stain', 'In the inside corners, at the foot and where other level meshes meet it (dirt, moss); kept up as you build'],
  ['streaks', 'Streaks', 'Running down the sides below ledges and sills (rain streaks); kept up as you edit'],
];
const surfaceUseOf = (ins, k) => { const r = (ins.wear || []).find(w => w.layer === k); return r ? r.kind : 'paint'; };
function surfaceList(ins, brush = -1, onBrush = null) {
  const layers = (ins.surface && ins.surface.layers) || [];
  const set = (layer, fields) => send('editor.level.surface', { layer, ...fields });
  const rows = [];
  let n = 0;
  for (let k = 0; k < 3; ++k) {
    if (!layers[k]) continue;
    const use = surfaceUseOf(ins, k);
    const pick = el('select', { class: 'field surf-use', 'data-surface-use': String(k), title: SURFACE_USES.find(u => u[0] === use)[2],
      onchange: e => set(k, { use: e.target.value }) }, ...SURFACE_USES.map(([v, t]) => el('option', { value: v, selected: v === use }, t)));
    rows.push(el('div', { class: 'surf-row' + (brush === k ? ' on' : ''), 'data-surface-layer': String(k),
      // (the row, not its picker, use or remove: the brush's)
      onclick: onBrush ? e => { if (!e.target.closest('.as-mat-slot, select, .surf-remove')) onBrush(k); } : null },
      el('span', { class: 'surf-num', title: onBrush ? 'Paint this one with the brush' : '' }, String(++n)),
      Assets.materialPick(layers[k], p => set(k, { material: p }), { 'data-layer-slot': String(k) }, 'Remove'),
      pick,
      el('span', { class: 'tbtn surf-remove', role: 'button', 'data-surface-remove': String(k), title: 'Remove it (and what was painted of it)',
        html: icon('close'), onclick: () => set(k, { material: '' }) })));
  }
  const free = [0, 1, 2].find(k => !layers[k]);
  rows.push(free === undefined ? el('div', { class: 'palette-hint' }, 'Three materials at most')
    : el('div', { class: 'btn surf-add', role: 'button', 'data-surface-add': '', title: 'A material painted over the faces’ own',
      onclick: ev => Assets.materialMenu(ev, p => {
        set(free, { material: p, use: 'paint' });
        if (onBrush) onBrush(free);
      }) }, el('span', { html: icon('plus') }), 'Add material'));
  return rows;
}

// A level mesh's surface (level/poly_mesh.h): its materials painted over the faces' own and what each
// is for (surfaceList), how softly they blend, and the live wear's settings (Bake: into the painted mask).
function surfaceComponent(ins) {
  const S = ins.surface;
  const layered = S.layers.some(Boolean);
  const rows = [el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {},
    'Materials over the faces’ own: each painted on with the brush (Shift+B), or along the edges, in the corners or down the sides.')),
  el('div', { class: 'surf-list surf-wide' }, ...surfaceList(ins))];
  if (layered)
    rows.push(tip(numberRow('Blend', S.blend, v => send('editor.level.op', { op: 'layers', args: { materials: S.layers, blend: Math.min(1, Math.max(0, v)) } }), 0.01, 0, 2),
      'How softly a material fades into what is under it (by height, like the terrain’s layers)'),
    el('div', { class: 'prop' }, el('label', {}, ''), el('div', { class: 'as-ins-actions' },
      tip(actionButton('Paint', () => send('editor.tools.run', { menu: 'Paint/Layer Brush' }), 'primary'),
        'Paint them on (Shift+B): the panel picks the material, radius and strength'))));
  if (ins.wear && ins.wear.length) rows.push(...wearRows(ins));
  return component('Surface', 'surface', rows, el('span', { html: icon('material') }));
}

// Live wear (level/level_meshes.h LevelWear), in the Surface section: its rules, made again as the mesh
// is edited or cut and as what is around it changes; Bake puts what is made into the painted mask.
function wearRows(ins) {
  const set = (index, fields) => send('editor.level.wear', { op: 'set', index, ...fields });
  const names = { edges: 'Edge Wear', grime: 'Stain', streaks: 'Streaks' };
  const layers = (ins.surface && ins.surface.layers) || [];
  const rows = [el('div', { class: 'prop sub-head', 'data-wear-rules': '' }, el('label', {}, 'Live wear'), el('span', { class: 'muted' }, 'kept up as you edit'))];
  ins.wear.forEach((r, k) => {
    const what = layers[r.layer] ? layers[r.layer].split('/').pop().replace(/\.fmat$/, '') : 'no material';
    rows.push(el('div', { class: 'prop wear-rule' }, el('label', {}, names[r.kind] || r.kind), el('span', { class: 'muted' }, what)));
    rows.push(tip(numberRow(r.kind === 'streaks' ? 'Length' : 'Width', r.width, v => set(k, { width: v }), 0.01, 0.001),
      r.kind === 'streaks' ? 'How far down they run (metres)' : 'How far from an edge it reaches (metres)'));
    rows.push(numberRow('Strength', r.strength, v => set(k, { strength: v }), 0.05, 0));
    if (r.kind === 'grime') rows.push(tip(numberRow('Foot', r.foot, v => set(k, { foot: v }), 0.05, 0), 'How far up from its lowest point (metres)'));
    rows.push(numberRow('Seed', r.seed ?? 1, v => set(k, { seed: Math.max(0, Math.round(v)) }), 1, 0, 0));
  });
  rows.push(el('div', { class: 'prop' }, el('label', {}, ''),
    tip(actionButton('Bake into Mask', () => send('editor.level.wear', { op: 'bake' })), 'What is made now into the painted mask (it stops following the mesh)')));
  return rows;
}
function openingComponent(ins) {
  const O = ins.opening;
  const set = fields => send('editor.level.opening.set', fields);
  return component('Wall Opening', 'opening', [
    tip(numberRow('Distance', O.distance, v => set({ distance: v }), 0.05, 0), 'Along its wall, metres from its start'),
    numberRow('Width', O.width, v => set({ width: v }), 0.05, 0.01),
    numberRow('Height', O.height, v => set({ height: v }), 0.05, 0.01),
    tip(numberRow('Sill', O.sill, v => set({ sill: v }), 0.05, 0), 'Above the wall\'s foot'),
  ], el('span', { html: icon('inset') }));
}

// An acoustic material's controls (assets/acoustic_material.h): a preset, then what it absorbs on
// a bounce, scatters and lets through, per band. `values`: {preset, absorption, scattering,
// transmission}; `change` gets the fields changed (a preset: its values too).
const BANDS = ['Low', 'Mid', 'High'];
function acousticRows(values, presets, change) {
  const V = values;
  const preset = choice('Preset', [['', '(its own values)'], ...presets.map(p => [p.value, p.label])], V.preset || '', v => {
    const p = presets.find(x => x.value === v);
    if (p) change({ preset: v, absorption: p.absorption, scattering: p.scattering, transmission: p.transmission });
  });
  const band = (label, key, k, text) => tip(valueSlider(`${label} ${BANDS[k]}`, () => V[key][k], v => {
    const a = [...V[key]];
    a[k] = v;
    change({ [key]: a });
  }, 0, 1, 0.001, 3), text);
  return [
    tip(preset, 'Steam Audio\'s materials, and some for outdoors'),
    ...[0, 1, 2].map(k => band('Absorbs', 'absorption', k, 'The share of sound taken in on a bounce (soft, porous surfaces: more)')),
    tip(valueSlider('Scatters', () => V.scattering, v => change({ scattering: v }), 0, 1, 0.01), 'The share of the bounce that scatters instead of mirroring'),
    ...[0, 1, 2].map(k => band('Passes', 'transmission', k, 'The share that gets through it (heard through walls)')),
  ];
}

// A mesh's acoustics: its material's, values of its own, or left out (sound passes as if it were
// not there). A moving body (Rigidbody) occludes only when told to.
function acousticsComponent(ins) {
  const A = ins.acoustics;
  const set = fields => send('editor.audio.surface.set', { id: ins.id, ...fields });
  const rows = [
    tip(choice('Acoustics', [['material', `Its material's (${A.material.label})`], ['own', 'Its own'], ['excluded', 'None (sound passes)']],
      A.mode, v => set({ mode: v })), 'How this mesh treats sound: blocks it, lets some through, absorbs it'),
  ];
  if (A.mode === 'own') rows.push(...acousticRows(A.own, A.presets, fields => set({ mode: 'own', ...fields })));
  // What it sounds like hit, scraped or rolled on (physics sounds): its material's, or a preset.
  rows.push(tip(choice('Sounds Like', [['', `Its material's (${A.surfaceMaterial})`], ...A.surfacePresets.map(p => [p.value, p.label])],
    A.surface || '', v => set({ surface: v })), 'What it sounds like when something hits it, scrapes along it or rolls on it'));
  if (A.surface) {
    rows.push(tip(numberRow('Hit Volume', A.impactGain, v => set({ impactGain: v }), 0.01, 0), 'Its physics sounds\' level (1: as the preset)'),
      tip(numberRow('Hit Pitch', A.impactPitch, v => set({ impactPitch: Math.max(0.25, v) }), 0.01, 0.25), 'Their pitch (2: an octave up)'));
  }
  if (A.dynamic) {
    const box = el('input', { type: 'checkbox', class: 'check', checked: A.moving });
    box.addEventListener('change', () => set({ moving: box.checked }));
    rows.push(tip(el('div', { class: 'prop' }, el('label', {}, 'Occludes Moving'), box), 'A Rigidbody blocks sound only when this is on (large moving things: doors, vehicles)'));
  }
  return component('Acoustics', 'acoustics', rows, el('span', { html: icon('listener') }));
}

// Unity's Audio Source: a sound (.fsound) or clip playing at the entity while the game runs (Play
// starts the ones marked Play On Start). Volume and pitch multiply the sound's; its loop, 3D,
// distances and bus are the .fsound's unless overridden here (a clip always uses these).
function audioSourceComponent(ins) {
  const A = ins.audioSource, S = A.soundSettings;
  const set = fields => send('editor.audio.source.set', { id: ins.id, ...fields });
  const check = (label, key, text) => {
    const box = el('input', { type: 'checkbox', class: 'check', checked: !!A[key], 'data-audio': key });
    box.addEventListener('change', () => set({ [key]: box.checked }));
    return tip(el('div', { class: 'prop' }, el('label', {}, label), box), text);
  };
  const own = A.ownSettings || !S;
  const buses = [['sfx', 'Effects'], ['physics', 'Physics'], ['ambience', 'Ambience'], ['music', 'Music'], ['ui', 'UI']];
  const busName = b => (buses.find(x => x[0] === b) || [b, b])[1];
  const rows = [
    ...Assets.soundSlot(ins),
    el('div', { class: 'prop' }, el('label', {}, 'Preview'), el('div', { class: 'row-buttons' },
      actionButton('\u25b6 Play', () => send('editor.audio.play', { id: ins.id })),
      actionButton('\u25a0 Stop', () => send('editor.audio.stop')),
      ...(A.playing ? [el('span', { class: 'muted' }, 'playing')] : []))),
    check('Play On Start', 'playOnStart', 'Plays when the game starts (Play), from where the entity is'),
    tip(numberRow('Volume', A.volume, v => set({ volume: v }), 0.01, 0), 'Multiplies the sound\'s level (1: as it is)'),
    tip(numberRow('Pitch', A.pitch, v => set({ pitch: Math.max(0.01, v) }), 0.01, 0.01), 'Multiplies its pitch (and speed): 2 is an octave up'),
    check('Vary', 'vary', "Each play a little different: the sound's variation, or the project's Sound variation (off: exactly as made)"),
  ];
  if (S) rows.push(check('Override Sound', 'ownSettings', 'Use the settings below instead of the sound\'s own'));
  if (own) {
    rows.push(check('Loop', 'loop', 'Plays until the game stops'),
      check('3D', 'spatial', 'Heard from where the entity is, quieter far away (else straight to both ears)'));
    if (A.spatial) {
      rows.push(tip(numberRow('Min Distance', A.minDistance, v => set({ minDistance: v }), 0.05, 0.01), 'Full volume within this many metres'),
        tip(numberRow('Max Distance', A.maxDistance, v => set({ maxDistance: v }), 0.5, 0.01), 'Silent beyond this many metres'),
        choice('Rolloff', [['inverse', 'Inverse distance'], ['linear', 'Linear']], A.rolloff, v => set({ rolloff: v })),
        check('Air Absorption', 'airAbsorption', 'Far away, the highs fade first (Steam Audio\'s model of air)'),
        check('Occlusion', 'occlusion', 'Muffled behind walls and what else stands between it and the listener'),
        check('Transmission', 'transmission', 'Heard through what is in the way, as its acoustic materials let it'),
        check('Reflections', 'reflections', 'Echoes and reverb from the scene around it'));
    }
    rows.push(choice('Bus', buses, A.bus, v => set({ bus: v })));
  } else {
    const what = [S.loop ? 'loops' : 'plays once', S.spatial ? `3D, ${fmt(S.minDistance, 1)}\u2013${fmt(S.maxDistance, 1)} m${S.rolloff === 'linear' ? ' (linear)' : ''}` : '2D',
      busName(S.bus), `${S.clips} clip${S.clips === 1 ? '' : 's'}`].join(', ');
    rows.push(el('div', { class: 'prop hint' }, el('label', {}, 'The sound'), el('span', {}, what)));
  }
  return component('Audio Source', 'audioSource', rows, el('span', { html: icon('audio') }));
}

// Where the game hears from while it runs (else the main camera).
function audioListenerComponent(ins) {
  const L = ins.audioListener;
  return component('Audio Listener', 'audioListener', [
    tip(numberRow('Gain', L.gain, v => send('editor.audio.listener.set', { id: ins.id, gain: v }), 0.01, 0), 'Multiplies everything heard'),
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'The game hears from here while it runs (the first one), else from the main camera')),
  ], el('span', { html: icon('listener') }));
}

// An Animator: its graph (.fgraph) drives every skinned mesh of the entity and its children, posed
// here and played by Play; its parameters start at these values (the graph's defaults unless set).
function animatorComponent(ins) {
  const A = ins.animator;
  const set = fields => send('editor.animator.set', { id: ins.id, ...fields });
  const graphs = [['', 'None'], ...A.graphs.map(g => [g, g.replace(/^Assets\//, '')])];
  if (A.graph && !A.graphs.includes(A.graph)) graphs.push([A.graph, A.graph + ' (missing)']);
  const rows = [choice('Graph', graphs, A.graph, v => set({ graph: v })),
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, `Drives ${A.meshes} skinned mesh${A.meshes === 1 ? '' : 'es'}`))];
  if (A.graph) rows.push(el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Open in Animator', () => Animator.open(A.graph))));
  // Preview in Editor: the graph plays in edit mode too (IK following its entities as they move);
  // the root stays put, and Play and Stop start it again. Only the setting is saved, never the pose.
  const previewBox = el('input', { type: 'checkbox', class: 'check', checked: !!A.editorPreview, 'data-editor-preview': '' });
  previewBox.addEventListener('change', () => set({ editorPreview: previewBox.checked }));
  rows.push(tip(el('div', { class: 'prop' }, el('label', {}, 'Preview in Editor'), previewBox),
    'Plays the graph while editing (its parameters at the values here), the IK following its entities as you move them; the root stays put. Play and Stop start it again; the pose is never saved'));
  if (A.problem) rows.push(el('div', { class: 'prop hint warn', 'data-animator-problem': '' }, el('label', {}, ''), el('span', {}, A.problem)));
  // IK settings of an older scene (its Foot IK and targets) kept here until moved into the graph,
  // where IK lives now (the Animator tab's IK).
  if (A.ownIk) {
    rows.push(el('div', { class: 'prop hint warn', 'data-own-ik': '' }, el('label', {}, ''), el('span', {}, A.graphIk
      ? "IK settings from an older scene, unused: the graph has IK of its own"
      : "IK settings from an older scene (Foot IK, targets): they belong in the graph's IK now")));
    rows.push(el('div', { class: 'prop' }, el('label', {}, ''), A.graphIk
      ? actionButton('Remove Them', () => set({ dropIk: true }))
      : tip(actionButton('Move IK to the Graph', () => set({ moveIk: true })),
        "Writes them into the graph (its IK, and entity parameters for their targets, bound here)")));
    rows[rows.length - 1].dataset.ownIkAction = '';
  }
  // Entity parameters: the graph's (what its IK follows: an aim's hold, a look's target), each
  // bound here to another entity: picked from the list, or with the pick button and a click in
  // the Hierarchy. Scripts can set them over these (Animation.SetEntity).
  const others = [['0', 'None'], ...state.entities.filter(e => e.id !== ins.id && e.bone === undefined).map(e => [String(e.id), e.name])];
  for (const p of A.entities) {
    const opts = [...others];
    if (p.id && !opts.some(([v]) => +v === p.id)) opts.push([String(p.id), `Entity ${p.id}`]);  // (not listed)
    const row = choice(p.name, opts, String(p.id || 0), v => set({ entities: { [p.name]: +v } }));
    row.dataset.entityParam = p.name;
    row.classList.add('entity-param');
    const picking = ui.picking && ui.picking.id === ins.id && ui.picking.param === p.name;
    const pick = el('span', { class: 'tbtn pick-entity' + (picking ? ' on' : ''), role: 'button', html: icon('target'),
      title: picking ? 'Click an entity in the Hierarchy (Esc: cancel)' : 'Pick: then click an entity in the Hierarchy', 'data-pick-entity': p.name });
    pick.addEventListener('click', () => {
      ui.picking = picking ? null : { id: ins.id, param: p.name };
      if (ui.picking) setStatus(`Click an entity in the Hierarchy for ${p.name} (Esc: cancel)`);
      renderInspector();
    });
    row.append(pick);
    rows.push(tip(row, p.graph ? `The graph's entity parameter${p.use ? ` (its IK's ${p.use})` : ''}: the entity it follows`
      : p.use ? `This Animator's own IK's ${p.use} (from an older scene)` : "Bound here, but the graph has no such entity parameter"));
  }
  for (const p of A.params) {
    const reset = el('span', { class: 'reset-field', role: 'button', title: `Back to the graph's default (${fmt(p.default, 3)})`,
      hidden: !p.own, onclick: () => set({ params: { [p.name]: null } }) }, '↺');
    let row;
    if (p.type === 'bool') {
      const box = el('input', { type: 'checkbox', class: 'check', checked: p.value !== 0, 'data-param': p.name });
      box.addEventListener('change', () => set({ params: { [p.name]: box.checked } }));
      row = el('div', { class: 'prop param' }, el('label', {}, p.name), box, reset);
    } else {
      row = numberRow(p.name, p.value, v => set({ params: { [p.name]: v } }), 0.01);
      row.classList.add('param');
      row.dataset.param = p.name;
      row.append(reset);
    }
    rows.push(tip(row, p.own ? `Set here (the graph's default: ${fmt(p.default, 3)})` : "The graph's default"));
  }
  return component('Animator', 'animator', rows, el('span', { html: icon('character') }));
}

// A component header's ⋮ menu: Remove Component (only for one added with Add Component: a light's
// own Light, a camera's own camera and a prefab's components stay).
function componentMore(c, ins, name) {
  const more = el('span', { class: 'more', role: 'button', html: icon('more'), title: 'More', 'data-component-more': name });
  more.addEventListener('mousedown', e => {
    e.stopPropagation();
    const r = more.getBoundingClientRect();
    openMenu([{ label: 'Remove Component', disabled: !(ins.removable || []).includes(name),
      action: () => send('editor.component.remove', { id: ins.id, component: name }) }], r.left - 150, r.bottom + 2, more);
  });
  c.head.append(more);
  return c;
}

// What Add Component shows for a component: the engine's, or a script component of the game's
// ("Script:<class>", described by ins.scriptTypes).
function addableInfo(name, ins) {
  if (ADDABLE_COMPONENTS[name]) return ADDABLE_COMPONENTS[name];
  if (name.startsWith('Data:')) {
    const t = (ins.dataTypes || []).find(t => t.name === name.slice(5));
    return t ? { label: t.label, icon: 'data', words: 'data component struct ecs ' + t.name } : null;
  }
  if (!name.startsWith('Script:')) return null;
  const t = (ins.scriptTypes || []).find(t => t.name === name.slice(7));
  return t ? { label: t.label, icon: 'code', words: 'script code ' + t.name } : null;
}

// The entity's data components (the game's C# [Component] structs, API v33): a section each, its
// public fields as the struct lays them out (entity fields shown, not set: their ids change from
// run to run), and Remove Component. A struct the game no longer has keeps its bytes, with a warning.
function dataComponents(ins) {
  return (ins.dataComponents || []).map(d => {
    const type = (ins.dataTypes || []).find(t => t.name === d.name);
    const set = (field, value) => send('editor.data.set', { id: ins.id, component: d.name, field, value });
    const rows = [];
    if (!d.known)
      rows.push(el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'The game has no ' + d.name + ' (renamed or removed?): its values are kept')));
    for (const f of d.fields) {
      const label = propLabel(f.name), v = f.values;
      let row;
      switch (f.kind) {
        case 'float': case 'double':
          row = numberRow(label, v[0] || 0, x => set(f.name, x), 0.01);
          break;
        case 'int': case 'uint': case 'long': case 'ulong': case 'byte':
          row = numberRow(label, v[0] || 0, x => set(f.name, Math.round(x)), 1,
            f.kind === 'int' || f.kind === 'long' ? -Infinity : 0, 0);
          break;
        case 'bool': {
          const box = el('input', { type: 'checkbox', class: 'check', checked: !!v[0] });
          box.addEventListener('change', () => set(f.name, box.checked));
          row = el('div', { class: 'prop' }, el('label', {}, label), box);
          break;
        }
        case 'vec3':
          row = vec3Row(label, v, x => set(f.name, x));
          break;
        case 'vec2': case 'vec4': case 'quat':
          row = vecNRow(label, v, f.kind === 'vec2' ? 2 : 4, x => set(f.name, x));
          break;
        default: {  // entity: which one, as the world has it now
          const e = state.entities.find(e => e.id === v[0]);
          row = el('div', { class: 'prop' }, el('label', {}, label), el('span', { class: 'value' }, v[0] ? (e ? e.name : '#' + v[0]) : 'None'));
        }
      }
      rows.push(row);
    }
    const c = component(type ? type.label : d.name, 'data.' + d.name, rows, el('span', { html: icon('data') }));
    c.node.dataset.data = d.name;
    return componentMore(c, ins, 'Data:' + d.name).node;
  });
}

// A property's label: "jumpHeight" / "JumpHeight" -> "Jump Height".
function propLabel(name) {
  return name.replace(/^./, c => c.toUpperCase()).replace(/([a-z0-9])([A-Z])/g, '$1 $2');
}

// N numbers in a row (Vector2, Vector4, Quaternion), each committed on its own.
function vecNRow(label, values, n, commit) {
  const v = [...values];
  const vec = el('div', { class: 'vec' });
  for (let i = 0; i < n; ++i) {
    const lab = el('label', {}, 'XYZW'[i]);
    const input = numberInput(v[i], x => { v[i] = x; commit(v); });
    scrub(lab, () => v[i], x => { v[i] = x; input.value = fmt(x); }, x => { v[i] = x; commit(v); }, 0.01);
    vec.append(el('div', {}, lab, input));
  }
  return el('div', { class: 'prop' }, el('label', { class: 'scrub' }, label), vec);
}

// The entity's script components (C# Falcon.Component classes): a section each, its [Property]
// values (the class's defaults where the scene sets none; while playing, the running one's), its
// checkbox, and Remove Component. A class the game no longer has keeps its values, with a warning.
// A script class's [Property] members as Inspector rows (its describe() form: name, kind, min, max,
// enums, asset, tooltip, group), each change sent through `set` ({name: value}); `self`: the
// entity they are on (an entity property lists the others).
function propertyRows(properties, values, set, self) {
  const rows = [];
  let group = null;
  for (const p of properties || []) {
    if ((p.group || null) !== group) {  // ([Property(Group = "...")]: a heading over its properties)
      group = p.group || null;
      if (group) rows.push(el('div', { class: 'prop-group' }, group));
    }
    const label = propLabel(p.name), value = values[p.name];
    let row;
    switch (p.kind) {
      case 'float': case 'double':
        row = numberRow(label, +value || 0, v => set({ [p.name]: p.max !== undefined ? Math.min(p.max, v) : v }), 0.01,
          p.min !== undefined ? p.min : -Infinity);
        break;
      case 'int': case 'uint': case 'long':
        row = numberRow(label, +value || 0, v => set({ [p.name]: Math.round(p.max !== undefined ? Math.min(p.max, v) : v) }), 1,
          p.kind === 'uint' ? 0 : p.min !== undefined ? p.min : -Infinity, 0);
        break;
      case 'bool': {
        const box = el('input', { type: 'checkbox', class: 'check', checked: !!value });
        box.addEventListener('change', () => set({ [p.name]: box.checked }));
        row = el('div', { class: 'prop' }, el('label', {}, label), box);
        break;
      }
      case 'enum':
        row = choice(label, (p.enums || []).map(e => [e, propLabel(e)]), String(value), v => set({ [p.name]: v }));
        break;
      case 'vec3':
        row = vec3Row(label, value || [0, 0, 0], v => set({ [p.name]: v }));
        break;
      case 'vec2': case 'vec4': case 'quat':
        row = vecNRow(label, value || [0, 0, 0, 0], p.kind === 'vec2' ? 2 : 4, v => set({ [p.name]: v }));
        break;
      case 'entity': {
        const others = [['0', 'None'], ...state.entities.filter(e => e.id !== self && e.bone === undefined).map(e => [String(e.id), e.name])];
        row = choice(label, others, String(value || 0), v => set({ [p.name]: +v }));
        break;
      }
      default: {  // string (an asset's path when it names a kind)
        const input = el('input', { type: 'text', class: 'field', value: value || '', placeholder: p.asset ? '*' + p.asset : '' });
        input.addEventListener('change', () => set({ [p.name]: input.value }));
        row = el('div', { class: 'prop' }, el('label', {}, label), input);
      }
    }
    rows.push(p.tooltip ? tip(row, p.tooltip) : row);
  }
  return rows;
}

function scriptComponents(ins) {
  const sections = [];
  (ins.scripts || []).forEach((sc, index) => {
    const type = (ins.scriptTypes || []).find(t => t.name === sc.class);
    const live = ins.scriptLive && ins.scriptLive[index];
    const values = { ...(type ? type.defaults : {}), ...sc.properties, ...(live || {}) };
    const set = v => send('editor.script.set', { id: ins.id, class: sc.class, values: v });
    const rows = [];
    if (!type)
      rows.push(el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'The game has no ' + sc.class + ' (renamed or removed?): its values are kept')));
    rows.push(...propertyRows(type ? type.properties : [], values, set, ins.id));
    // (API v34) Its [Button] methods, and its own page ([InspectorPage]) under its properties.
    if (type && type.buttons && type.buttons.length)
      rows.push(el('div', { class: 'prop script-buttons' }, el('label', {}, ''), el('div', { class: 'script-button-row' },
        ...type.buttons.map(b => actionButton(b.label, () => send('editor.script.button', { id: ins.id, class: sc.class, method: b.name }))))));
    if (type && type.pageUrl) {
      const slot = el('div', { class: 'drawer-slot', style: `height: ${type.pageHeight || 120}px` });
      rows.push(slot);
      Drawers.place(ins.id + ':' + sc.class, slot, type.pageUrl, sc.class, values, set);
    }
    const enabled = el('input', { type: 'checkbox', class: 'check', checked: sc.enabled, title: 'Enabled' });
    enabled.addEventListener('change', () => send('editor.script.set', { id: ins.id, class: sc.class, enabled: enabled.checked }));
    const c = component(type ? type.label : sc.class, 'script.' + sc.class, rows, el('span', {}, el('span', { html: icon('code') }), enabled));
    c.node.dataset.script = sc.class;
    sections.push(componentMore(c, ins, 'Script:' + sc.class).node);
  });
  return sections;
}

// Components' own Inspector pages ([InspectorPage], API v34): each in a frame over its section's
// slot, kept while its section is there (the Inspector is built again on every change, and a frame
// moved in the page would load again). The page hears the component's values ({type: "values",
// values}) and sets them ({set: {name: value}}: editor.script.set, one undo step).
const Drawers = (() => {
  const frames = new Map();  // entity:class -> { frame, url, slot, cls, values, set }
  let layer = null, looping = false;
  const post = (d, data) => { if (d.frame.contentWindow) d.frame.contentWindow.postMessage({ falcon: 'panel', data }, '*'); };
  function layout() {
    const box = inspector.getBoundingClientRect();
    for (const [key, d] of frames) {
      if (!d.slot.isConnected) { d.frame.remove(); frames.delete(key); continue; }
      const r = d.slot.getBoundingClientRect(), s = d.frame.style;
      const visible = r.height > 0 && r.bottom > box.top && r.top < box.bottom;
      s.display = visible ? '' : 'none';
      if (!visible) continue;
      s.left = r.left + 'px'; s.top = r.top + 'px'; s.width = r.width + 'px'; s.height = r.height + 'px';
      s.clipPath = `inset(${Math.max(0, box.top - r.top)}px 0 ${Math.max(0, r.bottom - box.bottom)}px 0)`;
    }
    looping = frames.size > 0;
    if (looping) requestAnimationFrame(layout);
  }
  function place(key, slot, url, cls, values, set) {
    let d = frames.get(key);
    if (!d || d.url !== url) {
      if (d) d.frame.remove();
      if (!layer) { layer = el('div', { class: 'drawer-layer' }); document.body.append(layer); }
      const frame = el('iframe', { class: 'drawer-frame', src: url, title: cls });
      d = { frame, url };
      frame.addEventListener('load', () => { postTheme(frame); post(d, { type: 'values', values: d.values }); });
      layer.append(frame);
      frames.set(key, d);
    }
    Object.assign(d, { slot, cls, values, set });
    post(d, { type: 'values', values });
    if (!looping) { looping = true; requestAnimationFrame(layout); }
  }
  addEventListener('message', e => {
    if (!e.data || e.data.falcon !== 'panel' || !e.data.data || typeof e.data.data.set !== 'object') return;
    for (const d of frames.values())
      if (e.source === d.frame.contentWindow) d.set(e.data.data.set);
  });
  // A message to the pages of a class (tests: test.drawer).
  const postTo = (cls, data) => { for (const d of frames.values()) if (d.cls === cls) post(d, data); };
  return { place, postTo };
})();

// Unity's Add Component: a button under the components opening a searchable list of the ones
// this entity does not have yet. Typing filters; Enter adds the first match.
function addComponentButton(ins) {
  const button = el('span', { class: 'tbtn wide add-component', role: 'button', 'data-add-component': '' }, 'Add Component');
  button.addEventListener('mousedown', e => {
    if (e.button !== 0) return;
    e.stopPropagation();  // (the outside-click handler would close the menu again)
    if (openMenuState && openMenuState.anchor === button) { closeMenu(); return; }
    const names = (ins.addable || []).filter(n => addableInfo(n, ins));
    const items = names.length ? names.map(n => ({ label: addableInfo(n, ins).label, icon: addableInfo(n, ins).icon,
      action: () => send('editor.component.add', { id: ins.id, component: n }) }))
      : [{ label: 'Nothing more to add', disabled: true }];
    const r = button.getBoundingClientRect();
    openMenu(items, r.left, r.bottom + 2, button);
    const menu = openMenuState.node;
    menu.classList.add('add-component-menu');
    menu.style.minWidth = r.width + 'px';
    const rows = [...menu.querySelectorAll('.menu-item')];
    rows.forEach((row, i) => { if (names[i]) row.dataset.component = names[i]; });
    const none = el('div', { class: 'menu-item disabled', hidden: true }, el('span'), el('span', {}, 'No component matches'));
    menu.append(none);
    const search = el('input', { class: 'field menu-search', placeholder: 'Search…', spellcheck: 'false' });
    search.addEventListener('input', () => {
      const q = search.value.trim().toLowerCase();
      let shown = 0;
      rows.forEach((row, i) => {
        const c = addableInfo(names[i], ins);
        row.hidden = !!q && !!c && !(c.label + ' ' + c.words).toLowerCase().includes(q);
        shown += !row.hidden;
      });
      none.hidden = shown > 0;
    });
    search.addEventListener('keydown', ev => {  // (Escape closes it, as any menu)
      if (ev.key !== 'Enter') return;
      const first = rows.find(row => !row.hidden && !row.classList.contains('disabled'));
      if (first) first.click();
    });
    search.addEventListener('mousedown', ev => ev.stopPropagation());
    menu.prepend(el('div', { class: 'menu-title' }, 'Component'));
    menu.prepend(search);
    setTimeout(() => search.focus());
  });
  return el('div', { class: 'add-component-row' }, button);
}

function renderInspector() {
  // An asset picked in the Project panel shows here until an entity is selected.
  const asset = Assets.inspectorNodes();
  if (asset) {
    inspector.dataset.id = 'asset';
    inspector.replaceChildren(...asset);
    return;
  }
  const ins = state.inspector;
  if (!ins) {
    inspector.replaceChildren(el('div', { class: 'empty' }, 'Select an entity in the Hierarchy or click it in the Scene view.'));
    return;
  }
  // Keep a field the user is typing in: rebuild only when nothing in the inspector has focus.
  if (inspector.contains(document.activeElement) && isTextField(document.activeElement) && inspector.dataset.id === String(ins.id)) return;
  if (pressedIn(inspector) && inspector.dataset.id === String(ins.id)) return deferRender(renderInspector);
  inspector.dataset.id = ins.id;
  const entity = entityById.get(ins.id) || {};
  const kind = entityIcon(entity);
  if (ins.kind === 'bone') return inspector.replaceChildren(...boneInspector(ins, kind));
  const active = el('input', { type: 'checkbox', class: 'check', checked: !entity.hidden, title: 'Visible' });
  active.addEventListener('change', () => send('editor.render.set', { id: ins.id, visible: active.checked }));
  const name = el('input', { class: 'field name-field', value: ins.name });
  name.addEventListener('keydown', e => { if (e.key === 'Enter') name.blur(); if (e.key === 'Escape') { name.value = ins.name; name.blur(); } });
  name.addEventListener('change', () => { if (name.value.trim()) send('editor.entity.rename', { id: ins.id, name: name.value.trim() }); });
  const header = el('div', { class: 'ins-header' }, el('span', { class: 'big-ico', html: icon(kind) }), active, name,
    el('div', { class: 'ins-sub' }, el('span', {}, `Entity ${ins.id}`), el('span', {}, ins.scene)));

  // Every ECS component is its own section, like Unity's components (the transform components
  // and the parent link are the Transform above).
  const hiddenComponents = ['WorldTransform', 'LocalTransform', 'Parent', 'Light', 'CameraComponent', 'PhysicsBody', 'Collider',
    'AudioSource', 'AudioListener', 'AcousticSurface', 'CutVolume', 'LevelWall', 'LevelRoof', 'WallOpening', 'LevelWear', 'LevelModifiers', 'LevelSpline', 'ScatterSet', 'SnapPoint', 'LevelVolume', ...Particles.names];
  const dataNames = (ins.dataComponents || []).map(d => d.name);  // (drawn by dataComponents)
  const sections = (ins.components || []).filter(c => !hiddenComponents.includes(c.name) && !dataNames.includes(c.name)).map(c => ecsComponent(ins, c));
  if (ins.light) sections.unshift(componentMore(lightComponent(ins), ins, 'Light').node);
  if (ins.acoustics) sections.unshift(acousticsComponent(ins).node);
  if (ins.render && ins.render.mesh !== undefined) sections.unshift(component('Material', 'material', Assets.materialSlot(ins)).node);
  if (ins.camera) sections.unshift(componentMore(cameraComponent(ins), ins, 'CameraComponent').node);
  if (ins.water) sections.unshift(...waterComponents(ins));
  if (ins.body) sections.push(componentMore(rigidbodyComponent(ins), ins, 'PhysicsBody').node);
  if (ins.collider) sections.push(componentMore(colliderComponent(ins), ins, 'Collider').node);
  // A level object's sections first, in one order: its own (wall, roof, opening, cut, volume, snap
  // point, spline), its Surface (with its live wear), its Modifiers.
  const level = [];
  if (ins.wall) level.push(wallComponent(ins).node);
  if (ins.roof) level.push(roofComponent(ins).node);
  if (ins.opening) level.push(openingComponent(ins).node);
  if (ins.cut) level.push(cutComponent(ins).node);
  if (ins.volume) level.push(component('Volume', 'volume', [
    choice('Kind', [['trigger', 'Trigger'], ['kill', 'Kill'], ['ladder', 'Ladder'], ['audio', 'Audio'], ['navBlocker', 'Nav Blocker'], ['navArea', 'Nav Area']],
      ins.volume.kind, v => send('editor.level.volume', { kind: v })),
    tip(textRow('Tag', ins.volume.tag, v => send('editor.level.volume', { tag: v }), 'none'), 'What the game makes of it (Falcon.Level.Volumes)'),
    ins.volume.kind === 'navArea' ? tip(numberRow('Cost', ins.volume.cost, v => send('editor.level.volume', { cost: v }), 0.1, 0.01),
      'Walking through it costs this many times as much: paths go round when they can') : null,
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'Not drawn in the game; shape it with the level tools')),
    el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Make It a Mesh', () => send('editor.level.volume', { remove: true }))),
  ].filter(Boolean), el('span', { html: icon('cube') })).node);
  if (ins.snapPoint) level.push(component('Snap Point', 'snapPoint', [
    tip(textRow('Tag', ins.snapPoint.tag, v => send('editor.level.snapPoint', { set: true, tag: v }), 'fits any'),
      'Only snap points with the same tag (or none) join'),
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'Faces out along its blue arrow (+z); kit pieces moved near another snap point click onto it')),
  ], el('span', { html: icon('magnet') })).node);
  if (ins.spline) level.push(splineComponent(ins).node);
  if (ins.surface) level.push(surfaceComponent(ins).node);
  if (ins.modifiers) level.push(modifiersComponent(ins).node);
  if (!ins.surface && ins.wear && ins.wear.length) level.push(component('Live Wear', 'wear', wearRows(ins), el('span', { html: icon('brush') })).node);
  sections.unshift(...level);
  if (ins.clipped)
    sections.unshift(el('div', { class: 'bake-stale', 'data-clipped': '' }, el('span', { html: icon('warning') }),
      el('span', {}, 'Cut without caps: this mesh isn\'t closed, so the cuts through it are clipped open (you can see through them). Close it (Mesh/Fill, Alt+F, on its open edges) for capped cuts.')));
  if (ins.audioSource) sections.push(componentMore(audioSourceComponent(ins), ins, 'AudioSource').node);
  if (ins.audioListener) sections.push(componentMore(audioListenerComponent(ins), ins, 'AudioListener').node);
  if (ins.animator) sections.push(componentMore(animatorComponent(ins), ins, 'Animator').node);
  sections.push(...Particles.sections(ins));
  sections.push(...scriptComponents(ins));
  sections.push(...dataComponents(ins));
  const prefabBar = ins.prefab ? el('div', { class: 'prefab-bar' },
    el('span', { html: icon('prefab') }), el('span', { class: 'prefab-name', title: ins.prefab }, ins.prefab.replace(/^.*\//, '').replace(/\.fprefab$/, '')),
    actionButton('Select', () => Assets.reveal(ins.prefab)),
    actionButton('Apply', () => send('editor.prefab.apply', { id: ins.id })),
    actionButton('Revert', () => send('editor.prefab.revert', { id: ins.id })),
    actionButton('Unpack', () => send('editor.prefab.unpack', { id: ins.id }))) : null;
  inspector.replaceChildren(...[header, prefabBar, transformComponent(ins), ...sections, addComponentButton(ins)].filter(Boolean));
}

// ---------------------------------------------------------------- lighting panel

function setSetting(key, value) {
  if (state.settings) state.settings[key] = value;
  send('editor.settings.set', { key, value });
}

function sliderProp(label, key, min, max, step, digits = 2) {
  const s = state.settings;
  const range = el('input', { type: 'range', min, max, step, value: s[key] });
  const input = numberInput(s[key], v => { setSetting(key, v); setRange(range, v); }, digits);
  range.addEventListener('input', () => { input.value = fmt(+range.value, digits); setSetting(key, +range.value); });
  const lab = el('label', { class: 'scrub' }, label);
  scrub(lab, () => state.settings[key], v => { v = Math.min(max, Math.max(min, v)); setRange(range, v); input.value = fmt(v, digits); setSetting(key, v); },
    v => { v = Math.min(max, Math.max(min, v)); setSetting(key, v); }, (max - min) / 300);
  return el('div', { class: 'prop' }, lab, el('div', { class: 'slider-row' }, range, input));
}
function toggleProp(label, key) {
  const c = el('input', { type: 'checkbox', class: 'check', checked: !!state.settings[key] });
  c.addEventListener('change', () => setSetting(key, c.checked));
  return el('div', { class: 'prop' }, el('label', {}, label), c);
}
// A whole number typed in (an App ID).
function numberFieldProp(label, key) {
  const input = el('input', { class: 'field', value: String(state.settings[key] || 0), spellcheck: 'false' });
  input.addEventListener('change', () => { const v = Math.max(0, Math.floor(+input.value || 0)); input.value = String(v); setSetting(key, v); });
  return el('div', { class: 'prop' }, el('label', {}, label), input);
}
// Text typed in.
function textFieldProp(label, key) {
  const input = el('input', { class: 'field', value: state.settings[key] || '', spellcheck: 'false' });
  input.addEventListener('change', () => setSetting(key, input.value.trim()));
  return el('div', { class: 'prop' }, el('label', {}, label), input);
}
// A select whose values are text (selectProp's are numbers); `then` after a change.
function selectTextProp(label, key, options, then) {
  const sel = el('select', { class: 'field' }, options.map(([v, text]) => el('option', { value: v, selected: String(state.settings[key]) === v }, text)));
  sel.addEventListener('change', () => { setSetting(key, sel.value); if (then) then(); });
  return el('div', { class: 'prop' }, el('label', {}, label), sel);
}
function selectProp(label, key, options) {
  const sel = el('select', { class: 'field' }, options.map(([v, text]) => el('option', { value: v, selected: String(state.settings[key]) === String(v) }, text)));
  sel.addEventListener('change', () => setSetting(key, +sel.value));
  return el('div', { class: 'prop' }, el('label', {}, label), sel);
}

function renderLighting() {
  const root = $('#lighting');
  if (!state.settings) { root.replaceChildren(el('div', { class: 'empty' }, 'Waiting for the engine…')); return; }
  if (root.contains(document.activeElement) && isTextField(document.activeElement)) return;
  if (pressedIn(root)) return deferRender(renderLighting);
  root.replaceChildren(
    component('Global Illumination', 'gi', [
      tip(toggleProp('Probe GI', 'gi'), 'Sunlight, sky light and point and spot lights bouncing round the scene, from light probes round the camera that trace rays through it; off: flat ambient'),
      sliderProp('Bounce intensity', 'giIntensity', 0, 4, 0.01),
      sliderProp('Sky intensity', 'skyIntensity', 0, 4, 0.01),
      tip(sliderProp('Probe spacing', 'giProbeSpacing', 0.5, 2, 0.25), 'Metres between the nearest probes (the next grids double it): closer probes catch smaller rooms and cost the same, but reach less far'),
      tip(selectProp('Rays a frame', 'giProbeRays', [[65536, 'Low (64k)'], [131072, 'Medium (128k)'], [262144, 'High (256k)'], [524288, 'Ultra (512k)']]),
        'How many probes update each frame (64 rays each): more relights faster after a change'),
    ]).node,
    component('Reflections', 'reflections', [
      tip(toggleProp('Reflections', 'reflections'), 'Shiny surfaces reflect what is on screen (traced against depth), elsewhere what a ray through the scene hits, lit by the sun and the probes; off: ambient only'),
      ...(state.settings.reflections ? [
        tip(sliderProp('Intensity', 'reflectIntensity', 0, 2, 0.01), 'Multiplies what reflections add (1: physically based)'),
        tip(toggleProp('Screen space', 'reflectScreenSpace'), 'Trace what is on screen; off: only the ray through the scene (never missing, but lit by the sun and the probes alone)'),
        ...(state.settings.reflectScreenSpace ? [
          tip(sliderProp('Max roughness', 'reflectRoughness', 0.05, 1, 0.01), 'Surfaces rougher than this skip the screen-space trace (its reflections fade out toward the ray through the scene, or the probes\' blur on rough ones)'),
          tip(sliderProp('Thickness', 'reflectThickness', 0.25, 4, 0.05), 'How far behind a surface a ray may pass and still hit it: higher fills gaps behind thin objects, lower avoids smears'),
          tip(selectProp('Quality', 'reflectSteps', [[16, 'Low (16 steps)'], [32, 'Medium (32 steps)'], [64, 'High (64 steps)'], [96, 'Ultra (96 steps)']]),
            'Steps per ray: more find thinner and farther things, and cost more'),
          tip(toggleProp('Half resolution', 'reflectHalfRes'), 'One ray per 2x2 pixels (each pixel combines the four nearest); off: a ray per pixel, sharper, about twice the trace cost'),
        ] : []),
      ] : []),
    ]).node,
    component('Exposure & Tonemapping', 'exposure', [
      sliderProp('Exposure (EV)', 'exposure', -6, 6, 0.1, 1),
      tip(sliderProp('Manual exposure', 'manualExposure', 0, 8, 0.01), '0 = automatic (histogram)'),
      tip(sliderProp('Dark-scene lift (EV)', 'exposureLiftKnee', -1, 10, 0.1, 1),
        'Automatic exposure brightens fully up to this many EV past a sunlit scene\'s, then at half the rate: a dark room stays darker and its doors and windows glare less (-1 = no limit)'),
      sliderProp('AgX contrast', 'lookPower', 0.5, 2.5, 0.01),
      sliderProp('AgX saturation', 'lookSaturation', 0, 2.5, 0.01),
    ]).node,
    component('Post-processing', 'post', [
      toggleProp('Temporal AA', 'taa'),
      tip(toggleProp('FXAA', 'fxaa'), 'Fast approximate AA on the final image'),
      tip(selectProp('MSAA', 'msaa', [[1, 'Off'], [2, '2x'], [4, '4x'], [8, '8x']]), 'Multisampled edges; shading stays one sample per pixel'),
      tip(selectProp('Upscaler', 'upscaler', [[0, 'Off'], [1, 'TAAU'], [2, 'AMD FSR'], [3, 'NVIDIA DLSS']]),
        'Render below the display resolution and upscale. FSR: FSR 4 on supported AMD GPUs, else FSR 3.1. DLSS: NVIDIA RTX GPUs, DLAA at Native AA. Both Direct3D 12; TAAU where they cannot run'),
      ...(state.settings.upscaler ? [
        selectProp('Quality', 'upscaleQuality', [[0, 'Native AA'], [1, 'Quality'], [2, 'Balanced'], [3, 'Performance'], [4, 'Ultra Performance']]),
        ...(state.settings.upscaler === 2 ? [sliderProp('Sharpness', 'sharpness', 0, 1, 0.01)] : []),
        ...(state.settings.upscalerName ? [el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, state.settings.upscalerName))] : []),
      ] : []),
      selectProp('Ambient occlusion', 'ao', [[0, 'GTAO'], [1, 'SSAO'], [2, 'Off']]),
      sliderProp('AO radius', 'aoRadius', 0.05, 3, 0.01),
      sliderProp('AO power', 'aoPower', 0, 4, 0.01),
      toggleProp('AO half res', 'aoHalfRes'),
      sliderProp('Bloom', 'bloom', 0, 1, 0.005, 3),
    ]).node,
    component('Rendering', 'rendering', [
      selectProp('Draw mode', 'debugView', DRAW_MODES.map(([v, t]) => [v, t])),
      tip(sliderProp('Full detail within (m)', 'detailDistance', 0, 200, 0.5, 1), 'Nearer than this, full detail and nothing culled for its size' +
        (state.settings.projectSettings ? '. Saved with the project (File > Project Settings)' : '')),
      tip(sliderProp('LOD error (px)', 'lodPixels', 0, 8, 0.05), state.settings.projectSettings
        ? 'Lower keeps more detail at a distance. Saved with the project (File > Project Settings)' : 'Lower keeps more detail at a distance'),
      tip(sliderProp('Cull size (px)', 'cullPixels', 0, 32, 0.5), 'Objects smaller than this on screen are not drawn (0 = draw everything)' +
        (state.settings.projectSettings ? '. Saved with the project (File > Project Settings)' : '')),
      toggleProp('Occlusion culling', 'occlusion'),
      toggleProp('Backface cone culling', 'coneCulling'),
      toggleProp('Freeze culling', 'freezeCulling'),
      toggleProp('Shadow page cache', 'shadowCache'),
      tip(sliderProp('Shadow budget (ms)', 'shadowBudgetMs', 0, 8, 0.25), 'GPU time a frame for re-rendering the shadows of lights that move (0: no cap). Over it, moving lights take turns and their shadows lag them by a few frames'),
      toggleProp('VSync', 'vsync'),
    ]).node,
    component('Scene Camera', 'camera', [
      sliderProp('Speed', 'cameraSpeed', 0.1, 100, 0.1, 2),
    ], el('span', { html: icon('camera') })).node,
  );
}

// ---------------------------------------------------------------- audio panel

// Baked acoustics: Steam Audio's probes over the floor of the region, the listener's reverb at
// each and the paths sound takes between them, baked on a thread of the engine's into
// <scene>.audio/ (editor.audioBake: its status as it changes).
const BAKE_STAGES = { scene: ['Preparing the scene', 0, 5], probes: ['Placing probes', 5, 10], reverb: ['Baking reverb', 10, 70],
  paths: ['Baking paths', 70, 97], saving: ['Saving', 97, 100] };

function bakeAge(seconds) {
  if (!seconds) return '';
  const s = Math.max(0, Date.now() / 1000 - seconds);
  if (s < 90) return 'just now';
  if (s < 5400) return `${Math.round(s / 60)} min ago`;
  if (s < 129600) return `${Math.round(s / 3600)} h ago`;
  return new Date(seconds * 1000).toLocaleDateString();
}

function renderAudio() {
  const root = $('#audio');
  if (!root) return;
  const B = state.audioBake;
  if (!B) { root.replaceChildren(el('div', { class: 'empty' }, 'Waiting for the engine…')); return; }
  if (root.contains(document.activeElement) && isTextField(document.activeElement)) return;
  if (pressedIn(root)) return deferRender(renderAudio);
  const S = B.settings, I = B.info;
  const set = fields => send('editor.audio.bake.set', fields);
  const baking = B.state === 'baking';
  const status = [];
  if (B.state === 'unavailable') {
    status.push(el('div', { class: 'bake-status' }, 'Baking needs Steam Audio and a scene saved to a file.'));
  } else if (baking) {
    const [label, from, to] = BAKE_STAGES[B.stage] || [B.stage, 0, 100];
    const pct = Math.round(from + (to - from) * Math.min(1, B.fraction || 0));
    const fill = el('div', { class: 'bake-fill' });
    fill.style.setProperty('--pct', pct + '%');  // (the page's CSP drops style attributes)
    status.push(el('div', { class: 'prop', 'data-bake-progress': '' }, el('label', {}, label),
      el('div', { class: 'bake-bar' }, fill, el('span', {}, `${pct}%`))));
    if (B.probes) status.push(el('div', { class: 'bake-status' }, `${B.probes} probes`));
  } else if (B.state === 'none') {
    status.push(el('div', { class: 'bake-status', 'data-bake-state': 'none' },
      'Not baked: reverb and reflections are simulated as you go. Bake for the listener\'s reverb from probes, and sound finding its way around walls (pathing).'));
  } else {
    const what = [I.reverb && 'reverb', I.pathing && 'paths'].filter(Boolean).join(' and ');
    status.push(el('div', { class: 'bake-status', 'data-bake-state': B.state },
      `${I.probes} probes, ${what}, ${(I.bytes / 1048576).toFixed(1)} MB · baked ${bakeAge(I.bakedAt)} in ${Math.max(1, Math.round(I.seconds))} s`));
    if (B.state === 'stale')
      status.push(el('div', { class: 'bake-stale', 'data-bake-stale': '' }, el('span', { html: icon('warning') }),
        el('span', {}, `Out of date: ${B.staleWhy}. Still used until you bake again.`)));
  }
  if (B.error && !baking && B.error !== 'Cancelled')
    status.push(el('div', { class: 'bake-stale error' }, el('span', {}, `The last bake failed: ${B.error}`)));
  const show = el('input', { type: 'checkbox', class: 'check', checked: !!B.showProbes });
  show.addEventListener('change', () => send('editor.audio.bake.show', { probes: show.checked }));
  const region = S.autoRegion ? [] : [
    vec3Row('Center', S.center, v => set({ center: v }), 0.1),
    vec3Row('Size', S.size, v => set({ size: v }), 0.1),
  ];
  const buttons = baking
    ? [actionButton('Cancel', () => send('editor.audio.bake.cancel'))]
    : [actionButton(B.state === 'none' ? 'Bake' : 'Bake again', () => send('editor.audio.bake.start'), 'primary'),
      ...(B.state === 'baked' || B.state === 'stale' ? [actionButton('Clear', () => send('editor.audio.bake.clear'), 'danger')] : [])];
  root.replaceChildren(
    component('Baked Acoustics', 'audioBake', [
      ...status,
      el('div', { class: 'prop' }, el('label', {}, ''), el('div', { class: 'row-buttons' }, ...buttons)),
      tip(el('div', { class: 'prop' }, el('label', {}, 'Show probes'), show), 'Draws the probes and the region in the Scene view'),
    ], el('span', { html: icon('audio') })).node,
    component('Bake Settings', 'audioBakeSettings', [
      tip(numberRow('Spacing (m)', S.spacing, v => set({ spacing: v }), 0.1, 0.5, 2),
        'Between probes on the floor grid: closer catches smaller rooms, and takes longer (four times the probes at half the spacing)'),
      tip(numberRow('Height (m)', S.height, v => set({ height: v }), 0.05, 0.1, 2), 'Above the floor: about where ears are'),
      tip(choice('Quality', [['1', 'Low'], ['2', 'Medium'], ['3', 'High']], String(S.quality), v => set({ quality: +v })),
        'Rays and bounces per probe, and how long a tail is simulated'),
      tip(el('div', { class: 'prop' }, el('label', {}, 'Pathing'), (() => {
        const c = el('input', { type: 'checkbox', class: 'check', checked: !!S.pathing });
        c.addEventListener('change', () => set({ pathing: c.checked }));
        return c;
      })()), 'Also bake the paths between probes, so a sound behind a wall is heard through the doorway around it (sounds with reflections)'),
      tip(choice('Region', [['auto', 'Static objects'], ['box', 'A box']], S.autoRegion ? 'auto' : 'box',
        v => set({ autoRegion: v === 'auto' })), 'Where probes go: around the scene\'s static objects (the terrain when there are none), or a box of your own'),
      ...region,
      el('div', { class: 'prop' }, el('label', {}, ''), el('div', { class: 'row-buttons' },
        tip(actionButton('Fit to objects', () => send('editor.audio.bake.fit', { to: 'objects' })), 'The static objects\' bounds'),
        tip(actionButton('Around the view', () => send('editor.audio.bake.fit', { to: 'view' })), 'A 64 x 24 x 64 m box around the Scene view\'s camera'))),
    ]).node,
  );
}

// ---------------------------------------------------------------- terrain panel

const BRUSH_OPS = [['raise', 'Raise'], ['lower', 'Lower'], ['smooth', 'Smooth'], ['flatten', 'Flatten'], ['set', 'Set Height'],
  ['ramp', 'Ramp'], ['terrace', 'Terrace'], ['noise', 'Noise'], ['sharpen', 'Sharpen'], ['stamp', 'Stamp'],
  ['paint', 'Paint Layer'], ['hole', 'Hole'], ['scatter', 'Scatter Paint'], ['erode', 'Erode']];
const BRUSH_HINTS = {
  raise: 'Raises the ground under the brush (Shift lowers it)', lower: 'Lowers the ground (Shift raises it)',
  smooth: 'Evens out bumps (also Ctrl with any brush)', flatten: 'Levels to the height where the stroke started',
  set: 'Moves the ground toward a fixed height', ramp: 'Drag from one point to another: a slope between their heights',
  terrace: 'Cuts the slope into steps', noise: 'Adds natural bumpiness (Shift carves it in)',
  sharpen: 'Exaggerates ridges and dips', stamp: 'Presses the brush image in as a height map, once per click (Shift carves)',
  paint: 'Paints the chosen material layer (the filters decide where it takes)', hole: 'Cuts the ground out, for caves and tunnels (Shift fills)',
  erode: 'Water and slumping wear the ground where you paint; it runs when you release (strength: how long)',
  scatter: 'Paints a scatter layer\'s meshes onto the terrain (Shift erases). On a layer generated over the terrain, painting clears it instead (Placement)',
};
const FALLOFFS = [['smooth', 'Smooth'], ['linear', 'Linear'], ['sphere', 'Sphere'], ['sharp', 'Sharp'], ['constant', 'Constant'], ['custom', 'Custom']];
const newTerrain = { size: 1024, spacing: 0.5, heightMin: -200, heightMax: 800, hills: 60, seed: 1 };
const heightImport = { image: '', low: 0, high: 100 };

// The top of the Terrain tab: whether dragging in the Scene view sculpts (the brush tool), and
// what with. Picking an operation turns the brush on.
function brushModeBar(t) {
  const on = state.tools.tool === 'brush';
  const can = !!t.exists;
  const toggle = el('span', { class: 'brush-toggle' + (on ? ' on' : ''), role: 'button', 'data-brush-toggle': '',
    title: on ? 'Stop brushing: back to the Move tool (Esc)' : 'Sculpt and paint by dragging over the terrain in the Scene view (B)',
    onclick: () => sendTools({ tool: on ? 'move' : 'brush' }) },
    el('span', { html: icon('brush') }), el('span', {}, on ? 'Brush on' : 'Brush off'), el('span', { class: 'brush-switch' }));
  const tiles = el('div', { class: 'brush-ops' + (on ? ' on' : '') }, ...BRUSH_OPS.map(([v, label]) =>
    el('span', { class: 'brush-op' + (v === t.brush.op ? ' picked' : ''), role: 'button', 'data-brush-op': v, title: BRUSH_HINTS[v] || '',
      onclick: () => { setBrush({ op: v }); if (!on) sendTools({ tool: 'brush' }); renderTerrain(); } }, label)));
  const hint = el('div', { class: 'brush-hint' }, !can ? 'Create a terrain below to sculpt it'
    : on ? 'Drag over the terrain in the Scene view. Shift inverts, Ctrl smooths, [ and ] resize, Esc stops.'
      : 'Pick an operation to start brushing.');
  return el('div', { class: 'brush-mode' + (can ? '' : ' disabled') }, toggle, tiles, hint);
}

function setBrush(changes) {
  if (state.terrain) Object.assign(state.terrain.brush, changes);
  send('editor.terrain.brush', changes);
}

// A slider + number field on a brush setting (or any object/key with a custom sender).
// `open` ([lowest, highest], either may be ±Infinity): min..max is only the slider's span, and a
// value typed or dragged on the label may go past it to those (the slider stretching to show it).
function valueSlider(label, get, set, min, max, step, digits = 2, open = null) {
  const [lo, hi] = open || [min, max];
  const range = el('input', { type: 'range', min, max, step, value: get() });
  const show = v => {
    if (open) { range.min = Math.min(min, v); range.max = Math.max(max, v); }
    setRange(range, v);
  };
  const input = numberInput(get(), v => { v = Math.min(hi, Math.max(lo, v)); set(v); show(v); }, digits);
  range.addEventListener('input', () => { input.value = fmt(+range.value, digits); set(+range.value); });
  const lab = el('label', { class: 'scrub' }, label);
  scrub(lab, get, v => { v = Math.min(hi, Math.max(lo, v)); show(v); input.value = fmt(v, digits); set(v); },
    v => set(Math.min(hi, Math.max(lo, v))), (max - min) / 300);
  if (open) show(get());
  return el('div', { class: 'prop' }, lab, el('div', { class: 'slider-row' }, range, input));
}
const brushSlider = (label, key, min, max, step, digits = 2) =>
  valueSlider(label, () => state.terrain.brush[key], v => setBrush({ [key]: v }), min, max, step, digits);

function choice(label, options, value, onChange) {
  const sel = el('select', { class: 'field' }, options.map(([v, text]) => el('option', { value: v, selected: value === v }, text)));
  sel.addEventListener('change', () => onChange(sel.value));
  return el('div', { class: 'prop' }, el('label', {}, label), sel);
}

function actionButton(text, onClick, cls = '') {
  return el('span', { class: 'tbtn wide ' + cls, role: 'button', onclick: onClick }, text);
}

// The falloff curve: 9 points from the brush center (left) to its edge (right); drag them.
function falloffEditor(brush) {
  const W = 240, H = 72, pad = 6;
  const canvas = el('canvas', { class: 'curve', width: W, height: H });
  const points = brush.falloff.slice();
  const draw = () => {
    const g = canvas.getContext('2d');
    g.clearRect(0, 0, W, H);
    g.strokeStyle = 'rgba(255,255,255,0.12)';
    g.beginPath();
    for (let i = 0; i <= 4; ++i) { const y = pad + (H - 2 * pad) * i / 4; g.moveTo(pad, y); g.lineTo(W - pad, y); }
    g.stroke();
    g.fillStyle = 'rgba(90,160,255,0.18)';
    g.strokeStyle = '#5aa0ff';
    g.lineWidth = 1.5;
    g.beginPath();
    points.forEach((v, i) => { const x = pad + (W - 2 * pad) * i / 8, y = pad + (H - 2 * pad) * (1 - v); if (i) g.lineTo(x, y); else g.moveTo(x, y); });
    g.stroke();
    g.lineTo(W - pad, H - pad); g.lineTo(pad, H - pad); g.closePath(); g.fill();
    g.fillStyle = '#dfe8ff';
    points.forEach((v, i) => { g.beginPath(); g.arc(pad + (W - 2 * pad) * i / 8, pad + (H - 2 * pad) * (1 - v), 3, 0, Math.PI * 2); g.fill(); });
  };
  canvas.addEventListener('mousedown', down => {
    if (down.button !== 0) return;
    down.preventDefault();
    const box = canvas.getBoundingClientRect();
    const at = e => {
      const x = (e.clientX - box.left) / box.width * W, y = (e.clientY - box.top) / box.height * H;
      return [Math.min(8, Math.max(0, Math.round((x - pad) / (W - 2 * pad) * 8))), Math.min(1, Math.max(0, 1 - (y - pad) / (H - 2 * pad)))];
    };
    const [index] = at(down);
    const move = e => { points[index] = at(e)[1]; draw(); };
    const up = () => {
      document.removeEventListener('mousemove', move);
      document.removeEventListener('mouseup', up);
      setBrush({ falloff: points.slice(), falloffPreset: undefined });
      state.terrain.brush.falloffPreset = 'custom';
      deferRender(renderTerrain);
    };
    move(down);
    document.addEventListener('mousemove', move);
    document.addEventListener('mouseup', up);
  });
  draw();
  return el('div', { class: 'curve-box', title: 'Falloff: how strong the brush is from its center (left) to its edge (right). Drag the points.' }, canvas);
}

function renderTerrain() {
  const root = $('#terrain');
  const t = state.terrain;
  if (!t) { root.replaceChildren(el('div', { class: 'empty' }, 'Waiting for the engine…')); return; }
  if (root.contains(document.activeElement) && isTextField(document.activeElement)) return;
  if (pressedIn(root)) return deferRender(renderTerrain);
  const sections = [];
  if (!t.exists) {
    const n = newTerrain;
    const tiles = () => Math.max(1, Math.round(n.size / (256 * n.spacing)));
    sections.push(component('New Terrain', 'terrain.new', [
      choice('Size', [128, 256, 512, 1024, 2048, 4096, 8192].map(s => [String(s), `${s} m`]), String(n.size), v => { n.size = +v; renderTerrain(); }),
      choice('Sample spacing', [['0.25', '0.25 m'], ['0.5', '0.5 m'], ['1', '1 m'], ['2', '2 m']], String(n.spacing), v => { n.spacing = +v; renderTerrain(); }),
      el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, `${tiles()} x ${tiles()} tiles of 256 x 256 samples${tiles() > 64 ? ' (too many: 64 at most)' : ''}`)),
      valueSlider('Lowest height', () => n.heightMin, v => { n.heightMin = v; }, -2000, 0, 10, 0),
      valueSlider('Highest height', () => n.heightMax, v => { n.heightMax = v; }, 10, 4000, 10, 0),
      tip(valueSlider('Hills', () => n.hills, v => { n.hills = v; }, 0, 500, 1, 0), 'Start with rolling hills this high (0: flat)'),
      valueSlider('Seed', () => n.seed, v => { n.seed = Math.round(v); }, 1, 9999, 1, 0),
      el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Create Terrain', () => {
        if (tiles() > 64) return setStatus('A terrain has at most 64 x 64 tiles: pick a larger spacing or a smaller size', 'error');
        send('editor.terrain.create', { tiles: tiles(), spacing: n.spacing, heightMin: n.heightMin, heightMax: n.heightMax, hills: n.hills, seed: n.seed });
      }, 'primary')),
    ], el('span', { html: icon('mountain') })).node);
  } else {
    sections.push(component('Terrain', 'terrain.info', [
      el('div', { class: 'prop' }, el('label', {}, 'Size'), el('span', {}, `${fmt(t.size, 0)} x ${fmt(t.size, 0)} m, ${fmt(t.spacing, 2)} m samples`)),
      el('div', { class: 'prop' }, el('label', {}, 'Heights'), el('span', {}, `${fmt(t.heightMin, 0)} to ${fmt(t.heightMax, 0)} m`)),
      el('div', { class: 'prop' }, el('label', {}, 'Tiles loaded'), el('span', {}, `${t.resident} of ${t.tiles * t.tiles}`)),
      el('div', { class: 'prop' }, el('label', {}, 'Patches'), el('span', {}, `${t.patches} of ${t.patchCapacity}`)),
      tip(valueSlider('Detail (px)', () => t.pixels, v => { t.pixels = v; send('editor.terrain.settings', { pixels: v }); }, 0.25, 8, 0.05, 2),
        'Largest height error a patch may show on screen, in pixels: lower is more detailed and slower'),
      el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Delete Terrain', () => send('editor.terrain.delete'), 'danger')),
    ], el('span', { html: icon('mountain') })).node);
    const hi = heightImport;
    sections.push(component('Import Heightmap', 'terrain.import', [
      tip(choice('Image', [['', 'Pick an image…'], ...(t.images || []).map(i => [i, i])], hi.image, v => { hi.image = v; renderTerrain(); }),
        'A grayscale picture in the scene folder (16-bit PNG for smooth slopes), stretched over the whole terrain. Its top edge is the -Z side.'),
      valueSlider('Black at', () => hi.low, v => { hi.low = v; }, t.heightMin ?? -200, t.heightMax ?? 800, 0.1, 1),
      valueSlider('White at', () => hi.high, v => { hi.high = v; }, t.heightMin ?? -200, t.heightMax ?? 800, 0.1, 1),
      el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Import', () => {
        if (!hi.image) return setStatus('Pick a heightmap image first', 'error');
        send('editor.terrain.import', { image: hi.image, low: hi.low, high: hi.high });
      })),
    ], el('span', { html: icon('mountain') })).node);
  }
  const b = t.brush;
  const op = b.op;
  sections.unshift(brushModeBar(t));
  const opRows = [];
  if (op === 'set') opRows.push(brushSlider('Target height', 'height', t.heightMin ?? -200, t.heightMax ?? 800, 0.1, 1));
  if (op === 'terrace') opRows.push(brushSlider('Step height', 'terraceStep', 0.25, 50, 0.25, 2), brushSlider('Sharpness', 'terraceSharpness', 0, 1, 0.01));
  if (op === 'noise') opRows.push(brushSlider('Feature size', 'noiseScale', 0.5, 200, 0.5, 1), brushSlider('Amplitude', 'noiseAmplitude', 0.05, 20, 0.05, 2));
  if (op === 'stamp') opRows.push(brushSlider('Stamp height', 'stampHeight', 0.5, 200, 0.5, 1));
  const layers = t.layers || [];
  if (op === 'paint') {
    opRows.push(layers.length
      ? choice('Layer', layers.map((l, i) => [String(i), `${i + 1}. ${l.name}`]), String(Math.min(b.layer, layers.length - 1)), v => setBrush({ layer: +v }))
      : el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'Add a layer below first')));
  }
  const scatterLayers = t.scatter?.layers || [];
  if (op === 'scatter') {
    const paintable = scatterLayers.map((l, i) => [l, i]).filter(([l]) => l.paintBit >= 0);
    opRows.push(paintable.length
      ? choice('Scatter layer', paintable.map(([l, i]) => [String(i), `${i + 1}. ${l.name}`]),
        String(paintable.some(([, i]) => i === b.scatterLayer) ? b.scatterLayer : paintable[0][1]), v => setBrush({ scatterLayer: +v }))
      : el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'Add a scatter layer below first')));
    const target = scatterLayers[b.scatterLayer];
    if (target && target.paintBit >= 0 && target.paint === 0)
      opRows.push(el('div', { class: 'prop hint warn' }, el('label', {}, ''),
        el('span', {}, `${target.name} is generated over the terrain: painting shows nothing until its Placement is Painted by hand`)));
  }
  if (op === 'paint' || op === 'hole' || op === 'scatter') {
    opRows.push(
      tip(brushSlider('Slope from', 'slopeMin', 0, 90, 1, 0), 'Only where the ground is at least this steep, degrees'),
      brushSlider('Slope to', 'slopeMax', 0, 90, 1, 0),
      tip(valueSlider('Height from', () => Math.max(b.heightMin, t.heightMin ?? -1e4), v => setBrush({ heightMin: v <= (t.heightMin ?? -1e4) ? -1e5 : v }),
        t.heightMin ?? -200, t.heightMax ?? 800, 0.5, 1), 'Only above this height'),
      valueSlider('Height to', () => Math.min(b.heightMax, t.heightMax ?? 1e4), v => setBrush({ heightMax: v >= (t.heightMax ?? 1e4) ? 1e5 : v }),
        t.heightMin ?? -200, t.heightMax ?? 800, 0.5, 1),
      tip(brushSlider('Noise mask', 'noiseMask', 0, 1, 0.01), 'Breaks the stroke up with world-space noise, as nature would'),
      brushSlider('Noise size', 'noiseMaskScale', 0.5, 100, 0.5, 1));
  }
  const alphas = [['', 'None'], ...(t.alphas || []).map(a => [a, a])];
  sections.push(component('Brush', 'terrain.brush', [
    el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, BRUSH_HINTS[op] || '')),
    tip(brushSlider('Radius', 'radius', 0.5, 300, 0.5, 1), '[ and ] in the Scene view'),
    brushSlider('Strength', 'strength', 0, 1, 0.01),
    ...opRows,
    choice('Falloff', FALLOFFS, b.falloffPreset, v => { if (v !== 'custom') setBrush({ falloffPreset: v }); }),
    falloffEditor(b),
    tip(choice('Shape image', alphas, b.alpha, v => setBrush({ alpha: v })), 'A grayscale picture in the scene folder\'s brushes/ folder: white = full strength'),
    brushSlider('Rotation', 'rotation', -180, 180, 1, 0),
    tip(brushSlider('Spacing', 'spacing', 0.02, 1, 0.01), 'Distance between dabs along a stroke, as a share of the radius'),
    tip(brushSlider('Scatter', 'jitterPosition', 0, 1, 0.01), 'Random offset of each dab, as a share of the radius'),
    tip(brushSlider('Random angle', 'jitterRotation', 0, 180, 1, 0), 'Random rotation of each dab, degrees'),
    tip(brushSlider('Random size', 'jitterScale', 0, 0.9, 0.01), 'Random change of each dab\'s radius'),
  ], el('span', { html: icon('brush') })).node);
  if (t.exists) sections.push(...generateSections(t), ...layerSections(t), ...scatterSections(t));
  const name = el('input', { class: 'field', placeholder: 'Preset name' });
  sections.push(component('Brush Presets', 'terrain.presets', [
    ...(t.presets || []).map(p => el('div', { class: 'prop preset' },
      el('span', { class: 'preset-name', role: 'button', title: 'Use this brush', onclick: () => send('editor.terrain.preset.load', { name: p }) }, p),
      el('span', { class: 'tbtn', role: 'button', title: 'Delete', html: icon('close'), onclick: () => send('editor.terrain.preset.delete', { name: p }) }))),
    el('div', { class: 'prop' }, name, actionButton('Save', () => { if (name.value.trim()) send('editor.terrain.preset.save', { name: name.value.trim() }); })),
  ]).node);
  root.replaceChildren(...sections);
}

// Material layers: the list (click to pick the one to edit and paint), and the picked one's settings.
const GENERATED = [['grass', 'Grass'], ['dirt', 'Dirt'], ['rock', 'Rock'], ['sand', 'Sand'], ['snow', 'Snow']];
let pickedLayer = 0;
function layerSections(t) {
  const layers = t.layers || [];
  pickedLayer = Math.min(pickedLayer, Math.max(layers.length - 1, 0));
  const addButton = actionButton('Add Layer', e => {
    e.stopPropagation();
    menuBelow(e.currentTarget, [
      ...GENERATED.map(([g, label]) => ({ label: `Generated ${label}`, action: () => { pickedLayer = layers.length; send('editor.terrain.layer.add', { generated: g }); } })),
      '-',
      { label: 'From Images (pick them below)', action: () => { pickedLayer = layers.length; send('editor.terrain.layer.add', { generated: '', name: 'Layer ' + (layers.length + 1) }); } },
    ]);
  });
  const rows = layers.map((l, i) => el('div', { class: 'prop preset' + (i === pickedLayer ? ' picked' : '') },
    el('span', { class: 'preset-name', role: 'button', title: 'Edit this layer and paint with it', onclick: () => { pickedLayer = i; setBrush({ layer: i }); renderTerrain(); } },
      `${i + 1}. ${l.name}`),
    el('span', { class: 'muted' }, l.generated ? l.generated : 'images')));
  const list = component('Layers', 'terrain.layers', [
    ...(layers.length ? rows : [el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'No layers: the terrain shows its default look'))]),
    ...(t.building ? [el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'Building layer textures…'))] : []),
    el('div', { class: 'prop' }, addButton, layers.length ? actionButton('Remove Last', () => send('editor.terrain.layer.remove'), 'danger') : null),
  ], el('span', { html: icon('mountain') })).node;
  if (!layers.length) return [list];
  const l = layers[pickedLayer];
  const set = changes => { Object.assign(l, changes); send('editor.terrain.layer.set', { index: pickedLayer, ...changes }); };
  const slider = (label, key, min, max, step, digits = 2) => valueSlider(label, () => l[key], v => set({ [key]: v }), min, max, step, digits);
  const images = [['', 'None'], ...(t.images || []).map(i => [i, i])];
  const nameField = el('input', { class: 'field', value: l.name });
  nameField.addEventListener('change', () => set({ name: nameField.value.trim() || l.name }));
  const hex = el('input', { type: 'checkbox', class: 'check', checked: l.hex });
  hex.addEventListener('change', () => set({ hex: hex.checked }));
  const source = l.generated ? [
    choice('Look', GENERATED, l.generated, v => { set({ generated: v }); renderTerrain(); }),
    slider('Seed', 'seed', 1, 999, 1, 0),
  ] : [
    choice('Colour', images, l.albedo, v => set({ albedo: v })),
    choice('Normal map', images, l.normal, v => set({ normal: v })),
    choice('Roughness', images, l.roughnessMap, v => set({ roughnessMap: v })),
    choice('Occlusion', images, l.ao, v => set({ ao: v })),
    tip(choice('Height', images, l.height, v => set({ height: v })), 'Used for blending against other layers; without it, the colour\'s brightness'),
  ];
  const editor = component(`Layer ${pickedLayer + 1}`, 'terrain.layer', [
    el('div', { class: 'prop' }, el('label', {}, 'Name'), nameField),
    choice('Textures', [['generated', 'Generated'], ['images', 'Images']], l.generated ? 'generated' : 'images',
      v => { set({ generated: v === 'generated' ? 'grass' : '' }); renderTerrain(); }),
    ...source,
    tip(slider('Tile size (m)', 'tileSize', 0.25, 64, 0.25, 2), 'Metres per texture repeat'),
    tip(el('div', { class: 'prop' }, el('label', {}, 'Hex tiling'), hex), 'Every cell of a hex grid shows the texture at its own angle and offset, so it never visibly repeats'),
    tip(slider('Random angle', 'rotation', 0, 1, 0.01), 'Hex cells turn up to this share of a full turn'),
    tip(slider('Cell contrast', 'contrast', 0, 1, 0.01), 'How sharply hex cells meet'),
    tip(slider('Blend width', 'heightBlend', 0.01, 1, 0.01), 'Transitions against other layers: small is sharp (rocks poke through), large is soft'),
    tip(slider('Variation', 'macro', 0, 1, 0.01), 'Large-scale changes of brightness and hue across the ground'),
    slider('Variation size (m)', 'macroScale', 1, 500, 1, 0),
    tip(choice('Projection', [['0', 'From above'], ['1', 'Auto (sides on steep ground)'], ['2', 'Always from the sides too']], String(l.projection),
      v => set({ projection: +v })), 'Steep ground takes the texture from the sides, so cliffs don\'t stretch'),
    slider('Tint red', 'tintR', 0, 2, 0.01),
    slider('Tint green', 'tintG', 0, 2, 0.01),
    slider('Tint blue', 'tintB', 0, 2, 0.01),
    slider('Roughness', 'roughness', 0, 2, 0.01),
    slider('Normal strength', 'normalStrength', 0, 4, 0.01),
    tip(slider('Far scale', 'farScale', 1, 16, 0.5, 1), 'In the distance a larger copy of the texture is blended in, hiding repetition'),
    tip(choice('Sounds like', [['', `Its look (${l.generated || 'generic'})`], ...ACOUSTIC_PRESETS], l.acoustic || '', v => set({ acoustic: v })),
      'Its acoustic material: how the ground blocks, absorbs and lets sound through'),
    tip(choice('Hit sounds', [['', 'As it sounds like'], ...ACOUSTIC_PRESETS], l.surface || '', v => set({ surface: v })),
      'What it sounds like when something hits it, slides or rolls on it'),
  ]).node;
  return [list, editor];
}

// Generation: a stack of steps (noise, terrace, plateau, smooth, erode, paint) run on the GPU over
// the terrain or a square of it. Presets start a stack; every step can be edited.
const ACOUSTIC_PRESETS = [['generic', 'Generic'], ['sand', 'Sand'], ['grass', 'Grass'], ['dirt', 'Dirt'], ['rock', 'Rock'], ['snow', 'Snow'],
  ['gravel', 'Gravel'], ['concrete', 'Concrete'], ['water', 'Water'], ['foliage', 'Foliage']];
const GEN_OPS = [['noise', 'Noise'], ['terrace', 'Terrace'], ['plateau', 'Plateau'], ['smooth', 'Smooth'], ['erode', 'Erode'], ['paint', 'Paint Layer']];
const GEN_NOISE = [['fbm', 'Fractal (fbm)'], ['ridged', 'Ridged (ranges)'], ['billow', 'Billow (rounded)'], ['eroded', 'Eroded fractal'],
  ['cells', 'Cells (mounds)'], ['crags', 'Crags (ridges between cells)'], ['mesas', 'Mesas (flat tops)']];
const GEN_BLEND = [['add', 'Add'], ['max', 'Max'], ['min', 'Min'], ['replace', 'Replace'], ['multiply', 'Multiply']];
let pickedStep = 0;
function generateSections(t) {
  const g = t.generate || {};
  const stack = g.stack || {};
  const steps = stack.steps || [];
  const region = stack.region || { all: true };
  pickedStep = Math.min(pickedStep, Math.max(steps.length - 1, 0));
  const opName = op => (GEN_OPS.find(o => o[0] === op) || [op, op])[1];
  const setRegion = changes => { Object.assign(region, changes); send('editor.terrain.generate.region', changes); };
  const regionRows = [
    choice('Region', [['all', 'Whole terrain'], ['square', 'Square']], region.all ? 'all' : 'square', v => { setRegion({ all: v === 'all' }); renderTerrain(); }),
  ];
  if (!region.all) {
    regionRows.push(
      el('div', { class: 'prop' }, el('label', {}, 'Centre'), el('span', {}, `${Math.round(region.x)}, ${Math.round(region.z)}`),
        tip(actionButton('Where I look', () => send('editor.terrain.generate.region', { atView: true })), 'Centres the square where the Scene view looks at the ground')),
      valueSlider('Size (m)', () => region.size, v => setRegion({ size: v }), 16, Math.max(t.size || 1024, 64), 8, 0),
      tip(valueSlider('Falloff (m)', () => region.falloff, v => setRegion({ falloff: v }), 0, 512, 1, 0), 'How far inside its edge it blends into the ground around'));
  }
  const flat = el('input', { type: 'checkbox', class: 'check', checked: region.flat });
  flat.addEventListener('change', () => { setRegion({ flat: flat.checked }); renderTerrain(); });
  regionRows.push(tip(el('div', { class: 'prop' }, el('label', {}, 'Start flat'), flat), 'Start from flat ground instead of the current heights'));
  if (region.flat) regionRows.push(valueSlider('Flat at (m)', () => region.base, v => setRegion({ base: v }), t.heightMin ?? -200, t.heightMax ?? 800, 1, 0));
  const presetPick = el('select', { class: 'field' }, ...(g.presets || []).map(n => el('option', { value: n }, n)));
  const rows = steps.map((s, i) => {
    const on = el('input', { type: 'checkbox', class: 'check', checked: s.enabled, title: 'On / off' });
    on.addEventListener('change', () => { s.enabled = on.checked; send('editor.terrain.generate.step.set', { index: i, enabled: on.checked }); });
    return el('div', { class: 'prop preset step' + (i === pickedStep ? ' picked' : '') }, on,
      el('span', { class: 'preset-name', role: 'button', title: 'Edit this step', onclick: () => { pickedStep = i; renderTerrain(); } },
        `${i + 1}. ${s.name || opName(s.op)}`),
      el('span', { class: 'muted' }, opName(s.op)),
      i > 0 ? el('span', { class: 'tbtn flip', role: 'button', title: 'Earlier', html: icon('chev'),
        onclick: () => { pickedStep = i - 1; send('editor.terrain.generate.step.move', { index: i, to: i - 1 }); } }) : el('span'),
      el('span', { class: 'tbtn', role: 'button', title: 'Remove', html: icon('close'), onclick: () => send('editor.terrain.generate.step.remove', { index: i }) }));
  });
  const addStep = actionButton('Add Step', e => {
    e.stopPropagation();
    menuBelow(e.currentTarget, GEN_OPS.map(([op, label]) => ({ label, action: () => { pickedStep = steps.length; send('editor.terrain.generate.step.add', { op, name: label }); } })));
  });
  const run = [];
  if (g.running) {
    const fill = el('div');
    fill.style.setProperty('--pct', `${Math.round((g.progress || 0) * 100)}%`);  // (the page's CSP drops style attributes)
    run.push(el('div', { class: 'prop' }, el('label', {}, g.status || 'Generating'),
      el('div', { class: 'progress' }, fill),
      actionButton('Stop', () => send('editor.terrain.generate.cancel'), 'danger')));
  } else {
    run.push(el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Generate', () => send('editor.terrain.generate.run'), 'primary'),
      g.lastMs ? el('span', { class: 'muted' }, `last: ${(g.lastMs / 1000).toFixed(1)} s`) : null));
  }
  const list = component('Generate', 'terrain.generate', [
    el('div', { class: 'prop' }, presetPick, tip(actionButton('Use Preset', () => { pickedStep = 0; send('editor.terrain.generate.preset', { name: presetPick.value }); }),
      'Replaces the steps with the preset\'s (paint steps add the layers they need)')),
    ...regionRows,
    ...(steps.length ? rows : [el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'No steps: use a preset or add steps'))]),
    el('div', { class: 'prop' }, el('label', {}, ''), addStep),
    ...run,
  ], el('span', { html: icon('mountain') })).node;
  if (!steps.length) return [list];
  const index = pickedStep;
  const s = steps[index];
  const set = changes => { Object.assign(s, changes); send('editor.terrain.generate.step.set', { index, ...changes }); };
  const slider = (label, key, min, max, step, digits = 2, title = '') => {
    const row = valueSlider(label, () => s[key], v => set({ [key]: v }), min, max, step, digits);
    return title ? tip(row, title) : row;
  };
  const nameField = el('input', { class: 'field', value: s.name });
  nameField.addEventListener('change', () => set({ name: nameField.value.trim() }));
  const body = [el('div', { class: 'prop' }, el('label', {}, 'Name'), nameField)];
  if (s.op === 'noise') {
    body.push(
      choice('Kind', GEN_NOISE, s.noise, v => { set({ noise: v }); renderTerrain(); }),
      choice('Blend', GEN_BLEND, s.blend, v => set({ blend: v })),
      slider('Feature size (m)', 'scale', 10, 5000, 10, 0, 'Width of its hills or cells'),
      slider('Height (m)', 'height', -1000, 1000, 1, 0, 'From its lowest to its highest (negative: inverted)'),
      slider('Offset', 'offset', -1, 1, 0.01, 2, 'Moves it up or down, as a share of its height'));
    if (['fbm', 'ridged', 'billow', 'eroded'].includes(s.noise))
      body.push(slider('Octaves', 'octaves', 1, 12, 1, 0, 'Layers of finer detail'), slider('Lacunarity', 'lacunarity', 1.5, 3, 0.01), slider('Gain', 'gain', 0.2, 0.8, 0.01, 2, 'How strong each finer layer is'));
    if (['ridged', 'cells', 'crags', 'mesas'].includes(s.noise)) body.push(slider('Sharpness', 'sharpness', 0.2, 8, 0.05));
    body.push(slider('Warp (m)', 'warp', 0, 1000, 1, 0, 'Bends it with another noise: twisting, natural shapes'), slider('Warp size (m)', 'warpScale', 20, 5000, 10, 0));
  } else if (s.op === 'terrace') {
    body.push(slider('Step height (m)', 'step', 0.5, 100, 0.5, 1), slider('Sharpness', 'stepSharpness', 0, 1, 0.01, 2, '0 gentle slopes .. 1 cliffs'),
      slider('Strata', 'strata', 0, 1, 0.01, 2, 'Breaks the steps up with noise'));
  } else if (s.op === 'plateau') {
    body.push(slider('Level (m)', 'level', t.heightMin ?? -200, t.heightMax ?? 800, 1, 0), slider('Softness (m)', 'knee', 1, 200, 1, 0, 'Over how many metres above the level it flattens'));
  } else if (s.op === 'smooth') {
    body.push(slider('Radius (m)', 'radius', 0.5, 50, 0.5, 1));
  } else if (s.op === 'erode') {
    body.push(
      slider('Iterations', 'iterations', 10, 2000, 10, 0, 'How long the water runs'),
      slider('Cell size (m)', 'cellSize', 0.5, 16, 0.5, 1, 'Coarser: wider valleys, faster; finer: small gullies'),
      slider('Rain', 'rain', 0, 0.1, 0.001, 3),
      slider('Capacity', 'capacity', 0, 5, 0.05, 2, 'How much sediment the water carries'),
      slider('Erosion', 'solubility', 0, 0.5, 0.005, 3, 'How fast water picks the ground up'),
      slider('Deposition', 'deposition', 0, 1, 0.01, 2, 'How fast it drops it (low: smooth fans)'),
      slider('Evaporation', 'evaporation', 0, 0.2, 0.001, 3),
      slider('Talus angle', 'talus', 10, 80, 1, 0, 'Slopes steeper than this slump (thermal erosion)'),
      slider('Slumping', 'thermal', 0, 1, 0.01));
  } else if (s.op === 'paint') {
    const layers = t.layers || [];
    body.push(layers.length
      ? choice('Layer', layers.map((l, i) => [String(i), `${i + 1}. ${l.name}`]), String(Math.min(s.layer, layers.length - 1)), v => set({ layer: +v, look: '' }))
      : el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'Add a layer first')));
    body.push(slider('Flow from', 'flowMin', 0, 1, 0.01, 2, 'Where water ran in the last erosion: gullies and rivers'), slider('Flow to', 'flowMax', 0, 1, 0.01),
      slider('Deposit from', 'depositMin', 0, 1, 0.01, 2, 'Where the last erosion laid sediment down'), slider('Deposit to', 'depositMax', 0, 1, 0.01));
  }
  const hMin = t.heightMin ?? -200, hMax = t.heightMax ?? 800;
  body.push(
    slider('Amount', 'amount', 0, 1, 0.01, 2, 'How much of the step is taken'),
    tip(valueSlider('Height from', () => Math.max(s.heightMin, hMin), v => set({ heightMin: v <= hMin ? -1e5 : v }), hMin, hMax, 1, 0), 'Masks: only where the ground is in these ranges'),
    valueSlider('Height to', () => Math.min(s.heightMax, hMax), v => set({ heightMax: v >= hMax ? 1e5 : v }), hMin, hMax, 1, 0),
    slider('Slope from', 'slopeMin', 0, 90, 1, 0), slider('Slope to', 'slopeMax', 0, 90, 1, 0),
    slider('Curvature from', 'curvatureMin', -1, 1, 0.01, 2, 'Ridges are below 0, valleys above'), slider('Curvature to', 'curvatureMax', -1, 1, 0.01),
    slider('Mask noise', 'maskNoise', 0, 1, 0.01, 2, 'Breaks the step up into patches'), slider('Mask noise size (m)', 'maskNoiseScale', 1, 2000, 1, 0),
    slider('Seed', 'seed', 0, 999, 1, 0));
  return [list, component(`Step ${index + 1}: ${opName(s.op)}`, 'terrain.generateStep', body).node];
}

// Scatter layers: meshes of the scene spread over the terrain by rules, generated around the camera.
const SCATTER_PLACEMENT = [['2', 'Painted by hand'], ['0', 'Generated over the terrain'], ['1', 'Generated, painting clears it']];
const paintScatter = index => { setBrush({ op: 'scatter', scatterLayer: index }); sendTools({ tool: 'brush' }); renderTerrain(); };
let pickedScatter = 0, pickedScatterMesh = 0;
const SCATTER_MAX_MESHES = 16;
function scatterSections(t) {
  const sc = t.scatter || {};
  const layers = sc.layers || [];
  const meshes = (sc.meshes || []).map(m => [m, m]);
  pickedScatter = Math.min(pickedScatter, Math.max(layers.length - 1, 0));
  const meshPick = el('select', { class: 'field' }, ...meshes.map(([v, label]) => el('option', { value: v }, label)));
  const addScatter = what => {
    pickedScatter = layers.length;
    send('editor.terrain.scatter.add', what);
    if (layers.filter(l => l.paintBit >= 0).length < 7) sendTools({ tool: 'brush' });  // (the engine sets the brush to paint it)
  };
  const presetPick = el('select', { class: 'field' }, ...(sc.presets || []).map(n => el('option', { value: n }, n)));
  const rows = layers.map((l, i) => el('div', { class: 'prop preset' + (i === pickedScatter ? ' picked' : '') },
    el('span', { class: 'preset-name', role: 'button', title: 'Edit this layer', onclick: () => { pickedScatter = i; pickedScatterMesh = 0; if (l.paintBit >= 0) setBrush({ scatterLayer: i }); renderTerrain(); } },
      `${i + 1}. ${l.name}`),
    el('span', { class: 'muted' }, !l.enabled ? 'off' : (sc.grows || [])[i] === 0 ? 'no ground fits'
      : `${l.paint === 2 && l.paintBit >= 0 ? 'painted' : 'generated'}, ${l.assets.length} mesh${l.assets.length === 1 ? '' : 'es'}`),
    l.paintBit >= 0 && l.paint !== 0 ? el('span', { class: 'tbtn', role: 'button', title: 'Paint this layer onto the terrain', 'data-scatter-paint': i,
      html: icon('brush'), onclick: () => { pickedScatter = i; paintScatter(i); } }) : null));
  const status = [];
  if (sc.picked) status.push(el('div', { class: 'prop' }, el('label', {}, 'Selected'), el('span', {}, `${sc.picked.name}: ${sc.picked.mesh}`),
    actionButton('Delete', () => send('editor.terrain.scatter.exclude'), 'danger')));
  if (sc.cells) status.push(el('div', { class: 'prop' }, el('label', {}, 'Generated'), el('span', {}, `${sc.cells} cells, ${sc.slots} candidates`)));
  if (sc.starved) status.push(el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, `${sc.starved} cells have no room: raise a mesh's budget`)));
  if (sc.thinPixels > 0 || sc.thinShadow > 1) status.push(el('div', { class: 'prop hint warn', 'data-scatter-thinned': '' }, el('label', {}, ''),
    el('span', {}, 'Too much scatter for the GPU this frame: '
      + (sc.thinPixels > 0 ? `the ones smaller than ${sc.thinPixels} px on screen are left out` : 'small ones cast no shadow on coarse shadow levels')
      + '. Lower the density or draw distance of a layer to see them all.')));
  if (sc.unplaced) status.push(el('div', { class: 'prop hint' }, el('label', {}, ''),
    el('span', {}, `${sc.unplaced} mesh${sc.unplaced === 1 ? '' : 'es'} out of instance slots this session: save and restart to show them`)));
  const list = component('Scatter', 'terrain.scatter', [
    ...(layers.length ? rows : [el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, 'No scatter layers: add one to spread a mesh over the ground'))]),
    ...status,
    el('div', { class: 'prop' }, meshPick, actionButton('Add Layer', () => addScatter({ mesh: meshPick.value }))),
    ...((sc.presets || []).length ? [el('div', { class: 'prop' }, presetPick,
      tip(actionButton('Add Preset', () => addScatter({ preset: presetPick.value })),
        'Rocks of a scene cooked with --rocks, shaded with the terrain\'s rock layer'))] : []),
  ], el('span', { html: icon('mountain') })).node;
  if (!layers.length) return [list];
  const index = pickedScatter;
  const l = layers[index];
  const live = (sc.live || [])[index] || [];
  const set = changes => { Object.assign(l, changes); send('editor.terrain.scatter.set', { index, ...changes }); };
  const slider = (label, key, min, max, step, digits = 2) => valueSlider(label, () => l[key], v => set({ [key]: v }), min, max, step, digits);
  const check = (label, key, title) => {
    const box = el('input', { type: 'checkbox', class: 'check', checked: l[key] });
    box.addEventListener('change', () => set({ [key]: box.checked }));
    return tip(el('div', { class: 'prop' }, el('label', {}, label), box), title);
  };
  const nameField = el('input', { class: 'field', value: l.name });
  nameField.addEventListener('change', () => set({ name: nameField.value.trim() || l.name }));
  const setAssets = assets => { set({ assets }); renderTerrain(); };
  pickedScatterMesh = Math.min(pickedScatterMesh, Math.max(l.assets.length - 1, 0));
  const addMeshes = names => {
    const add = names.filter(n => meshes.some(([v]) => v === n)).slice(0, SCATTER_MAX_MESHES - l.assets.length)
      .map(mesh => ({ mesh, weight: 1, scaleMin: 0.8, scaleMax: 1.2, budget: 20000 }));
    if (!add.length) return;
    pickedScatterMesh = l.assets.length;
    setAssets([...l.assets, ...add]);
  };
  const shares = [];
  const meshRows = l.assets.map((a, k) => {
    const pick = el('select', { class: 'field' }, ...meshes.map(([v, label]) => el('option', { value: v, selected: v === a.mesh }, label)));
    pick.addEventListener('change', () => setAssets(l.assets.map((x, j) => j === k ? { ...x, mesh: pick.value } : x)));
    shares[k] = el('span', { class: 'share', title: 'Its share of the layer\'s instances' }, '');
    return el('div', { class: 'scatter-mesh' + (k === pickedScatterMesh ? ' picked' : '') },
      el('span', { class: 'preset-name', role: 'button', title: 'Edit its share, size and budget', onclick: () => { pickedScatterMesh = k; renderTerrain(); } }, `${k + 1}`),
      pick, shares[k],
      live[k] === false ? el('span', { class: 'muted', title: 'Out of instance slots this session: save and restart to show it' }, 'restart') : null,
      l.assets.length > 1 ? el('span', { class: 'tbtn', role: 'button', title: 'Remove from the layer', html: icon('close'),
        onclick: () => setAssets(l.assets.filter((_, j) => j !== k)) }) : null);
  });
  const showShares = () => {
    const sum = l.assets.reduce((s, a) => s + a.weight, 0);
    l.assets.forEach((a, k) => setText(shares[k], sum > 0 ? `${Math.round(a.weight / sum * 100)}%` : '0%'));
  };
  showShares();
  const a = l.assets[pickedScatterMesh];
  const value = (label, key, min, max, step, digits) =>
    valueSlider(label, () => a[key], v => { a[key] = v; showShares(); send('editor.terrain.scatter.set', { index, assets: l.assets }); }, min, max, step, digits);
  const addPick = el('select', { class: 'field' }, ...meshes.map(([v, label]) => el('option', { value: v }, label)));
  const full = l.assets.length >= SCATTER_MAX_MESHES;
  const meshBox = el('div', { class: 'scatter-meshes', 'data-scatter-drop': '' },
    el('div', { class: 'scatter-meshes-head' }, el('span', {}, 'Meshes'), el('span', { class: 'muted' }, `${l.assets.length} of ${SCATTER_MAX_MESHES}`)),
    ...meshRows,
    full ? el('div', { class: 'scatter-drop-hint' }, 'A layer takes 16 meshes at most')
      : el('div', { class: 'scatter-mesh' }, addPick, actionButton('Add Mesh', () => addMeshes([addPick.value]))),
    full ? null : el('div', { class: 'scatter-drop-hint' }, 'Or drop meshes or a model here from the Project panel'));
  meshBox.addEventListener('scatter-drop', e => addMeshes(e.detail.meshes));
  const assetRows = a ? [
    meshBox,
    el('div', { class: 'prop sub-head' }, el('label', {}, `Mesh ${pickedScatterMesh + 1}`), el('span', { class: 'muted' }, a.mesh)),
    tip(value('Share', 'weight', 0, 10, 0.05, 2), 'How often this mesh is picked against the layer\'s others'),
    value('Scale from', 'scaleMin', 0.05, 10, 0.01, 2),
    value('Scale to', 'scaleMax', 0.05, 10, 0.01, 2),
    tip(valueSlider('Budget', () => a.budget, v => setAssets(l.assets.map((x, j) => j === pickedScatterMesh ? { ...x, budget: Math.round(v) } : x)), 256, 400000, 256, 0),
      'Instances of this mesh that can exist at once around the camera'),
  ] : [meshBox];
  // Randomness: the seed (the dice: another one), how far from an even grid, how far turned.
  const seedRow = tip(slider('Seed', 'seed', 1, 999, 1, 0), 'Another arrangement of the same rules');
  seedRow.lastElementChild.classList.add('with-btn');
  seedRow.lastElementChild.append(el('span', { class: 'tbtn', role: 'button', title: 'A new random seed', html: icon('dice'),
    onclick: () => { set({ seed: 1 + Math.floor(Math.random() * 999) }); renderTerrain(); } }));
  const randomness = [
    el('div', { class: 'prop sub-head' }, el('label', {}, 'Randomness'), el('span')),
    seedRow,
    tip(slider('Spacing jitter', 'jitter', 0, 1, 0.01), '0: on an even grid, 1: anywhere in its square (natural)'),
    tip(slider('Random turn (°)', 'yawRange', 0, 360, 1, 0), 'How far each one is turned at random around its up axis (0: all face the same way)'),
    tip(slider('Random lean', 'tilt', 0, 45, 0.5, 1), 'Degrees'),
    tip(slider('Shade variation', 'tintVariation', 0, 1, 0.01), 'Each one a little darker at random'),
  ];
  const terrainLayers = [['-1', 'Any'], ...(t.layers || []).map((tl, i) => [String(i), `${i + 1}. ${tl.name}`])];
  const materials = [0, 1, 2, 3].map(k => choice(k === 0 ? 'On ground layer' : 'or', terrainLayers, String(l.materialLayers[k] ?? -1),
    v => { const m = [...l.materialLayers]; m[k] = +v; set({ materialLayers: m }); }));
  const hMin = t.heightMin ?? -200, hMax = t.heightMax ?? 800;
  const editor = component(`Scatter ${index + 1}`, 'terrain.scatterLayer', [
    el('div', { class: 'prop' }, el('label', {}, 'Name'), nameField),
    check('Enabled', 'enabled', 'Off: none of it shows'),
    l.paintBit >= 0 ? tip(choice('Placement', SCATTER_PLACEMENT, String(l.paint), v => { set({ paint: +v }); renderTerrain(); }),
      'Painted by hand: they grow only where you paint them. Generated: they cover the terrain wherever the rules below allow')
      : el('div', { class: 'prop hint' }, el('label', {}, 'Placement'), el('span', {}, 'Generated over the terrain (7 layers at most can be painted)')),
    l.paintBit >= 0 && l.paint !== 0 ? el('div', { class: 'prop' }, el('label', {}, ''),
      tip(actionButton(l.paint === 2 ? 'Paint on the Terrain' : 'Paint to Clear', () => paintScatter(index), 'primary'),
        'The brush paints this layer (Shift erases); the rules below still decide where they can grow')) : null,
    (sc.grows || [])[index] === 0 ? el('div', { class: 'prop hint warn', 'data-scatter-nowhere': '' }, el('label', {}, ''),
      el('span', {}, `Nothing grows: no ground on this terrain has a slope of ${fmt(l.slopeMin, 0)}–${fmt(l.slopeMax, 0)}° `
        + 'within its height range. Widen Slope from / to or Height from / to under Where they grow.')) : null,
    ...assetRows,
    tip(valueSlider('Density', () => l.density, v => set({ density: v }), 0.001, 4, 0.001, 3), 'Instances per square metre (before the rules below thin them out)'),
    tip(slider('Draw distance', 'drawDistance', 20, 2000, 10, 0), 'Metres from the camera they are generated to'),
    ...randomness,
    el('div', { class: 'prop sub-head' }, el('label', {}, 'Where they grow'), el('span')),
    ...materials,
    tip(slider('Ground share', 'materialMin', 0, 1, 0.01), 'How much of those ground layers there must be'),
    tip(slider('Slope from', 'slopeMin', 0, 90, 1, 0), 'Degrees'),
    slider('Slope to', 'slopeMax', 0, 90, 1, 0),
    valueSlider('Height from', () => Math.max(l.heightMin, hMin), v => set({ heightMin: v <= hMin ? -1e5 : v }), hMin, hMax, 0.5, 1),
    valueSlider('Height to', () => Math.min(l.heightMax, hMax), v => set({ heightMax: v >= hMax ? 1e5 : v }), hMin, hMax, 0.5, 1),
    tip(slider('Clumping', 'clumpAmount', 0, 1, 0.01), 'Gathers them into patches with gaps between'),
    slider('Clump size (m)', 'clumpScale', 1, 500, 1, 0),
    tip(slider('Clump threshold', 'clumpThreshold', 0, 1, 0.01), 'More: smaller, rarer patches'),
    el('div', { class: 'prop sub-head' }, el('label', {}, 'How they stand'), el('span')),
    tip(slider('Follow slope', 'align', 0, 1, 0.01), '0 stands upright, 1 leans with the ground'),
    tip(slider('Sink', 'sink', -2, 2, 0.01), 'Into the ground, metres per unit of scale (rocks look buried)'),
    check('Cast shadows', 'castShadows', 'Off: they cast no shadows (cheaper for many small meshes, like grass and pebbles)'),
    check('Collision', 'collision', 'Bodies collide with the ones near the camera'),
    tip(choice('Shade with', [['-1', 'Its own material'], ...(t.layers || []).map((tl, i) => [String(i), `${i + 1}. ${tl.name}`])],
      String(l.terrainLook ? (t.layers || []).findIndex(tl => tl.generated === l.terrainLook) : l.terrainLayer),
      v => { set({ terrainLayer: +v, terrainLook: '' }); renderTerrain(); }),
      'A terrain layer instead of the mesh\'s material (rocks that match the cliffs around them)'),
    (l.terrainLayer >= 0 || l.terrainLook) ? tip(slider('Ground at base (m)', 'baseBlend', 0, 5, 0.05, 2),
      'The ground\'s own material blends onto the parts up to this high (rocks sit in the ground)') : null,
    el('div', { class: 'prop' }, el('label', {}, 'Deleted'), el('span', {}, String((sc.deleted || [])[index] || 0)),
      (sc.deleted || [])[index] ? actionButton('Restore', () => send('editor.terrain.scatter.restore', { index })) : null),
    el('div', { class: 'prop' }, el('label', {}, ''), actionButton('Remove Layer', () => send('editor.terrain.scatter.remove', { index }), 'danger')),
  ].filter(Boolean)).node;
  return [list, editor];
}

// ---------------------------------------------------------------- console

function addLog(level, message) {
  ui.logs.push({ level, message, time: new Date() });
  if (ui.logs.length > 2000) ui.logs.splice(0, ui.logs.length - 2000);
  setStatus(message, level);
  scheduleConsole();
}
let consoleQueued = false;
function scheduleConsole() {
  if (consoleQueued) return;
  consoleQueued = true;
  requestAnimationFrame(() => { consoleQueued = false; renderConsole(); });
}
function renderConsole() {
  const counts = { info: 0, warning: 0, error: 0 };
  for (const l of ui.logs) counts[l.level]++;
  for (const level of ['info', 'warning', 'error']) {
    const b = $('#flt-' + level);
    setText(b.lastElementChild, String(counts[level]));
    b.classList.toggle('on', ui.logFilter[level]);
  }
  $('#btn-collapse').classList.toggle('on', ui.logCollapse);
  if ($('[data-body="console"]').hidden) return;
  const q = ui.logSearch.toLowerCase();
  let list = ui.logs.filter(l => ui.logFilter[l.level] && (!q || l.message.toLowerCase().includes(q)));
  if (ui.logCollapse) {
    const map = new Map();
    for (const l of list) {
      const key = l.level + '\u0000' + l.message;
      if (map.has(key)) map.get(key).count++; else map.set(key, { ...l, count: 1 });
    }
    list = [...map.values()];
  }
  const box = $('#log-list');
  const atBottom = box.scrollTop + box.clientHeight >= box.scrollHeight - 4;
  box.replaceChildren(...list.slice(-500).map((l, i) => {
    const row = el('div', { class: `log ${l.level}` + (i === ui.selectedLog ? ' selected' : '') },
      el('span', { html: icon(l.level) }), el('span', { class: 'text' }, `[${l.time.toLocaleTimeString([], { hour12: false })}] ${l.message}`),
      ui.logCollapse ? el('span', { class: 'count' }, String(l.count)) : el('span'));
    row.addEventListener('mousedown', () => { ui.selectedLog = i; setText($('#log-detail'), l.message); $$('#log-list .log').forEach((r, k) => r.classList.toggle('selected', k === i)); });
    return row;
  }));
  if (atBottom) box.scrollTop = box.scrollHeight;
}
$('#btn-clear').addEventListener('click', () => { ui.logs = []; ui.selectedLog = -1; setText($('#log-detail'), ''); renderConsole(); });
$('#btn-collapse').addEventListener('click', () => { ui.logCollapse = !ui.logCollapse; store.set('collapse', ui.logCollapse); renderConsole(); });
for (const level of ['info', 'warning', 'error']) {
  $('#flt-' + level).addEventListener('click', () => { ui.logFilter[level] = !ui.logFilter[level]; renderConsole(); });
}
$('#log-search').addEventListener('input', e => { ui.logSearch = e.target.value; renderConsole(); });
$$('[data-tab="console"]').forEach(t => t.addEventListener('click', renderConsole));

// ---------------------------------------------------------------- profiler and stats

function renderStatsOverlay() {
  const s = ui.lastStats;
  const box = $('#stats-overlay');
  if (box.hidden || !s) return;
  const row = (k, v) => el('div', {}, el('span', { class: 'muted' }, k), el('span', {}, v));
  box.replaceChildren(el('b', {}, 'Statistics'),
    row('FPS', `${(s.frameMs > 0 ? 1000 / s.frameMs : 0).toFixed(0)} (${s.frameMs.toFixed(2)} ms)`),
    row('Tris', `${((s.triangles || 0) / 1e6).toFixed(2)} M`), row('Clusters', String(s.clusters)),
    row('Instances', String(s.objects)), row('Shadow pages', String(s.vsmPages)), row('Bodies', String(s.bodies)));
}

// ---------------------------------------------------------------- status bar and top bar

function setStatus(text, level = 'info') {
  const msg = $('#status-msg');
  msg.className = 'msg ' + level;
  msg.innerHTML = icon(level);
  msg.append(text);
}

function renderTopbar() {
  setText($('#scene-title'), state.projectInfo ? `${state.projectInfo.name} — ${sceneLabel(state.projectInfo.current)}` : state.scene || '');
  $('#dirty').hidden = !state.dirty;
  $('#btn-play').classList.toggle('on', state.playing);
  $('#btn-pause').classList.toggle('on', state.paused);
  setText($('#mode-pill'), state.playing ? (state.paused ? 'Paused' : 'Playing') : 'Editing');
  document.body.classList.toggle('playing', state.playing);
  // The project's scripts: shown while they build or load, and when they failed (a click: the Console).
  const s = state.scripts || {};
  const pill = $('#scripts-pill');
  const text = s.state === 'building' ? (s.playPending ? 'Building scripts, then Play…' : 'Building scripts…')
    : s.state === 'loading' ? 'Loading scripts…' : s.state === 'failed' ? 'Scripts failed' : '';
  pill.hidden = !text;
  setText(pill, text);
  pill.classList.toggle('error', s.state === 'failed');
  pill.title = s.state === 'failed' ? `${s.error || 'The build failed'} (click: the Console)` : '';
  $('#btn-play').classList.toggle('pending', !!s.playPending);
  // Play with clients (a project's): the editor's game hosts, client processes of the scene join.
  const st = state.settings || {};
  for (const [id, key] of [['#play-clients', 'netPlayClients'], ['#play-conditions', 'netPlayConditions']]) {
    const sel = $(id);
    sel.hidden = !st.projectSettings || (id === '#play-conditions' && !st.netPlayClients);
    if (document.activeElement !== sel) sel.value = String(st[key] || 0);
    sel.disabled = state.playing;
  }
}
$('#play-clients').addEventListener('change', e => { setSetting('netPlayClients', +e.target.value); renderTopbar(); });
$('#play-conditions').addEventListener('change', e => setSetting('netPlayConditions', +e.target.value));
$('#scripts-pill').addEventListener('click', () => showTab('bottom', 'console'));

// ---------------------------------------------------------------- engine messages

function applyPagePrefs(prefs) {
  pagePrefs = prefs;
  prefsLoaded = true;
  Object.assign(sizes, { left: 260, right: 330, bottom: 230 }, store.get('sizes', {}));
  applySizes();
  ui.maximizeOnPlay = store.get('maxplay', false);
  ui.showStats = store.get('stats', false);
  ui.logCollapse = store.get('collapse', false);
  collapsed = new Set(store.get('collapsed', []));
  restoreTabs();
  renderTools();
  renderInspector();
  renderLighting();
  renderTerrain();
  renderConsole();
  Assets.applyPrefs();
}

function onState(p) {
  const hadEntities = 'entities' in p;
  Object.assign(state, p);
  if (hadEntities) {
    entityById.clear();
    for (const e of state.entities) entityById.set(e.id, e);
    rebuildRows();
    Assets.render();  // the project's name
  } else {
    renderTree();
  }
  revealSelected();
  renderTopbar();
  renderTools();
  renderInspector();
  renderLighting();
  renderTerrain();
  applyPlayLayout();
}

// window.falcon is provided (read-only) by cef-lite; a plain browser gets a stand-in.
if (!window.falcon) window.falcon = {};
// cef-lite calls onMessage straight from its renderer-side IPC handler, where exceptions vanish
// silently. Messages are queued and handled in order from a normal task instead (errors are
// reported as ui.error).
const inbox = [];
let inboxQueued = false;
window.falcon.onMessage = message => {
  inbox.push(message);
  if (!inboxQueued) {
    inboxQueued = true;
    setTimeout(drainInbox, 0);
  }
};
function drainInbox() {
  inboxQueued = false;
  // Unpack batches, then merge runs of editor.state into one (each is a partial update applied
  // in order), so a burst of edits rebuilds the panels once.
  const messages = [];
  for (const text of inbox.splice(0)) {
    let p;
    try { p = JSON.parse(text); } catch { addLog('error', 'Bad message from the engine'); continue; }
    for (const m of p.type === 'ui.batch' ? p.messages : [p]) {
      const last = messages[messages.length - 1];
      if (m.type === 'editor.state' && last && last.type === 'editor.state') Object.assign(last, m);
      else messages.push(m);
    }
  }
  for (const m of messages) {
    try {
      handleEngineMessage(m);
    } catch (err) {
      send('ui.error', { message: String(err && err.message), source: String(err && err.stack) });
    }
  }
}

function handleEngineMessage(p) {
  switch (p.type) {
    case 'editor.state': onState(p); break;
    case 'engine.connected':
      setText($('#status-backend'), `SDL_GPU · ${p.backend}`);
      addLog('info', `Engine connected (${p.backend}).`);
      break;
    case 'editor.notice': addLog(p.level === 'error' ? 'error' : 'info', p.message); break;
    case 'editor.tools':  // editor code's menu items and windows (the Tools menu), after each build is loaded
      // (not state.tools: that's the tools' state, element mode and levelSelected among it)
      window.codeTools = p.tools;  // (commands: the level toolbar, wheels, palette, keymap; leveltools.js may load later)
      if (window.levelTools) window.levelTools.setCode(p.tools);
      for (const menu of [...editorWindows.keys()]) {  // (open windows: their C# side made again on the new code)
        if (((p.tools && p.tools.windows) || []).some(w => w.menu === menu)) send('editor.window.open', { window: menu });
        else closeEditorWindow(menu);
      }
      break;
    case 'editor.tools.state':  // editor code's commands' settings as they are now (the Level window's)
      window.codeToolState = p.state;
      if (window.levelTools) window.levelTools.setState(p.state);
      break;
    case 'editor.view.mouse': if (window.levelTools) window.levelTools.onMouse(p); break;  // (a directional wheel opening there)
    case 'editor.level.uvs': case 'editor.level.uvEditor': case 'editor.level.picked':  // (the UV editor, the material palette, trims)
    case 'editor.level.trimPanel': case 'editor.trim.list': case 'editor.level.smartList':
      if (window.levelTools) window.levelTools.onLevelMessage(p);
      break;
    case 'editor.drawer.post':  // (tests) a message to a component's Inspector page
      Drawers.postTo(p.class, p.data);
      break;
    case 'editor.window.send': {  // an editor window's C# side to its page
      const open = editorWindows.get(p.window);
      if (open && open.frame.contentWindow) open.frame.contentWindow.postMessage({ falcon: 'panel', data: JSON.parse(p.data) }, '*');
      break;
    }
    case 'editor.gameui': {  // the project's game UI (UI/Game/index.html) over the game's view while playing
      const host = document.getElementById('game-hud').parentElement;  // (the scene and game view, where the HUD is)
      let frame = document.getElementById('game-ui');
      if (!p.url) { if (frame) frame.remove(); break; }
      if (!frame) {
        frame = el('iframe', { id: 'game-ui', class: 'game-ui', title: 'Game UI' });
        host.append(frame);
      }
      frame.src = p.url;
      break;
    }
    case 'editor.pages.changed':  // live reload: files of the project's pages changed (its UI/, its libraries' UI/)
      // Every frame showing a project page loads it again (the game's UI, editor windows,
      // Inspector pages): setting src navigates even to the same address.
      for (const frame of document.querySelectorAll('iframe')) {
        const src = frame.getAttribute('src');
        if (src && src.startsWith('file:')) frame.setAttribute('src', src);
      }
      break;
    case 'editor.gameui.send': {  // the game's Ui.Send to its page
      const frame = document.getElementById('game-ui');
      if (frame && frame.contentWindow) frame.contentWindow.postMessage({ falcon: 'ui', data: p.data }, '*');
      break;
    }
    case 'editor.window.show': {  // editor code (or a test) opening a window
      const w = ((window.codeTools && window.codeTools.windows) || []).find(x => x.menu === p.window);
      if (w) openEditorWindow(w);
      break;
    }
    case 'editor.libraries':  // the Library Manager's list (shown again when it is open)
      state.libraries = p;
      if (librariesNode && librariesNode.isConnected) { closeShortcuts(); librariesDialog(); }
      break;
    case 'editor.hud': {  // the game's text over the view (Hud.Show)
      const hud = $('#game-hud');
      hud.textContent = p.text || '';
      hud.hidden = !p.text;
      hud.classList.toggle('beside-tools', !state.tools.gameView);
      break;
    }
    case 'editor.log': for (const e of p.entries) addLog(e.level, e.message); break;
    case 'engine.render': break;  // the profiler / stats overlay use engine.stats
    case 'editor.ping':  // tests: the page is alive and painting
      setStatus(`ping ${p.n}`);
      send('editor.pong', { n: p.n });
      break;
    case 'editor.probe': {  // tests: element rects (to aim test.mouse) and interaction state
      const rects = {};
      for (const sel of p.rects || []) {
        const n = document.querySelector(sel);
        // (the Level window's rows scrolled to as a user would; elsewhere when the step asks: "reveal")
        if (n && (p.reveal || n.closest('.lw-body'))) n.scrollIntoView({ block: 'nearest' });
        if (n) { const r = n.getBoundingClientRect(); rects[sel] = [r.left, r.top, r.width, r.height]; }
      }
      const a = document.activeElement;
      const field = a && a.tagName === 'INPUT' && a.type !== 'range' && a.type !== 'checkbox';
      send('editor.probed', {
        rects,
        menuOpen: !!openMenuState,
        menuItems: openMenuState ? openMenuState.node.querySelectorAll('.menu-item').length : 0,
        submenuItems: openMenuState && openMenuState.subs.length ? openMenuState.subs[openMenuState.subs.length - 1].querySelectorAll('.menu-item').length : 0,
        // Add Component's open list: the engine's components it shows (filtered by its search), in
        // order. (The game's, Script: and Data:, come from whatever game the project has.)
        addComponents: openMenuState && openMenuState.node.classList.contains('add-component-menu')
          ? [...openMenuState.node.querySelectorAll('.menu-item[data-component]')].filter(n => !n.hidden && !/^(Script|Data):/.test(n.dataset.component))
            .map(n => n.dataset.component).join(',') : '',
        hierarchyScroll: tree.scrollTop,
        // The open dialog: on screen whole (its buttons too), and its body scrolls when too long.
        dialogFits: (() => { const m = $('.modal.dialog'); if (!m) return false; const r = m.getBoundingClientRect();
          return r.top >= 0 && r.bottom <= window.innerHeight + 0.5; })(),
        dialogScrolls: (() => { const b = $('.modal.dialog .dialog-body'); return !!b && b.scrollHeight > b.clientHeight + 1; })(),
        focus: a && a !== document.body ? a.tagName.toLowerCase() + (a.type ? ':' + a.type : '') : '',
        selection: field ? a.value.slice(a.selectionStart, a.selectionEnd) : String(window.getSelection()),
        value: field ? a.value : '',
        selectionAll: !!field && a.value !== '' && a.selectionStart === 0 && a.selectionEnd === a.value.length,
        giIntensity: state.settings ? state.settings.giIntensity : 0,
        brushPill: !$('#brush-pill').hidden,
        brushPillText: $('#brush-pill').hidden ? '' : $('#brush-pill').textContent,
        brushOn: !!$('[data-brush-toggle].on'),
        terrainTab: !$('[data-body="terrain"]').hidden,
        scatterMeshes: $$('[data-scatter-drop] .scatter-mesh .preset-name').length,
        scatterNowhere: !!$('[data-scatter-nowhere]'),
        // Meshes drawn clipped (not closed: cut without caps): the Hierarchy's badges, the Inspector's note.
        clipBadges: tree.querySelectorAll('.clip-badge').length,
        clippedNote: !!$('[data-clipped]'),
        // The Inspector's Surface section: its materials and what each is used for; the paint panel's.
        surfaceLayers: $$('#inspector [data-layer-slot] .as-tex-name').map(n => n.textContent).join(','),
        surfaceUses: $$('#inspector [data-surface-use]').map(n => n.value).join(','),
        paintMaterials: $$('#paint-panel [data-surface-layer]').map(n => n.querySelector('.as-tex-name').textContent +
          ':' + n.querySelector('[data-surface-use]').value + (n.classList.contains('on') ? '*' : '')).join(','),
        // A level object's sections: the first two, and whether they come before the rest (after the
        // transform); its material pickers (a wall's sides, a roof's, a cut's cap).
        inspectorLevel: (() => {
          const LEVEL = ['wall', 'roof', 'opening', 'cut', 'volume', 'snapPoint', 'spline', 'surface', 'modifiers', 'wear'];
          const keys = $$('#inspector section.component').map(n => n.dataset.component).filter(k => k !== 'transform');
          return keys.filter(k => LEVEL.includes(k)).slice(0, 2).join(',') + (keys.length && !LEVEL.includes(keys[0]) ? ' (not first)' : '');
        })(),
        inspectorMaterialRows: $$('#inspector [data-material-row]').length,
        // The Inspector's Audio Source: the sound in its slot; a .fsound's: its clips.
        audioSourceSound: $('[data-sound-slot] .as-tex-name') ? $('[data-sound-slot] .as-tex-name').textContent : '',
        soundClips: $('[data-sound-clip-list]') ? $$('[data-sound-clip-list] .as-tex-slot').length - 1 : -1,
        ...Profiler.probe(),
        ...NetworkPanel.probe(),
        ...Assets.probe(),
        ...Animator.probe(),
        ...(window.levelTools && window.levelTools.probe ? window.levelTools.probe() : {}),
      });
      break;
    }
    case 'editor.assets': Assets.onList(p); break;
    case 'editor.assets.done': Assets.onDone(p); break;
    case 'editor.assets.image': Assets.onImage(p); if (window.levelTools) window.levelTools.onLevelMessage(p); break;
    case 'editor.assets.preview': Assets.onPreview(p); break;
    case 'editor.assets.material': Assets.onMaterial(p); break;
    case 'editor.assets.sound': Assets.onSound(p); break;
    case 'editor.assets.clip': Assets.onClip(p); break;
    case 'editor.assets.used': Assets.onUsed(p); break;
    case 'editor.cook': Assets.onCook(p); break;
    case 'editor.audioBake': state.audioBake = p; renderAudio(); break;
    case 'editor.anim.show': Animator.open(p.path); break;
    case 'editor.anim.do': Animator.run(p); break;
    case 'editor.anim.graph': Animator.onGraph(p); break;
    case 'editor.anim.events': Animator.onEvents(p); break;
    case 'editor.anim.preview': Animator.onPreview(p); break;
    case 'editor.anim.state': Animator.onState(p); break;
    case 'profiler.frames': Profiler.onFrames(p); break;
    case 'net.status': NetworkPanel.onStatus(p); break;
    case 'profiler.frameDetail': Profiler.onDetail(p); break;
    case 'profiler.memoryDetail': Profiler.onMemory(p); break;
    case 'editor.prefs': applyPagePrefs(p.page || {}); break;
    case 'editor.prefs.changed':  // (no saved prefs, as tests run: one a script set)
      // (the page's own set heard back: nothing to do, and re-rendering the level tools then would
      // lose the click that set it, the profiler's tabs')
      if (JSON.stringify(pagePrefs[p.key]) === JSON.stringify(p.value)) break;
      pagePrefs[p.key] = p.value;
      if (window.levelTools) window.levelTools.render();
      break;
    case 'ui.window': setMaximized(!!p.maximized); break;
    case 'engine.stats':
      ui.lastStats = p;
      renderStatsOverlay();
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- start

Profiler.init({ $, el, send, store, isTextField, toggleMaximize });
NetworkPanel.init({ el, send });
Animator.init({ $, el, send, icon, fmt, numberRow, vec3Row, reportLayout, showTab, pressedIn, choice, actionButton, setStatus });
Particles.init({ el, send, tip, icon, numberInput, numberRow, scrub, fmt, setRange, colorField, vec3Row, choice, actionButton, component,
  componentMore, toSrgb, openPopover, closePopover: closeColorPop, popoverFor });
Assets.init({
  $, el, send, store, cmd, dialog, openMenu, menuBelow, setStatus, actionButton, renderInspector, valueSlider, choice, colorField, showTab, tip,
  acousticRows,
  getState: () => state,
});
Assets.shown(!$('[data-body="project"]').hidden);
applyIcons();
restoreTabs();
renderTools();
renderInspector();
renderLighting();
renderConsole();
setStatus('Starting…');
reportLayout();
reportChrome();
send('ui.window', { action: 'query' });
send('editor.ready', { surface: 'editor-shell', version: 3 });

// What other page scripts (leveltools.js) use: this file's top level is a block (the try above),
// so its names are not global. Getters for what changes.
window.editorPage = {
  send, el, icon, $, $$, state, viewport, cmd, reportLayout, capturePointer, sendTools, setSetting, isTextField, keyName,
  DRAW_MODES, closeShortcuts, openMenu, surfaceList,
  get pagePrefs() { return pagePrefs; },
  get prefsLoaded() { return prefsLoaded; },
  get openMenuState() { return openMenuState; },
};

} catch (err) {
  if (window.falcon && window.falcon.postMessage) {
    window.falcon.postMessage(JSON.stringify({ type: 'ui.error', message: String(err && err.message), source: String(err && err.stack) }));
  }
  throw err;
}
