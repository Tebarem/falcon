// Project panel: the asset browser (engine side: engine/editor/asset_browser.h).
//
// Left: Favorites (every scene, model, material, prefab or image in the project, and pinned
// folders), the project's folders, and the open scene's meshes (Engine: shapes, rocks, the
// scene's own). Right: the location's items as a grid of previews or a sortable list, with a
// breadcrumb, search (the whole project) and type filters. A scene is one item: its .fvt, edits
// and terrain go wherever it goes.
// Click, Ctrl / Shift and a rubber band select; the arrows, Home / End and typing move through
// the items; Enter opens, Backspace goes up, F2 renames in place, Delete deletes (to the Recycle
// Bin), Ctrl+D duplicates. Items drag onto folders (in the tree, the grid or the breadcrumb).
// One selected asset shows in the Inspector until an entity is selected.
// Scenes, models and meshes show previews the engine rendered (once: they are cached in the
// project's Cache/Thumbnails); a folder shows a few of its items'.
// A model opens like a folder (its meshes). Meshes and models drag into the scene view (placed
// on what is under the mouse), onto a hierarchy row (as its child) or onto a scatter layer's mesh;
// double-click places one at the view's pivot. Import (and files dropped on the window) copies
// files into the project; after models change, the banner offers to reload the scene (it is
// cooked again with them).
// Materials (.fmat): Create > Material; selected, the Inspector edits it (shown live where it is
// used; textures by dropping images on its slots). Dragged onto a mesh in the view, a hierarchy
// row or a mesh's Material slot, it is assigned.
// Sounds (.fsound: clips with variation, distance and bus; Create > Sound, from selected clips) and
// clips (.wav, .ogg): selected, the Inspector edits or plays one. Dragged into the scene view (or
// onto a hierarchy row), one becomes an entity with an Audio Source; onto an Audio Source's slot,
// it is what plays.
// Library (every project's, project/asset_library.h): Falcon's materials (read-only) and My Library
// (a folder of the user's: made and changed like Assets/, its folder changed from the ⚙). Their
// paths start "@falcon/" and "@library/"; a library material's picture is its colour texture.
// The type filters and Favorites' kinds are the project's.
// The engine rescans about once a second while the tab shows, and sends the list when it changed.
'use strict';

