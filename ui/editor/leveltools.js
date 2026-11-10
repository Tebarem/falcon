'use strict';
// The level tools' controls in the editor page:
//   - the Level window floating over the scene view: the level commands in sections (Create, Edit,
//     Select, Surface, Object, World, Place) for the element mode, with search, settings and icon mode
//   - hover cards over the scene view's buttons: a command's name, key and help (titles read the same way)
//   - directional wheels: hold Tab (element modes), Shift+S (snapping), Alt+Q (the mode's
//     operations: Wheel = "ops"), Alt+Z (draw modes) or Shift+A (create: Wheel = "create"), flick
//     toward an item, release; a click picks too
//   - the command palette (F3 or Ctrl+Space): every command and menu item, typed to filter
//   - the status strip along the foot of the view: the mode, what is chosen and the keys that matter now
//     (a modal operation's name, amount and keys while it runs); the window's footer: the running tool's
//     settings, the paint brush, or the last operation (changed: done again)
//   - Edit > Keymap…: the commands' keys, rebound by the user (prefs "keymap": {path: key})
//   - chord menus: Shift+1–5 a numbered ring, then 1–6 while Shift is held (Edit > Chord Menus…: the
//     user's own rings, prefs "chords")
//   - the material palette in the window (level meshes: Alt+1..9, drops from the Project panel, the eyedropper)
//   - the prefab shelf in the window (Ctrl+Shift+1..9 at the mouse, a click at the view's centre, Ctrl+click
//     replaces the selection; drops from the Project panel)
//   - the UV editor (UV/Editor, Alt+U): the chosen faces' UVs, dragged, scaled, turned, fitted
//   - trim sheets (UV/Trim Sheets, Alt+T): a sheet's strips over its texture, a click puts faces on one
//   - the paint panel (the paint tool, Shift+B): the mesh's materials (each with its use), erase,
//     vertex colours, radius, strength
// It uses editor.js's helpers through window.editorPage (editor.js's top level is a block: its names are
// not global) and is window.levelTools to it (editor.js may handle messages before this file has run).
// CEF 151 rule (editor.js): no <button> made at runtime (span role="button"); the CSP drops style
// attributes (positions set through CSSOM).
window.levelTools = (() => { try {
  const P = window.editorPage;
  const { send, el, icon, $, state, viewport, cmd, reportLayout, capturePointer, sendTools, setSetting, isTextField, keyName, DRAW_MODES,
    closeShortcuts, openMenu, surfaceList } = P;
  let code = window.codeTools || { menus: [] };  // (described before this file ran)
  const commands = () => code.menus || [];
  const mode = () => (state.tools && state.tools.element) || 'object';
  const levelSelected = () => !!(state.tools && state.tools.levelSelected);
  const label = c => c.path.split('/').pop();
  const keymap = () => (P.pagePrefs && P.pagePrefs.keymap) || {};
  const keyOf = c => (c.path in keymap() ? keymap()[c.path] : c.key) || '';
  const inMode = (c, m) => !c.modes || c.modes.split('|').includes(m);
  const run = path => send('editor.tools.run', { menu: path });
  let chord = null;  // the open chord menu: {ring, stack: [{label, slots}], x, y, node, why}
  let chordNote = '', chordNoteTimer = 0;  // a word on the status strip (Shift+2 with no level mesh)
  // The Level window's sections (editor code's commands without one: "Tools").
  const GROUPS = [['create', 'Create'], ['edit', 'Edit'], ['select', 'Select'], ['surface', 'Surface'], ['object', 'Object'],
    ['world', 'World'], ['view', 'View'], ['starter', 'Starter content']];
  const groupIndex = c => { const k = GROUPS.findIndex(g => g[0] === c.group); return k < 0 ? GROUPS.length : k; };
  const groupTitle = c => (GROUPS.find(g => g[0] === c.group) || [0, 'Tools'])[1];
  const sortedCommands = () => commands().map((c, k) => ({ c, k })).sort((a, b) => groupIndex(a.c) - groupIndex(b.c) || a.k - b.k).map(x => x.c);
  // The commands' settings as they are now (editor code's: editor.tools.state).
  let codeState = window.codeToolState || {};
  const optionValue = (c, name) => ((codeState.options || {})[c.path] || {})[name];
  // Typed text against a name: every letter in order, earlier and closer is better (-1: no match).
  const score = (text, q) => {
    let pos = -1, s = 0;
    for (const ch of q) {
      const next = text.indexOf(ch, pos + 1);
      if (next < 0) return -1;
      s += next - pos;
      pos = next;
    }
    return s;
  };

  // ---- the Level window: the level commands in sections, floating over the view. Dragged by its title
  //   bar, its height by the grip at its foot; its place, height, folded sections and icon mode are kept
  //   (prefs "levelWindow": {open, x, y, height, folded: {key: true}, more: {key: true}, icons, opened}).
  //   Window > Level Tools (Ctrl+Shift+L) shows and hides it; it opens by itself the first time a level
  //   mesh is selected. A row that can't run now is greyed (its hover card says why); ⚙ opens a
  //   command's settings under its row. The material palette and the prefab shelf live in it.
  const strip = el('div', { class: 'overlay status-strip', id: 'status-strip', hidden: true });
  const win = el('div', { class: 'overlay level-window', id: 'level-window', hidden: true });
  const winSearch = el('input', { class: 'field lw-search', placeholder: 'Search commands', spellcheck: false });
  const winBody = el('div', { class: 'lw-body' });
  const winIcons = el('span', { class: 'tbtn', role: 'button', 'data-window-icons': '', 'data-tip': 'Icons only — the window as a narrow rail of icons', html: icon('iconRail') });
  const winClose = el('span', { class: 'tbtn', role: 'button', 'data-tip': 'Close — Window > Level Tools (Ctrl+Shift+L) opens it again', html: icon('close') });
  const winHead = el('div', { class: 'lw-head' }, el('span', { class: 'lw-title', html: icon('levelTools') }, 'Level'), el('span', { class: 'spacer' }), winIcons, winClose);
  const winFoot = el('div', { class: 'lw-foot', hidden: true });
  const winGrip = el('div', { class: 'lw-grip' });
  win.append(winHead, winSearch, winBody, winFoot, winGrip);
  viewport.append(win, strip);
  // (a field being typed in keeps its text: the window is drawn again once it lets go)
  win.addEventListener('focusout', () => setTimeout(render, 0));
  const winPrefs = () => (P.pagePrefs && P.pagePrefs.levelWindow) || {};
  function saveWindow(changes) {
    const value = { ...winPrefs(), ...changes };
    if (P.pagePrefs) P.pagePrefs.levelWindow = value;
    send('editor.prefs.set', { key: 'levelWindow', value });
    winKey = '';
    render();
  }
  const windowOpen = () => !!winPrefs().open;
  const toggleWindow = open => saveWindow({ open: open === undefined ? !windowOpen() : !!open, opened: true });
  winClose.addEventListener('click', () => toggleWindow(false));
  winIcons.addEventListener('click', () => saveWindow({ icons: !winPrefs().icons }));
  // Kept inside the view: its place from the prefs (left of the view, beside the tools, at first).
  function placeWindow() {
    if (win.hidden) return;
    const v = viewport.getBoundingClientRect(), p = winPrefs();
    const x = Math.max(4, Math.min(p.x ?? 48, v.width - win.offsetWidth - 4));
    const y = Math.max(4, Math.min(p.y ?? 8, v.height - 120));
    win.style.left = `${x}px`;  // (CSSOM: the CSP drops style attributes)
    win.style.top = `${y}px`;
    const room = Math.max(120, v.height - y - 8);
    win.style.maxHeight = `${room}px`;
    win.style.height = p.height ? `${Math.min(p.height, room)}px` : '';
    placePanels();
  }
  // The UV and trim panels beside the window, level with its top (left of it when there is no room on
  // its right); without it, beside the tools.
  function placePanels() {
    const room = viewport.clientWidth;
    for (const panel of [uvPanel, trimPanel]) {
      if (!panel || panel.hidden) continue;
      let left = 48, top = 8;
      if (!win.hidden) {
        left = win.offsetLeft + win.offsetWidth + 6;
        if (left + panel.offsetWidth > room - 4) left = Math.max(4, win.offsetLeft - panel.offsetWidth - 6);
        top = win.offsetTop;
      }
      panel.style.left = `${left}px`;  // (CSSOM: the CSP drops style attributes)
      panel.style.top = `${top}px`;
    }
    clearOfWindow();
  }
  // The mode bar and the status strip are centred on the view's free part, beside the window rather
  // than under it (on the wider side of it); without it, on the view.
  function clearOfWindow() {
    const room = viewport.clientWidth;
    let from = 0, to = room;
    if (!win.hidden) {
      const l = win.offsetLeft, r = l + win.offsetWidth;
      if (room - r >= l) from = r; else to = l;
    }
    for (const bar of [$('#mode-bar'), strip]) {
      if (!bar) continue;
      // (the mode bar also keeps off the axis gizmo in the top right corner)
      const end = bar === strip ? to : Math.min(to, room - 120);
      const half = Math.min(bar.offsetWidth, end - from - 16) / 2;
      bar.style.left = win.hidden ? '' : `${Math.round(Math.max(from + 8 + half, Math.min((from + end) / 2, end - 8 - half)))}px`;
    }
    strip.style.maxWidth = win.hidden ? '' : `${Math.max(120, to - from - 16)}px`;
  }
  // Dragged by the title bar; its height by the grip (each kept when let go).
  function dragWindow(down, grip) {
    if (down.button !== 0 || (!grip && down.target.closest('[role=button]'))) return;
    down.preventDefault();
    const v = viewport.getBoundingClientRect();
    const x0 = down.clientX - win.offsetLeft, y0 = down.clientY - win.offsetTop, h0 = win.offsetHeight;
    let changes = null;
    const move = e => {
      if (grip) {
        changes = { height: Math.round(Math.max(120, Math.min(h0 + e.clientY - down.clientY, v.height - win.offsetTop - 8))) };
        win.style.height = `${changes.height}px`;
      } else {
        changes = { x: Math.round(Math.max(4, Math.min(e.clientX - x0, v.width - win.offsetWidth - 4))), y: Math.round(Math.max(4, Math.min(e.clientY - y0, v.height - 120))) };
        win.style.left = `${changes.x}px`;
        win.style.top = `${changes.y}px`;
        placePanels();
      }
    };
    const up = () => {
      removeEventListener('mousemove', move, true);
      removeEventListener('mouseup', up, true);
      capturePointer(!!P.openMenuState || !!$('.modal-back'));
      if (changes) saveWindow(changes);
    };
    addEventListener('mousemove', move, true);
    addEventListener('mouseup', up, true);
    capturePointer(true);
  }
  winHead.addEventListener('mousedown', e => dragWindow(e, false));
  winGrip.addEventListener('mousedown', e => dragWindow(e, true));
  new ResizeObserver(() => { placeWindow(); reportLayout(); }).observe(viewport);
  new ResizeObserver(() => { placePanels(); reportLayout(); }).observe(win);
  winSearch.addEventListener('input', () => { winKey = ''; render(); });
  winSearch.addEventListener('keydown', e => {
    e.stopPropagation();
    if (e.key === 'Escape') { winSearch.value = ''; winSearch.blur(); winKey = ''; render(); }
    else if (e.key === 'Enter') {  // the first match that can run
      const first = winBody.querySelector('[data-command]:not(.off)');
      if (first) run(first.dataset.command);
    }
  });

  // What a command needs before it can run: "" (it can), else why not.
  const inspected = () => (state.selectedEntity && state.inspector) || {};
  const PLURAL = { vertex: 'vertices', edge: 'edges', face: 'faces' };
  const NEEDS = {
    level: () => levelSelected() ? '' : 'Select a level mesh first',
    // (in an element mode, chosen elements; commands that also work on the whole mesh run without)
    elements: c => !levelSelected() ? 'Select a level mesh first'
      : mode() !== 'object' && !inMode(c, 'object') && !((state.tools && state.tools.elementCount) > 0) ? `Choose some ${PLURAL[mode()]} first` : '',
    selection: () => state.selectedEntity ? '' : 'Select something first',
    wall: () => inspected().wall ? '' : 'Select a wall or room first',
    cut: () => inspected().cut ? '' : 'Select a cut first',
    spline: () => inspected().spline ? '' : 'Select a spline first',
  };
  const whyNot = c => (c.needs && NEEDS[c.needs] ? NEEDS[c.needs](c) : '');
  // The object's own sections (a wall's, a cut's, a spline's) only while one is selected.
  const CONTEXT = ['wall', 'cut', 'spline'];
  const WINDOW_GROUPS = ['start', 'create', 'edit', 'select', 'surface', 'object', 'world', 'tools', 'place'];
  const TITLES = { start: 'Start here', place: 'Place', tools: 'Tools' };
  // (editor code's own commands with an icon and no section: Tools)
  const groupOf = c => c.group || (c.icon && !c.builtin ? 'tools' : '');
  const titleOf = g => TITLES[g] || (GROUPS.find(x => x[0] === g) || [0, g])[1];
  const START = [['Create/Box', 'Box'], ['Create/Room', 'Room'], ['Create/Wall', 'Wall'], ['Help/Make Sample Level', 'Sample level']];
  let winKey = '', drawerFor = null;
  // Folded sections ("group" or "group/section"): the prefs', else Create's less used ones.
  const FOLDED = new Set(['create/Cuts', 'create/Volumes', 'create/Splines', 'create/Kit']);
  const isFolded = key => { const f = winPrefs().folded || {}; return key in f ? !!f[key] : FOLDED.has(key); };

  // A command's row: icon, name, key and ⚙ (its settings, opened under it).
  function commandRow(c, name) {
    const why = whyNot(c), key = keyOf(c), open = drawerFor === c.path;
    const gear = c.options && c.options.length ? el('span', { class: 'lw-gear' + (open ? ' on' : ''), role: 'button', 'data-gear': c.path, html: icon('gear'),
      onclick: () => { drawerFor = open ? null : c.path; winKey = ''; render(); } }) : null;
    return el('span', {
      class: 'lw-row' + (why ? ' off' : '') + (open ? ' open' : ''), role: 'button', 'data-command': c.path, 'data-why': why || null,
      onclick: e => { if (!e.target.closest('.lw-gear') && !why) run(c.path); },
    }, el('span', { class: 'lw-icon', html: icon(c.icon || 'cube') }), el('span', { class: 'lw-label' }, name || c.label || label(c)),
    key ? el('kbd', {}, key) : null, gear);
  }
  // A command's settings: numbers typed, choices as buttons, switches as boxes (editor.tools.option).
  function optionsDrawer(c) {
    const set = (name, value) => send('editor.tools.option', { menu: c.path, name, value });
    return el('div', { class: 'lw-drawer', 'data-options': c.path }, ...(c.options || []).map(o => {
      const v = optionValue(c, o.name) ?? o.value ?? 0;
      if (o.kind === 'choice')
        return el('div', { class: 'lw-option' }, el('span', {}, o.name), el('span', { class: 'lw-choices' }, ...(o.choices || []).map((ch, k) =>
          el('span', { class: 'btn' + (k === v ? ' on' : ''), role: 'button', 'data-choice': String(k), onclick: () => set(o.name, k) }, ch))));
      if (o.kind === 'bool')
        return el('label', { class: 'lw-option' }, el('span', {}, o.name),
          el('input', { type: 'checkbox', class: 'check', checked: !!v, onchange: e => set(o.name, e.target.checked ? 1 : 0) }));
      return el('label', { class: 'lw-option' }, el('span', {}, o.name),
        el('input', { class: 'field', type: 'number', step: o.kind === 'number' ? 'any' : '1', min: o.min ?? null, max: o.max ?? null,
          value: String(+(+v).toFixed(3)), 'data-option': o.name, onchange: e => set(o.name, parseFloat(e.target.value) || 0) }),
        el('span', { class: 'lw-unit' }, o.unit || ''));
    }));
  }
  // The rows of a list of commands, a command's settings under its row.
  const rowsOf = list => list.flatMap(c => [commandRow(c), drawerFor === c.path && !winPrefs().icons ? optionsDrawer(c) : null]).filter(Boolean);

  // The material palette's swatches (Alt+1..9) and the prefab shelf's pieces (Ctrl+Shift+1..9).
  const shortName = p => (p ? p.split('/').pop().replace(/\.(fmat|fprefab)$/, '') : 'Default');
  function materialSwatches() {
    const list = paletteList();
    // (drops: assets.js finds [data-palette-drop] under the mouse and sends it palette-drop)
    return el('div', { class: 'lw-swatches', id: 'material-palette', 'data-palette-drop': '', 'onpalette-drop': e => addToPalette(e.detail) }, ...list.map((p, k) => el('span', {
      class: 'swatch' + (p === picked ? ' on' : ''), role: 'button', 'data-material': p,
      'data-tip': `${shortName(p)} (Alt+${k + 1}): on the chosen faces (the object mode: the whole mesh); right-click: off the palette`,
      onclick: () => applyMaterial(p),
      oncontextmenu: e => { e.preventDefault(); savePalette(list.filter(x => x !== p)); },
    }, el('span', { class: 'swatch-key' }, String(k + 1)), shortName(p))),
    list.length ? null : el('span', { class: 'palette-hint' }, 'Drop materials here · Alt+C picks one off a face'));
  }
  function prefabSwatches() {
    const list = shelfList();
    return el('div', { class: 'lw-swatches', id: 'prefab-shelf', 'data-prefab-drop': '',
      'onpalette-drop': e => { if (e.detail) saveShelf([...shelfList().filter(p => p !== e.detail), e.detail]); } }, ...list.map((p, k) => el('span', {
      class: 'swatch', role: 'button', 'data-prefab': p,
      'data-tip': `${shortName(p)} (Ctrl+Shift+${k + 1}): at the mouse; a click: at the view's centre; Ctrl+click: in place of the selection; right-click: off the shelf`,
      onclick: e => e.ctrlKey ? send('editor.replace', { prefab: p }) : placePrefab(p, false),
      oncontextmenu: e => { e.preventDefault(); saveShelf(list.filter(x => x !== p)); },
    }, el('span', { class: 'swatch-key' }, String(k + 1)), shortName(p))),
    list.length ? null : el('span', { class: 'palette-hint' }, 'Drop prefabs here from the Project panel'));
  }

  // The sections for the mode: [group, [[section, nodes]]] (folded "N more" past eight rows).
  function windowSections(m) {
    const ins = inspected();
    const shown = c => groupOf(c) && inMode(c, m) && !(c.group === 'object' && CONTEXT.includes(c.needs) && !ins[c.needs]);
    const out = [];
    for (const g of WINDOW_GROUPS) {
      if (g === 'start') {
        if (m === 'object' && !state.selectedEntity)
          out.push([g, [['', START.map(([path, name]) => commands().find(c => c.path === path) && commandRow(commands().find(c => c.path === path), name)).filter(Boolean)]]]);
        continue;
      }
      if (g === 'place') {
        if (m === 'object' && state.project) out.push([g, [['Prefabs', [prefabSwatches()], 0]]]);
        continue;
      }
      if (g === 'create' && m !== 'object') continue;
      const list = commands().filter(c => groupOf(c) === g && shown(c));
      // (a section with nothing to run now stays out, but Create; Surface keeps its materials)
      if (g !== 'create' && g !== 'surface' && list.every(c => whyNot(c))) continue;
      if (g === 'surface' && list.every(c => whyNot(c)) && !levelSelected()) continue;
      const sections = [];
      for (const c of list) {
        let s = sections.find(x => x[0] === (c.section || ''));
        if (!s) sections.push(s = [c.section || '', []]);
        s[1].push(c);
      }
      const parts = sections.map(([name, cmds]) => {
        const key = `${g}/${name}`, more = !!(winPrefs().more || {})[key];
        const cut = cmds.length > 8 && !more && !winPrefs().icons ? 6 : cmds.length;
        const nodes = rowsOf(cmds.slice(0, cut));
        if (cmds.length > 8 && !winPrefs().icons)
          nodes.push(el('span', { class: 'lw-more', role: 'button', 'data-more': key,
            onclick: () => saveWindow({ more: { ...(winPrefs().more || {}), [key]: !more } }) }, more ? 'Fewer' : `${cmds.length - cut} more`));
        if (name === 'Materials') nodes.unshift(materialSwatches());
        return [name, nodes, cmds.length];
      });
      if (g === 'surface' && (m === 'object' || m === 'face') && !sections.some(s => s[0] === 'Materials'))
        parts.unshift(['Materials', [materialSwatches()], 0]);
      out.push([g, parts]);
    }
    return out;
  }
  // Every command whose name matches what is typed, best first (the palette's matcher).
  function searchRows(q) {
    const m = mode();
    const found = commands().filter(c => groupOf(c) && groupOf(c) !== 'view' && inMode(c, m))
      .map(c => ({ c, s: score(`${c.label || label(c)} ${c.section || ''} ${c.path}`.toLowerCase(), q) })).filter(x => x.s >= 0)
      .sort((a, b) => (whyNot(a.c) ? 1 : 0) - (whyNot(b.c) ? 1 : 0) || a.s - b.s).slice(0, 30).map(x => x.c);
    return found.length ? rowsOf(found) : [el('div', { class: 'palette-hint lw-empty' }, 'No command matches')];
  }

  // The footer: the running tool's settings (a shape, a wall), the paint brush's panel, or the last
  // operation with its amount and settings (a change takes it back and does it again).
  function footer() {
    const t = state.tools || {};
    if (t.tool === 'paint') return [paintPanel];
    const active = codeState.active && commands().find(c => c.path === codeState.active);
    if (active && active.options && active.options.length)
      return [el('div', { class: 'lw-foot-head' }, el('span', { class: 'lw-icon', html: icon(active.icon || 'cube') }), active.label || label(active),
        el('span', { class: 'lw-foot-note' }, 'settings')), optionsDrawer(active)];
    const last = codeState.last;
    if (!last) return [];
    const c = last.menu && commands().find(x => x.path === last.menu);
    return [el('div', { class: 'lw-foot-head', id: 'last-op', 'data-tip': 'The last operation — change it here: it is done again (still one undo step)' },
      el('span', { class: 'lw-foot-note' }, 'Last'), last.name),
    last.amountName ? el('div', { class: 'lw-drawer' }, el('label', { class: 'lw-option' }, el('span', {}, last.amountName),
      el('input', { class: 'field', type: 'number', step: 'any', value: String(last.amount), 'data-last-amount': '',
        onchange: e => send('editor.tools.option', { menu: 'Last', name: last.amountName, value: parseFloat(e.target.value) || 0 }) }),
      el('span', { class: 'lw-unit' }, last.unit || ''))) : null,
    c && c.options && c.options.length ? optionsDrawer(c) : null].filter(Boolean);
  }
  // The status strip along the foot of the view: the mode, what is chosen and the keys that matter now;
  // while a modal operation runs, its name and amount and its own keys (editor code's SceneViewport.Hint).
  function statusParts() {
    const t = state.tools || {}, hint = t.hint || '';
    if (chordNote) return [el('b', {}, chordNote)];
    if (hint) {
      const [head, ...keys] = hint.split(' · ');
      return [el('b', {}, head), ...keys.map(k => el('span', { class: 'st-key' }, k))];
    }
    const m = mode();
    if (!levelSelected() && m === 'object') return [];
    const n = t.elementCount || 0;
    const what = m === 'object' ? 'Object' : `${{ vertex: 'Vertices', edge: 'Edges', face: 'Faces' }[m]}${n ? ` · ${n} chosen` : ''}`;
    const keys = m === 'object' ? [['Tab', 'modes'], ['Shift+A', 'create'], ['Shift+1–5', 'menus'], ['F3', 'find a command']]
      : [...commands().filter(c => c.wheel === 'ops' && inMode(c, m) && keyOf(c)).slice(0, 3).map(c => [keyOf(c), (c.label || label(c)).toLowerCase()]),
        ['Shift+1–5', 'menus'], ['1–4', 'modes'], ['F3', 'find']];
    return [el('b', {}, what), ...keys.map(([k, a]) => el('span', { class: 'st-key' }, el('kbd', {}, k), ` ${a}`))];
  }

  function render() {
    try {
      renderNow();
    } catch (err) {
      send('ui.error', { message: `leveltools.js render: ${err && err.message}`, source: String(err && err.stack) });
    }
  }
  function renderNow() {
    const m = mode();
    // The first time a level mesh is selected: the window opens by itself (once; closing it is kept).
    // (Not without saved prefs, as in tests: there it opens when asked.)
    if (levelSelected() && P.prefsLoaded && P.pagePrefs && P.pagePrefs.levelWindow === undefined) {
      saveWindow({ open: true, opened: true });
      return;
    }
    const p = winPrefs();
    const show = !state.playing && !!p.open && !!state.project;
    const q = winSearch.value.toLowerCase().replace(/\s+/g, '');
    const ins = inspected();
    const key = JSON.stringify([show, m, levelSelected(), state.selectedEntity, state.tools && state.tools.elementCount, !!ins.wall, !!ins.cut, !!ins.spline,
      keymap(), codeState, commands().length, paletteList(), shelfList(), picked, q, drawerFor, p, !!state.project, state.tools && state.tools.tool]);
    const typing = win.contains(document.activeElement) && isTextField(document.activeElement) && document.activeElement !== winSearch;
    if (key !== winKey && !typing) {
      winKey = key;
      win.hidden = !show;
      win.classList.toggle('icons', !!p.icons);
      winIcons.classList.toggle('on', !!p.icons);
      if (show) {
        const body = q ? [el('div', { class: 'lw-group' }, ...searchRows(q))]
          : windowSections(m).map(([g, sections]) => {
            const folded = isFolded(g);
            return el('div', { class: `lw-group g-${g}` + (folded ? ' folded' : ''), 'data-group': g },
              el('div', { class: 'lw-group-head', role: 'button', 'data-tip': folded ? `${titleOf(g)} — click to open` : '',
                onclick: () => saveWindow({ folded: { ...(p.folded || {}), [g]: !folded } }) },
              el('span', { class: 'lw-chev', html: icon(folded ? 'chevRight' : 'chev') }), titleOf(g)),
              folded ? null : sections.map(([name, nodes, count]) => {
                // (a sub-section folds by its heading; not in icon mode, which has no headings)
                const key = `${g}/${name}`, titled = !!name && name !== titleOf(g), shut = titled && !p.icons && isFolded(key);
                return el('div', { class: 'lw-section' + (g === 'create' || g === 'start' ? ' two' : '') + (shut ? ' folded' : ''), 'data-section': name },
                  titled ? el('div', { class: 'lw-section-head', role: 'button', onclick: () => saveWindow({ folded: { ...(p.folded || {}), [key]: !shut } }) },
                    el('span', { class: 'lw-chev', html: icon(shut ? 'chevRight' : 'chev') }), name, shut && count ? el('span', { class: 'lw-count' }, String(count)) : null) : null,
                  ...(shut ? [] : nodes));
              }));
          });
        winBody.replaceChildren(...body);
      }
      // (the paint panel: in the footer while the window shows, else over the view on its own)
      const foot = show ? footer() : [];
      if (!show && paintPanel.parentNode !== viewport) viewport.append(paintPanel);
      paintPanel.classList.toggle('overlay', !show);
      winFoot.replaceChildren(...foot);
      winFoot.hidden = !foot.length;
      placeWindow();
      hideCard();
    }
    const parts = state.playing ? [] : statusParts();
    const text = parts.map(n => n.textContent).join('|');
    if (text !== strip.dataset.text) {
      strip.dataset.text = text;
      strip.replaceChildren(...parts);
      strip.hidden = !parts.length;
    }
    renderChord();
    clearOfWindow();
    reportLayout();
  }

  // ---- hover cards: the name, key and help of a button over the scene view (the off-screen browser
  // draws no title tooltips)
  const card = el('div', { class: 'hover-card', id: 'hover-card', hidden: true });
  document.body.append(card);
  // (the view's overlays, the scene toolbar and the Inspector: their tips were never seen before)
  const HOVERABLE = '#viewport .overlay [data-command], #viewport .overlay [title], #viewport .overlay [data-tip], ' +
    '#scene-toolbar [title], #scene-toolbar [data-tip], #inspector [title], #inspector [data-tip]';
  let cardFor = null, cardTimer = 0, cardHiddenAt = 0;
  // [name, key, help, why not]: a command's own, else read from its tip ("Move tool (W)", "Objects (1): click…",
  // "Light Grid — Alt+1; right-click: remove").
  function cardContent(n) {
    const c = n.dataset.command && commands().find(c => c.path === n.dataset.command);
    if (c) return [c.label || label(c), keyOf(c), c.help || '', n.dataset.why || ''];
    const tip = n.dataset.tip || '';
    const keyed = /^([^(—]+?)\s*\(([^)]{1,16})\)\s*:?\s*(.*)$/.exec(tip);
    if (keyed) return [keyed[1], keyed[2], keyed[3]];
    const dash = tip.indexOf(' — ');
    return dash < 0 ? [tip, '', ''] : [tip.slice(0, dash), '', tip.slice(dash + 3)];
  }
  function showCard(n) {
    if (!n.isConnected || state.playing) return;
    const [name, key, help, why] = cardContent(n);
    if (!name) return;
    card.replaceChildren(el('div', { class: 'hc-head' }, el('b', { class: 'hc-name' }, name), key ? el('kbd', { class: 'hc-key' }, key) : ''),
      help ? el('div', { class: 'hc-help' }, help) : '', why ? el('div', { class: 'hc-why' }, why) : '');
    card.hidden = false;
    // Beside it (the side bars), else under it (the bars along the top), kept on the page.
    const r = n.getBoundingClientRect(), w = card.offsetWidth, h = card.offsetHeight;
    let x = r.right + 8, y = r.top + r.height / 2 - h / 2;
    if (x + w > innerWidth - 4) { x = r.left + r.width / 2 - w / 2; y = r.bottom + 8; }
    card.style.left = `${Math.max(4, Math.min(x, innerWidth - w - 4))}px`;
    card.style.top = `${Math.max(4, Math.min(y, innerHeight - h - 4))}px`;
  }
  function hideCard() {
    clearTimeout(cardTimer);
    if (!card.hidden) cardHiddenAt = performance.now();
    card.hidden = true;
    cardFor = null;
  }
  document.addEventListener('mouseover', e => {
    const n = e.target.closest ? e.target.closest(HOVERABLE) : null;
    if (n === cardFor) return;
    hideCard();
    if (!n) return;
    if (n.hasAttribute('title')) { n.dataset.tip = n.getAttribute('title'); n.removeAttribute('title'); }
    cardFor = n;
    // (straight away when moving along a bar from one card to the next)
    cardTimer = setTimeout(() => showCard(n), performance.now() - cardHiddenAt < 400 ? 0 : 350);
  });
  document.addEventListener('mousedown', hideCard, true);
  document.addEventListener('wheel', hideCard, true);

  // ---- menus of commands (at the mouse, in the GameObject and Help menus): a command's row, sections
  //   under their headings
  const menuItem = c => ({ label: c.label || label(c), icon: c.icon, shortcut: keyOf(c), value: c.path, disabled: !!whyNot(c), action: () => run(c.path) });
  function sectioned(list) {
    const out = [];
    let at = null;
    for (const c of list) {
      const s = c.section || groupTitle(c);
      if (s !== at && new Set(list.map(x => x.section || groupTitle(x))).size > 1) out.push({ heading: s });
      at = s;
      out.push(menuItem(c));
    }
    return out;
  }
  // The create commands: the shapes, then a submenu a section (the GameObject menu's Level submenu, Shift+A).
  function createItems() {
    const list = commands().filter(c => c.group === 'create');
    const sections = [...new Set(list.map(c => c.section || 'Create'))];
    return sections.flatMap((s, k) => {
      const of = list.filter(c => (c.section || 'Create') === s);
      return k === 0 ? [{ heading: s }, ...of.map(menuItem)]
        : [k === 1 ? '-' : null, { label: s, icon: of[0].icon, value: s, submenu: of.map(menuItem) }].filter(Boolean);
    });
  }
  // Help > Level Starter Content: the sample level, prototype materials, the starter kit, trims, smart materials.
  const starterItems = () => commands().filter(c => c.group === 'starter').map(c => ({ ...menuItem(c), label: c.label || label(c).replace(/^Make |^Install /, '') }));
  // Shift+A, Shift+G, Shift+M, Shift+P, Shift+U and Shift+V: a menu where the mouse is (too many for a wheel).
  // ---- the chord menus' layout (LevelCommands.Chords): rings "1".."5" of groups in fixed slots, a
  // slot a command's path or a group of its own; the user's own ring (prefs "chords") replaces one.
  const chordRing = n => ((P.pagePrefs && P.pagePrefs.chords) || {})[n] || (code.chords || {})[n] || null;
  // What is wrong with the editor's own layout (tests): a path no command has, a command twice on a ring.
  function chordProblems() {
    const known = new Set(commands().map(c => c.path)), out = [];
    for (const [n, ring] of Object.entries(code.chords || {})) {
      const seen = new Set();
      const walk = slots => slots.forEach(s => {
        if (s == null) return;
        if (typeof s !== 'string') return walk(s.slots || []);
        if (!known.has(s)) out.push(`${n}: ${s} unknown`);
        else if (seen.has(s)) out.push(`${n}: ${s} twice`);
        seen.add(s);
      });
      walk(ring.slots || []);
    }
    return out.join('; ');
  }

  const MENUS = {
    'Shift+A': () => [...createItems(), '-',
      { label: 'Empty', icon: 'empty', action: () => send('editor.entity.create', { kind: 'empty' }) },
      { label: 'Point Light', icon: 'light', action: () => send('editor.entity.create', { kind: 'light', light: 'point' }) }],
    'Shift+G': () => sectioned(commands().filter(c => c.wheel === 'arrange')),
    'Shift+M': () => sectioned(commands().filter(c => c.wheel === 'modifiers')),
    'Shift+P': () => sectioned(commands().filter(c => c.wheel === 'paint')),
    'Shift+U': () => sectioned(commands().filter(c => c.wheel === 'uv' && inMode(c, mode()))),
    'Shift+V': () => sectioned(commands().filter(c => c.wheel === 'view')),
  };
  // Every operation of the mode (Alt+Q's More…).
  const opsItems = () => sectioned(commands().filter(c => (c.group === 'edit' || c.group === 'select' || c.wheel === 'ops') && inMode(c, mode())));

  // ---- directional wheels (eight items at most: Alt+Q's last is More…, the menu of all the mode's operations)
  const WHEELS = {
    'Tab': () => [['object', 'Objects', 'cube'], ['vertex', 'Vertices', 'vertex'], ['edge', 'Edges', 'edge'], ['face', 'Faces', 'face']]
      .map(([e, l, i]) => ({ label: l, icon: i, on: mode() === e, run: () => sendTools({ element: e }) })),
    'Shift+S': () => {
      const t = state.tools || {};
      return [
        { label: t.grid ? 'Grid off' : 'Grid on', icon: 'grid', run: () => sendTools({ grid: !t.grid }) },
        { label: t.snapAbsolute ? 'Snap relative' : 'Snap to grid lines', icon: 'magnet', run: () => sendTools({ snapAbsolute: !t.snapAbsolute }) },
        ...[0.1, 0.25, 0.5, 1].map(v => ({ label: `Grid ${v} m`, on: Math.abs((t.snapMove || 0) - v) < 1e-4, run: () => sendTools({ snapMove: v }) })),
      ];
    },
    'Alt+Q': () => {
      const ops = commands().filter(c => c.wheel === 'ops' && inMode(c, mode())).map(c => ({ label: c.label || label(c), icon: c.icon, run: () => run(c.path) }));
      return [...ops.slice(0, 7), { label: 'More…', icon: 'more', run: w => openMenu(opsItems(), w.x, w.y) }];
    },
    'Alt+Z': () => DRAW_MODES.slice(0, 7).map(([id, l]) => ({ label: l, on: state.settings && state.settings.debugView === id, run: () => setSetting('debugView', id) })),
  };
  let wheel = null;  // {name, key, items, node, x, y, pick}
  let pending = null;  // a wheel or menu waiting for the mouse's place (editor.view.mouse)
  function openWheel(name, key) {
    if (wheel || pending) return;
    const menu = !!MENUS[name];
    const items = (menu ? MENUS : WHEELS)[name]();
    if (!items.length) return;
    pending = { name, key, items, menu };
    send('editor.view.mouse', { tag: name });
  }
  function onMouse(p) {
    if (p.tag === 'chord') {
      if (chord && !chord.node) placeChord(p);
      return;
    }
    if (!pending || p.tag !== pending.name) return;
    const w = pending;
    pending = null;
    if (!p.inside) return;
    const r = viewport.getBoundingClientRect();
    w.x = r.left + p.x * r.width;
    w.y = r.top + p.y * r.height;
    if (w.menu) {  // (a menu stays open once the keys are let go: a click picks)
      openMenu(w.items, w.x, w.y);
      return;
    }
    const radius = 90;
    w.node = el('div', { class: 'wheel' });
    w.items.forEach((item, k) => {
      const a = -Math.PI / 2 + k * 2 * Math.PI / w.items.length;  // first item up, then clockwise
      item.angle = a;
      const n = el('div', {
        class: 'wheel-item' + (item.on ? ' on' : ''), 'data-wheel-item': item.label, html: (item.icon ? icon(item.icon) : '') + `<span>${item.label}</span>`,
        onmousedown: e => { e.preventDefault(); w.pick = item; closeWheel(true); },
      });
      n.style.left = `${w.x + Math.cos(a) * radius}px`;  // (CSSOM: the CSP drops style attributes)
      n.style.top = `${w.y + Math.sin(a) * radius}px`;
      w.node.append(n);
    });
    const center = el('div', { class: 'wheel-center' });
    center.style.left = `${w.x}px`;
    center.style.top = `${w.y}px`;
    w.node.append(center);
    document.body.append(w.node);
    wheel = w;
    capturePointer(true);
  }
  function closeWheel(runIt) {
    const w = wheel;
    wheel = null;
    if (!w) return;
    w.node.remove();
    capturePointer(!!P.openMenuState || !!$('.modal-back'));
    if (runIt && w.pick) w.pick.run(w);
  }
  document.addEventListener('mousemove', e => {
    if (!wheel) return;
    const dx = e.clientX - wheel.x, dy = e.clientY - wheel.y;
    let pick = null;
    if (Math.hypot(dx, dy) > 18) {  // the item in the mouse's direction
      const a = Math.atan2(dy, dx);
      let best = Infinity;
      for (const item of wheel.items) {
        const d = Math.abs(Math.atan2(Math.sin(a - item.angle), Math.cos(a - item.angle)));
        if (d < best) { best = d; pick = item; }
      }
    }
    wheel.pick = pick;
    [...wheel.node.querySelectorAll('.wheel-item')].forEach((n, k) => n.classList.toggle('hot', wheel.items[k] === pick));
  });

  // ---- chord menus: Shift+1–4 open the element modes' rings (the mode switched first), Shift+5 world
  // and tools; then, while Shift is held, 1–6 pick a group and then its command (a click picks too).
  // Backspace goes back up; Esc, or letting go of Shift, closes it. The slots are fixed, 1 at the top
  // and then clockwise, so a command's keys and place never change; one that can't run now stays in
  // its place, greyed (pressing it says why). A list longer than six: five, then More….
  const RING_MODES = { 1: 'object', 2: 'vertex', 3: 'edge', 4: 'face' };
  const RING_MAX = 6;
  function noteChord(text) {
    chordNote = text;
    clearTimeout(chordNoteTimer);
    chordNoteTimer = setTimeout(() => { chordNote = ''; render(); }, 2500);
    render();
  }
  function openChord(n) {
    const ring = chordRing(String(n)), m = RING_MODES[n];
    if (!ring) return;
    if (m && m !== 'object' && !levelSelected()) {
      noteChord('Select a level mesh first');
      return;
    }
    if (m && levelSelected() && mode() !== m) sendTools({ element: m });
    chord = { ring: n, stack: [{ label: ring.label, slots: ring.slots || [] }], why: '' };
    send('editor.view.mouse', { tag: 'chord' });  // (keys pick at once; it is drawn when the mouse's place is known)
  }
  function placeChord(p) {
    const r = viewport.getBoundingClientRect();
    const x = p.inside ? r.left + p.x * r.width : r.left + r.width / 2, y = p.inside ? r.top + p.y * r.height : r.top + r.height / 2;
    chord.x = Math.max(r.left + 140, Math.min(x, r.right - 140));  // (kept inside the view, slots and all)
    chord.y = Math.max(r.top + 80, Math.min(y, r.bottom - 100));
    chord.node = el('div', { class: 'chord', 'data-chord': '' });
    document.body.append(chord.node);
    capturePointer(true);
    renderChord();
  }
  // The slots of the level open now: a command (greyed, with why, when it can't run), a group, or empty.
  function chordItems() {
    const list = chord.stack[chord.stack.length - 1].slots;
    const slots = list.length > RING_MAX ? [...list.slice(0, RING_MAX - 1), { label: 'More', icon: 'more', slots: list.slice(RING_MAX - 1) }] : list;
    const m = RING_MODES[chord.ring];
    return slots.map(s => {
      if (s == null) return { empty: true, label: '', why: 'Empty' };
      if (typeof s !== 'string') return { group: s, label: s.label, icon: s.icon || 'more' };
      const c = commands().find(x => x.path === s);
      if (!c) return { label: s.split('/').pop(), icon: 'warning', why: 'Not in this editor' };
      const why = whyNot(c) || (m && !inMode(c, m) ? `Not in the ${m} mode` : '');
      return { command: c, label: c.label || label(c), icon: c.icon || 'cube', key: keyOf(c), why };
    });
  }
  const chordPath = () => chord.stack.map(l => l.label).join(' › ');
  function renderChord() {
    if (!chord || !chord.node) return;
    const rx = 78, ry = 58;
    const nodes = chordItems().map((it, k) => {
      if (it.empty) return null;  // (its place kept: the others' numbers don't move)
      const a = -Math.PI / 2 + k * 2 * Math.PI / RING_MAX;
      const n = el('span', {
        class: 'chord-slot' + (it.why ? ' off' : '') + (it.group ? ' group' : ''), role: 'button', 'data-chord-slot': String(k + 1),
        onmousedown: e => { e.preventDefault(); e.stopPropagation(); pickChord(k + 1); },
      }, el('span', { class: 'chord-num' }, String(k + 1)), el('span', { class: 'chord-icon', html: icon(it.icon) }), el('span', {}, it.label),
      it.key ? el('kbd', {}, it.key) : it.group ? el('span', { class: 'chord-more', html: icon('chevRight') }) : null);
      n.style.left = `${chord.x + Math.cos(a) * rx}px`;  // (CSSOM: the CSP drops style attributes)
      n.style.top = `${chord.y + Math.sin(a) * ry}px`;
      return n;
    });
    const center = el('span', { class: 'chord-center' }, chordPath());
    center.style.left = `${chord.x}px`;
    center.style.top = `${chord.y}px`;
    const why = chord.why ? el('span', { class: 'chord-why' }, chord.why) : null;
    if (why) {
      why.style.left = `${chord.x}px`;
      why.style.top = `${chord.y + ry + 22}px`;
    }
    chord.node.replaceChildren(center, ...nodes.filter(Boolean), ...(why ? [why] : []));
  }
  function pickChord(k) {
    if (!chord) return;
    const it = chordItems()[k - 1];
    if (!it || it.empty) return;
    if (it.why) chord.why = `${it.label}: ${it.why}`;
    else if (it.group) {
      chord.stack.push({ label: it.label, slots: it.group.slots || [] });
      chord.why = '';
    } else {
      closeChord();
      run(it.command.path);
      return;
    }
    renderChord();
  }
  function chordBack() {
    if (chord.stack.length < 2) return closeChord();
    chord.stack.pop();
    chord.why = '';
    renderChord();
  }
  function closeChord() {
    const c = chord;
    chord = null;
    if (c && c.node) {
      c.node.remove();
      capturePointer(!!P.openMenuState || !!$('.modal-back'));
    }
  }
  document.addEventListener('mousedown', e => { if (chord && !e.target.closest('.chord-slot')) closeChord(); }, true);

  // ---- the command palette
  function openPalette() {
    if ($('.modal-back')) return;
    const items = [
      ...sortedCommands().map(c => ({ label: c.path, icon: c.icon, section: c.group ? groupTitle(c) : '', key: keyOf(c) || c.shortcut || '', help: c.help || '', run: () => run(c.path) })),
      { label: 'Edit/Keymap…', key: '', help: 'Rebind the commands\' keys', run: openKeymap },
      { label: 'Edit/Chord Menus…', key: '', help: 'What the Shift+1–5 rings hold, slot by slot', run: () => openChordEditor() },
      { label: 'File/Save Scene', key: 'Ctrl+S', help: '', run: () => cmd.save() },
      { label: 'Edit/Play', key: 'Ctrl+P', help: '', run: () => cmd.play() },
      { label: 'GameObject/Level Mesh/Box', key: '', help: 'An editable mesh', run: () => send('editor.entity.create', { kind: 'level', shape: 'box' }) },
      { label: 'GameObject/Level Mesh/Plane', key: '', help: 'An editable mesh', run: () => send('editor.entity.create', { kind: 'level', shape: 'plane' }) },
    ];
    const list = el('div', { class: 'palette-list' });
    const input = el('input', { class: 'palette-input', placeholder: 'Type a command…', spellcheck: false });
    let shown = [], at = 0;
    const fill = () => {
      const q = input.value.toLowerCase().replace(/\s+/g, '');
      shown = items.map(i => ({ i, s: q ? score(i.label.toLowerCase(), q) : 0 })).filter(x => x.s >= 0).sort((a, b) => a.s - b.s).slice(0, 40).map(x => x.i);
      at = Math.min(at, Math.max(shown.length - 1, 0));
      list.replaceChildren(...shown.map((i, k) => el('div', {
        class: 'palette-item' + (k === at ? ' on' : ''), onmousedown: e => { e.preventDefault(); pick(i); },
      }, el('span', { class: 'palette-icon', html: i.icon ? icon(i.icon) : '' }), el('span', { class: 'palette-label' }, i.label),
      i.section ? el('span', { class: 'palette-section' }, i.section) : null, el('span', { class: 'palette-help' }, i.help), i.key ? el('kbd', {}, i.key) : null)));
    };
    const pick = i => { closeShortcuts(); if (i) i.run(); };
    input.addEventListener('input', () => { at = 0; fill(); });
    input.addEventListener('keydown', e => {
      if (e.key === 'ArrowDown') { at = Math.min(at + 1, shown.length - 1); fill(); e.preventDefault(); }
      else if (e.key === 'ArrowUp') { at = Math.max(at - 1, 0); fill(); e.preventDefault(); }
      else if (e.key === 'Enter') pick(shown[at]);
      else if (e.key === 'Escape') closeShortcuts();
      e.stopPropagation();
    });
    const back = el('div', { class: 'modal-back', onmousedown: e => { if (e.target === back) closeShortcuts(); } },
      el('div', { class: 'modal palette' }, input, list));
    document.body.append(back);
    capturePointer(true);
    fill();
    input.focus();
  }

  // ---- the keymap
  function openKeymap() {
    closeShortcuts();
    const edits = { ...keymap() };
    const rows = sortedCommands().map(c => {
      const field = el('input', { class: 'keymap-key', value: keyOf(c), placeholder: '(none)', spellcheck: false });
      field.addEventListener('keydown', e => {  // a key pressed is the binding (Backspace clears it)
        e.preventDefault();
        e.stopPropagation();
        if (['Shift', 'Alt', 'Control', 'Meta'].includes(e.key)) return;
        const name = e.key === 'Backspace' ? '' : keyName(e);
        field.value = name;
        edits[c.path] = name;
        conflicts();
      });
      return el('div', { class: 'keymap-row', 'data-path': c.path, 'data-group': groupTitle(c) },
        el('span', { class: 'keymap-name', html: c.icon ? icon(c.icon) : '' }, c.path), field);
    });
    // Headings between the sections; the filter hides rows (and headings with none left).
    const body = [];
    let heading = null;
    for (const row of rows) {
      if (row.dataset.group !== heading) body.push(el('div', { class: 'keymap-group' }, heading = row.dataset.group));
      body.push(row);
    }
    const filter = el('input', { class: 'keymap-filter', placeholder: 'Filter', spellcheck: false });
    filter.addEventListener('keydown', e => e.stopPropagation());
    filter.addEventListener('input', () => {
      const q = filter.value.toLowerCase();
      for (const row of rows) row.hidden = !!q && !row.dataset.path.toLowerCase().includes(q);
      for (const h of body.filter(n => n.classList.contains('keymap-group')))
        h.hidden = rows.every(r => r.hidden || r.dataset.group !== h.textContent);
    });
    const conflicts = () => {  // the same key in overlapping modes: both marked
      const all = commands().map(c => ({ c, key: (c.path in edits ? edits[c.path] : c.key) || '' }));
      for (const row of rows) {
        const me = all.find(x => x.c.path === row.dataset.path);
        const clash = me.key && all.some(x => x !== me && x.key === me.key &&
          ['object', 'vertex', 'edge', 'face'].some(m => inMode(x.c, m) && inMode(me.c, m)));
        row.classList.toggle('clash', !!clash);
      }
    };
    const save = () => {
      P.pagePrefs.keymap = edits;
      send('editor.prefs.set', { key: 'keymap', value: edits });
      closeShortcuts();
      winKey = '';
      render();
    };
    const back = el('div', { class: 'modal-back', onmousedown: e => { if (e.target === back) closeShortcuts(); } },
      el('div', { class: 'modal keymap' },
        el('div', { class: 'modal-head', html: icon('keyboard') }, 'Keymap', filter, el('span', { class: 'spacer' }),
          el('span', { class: 'tbtn', role: 'button', html: icon('close'), onclick: closeShortcuts })),
        el('div', { class: 'modal-body' }, rows.length ? body : el('div', {}, 'No commands yet (editor code\'s [EditorMenu(...)]).')),
        el('div', { class: 'modal-foot' },
          el('span', { class: 'btn', role: 'button', 'data-chord-editor': '', onclick: () => openChordEditor() }, 'Chord Menus…'),
          el('span', { class: 'spacer' }),
          el('span', { class: 'btn', role: 'button', onclick: () => {
            send('editor.prefs.set', { key: 'keymap', value: {} });
            P.pagePrefs.keymap = {};
            closeShortcuts();
            winKey = '';
            render();
          } }, 'Defaults'),
          el('span', { class: 'btn primary', role: 'button', onclick: save }, 'Save'))));
    document.body.append(back);
    capturePointer(true);
    conflicts();
  }

  // ---- the chord menus' editor (Edit > Chord Menus…): a ring's six slots, each a command, a group
  //   (clicked: its own six) or empty; Change picks a command or names a new group. Save keeps the rings
  //   changed as the user's own (prefs "chords"); Defaults gives the ring shown back the editor's.
  function openChordEditor(n = 1) {
    closeShortcuts();
    const own = { ...((P.pagePrefs && P.pagePrefs.chords) || {}) };
    const rings = {};  // the rings changed here: copies
    let ringN = n, stack = [], picking = -1;
    const six = level => {  // (a level as its six slots: a longer list's sixth and later under More…)
      if (level.slots.length > RING_MAX)
        level.slots = [...level.slots.slice(0, RING_MAX - 1), { label: 'More', icon: 'more', slots: level.slots.slice(RING_MAX - 1) }];
      while (level.slots.length < RING_MAX) level.slots.push(null);
      return level;
    };
    const tabs = el('div', { class: 'chord-edit-tabs' });
    const body = el('div', { class: 'modal-body chord-edit' });
    const open = k => {
      ringN = k;
      rings[k] = rings[k] || JSON.parse(JSON.stringify(chordRing(String(k)) || { label: `Shift+${k}`, slots: [] }));
      stack = [six(rings[k])];
      picking = -1;
      fill();
    };
    function picker(level, k) {
      const input = el('input', { class: 'keymap-filter', placeholder: 'A command, or a new group\'s name', spellcheck: false });
      const list = el('div', { class: 'chord-edit-pick' });
      const put = s => { level.slots[k] = s; picking = -1; fill(); };
      const show = () => {
        const q = input.value.trim().toLowerCase();
        const found = sortedCommands().map(c => ({ c, s: q ? score(c.path.toLowerCase(), q) : 0 })).filter(x => x.s >= 0).sort((a, b) => a.s - b.s).slice(0, 8);
        list.replaceChildren(...found.map(({ c }) => el('div', { class: 'palette-item', onmousedown: e => { e.preventDefault(); put(c.path); } },
          el('span', { class: 'palette-icon', html: icon(c.icon || 'cube') }), el('span', { class: 'palette-label' }, c.path))),
        ...(q ? [el('div', { class: 'palette-item', onmousedown: e => { e.preventDefault(); put({ label: input.value.trim(), icon: 'folder', slots: [] }); } },
          el('span', { class: 'palette-icon', html: icon('folder') }), el('span', { class: 'palette-label' }, `New group "${input.value.trim()}"`))] : []));
      };
      input.addEventListener('input', show);
      input.addEventListener('keydown', e => { e.stopPropagation(); if (e.key === 'Escape') { picking = -1; fill(); } });
      show();
      setTimeout(() => input.focus(), 0);
      return el('div', { class: 'chord-edit-picker' }, input, list);
    }
    function fill() {
      tabs.replaceChildren(...[1, 2, 3, 4, 5].map(k => el('span', {
        class: 'btn' + (k === ringN ? ' primary' : ''), role: 'button', 'data-chord-ring': String(k), onclick: () => open(k),
      }, `Shift+${k} ${(rings[k] || chordRing(String(k)) || {}).label || ''}`)));
      const level = stack[stack.length - 1];
      const rows = level.slots.map((s, k) => {
        const c = typeof s === 'string' ? commands().find(x => x.path === s) : null;
        const group = s && typeof s !== 'string';
        const name = s == null ? '(empty)' : group ? `${s.label} ›` : c ? c.path : `${s} (not in this editor)`;
        return el('div', { class: 'keymap-row chord-edit-row' + (group ? ' group' : ''), 'data-chord-edit': String(k + 1) },
          el('span', { class: 'chord-num' }, String(k + 1)),
          el('span', { class: 'keymap-name', html: icon(s == null ? 'empty' : group ? s.icon || 'more' : (c && c.icon) || 'cube'),
            onclick: () => { if (group) { stack.push(six(s)); picking = -1; fill(); } } }, name),
          el('span', { class: 'btn', role: 'button', onclick: () => { picking = picking === k ? -1 : k; fill(); } }, 'Change'),
          s == null ? null : el('span', { class: 'btn', role: 'button', onclick: () => { level.slots[k] = null; fill(); } }, 'Clear'),
          picking === k ? picker(level, k) : null);
      });
      body.replaceChildren(el('div', { class: 'keymap-group' }, stack.map(l => l.label).join(' › '),
        stack.length > 1 ? el('span', { class: 'btn', role: 'button', onclick: () => { stack.pop(); picking = -1; fill(); } }, 'Back') : null), ...rows);
    }
    const save = () => {
      for (const [k, r] of Object.entries(rings)) own[k] = r;
      P.pagePrefs.chords = own;
      send('editor.prefs.set', { key: 'chords', value: own });
      closeShortcuts();
    };
    const defaults = () => {  // (this ring the editor's again, kept at once)
      delete own[ringN];
      delete rings[ringN];
      P.pagePrefs.chords = { ...own };
      send('editor.prefs.set', { key: 'chords', value: P.pagePrefs.chords });
      open(ringN);
    };
    const back = el('div', { class: 'modal-back', onmousedown: e => { if (e.target === back) closeShortcuts(); } },
      el('div', { class: 'modal keymap chord-editor' },
        el('div', { class: 'modal-head', html: icon('keyboard') }, 'Chord Menus', el('span', { class: 'spacer' }),
          el('span', { class: 'tbtn', role: 'button', html: icon('close'), onclick: closeShortcuts })),
        tabs, body,
        el('div', { class: 'modal-foot' }, el('span', { class: 'muted' }, 'Hold Shift: Shift+1–5 opens a ring, then 1–6 pick'), el('span', { class: 'spacer' }),
          el('span', { class: 'btn', role: 'button', onclick: defaults }, 'Defaults'),
          el('span', { class: 'btn primary', role: 'button', onclick: save }, 'Save'))));
    document.body.append(back);
    capturePointer(true);
    open(n);
  }

  // ---- the material palette: materials at hand for level mesh faces (prefs "materialPalette"), in the
  //   Level window's Surface section: a click (or Alt+1..9) puts one on the chosen faces (the object mode:
  //   the whole mesh); drop materials from the Project panel onto it; the eyedropper (Alt+C) adds the
  //   picked one first
  const paletteList = () => ((P.pagePrefs && P.pagePrefs.materialPalette) || []).slice(0, 9);
  function savePalette(list) {
    if (P.pagePrefs) P.pagePrefs.materialPalette = list.slice(0, 9);
    send('editor.prefs.set', { key: 'materialPalette', value: list.slice(0, 9) });
    winKey = '';
    render();
  }
  function addToPalette(path) {
    if (path === undefined || path === null) return;
    savePalette([path, ...paletteList().filter(p => p !== path)]);
  }
  const applyMaterial = path => send('editor.level.material', { material: path });
  let picked = null;

  // ---- the prefab shelf: kit pieces at hand (prefs "prefabShelf"), the Level window's Place section: a
  //   click places one at the view's centre, Ctrl+Shift+1..9 where the mouse is, Ctrl+click puts it in place of
  //   each selected one (Replace); right-click takes it off; prefabs dropped from the Project panel go on
  //   it (the scatter brush scatters what is on it)
  const shelfList = () => ((P.pagePrefs && P.pagePrefs.prefabShelf) || []).slice(0, 9);
  function saveShelf(list) {
    if (P.pagePrefs) P.pagePrefs.prefabShelf = list.slice(0, 9);
    send('editor.prefs.set', { key: 'prefabShelf', value: list.slice(0, 9) });
    winKey = '';
    render();
  }
  const placePrefab = (path, atMouse) => send('editor.asset.place', { prefab: path, ...(atMouse ? { atMouse: true } : {}) });

  // ---- the UV editor: the chosen faces' UVs (engine: editor.level.uvs), dragged to move, the wheel
  //   to scale (Shift: turn), its buttons for the UV operations, fields for the first face's settings
  const uvPanel = el('div', { class: 'overlay uv-panel', id: 'uv-panel', hidden: true });
  viewport.append(uvPanel);
  let uvData = { faces: [] }, uvOpen = false, uvDrag = null, uvFrame = null;
  const levelOp = (op, args) => send('editor.level.op', { op, args: args || {} });
  function toggleUvEditor(open) {
    uvOpen = open === undefined ? !uvOpen : open;
    uvPanel.hidden = !uvOpen;
    send('editor.level.uvs', { open: uvOpen });
    renderUvs();
    placePanels();
    reportLayout();
  }
  const SIZE = 220;
  function renderUvs() {
    if (!uvOpen) return;
    const faces = uvData.faces || [];
    // The view: every UV in it, square, a little margin; whole tiles at least.
    let lo = [Infinity, Infinity], hi = [-Infinity, -Infinity];
    for (const f of faces)
      for (let k = 0; k + 1 < f.uv.length; k += 2) {
        lo = [Math.min(lo[0], f.uv[k]), Math.min(lo[1], f.uv[k + 1])];
        hi = [Math.max(hi[0], f.uv[k]), Math.max(hi[1], f.uv[k + 1])];
      }
    if (!faces.length) lo = [0, 0], hi = [1, 1];
    if (!uvDrag || !uvFrame) {
      const span = Math.max(hi[0] - lo[0], hi[1] - lo[1], 1) * 1.15;
      const c = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2];
      uvFrame = { x: c[0] - span / 2, y: c[1] - span / 2, span };
    }
    const fr = uvFrame, px = SIZE / fr.span;
    const X = u => (u - fr.x) * px, Y = v => (v - fr.y) * px;
    const dx = uvDrag ? uvDrag.du * px : 0, dy = uvDrag ? uvDrag.dv * px : 0;
    let grid = '';
    for (let u = Math.ceil(fr.x); u <= fr.x + fr.span; ++u) grid += `<line class="uv-tile" x1="${X(u)}" y1="0" x2="${X(u)}" y2="${SIZE}"/>`;
    for (let v = Math.ceil(fr.y); v <= fr.y + fr.span; ++v) grid += `<line class="uv-tile" x1="0" y1="${Y(v)}" x2="${SIZE}" y2="${Y(v)}"/>`;
    const polys = faces.map(f => {
      const pts = [];
      for (let k = 0; k + 1 < f.uv.length; k += 2) pts.push(`${X(f.uv[k]) + dx},${Y(f.uv[k + 1]) + dy}`);
      return `<polygon class="uv-face" points="${pts.join(' ')}"/>`;
    }).join('');
    const first = faces[0];
    const field = (label, value, set) => el('label', { class: 'uv-field' }, label,
      el('input', { type: 'number', step: '0.05', value: first ? +value.toFixed(3) : '', disabled: !first,
        onchange: e => set(parseFloat(e.target.value) || 0) }));
    const b = (text, title, fn) => el('span', { class: 'btn uv-btn', role: 'button', title, onclick: fn }, text);
    uvPanel.replaceChildren(
      el('div', { class: 'uv-head' }, el('span', { class: 'uv-title', html: icon('uvEditor') }, `UVs · ${faces.length} face${faces.length === 1 ? '' : 's'}${first ? ' · ' + first.mode : ''}`),
        el('span', { class: 'spacer' }), el('span', { class: 'tbtn', role: 'button', html: icon('close'), onclick: () => toggleUvEditor(false) })),
      el('div', { class: 'uv-canvas', html: `<svg width="${SIZE}" height="${SIZE}" viewBox="0 0 ${SIZE} ${SIZE}">${grid}${polys}</svg>` }),
      el('div', { class: 'uv-buttons' },
        b('Fit', 'The texture once across each face', () => levelOp('uvFit')),
        b('Align', 'Along each face\'s longest edge', () => levelOp('uvAlign')),
        b('Match', 'Carry the first face\'s texture across the others', () => levelOp('uvMatch')),
        b('Unwrap', 'Flat charts, a metre a tile', () => levelOp('uvUnwrap')),
        b('⟳ 90', 'Turn a quarter', () => levelOp('uvNudge', { rotation: 90 })),
        b('⇋ U', 'Mirror across', () => levelOp('uvNudge', { scale: [-1, 1] })),
        b('⇵ V', 'Mirror up and down', () => levelOp('uvNudge', { scale: [1, -1] })),
        b('World', 'Texture lock: projected in world space', () => levelOp('uv', { mode: 'world' })),
        b('Local', 'Projected in the mesh\'s space', () => levelOp('uv', { mode: 'local' })),
        b('Reset', 'A local projection, a metre a tile', () => levelOp('uvReset'))),
      el('div', { class: 'uv-fields' },
        field('Offset U', first ? first.offset[0] : 0, v => levelOp('uv', { offset: [v, first.offset[1]] })),
        field('Offset V', first ? first.offset[1] : 0, v => levelOp('uv', { offset: [first.offset[0], v] })),
        field('Scale U', first ? first.scale[0] : 1, v => levelOp('uv', { scale: [v || 1, first.scale[1]] })),
        field('Scale V', first ? first.scale[1] : 1, v => levelOp('uv', { scale: [first.scale[0], v || 1] })),
        field('Rotation', first ? first.rotation : 0, v => levelOp('uv', { rotation: v }))));
    const canvas = uvPanel.querySelector('.uv-canvas');
    canvas.addEventListener('mousedown', e => {
      if (e.button !== 0 || !faces.length) return;
      e.preventDefault();
      uvDrag = { x: e.clientX, y: e.clientY, du: 0, dv: 0, px };
      capturePointer(true);
    });
    canvas.addEventListener('wheel', e => {
      if (!faces.length) return;
      e.preventDefault();
      if (e.shiftKey) levelOp('uvNudge', { rotation: e.deltaY > 0 ? 15 : -15 });
      else { const f = e.deltaY > 0 ? 1.1 : 1 / 1.1; levelOp('uvNudge', { scale: [f, f] }); }
    }, { passive: false });
  }
  document.addEventListener('mousemove', e => {
    if (!uvDrag) return;
    uvDrag.du = (e.clientX - uvDrag.x) / uvDrag.px;
    uvDrag.dv = (e.clientY - uvDrag.y) / uvDrag.px;
    renderUvs();
  });
  document.addEventListener('mouseup', () => {
    if (!uvDrag) return;
    const d = uvDrag;
    uvDrag = null;
    capturePointer(!!P.openMenuState || !!$('.modal-back'));
    // Moving the faces' UVs by (du, dv) is their offset by the same (offset adds after the rest).
    if (Math.hypot(d.du, d.dv) > 1e-4) levelOp('uvNudge', { offset: [d.du, d.dv] });
    else renderUvs();
  });
  // ---- trim sheets (.ftrim): a sheet's texture with its strips over it; a click on a strip puts the
  //   chosen faces on it (editor.level.trim); Edit changes the strips (editor.trim.save)
  const trimPanel = el('div', { class: 'overlay uv-panel trim-panel', id: 'trim-panel', hidden: true });
  viewport.append(trimPanel);
  let trims = { sheets: [] }, trimOpen = false, trimIndex = 0, trimEdit = null;
  const trimImages = new Map();  // image path -> data URL (editor.assets.read)
  function toggleTrims(open) {
    trimOpen = open === undefined ? !trimOpen : open;
    trimPanel.hidden = !trimOpen;
    if (trimOpen && uvOpen) toggleUvEditor(false);
    if (trimOpen) send('editor.trim.list', {});
    renderTrims();
    placePanels();
    reportLayout();
  }
  function renderTrims() {
    if (!trimOpen) return;
    const sheets = trims.sheets || [];
    trimIndex = Math.min(trimIndex, Math.max(sheets.length - 1, 0));
    const sheet = sheets[trimIndex];
    const b = (text, title, fn) => el('span', { class: 'btn uv-btn', role: 'button', title, onclick: fn }, text);
    const head = el('div', { class: 'uv-head' }, el('span', { class: 'uv-title', html: icon('trim') }, 'Trim sheets'), el('span', { class: 'spacer' }),
      el('span', { class: 'tbtn', role: 'button', html: icon('close'), onclick: () => toggleTrims(false) }));
    if (!sheet) {
      trimPanel.replaceChildren(head, el('div', { class: 'palette-hint' }, 'No trim sheets in the project yet.'),
        el('div', { class: 'uv-buttons' }, b('Make starter trims', 'Eight strips in Assets/Trims to start from', () => send('editor.trim.starter', {}))));
      return;
    }
    const pick = el('select', { class: 'trim-pick', onchange: e => { trimIndex = +e.target.value; trimEdit = null; renderTrims(); } },
      ...sheets.map((s, k) => el('option', { value: String(k), selected: k === trimIndex }, s.path.split('/').pop().replace(/\.ftrim$/, ''))));
    // The texture (when the material has one) with a band per strip; else the bands alone.
    const url = sheet.image ? trimImages.get(sheet.image) : null;
    if (sheet.image && url === undefined) {
      trimImages.set(sheet.image, null);
      send('editor.assets.read', { path: sheet.image });
    }
    const strips = trimEdit ? trimEdit.strips : sheet.strips;
    const picture = el('div', { class: 'trim-picture' + (url ? '' : ' plain') }, url ? el('img', { src: url, alt: '' }) : null);
    strips.forEach((s, k) => {
      const band = el('div', {
        class: 'trim-band' + (trims.lastPath === sheet.path && trims.lastStrip === k ? ' on' : ''), title: `${s.name} (${s.v0}–${s.v1})`,
        'data-strip': String(k), onclick: () => { if (!trimEdit) send('editor.level.trim', { path: sheet.path, strip: k }); },
      }, el('span', {}, s.name));
      band.style.top = `${s.v0 * 100}%`;  // (CSSOM: the CSP drops style attributes)
      band.style.height = `${(s.v1 - s.v0) * 100}%`;
      picture.append(band);
    });
    const parts = [head, el('div', { class: 'uv-buttons' }, pick, trimEdit ? null : b('Edit', 'Change the strips', () => {
      trimEdit = { material: sheet.material, aspect: sheet.aspect, strips: sheet.strips.map(s => ({ ...s })) };
      renderTrims();
    }), trimEdit ? null : b('New', 'A new trim sheet on the picked material (Alt+C)', () => {
      const path = `Assets/Trims/Trim ${sheets.length + 1}.ftrim`;
      send('editor.trim.save', { path, sheet: { material: picked || '', aspect: 1, strips: [{ name: 'Strip', v0: 0, v1: 0.25 }] } });
      trimIndex = sheets.length;
    })), picture];
    if (trimEdit) {
      const num = (value, set) => el('input', { type: 'number', step: '0.0625', min: '0', max: '1', value: String(value), onchange: e => set(parseFloat(e.target.value) || 0) });
      parts.push(el('label', { class: 'uv-field' }, 'Material', el('input', { type: 'text', value: trimEdit.material, onchange: e => { trimEdit.material = e.target.value; } })));
      trimEdit.strips.forEach((s, k) => parts.push(el('div', { class: 'trim-row' },
        el('input', { type: 'text', value: s.name, onchange: e => { s.name = e.target.value; } }),
        num(s.v0, v => { s.v0 = v; renderTrims(); }), num(s.v1, v => { s.v1 = v; renderTrims(); }),
        el('span', { class: 'tbtn', role: 'button', html: icon('close'), title: 'Remove', onclick: () => { trimEdit.strips.splice(k, 1); renderTrims(); } }))));
      parts.push(el('div', { class: 'uv-buttons' },
        b('+ Strip', 'Another strip below the last', () => {
          const last = trimEdit.strips[trimEdit.strips.length - 1];
          const v0 = last ? last.v1 : 0;
          trimEdit.strips.push({ name: `Strip ${trimEdit.strips.length + 1}`, v0, v1: Math.min(1, v0 + 0.125) });
          renderTrims();
        }),
        b('Save', 'Write the .ftrim', () => {
          send('editor.trim.save', { path: sheet.path, sheet: trimEdit });
          trimEdit = null;
        }),
        b('Cancel', '', () => { trimEdit = null; renderTrims(); })));
    } else {
      parts.push(el('div', { class: 'palette-hint' }, 'Click a strip: the chosen faces go on it (T: again)'));
    }
    trimPanel.replaceChildren(...parts.filter(Boolean));
  }
  // ---- the level paint brush's panel (the paint tool: Paint/Layer Brush, Shift+B): what it paints (the
  //   selected mesh's materials, each with what it is used for, and + to add one), its radius ([ ]),
  //   strength and colour (vertex colours; alpha: occlusion), the decals it stamps, the damage it cuts
  const paintPanel = el('div', { class: 'overlay paint-panel', id: 'paint-panel', hidden: true });
  viewport.append(paintPanel);
  let paintKey = '', smart = null, smartPick = 0;  // (smart: the project's .fsmart presets, asked for when the panel opens)
  const hex2 = n => n.toString(16).padStart(2, '0');
  // (not drawn again under a held mouse: a slider being dragged stays; once let go, it is, after the
  // click this mouse-up makes: drawn again at once, the click would land on nothing)
  let paintPressed = false;
  paintPanel.addEventListener('mousedown', () => { paintPressed = true; });
  document.addEventListener('mouseup', () => {
    if (!paintPressed) return;
    paintPressed = false;
    setTimeout(renderPaint);
  });
  const COLOURS = ['#ffffff', '#bfbfbf', '#808080', '#404040', '#000000', '#b3392e', '#d9822b', '#e8c547', '#4c9a4a', '#3a79bb', '#7d4fa8', '#7a5230'];
  function renderPaint() {
    if (paintPressed) return;
    const t = state.tools || {};
    const show = !state.playing && t.tool === 'paint';
    const ins = inspected();
    const key = JSON.stringify([show, t.paintMode, t.paintRadius, t.paintStrength, t.paintColor, t.paintDecal, t.paintDamage, t.paintRubble, t.paintCap, t.damageSpacing, t.damageRandomness, t.damageDensity, paletteList(), smart, smartPick,
      t.scatterPrefabs, t.scatterDensity, t.scatterSpacing, t.scatterSlope, t.scatterScale, t.scatterAlign, shelfList(), ins.surface, ins.wear]);
    if (key === paintKey) return;
    paintKey = key;
    paintPanel.hidden = !show;
    if (!show) { smart = null; reportLayout(); return; }
    if (smart === null) {
      smart = [];
      send('editor.level.smart.list', {});
    }
    const mode = t.paintMode || 0, color = (t.paintColor === undefined ? 0xffffffff : t.paintColor) >>> 0;
    const layers = (ins.surface && ins.surface.layers) || [];
    const modeButton = (m, text, on = mode === m) => el('span', { class: 'btn uv-btn' + (on ? ' on' : ''), role: 'button', 'data-paint-mode': String(m),
      // (scatter: the shelf's prefabs, when it has none picked yet)
      onclick: () => sendTools({ paintMode: m, ...(m === 7 && !(t.scatterPrefabs || []).length ? { scatterPrefabs: shelfList() } : {}) }) }, text);
    // A slider with its value beside it (shown: how the value reads).
    const slider = (label, value, min, max, step, set, shown = v => (+v).toFixed(2)) => {
      const out = el('span', { class: 'slider-value' }, shown(value));
      return el('label', { class: 'uv-field' }, label,
        el('input', { type: 'range', min: String(min), max: String(max), step: String(step), value: String(value),
          oninput: e => { out.textContent = shown(+e.target.value); set(parseFloat(e.target.value)); } }), out);
    };
    const parts = [
      el('div', { class: 'uv-head' }, el('span', { class: 'uv-title', html: icon('paint') }, 'Paint'), el('span', { class: 'spacer' }),
        el('span', { class: 'tbtn', role: 'button', html: icon('close'), onclick: () => sendTools({ tool: 'move' }) })),
      // (Materials: the brush on the first of them, until one is picked in the list)
      el('div', { class: 'uv-buttons' }, modeButton(Math.max(0, layers.findIndex(Boolean)), 'Materials', mode < 3), modeButton(3, 'Erase'),
        modeButton(4, 'Colour'), modeButton(5, 'Decals'), modeButton(6, 'Damage'), modeButton(7, 'Scatter')),
      slider('Radius', Math.log10(t.paintRadius || 0.5), -1.5, 1.3, 0.01, v => sendTools({ paintRadius: Math.round(Math.pow(10, v) * 1000) / 1000 }),
        v => `${Math.pow(10, v).toFixed(2)} m`),
      slider('Strength', t.paintStrength === undefined ? 0.5 : t.paintStrength, 0, 1, 0.01, v => sendTools({ paintStrength: v }), v => `${Math.round(v * 100)}%`),
    ];
    if (mode === 4) {
      const rgb = `#${hex2(color & 255)}${hex2((color >>> 8) & 255)}${hex2((color >>> 16) & 255)}`;
      const pack = (hex, a) => ((parseInt(hex.slice(1, 3), 16) | parseInt(hex.slice(3, 5), 16) << 8 | parseInt(hex.slice(5, 7), 16) << 16 |
        Math.round(a * 255) << 24) >>> 0);
      const alpha = (color >>> 24) / 255;
      // (a row of colours: the off-screen browser opens no colour picker)
      parts.push(el('div', { class: 'uv-field' }, 'Colour', el('span', { class: 'colour-row' }, ...COLOURS.map(c => {
        const chip = el('span', { class: 'colour-chip' + (c === rgb ? ' on' : ''), role: 'button', 'data-colour': c, onclick: () => sendTools({ paintColor: pack(c, alpha) }) });
        chip.style.background = c;  // (CSSOM: the CSP drops style attributes)
        return chip;
      }))));
      parts.push(slider('Occlusion', alpha, 0, 1, 0.01, v => sendTools({ paintColor: pack(rgb, v) }), v => `${Math.round(v * 100)}%`));
    } else if (mode === 5) {  // decals: their material from the material palette (Strength: their opacity)
      const choices = [...new Set([t.paintDecal, ...paletteList()].filter(Boolean))];
      parts.push(choices.length
        ? el('label', { class: 'uv-field' }, 'Decal', el('select', { class: 'trim-pick', 'data-paint-decal': '', onchange: e => sendTools({ paintDecal: e.target.value }) },
          t.paintDecal ? null : el('option', { value: '', selected: true }, 'Pick a material'),
          ...choices.map(p => el('option', { value: p, selected: p === t.paintDecal }, p.split('/').pop().replace(/\.fmat$/, '')))))
        : el('div', { class: 'palette-hint' }, 'Put decal materials in the material palette first'));
    } else if (mode === 7) {  // scatter: which of the shelf's prefabs, how many, how far apart, how steep, how big, turned how
      const picked = t.scatterPrefabs || [];
      const pieces = [...new Set([...shelfList(), ...picked])];
      parts.push(pieces.length
        ? el('div', { class: 'uv-buttons' }, ...pieces.map(p => el('label', { class: 'uv-field', title: p }, p.split('/').pop().replace(/\.fprefab$/, ''),
          el('input', { type: 'checkbox', class: 'check', 'data-scatter-prefab': p, checked: picked.includes(p),
            onchange: e => sendTools({ scatterPrefabs: e.target.checked ? [...picked, p] : picked.filter(x => x !== p) }) }))))
        : el('div', { class: 'palette-hint' }, 'Put prefabs on the prefab shelf first'));
      parts.push(slider('Density', t.scatterDensity === undefined ? 1 : t.scatterDensity, 0, 10, 0.1, v => sendTools({ scatterDensity: v })),
        slider('Spacing', t.scatterSpacing === undefined ? 0.5 : t.scatterSpacing, 0, 5, 0.05, v => sendTools({ scatterSpacing: v })),
        slider('Max slope', t.scatterSlope === undefined ? 45 : t.scatterSlope, 0, 90, 1, v => sendTools({ scatterSlope: v })));
      const scale = t.scatterScale || [0.8, 1.2];
      parts.push(slider('Smallest', scale[0], 0.1, 3, 0.05, v => sendTools({ scatterScale: [v, scale[1]] })),
        slider('Largest', scale[1], 0.1, 3, 0.05, v => sendTools({ scatterScale: [scale[0], v] })),
        el('label', { class: 'uv-field', title: 'Each piece turned to the surface under it (else upright)' }, 'Follow surface',
          el('input', { type: 'checkbox', class: 'check', checked: !!t.scatterAlign, onchange: e => sendTools({ scatterAlign: e.target.checked }) })));
    } else if (mode === 6) {  // damage: holes, chipped edges or broken corners (Strength: how rough); its broken faces' material
      const where = t.paintDamage || 0;
      const whereButton = (k, text, title) => el('span', { class: 'btn uv-btn' + (where === k ? ' on' : ''), role: 'button', title,
        'data-paint-damage': String(k), onclick: () => sendTools({ paintDamage: k }) }, text);
      parts.push(el('div', { class: 'uv-buttons' }, whereButton(0, 'Holes', 'Lumpy holes where you drag'),
        whereButton(1, 'Chip edges', 'Chips off the sharp edges near the brush'), whereButton(2, 'Break corners', 'The corners near the brush broken off')));
      parts.push(el('label', { class: 'uv-field', title: 'A heap of rubble under each hole in a wall or ceiling' }, 'Rubble',
        el('input', { type: 'checkbox', class: 'check', 'data-paint-rubble': '', checked: t.paintRubble !== false,
          onchange: e => sendTools({ paintRubble: e.target.checked }) })));
      parts.push(el('label', { class: 'uv-field' }, 'Inside', el('select', { class: 'trim-pick', onchange: e => sendTools({ paintCap: e.target.value }) },
        el('option', { value: '', selected: !t.paintCap }, "The mesh's own"),
        ...[...new Set([t.paintCap, ...paletteList()].filter(Boolean))].map(p =>
          el('option', { value: p, selected: p === t.paintCap }, p.split('/').pop().replace(/\.fmat$/, ''))))));
      // Damage over the selection: how far apart (0: from the brush's size), how irregular, how many.
      const spacing = t.damageSpacing || 0, auto = (t.paintRadius || 0.5) * 1.5;
      const titled = (node, text) => { node.title = text; return node; };
      parts.push(el('div', { class: 'modifier-head' }, el('label', {}, 'Damage selection')),
        titled(slider('Spacing', spacing, 0, 5, 0.01, v => sendTools({ damageSpacing: v }),
          v => (+v > 0 ? `${(+v).toFixed(2)} m` : `Auto (${auto.toFixed(2)} m)`)),
          'How far apart the chips (along edges) and holes (over faces) are, in metres. Auto: from the brush size (1.5 radii; holes 2.5)'),
        titled(slider('Randomness', t.damageRandomness || 0, 0, 1, 0.01, v => sendTools({ damageRandomness: v }), v => `${Math.round(v * 100)}%`),
          'How far each chip strays from its even place along the edge (up to half the spacing) and how much its size varies (up to ±50%)'),
        titled(slider('Density', t.damageDensity === undefined ? 1 : t.damageDensity, 0, 1, 0.01, v => sendTools({ damageDensity: v }), v => `${Math.round(v * 100)}%`),
          'The share of the places that get damage, picked at random (100%: every one; 50%: about half)'));
      parts.push(el('div', { class: 'uv-buttons' }, el('span', { class: 'btn uv-btn', role: 'button', 'data-damage-selection': '',
        title: 'At once, at this size: the chosen vertices broken, the chosen edges chipped, the chosen faces holed; a whole mesh along its sharp edges; a selected spline along its curve',
        onclick: () => send('editor.level.damage', {}) }, 'Damage selection')));
    } else if (!ins.surface) {
      parts.push(el('div', { class: 'palette-hint' }, 'Click a level mesh in the view to paint it'));
    } else {  // the mesh's materials, the brush on the one picked (Erase: off all of them)
      if (mode < 3) parts.push(el('div', { class: 'surf-list' }, ...surfaceList(ins, layers[mode] ? mode : -1, k => sendTools({ paintMode: k }))));
      parts.push(el('div', { class: 'uv-buttons' },
        mode < 3 && layers[mode] ? el('span', { class: 'btn uv-btn', role: 'button', title: 'The picked material over the whole mesh',
          onclick: () => send('editor.level.mask', { op: 'fill', channel: mode, value: 1 }) }, 'Fill') : null,
        layers.some(Boolean) ? el('span', { class: 'btn uv-btn', role: 'button', title: 'What was painted, off everywhere (the edge wear, stains and streaks stay)',
          onclick: () => send('editor.level.mask', { op: 'clear' }) }, 'Clear painting') : null));
    }
    // Smart materials: a preset (its faces' material, layers, wear and grime) in one click.
    parts.push(smart.length
      ? el('div', { class: 'uv-buttons' },
        el('select', { class: 'trim-pick', onchange: e => { smartPick = +e.target.value; } },
          ...smart.map((p, k) => el('option', { value: String(k), selected: k === smartPick }, p.split('/').pop().replace(/\.fsmart$/, '')))),
        el('span', { class: 'btn uv-btn', role: 'button', title: 'Its material, layers, wear and grime on the selected mesh',
          onclick: () => send('editor.level.smart', { path: smart[Math.min(smartPick, smart.length - 1)] }) }, 'Apply'))
      : el('div', { class: 'uv-buttons' }, el('span', { class: 'btn uv-btn', role: 'button', title: 'Old concrete, chipped paint, rusty steel in Assets/Smart',
          onclick: () => send('editor.level.smart.starter', {}) }, 'Make starter smart materials')));
    parts.push(el('div', { class: 'palette-hint' }, mode === 5 ? 'Drag on any surface · [ ] size · Ctrl+Z: the stroke' : mode === 6 ? 'Drag on level meshes · [ ] size · Ctrl+Z: the stroke'
      : mode === 7 ? 'Drag on any surface (Shift: erase) · [ ] size · Ctrl+Z: the stroke' : 'Drag on the mesh · click another to paint it · [ ] size · Esc: done'));
    paintPanel.replaceChildren(...parts);
    reportLayout();
  }
  function onLevelMessage(p) {
    if (p.type === 'editor.level.uvs') {
      uvData = p;
      renderUvs();
    } else if (p.type === 'editor.level.uvEditor') {
      if (trimOpen) toggleTrims(false);
      toggleUvEditor();
    } else if (p.type === 'editor.level.trimPanel') {
      toggleTrims();
    } else if (p.type === 'editor.level.smartList') {
      smart = p.presets || [];
      renderPaint();
    } else if (p.type === 'editor.trim.list') {
      trims = p;
      renderTrims();
    } else if (p.type === 'editor.assets.image') {
      if (trimImages.has(p.path)) {
        trimImages.set(p.path, p.url || null);
        renderTrims();
      }
    } else if (p.type === 'editor.level.picked') {
      picked = p.material;
      addToPalette(p.material);
    }
  }

  // ---- keys (before editor.js's own: capture phase)
  document.addEventListener('keydown', e => {
    if (isTextField(document.activeElement)) return;
    if (state.tools && state.tools.captured) {  // a modal operation holds the view: its keys (Esc, Enter, digits), not the page's
      e.stopPropagation();
      return;
    }
    const name = keyName(e);
    if (chord) {  // a chord menu open: 1–9 pick, Backspace goes up, Esc closes; another key closes it and goes on
      const digit = /^(?:Shift\+)?([1-9])$/.exec(name);
      if (digit || /^(Shift\+)?(Backspace|Escape)$/.test(name)) {
        e.preventDefault();
        e.stopPropagation();
        if (!e.repeat) {
          if (digit) pickChord(+digit[1]);
          else if (name.endsWith('Backspace')) chordBack();
          else closeChord();
        }
        return;
      }
      if (e.key !== 'Shift') closeChord();
    }
    const ringKey = /^Shift\+([1-5])$/.exec(name);
    if (ringKey && !state.playing && !$('.modal-back') && !P.openMenuState && !wheel) {
      e.preventDefault();
      e.stopPropagation();
      if (!e.repeat) openChord(+ringKey[1]);
      return;
    }
    if (name === 'F3' || name === 'Ctrl+Space') {
      e.preventDefault();
      e.stopPropagation();
      openPalette();
      return;
    }
    const piece = /^Ctrl\+Shift\+([1-9])$/.exec(name);
    if (piece && !state.playing && shelfList()[+piece[1] - 1] !== undefined) {  // a shelf prefab where the mouse is
      e.preventDefault();
      e.stopPropagation();
      placePrefab(shelfList()[+piece[1] - 1], true);
      return;
    }
    const slot = /^Alt\+([1-9])$/.exec(name);
    if (slot && !state.playing && levelSelected() && paletteList()[+slot[1] - 1] !== undefined) {  // a palette material on the chosen faces
      e.preventDefault();
      e.stopPropagation();
      applyMaterial(paletteList()[+slot[1] - 1]);
      return;
    }
    if ((WHEELS[name] || MENUS[name]) && !state.playing && !$('.modal-back') && !P.openMenuState) {
      e.preventDefault();
      e.stopPropagation();
      if (!e.repeat) openWheel(name, e.key);
    }
  }, true);
  document.addEventListener('keyup', e => {
    if (chord && e.key === 'Shift') closeChord();  // (letting go of Shift: nothing picked)
    if (pending && !pending.menu && (e.key === pending.key || e.key === 'Tab')) pending = null;  // (released before it opened)
    if (wheel && (e.key === wheel.key || ['Shift', 'Alt'].includes(e.key))) closeWheel(true);
  }, true);

  const renderAll = render;
  return {
    render: () => { renderAll(); renderPaint(); },
    onMouse, openPalette, openKeymap, openChordEditor, onLevelMessage, toggleUvEditor, toggleTrims, toggleWindow, windowOpen, createItems, starterItems,
    setCode: tools => { code = tools || { menus: [] }; winKey = ''; render(); },
    setState: s => { codeState = s || {}; winKey = ''; render(); },
    // tests: the Level window (its place in the view, width, sections, rows), the hover card, the status strip
    probe: () => {
      const v = viewport.getBoundingClientRect(), b = win.getBoundingClientRect();
      const rows = [...win.querySelectorAll('.lw-row')];
      return {
        levelWindowX: win.hidden ? -1 : Math.round(b.left - v.left),
        levelWindowY: win.hidden ? -1 : Math.round(b.top - v.top),
        levelWindowWidth: win.hidden ? 0 : Math.round(b.width),
        levelWindowSections: win.hidden ? '' : [...win.querySelectorAll('.lw-group[data-group]')].map(g => g.dataset.group).join(','),
        levelWindowRows: win.hidden ? 0 : rows.length,
        levelWindowFirst: win.hidden || !rows.length ? '' : rows[0].dataset.command,
        levelWindowOff: win.hidden ? 0 : rows.filter(r => r.classList.contains('off')).length,
        levelWindowIcons: !!winPrefs().icons,
        hoverWhy: card.hidden ? '' : (card.querySelector('.hc-why') || {}).textContent || '',
        hoverName: card.hidden ? '' : (card.querySelector('.hc-name') || {}).textContent || '',
        hoverKey: card.hidden ? '' : (card.querySelector('.hc-key') || {}).textContent || '',
        hoverHelp: !card.hidden && !!card.querySelector('.hc-help'),
        opReadout: strip.hidden || !(state.tools && state.tools.hint) ? '' : strip.textContent,
        statusText: strip.hidden ? '' : [...strip.children].map(n => n.textContent).join(' · '),
        lastOp: (codeState.last && codeState.last.name) || '',
        chordProblems: code.chords ? chordProblems() : 'no chords',
        // the chord menu: open, its path ("Edge › Geometry"), its slots, those greyed, the last why
        ringOpen: !!chord,
        ringPath: chord ? chordPath() : '',
        ringSlots: chord ? chordItems().length : 0,
        ringOff: chord ? chordItems().filter(it => it.why).length : 0,
        ringWhy: chord ? chord.why : '',
        footer: winFoot.hidden ? '' : (winFoot.querySelector('.lw-foot-head, .uv-head') || {}).textContent || '',
        // The mode bar: its mode ("" hidden) and chip.
        modeBar: $('#mode-bar').hidden ? '' : ($('#mode-bar .mode-btn.on') || { dataset: {} }).dataset.element || '',
        modeCount: $('#mode-bar').hidden ? '' : $('#mode-count').textContent,
        // The commands: "ok", or what is wrong (a path twice, a built-in with no section, an icon
        // the editor doesn't have); and each setting's value as "option:<path>/<name>".
        commandsCheck: (() => {
          const seen = new Set();
          for (const c of commands()) {
            if (seen.has(c.path)) return `twice: ${c.path}`;
            seen.add(c.path);
            if (c.builtin && !c.group) return `no section: ${c.path}`;
            if (c.icon && !(c.icon in ICONS)) return `no icon ${c.icon}: ${c.path}`;
          }
          return 'ok';
        })(),
        ...Object.fromEntries(commands().flatMap(c => (c.options || []).map(o => [`option:${c.path}/${o.name}`, optionValue(c, o.name) ?? null]))),
      };
    },
  };
} catch (err) {  // (file:// pages only see "Script error."; the engine's log gets the real one)
  send('ui.error', { message: `leveltools.js: ${err && err.message}`, source: String(err && err.stack) });
  return { render() {}, onMouse() {}, openPalette() {}, openKeymap() {}, setCode() {}, setState() {}, onLevelMessage() {}, toggleUvEditor() {}, toggleTrims() {},
    toggleWindow() {}, windowOpen: () => false, createItems: () => [], starterItems: () => [], probe: () => ({}) };
} })();