const Assets = (() => {
  let h;  // editor.js helpers (Assets.init)
  const F = { hidden: 1, startup: 2, current: 4, inProject: 8, library: 16, readOnly: 32 };
  const KIND_LABEL = {
    folder: 'Folder', scene: 'Scene', model: 'Model', material: 'Material', prefab: 'Prefab', image: 'Image',
    script: 'Script', data: 'Data', json: 'JSON', project: 'Project', file: 'File', mesh: 'Mesh', sound: 'Sound', clip: 'Audio Clip',
    animgraph: 'Animation Graph',
  };
  const KIND_ICON = {
    folder: 'folder', scene: 'scene', model: 'model', material: 'material', prefab: 'prefab', image: 'image',
    script: 'file', data: 'data', json: 'file', project: 'file', file: 'file', sound: 'audio', clip: 'wave',
    animgraph: 'character',
  };
  const SHAPE_ICONS = new Set(['cube', 'sphere', 'capsule', 'cylinder', 'plane', 'quad', 'cone']);
  const KINDS = [['scene', 'Scenes'], ['model', 'Models'], ['material', 'Materials'], ['prefab', 'Prefabs'], ['image', 'Images'], ['sound', 'Sounds']];
  const isAudio = e => e.kind === 'sound' || e.kind === 'clip';
  const GROUPS = [['shape', 'Shapes'], ['rock', 'Rocks'], ['scene', 'Scene meshes']];  // (model meshes: under their model)
  const LIST_ROW = 22;
  const MAX_IMAGE_REQUESTS = 3;

  const S = {
    root: '', project: false, truncated: false, received: false, stale: false,
    cook: null,  // a scene being cooked before it opens (editor.cook): {scene, stage, done, total, reused, models}
    items: new Map(),     // path -> item
    children: new Map(),  // folder path -> [item]
    meshes: [],           // [{name, group, source}]
    libraries: [],        // [{prefix, name, folder, readOnly}]
    loc: { type: 'folder', path: '' }, back: [], fwd: [],
    search: '', chips: new Set(),
    view: 'grid', size: 64, sort: ['name', 1],
    expanded: new Set(['']), favorites: [], showHidden: false, closedSections: new Set(),
    selection: new Set(), anchor: null, focus: null, renaming: null, paintPending: false,
    images: new Map(), pendingImages: new Set(),
    previews: new Map(),  // preview key ("scene:<path>", "mesh:<name>") -> PNG data URL
    previewsAsked: new Set(), previewsWanted: [],
    materials: new Map(),  // .fmat path -> its values (editor.assets.material)
    acousticPresets: [],   // (with the materials)
    surfacePresets: [],
    sounds: new Map(),     // .fsound path -> {sound, schema, presets} (editor.assets.sound)
    clips: new Map(),      // clip path -> {seconds, channels} or {error} (editor.assets.clip)
    entries: [], inspect: null, visible: false,
    typed: '', typedAt: 0,
  };
  const R = {};
  // The page's CSP (style-src 'self') drops style attributes: styles go through the CSSOM.
  const styled = (node, style) => { Object.assign(node.style, style); return node; };

  // ------------------------------------------------------------------ items

  const baseName = p => p.slice(p.lastIndexOf('/') + 1);
  const parentOf = p => { const i = p.lastIndexOf('/'); return i < 0 ? '' : p.slice(0, i); };
  const stemOf = name => { const i = name.lastIndexOf('.'); return i > 0 ? name.slice(0, i) : name; };
  const isFolder = e => e.kind === 'folder';
  const visibleItem = i => S.showHidden || !(i.flags & F.hidden);
  // Libraries: their paths, whose they are, and what can be changed.
  const inLibrary = p => typeof p === 'string' && p.startsWith('@');
  const libraryOf = p => inLibrary(p) ? S.libraries.find(l => p === l.prefix || p.startsWith(l.prefix + '/')) || null : null;
  const readOnlyPath = p => !!(libraryOf(p) || {}).readOnly;
  const writableFolder = p => inLibrary(p) ? !!libraryOf(p) && !readOnlyPath(p) : S.project;  // (things made in it go there)
  const editablePath = p => inLibrary(p) ? p.includes('/') && writableFolder(p) : S.project;

  function fileEntry(i) {
    const file = baseName(i.path);
    return {
      key: i.path, path: i.path, file, kind: i.kind, size: i.size, modified: i.modified, flags: i.flags,
      name: i.kind === 'folder' ? file : stemOf(file),
    };
  }
  const meshEntry = m => ({ key: 'mesh:' + m.name, name: m.name, kind: 'mesh', mesh: m.name, group: m.group, source: m.source, flags: 0 });
  function entryByKey(key) {
    if (!key) return null;
    if (key.startsWith('mesh:')) {
      const m = S.meshes.find(x => x.name === key.slice(5));
      return m ? meshEntry(m) : null;
    }
    const i = S.items.get(key);
    return i ? fileEntry(i) : null;
  }
  const selectedEntries = () => [...S.selection].map(entryByKey).filter(Boolean);
  const selectedPaths = () => selectedEntries().filter(e => e.path !== undefined).map(e => e.path);

  function iconFor(e) {
    if (e.kind === 'mesh') return SHAPE_ICONS.has(e.mesh) ? e.mesh : e.group === 'rock' ? 'rock' : 'cube';
    return KIND_ICON[e.kind] || 'file';
  }

  function compare(a, b) {
    if (isFolder(a) !== isFolder(b)) return isFolder(a) ? -1 : 1;
    const [key, dir] = S.sort;
    let d = 0;
    if (key === 'kind') d = (KIND_LABEL[a.kind] || '').localeCompare(KIND_LABEL[b.kind] || '');
    else if (key === 'size') d = (a.size || 0) - (b.size || 0);
    else if (key === 'modified') d = (a.modified || 0) - (b.modified || 0);
    if (d === 0) d = a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
    return d * dir;
  }

  // The items to show: the search (the whole project and the scene's meshes), else the location.
  function computeEntries() {
    let list;
    const chip = e => !S.chips.size || S.chips.has(e.kind) || (e.kind === 'mesh' && S.chips.has('model')) || (e.kind === 'clip' && S.chips.has('sound'));
    if (S.search) {
      const q = S.search.toLowerCase();
      list = [...S.items.values()].filter(visibleItem).map(fileEntry)
        .concat(S.meshes.map(meshEntry))
        .filter(e => e.name.toLowerCase().includes(q) && chip(e) && (!S.chips.size || !isFolder(e)));
    } else if (S.loc.type === 'kind') {
      list = [...S.items.values()].filter(i => (i.kind === S.loc.kind || (S.loc.kind === 'sound' && i.kind === 'clip')) && visibleItem(i) &&
        !(i.flags & F.library)).map(fileEntry);
    } else if (S.loc.type === 'engine') {
      list = S.meshes.filter(m => m.group === S.loc.group).map(meshEntry);
    } else if (S.loc.type === 'model') {
      list = S.meshes.filter(m => m.source === S.loc.path).map(meshEntry);
    } else {
      list = (S.children.get(S.loc.path) || []).filter(visibleItem).map(fileEntry)
        .filter(e => !S.chips.size || (!isFolder(e) && chip(e)));
    }
    S.entries = list.sort(compare);
  }

  // ------------------------------------------------------------------ engine messages

  function onList(p) {
    S.received = true;
    const before = S.items;
    S.root = p.root;
    S.project = !!p.project;
    S.truncated = !!p.truncated;
    S.items = new Map();
    S.children = new Map();
    for (const [path, kind, size, modified, flags] of p.items) {
      const item = { path, kind, size, modified, flags };
      S.items.set(path, item);
      if (inLibrary(path) && !path.includes('/')) continue;  // (a library's own folder: the tree's Library section)
      const parent = parentOf(path);
      if (!S.children.has(parent)) S.children.set(parent, []);
      S.children.get(parent).push(item);
      const img = S.images.get(path);
      if (img && img.modified !== modified) S.images.delete(path);  // changed on disk: read again
      const old = before.get(path);
      if (old && old.modified !== modified) {  // its preview too (the engine renders it again if its content changed)
        const key = previewKey(fileEntry(item));
        if (key) { S.previews.delete(key); S.previewsAsked.delete(key); }
      }
    }
    S.meshes = (p.meshes || []).map(([name, group, source]) => ({ name, group, source: source || '' }));
    S.libraries = p.libraries || [];
    S.stale = !!p.stale;
    for (const key of [...S.selection]) if (!entryByKey(key)) S.selection.delete(key);
    if (S.loc.type === 'folder' && S.loc.path && !S.items.has(S.loc.path)) S.loc = { type: 'folder', path: parentOf(S.loc.path) };
    while (S.loc.type === 'folder' && S.loc.path && !S.items.has(S.loc.path)) S.loc.path = parentOf(S.loc.path);
    render();
  }

  // What an operation made: selected (a new folder is renamed in place right away).
  function onDone(p) {
    const made = (p.paths || []).filter(path => S.items.has(path));
    if (!made.length || p.op === 'addToLibrary') return;  // (copies: what is shown stays)
    if (p.op === 'move' && S.loc.type === 'folder' && !made.some(path => parentOf(path) === S.loc.path)) return;
    // Something made elsewhere than the folder shown (a material goes into Assets/): show its folder.
    if (['newFolder', 'newMaterial', 'import'].includes(p.op) && (S.search || S.loc.type !== 'folder' || S.loc.path !== parentOf(made[0])))
      navigate({ type: 'folder', path: parentOf(made[0]) });
    select(made, made[0]);
    reveal(made[0]);
    if (p.op === 'newFolder' || p.op === 'newMaterial') startRename(made[0]);
  }

  function onImage(p) {
    S.pendingImages.delete(p.path);
    const item = S.items.get(p.path);
    S.images.set(p.path, { url: p.url, width: p.width, height: p.height, modified: item ? item.modified : 0 });
    schedulePaint();
    const shown = S.inspect && S.materials.get(S.inspect.key);  // (a material's texture slots show it too)
    if (S.inspect && (S.inspect.key === p.path || (shown && Object.values(shown.textures || {}).includes(p.path)))) h.renderInspector();
  }

  // Rendered previews: scenes and meshes (in a project). Asked for once, as they come into view.
  function previewKey(e) {
    if (!S.project || !e || inLibrary(e.path)) return null;  // (a library's: its pictures are read, not rendered)
    if (e.kind === 'scene') return 'scene:' + e.path;
    if (e.kind === 'mesh') return 'mesh:' + e.mesh;
    if (e.kind === 'model') return 'model:' + e.path;
    if (e.kind === 'material') return 'material:' + e.path;
    if (e.kind === 'prefab') return 'prefab:' + e.path;
    return null;
  }
  // The picture for an item, if there is one yet: its preview, or (images) the image itself.
  function pictureOf(e) {
    const key = previewKey(e);
    if (key) {
      if (S.previews.has(key)) return S.previews.get(key);
      if (!S.previewsAsked.has(key)) { S.previewsAsked.add(key); S.previewsWanted.push(key); }
      return null;
    }
    if (e.kind === 'image' || (e.kind === 'material' && inLibrary(e.path))) {  // (a library material: its colour texture)
      const img = S.images.get(e.path);
      if (img && img.url) return img.url;
      requestImage(e.path);
    }
    return null;
  }
  function flushPreviewRequests() {
    if (!S.previewsWanted.length) return;
    h.send('editor.assets.previews', { keys: S.previewsWanted.splice(0) });
  }
  function onPreview(p) {
    S.previews.set(p.key, p.url);
    schedulePaint();
    if (S.inspect && previewKey(entryByKey(S.inspect.key)) === p.key) h.renderInspector();
  }
  function regenerate(entries) {
    const keys = entries.map(previewKey).filter(Boolean);
    for (const k of keys) { S.previews.delete(k); S.previewsAsked.add(k); }
    if (keys.length) h.send('editor.assets.regenerate', { keys });
    paint();
  }

  function requestImage(path) {
    if (S.images.has(path) || S.pendingImages.has(path) || S.pendingImages.size >= MAX_IMAGE_REQUESTS) return;
    S.pendingImages.add(path);
    h.send('editor.assets.read', { path });
  }

  // ------------------------------------------------------------------ navigation and selection

  const sameLoc = (a, b) => a.type === b.type && a.path === b.path && a.kind === b.kind && a.group === b.group;
  function navigate(loc, push = true) {
    if (S.search) { S.search = ''; R.search.value = ''; }
    if (sameLoc(loc, S.loc)) return render();
    if (push) { S.back.push(S.loc); S.fwd = []; }
    S.loc = loc;
    if (loc.type === 'folder') for (let p = loc.path; p; p = parentOf(p)) S.expanded.add(parentOf(p));
    if (loc.type === 'folder') h.send('editor.assets.folder', { path: loc.path });
    S.selection.clear();
    S.focus = null;
    R.content.scrollTop = 0;
    savePrefs();
    render();
  }
  function goBack() { if (S.back.length) { S.fwd.push(S.loc); navigate(S.back.pop(), false); } }
  function goForward() { if (S.fwd.length) { S.back.push(S.loc); navigate(S.fwd.pop(), false); } }
  function goUp() {
    if (S.loc.type === 'model') navigate({ type: 'folder', path: parentOf(S.loc.path) });
    else if (S.loc.type === 'folder' && S.loc.path) navigate({ type: 'folder', path: parentOf(S.loc.path) });
  }

  function select(keys, focus = keys[keys.length - 1]) {
    S.selection = new Set(keys);
    S.anchor = focus;
    S.focus = focus;
    selectionChanged();
  }
  function toggle(key) {
    if (S.selection.has(key)) S.selection.delete(key); else S.selection.add(key);
    S.anchor = key;
    S.focus = key;
    selectionChanged();
  }
  function range(key) {
    const keys = S.entries.map(e => e.key);
    const a = keys.indexOf(S.anchor), b = keys.indexOf(key);
    if (a < 0 || b < 0) return select([key]);
    S.selection = new Set(keys.slice(Math.min(a, b), Math.max(a, b) + 1));
    S.focus = key;
    selectionChanged();
  }
  function selectionChanged() {
    if (S.selection.size === 1) inspect([...S.selection][0]);
    else if (S.inspect) stopInspecting();
    restyle();
    renderFooter();
  }
  // Selection and focus change the cells' classes only: rebuilding a cell between mousedown and
  // mouseup would lose the click (and the double-click).
  function restyle() {
    for (const node of R.spacer.children) {
      const key = node.dataset.key;
      if (!key) continue;
      node.classList.toggle('selected', S.selection.has(key));
      node.classList.toggle('focus', S.focus === key);
    }
  }

  // ------------------------------------------------------------------ operations

  const currentFolder = () => S.loc.type === 'folder' ? S.loc.path : '';
  const READ_ONLY = 'Falcon\'s library is read-only: use what is in it, or add your own to My Library';
  function newFolder() {
    if (!writableFolder(currentFolder())) return h.setStatus(inLibrary(currentFolder()) ? READ_ONLY : 'Folders can be made in a project only', 'warning');
    if (S.search || S.loc.type !== 'folder') navigate({ type: 'folder', path: currentFolder() });
    h.send('editor.assets.newFolder', { parent: currentFolder() });
  }
  function newMaterial() {  // (in Falcon's library: into the project's Assets/)
    if (!S.project && !writableFolder(currentFolder())) return h.setStatus('Materials are made in a project (or My Library)', 'warning');
    if (S.search || S.loc.type !== 'folder') navigate({ type: 'folder', path: currentFolder() });
    h.send('editor.assets.newMaterial', { folder: currentFolder() });
  }
  // A new .fgraph for a model's skeleton (none given: the scene's first skinned model), opened in
  // the Animator.
  function newGraph(model) {
    if (!S.project) return h.setStatus('Animation graphs are made in a project', 'warning');
    const folder = currentFolder();
    h.send('editor.anim.graph.create', { folder: folder === 'Assets' || folder.startsWith('Assets/') ? folder : 'Assets', model });
  }
  // A new .fsound (its variations: the clips selected, if any).
  function newSound() {
    if (!S.project) return h.setStatus('Sounds are made in a project', 'warning');
    const clips = selectedEntries().filter(e => e.kind === 'clip').map(e => e.path);
    if (S.search || S.loc.type !== 'folder') navigate({ type: 'folder', path: currentFolder() });
    h.send('editor.assets.newSound', { folder: currentFolder(), clips });
  }
  function newScene() {
    if (!S.project) return h.setStatus('Scenes can be made in a project only', 'warning');
    h.cmd.newScene(S.loc.type === 'folder' && S.loc.path ? S.loc.path : 'Scenes');
  }
  function remove() {
    const paths = selectedPaths();
    if (!paths.length || !paths.every(editablePath)) return;
    const names = paths.map(p => baseName(p));
    const scenes = paths.filter(p => S.items.get(p)?.kind === 'scene').length;
    const title = paths.length === 1 ? `Delete ${stemOf(names[0])}?` : `Delete ${paths.length} items?`;
    const text = (scenes ? 'A scene goes with its terrain and edits. ' : '') + 'Deleted files go to the Recycle Bin.';
    h.dialog(title, [el('div', { class: 'modal-text' }, text)], [
      { label: 'Cancel' },
      { label: 'Delete', primary: true, action: () => h.send('editor.assets.delete', { paths }) },
    ]);
  }
  function duplicate() {
    const paths = selectedPaths();
    if (paths.length && paths.every(editablePath)) h.send('editor.assets.duplicate', { paths });
  }
  // Project materials, images, models and clips copied into My Library (a material with its textures).
  const ADDABLE = new Set(['material', 'image', 'model', 'clip']);
  function addToLibrary(paths, folder = '@library') {
    paths = paths.filter(p => !inLibrary(p) && ADDABLE.has((S.items.get(p) || {}).kind));
    if (paths.length) h.send('editor.assets.addToLibrary', { paths, folder });
  }
  function moveItems(paths, to) {
    paths = paths.filter(p => p !== to && !to.startsWith(p + '/') && parentOf(p) !== to);
    if (paths.length) h.send('editor.assets.move', { paths, to });
  }
  function toggleFavorite(path) {
    S.favorites = S.favorites.includes(path) ? S.favorites.filter(p => p !== path) : [...S.favorites, path];
    savePrefs();
    renderTree();
  }

  // Double-click / Enter.
  function open(e) {
    if (!e) return;
    if (isFolder(e)) return navigate({ type: 'folder', path: e.path });
    if (e.kind === 'scene') {
      if (e.flags & F.current) return h.setStatus(`${e.name} is the open scene`);
      if (!(e.flags & F.inProject)) return h.setStatus(`${e.name} is not one of the project's scenes`, 'warning');
      return h.cmd.openScene(e.path);
    }
    if (e.kind === 'model') return navigate({ type: 'model', path: e.path });
    if (e.kind === 'mesh') return place(e);
    if (e.kind === 'animgraph') return Animator.open(e.path);
    inspect(e.key);
  }
  // A library item used in the project: copied into it first (Assets/Falcon/..., Assets/Library/...;
  // a copy made before is used as it is). Resolves to its project path (null: it couldn't be).
  let useId = 0;
  const using = new Map();
  function useInProject(path) {
    if (!inLibrary(path)) return Promise.resolve(path);
    if (!S.project) { h.setStatus('Library items are used in a project', 'warning'); return Promise.resolve(null); }
    const id = ++useId;
    h.send('editor.assets.use', { path, id });
    return new Promise(resolve => using.set(id, resolve));
  }
  function onUsed(p) {
    const resolve = using.get(p.id);
    using.delete(p.id);
    if (resolve) resolve(p.project || null);
  }
  const usingThen = (path, fn) => useInProject(path).then(p => { if (p) fn(p); });

  // Into the scene: at a point of the view (fractions), under a parent, or at the view's pivot.
  function place(e, at = null, parent = 0) {
    if (!e) return;
    if (inLibrary(e.path)) return usingThen(e.path, path => place({ ...e, path }, at, parent));
    const what = e.kind === 'model' ? { model: e.path } : e.kind === 'prefab' ? { prefab: e.path } : e.kind === 'mesh' ? { mesh: e.mesh }
      : isAudio(e) ? { sound: e.path } : null;
    if (!what) return;
    h.send('editor.asset.place', { ...what, ...(at ? { at } : {}), ...(parent ? { parent } : {}) });
  }
  function importFiles() {  // (in Falcon's library: into the project's Assets/)
    if (!S.project && !writableFolder(currentFolder())) return h.setStatus('Files can be imported into a project (or My Library) only', 'warning');
    h.send('editor.assets.import', { folder: currentFolder() });
  }
  function reloadScene() {
    const current = [...S.items.values()].find(i => i.flags & F.current);
    if (current) h.cmd.openScene(current.path);
  }

  // ------------------------------------------------------------------ cooking

  const COOK_STAGES = {
    waiting: ['Waiting for another window cooking it', 0, 0],
    import: ['Reading the models', 0, 5],
    meshes: ['Building meshes', 5, 10],
    textures: ['Encoding textures', 10, 95],
    writing: ['Writing', 95, 100],
    installing: ['Moving it into place', 100, 100],
    done: ['Opening', 100, 100],
  };

  // The engine cooks a scene before opening it (Reload Scene, another scene): progress here;
  // a failure in a dialog.
  function onCook(p) {
    S.cook = p.running ? p : null;
    renderBanner();
    if (p.running) S.cookFailed = false;
    if (!p.running && p.error && p.error !== 'Cancelled') {
      S.cookFailed = true;
      h.dialog(`Cooking ${sceneName(p.scene)} failed`, [el('div', { class: 'modal-text as-cook-error' }, p.error)],
        [{ label: 'OK', primary: true }]);
    }
  }

  const sceneName = path => (path || '').replace(/^.*\//, '').replace(/\.fpak$/, '');

  function renderBanner() {
    if (!R.banner) return;
    const c = S.cook;
    R.banner.hidden = !S.stale && !c;
    R.bannerReload.hidden = !!c;
    R.bannerCancel.hidden = !c;
    R.bannerBar.hidden = !c;
    if (!c) {
      R.bannerText.textContent = 'Models changed since this scene was cooked: new ones are not in it yet.';
      return;
    }
    const [label, from, to] = COOK_STAGES[c.stage] || [c.stage, 0, 100];
    const part = c.total ? Math.min(1, c.done / c.total) : 0;
    const pct = Math.round(from + (to - from) * part);
    let text = `Cooking ${sceneName(c.scene)}: ${c.models} model${c.models === 1 ? '' : 's'} · ${label}`;
    if (c.total && c.stage !== 'waiting') text += ` ${c.done}/${c.total}`;
    if (c.stage === 'textures' && c.reused) text += ` (${c.reused} unchanged)`;
    R.bannerText.textContent = c.stage === 'waiting' ? text : `${text} · ${pct}%`;
    R.bannerFill.style.width = `${c.stage === 'waiting' ? 0 : pct}%`;
  }

  // ------------------------------------------------------------------ rename in place

  function startRename(key) {
    const e = entryByKey(key);
    if (!e || e.kind === 'mesh' || !editablePath(e.path) || (e.flags & F.current)) return;
    if (!S.entries.some(x => x.key === key)) return;  // only what is shown (its field gets the keys)
    S.renaming = key;
    S.focus = key;
    reveal(key);
    paint();
  }
  function renameField(e) {
    const original = e.kind === 'scene' ? e.name : e.file;
    const input = el('input', { class: 'field as-rename', value: original, spellcheck: 'false' });
    let done = false;
    const finish = commit => {
      if (done) return;
      done = true;
      S.renaming = null;
      const name = input.value.trim();
      if (commit && name && name !== original) h.send('editor.assets.rename', { path: e.path, name });
      R.root.focus({ preventScroll: true });
      paint();
    };
    input.addEventListener('keydown', ev => {
      ev.stopPropagation();
      if (ev.key === 'Enter') finish(true);
      if (ev.key === 'Escape') finish(false);
    });
    input.addEventListener('blur', () => finish(true));
    input.addEventListener('mousedown', ev => ev.stopPropagation());
    setTimeout(() => {
      input.focus();
      const dot = e.kind === 'scene' || isFolder(e) ? -1 : original.lastIndexOf('.');
      input.setSelectionRange(0, dot > 0 ? dot : original.length);  // the name, not its extension
    }, 0);
    return input;
  }

  // ------------------------------------------------------------------ layout

  function metrics() {
    const width = Math.max(1, R.content.clientWidth);
    if (S.view === 'list') return { list: true, cols: 1, slot: width, cw: width, ch: LIST_ROW, pad: 0 };
    const pad = 8, cw = S.size + 20, ch = S.size + 30;
    const cols = Math.max(1, Math.floor((width - pad * 2) / cw));
    return { list: false, cols, slot: (width - pad * 2) / cols, cw, ch, pad };
  }
  function cellRect(i, m) {
    const row = Math.floor(i / m.cols), col = i % m.cols;
    if (m.list) return { x: 0, y: row * m.ch, w: m.cw, h: m.ch };
    return { x: m.pad + col * m.slot + (m.slot - m.cw) / 2, y: m.pad + row * m.ch, w: m.cw, h: m.ch };
  }
  function reveal(key) {
    const i = S.entries.findIndex(e => e.key === key);
    if (i < 0) return;
    const r = cellRect(i, metrics());
    const c = R.content;
    if (r.y < c.scrollTop) c.scrollTop = r.y - 4;
    else if (r.y + r.h > c.scrollTop + c.clientHeight) c.scrollTop = r.y + r.h - c.clientHeight + 4;
  }

  // ------------------------------------------------------------------ rendering

  function render() {
    if (!R.root) return;
    computeEntries();
    renderTree();
    renderBar();
    paint();
    renderFooter();
    if (S.inspect) h.renderInspector();
  }

  function schedulePaint() {
    if (S.paintPending) return;
    S.paintPending = true;
    requestAnimationFrame(() => { S.paintPending = false; paint(); });
  }

  // The grid or list: only the rows in view exist (a folder can hold thousands).
  function paint() {
    if (!R.root) return;
    if (S.renaming && R.spacer.querySelector('.as-rename')) return;  // typing: the field stays
    const m = metrics();
    const rows = Math.ceil(S.entries.length / m.cols);
    R.spacer.style.height = `${(m.list ? 0 : m.pad * 2) + rows * m.ch}px`;
    R.head.hidden = !m.list;
    const top = R.content.scrollTop, bottom = top + R.content.clientHeight;
    const first = Math.max(0, Math.floor((top - m.pad) / m.ch) - 2) * m.cols;
    const last = Math.min(S.entries.length, (Math.ceil((bottom - m.pad) / m.ch) + 2) * m.cols);
    const nodes = [];
    for (let i = first; i < last; ++i) nodes.push(m.list ? rowNode(S.entries[i], cellRect(i, m)) : cellNode(S.entries[i], cellRect(i, m)));
    if (!S.entries.length) nodes.push(el('div', { class: 'as-empty' }, emptyText()));
    if (R.band) nodes.push(R.band);
    R.spacer.replaceChildren(...nodes);
    flushPreviewRequests();
    for (const head of R.head.children) {
      const key = head.dataset.sort;
      head.classList.toggle('sorted', S.sort[0] === key);
      head.dataset.dir = S.sort[1] > 0 ? 'up' : 'down';
    }
  }

  function emptyText() {
    if (!S.received) return 'Loading…';
    if (S.search) return `Nothing called "${S.search}".`;
    if (S.loc.type === 'kind') return `No ${KINDS.find(k => k[0] === S.loc.kind)[1].toLowerCase()} in the project yet.`;
    if (S.chips.size) return 'Nothing of these types here.';
    if (inLibrary(S.loc.path)) return readOnlyPath(S.loc.path) ? 'Nothing here.'
      : 'This folder is empty. Right-click to create a folder or a material, or import images and models.';
    return S.project ? 'This folder is empty. Right-click to create a folder or a scene.' : 'Nothing here.';
  }

  function thumbHtml(e) {
    if (isFolder(e)) return folderHtml(e);
    const url = pictureOf(e);
    return url ? `<img src="${url}" alt="">` : icon(iconFor(e));
  }
  // A folder: up to four of its items' pictures, or the folder icon.
  function folderHtml(e) {
    const kids = (S.children.get(e.path) || []).filter(i => visibleItem(i) && i.kind !== 'folder').map(fileEntry)
      .filter(k => previewKey(k) || k.kind === 'image' || (k.kind === 'material' && inLibrary(k.path))).sort(compare).slice(0, 4);
    const urls = kids.map(pictureOf).filter(Boolean);
    if (!urls.length) return icon(iconFor(e));
    return `<div class="as-mosaic n${urls.length}">${urls.map(u => `<img src="${u}" alt="">`).join('')}</div>` +
      `<span class="as-folder-badge">${icon('folder')}</span>`;
  }

  function itemClass(e, base) {
    let c = base;
    if (S.selection.has(e.key)) c += ' selected';
    if (S.focus === e.key) c += ' focus';
    if (e.flags & F.hidden) c += ' dim';
    if (e.flags & F.current) c += ' current';
    return c;
  }
  function itemTitle(e) {
    if (e.kind === 'mesh') return `${e.name}\nMesh (${GROUPS.find(g => g[0] === e.group)[1]})`;
    let t = `${e.path}\n${KIND_LABEL[e.kind]}`;
    if (e.flags & F.readOnly) t += ' · Falcon\'s (used: copied into the project)';
    if (e.kind === 'scene') t += (e.flags & F.current ? ' · open' : '') + (e.flags & F.startup ? ' · startup' : '');
    return t;
  }

  function cellNode(e, r) {
    const html = thumbHtml(e);
    const picture = html.startsWith('<img');
    const thumb = styled(el('div', {
      class: 'as-thumb' + (picture ? (e.kind === 'image' ? ' picture' : ' preview') : '') +
        (!picture && previewKey(e) && !S.previews.has(previewKey(e)) ? ' pending' : ''),
      html }), { width: `${S.size}px`, height: `${S.size}px` });
    if (e.flags & F.startup) thumb.append(el('span', { class: 'as-badge startup', title: 'Startup scene', html: icon('star') }));
    if (e.flags & F.current) thumb.append(el('span', { class: 'as-badge open', title: 'Open scene' }));
    const label = S.renaming === e.key ? renameField(e) : el('div', { class: 'as-label' }, e.name);
    const node = styled(el('div', {
      class: itemClass(e, 'as-cell'), 'data-key': e.key, title: S.renaming === e.key ? null : itemTitle(e),
    }, thumb, label), { left: `${r.x}px`, top: `${r.y}px`, width: `${r.w}px` });
    if (isFolder(e) && !readOnlyPath(e.path)) node.dataset.drop = e.path;
    return node;
  }

  function rowNode(e, r) {
    const name = S.renaming === e.key ? renameField(e) : el('span', { class: 'as-name' }, e.name);
    const node = el('div', {
      class: itemClass(e, 'as-row'), 'data-key': e.key, title: S.renaming === e.key ? null : itemTitle(e),
    },
      el('span', { class: 'as-rname' }, el('span', { class: 'as-ricon', html: icon(iconFor(e)) }), name,
        e.flags & F.startup ? el('span', { class: 'as-rbadge', html: icon('star'), title: 'Startup scene' }) : null),
      el('span', {}, KIND_LABEL[e.kind] || ''),
      el('span', { class: 'num' }, isFolder(e) || e.kind === 'mesh' ? '' : formatSize(e.size)),
      el('span', {}, e.modified ? formatDate(e.modified) : ''));
    node.style.top = `${r.y}px`;
    if (isFolder(e) && !readOnlyPath(e.path)) node.dataset.drop = e.path;
    return node;
  }

  const fmtSeconds = t => t < 60 ? `${t.toFixed(t < 10 ? 2 : 1)} s` : `${Math.floor(t / 60)}:${String(Math.floor(t % 60)).padStart(2, '0')}`;
  function formatSize(n) {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(n < 10240 ? 1 : 0)} KB`;
    if (n < 1024 ** 3) return `${(n / 1024 / 1024).toFixed(1)} MB`;
    return `${(n / 1024 ** 3).toFixed(2)} GB`;
  }
  function formatDate(seconds) {
    const d = new Date(seconds * 1000);
    const pad = v => String(v).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }

  function projectName() {
    const info = h.getState().projectInfo;
    return S.project ? (info && info.name) || 'Project' : 'Scene folder';
  }

  function renderTree() {
    const rows = [];
    let open = true;  // the current section's rows show
    const head = (id, text, button = null) => {
      open = !S.closedSections.has(id);
      rows.push(el('div', { class: 'as-thead' + (open ? '' : ' closed'), role: 'button', 'data-section': id, html: icon('chev'),
        onmousedown: ev => {
          if (ev.button !== 0 || ev.target.closest('.as-thead-btn')) return;
          if (S.closedSections.has(id)) S.closedSections.delete(id); else S.closedSections.add(id);
          savePrefs();
          renderTree();
        } }, text, button));
    };
    const push = node => { if (open) rows.push(node); };
    const row = ({ label, iconName, active, depth = 0, arrow = null, drop = null, title = null, data = {}, onclick }) => {
      const node = styled(el('div', { class: 'as-trow' + (active ? ' selected' : ''), title, ...data },
        el('span', { class: 'arrow', html: arrow === null ? '' : icon(arrow ? 'chev' : 'chevRight') }),
        el('span', { class: 'ico', html: icon(iconName) }), el('span', { class: 'name' }, label)), { paddingLeft: `${6 + depth * 12}px` });
      if (drop !== null) node.dataset.drop = drop;
      node.addEventListener('mousedown', ev => {
        if (ev.button !== 0) return;
        if (ev.target.closest('.arrow') && arrow !== null) return;
        onclick();
      });
      return node;
    };
    head('favorites', 'Favorites');
    for (const [kind, label] of KINDS) {
      push(row({ label, iconName: 'star', active: !S.search && S.loc.type === 'kind' && S.loc.kind === kind,
        data: { 'data-fav': kind }, onclick: () => navigate({ type: 'kind', kind }) }));
    }
    for (const path of S.favorites.filter(p => S.items.has(p))) {
      push(row({ label: baseName(path), iconName: 'folder', title: path, drop: path, data: { 'data-tree': path },
        active: !S.search && S.loc.type === 'folder' && S.loc.path === path, onclick: () => navigate({ type: 'folder', path }) }));
    }
    head('project', S.project ? 'Project' : 'Files');
    const addFolder = (path, depth, lib = null) => {
      const kids = (S.children.get(path) || []).filter(i => i.kind === 'folder' && visibleItem(i))
        .sort((a, b) => baseName(a.path).localeCompare(baseName(b.path), undefined, { numeric: true, sensitivity: 'base' }));
      const expanded = S.expanded.has(path);
      const active = !S.search && S.loc.type === 'folder' && S.loc.path === path;
      const node = row({
        label: lib ? lib.name : path ? baseName(path) : projectName(), iconName: expanded && kids.length ? 'folderOpen' : 'folder', depth,
        arrow: kids.length ? expanded : null, drop: readOnlyPath(path) ? null : path, title: lib ? lib.folder : path || S.root, active,
        data: { 'data-tree': path }, onclick: () => navigate({ type: 'folder', path }),
      });
      const arrow = node.querySelector('.arrow');
      arrow.addEventListener('mousedown', ev => {
        if (ev.button !== 0 || !kids.length) return;
        if (S.expanded.has(path)) S.expanded.delete(path); else S.expanded.add(path);
        savePrefs();
        renderTree();
      });
      push(node);
      if (expanded) for (const k of kids) addFolder(k.path, depth + 1);
    };
    addFolder('', 0);
    if (S.libraries.length) {
      const gear = el('span', { class: 'tbtn as-thead-btn', role: 'button', title: 'My Library\'s folder', 'data-as': 'libraryFolder', html: icon('gear'),
        onclick: ev => h.openMenu(libraryMenu(), ev.clientX, ev.clientY) });
      head('library', 'Library', gear);
      for (const lib of S.libraries) addFolder(lib.prefix, 0, lib);
    }
    const groups = GROUPS.filter(([g]) => S.meshes.some(m => m.group === g));
    if (groups.length) {
      head('engine', 'Engine');
      for (const [group, label] of groups) {
        push(row({ label, iconName: group === 'shape' ? 'cube' : group === 'rock' ? 'rock' : 'model', data: { 'data-engine': group },
          active: !S.search && S.loc.type === 'engine' && S.loc.group === group, onclick: () => navigate({ type: 'engine', group }) }));
      }
    }
    R.tree.replaceChildren(...rows);
  }

  // The breadcrumb, back / forward and the chips.
  function renderBar() {
    R.back.classList.toggle('disabled', !S.back.length);
    R.fwd.classList.toggle('disabled', !S.fwd.length);
    const crumbs = [];
    const crumb = (label, onclick, drop = null) => {
      const node = el('span', { class: 'as-crumb', role: 'button', onclick }, label);
      if (drop !== null) node.dataset.drop = drop;
      return node;
    };
    const sep = () => el('span', { class: 'as-sep', html: icon('chevRight') });
    if (S.search) {
      crumbs.push(el('span', { class: 'as-crumb static' }, `Search: "${S.search}"`));
    } else if (S.loc.type === 'kind') {
      crumbs.push(crumb('Favorites', () => {}), sep(), el('span', { class: 'as-crumb static' }, KINDS.find(k => k[0] === S.loc.kind)[1]));
    } else if (S.loc.type === 'engine') {
      crumbs.push(el('span', { class: 'as-crumb static' }, 'Engine'), sep(), el('span', { class: 'as-crumb static' }, GROUPS.find(g => g[0] === S.loc.group)[1]));
    } else {
      // The project's (or a library's) folders down to the location; a model's: its name last.
      const folder = S.loc.type === 'model' ? parentOf(S.loc.path) : S.loc.path;
      const lib = libraryOf(folder);
      const top = lib ? lib.prefix : '';
      const drop = path => readOnlyPath(path) ? null : path;
      crumbs.push(crumb(lib ? lib.name : projectName(), () => navigate({ type: 'folder', path: top }), drop(top)));
      let acc = top;
      const rest = lib ? folder.slice(top.length + 1) : folder;
      for (const part of rest ? rest.split('/') : []) {
        acc = acc ? `${acc}/${part}` : part;
        const path = acc;
        crumbs.push(sep(), crumb(part, () => navigate({ type: 'folder', path }), drop(path)));
      }
      if (S.loc.type === 'model') crumbs.push(sep(), el('span', { class: 'as-crumb static' }, stemOf(baseName(S.loc.path))));
    }
    R.crumbs.replaceChildren(...crumbs);
    for (const chip of R.chips.children) chip.classList.toggle('on', S.chips.has(chip.dataset.kind));
    R.gridBtn.classList.toggle('on', S.view === 'grid');
    R.listBtn.classList.toggle('on', S.view === 'list');
    R.sizeSlider.hidden = S.view !== 'grid';
  }

  function renderFooter() {
    const sel = selectedEntries();
    let text;
    if (sel.length === 1) {
      const e = sel[0];
      text = e.kind === 'mesh' ? `${e.name} · Mesh` : `${e.path} · ${KIND_LABEL[e.kind]}${isFolder(e) ? '' : ' · ' + formatSize(e.size)}`;
    } else if (sel.length > 1) {
      text = `${sel.length} selected`;
    } else {
      text = `${S.entries.length} item${S.entries.length === 1 ? '' : 's'}`;
    }
    R.footText.textContent = text;
    R.footWarn.hidden = !S.truncated;
    renderBanner();
  }

  // ------------------------------------------------------------------ menus

  function itemMenu(ev) {
    const sel = selectedEntries();
    if (!sel.length) return emptyMenu(ev);
    const files = sel.filter(e => e.path !== undefined);
    const one = sel.length === 1 ? sel[0] : null;
    const editable = files.length > 0 && files.every(e => editablePath(e.path)) && !files.some(e => e.flags & F.current);
    const addable = S.project && files.filter(e => !inLibrary(e.path) && ADDABLE.has(e.kind));
    const items = [];
    if (one && isFolder(one)) items.push({ label: 'Open', icon: 'folderOpen', shortcut: 'Enter', action: () => open(one) });
    if (one && one.kind === 'scene') {
      items.push({ label: 'Open Scene', icon: 'scene', shortcut: 'Enter', disabled: !!(one.flags & F.current) || !(one.flags & F.inProject), action: () => open(one) });
      items.push({ label: 'Set as Startup Scene', icon: 'star', disabled: !!(one.flags & F.startup) || !(one.flags & F.inProject),
        action: () => h.send('editor.project.setStartup', { scene: one.path }) });
    }
    if (one && one.kind === 'model') items.push({ label: 'Open', icon: 'folderOpen', shortcut: 'Enter', action: () => open(one) });
    if (one && one.kind === 'model') items.push({ label: 'Create Animation Graph', icon: 'character', disabled: !S.project, action: () => newGraph(one.path) });
    if (one && (one.kind === 'mesh' || one.kind === 'model' || one.kind === 'prefab' || isAudio(one))) items.push({ label: 'Place in Scene', icon: 'plus', shortcut: one.kind === 'mesh' ? 'Enter' : '', action: () => place(one) });
    if (one && isAudio(one)) items.push({ label: 'Play', icon: 'play', action: () => h.send('editor.audio.play', { path: one.path }) });
    if (sel.some(e => e.kind === 'clip')) items.push({ label: 'Create Sound from Clips', icon: 'audio', disabled: !S.project, action: newSound });
    const previewable = sel.filter(e => previewKey(e));
    if (previewable.length) items.push({ label: 'Regenerate Preview', icon: 'refresh', action: () => regenerate(previewable) });
    if (addable && addable.length && S.libraries.some(l => !l.readOnly))
      items.push({ label: 'Add to Library', icon: 'plus', title: 'Copy into My Library (a material with its textures), for every project',
        action: () => addToLibrary(addable.map(e => e.path)) });
    if (files.length) {
      if (items.length) items.push('-');
      items.push({ label: 'Rename', shortcut: 'F2', disabled: !one || !editable, action: () => startRename(one.key) });
      items.push({ label: 'Duplicate', shortcut: 'Ctrl+D', disabled: !editable, action: duplicate });
      items.push({ label: 'Delete…', shortcut: 'Delete', disabled: !editable, action: remove });
      items.push('-');
      items.push({ label: 'Show in Explorer', icon: 'reveal', disabled: !one, action: () => h.send('editor.assets.reveal', { path: one.path }) });
      items.push({ label: 'Copy Path', icon: 'copy', disabled: !one, action: () => h.send('editor.assets.copyPath', { path: one.path }) });
      if (one && isFolder(one)) {
        items.push('-');
        items.push({ label: S.favorites.includes(one.path) ? 'Remove from Favorites' : 'Add to Favorites', icon: 'star', action: () => toggleFavorite(one.path) });
      }
    }
    h.openMenu(items, ev.clientX, ev.clientY);
  }

  function createItems() {
    return [
      { label: 'Folder', icon: 'folder', action: newFolder, disabled: !writableFolder(currentFolder()) },
      { label: 'Scene…', icon: 'scene', action: newScene, disabled: !S.project },
      { label: 'Material', icon: 'material', action: newMaterial, disabled: !S.project && !writableFolder(currentFolder()) },
      { label: 'Sound', icon: 'audio', action: newSound, disabled: !S.project },
      { label: 'Animation Graph', icon: 'character', action: () => newGraph(''), disabled: !S.project },
      { label: 'Prefab (from the selection)', icon: 'prefab', disabled: !S.project || !h.getState().selectedEntity,
        action: () => h.send('editor.prefab.create', { id: h.getState().selectedEntity, folder: currentFolder() }) },
    ];
  }

  function emptyMenu(ev) {
    const folder = currentFolder();
    h.openMenu([
      { label: 'Create', icon: 'plus', submenu: createItems() },
      '-',
      { label: 'Import…', icon: 'import', disabled: !S.project && !writableFolder(folder), action: importFiles },
      { label: 'Show in Explorer', icon: 'reveal', action: () => h.send('editor.assets.reveal', { path: folder }) },
      { label: 'Show Hidden Files', checked: S.showHidden, action: () => { S.showHidden = !S.showHidden; savePrefs(); render(); } },
      { label: 'Refresh', icon: 'refresh', action: () => h.send('editor.assets.refresh') },
    ], ev.clientX, ev.clientY);
  }
  // The Library section's ⚙.
  function libraryMenu() {
    const mine = S.libraries.find(l => !l.readOnly);
    return [
      { label: 'Change My Library Folder…', icon: 'folderOpen', action: () => h.send('editor.assets.libraryFolder', {}) },
      { label: 'Show My Library in Explorer', icon: 'reveal', disabled: !mine, action: () => h.send('editor.assets.reveal', { path: mine.prefix }) },
    ];
  }

  // ------------------------------------------------------------------ mouse

  // Dragging items onto a folder moves them there (Escape cancels).
  function startDrag(down, key) {
    const entry = entryByKey(key);
    if (!entry) return;
    const placeable = entry.kind === 'mesh' || entry.kind === 'model' || entry.kind === 'prefab' || isAudio(entry);
    const material = entry.kind === 'material', image = entry.kind === 'image', audio = isAudio(entry);
    if (!placeable && (entry.path === undefined || (!S.project && !inLibrary(entry.path)))) return;
    const d = { active: false, paths: null, target: null, scene: null };
    const highlight = node => {
      if (d.target === node) return;
      if (d.target) d.target.classList.remove('drop');
      d.target = node;
      if (node) node.classList.add('drop');
    };
    const onMove = ev => {
      if (!d.active) {
        if (Math.hypot(ev.clientX - down.clientX, ev.clientY - down.clientY) < 5) return;
        d.active = true;
        d.paths = entry.path === undefined ? [] : S.selection.has(key) ? selectedPaths() : [key];
        const n = Math.max(1, d.paths.length);
        const url = pictureOf(entry);
        R.ghost = el('div', { class: 'as-ghost' }, el('span', { class: 'as-ghost-pic', html: url ? `<img src="${url}" alt="">` : icon(iconFor(entry)) }),
          el('span', {}, n === 1 ? entry.name : `${n} items`));
        document.body.append(R.ghost);
      }
      R.ghost.style.left = `${ev.clientX + 12}px`;
      R.ghost.style.top = `${ev.clientY + 10}px`;
      const under = document.elementFromPoint(ev.clientX, ev.clientY);
      d.scene = null;
      // Materials: a mesh in the view, a hierarchy row, a Material slot. Images: a texture slot.
      if ((material || image) && under) {
        const palette = material ? under.closest('[data-palette-drop]') : null;  // (the level tools' material palette)
        if (palette) { d.scene = { palette }; highlight(palette); R.ghost.classList.add('into-scene'); return; }
        const vp = under.closest('#viewport');
        const row = under.closest('#tree .row');
        const slot = under.closest(material ? '[data-material-slot], [data-material-drop]' : '[data-texture-slot]');
        R.ghost.classList.toggle('into-scene', !!(vp || row || slot));
        if (material && vp && !under.closest('.overlay')) {
          const r = vp.getBoundingClientRect();
          d.scene = { assign: { at: { x: (ev.clientX - r.left) / r.width, y: (ev.clientY - r.top) / r.height } } };
          highlight(null);
          return;
        }
        if (material && row && row.dataset.id) { d.scene = { assign: { id: +row.dataset.id } }; highlight(row); return; }
        if (slot) { d.scene = { slot }; highlight(slot); return; }
      }
      // Sounds and clips: an Audio Source's slot.
      if (audio && under) {
        const slot = under.closest('[data-sound-slot]');
        if (slot) { d.scene = { soundSlot: slot }; highlight(slot); R.ghost.classList.add('into-scene'); return; }
      }
      // Meshes, models and sounds: the scene view, a hierarchy row; meshes: a scatter layer's mesh.
      if (placeable && under) {
        const vp = under.closest('#viewport');
        const row = under.closest('#tree .row');
        const pick = entry.kind === 'mesh' ? under.closest('#terrain select') : null;
        const layer = entry.kind !== 'prefab' ? under.closest('[data-scatter-drop]') : null;
        const shelf = entry.kind === 'prefab' ? under.closest('[data-prefab-drop]') : null;  // (the level tools' prefab shelf)
        if (shelf) { d.scene = { palette: shelf }; highlight(shelf); R.ghost.classList.add('into-scene'); return; }
        if (vp && !under.closest('.overlay')) {
          const r = vp.getBoundingClientRect();
          d.scene = { at: { x: (ev.clientX - r.left) / r.width, y: (ev.clientY - r.top) / r.height } };
          highlight(null);
          R.ghost.classList.add('into-scene');
          return;
        }
        if (row && row.dataset.id) { d.scene = { parent: +row.dataset.id }; highlight(row); R.ghost.classList.remove('into-scene'); return; }
        if (pick && [...pick.options].some(o => o.value === entry.mesh)) { d.scene = { select: pick }; highlight(pick); return; }
        if (layer) { d.scene = { layer }; highlight(layer); return; }
      }
      R.ghost.classList.remove('into-scene');
      const drop = d.paths.length ? under && under.closest('[data-drop]') : null;
      const to = drop ? drop.dataset.drop : null;
      // (Items move within the project or one library: My Library's copies come from Add to Library.)
      const valid = to !== null && d.paths.every(p => p !== to && !to.startsWith(p + '/') && editablePath(p) &&
        (libraryOf(p) || null) === (libraryOf(to) || null)) && d.paths.some(p => parentOf(p) !== to);
      d.adding = to !== null && S.project && writableFolder(to) && inLibrary(to) &&
        d.paths.every(p => !inLibrary(p) && ADDABLE.has((S.items.get(p) || {}).kind));  // (a project's onto My Library: Add to Library)
      highlight(valid || d.adding ? drop : null);
    };
    const finish = commit => {
      document.removeEventListener('mousemove', onMove, true);
      document.removeEventListener('mouseup', onUp, true);
      document.removeEventListener('keydown', onKey, true);
      if (R.ghost) { R.ghost.remove(); R.ghost = null; }
      const target = d.target;
      highlight(null);
      // (A library's item: its project copy, made first.)
      if (commit && d.active && d.scene && d.scene.palette) {
        const palette = d.scene.palette;
        usingThen(entry.path, p => palette.dispatchEvent(new CustomEvent('palette-drop', { detail: p })));
      } else if (commit && d.active && d.scene && d.scene.assign) {
        const assign = d.scene.assign;
        usingThen(entry.path, p => h.send('editor.asset.assign', { material: p, ...assign }));
      } else if (commit && d.active && d.scene && d.scene.slot) {
        const slot = d.scene.slot;
        if (material && slot.hasAttribute('data-material-drop')) usingThen(entry.path, p => slot.dispatchEvent(new CustomEvent('material-drop', { detail: p })));
        else if (material) usingThen(entry.path, p => h.send('editor.entity.material', { id: +slot.dataset.materialSlot, material: p }));
        else pickTexture(slot.dataset.texturePath, slot.dataset.textureSlot, entry.path);
      } else if (commit && d.active && d.scene && d.scene.soundSlot) {
        const slot = d.scene.soundSlot;
        usingThen(entry.path, p => h.send('editor.audio.source.set', { id: +slot.dataset.soundSlot, sound: p }));
      } else if (commit && d.active && d.scene && d.scene.layer) {
        // Onto a scatter layer: the meshes (the selected ones, a model's) join it.
        const entries = S.selection.has(key) ? [...S.selection].map(entryByKey).filter(Boolean) : [entry];
        const meshes = entries.flatMap(e => e.kind === 'mesh' ? [e.mesh] : e.kind === 'model' ? S.meshes.filter(m => m.source === e.path).map(m => m.name) : []);
        d.scene.layer.dispatchEvent(new CustomEvent('scatter-drop', { detail: { meshes } }));
      } else if (commit && d.active && d.scene && d.scene.select) {
        d.scene.select.value = entry.mesh;
        d.scene.select.dispatchEvent(new Event('change', { bubbles: true }));
      } else if (commit && d.active && d.scene) place(entry, d.scene.at || null, d.scene.parent || 0);
      else if (commit && d.active && target && d.adding) addToLibrary(d.paths, target.dataset.drop);
      else if (commit && d.active && target) moveItems(d.paths, target.dataset.drop);
      // A click on one of several selected items selects just it (once it is clear it was no drag).
      else if (commit && !d.active && S.selection.size > 1 && S.selection.has(key) && !down.ctrlKey && !down.shiftKey) select([key]);
    };
    const onUp = () => finish(true);
    const onKey = ev => { if (ev.key === 'Escape') { ev.stopPropagation(); ev.preventDefault(); finish(false); } };
    document.addEventListener('mousemove', onMove, true);
    document.addEventListener('mouseup', onUp, true);
    document.addEventListener('keydown', onKey, true);
  }

  // A rubber band from empty space selects what it touches (Ctrl adds to the selection).
  function startBand(down) {
    const box = R.content.getBoundingClientRect();
    const point = ev => ({ x: ev.clientX - box.left + R.content.scrollLeft, y: ev.clientY - box.top + R.content.scrollTop });
    const start = point(down);
    const base = down.ctrlKey ? new Set(S.selection) : new Set();
    if (!down.ctrlKey) select([]);
    R.band = el('div', { class: 'as-band' });
    let moved = false;
    const move = ev => {
      const p = point(ev);
      const x0 = Math.min(start.x, p.x), y0 = Math.min(start.y, p.y), x1 = Math.max(start.x, p.x), y1 = Math.max(start.y, p.y);
      if (!moved && x1 - x0 + y1 - y0 < 4) return;
      moved = true;
      Object.assign(R.band.style, { left: `${x0}px`, top: `${y0}px`, width: `${x1 - x0}px`, height: `${y1 - y0}px` });
      const m = metrics();
      const keys = new Set(base);
      S.entries.forEach((e, i) => {
        const r = cellRect(i, m);
        if (r.x < x1 && r.x + r.w > x0 && r.y < y1 && r.y + r.h > y0) keys.add(e.key);
      });
      S.selection = keys;
      S.focus = null;
      restyle();
      renderFooter();
    };
    const up = () => {
      document.removeEventListener('mousemove', move, true);
      document.removeEventListener('mouseup', up, true);
      R.band = null;
      if (S.selection.size === 1) inspect([...S.selection][0]);
      paint();
    };
    document.addEventListener('mousemove', move, true);
    document.addEventListener('mouseup', up, true);
  }

  function onContentDown(ev) {
    if (ev.target.closest('.as-rename')) return;
    R.root.focus({ preventScroll: true });
    const cell = ev.target.closest('[data-key]');
    if (!cell) {
      if (ev.button === 0) startBand(ev);
      else if (ev.button === 2 && !ev.ctrlKey) select([]);
      return;
    }
    const key = cell.dataset.key;
    if (ev.button === 2) {
      if (!S.selection.has(key)) select([key]);
      return;
    }
    if (ev.button !== 0) return;
    if (ev.ctrlKey) toggle(key);
    else if (ev.shiftKey) range(key);
    else if (!S.selection.has(key)) select([key]);
    else { S.focus = key; S.anchor = key; if (S.selection.size === 1) inspect(key); restyle(); }
    startDrag(ev, key);
  }

  // ------------------------------------------------------------------ keyboard

  function key(e, name) {
    if (S.renaming) return false;
    const list = S.entries;
    const m = metrics();
    let i = list.findIndex(x => x.key === S.focus);
    const go = j => {
      if (!list.length) return;
      j = Math.max(0, Math.min(list.length - 1, j));
      const k = list[j].key;
      if (e.shiftKey) range(k); else select([k]);
      S.focus = k;
      reveal(k);
      restyle();
    };
    const base = name.replace('Shift+', '');
    if (i < 0 && ['ArrowRight', 'ArrowLeft', 'ArrowDown', 'ArrowUp'].includes(base)) i = -1;
    switch (base) {
      case 'ArrowRight': if (!m.list) go(i + 1); return !m.list;
      case 'ArrowLeft': if (!m.list) go(Math.max(0, i - 1)); return !m.list;
      case 'ArrowDown': go(i < 0 ? 0 : i + m.cols); return true;
      case 'ArrowUp': go(i < 0 ? 0 : i - m.cols); return true;
      case 'Home': go(0); return true;
      case 'End': go(list.length - 1); return true;
      case 'PageDown': go(i + m.cols * Math.max(1, Math.floor(R.content.clientHeight / m.ch) - 1)); return true;
      case 'PageUp': go(i - m.cols * Math.max(1, Math.floor(R.content.clientHeight / m.ch) - 1)); return true;
      default: break;
    }
    switch (name) {
      case 'Enter': open(entryByKey(S.focus) || selectedEntries()[0]); return true;
      case 'Backspace': case 'Alt+ArrowUp': goUp(); return true;
      case 'Alt+ArrowLeft': goBack(); return true;
      case 'Alt+ArrowRight': goForward(); return true;
      case 'F2': if (S.focus || S.selection.size) startRename(S.focus || [...S.selection][0]); return true;
      case 'Delete': remove(); return true;
      case 'Ctrl+D': duplicate(); return true;
      case 'Ctrl+A': select(list.map(x => x.key), S.focus); return true;
      case 'Ctrl+F': R.search.focus(); R.search.select(); return true;
      case 'Ctrl+Shift+N': newFolder(); return true;
      case 'Escape': if (S.selection.size) { select([]); return true; } return false;
      default: break;
    }
    // Typing jumps to the next item whose name starts with what was typed.
    if (e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey && e.key !== ' ') {
      const now = performance.now();
      S.typed = now - S.typedAt > 800 ? e.key.toLowerCase() : S.typed + e.key.toLowerCase();
      S.typedAt = now;
      const start = S.typed.length === 1 ? i + 1 : Math.max(0, i);
      for (let n = 0; n < list.length; ++n) {
        const j = (start + n) % list.length;
        if (list[j].name.toLowerCase().startsWith(S.typed)) { go(j); break; }
      }
      return true;
    }
    return false;
  }

  // ------------------------------------------------------------------ inspector

  function inspect(key) {
    S.inspect = { key, since: h.getState().selectedEntity };
    h.renderInspector();
  }
  function stopInspecting() {
    if (!S.inspect) return;
    S.inspect = null;
    h.renderInspector();
  }
  // The asset in the Inspector, until an entity is selected.
  function inspecting() {
    if (!S.inspect) return null;
    const e = entryByKey(S.inspect.key);
    if (!e || h.getState().selectedEntity !== S.inspect.since) { S.inspect = null; return null; }
    return e;
  }

  // ---- materials

  function onMaterial(p) {
    S.materials.set(p.path, p.material);
    if (p.acousticPresets) S.acousticPresets = p.acousticPresets;
    if (p.surfacePresets) S.surfacePresets = p.surfacePresets;
    if (S.inspect && S.inspect.key === p.path) h.renderInspector();
  }
  let sendTimer = 0, pending = {}, previewTimer = 0;
  // A change: kept here, sent at most every 60 ms; the preview renders again once edits stop.
  function setMaterial(path, values) {
    const m = S.materials.get(path);
    if (m) Object.assign(m, values);
    pending[path] = Object.assign(pending[path] || {}, values);
    if (!sendTimer) sendTimer = setTimeout(() => {
      sendTimer = 0;
      for (const [p, v] of Object.entries(pending)) h.send('editor.material.set', { path: p, values: v });
      pending = {};
    }, 60);
    clearTimeout(previewTimer);
    previewTimer = setTimeout(() => regenerate([fileEntry(S.items.get(path) || { path, kind: 'material' })]), 1500);
  }
  // The displacement amounts: written as they change (a moment after the last), shown after a cook.
  let displaceTimer = 0, displaceNoted = false;
  function displaceChanged(path, values) {
    clearTimeout(displaceTimer);
    displaceTimer = setTimeout(() => {
      setMaterial(path, values);
      if (!displaceNoted) h.setStatus('Height displacement is cooked into the meshes that use the material: save and Reload Scene to see it');
      displaceNoted = true;
    }, 250);
  }
  // An image for a material's slot: a library material keeps a library image (a project's is copied
  // beside it by the engine); a project material uses a library image's project copy.
  function pickTexture(path, slot, image) {
    if (inLibrary(path) || !inLibrary(image)) return setTexture(path, slot, image);
    usingThen(image, p => setTexture(path, slot, p));
  }
  function setTexture(path, slot, image) {
    const m = S.materials.get(path);
    if (!m) return;
    m.textures = Object.assign({}, m.textures, { [slot]: image });
    setMaterial(path, { textures: m.textures });
    h.renderInspector();
    if (slot === 'height') h.setStatus('Height displacement is cooked into the meshes that use the material: save and Reload Scene to see it');
    else if (image) h.setStatus('The texture shows once the scene is cooked again (the panel offers Reload Scene)');
  }
  function materialEditor(e) {
    const m = S.materials.get(e.path);
    if (!m) {
      h.send('editor.assets.readMaterial', { path: e.path });
      return [el('div', { class: 'empty' }, 'Loading…')];
    }
    if (readOnlyPath(e.path)) return readOnlyMaterial(e, m);
    const rows = [];
    const color = (label, key, alpha) => el('div', { class: 'prop' }, el('label', {}, label),
      h.colorField(m[key].slice(0, 3), v => setMaterial(e.path, { [key]: alpha ? [...v, m[key][3]] : v }),
        v => setMaterial(e.path, { [key]: alpha ? [...v, m[key][3]] : v })));
    const slider = (label, key, min, max, step, digits = 2) =>
      h.valueSlider(label, () => m[key], v => setMaterial(e.path, { [key]: v }), min, max, step, digits);
    rows.push(color('Base Color', 'baseColor', true));
    rows.push(h.valueSlider('Opacity', () => m.baseColor[3], v => setMaterial(e.path, { baseColor: [...m.baseColor.slice(0, 3), v] }), 0, 1, 0.01));
    rows.push(slider('Metallic', 'metallic', 0, 1, 0.01));
    rows.push(slider('Roughness', 'roughness', 0, 1, 0.01));
    // (the roughness map's black: above 0 the material is rougher than its map, its variation kept)
    const roughMin = slider('Roughness Min', 'roughnessMin', 0, 1, 0.01);
    roughMin.title = 'The roughness where the roughness map is black (its white gives Roughness): raise it to make a material rougher than its map';
    rows.push(roughMin);
    rows.push(color('Emission', 'emissive', false));
    rows.push(slider('Emission Strength', 'emissiveStrength', 0, 50, 0.1, 1));
    rows.push(slider('Normal Strength', 'normalScale', 0, 2, 0.01));
    rows.push(slider('Occlusion', 'occlusionStrength', 0, 1, 0.01));
    rows.push(h.choice('Alpha', [['opaque', 'Opaque'], ['mask', 'Cutout'], ['blend', 'Transparent']], m.alpha,
      v => { setMaterial(e.path, { alpha: v }); h.renderInspector(); }));
    if (m.alpha === 'mask') rows.push(slider('Cutoff', 'alphaCutoff', 0, 1, 0.01));
    if (m.alpha === 'blend') {  // (T2) what is seen through it, bent and (by the roughness) blurred
      rows.push(slider('Refraction', 'refraction', 0, 1, 0.01));
      rows.push(slider('IOR', 'ior', 1, 3, 0.01));
    }
    if (m.shader) {  // (V1) a surface shader's vertex function: how far it moves, and within what distance it runs
      rows.push(slider('Vertex Bound', 'vertexBound', 0, 20, 0.05));
      rows.push(slider('Vertex Distance', 'vertexDistance', 0, 1000, 1, 0));
    }
    const sided = el('input', { type: 'checkbox', class: 'check', checked: !!m.doubleSided });
    sided.addEventListener('change', () => setMaterial(e.path, { doubleSided: sided.checked }));
    rows.push(el('div', { class: 'prop' }, el('label', {}, 'Double Sided'), sided));
    // How much of the world one repeat covers: level faces with world or box UVs take it as their scale.
    for (const [k, label] of [[0, 'Size X (m)'], [1, 'Size Y (m)']])
      rows.push(h.tip(h.valueSlider(label, () => (m.size || [0, 0])[k], v => { const s = [...(m.size || [0, 0])]; s[k] = v; setMaterial(e.path, { size: s }); },
        0, 20, 0.05, 2), 'The real-world size one repeat of its textures covers (0: not said); level faces with world or box UVs take it when it is put on them'));
    // Texture slots: an image dropped on one (or picked from the menu): the project's, a library's.
    const images = [...S.items.values()].filter(i => i.kind === 'image' && !(i.flags & F.library)).map(i => i.path).sort();
    const libraryImages = [...S.items.values()].filter(i => i.kind === 'image' && (i.flags & F.library)).map(i => i.path).sort();
    const slots = [['baseColor', 'Base Color'], ['normal', 'Normal'], ['metallicRoughness', 'Metallic / Rough'], ['occlusion', 'Occlusion'],
      ['height', 'Height']];
    const textures = slots.map(([key, label]) => {
      const path = (m.textures || {})[key] || '';
      const img = path ? S.images.get(path) : null;
      if (path && !img) requestImage(path);
      const thumb = el('span', { class: 'as-tex-thumb', html: img && img.url ? `<img src="${img.url}" alt="">` : icon('image') });
      const name = el('span', { class: 'as-tex-name' }, path ? baseName(path) : 'None (drop an image)');
      const clear = path ? el('span', { class: 'tbtn', role: 'button', title: 'Remove', html: icon('close'),
        onclick: ev => { ev.stopPropagation(); setTexture(e.path, key, ''); } }) : null;
      const slot = el('div', { class: 'as-tex-slot', role: 'button', 'data-texture-slot': key, 'data-texture-path': e.path,
        title: path || 'Drop an image from the Project panel, or click to pick one' }, thumb, name, clear);
      const imageItem = p => ({ label: inLibrary(p) ? `${baseName(parentOf(p))}/${baseName(p)}` : baseName(p), icon: 'image', checked: p === path,
        title: p, action: () => pickTexture(e.path, key, p) });
      slot.addEventListener('click', ev => h.openMenu([
        ...images.map(imageItem),
        ...(libraryImages.length ? [{ label: 'Library', icon: 'folder', submenu: libraryImages.map(imageItem) }] : []),
        ...(images.length || libraryImages.length ? ['-'] : []),
        { label: 'None', disabled: !path, action: () => setTexture(e.path, key, '') },
      ], ev.clientX, ev.clientY));
      return el('div', { class: 'prop' }, el('label', {}, label), slot);
    });
    // Height displacement: real geometry, cooked into the meshes that use the material (with a
    // Reload Scene), so the amounts are saved as they change but show after the next cook.
    const cooked = (label, key, min, max, step, digits, title, open = null) => {
      const row = h.valueSlider(label, () => m[key] || 0, v => { m[key] = v; displaceChanged(e.path, { [key]: v }); }, min, max, step, digits, open);
      row.setAttribute('title', title);
      return row;
    };
    const height = (m.textures || {}).height;
    const displacement = [
      h.tip(h.valueSlider('Parallax (m)', () => m.parallax || 0, v => setMaterial(e.path, { parallax: v }), 0, 0.2, 0.001, 3, [0, Infinity]),
        'How deep the height map\'s black lies under the surface, drawn by the view (no geometry; 0: none). Bricks about 0.02, cobbles 0.04. ' +
        'Type or drag the label past the slider for more. With Amount too, it adds relief finer than the cooked geometry'),
      cooked('Amount (m)', 'displacement', -1, 1, 0.005, 3,
        'How far white in the height map pushes the surface out along its normal (negative: in). Type or drag the label past the slider for more',
        [-Infinity, Infinity]),
      cooked('Mid Level', 'heightCenter', 0, 1, 0.01, 2, 'The height that stays on the surface: 0 pushes everything out, 0.5 pushes dark in and light out'),
      cooked('Detail (m)', 'displacementDetail', 0, 0.5, 0.001, 3, 'Triangle size the meshes are subdivided to (0: about one texel of the height map). Smaller: finer and heavier'),
      el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {}, height
        ? 'Real geometry, cooked into the meshes that use this material: changes show after saving and Reload Scene'
        : 'Drop a grey height map on the Height slot above (white is high)')),
    ];
    // Acoustics: heard at once where the material is used (not cooked).
    const acoustic = m.acoustic ? h.acousticRows(m.acoustic, S.acousticPresets, fields => {
      Object.assign(m.acoustic, fields);
      if (!fields.preset) m.acoustic.preset = '';
      // A preset picked while its surface sounds are still generic: they follow (concrete sounds like concrete).
      if (fields.preset && m.surface && (m.surface.preset || 'generic') === 'generic' && S.surfacePresets.some(p => p.value === fields.preset))
        setSurface(e.path, m, { preset: fields.preset, ...surfaceCharacter(fields.preset) });
      setMaterial(e.path, { acoustic: { ...m.acoustic } });
      if (fields.preset !== undefined) h.renderInspector();
    }) : [];
    const surface = m.surface ? surfaceRows(e.path, m) : [];
    return [el('div', { class: 'as-ins-props' }, rows), el('div', { class: 'as-ins-sub' }, 'Textures'), el('div', { class: 'as-ins-props' }, textures),
      el('div', { class: 'as-ins-sub' }, 'Height'), el('div', { class: 'as-ins-props', 'data-displacement': '' }, displacement),
      el('div', { class: 'as-ins-sub' }, 'Acoustics'), el('div', { class: 'as-ins-props', 'data-acoustics': '' }, acoustic),
      el('div', { class: 'as-ins-sub' }, 'Surface Sounds'), el('div', { class: 'as-ins-props', 'data-surface-sounds': '' }, surface)];
  }

  // A Falcon material: what it is, not edited (a project uses its copy, which can be).
  function readOnlyMaterial(e, m) {
    const prop = (label, value) => el('div', { class: 'prop' }, el('label', {}, label), el('span', { class: 'as-value' }, value));
    const size = m.size || [0, 0];
    const rows = [prop('Size', size[0] > 0 ? `${size[0]} × ${size[1]} m` : 'Not said')];
    for (const [key, label] of [['baseColor', 'Base Color'], ['normal', 'Normal'], ['metallicRoughness', 'Metallic / Rough'], ['occlusion', 'Occlusion']]) {
      const path = (m.textures || {})[key] || '';
      const img = path ? S.images.get(path) : null;
      if (path && !img) requestImage(path);
      rows.push(el('div', { class: 'prop' }, el('label', {}, label), el('div', { class: 'as-tex-slot readonly', title: path },
        el('span', { class: 'as-tex-thumb', html: img && img.url ? `<img src="${img.url}" alt="">` : icon('image') }),
        el('span', { class: 'as-tex-name' }, path ? baseName(path) : 'None'))));
    }
    rows.push(el('div', { class: 'prop hint' }, el('label', {}, ''), el('span', {},
      'Falcon\'s materials are read-only. Drag one onto a face or a mesh (or pick it in a material slot): it is copied into the project\'s Assets/Falcon, where it can be changed.')));
    return [el('div', { class: 'as-ins-props', 'data-library-material': '' }, rows)];
  }

  // ---- sounds

  function onSound(p) {
    S.sounds.set(p.path, { sound: p.sound, schema: p.schema || [], presets: p.presets || [] });
    if (S.inspect && S.inspect.key === p.path) h.renderInspector();
  }
  function onClip(p) {
    S.clips.set(p.path, p);
    if (S.inspect && S.inspect.key === p.path) h.renderInspector();
  }
  let soundTimer = 0, soundPending = {};
  function setSound(path, values) {
    const s = S.sounds.get(path);
    if (s) Object.assign(s.sound, values);
    soundPending[path] = Object.assign(soundPending[path] || {}, values);
    clearTimeout(soundTimer);
    soundTimer = setTimeout(() => {
      for (const [p, v] of Object.entries(soundPending)) h.send('editor.sound.set', { path: p, values: v });
      soundPending = {};
    }, 120);
  }
  const playButtons = path => el('div', { class: 'row-buttons' },
    h.actionButton('\u25b6 Play', () => h.send('editor.audio.play', { path }), 'primary'),
    h.actionButton('\u25a0 Stop', () => h.send('editor.audio.stop')));
  // A .fsound: its clips (dropped from the Project panel or picked), then its fields by group.
  function soundEditor(e) {
    const entry = S.sounds.get(e.path);
    if (!entry) {
      h.send('editor.assets.readSound', { path: e.path });
      return [el('div', { class: 'empty' }, 'Loading…')];
    }
    const s = entry.sound;
    const clipsAll = [...S.items.values()].filter(i => i.kind === 'clip').map(i => i.path).sort();
    const clips = (s.clips || []).map((path, i) => el('div', { class: 'as-tex-slot', title: path },
      el('span', { class: 'as-tex-thumb', html: icon('wave') }), el('span', { class: 'as-tex-name' }, baseName(path)),
      el('span', { class: 'tbtn', role: 'button', title: 'Play', html: icon('play'),
        onclick: ev => { ev.stopPropagation(); h.send('editor.audio.play', { path }); } }),
      el('span', { class: 'tbtn', role: 'button', title: 'Remove', html: icon('close'),
        onclick: ev => { ev.stopPropagation(); setSound(e.path, { clips: s.clips.filter((_, k) => k !== i) }); h.renderInspector(); } })));
    const add = el('div', { class: 'as-tex-slot', role: 'button', 'data-sound-clips': e.path, title: 'Add a clip of the project' },
      el('span', { class: 'as-tex-thumb', html: icon('plus') }), el('span', { class: 'as-tex-name' }, clipsAll.length ? 'Add a clip…' : 'Import .wav or .ogg clips first'));
    add.addEventListener('click', ev => h.openMenu(clipsAll.length ? clipsAll.map(p => ({ label: baseName(p), icon: 'wave',
      action: () => { setSound(e.path, { clips: [...(s.clips || []), p] }); h.renderInspector(); } })) : [{ label: 'No clips in the project', disabled: true }],
      ev.clientX, ev.clientY));
    const groups = new Map();
    for (const f of entry.schema) {
      if (!groups.has(f.group)) groups.set(f.group, []);
      let row;
      if (f.kind === 'number') {
        const digits = f.step >= 1 ? 0 : f.step >= 0.1 ? 1 : f.step >= 0.01 ? 2 : 3;
        row = h.valueSlider(f.label, () => s[f.key], v => setSound(e.path, { [f.key]: v }), f.min, Math.min(f.max, f.key.endsWith('Distance') ? 200 : f.max), f.step, digits);
      } else if (f.kind === 'bool') {
        const box = el('input', { type: 'checkbox', class: 'check', checked: !!s[f.key], 'data-sound-field': f.key });
        box.addEventListener('change', () => setSound(e.path, { [f.key]: box.checked }));
        row = el('div', { class: 'prop' }, el('label', {}, f.label), box);
      } else {
        row = h.choice(f.label, f.choices.map(c => [c, c[0].toUpperCase() + c.slice(1)]), s[f.key], v => setSound(e.path, { [f.key]: v }));
      }
      groups.get(f.group).push(f.tip ? h.tip(row, f.tip) : row);
    }
    const presets = h.choice('Preset', [['', '(none)'], ...entry.presets.map(p => [p.value, p.label])], s.preset || '',
      v => { if (v) { h.send('editor.sound.set', { path: e.path, values: { preset: v } }); h.send('editor.assets.readSound', { path: e.path }); } });
    const out = [el('div', { class: 'as-ins-props' }, el('div', { class: 'prop' }, el('label', {}, 'Preview'), playButtons(e.path)), presets),
      el('div', { class: 'as-ins-sub' }, 'Clips'), el('div', { class: 'as-ins-props', 'data-sound-clip-list': '' }, clips, add)];
    for (const [group, rows] of groups) out.push(el('div', { class: 'as-ins-sub' }, group), el('div', { class: 'as-ins-props' }, rows));
    return out;
  }

  // An Audio Source's Sound slot (the Inspector): a sound or clip dropped on it, or picked.
  function soundSlot(ins) {
    const path = ins.audioSource.sound || '';
    const slot = el('div', { class: 'as-mat-slot', role: 'button', 'data-sound-slot': String(ins.id),
      title: 'Drop a sound or clip from the Project panel, or click to pick one' },
      el('span', { class: 'as-tex-thumb', html: icon(path.endsWith('.fsound') ? 'audio' : path ? 'wave' : 'audio') }),
      el('span', { class: 'as-tex-name' }, path ? stemOf(baseName(path)) : 'None'));
    const sounds = [...S.items.values()].filter(i => i.kind === 'sound' || i.kind === 'clip').map(i => i.path).sort();
    slot.addEventListener('click', ev => h.openMenu([
      { label: 'None', checked: !path, action: () => h.send('editor.audio.source.set', { id: ins.id, sound: '' }) },
      ...(sounds.length ? ['-'] : []),
      ...sounds.map(p => ({ label: stemOf(baseName(p)), icon: p.endsWith('.fsound') ? 'audio' : 'wave', checked: p === path,
        action: () => h.send('editor.audio.source.set', { id: ins.id, sound: p }) })),
      '-',
      { label: 'New Sound', icon: 'plus', disabled: !S.project, action: () => h.send('editor.assets.newSound', { folder: 'Assets' }) },
    ], ev.clientX, ev.clientY));
    const actions = path && S.items.has(path) ? [h.actionButton('Edit', () => select([path]))] : [];
    return [el('div', { class: 'prop' }, el('label', {}, 'Sound'), slot), ...(actions.length ? [el('div', { class: 'as-ins-actions' }, actions)] : [])];
  }

  // A sound slot (a water material's sounds): a sound or clip of the project picked, or none (`none`
  // says what that means, e.g. synthesized).
  function soundPicker(path, pick, none = 'None') {
    const slot = el('div', { class: 'as-mat-slot', role: 'button', title: path || none },
      el('span', { class: 'as-tex-thumb', html: icon(path ? (path.endsWith('.fsound') ? 'audio' : 'wave') : 'dice') }),
      el('span', { class: 'as-tex-name' }, path ? stemOf(baseName(path)) : none),
      ...(path ? [el('span', { class: 'tbtn', role: 'button', title: 'Play', html: icon('play'),
        onclick: ev => { ev.stopPropagation(); h.send('editor.audio.play', { path }); } })] : []));
    const sounds = [...S.items.values()].filter(i => i.kind === 'sound' || i.kind === 'clip').map(i => i.path).sort();
    slot.addEventListener('click', ev => h.openMenu([
      { label: none, checked: !path, action: () => pick('') },
      ...(sounds.length ? ['-'] : []),
      ...sounds.map(p => ({ label: stemOf(baseName(p)), icon: p.endsWith('.fsound') ? 'audio' : 'wave', checked: p === path, action: () => pick(p) })),
    ], ev.clientX, ev.clientY));
    return slot;
  }

  // ---- surface sounds (a material's: physics impacts, scrapes and rolls)

  const SURFACE_CHARACTER = ['hardness', 'roughness', 'frequency', 'decay', 'noise'];
  function surfaceCharacter(preset) {
    const p = S.surfacePresets.find(x => x.value === preset);
    return p ? Object.fromEntries(SURFACE_CHARACTER.map(k => [k, p[k]])) : {};
  }
  function setSurface(path, m, fields) {
    Object.assign(m.surface, fields);
    if (fields.preset === undefined && SURFACE_CHARACTER.some(k => k in fields)) m.surface.preset = '';
    setMaterial(path, { surface: { ...m.surface } });
  }
  function surfaceRows(path, m) {
    const s = m.surface;
    const set = fields => setSurface(path, m, fields);
    const sounds = [...S.items.values()].filter(i => i.kind === 'sound' || i.kind === 'clip').map(i => i.path).sort();
    const slot = (label, key, text) => {
      const p = s[key] || '';
      const node = el('div', { class: 'as-tex-slot', role: 'button', 'data-surface-slot': key, title: p || 'Synthesized (pick a sound or clip of the project)' },
        el('span', { class: 'as-tex-thumb', html: icon(p ? (p.endsWith('.fsound') ? 'audio' : 'wave') : 'dice') }),
        el('span', { class: 'as-tex-name' }, p ? baseName(p) : (s.synth ? 'Synthesized' : 'None')),
        ...(p ? [el('span', { class: 'tbtn', role: 'button', title: 'Play', html: icon('play'),
          onclick: ev => { ev.stopPropagation(); h.send('editor.audio.play', { path: p }); } })] : []));
      node.addEventListener('click', ev => h.openMenu([
        { label: s.synth ? 'Synthesized' : 'None', checked: !p, action: () => { set({ [key]: '' }); h.renderInspector(); } },
        ...(sounds.length ? ['-'] : []),
        ...sounds.map(x => ({ label: baseName(x), icon: x.endsWith('.fsound') ? 'audio' : 'wave', checked: x === p,
          action: () => { set({ [key]: x }); h.renderInspector(); } })),
      ], ev.clientX, ev.clientY));
      return h.tip(el('div', { class: 'prop' }, el('label', {}, label), node), text);
    };
    const slider = (label, key, min, max, step, digits, text) =>
      h.tip(h.valueSlider(label, () => s[key], v => set({ [key]: v }), min, max, step, digits), text);
    const synth = el('input', { type: 'checkbox', class: 'check', checked: !!s.synth });
    synth.addEventListener('change', () => { set({ synth: synth.checked }); h.renderInspector(); });
    return [
      h.tip(h.choice('Preset', [['', '(its own)'], ...S.surfacePresets.map(p => [p.value, p.label])], s.preset || '',
        v => { if (v) { set({ preset: v, ...surfaceCharacter(v) }); h.renderInspector(); } }), 'What it sounds like hit, scraped or rolled on'),
      slot('Impact', 'impact', 'Something hits it (a .fsound with variations is best)'),
      slot('Scrape', 'scrape', 'Something slides along it (a loop)'),
      slot('Roll', 'roll', 'Something rolls on it (a loop)'),
      h.tip(el('div', { class: 'prop' }, el('label', {}, 'Synthesize'), synth), 'Without a sound set: synthesized from the character below (else silent)'),
      slider('Volume', 'gain', 0, 4, 0.01, 2, 'Its physics sounds\' level'),
      slider('Pitch', 'pitch', 0.25, 4, 0.01, 2, 'Their pitch (2: an octave up)'),
      slider('Hardness', 'hardness', 0, 1, 0.01, 2, 'Soft (sand, cloth: a thud) to hard (metal, glass: a ring); also how much it lets the other side ring'),
      slider('Roughness', 'roughness', 0, 1, 0.01, 2, 'A scrape\'s grain: smooth (glass) to coarse (gravel)'),
      slider('Ring Pitch (Hz)', 'frequency', 40, 4000, 1, 0, 'The synthesized ringing\'s base pitch'),
      slider('Ring Time (s)', 'decay', 0.005, 2, 0.005, 3, 'How long the synthesized hit rings'),
      slider('Noise', 'noise', 0, 1, 0.01, 2, 'How much of a hit is noise (dirt, gravel) rather than tone (metal, glass)'),
    ];
  }

  // A mesh entity's Material slot (the Inspector): a material dropped on it, or picked.
  function materialSlot(ins) {
    const path = ins.render.material || '';
    const item = path ? S.items.get(path) : null;
    const url = item ? pictureOf(fileEntry(item)) : null;
    flushPreviewRequests();
    const slot = el('div', { class: 'as-mat-slot', role: 'button', 'data-material-slot': String(ins.id),
      title: 'Drop a material from the Project panel, or click to pick one' },
      el('span', { class: 'as-tex-thumb', html: url ? `<img src="${url}" alt="">` : icon('material') }),
      el('span', { class: 'as-tex-name' }, path ? stemOf(baseName(path)) : `${ins.render.mesh || 'Mesh'}'s own`));
    const pick = p => h.send('editor.entity.material', { id: ins.id, material: p });
    slot.addEventListener('click', ev => h.openMenu([
      { label: 'Its own (the mesh\'s)', checked: !path, action: () => pick('') },
      ...materialItems(path, pick),
      '-',
      { label: 'New Material', icon: 'plus', disabled: !S.project, action: () => h.send('editor.assets.newMaterial', { folder: 'Assets' }) },
    ], ev.clientX, ev.clientY));
    const actions = path ? [h.actionButton('Edit', () => { const it = S.items.get(path); if (it) { select([path]); } })] : [];
    return [el('div', { class: 'prop' }, el('label', {}, 'Material'), slot), ...(actions.length ? [el('div', { class: 'as-ins-actions' }, actions)] : [])];
  }

  // A material slot that calls `onPick(path)` ("" none): click for the project's materials, or drop
  // one from the Project panel onto it ([data-material-drop]: its 'material-drop' event). `attrs` go
  // on the slot.
  function materialPick(path, onPick, attrs = {}, noneLabel = 'None') {
    const item = path ? S.items.get(path) : null;
    const url = item ? pictureOf(fileEntry(item)) : null;
    flushPreviewRequests();
    const slot = el('div', { class: 'as-mat-slot', role: 'button', 'data-material-drop': '', ...attrs },
      el('span', { class: 'as-tex-thumb', html: url ? `<img src="${url}" alt="">` : icon('material') }),
      el('span', { class: 'as-tex-name' + (path ? '' : ' muted') }, path ? stemOf(baseName(path)) : noneLabel));
    slot.addEventListener('click', ev => h.openMenu([
      { label: noneLabel, checked: !path, value: '', action: () => onPick('') },
      ...materialItems(path, onPick),
    ], ev.clientX, ev.clientY));
    slot.addEventListener('material-drop', ev => onPick(ev.detail));
    return slot;
  }
  // The material picker's menu at the mouse on its own (an "Add material" button): `onPick(path)`.
  function materialMenu(ev, onPick) {
    const items = materialItems('', onPick);
    while (items[0] === '-') items.shift();
    h.openMenu(items.length ? items : [{ label: 'No materials yet', disabled: true }], ev.clientX, ev.clientY);
  }
  // A picker's materials: the project's, then each library's (a submenu by folder; picked, one is
  // copied into the project by the engine).
  function materialItems(current, pick) {
    const all = [...S.items.values()].filter(i => i.kind === 'material').map(i => i.path).sort();
    const item = p => ({ label: stemOf(baseName(p)), icon: 'material', checked: p === current, value: p, action: () => usingThen(p, pick) });
    const project = all.filter(p => !inLibrary(p));
    const libs = [];
    for (const lib of S.libraries) {
      const mine = all.filter(p => p.startsWith(lib.prefix + '/'));
      if (!mine.length) continue;
      const folders = new Map();  // (by their folder in the library)
      for (const p of mine) {
        const f = parentOf(p) === lib.prefix ? '' : parentOf(p).slice(lib.prefix.length + 1);
        if (!folders.has(f)) folders.set(f, []);
        folders.get(f).push(p);
      }
      const submenu = [...folders].sort((a, b) => a[0].localeCompare(b[0])).flatMap(([f, ps]) =>
        f ? [{ label: f, icon: 'folder', submenu: ps.map(item) }] : ps.map(item));
      libs.push({ label: lib.name, icon: 'folder', submenu });
    }
    return [...(project.length ? ['-', ...project.map(item)] : []), ...(libs.length ? ['-', ...libs] : [])];
  }

  function inspectorNodes() {
    const e = inspecting();
    if (!e) return null;
    const prop = (label, value) => el('div', { class: 'prop' }, el('label', {}, label), el('span', { class: 'as-value', title: String(value) }, value));
    const img = e.kind === 'image' ? S.images.get(e.path) : null;
    const url = isFolder(e) ? null : pictureOf(e);
    flushPreviewRequests();
    const preview = el('div', { class: 'as-preview', html: url ? `<img src="${url}" alt="">` : icon(iconFor(e)) });
    const header = el('div', { class: 'as-ins-head' }, preview,
      el('div', { class: 'as-ins-title' }, el('div', { class: 'as-ins-name' }, e.name), el('div', { class: 'muted' }, KIND_LABEL[e.kind] || '')));
    const rows = [];
    if (e.kind === 'mesh') {
      rows.push(prop('Mesh', e.mesh), prop('Group', GROUPS.find(g => g[0] === e.group)[1]));
    } else {
      rows.push(prop('Path', e.path || '/'), prop('Type', KIND_LABEL[e.kind] || e.kind));
      if (!isFolder(e)) rows.push(prop('Size', formatSize(e.size)));
      else rows.push(prop('Contains', `${(S.children.get(e.path) || []).filter(visibleItem).length} items`));
      if (e.modified) rows.push(prop('Modified', formatDate(e.modified)));
      if (e.kind === 'scene') {
        rows.push(prop('In the project', e.flags & F.inProject ? 'Yes' : 'No'));
        rows.push(prop('Startup scene', e.flags & F.startup ? 'Yes' : 'No'));
        if (e.flags & F.current) rows.push(prop('Open', 'This scene is open'));
      }
      if (img) rows.push(prop('Pixels', `${img.width} × ${img.height}`));
      if (e.kind === 'clip') {
        const c = S.clips.get(e.path);
        if (!c) h.send('editor.assets.readClip', { path: e.path });
        else if (c.error) rows.push(prop('Problem', c.error));
        else rows.push(prop('Length', `${fmtSeconds(c.seconds)}`), prop('Channels', c.channels === 1 ? 'Mono' : 'Stereo'));
        rows.push(el('div', { class: 'prop' }, el('label', {}, 'Preview'), playButtons(e.path)));
      }
    }
    const actions = [];
    if (isFolder(e)) actions.push(h.actionButton('Open', () => open(e)));
    if (e.kind === 'scene' && !(e.flags & F.current) && (e.flags & F.inProject)) actions.push(h.actionButton('Open Scene', () => open(e), 'primary'));
    if (e.kind === 'scene' && !(e.flags & F.startup) && (e.flags & F.inProject)) {
      actions.push(h.actionButton('Set as Startup', () => h.send('editor.project.setStartup', { scene: e.path })));
    }
    if (e.kind === 'mesh' || e.kind === 'model' || e.kind === 'prefab' || isAudio(e)) actions.push(h.actionButton('Place in Scene', () => place(e), e.kind === 'sound' ? '' : 'primary'));
    if (e.kind === 'clip') actions.push(h.actionButton('Create Sound', () => { select([e.path]); newSound(); }));
    if (e.path !== undefined) {
      actions.push(h.actionButton('Show in Explorer', () => h.send('editor.assets.reveal', { path: e.path })));
      actions.push(h.actionButton('Copy Path', () => h.send('editor.assets.copyPath', { path: e.path })));
    }
    if (e.kind === 'material') return [header, ...materialEditor(e), el('div', { class: 'as-ins-actions' }, actions)];
    if (e.kind === 'sound') return [header, ...soundEditor(e), el('div', { class: 'as-ins-actions' }, actions)];
    return [header, el('div', { class: 'as-ins-props' }, rows), el('div', { class: 'as-ins-actions' }, actions)];
  }

  // ------------------------------------------------------------------ prefs

  let saveTimer = 0;
  function savePrefs() {
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => h.store.set('assets', {
      folder: S.loc.type === 'folder' ? S.loc.path : '', view: S.view, size: S.size, sort: S.sort,
      expanded: [...S.expanded], favorites: S.favorites, showHidden: S.showHidden, closedSections: [...S.closedSections],
    }), 250);
  }
  function applyPrefs() {
    const p = h.store.get('assets', {});
    if (typeof p.folder === 'string') S.loc = { type: 'folder', path: p.folder };
    if (p.view === 'grid' || p.view === 'list') S.view = p.view;
    if (Number.isFinite(p.size)) S.size = Math.max(40, Math.min(160, p.size));
    if (Array.isArray(p.sort) && p.sort.length === 2) S.sort = p.sort;
    if (Array.isArray(p.expanded)) S.expanded = new Set(['', ...p.expanded]);
    if (Array.isArray(p.favorites)) S.favorites = p.favorites;
    S.showHidden = !!p.showHidden;
    if (Array.isArray(p.closedSections)) S.closedSections = new Set(p.closedSections);
    if (R.sizeSlider) {
      R.sizeSlider.value = S.size;
      R.sizeSlider.style.setProperty('--pct', `${(S.size - 40) / 1.2}%`);
    }
    render();
  }

  // ------------------------------------------------------------------ build

  let el, icon;
  function build(root) {
    R.root = root;
    const tbtn = (iconName, title, onclick, name) => el('span', { class: 'tbtn', role: 'button', title, html: icon(iconName), onclick, 'data-as': name });
    R.back = tbtn('chevLeft', 'Back (Alt+Left)', goBack, 'back');
    R.fwd = tbtn('chevRight', 'Forward (Alt+Right)', goForward, 'forward');
    R.crumbs = el('div', { class: 'as-crumbs' });
    R.search = el('input', { class: 'field', placeholder: 'Search the project and libraries', autocomplete: 'off', spellcheck: 'false' });
    R.search.addEventListener('input', () => { S.search = R.search.value.trim(); S.selection.clear(); render(); });
    R.search.addEventListener('keydown', ev => {
      if (ev.key === 'Escape') { R.search.value = ''; S.search = ''; render(); R.root.focus(); }
      if (ev.key === 'ArrowDown' || ev.key === 'Enter') { ev.preventDefault(); R.root.focus(); if (S.entries.length) select([S.entries[0].key]); }
    });
    const search = el('label', { class: 'search as-search' }, el('span', { html: icon('search') }), R.search);
    R.chips = el('div', { class: 'as-chips' }, KINDS.map(([kind, label]) => el('span', {
      class: 'as-chip', role: 'button', 'data-kind': kind, title: `Show only ${label.toLowerCase()}`,
      onclick: () => { if (S.chips.has(kind)) S.chips.delete(kind); else S.chips.add(kind); render(); },
    }, label)));
    R.gridBtn = tbtn('grid', 'Grid', () => { S.view = 'grid'; savePrefs(); render(); }, 'grid');
    R.listBtn = tbtn('list', 'List', () => { S.view = 'list'; savePrefs(); render(); }, 'list');
    R.sizeSlider = el('input', { type: 'range', class: 'as-size', min: '40', max: '160', step: '4', value: String(S.size), title: 'Preview size' });
    const sliderFill = () => R.sizeSlider.style.setProperty('--pct', `${(S.size - 40) / 1.2}%`);
    R.sizeSlider.addEventListener('input', () => { S.size = Number(R.sizeSlider.value); sliderFill(); savePrefs(); paint(); });
    sliderFill();
    const create = el('span', { class: 'tbtn', role: 'button', title: 'Create', html: icon('plus') + icon('chev', 'chev'), 'data-as': 'create' });
    const importBtn = tbtn('import', 'Import… (or drop files on the window)', importFiles, 'import');
    create.addEventListener('click', () => h.menuBelow(create, createItems()));
    const bar = el('div', { class: 'toolbar as-bar' }, create, importBtn, el('span', { class: 'divider' }), R.back, R.fwd, R.crumbs,
      el('span', { class: 'spacer' }), search, R.chips, el('span', { class: 'divider' }), R.gridBtn, R.listBtn, R.sizeSlider);

    R.tree = el('div', { class: 'as-tree' });
    R.head = el('div', { class: 'as-listhead' }, [['name', 'Name'], ['kind', 'Type'], ['size', 'Size'], ['modified', 'Modified']].map(([k, label]) =>
      el('span', { 'data-sort': k, role: 'button', class: k === 'size' ? 'num' : '', onclick: () => {
        S.sort = S.sort[0] === k ? [k, -S.sort[1]] : [k, 1];
        savePrefs();
        render();
      } }, label)));
    R.spacer = el('div', { class: 'as-spacer' });
    R.content = el('div', { class: 'as-content' }, R.spacer);
    R.bannerText = el('span', { class: 'as-banner-text' });
    R.bannerFill = el('div', { class: 'as-progress-fill' });
    R.bannerBar = el('div', { class: 'as-progress', hidden: true }, R.bannerFill);
    R.bannerReload = el('span', { class: 'tbtn wide primary', role: 'button', 'data-as': 'reload', onclick: () => reloadScene() }, 'Reload Scene');
    R.bannerCancel = el('span', { class: 'tbtn wide', role: 'button', 'data-as': 'cancelCook', hidden: true,
      onclick: () => h.send('editor.project.cancelCook') }, 'Cancel');
    R.banner = el('div', { class: 'as-banner', hidden: true },
      el('span', { html: icon('warning') }), R.bannerText, R.bannerBar, R.bannerReload, R.bannerCancel);
    R.content.addEventListener('mousedown', onContentDown);
    R.content.addEventListener('dblclick', ev => {
      const cell = ev.target.closest('[data-key]');
      if (cell && !ev.target.closest('.as-rename')) open(entryByKey(cell.dataset.key));
    });
    R.content.addEventListener('contextmenu', ev => {
      ev.preventDefault();
      if (ev.target.closest('[data-key]')) itemMenu(ev); else emptyMenu(ev);
    });
    R.content.addEventListener('scroll', schedulePaint);
    new ResizeObserver(schedulePaint).observe(R.content);
    R.tree.addEventListener('contextmenu', ev => {
      ev.preventDefault();
      const row = ev.target.closest('[data-tree]');
      if (!row) return;
      const path = row.dataset.tree;
      h.openMenu([
        { label: 'Open', icon: 'folderOpen', action: () => navigate({ type: 'folder', path }) },
        { label: 'Create Folder', icon: 'folder', disabled: !writableFolder(path), action: () => { navigate({ type: 'folder', path }); newFolder(); } },
        '-',
        { label: 'Show in Explorer', icon: 'reveal', action: () => h.send('editor.assets.reveal', { path }) },
        { label: path && S.favorites.includes(path) ? 'Remove from Favorites' : 'Add to Favorites', icon: 'star', disabled: !path, action: () => toggleFavorite(path) },
        ...(inLibrary(path) && !path.includes('/') && !readOnlyPath(path) ? ['-', libraryMenu()[0]] : []),
      ], ev.clientX, ev.clientY);
    });
    R.footText = el('span', { class: 'as-foottext' });
    R.footWarn = el('span', { class: 'as-footwarn', title: 'The project has more files than the browser lists (5000)' }, 'Some files not listed');
    root.replaceChildren(bar,
      el('div', { class: 'as-body' }, R.tree, el('div', { class: 'as-main' }, R.banner, R.head, R.content)),
      el('div', { class: 'as-footer' }, R.footText, el('span', { class: 'spacer' }), R.footWarn));
    root.addEventListener('mousedown', ev => {
      if (!ev.target.closest('input') && !ev.target.closest('.as-content')) R.root.focus({ preventScroll: true });
    });
    render();
  }

  return {
    init(helpers) {
      h = helpers;
      el = h.el;
      icon = window.icon;
      build(h.$('#assets'));
    },
    // The Project tab shows / hides: the engine watches the project only while it shows.
    shown(on) {
      if (on === S.visible) return;
      S.visible = on;
      h.send('editor.assets.watch', { on });
      if (on) schedulePaint();
    },
    onList, onDone, onImage, onPreview, onMaterial, onSound, onClip, onCook, onUsed, applyPrefs, render, materialSlot, materialPick, materialMenu, soundSlot, soundPicker,
    useInProject,
    // The scene's meshes' names (shapes, rocks, scene and model meshes).
    meshes: () => S.meshes.map(m => m.name),
    // The project's assets of a kind ('material', 'prefab', ...), by path.
    paths: kind => [...S.items.values()].filter(i => i.kind === kind).map(i => i.path).sort(),
    // The folder shown (for things saved from elsewhere: prefabs), and an asset shown and selected.
    folder: () => (currentFolder() === 'Assets' || currentFolder().startsWith('Assets/') ? currentFolder() : 'Assets'),
    reveal(path) {
      if (!path || !S.items.has(path)) return;
      h.showTab('bottom', 'project');
      navigate({ type: 'folder', path: parentOf(path) });
      select([path]);
      reveal(path);
    },
    focused: () => !!R.root && R.root.contains(document.activeElement) && document.activeElement !== R.search,
    key,
    inspectorNodes,
    stopInspecting,
    probe: () => ({
      assetsFolder: S.loc.type === 'folder' ? S.loc.path : `${S.loc.type}:${S.loc.kind || S.loc.group}`,
      assetsLibraries: S.libraries.map(l => `${l.prefix}${l.readOnly ? ':ro' : ''}`).join('|'),
      assetsSearch: S.search,
      assetsCount: S.entries.length,
      assetsItems: S.entries.slice(0, 200).map(e => e.name).join('|'),
      assetsSelection: [...S.selection].join('|'),
      assetsRenaming: !!S.renaming,
      assetsView: S.view,
      assetsInspecting: S.inspect && inspecting() ? S.inspect.key : '',
      assetsPicture: !!(S.inspect && (S.images.get(S.inspect.key) || {}).url),  // (an image's, a library material's)
      assetsFocused: !!R.root && R.root.contains(document.activeElement),
      assetsPreviews: S.previews.size,
      assetsStale: S.stale,
      assetsCooking: S.cook ? S.cook.stage : '',
      assetsCookFailed: !!S.cookFailed,  // (its dialog shown)
      assetsDialog: !!document.querySelector('.modal-back'),
    }),
  };
})();
