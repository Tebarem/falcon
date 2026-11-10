// The Animator tab (the centre dock, beside Scene and Game): a project animation graph (.fgraph)
// open for editing, and a live preview the engine draws into #anim-preview (the page reports that
// rect as the "anim" view; the scene view is hidden meanwhile, so nothing of the page may sit over
// the pane). Messages: engine/editor/anim_editor.h; the format: engine/anim/anim_graph_compiler.h.
//
// The graph is edited as its JSON document. On open every inline node moves into `nodes` (an id
// each) and every node gets `editor.graph`, the view it is drawn in, and `editor.pos`:
//   "root"                 the graph's own flow graph (its result: `root`)
//   "<machine>:<state>"    a state's pose graph (its result: the state's `pose`)
//   "entry:<machine>:<s>"  a state's entry condition
//   "when:<transition id>" a transition's condition (every transition gets an `id`)
// A state machine node opens its machine view: states (boxes) and transitions (arrows). Edits
// recompile at once (the preview and the scene's characters swap to the result); Ctrl+S saves.
const Animator = (() => {
  let h;  // editor.js helpers (Animator.init)
  const S = {
    path: '',        // the open graph
    doc: null,       // its JSON document (normalized)
    info: null,      // editor.anim.graph: {path, name, skeleton, errors, params, models, clips}
    preview: null,   // editor.anim.preview: {ok, model, error, options}
    state: null,     // editor.anim.state: {time, state, params, nodes}
    debug: new Map(),  // node id -> [flags, time, progress, active]
    values: {},      // parameters as set here (they drive the preview)
    shown: false,
    dirty: false,
    view: { kind: 'flow', scope: 'root' },
    views: {},       // per view key: {x, y, zoom}
    selected: new Set(),  // node ids, "state:<machine>:<name>", "transition:<id>", "param:<name>"
    undo: [], redo: [],
    compileTimer: 0,
  };
  const R = {};  // elements

  // ---------------------------------------------------------------- node types

  // in: [key, type] pins; props: [key, kind, default, options]; list: pins repeated per item.
  const TYPES = {
    clip: { title: 'Clip', group: 'Pose', out: 'pose', in: [['reverse', 'bool'], ['reset', 'bool']],
      props: [['clip', 'clip', ''], ['speed', 'float', 1], ['loop', 'bool', true], ['rootMotion', 'bool', true], ['syncOffset', 'int', 0]] },
    blend1d: { title: 'Blend 1D', group: 'Pose', out: 'pose', in: [['input', 'float']], props: [['loop', 'bool', true]],
      list: { key: 'sources', item: () => ({ value: 0, pose: null }), pins: [['pose', 'pose']], props: [['value', 'float', 0]],
        label: (it, i) => `Source ${i + 1} (${it && it.value !== undefined ? it.value : 0})` } },
    blend2d: { title: 'Blend 2D', group: 'Pose', out: 'pose', in: [['x', 'float'], ['y', 'float']], props: [['loop', 'bool', true]],
      list: { key: 'sources', item: () => ({ value: [0, 0], pose: null }), pins: [['pose', 'pose']], props: [['value', 'pair', [0, 0]]],
        label: (it, i) => `Source ${i + 1} (${((it && it.value) || [0, 0]).join(', ')})` } },
    layers: { title: 'Layers', group: 'Pose', out: 'pose', in: [['base', 'pose']],
      list: { key: 'layers', item: () => ({ pose: null, weight: 1, mode: 'overlay' }), pins: [['pose', 'pose'], ['weight', 'float'], ['mask', 'mask']],
        props: [['mode', 'enum', 'overlay', ['overlay', 'additive']], ['sync', 'bool', false], ['ignoreEvents', 'bool', false], ['rootMotion', 'bool', false]],
        label: (it, i) => `Layer ${i + 1}` } },
    speedScale: { title: 'Speed Scale', group: 'Pose', out: 'pose', in: [['pose', 'pose'], ['input', 'float']] },
    selector: { title: 'Selector', group: 'Pose', out: 'pose',
      list: { key: 'options', item: () => ({ pose: null, when: true }), pins: [['pose', 'pose'], ['when', 'bool']], label: (it, i) => `Option ${i + 1}` } },
    reference: { title: 'Reference Pose', group: 'Pose', out: 'pose' },
    stateMachine: { title: 'State Machine', group: 'Pose', out: 'pose', props: [['default', 'state', '']] },
    and: { title: 'And', group: 'Logic', out: 'bool', list: { key: 'inputs', bare: true, item: () => null, pins: [['', 'bool']], label: (it, i) => `In ${i + 1}` } },
    or: { title: 'Or', group: 'Logic', out: 'bool', list: { key: 'inputs', bare: true, item: () => null, pins: [['', 'bool']], label: (it, i) => `In ${i + 1}` } },
    not: { title: 'Not', group: 'Logic', out: 'bool', in: [['input', 'bool']] },
    compare: { title: 'Compare', group: 'Logic', out: 'bool', in: [['input', 'float'], ['value', 'float']],
      props: [['op', 'enum', '>=', ['>=', '<=', '>', '<', '~=']], ['epsilon', 'float', 0.0001]] },
    range: { title: 'In Range', group: 'Logic', out: 'bool', in: [['input', 'float']],
      props: [['min', 'float', 0], ['max', 'float', 1], ['inclusive', 'bool', true]] },
    math: { title: 'Math', group: 'Math', out: 'float', in: [['a', 'float'], ['b', 'float']],
      props: [['op', 'enum', 'add', ['add', 'sub', 'mul', 'div', 'mod', 'abs', 'negate', 'floor', 'ceil', 'int', 'frac', 'invfrac']],
        ['absResult', 'bool', false], ['negateResult', 'bool', false]] },
    remap: { title: 'Remap', group: 'Math', out: 'float', in: [['input', 'float']], props: [['from', 'pair', [0, 1]], ['to', 'pair', [0, 1]]] },
    clamp: { title: 'Clamp', group: 'Math', out: 'float', in: [['input', 'float']], props: [['min', 'float', 0], ['max', 'float', 1]] },
    id: { title: 'ID', group: 'IDs', out: 'id', props: [['value', 'string', '']] },
    idCompare: { title: 'ID Compare', group: 'IDs', out: 'bool', in: [['input', 'id']], props: [['ids', 'strings', []], ['not', 'bool', false]] },
    event: { title: 'Event', group: 'Events', out: 'bool', props: [['ids', 'strings', []], ['all', 'bool', false], ['inactive', 'bool', false]] },
    eventPercent: { title: 'Event Percent', group: 'Events', out: 'float', props: [['id', 'string', ''], ['inactive', 'bool', false]] },
    boneMask: { title: 'Bone Mask', group: 'Masks', out: 'mask', props: [['mask', 'maskName', '']] },
    boneMaskBlend: { title: 'Mask Blend', group: 'Masks', out: 'mask', in: [['a', 'mask'], ['b', 'mask'], ['weight', 'float']] },
    stateCompleted: { title: 'State Completed', group: 'State', out: 'bool', props: [['duration', 'float', 0]], condition: true },
    time: { title: 'Time In State', group: 'State', out: 'bool', in: [['value', 'float']],
      props: [['comparison', 'enum', 'percent', ['percent', 'syncEvent', 'seconds']], ['op', 'enum', '>=', ['<', '<=', '>', '>=']]], condition: true },
  };
  const PIN_COLOR = { pose: '#6fbf73', bool: '#e05c5c', float: '#6aa8e6', id: '#d6a548', mask: '#b48ead', any: '#999' };
  const NODE_W = 172, HEAD = 22, ROW = 18;

  // ---------------------------------------------------------------- the document

  const isRef = v => typeof v === 'string' && v.startsWith('@');
  const refId = v => v.slice(1);
  const nodeOf = id => S.doc && S.doc.nodes[id];
  const editorOf = o => (o.editor = o.editor && typeof o.editor === 'object' ? o.editor : {});
  const paramType = spec => (typeof spec === 'string' ? spec : (spec && spec.type) || 'float');
  function uniqueId(base) {
    let n = 1, id = base;
    while (S.doc.nodes[id]) id = `${base}${++n}`;
    return id;
  }
  // Every input of a node: {path: [...keys], type, label}.
  function inputsOf(n) {
    const t = TYPES[n.type];
    if (!t) return [];
    const pins = (t.in || []).map(([key, type]) => ({ path: [key], type, label: key }));
    if (t.list) {
      const items = Array.isArray(n[t.list.key]) ? n[t.list.key] : [];
      items.forEach((it, i) => t.list.pins.forEach(([key, type]) =>
        pins.push({ path: t.list.bare ? [t.list.key, i] : [t.list.key, i, key], type,
          label: t.list.label(it, i) + (t.list.pins.length > 1 ? ' ' + key : '') })));
    }
    return pins;
  }
  const getAt = (o, path) => path.reduce((v, k) => (v == null ? undefined : v[k]), o);
  function setAt(o, path, value) {
    const last = path[path.length - 1];
    const parent = getAt(o, path.slice(0, -1));
    if (parent == null) return;
    if (value === undefined) { if (Array.isArray(parent)) parent[last] = null; else delete parent[last]; }
    else parent[last] = value;
  }
  function stateByKey(key) {  // "<machine>:<state>"
    const i = key.indexOf(':');
    const sm = i > 0 && nodeOf(key.slice(0, i));
    const s = sm && sm.states && sm.states[key.slice(i + 1)];
    return s ? { machine: key.slice(0, i), name: key.slice(i + 1), s } : null;
  }
  function transitionById(id) {
    for (const [mid, n] of Object.entries(S.doc.nodes))
      if (n.type === 'stateMachine')
        for (const [name, s] of Object.entries(n.states || {}))
          for (const t of s.transitions || [])
            if (t.id === id) return { machine: mid, from: name, t };
    return null;
  }
  // The views' results: what each flow graph's Result node feeds.
  function resultOf(scope) {
    const d = S.doc;
    if (scope === 'root') return { get: () => d.root, set: v => { d.root = v === undefined ? null : v; }, type: 'pose', label: 'Result' };
    const m = /^(entry|when):(.*)$/.exec(scope);
    if (m && m[1] === 'when') {
      const t = transitionById(m[2]);
      return t && { get: () => t.t.when, set: v => { if (v === undefined) delete t.t.when; else t.t.when = v; }, type: 'bool', label: 'Condition' };
    }
    if (m) {
      const s = stateByKey(m[2]);
      return s && { get: () => s.s.entry, set: v => { if (v === undefined) delete s.s.entry; else s.s.entry = v; }, type: 'bool', label: 'Entry condition' };
    }
    const s = stateByKey(scope);
    return s && { get: () => s.s.pose, set: v => { s.s.pose = v === undefined ? null : v; }, type: 'pose', label: 'Pose' };
  }

  // Inline nodes into `nodes`; ids for transitions; each node's view, and a position when it has none.
  function normalize(doc) {
    S.doc = doc;
    doc.nodes = doc.nodes && typeof doc.nodes === 'object' ? doc.nodes : {};
    doc.parameters = doc.parameters && typeof doc.parameters === 'object' ? doc.parameters : {};
    const seen = new Set();
    const depthIn = {};  // view -> [[id, depth]]
    const hoist = v => {
      if (v && typeof v === 'object' && !Array.isArray(v) && v.type) {
        const id = uniqueId(v.type === 'stateMachine' ? 'machine' : v.type);
        doc.nodes[id] = v;
        return '@' + id;
      }
      return v;
    };
    const visit = (ref, scope, depth) => {
      if (!isRef(ref)) return;
      const id = refId(ref), n = doc.nodes[id];
      if (!n) return;
      const e = editorOf(n);
      if (!e.graph) e.graph = scope;
      if (seen.has(id)) return;
      seen.add(id);
      (depthIn[e.graph] = depthIn[e.graph] || []).push([id, depth]);
      for (const pin of inputsOf(n)) {
        const v = hoist(getAt(n, pin.path));
        if (v !== getAt(n, pin.path)) setAt(n, pin.path, v);
        visit(v, e.graph, depth + 1);
      }
      if (n.type === 'stateMachine') {
        const states = n.states = n.states && typeof n.states === 'object' ? n.states : {};
        Object.entries(states).forEach(([name, s], i) => {
          const se = editorOf(s);
          if (!Array.isArray(se.pos)) se.pos = [(i % 3) * 220, Math.floor(i / 3) * 120];
          s.pose = hoist(s.pose);
          visit(s.pose, `${id}:${name}`, 1);
          if (s.entry !== undefined) { s.entry = hoist(s.entry); visit(s.entry, `entry:${id}:${name}`, 1); }
          const used = new Set();
          for (const t of s.transitions || []) {
            if (!t.id || used.has(t.id)) {
              let tid = `${id}:${name}->${t.to}`, k = 1;
              while (used.has(tid)) tid = `${id}:${name}->${t.to}#${k++}`;
              t.id = tid;
            }
            used.add(t.id);
            if (t.when !== undefined) { t.when = hoist(t.when); visit(t.when, `when:${t.id}`, 1); }
          }
        });
      }
    };
    doc.root = hoist(doc.root);
    visit(doc.root, 'root', 1);
    // Nodes nothing reaches stay where they were drawn (or in the root graph).
    for (const [id, n] of Object.entries(doc.nodes))
      if (!seen.has(id)) {
        const e = editorOf(n);
        if (!e.graph) e.graph = 'root';
        (depthIn[e.graph] = depthIn[e.graph] || []).push([id, 1]);
      }
    // Positions: columns by depth from the result, for those without one.
    for (const list of Object.values(depthIn)) {
      const rows = {};
      for (const [id, depth] of list) {
        const e = editorOf(doc.nodes[id]);
        if (Array.isArray(e.pos)) continue;
        rows[depth] = (rows[depth] || 0) + 1;
        e.pos = [-depth * 230, (rows[depth] - 1) * 110];
      }
    }
  }
  const text = () => JSON.stringify(S.doc, null, 2) + '\n';

  // One edit: undoable, recompiled, redrawn.
  function change(fn) {
    S.undo.push(text());
    if (S.undo.length > 200) S.undo.shift();
    S.redo = [];
    fn();
    edited();
  }
  function edited() {
    S.dirty = true;
    clearTimeout(S.compileTimer);
    S.compileTimer = setTimeout(() => h.send('editor.anim.graph.compile', { path: S.path, text: text() }), 120);
    renderSide();
    draw();
  }
  function restore(from, to) {
    if (!from.length) return;
    to.push(text());
    S.doc = JSON.parse(from.pop());
    S.selected.clear();
    if (S.view.kind === 'machine' && !nodeOf(S.view.id)) S.view = { kind: 'flow', scope: 'root' };
    edited();
  }
  const undo = () => restore(S.undo, S.redo);
  const redo = () => restore(S.redo, S.undo);
  function save() {
    if (!S.path || !S.doc) return;
    clearTimeout(S.compileTimer);
    h.send('editor.anim.graph.save', { path: S.path, text: text() });
  }

  // ---------------------------------------------------------------- views

  const viewKey = () => S.view.kind === 'machine' ? 'm:' + S.view.id : 'f:' + S.view.scope;
  const camera = () => (S.views[viewKey()] = S.views[viewKey()] || { x: 0, y: 0, zoom: 1, fresh: true });
  // Nodes drawn in the current flow view, with their boxes (graph space).
  function flowNodes() {
    const out = [];
    for (const [id, n] of Object.entries(S.doc.nodes)) {
      if (editorOf(n).graph !== S.view.scope) continue;
      const pins = inputsOf(n);
      const [x, y] = editorOf(n).pos || [0, 0];
      out.push({ id, n, pins, x, y, w: NODE_W, h: HEAD + Math.max(1, pins.length) * ROW + 6 });
    }
    return out;
  }
  function resultBox() {
    const r = S.view.kind === 'flow' && resultOf(S.view.scope);
    if (!r) return null;
    const e = (S.doc.editor = S.doc.editor && typeof S.doc.editor === 'object' ? S.doc.editor : {});
    const at = ((e.results = e.results || {})[S.view.scope]) || [20, 0];
    return { id: '#result', r, x: at[0], y: at[1], w: 130, h: HEAD + ROW + 6, pins: [] };
  }
  function states() {  // machine view: state boxes
    const sm = nodeOf(S.view.id);
    return Object.entries((sm && sm.states) || {}).map(([name, s]) => {
      const [x, y] = editorOf(s).pos || [0, 0];
      return { key: `${S.view.id}:${name}`, name, s, x, y, w: 150, h: 44 };
    });
  }
  function enter(view) {
    S.view = view;
    S.selected.clear();
    renderSide();
    fitIfFresh();
    draw();
  }
  const nodeTitle = id => (TYPES[(nodeOf(id) || {}).type] || { title: id }).title + ' ' + id;
  // Breadcrumbs: Root > Machine > State (> condition).
  function crumbs() {
    const list = [{ label: 'Root', view: { kind: 'flow', scope: 'root' } }];
    const chainTo = machine => {
      const n = nodeOf(machine);
      const g = n && editorOf(n).graph;
      if (g && g !== 'root') chainScope(g);
      list.push({ label: machine, view: { kind: 'machine', id: machine } });
    };
    const chainScope = sc => {
      const m = /^(entry|when):(.*)$/.exec(sc);
      if (m && m[1] === 'when') {
        const t = transitionById(m[2]);
        if (t) { chainTo(t.machine); list.push({ label: `${t.from} → ${t.t.to}`, view: { kind: 'flow', scope: sc } }); }
        return;
      }
      const st = stateByKey(m ? m[2] : sc);
      if (st) { chainTo(st.machine); list.push({ label: st.name + (m ? ' (entry)' : ''), view: { kind: 'flow', scope: sc } }); }
    };
    if (S.view.kind === 'machine') chainTo(S.view.id);
    else if (S.view.scope !== 'root') chainScope(S.view.scope);
    return list;
  }

  // ---------------------------------------------------------------- drawing

  let ctx = null, drawQueued = false, drag = null;
  function draw() {
    if (!R.canvas || drawQueued) return;
    drawQueued = true;
    requestAnimationFrame(() => { drawQueued = false; paint(); });
  }
  function toScreen(x, y) { const c = camera(); return [(x - c.x) * c.zoom + R.canvas.clientWidth / 2, (y - c.y) * c.zoom + R.canvas.clientHeight / 2]; }
  function toGraph(px, py) { const c = camera(); return [(px - R.canvas.clientWidth / 2) / c.zoom + c.x, (py - R.canvas.clientHeight / 2) / c.zoom + c.y]; }
  function errorFor(id) {
    for (const e of (S.info && S.info.errors) || [])
      if (e.node === id || e.node.startsWith(id + '/')) return e.text;
    return '';
  }
  const outPin = b => [b.x + b.w, b.y + HEAD / 2];
  const inPin = (b, i) => [b.x, b.y + HEAD + i * ROW + ROW / 2 + 2];

  function paint() {
    const c = R.canvas;
    const w = c.clientWidth, hgt = c.clientHeight;
    if (!w || !hgt) return;
    const dpr = window.devicePixelRatio || 1;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(hgt * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(hgt * dpr); }
    ctx = c.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = '#1c1c1c';
    ctx.fillRect(0, 0, w, hgt);
    ctx.font = '12px system-ui, sans-serif';
    ctx.textBaseline = 'middle';
    if (!S.doc) {
      ctx.fillStyle = '#777';
      ctx.fillText('Open a graph: double-click a .fgraph in the Project panel', 16, 24);
      return;
    }
    const cam = camera();
    const step = 20 * cam.zoom;
    if (step > 6) {  // the grid
      ctx.strokeStyle = '#242424';
      ctx.lineWidth = 1;
      const [ox, oy] = toScreen(0, 0);
      ctx.beginPath();
      for (let x = ((ox % step) + step) % step; x < w; x += step) { ctx.moveTo(x + 0.5, 0); ctx.lineTo(x + 0.5, hgt); }
      for (let y = ((oy % step) + step) % step; y < hgt; y += step) { ctx.moveTo(0, y + 0.5); ctx.lineTo(w, y + 0.5); }
      ctx.stroke();
    }
    ctx.save();
    ctx.translate(w / 2, hgt / 2);
    ctx.scale(cam.zoom, cam.zoom);
    ctx.translate(-cam.x, -cam.y);
    ctx.font = '11px system-ui, sans-serif';
    if (S.view.kind === 'machine') paintMachine(); else paintFlow();
    ctx.restore();
    if (drag && drag.kind === 'box') {
      ctx.strokeStyle = '#4a90d9';
      ctx.fillStyle = 'rgba(74,144,217,0.12)';
      const x = Math.min(drag.sx, drag.px), y = Math.min(drag.sy, drag.py);
      ctx.fillRect(x, y, Math.abs(drag.px - drag.sx), Math.abs(drag.py - drag.sy));
      ctx.strokeRect(x + 0.5, y + 0.5, Math.abs(drag.px - drag.sx), Math.abs(drag.py - drag.sy));
    }
    if (S.view.kind === 'machine' || (S.view.kind === 'flow' && !flowNodes().length)) {
      ctx.fillStyle = '#666';
      ctx.font = '11px system-ui, sans-serif';
      ctx.fillText(S.view.kind === 'machine' ? 'Double-click: a state (empty space) or its pose (a state) · drag a state’s right edge to another: a transition'
        : 'Double-click or right-click: add a node · drag from a pin: link it', 12, hgt - 12);
    }
  }
  function wire(a, b, color, dashed = false) {
    const dx = Math.max(40, Math.abs(b[0] - a[0]) * 0.5);
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.setLineDash(dashed ? [5, 4] : []);
    ctx.beginPath();
    ctx.moveTo(a[0], a[1]);
    ctx.bezierCurveTo(a[0] + dx, a[1], b[0] - dx, b[1], b[0], b[1]);
    ctx.stroke();
    ctx.setLineDash([]);
  }
  function fitText(s, max) {
    if (ctx.measureText(s).width <= max) return s;
    while (s.length > 1 && ctx.measureText(s + '…').width > max) s = s.slice(0, -1);
    return s + '…';
  }
  function roundRect(x, y, w, hh, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + hh, r);
    ctx.arcTo(x + w, y + hh, x, y + hh, r);
    ctx.arcTo(x, y + hh, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }
  // An input's value drawn at its pin (a link to another view, a parameter or a constant).
  function inputLabel(v) {
    if (isRef(v)) { const n = nodeOf(refId(v)); return n && editorOf(n).graph !== S.view.scope ? '↗ ' + refId(v) : null; }
    if (typeof v === 'string' && v.startsWith('$')) return v.slice(1);
    if (typeof v === 'number') return String(+v.toFixed(4));
    if (typeof v === 'boolean') return v ? 'true' : 'false';
    return null;
  }
  function pinDot([x, y], type, filled) {
    ctx.beginPath();
    ctx.arc(x, y, 4.5, 0, Math.PI * 2);
    ctx.fillStyle = filled ? PIN_COLOR[type] || '#999' : '#1c1c1c';
    ctx.fill();
    ctx.strokeStyle = PIN_COLOR[type] || '#999';
    ctx.lineWidth = 1.5;
    ctx.stroke();
  }

  function paintFlow() {
    const nodes = flowNodes();
    const byId = new Map(nodes.map(b => [b.id, b]));
    const result = resultBox();
    const wires = [];
    for (const b of nodes)
      b.pins.forEach((pin, i) => {
        const v = getAt(b.n, pin.path);
        if (isRef(v) && byId.has(refId(v))) wires.push([byId.get(refId(v)), b, i, pin.type]);
      });
    if (result) {
      const v = result.r.get();
      if (isRef(v) && byId.has(refId(v))) wires.push([byId.get(refId(v)), result, 0, result.r.type]);
    }
    for (const [from, to, i, type] of wires) {
      const dbg = S.debug.get(from.id);
      wire(outPin(from), inPin(to, i), dbg && (dbg[0] & 2) ? '#9be29e' : PIN_COLOR[type] || '#999');
    }
    if (drag && drag.kind === 'link') {
      const end = toGraph(drag.px, drag.py);
      if (drag.from) wire(outPin(drag.from), end, '#ddd', true); else wire(end, inPin(drag.to, drag.pin), '#ddd', true);
    }
    for (const b of nodes) paintNode(b);
    if (result) {
      const b = result;
      roundRect(b.x, b.y, b.w, b.h, 4);
      ctx.fillStyle = '#2b2b2b';
      ctx.fill();
      ctx.strokeStyle = S.selected.has('#result') ? '#4a90d9' : '#666';
      ctx.lineWidth = 1.5;
      ctx.stroke();
      ctx.fillStyle = '#3a3a3a';
      ctx.fillRect(b.x + 1, b.y + 1, b.w - 2, HEAD - 2);
      ctx.fillStyle = '#ddd';
      ctx.fillText(b.r.label, b.x + 8, b.y + HEAD / 2);
      pinDot(inPin(b, 0), b.r.type, b.r.get() != null);
      const label = inputLabel(b.r.get());
      ctx.fillStyle = '#aaa';
      ctx.fillText(label ? fitText(label, b.w - 20) : b.r.type, b.x + 12, inPin(b, 0)[1]);
    }
  }
  function paintNode(b) {
    const t = TYPES[b.n.type] || { title: b.n.type, out: 'any' };
    const dbg = S.debug.get(b.id);
    const live = dbg && (dbg[0] & 2);
    const error = errorFor(b.id);
    roundRect(b.x, b.y, b.w, b.h, 4);
    ctx.fillStyle = '#2b2b2b';
    ctx.fill();
    ctx.lineWidth = S.selected.has(b.id) || error ? 2 : 1;
    ctx.strokeStyle = error ? '#e05c5c' : S.selected.has(b.id) ? '#4a90d9' : live ? '#7fd37f' : '#444';
    ctx.stroke();
    ctx.fillStyle = PIN_COLOR[t.out] || '#666';
    ctx.globalAlpha = 0.35;
    ctx.fillRect(b.x + 1, b.y + 1, b.w - 2, HEAD - 2);
    ctx.globalAlpha = 1;
    ctx.fillStyle = '#eee';
    ctx.fillText(fitText(t.title + (b.n.type === 'clip' && b.n.clip ? ': ' + b.n.clip : ''), b.w - 30), b.x + 8, b.y + HEAD / 2);
    pinDot(outPin(b), t.out, true);
    b.pins.forEach((pin, i) => {
      const v = getAt(b.n, pin.path);
      const p = inPin(b, i);
      pinDot(p, pin.type, v != null);
      ctx.fillStyle = '#bbb';
      ctx.fillText(fitText(pin.label, b.w / 2 - 14), b.x + 10, p[1]);
      const label = inputLabel(v);
      if (label) {
        ctx.fillStyle = typeof v === 'string' && v.startsWith('$') ? '#9fc8ff' : '#999';
        const s = fitText(label, b.w / 2 - 12);
        ctx.fillText(s, b.x + b.w - 10 - ctx.measureText(s).width, p[1]);
      }
    });
    if (!b.pins.length) {
      ctx.fillStyle = '#888';
      const note = b.n.type === 'stateMachine' ? `${Object.keys(b.n.states || {}).length} states · double-click to open`
        : b.n.type === 'id' ? (b.n.value || '') : b.n.type === 'stateCompleted' ? 'the state is done'
        : b.n.type === 'event' ? (b.n.ids || []).join(b.n.all ? ' & ' : ' | ') : b.n.type === 'eventPercent' ? b.n.id || ''
        : b.n.type === 'boneMask' ? b.n.mask || '(its own bones)' : '';
      ctx.fillText(fitText(note, b.w - 20), b.x + 10, b.y + HEAD + ROW / 2 + 2);
    }
    if (live && t.out === 'pose' && dbg[1] > 0) {  // how far through it is
      ctx.fillStyle = '#7fd37f';
      ctx.fillRect(b.x + 2, b.y + b.h - 4, (b.w - 4) * Math.min(1, dbg[1]), 2);
    }
  }

  // Where a transition's arrow runs between two boxes (offset sideways so A->B and B->A both show).
  function arrowEnds(a, b) {
    const ca = [a.x + a.w / 2, a.y + a.h / 2], cb = [b.x + b.w / 2, b.y + b.h / 2];
    const d = [cb[0] - ca[0], cb[1] - ca[1]], len = Math.hypot(d[0], d[1]) || 1;
    const u = [d[0] / len, d[1] / len], n = [-u[1] * 6, u[0] * 6];
    const edge = (box, c, dir) => {
      const tx = dir[0] ? (box.w / 2) / Math.abs(dir[0]) : Infinity, ty = dir[1] ? (box.h / 2) / Math.abs(dir[1]) : Infinity;
      const t = Math.min(tx, ty);
      return [c[0] + dir[0] * t, c[1] + dir[1] * t];
    };
    const p = edge(a, ca, u), q = edge(b, cb, [-u[0], -u[1]]);
    return [[p[0] + n[0], p[1] + n[1]], [q[0] + n[0], q[1] + n[1]]];
  }
  function paintMachine() {
    const list = states();
    const sm = nodeOf(S.view.id);
    const byName = new Map(list.map(s => [s.name, s]));
    const dbgMachine = S.debug.get(S.view.id);
    const activeKey = dbgMachine && (dbgMachine[0] & 2) ? dbgMachine[3] : '';
    for (const st of list)
      for (const t of st.s.transitions || []) {
        const to = byName.get(t.to);
        if (!to) continue;
        const [a, b] = arrowEnds(st, to);
        const sel = S.selected.has('transition:' + t.id);
        const dbg = S.debug.get(t.id);
        const running = dbg && (dbg[0] & 2) && dbg[2] >= 0;
        ctx.strokeStyle = errorFor(t.id) ? '#e05c5c' : sel ? '#4a90d9' : running ? '#7fd37f' : '#888';
        ctx.lineWidth = sel ? 2.5 : 1.5;
        ctx.beginPath();
        ctx.moveTo(a[0], a[1]);
        ctx.lineTo(b[0], b[1]);
        ctx.stroke();
        const ang = Math.atan2(b[1] - a[1], b[0] - a[0]);
        const mx = (a[0] + b[0]) / 2, my = (a[1] + b[1]) / 2;
        ctx.fillStyle = ctx.strokeStyle;
        ctx.beginPath();
        ctx.moveTo(mx + Math.cos(ang) * 7, my + Math.sin(ang) * 7);
        ctx.lineTo(mx + Math.cos(ang + 2.5) * 7, my + Math.sin(ang + 2.5) * 7);
        ctx.lineTo(mx + Math.cos(ang - 2.5) * 7, my + Math.sin(ang - 2.5) * 7);
        ctx.fill();
        if (running) {
          ctx.fillStyle = '#7fd37f';
          ctx.beginPath();
          ctx.arc(a[0] + (b[0] - a[0]) * dbg[2], a[1] + (b[1] - a[1]) * dbg[2], 4, 0, Math.PI * 2);
          ctx.fill();
        }
      }
    if (drag && drag.kind === 'transition') {
      const end = toGraph(drag.px, drag.py);
      ctx.strokeStyle = '#ddd';
      ctx.setLineDash([5, 4]);
      ctx.beginPath();
      ctx.moveTo(drag.from.x + drag.from.w / 2, drag.from.y + drag.from.h / 2);
      ctx.lineTo(end[0], end[1]);
      ctx.stroke();
      ctx.setLineDash([]);
    }
    for (const st of list) {
      const active = activeKey === st.key;
      const sel = S.selected.has('state:' + st.key);
      roundRect(st.x, st.y, st.w, st.h, 6);
      ctx.fillStyle = active ? '#2f4a2f' : '#2b2b2b';
      ctx.fill();
      ctx.lineWidth = sel ? 2.5 : 1.5;
      ctx.strokeStyle = errorFor(st.key) ? '#e05c5c' : sel ? '#4a90d9' : active ? '#7fd37f' : '#555';
      ctx.stroke();
      ctx.fillStyle = '#555';
      ctx.fillRect(st.x + st.w - 8, st.y + 8, 3, st.h - 16);  // the edge transitions are dragged from
      ctx.fillStyle = '#eee';
      ctx.font = '12px system-ui, sans-serif';
      ctx.fillText(fitText(st.name, st.w - 24), st.x + 12, st.y + 15);
      ctx.font = '11px system-ui, sans-serif';
      ctx.fillStyle = '#999';
      const pose = isRef(st.s.pose) && nodeOf(refId(st.s.pose));
      ctx.fillText(fitText(pose ? (TYPES[pose.type] || {}).title + (pose.clip ? ': ' + pose.clip : '') : 'no pose', st.w - 24), st.x + 12, st.y + 31);
      if (sm && sm.default === st.name) {  // the default state: an arrow in from the left
        ctx.fillStyle = '#d6a548';
        ctx.beginPath();
        ctx.moveTo(st.x - 4, st.y + st.h / 2);
        ctx.lineTo(st.x - 16, st.y + st.h / 2 - 6);
        ctx.lineTo(st.x - 16, st.y + st.h / 2 + 6);
        ctx.fill();
      }
      const dbg = S.debug.get(st.key);
      if (active && dbg && dbg[1] > 0) {
        ctx.fillStyle = '#7fd37f';
        ctx.fillRect(st.x + 3, st.y + st.h - 5, (st.w - 6) * Math.min(1, dbg[1]), 2);
      }
    }
  }

  // ---------------------------------------------------------------- hit tests

  function hitFlow(gx, gy) {
    const result = resultBox();
    const all = result ? [...flowNodes(), result] : flowNodes();
    for (let k = all.length - 1; k >= 0; --k) {
      const b = all[k];
      if (b !== result) {
        const [ox, oy] = outPin(b);
        if (Math.hypot(gx - ox, gy - oy) < 8) return { kind: 'out', box: b };
      }
      const pins = b === result ? 1 : b.pins.length;
      for (let i = 0; i < pins; ++i) {
        const [ix, iy] = inPin(b, i);
        if (Math.hypot(gx - ix, gy - iy) < 8) return { kind: 'in', box: b, pin: i };
      }
      if (gx >= b.x && gx <= b.x + b.w && gy >= b.y && gy <= b.y + b.h) return { kind: 'node', box: b };
    }
    return null;
  }
  function hitMachine(gx, gy) {
    const list = states();
    for (let k = list.length - 1; k >= 0; --k) {
      const s = list[k];
      if (gx >= s.x && gx <= s.x + s.w && gy >= s.y && gy <= s.y + s.h)
        return { kind: gx > s.x + s.w - 14 ? 'stateEdge' : 'state', state: s };
    }
    const byName = new Map(list.map(s => [s.name, s]));
    for (const st of list)
      for (const t of st.s.transitions || []) {
        const to = byName.get(t.to);
        if (!to) continue;
        const [a, b] = arrowEnds(st, to);
        const dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy || 1;
        const u = Math.max(0, Math.min(1, ((gx - a[0]) * dx + (gy - a[1]) * dy) / l2));
        if (Math.hypot(gx - a[0] - dx * u, gy - a[1] - dy * u) < 6) return { kind: 'transition', t, from: st.name };
      }
    return null;
  }

  // ---------------------------------------------------------------- editing

  // What a node's output may feed: the input's type, never itself or into a loop.
  function canLink(fromId, toBox, pin) {
    const from = nodeOf(fromId);
    if (!from || !toBox) return false;
    const out = (TYPES[from.type] || {}).out;
    const type = toBox.id === '#result' ? toBox.r.type : toBox.pins[pin] && toBox.pins[pin].type;
    if (out !== type || toBox.id === fromId) return false;
    return toBox.id === '#result' || !reaches(fromId, toBox.id);
  }
  function reaches(from, target) {  // does `from` (through its inputs) depend on `target`?
    const stack = [from], seen = new Set();
    while (stack.length) {
      const id = stack.pop();
      if (id === target) return true;
      if (seen.has(id)) continue;
      seen.add(id);
      const n = nodeOf(id);
      if (n) for (const pin of inputsOf(n)) { const v = getAt(n, pin.path); if (isRef(v)) stack.push(refId(v)); }
    }
    return false;
  }
  function link(fromId, toBox, pin) {
    change(() => {
      // A pose has one parent: it leaves the old one.
      if ((TYPES[nodeOf(fromId).type] || {}).out === 'pose') unlinkEverywhere(fromId);
      if (toBox.id === '#result') toBox.r.set('@' + fromId);
      else setAt(toBox.n, toBox.pins[pin].path, '@' + fromId);
    });
  }
  function unlinkEverywhere(id) {
    const ref = '@' + id;
    for (const n of Object.values(S.doc.nodes)) {
      for (const pin of inputsOf(n)) if (getAt(n, pin.path) === ref) setAt(n, pin.path, undefined);
      if (n.type === 'stateMachine')
        for (const s of Object.values(n.states || {})) {
          if (s.pose === ref) s.pose = null;
          if (s.entry === ref) delete s.entry;
          for (const t of s.transitions || []) if (t.when === ref) delete t.when;
        }
    }
    if (S.doc.root === ref) S.doc.root = null;
  }
  function addNode(type, gx, gy, from = null, id = null) {
    if (!TYPES[type]) return null;
    change(() => {
      id = id && !nodeOf(id) ? id : uniqueId(type === 'stateMachine' ? 'machine' : type);
      const n = { type };
      for (const [key, kind, def] of TYPES[type].props || [])
        if (kind !== 'state' && def !== undefined && def !== '') n[key] = Array.isArray(def) ? [...def] : def;
      if (type === 'clip') n.clip = (S.info && S.info.clips && S.info.clips[0]) || '';
      if (type === 'blend1d') n.sources = [{ value: 0, pose: null }, { value: 1, pose: null }];
      if (type === 'blend2d') n.sources = [{ value: [0, 0], pose: null }, { value: [1, 0], pose: null }, { value: [0, 1], pose: null }];
      if (type === 'layers') n.layers = [{ pose: null, weight: 1, mode: 'overlay' }];
      if (type === 'boneMask') n.mask = Object.keys(S.doc.boneMasks || {})[0] || '';
      if (type === 'selector') n.options = [{ pose: null, when: true }];
      if (type === 'and' || type === 'or') n.inputs = [null, null];
      if (type === 'stateMachine') { n.default = 'State'; n.states = { State: { pose: null, editor: { pos: [0, 0] } } }; }
      n.editor = { graph: S.view.scope, pos: [Math.round(gx / 10) * 10, Math.round(gy / 10) * 10] };
      S.doc.nodes[id] = n;
      if (from && from.from) {  // dragged from an output into empty space: the new node takes it
        const out = (TYPES[nodeOf(from.from.id).type] || {}).out;
        const pin = inputsOf(n).find(p => p.type === out);
        if (pin) setAt(n, pin.path, '@' + from.from.id);
      } else if (from && from.to) {  // from an input: the new node feeds it
        const want = from.to.id === '#result' ? from.to.r.type : from.to.pins[from.pin].type;
        if (TYPES[type].out === want) {
          if (from.to.id === '#result') from.to.r.set('@' + id);
          else setAt(from.to.n, from.to.pins[from.pin].path, '@' + id);
        }
      }
      S.selected = new Set([id]);
    });
    return id;
  }
  function deleteSelection() {
    if (!S.selected.size) return;
    change(() => {
      for (const key of S.selected) {
        if (key.startsWith('transition:')) {
          const t = transitionById(key.slice(11));
          if (t) { const s = nodeOf(t.machine).states[t.from]; s.transitions = s.transitions.filter(x => x !== t.t); }
        } else if (key.startsWith('state:')) {
          const st = stateByKey(key.slice(6));
          if (!st) continue;
          const sm = nodeOf(st.machine);
          if (Object.keys(sm.states).length <= 1) continue;  // a machine keeps one state
          delete sm.states[st.name];
          for (const s of Object.values(sm.states)) s.transitions = (s.transitions || []).filter(t => t.to !== st.name);
          if (sm.default === st.name) sm.default = Object.keys(sm.states)[0];
        } else if (key.startsWith('param:')) {
          deleteParam(key.slice(6));
        } else if (key.startsWith('mask:')) {
          delete (S.doc.boneMasks || {})[key.slice(5)];
        } else if (key !== '#result' && nodeOf(key)) {
          unlinkEverywhere(key);
          delete S.doc.nodes[key];
        }
      }
      S.selected.clear();
    });
  }
  // Copy / paste: the selected nodes (links among them kept), offset.
  let clipboard = null;
  function copy() {
    const ids = [...S.selected].filter(id => nodeOf(id) && !id.startsWith('mask:'));
    if (ids.length) clipboard = JSON.stringify(ids.map(id => [id, nodeOf(id)]));
  }
  function paste() {
    if (!clipboard || S.view.kind !== 'flow') return;
    change(() => {
      const items = JSON.parse(clipboard), rename = {};
      for (const [id, n] of items) { rename[id] = uniqueId(id.replace(/\d+$/, '')); S.doc.nodes[rename[id]] = n; }
      for (const id of Object.values(rename)) {
        const n = nodeOf(id), e = editorOf(n);
        e.graph = S.view.scope;
        e.pos = [(e.pos || [0, 0])[0] + 30, (e.pos || [0, 0])[1] + 30];
        for (const pin of inputsOf(n)) {
          const v = getAt(n, pin.path);
          if (isRef(v)) setAt(n, pin.path, rename[refId(v)] ? '@' + rename[refId(v)] : undefined);
        }
      }
      S.selected = new Set(Object.values(rename));
    });
  }
  function addState(gx, gy, name) {
    change(() => {
      const sm = nodeOf(S.view.id);
      if (!name) { name = 'State'; let k = 1; while (sm.states[name]) name = `State ${++k}`; }
      sm.states[name] = { pose: null, editor: { pos: [Math.round(gx / 10) * 10, Math.round(gy / 10) * 10] } };
      S.selected = new Set([`state:${S.view.id}:${name}`]);
    });
  }
  function uniqueTransitionId(machine, from, to) {
    let id = `${machine}:${from}->${to}`, k = 1;
    while (transitionById(id)) id = `${machine}:${from}->${to}#${k++}`;
    return id;
  }
  function addTransition(machine, from, to, extra = {}) {
    let id = null;
    change(() => {
      const s = nodeOf(machine).states[from];
      id = uniqueTransitionId(machine, from, to);
      (s.transitions = s.transitions || []).push({ id, to, duration: 0.2, ...extra });
      S.selected = new Set(['transition:' + id]);
    });
    return id;
  }
  function renameParam(from, to) {
    if (!to || to === from || S.doc.parameters[to]) return;
    S.doc.parameters = Object.fromEntries(Object.entries(S.doc.parameters).map(([k, v]) => [k === from ? to : k, v]));
    const walk = o => {
      if (Array.isArray(o)) o.forEach((v, i) => { if (v === '$' + from) o[i] = '$' + to; else walk(v); });
      else if (o && typeof o === 'object') for (const [k, v] of Object.entries(o)) { if (v === '$' + from) o[k] = '$' + to; else walk(v); }
    };
    walk(S.doc.nodes);
    walk(S.doc.ik);
    S.selected = new Set(['param:' + to]);
  }
  function deleteParam(name) {
    delete S.doc.parameters[name];
    const walk = o => {
      if (Array.isArray(o)) o.forEach((v, i) => { if (v === '$' + name) o[i] = null; else walk(v); });
      else if (o && typeof o === 'object') for (const [k, v] of Object.entries(o)) { if (v === '$' + name) delete o[k]; else walk(v); }
    };
    walk(S.doc.nodes);
    walk(S.doc.ik);
    S.selected.clear();
  }
  function renameState(machine, from, to) {
    const sm = nodeOf(machine);
    if (!to || to === from || sm.states[to] || to.includes(':')) return;
    sm.states = Object.fromEntries(Object.entries(sm.states).map(([k, v]) => [k === from ? to : k, v]));
    for (const s of Object.values(sm.states)) for (const t of s.transitions || []) if (t.to === from) t.to = to;
    if (sm.default === from) sm.default = to;
    for (const n of Object.values(S.doc.nodes)) {  // its views move with it
      const e = editorOf(n);
      if (e.graph === `${machine}:${from}`) e.graph = `${machine}:${to}`;
      if (e.graph === `entry:${machine}:${from}`) e.graph = `entry:${machine}:${to}`;
    }
    S.selected = new Set([`state:${machine}:${to}`]);
  }

  // ---------------------------------------------------------------- canvas input

  function local(e) { const r = R.canvas.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; }
  function onDown(e) {
    if (!S.doc) return;
    R.canvas.focus();
    const [px, py] = local(e);
    const [gx, gy] = toGraph(px, py);
    if (e.button === 1 || e.button === 2) { drag = { kind: 'pan', px, py, moved: false, button: e.button }; return; }
    const hit = S.view.kind === 'machine' ? hitMachine(gx, gy) : hitFlow(gx, gy);
    if (!hit) {
      if (!e.shiftKey) S.selected.clear();
      drag = { kind: 'box', sx: px, sy: py, px, py };
    } else if (hit.kind === 'out') {
      drag = { kind: 'link', from: hit.box, px, py };
    } else if (hit.kind === 'in') {
      const v = hit.box.id === '#result' ? hit.box.r.get() : getAt(hit.box.n, hit.box.pins[hit.pin].path);
      const source = isRef(v) && flowNodes().find(b => b.id === refId(v));
      if (source) {  // pick the wire up: it can go to another input, or nowhere
        change(() => { if (hit.box.id === '#result') hit.box.r.set(undefined); else setAt(hit.box.n, hit.box.pins[hit.pin].path, undefined); });
        drag = { kind: 'link', from: source, px, py };
      } else {
        drag = { kind: 'link', to: hit.box, pin: hit.pin, px, py };
      }
    } else if (hit.kind === 'transition') {
      S.selected = new Set(['transition:' + hit.t.id]);
    } else if (hit.kind === 'stateEdge' || (hit.kind === 'state' && (e.ctrlKey || e.altKey))) {
      drag = { kind: 'transition', from: hit.state, px, py };
    } else {
      const key = hit.kind === 'state' ? 'state:' + hit.state.key : hit.box.id;
      if (e.shiftKey) { if (S.selected.has(key)) S.selected.delete(key); else S.selected.add(key); }
      else if (!S.selected.has(key)) S.selected = new Set([key]);
      drag = { kind: 'move', gx, gy, before: text(), moved: false };
    }
    renderSide();
    draw();
  }
  function onMove(e) {
    const [px, py] = local(e);
    const cam = camera();
    if (drag.kind === 'pan') {
      cam.x -= (px - drag.px) / cam.zoom;
      cam.y -= (py - drag.py) / cam.zoom;
      drag.moved = drag.moved || Math.abs(px - drag.px) + Math.abs(py - drag.py) > 0;
    } else if (drag.kind === 'move') {
      const [gx, gy] = toGraph(px, py);
      const dx = gx - drag.gx, dy = gy - drag.gy;
      if (!drag.moved && Math.hypot(dx, dy) * cam.zoom < 3) return;
      drag.moved = true;
      drag.gx = gx;
      drag.gy = gy;
      for (const key of S.selected) {
        if (key === '#result') {
          const res = ((S.doc.editor = S.doc.editor || {}).results = S.doc.editor.results || {});
          const at = res[S.view.scope] || [20, 0];
          res[S.view.scope] = [at[0] + dx, at[1] + dy];
          continue;
        }
        const o = key.startsWith('state:') ? (stateByKey(key.slice(6)) || {}).s : nodeOf(key);
        if (o) { const ed = editorOf(o); ed.pos = [(ed.pos || [0, 0])[0] + dx, (ed.pos || [0, 0])[1] + dy]; }
      }
    }
    drag.px = px;
    drag.py = py;
    draw();
  }
  function onUp(e) {
    const d = drag;
    drag = null;
    const [px, py] = local(e);
    const [gx, gy] = toGraph(px, py);
    if (d.kind === 'move' && d.moved) {  // snapped, as one undo step
      for (const key of S.selected) {
        const o = key.startsWith('state:') ? (stateByKey(key.slice(6)) || {}).s : nodeOf(key);
        if (o) { const ed = editorOf(o); ed.pos = ed.pos.map(v => Math.round(v / 10) * 10); }
      }
      S.undo.push(d.before);
      S.redo = [];
      edited();
    } else if (d.kind === 'pan' && !d.moved && d.button === 2) {
      if (S.view.kind === 'machine') addState(gx, gy); else openPalette(px, py, gx, gy, null);
    } else if (d.kind === 'box') {
      const [ax, ay] = toGraph(Math.min(d.sx, px), Math.min(d.sy, py)), [bx, by] = toGraph(Math.max(d.sx, px), Math.max(d.sy, py));
      if (bx - ax > 2 || by - ay > 2) {
        const inside = b => b.x < bx && b.x + b.w > ax && b.y < by && b.y + b.h > ay;
        if (S.view.kind === 'machine') states().filter(inside).forEach(s => S.selected.add('state:' + s.key));
        else flowNodes().filter(inside).forEach(b => S.selected.add(b.id));
      }
    } else if (d.kind === 'link') {
      const hit = hitFlow(gx, gy);
      if (d.from && hit && hit.kind === 'in' && canLink(d.from.id, hit.box, hit.pin)) link(d.from.id, hit.box, hit.pin);
      else if (d.to && hit && (hit.kind === 'out' || hit.kind === 'node') && hit.box.id !== '#result' && canLink(hit.box.id, d.to, d.pin))
        link(hit.box.id, d.to, d.pin);
      else if (!hit) openPalette(px, py, gx, gy, d);
      else h.setStatus('Those don’t connect: another type, a loop, or the same node', 'warning');
    } else if (d.kind === 'transition') {
      const hit = hitMachine(gx, gy);
      if (hit && hit.state && hit.state.name !== d.from.name) addTransition(S.view.id, d.from.name, hit.state.name);
    }
    renderSide();
    draw();
  }
  function onDouble(e) {
    if (!S.doc) return;
    const [px, py] = local(e);
    const [gx, gy] = toGraph(px, py);
    if (S.view.kind === 'machine') {
      const hit = hitMachine(gx, gy);
      if (hit && hit.state) return enter({ kind: 'flow', scope: hit.state.key });
      if (hit && hit.kind === 'transition') {
        if (hit.t.when === undefined) change(() => { hit.t.when = true; });
        return enter({ kind: 'flow', scope: 'when:' + hit.t.id });
      }
      return addState(gx, gy);
    }
    const hit = hitFlow(gx, gy);
    if (hit && hit.kind === 'node' && hit.box.n && hit.box.n.type === 'stateMachine') return enter({ kind: 'machine', id: hit.box.id });
    if (!hit) openPalette(px, py, gx, gy, null);
  }
  function onWheel(e) {
    e.preventDefault();
    const [px, py] = local(e);
    const cam = camera();
    const [gx, gy] = toGraph(px, py);
    cam.zoom = Math.max(0.2, Math.min(2.5, cam.zoom * Math.pow(1.0015, -e.deltaY)));
    const [nx, ny] = toGraph(px, py);
    cam.x += gx - nx;
    cam.y += gy - ny;
    draw();
  }
  function fit() {
    const boxes = S.view.kind === 'machine' ? states() : [...flowNodes(), resultBox()].filter(Boolean);
    const cam = camera();
    cam.fresh = false;
    if (!boxes.length || !R.canvas || !R.canvas.clientWidth) { cam.fresh = !R.canvas || !R.canvas.clientWidth; return draw(); }
    const x0 = Math.min(...boxes.map(b => b.x)) - 20, y0 = Math.min(...boxes.map(b => b.y));
    const x1 = Math.max(...boxes.map(b => b.x + b.w)), y1 = Math.max(...boxes.map(b => b.y + b.h));
    cam.x = (x0 + x1) / 2;
    cam.y = (y0 + y1) / 2;
    cam.zoom = Math.max(0.4, Math.min(1.25, Math.min((R.canvas.clientWidth - 30) / (x1 - x0 + 1), (R.canvas.clientHeight - 40) / (y1 - y0 + 1))));
    draw();
  }
  function fitIfFresh() { if (S.doc && camera().fresh) fit(); }
  function onKey(e) {
    if (!S.doc) return;
    const k = e.key.toLowerCase();
    if (k === 'delete' || k === 'backspace') deleteSelection();
    else if ((e.ctrlKey || e.metaKey) && k === 'c') copy();
    else if ((e.ctrlKey || e.metaKey) && k === 'v') paste();
    else if (k === 'f') fit();
    else if (k === ' ') { const c = camera(); openPalette(R.canvas.clientWidth / 2, R.canvas.clientHeight / 2, c.x, c.y, null); }
    else if (k === 'escape') { const c = crumbs(); if (c.length > 1) enter(c[c.length - 2].view); }
    else return;
    e.preventDefault();
    e.stopPropagation();
  }

  // ---------------------------------------------------------------- the palette

  const pinTypesOf = type => inputsOf(Object.assign({ type }, TYPES[type].list ? { [TYPES[type].list.key]: [TYPES[type].list.item()] } : {})).map(p => p.type);
  function openPalette(px, py, gx, gy, from) {
    closePalette();
    if (S.view.kind !== 'flow') return;
    const { el } = h;
    // Only types that fit a dragged pin; conditions on states only in transitions' conditions.
    const wantOut = from && from.to ? (from.to.id === '#result' ? from.to.r.type : from.to.pins[from.pin].type) : null;
    const wantIn = from && from.from ? (TYPES[nodeOf(from.from.id).type] || {}).out : null;
    const inCondition = /^when:/.test(S.view.scope);
    const list = Object.entries(TYPES).filter(([type, t]) => (!t.condition || inCondition) && (!wantOut || t.out === wantOut) &&
      (!wantIn || pinTypesOf(type).includes(wantIn)));
    // Parameters, for a value input dragged out: "$Name".
    const params = from && from.to && wantOut !== 'pose'
      ? Object.entries(S.doc.parameters).filter(([, spec]) => paramType(spec) === wantOut).map(([name]) => name) : [];
    const search = el('input', { class: 'field', placeholder: 'Search nodes…', spellcheck: 'false' });
    const rows = el('div', { class: 'an-palette-list' });
    const render = () => {
      const q = search.value.trim().toLowerCase();
      const out = [];
      for (const name of params)
        if (!q || name.toLowerCase().includes(q))
          out.push(el('div', { class: 'menu-item', 'data-pick': '$' + name }, el('span', {}), el('span', {}, `Parameter ${name}`)));
      let group = '';
      for (const [type, t] of list) {
        if (q && !(t.title + ' ' + type + ' ' + t.group).toLowerCase().includes(q)) continue;
        if (t.group !== group && !q) { group = t.group; out.push(el('div', { class: 'menu-title' }, group)); }
        out.push(el('div', { class: 'menu-item', 'data-pick': type }, el('span', {}), el('span', {}, t.title)));
      }
      rows.replaceChildren(...(out.length ? out : [el('div', { class: 'menu-item disabled' }, el('span'), el('span', {}, 'Nothing matches'))]));
    };
    const choose = value => {
      closePalette();
      if (value.startsWith('$')) change(() => { if (from.to.id === '#result') from.to.r.set(value); else setAt(from.to.n, from.to.pins[from.pin].path, value); });
      else addNode(value, gx, gy, from);
    };
    rows.addEventListener('mousedown', ev => {
      const row = ev.target.closest('[data-pick]');
      if (row) { ev.preventDefault(); choose(row.dataset.pick); }
    });
    search.addEventListener('input', render);
    search.addEventListener('keydown', ev => {
      if (ev.key === 'Escape') closePalette();
      if (ev.key === 'Enter') { const first = rows.querySelector('[data-pick]'); if (first) choose(first.dataset.pick); }
      ev.stopPropagation();
    });
    R.palette = el('div', { class: 'menu an-palette' }, search, rows);
    R.canvasWrap.append(R.palette);
    const r = R.canvasWrap.getBoundingClientRect();
    R.palette.style.left = Math.max(0, Math.min(px, r.width - 230)) + 'px';
    R.palette.style.top = Math.max(0, Math.min(py + 24, r.height - 320)) + 'px';
    render();
    setTimeout(() => search.focus());
  }
  function closePalette() {
    if (!R.palette) return;
    R.palette.remove();
    R.palette = null;
  }

  // ---------------------------------------------------------------- the side panel

  function build() {
    if (R.root) return;
    const { el } = h;
    R.root = h.$('#animator');
    R.title = el('div', { class: 'an-title' });
    R.model = el('select', { class: 'field', 'data-anim': 'model' });
    R.model.addEventListener('change', () => h.send('editor.anim.preview.open', { model: R.model.value }));
    R.params = el('div', { class: 'an-params' });
    R.props = el('div', { class: 'an-props' });
    R.errors = el('div', { class: 'an-errors' });
    const addParam = el('span', { class: 'tbtn an-add', role: 'button', title: 'Add a parameter', html: h.icon('plus'), 'data-anim': 'add-param' });
    addParam.addEventListener('click', () => {
      if (!S.doc) return;
      change(() => {
        let name = 'Param', k = 1;
        while (S.doc.parameters[name]) name = `Param${++k}`;
        S.doc.parameters[name] = { type: 'float', default: 0 };
        S.selected = new Set(['param:' + name]);
      });
    });
    R.masks = el('div', { class: 'an-params' });
    R.ik = el('div', { class: 'an-params an-ik', 'data-anim': 'ik' });
    const addMask = el('span', { class: 'tbtn an-add', role: 'button', title: 'Add a bone mask', html: h.icon('plus'), 'data-anim': 'add-mask' });
    addMask.addEventListener('click', () => {
      if (!S.doc) return;
      change(() => {
        const masks = (S.doc.boneMasks = S.doc.boneMasks || {});
        let name = 'Mask', k = 1;
        while (masks[name]) name = `Mask${++k}`;
        masks[name] = { default: 0, bones: {}, propagate: true };
        S.selected = new Set(['mask:' + name]);
      });
    });
    const side = el('div', { class: 'an-side' }, R.title,
      el('div', { class: 'prop' }, el('label', {}, 'Preview'), R.model),
      el('div', { class: 'an-head' }, el('span', {}, 'Parameters'), el('span', { class: 'spacer' }), addParam), R.params,
      el('div', { class: 'an-head' }, el('span', {}, 'Bone Masks'), el('span', { class: 'spacer' }), addMask), R.masks,
      el('div', { class: 'an-head', title: 'Inverse kinematics after the pose, in the scene and the game (not in this preview)' }, 'IK'), R.ik,
      el('div', { class: 'an-head' }, 'Selection'), R.props, R.errors);
    R.crumbs = el('div', { class: 'an-crumbs' });
    R.canvas = el('canvas', { class: 'an-graph', tabindex: '0', 'data-anim': 'canvas' });
    buildEvents();
    R.canvasWrap = el('div', { class: 'an-canvas' }, R.crumbs, R.canvas, R.events);
    R.canvas.addEventListener('mousedown', e => { closePalette(); onDown(e); });
    document.addEventListener('mousemove', e => { if (drag) onMove(e); });
    document.addEventListener('mouseup', e => { if (drag) onUp(e); });
    R.canvas.addEventListener('dblclick', onDouble);
    R.canvas.addEventListener('wheel', onWheel, { passive: false });
    R.canvas.addEventListener('contextmenu', e => e.preventDefault());
    R.canvas.addEventListener('keydown', onKey);
    new ResizeObserver(() => { fitIfFresh(); draw(); }).observe(R.canvas);

    // The preview: its toolbar, then the pane the engine draws (nothing over it).
    const tool = (icon, title, key, onClick) => {
      const b = el('span', { class: 'tbtn', role: 'button', title, html: h.icon(icon), 'data-anim': key });
      b.addEventListener('click', onClick);
      return b;
    };
    const set = fields => h.send('editor.anim.preview.set', fields);
    R.play = tool('play', 'Play / pause', 'play', () => set({ paused: !(S.preview && S.preview.options.paused) }));
    R.restart = tool('refresh', 'From the start', 'restart', () => h.send('editor.anim.preview.restart'));
    R.step = tool('step', 'One frame (paused)', 'step', () => h.send('editor.anim.preview.step', { seconds: 1 / 30 }));
    R.speed = el('select', { class: 'field an-speed', title: 'Playback speed', 'data-anim': 'speed' },
      [0.1, 0.25, 0.5, 1, 1.5, 2].map(v => el('option', { value: v, selected: v === 1 }, `${v}×`)));
    R.speed.addEventListener('change', () => set({ speed: Number(R.speed.value) }));
    const toggle = (label, key, title) => {
      const b = el('span', { class: 'tbtn', role: 'button', title, 'data-anim': key }, label);
      b.addEventListener('click', () => set({ [key]: !(S.preview && S.preview.options[key]) }));
      return b;
    };
    R.bones = toggle('Bones', 'bones', 'Show the skeleton');
    R.grid = toggle('Grid', 'grid', 'Show the ground grid');
    R.moveRoot = toggle('Root', 'moveRoot', 'Root motion moves the character over the ground (else it stays in place)');
    R.status = el('span', { class: 'muted an-status' });
    R.pane = el('div', { class: 'an-preview', id: 'anim-preview', 'data-view': 'anim' });
    R.message = el('div', { class: 'an-preview-message', hidden: true });
    const previewCol = el('div', { class: 'an-preview-col' },
      el('div', { class: 'toolbar' }, R.play, R.restart, R.step, R.speed, el('span', { class: 'divider' }), R.bones, R.grid, R.moveRoot,
        el('span', { class: 'spacer' }), R.status),
      R.message, R.pane);
    R.root.replaceChildren(side, R.canvasWrap, previewCol);
    new ResizeObserver(() => h.reportLayout()).observe(R.pane);
  }

  function renderSide() {
    if (!R.root) return;
    renderTitle();
    renderParams();
    renderMasks();
    renderIk();
    renderProps();
    renderCrumbs();
    const { el } = h;
    R.errors.replaceChildren(...((S.info && S.info.errors) || []).map(e => {
      const row = el('div', { class: 'an-error', role: 'button', title: 'Show it' }, e.node ? el('b', {}, e.node + ': ') : null, e.text);
      row.addEventListener('click', () => reveal(e.node));
      return row;
    }));
  }
  function renderTitle() {
    const { el } = h;
    const saveBtn = el('span', { class: 'tbtn', role: 'button', title: 'Save (Ctrl+S)', 'data-anim': 'save' }, S.dirty ? 'Save •' : 'Saved');
    saveBtn.addEventListener('click', save);
    R.title.replaceChildren(el('div', { class: 'an-title-row' }, el('span', { class: 'an-name' }, S.doc ? (S.doc.name || 'Graph') : 'No graph'),
      el('span', { class: 'spacer' }), S.doc ? saveBtn : null),
    el('span', { class: 'muted an-path', title: S.path }, S.path.replace(/^Assets\//, '')));
    const models = (S.info && S.info.models) || [];
    const current = S.preview && S.preview.ok ? S.preview.model : '';
    R.model.replaceChildren(...(models.length ? models : ['']).map(m => el('option', { value: m, selected: m === current },
      m ? m.replace(/^Assets\//, '') : 'No model with this skeleton')));
  }
  function renderParams() {
    const { el } = h;
    R.inputs = {};
    if (!S.doc) return R.params.replaceChildren();
    const rows = Object.entries(S.doc.parameters).map(([name, spec]) => {
      const type = paramType(spec);
      const def = spec && typeof spec === 'object' ? spec.default : undefined;
      const value = name in S.values ? S.values[name] : def !== undefined ? def : type === 'bool' ? false : type === 'id' ? '' : 0;
      const set = v => { S.values[name] = v; h.send('editor.anim.param', { name, value: v }); };
      let field;
      if (type === 'bool') {
        field = el('input', { type: 'checkbox', class: 'check', checked: !!value, 'data-param': name });
        field.addEventListener('change', () => set(field.checked));
      } else if (type === 'id') {
        field = el('input', { class: 'field', value: typeof value === 'string' ? value : '', 'data-param': name, placeholder: 'ID' });
        field.addEventListener('change', () => set(field.value));
      } else if (type === 'entity') {  // (bound in each Animator; the preview has none)
        field = el('span', { class: 'muted', 'data-param': name, title: 'An entity: each Animator playing the graph binds one' }, 'entity');
      } else {
        field = h.numberRow('', Number(value) || 0, set, 0.01).querySelector('input');
        field.dataset.param = name;
      }
      R.inputs[name] = field;
      const label = el('span', { class: 'an-param-name' + (S.selected.has('param:' + name) ? ' selected' : ''), role: 'button', title: `${name} (${type}): edit` }, name);
      label.addEventListener('click', () => { S.selected = new Set(['param:' + name]); renderSide(); draw(); });
      return el('div', { class: 'prop an-param' }, label, field);
    });
    R.params.replaceChildren(...(rows.length ? rows : [el('div', { class: 'muted' }, 'No parameters')]));
  }
  function renderMasks() {
    const { el } = h;
    if (!R.masks) return;
    const names = Object.keys((S.doc && S.doc.boneMasks) || {});
    R.masks.replaceChildren(...(names.length ? names.map(name => {
      const b = el('span', { class: 'an-param-name' + (S.selected.has('mask:' + name) ? ' selected' : ''), role: 'button' }, name);
      b.addEventListener('click', () => { S.selected = new Set(['mask:' + name]); renderSide(); draw(); });
      return el('div', { class: 'prop an-param' }, b, el('span', { class: 'muted' }, `${Object.keys(S.doc.boneMasks[name].bones || {}).length} bones set`));
    }) : [el('div', { class: 'muted' }, 'No bone masks')]));
  }
  // The graph's IK (its "ik" section): feet, a look, an aim (and hold) and reaches, solved after the
  // pose in the scene and the game. Targets are entity parameters (each Animator binds them to
  // entities), weights a constant or a float/bool parameter.
  function renderIk() {
    const { el } = h;
    if (!R.ik) return;
    if (!S.doc) return R.ik.replaceChildren();
    const ik = S.doc.ik || {};
    const params = Object.entries(S.doc.parameters);
    const entities = params.filter(([, s]) => paramType(s) === 'entity').map(([n]) => n);
    const values = params.filter(([, s]) => paramType(s) === 'float' || paramType(s) === 'bool').map(([n]) => n);
    const edit = fn => change(() => {
      S.doc.ik = S.doc.ik || {};
      fn(S.doc.ik);
      if (!Object.keys(S.doc.ik).length) delete S.doc.ik;
    });
    // An entity parameter named `base` (the one there is, else a new one).
    const newEntity = base => {
      if (S.doc.parameters[base] && paramType(S.doc.parameters[base]) === 'entity') return base;
      let name = base, k = 1;
      while (S.doc.parameters[name]) name = `${base}${++k}`;
      S.doc.parameters[name] = { type: 'entity' };
      return name;
    };
    const rows = [];
    const entity = (label, part, key, base, optional, hint) => {
      const NEW = '#new';
      const cur = typeof part[key] === 'string' ? part[key].replace(/^\$/, '') : '';
      const opts = [...(optional ? [['', 'None']] : []), ...entities.map(n => [n, n]), [NEW, 'New entity parameter']];
      if (cur && !entities.includes(cur)) opts.unshift([cur, `${cur} (missing)`]);
      const row = h.choice(label, opts, cur, v => edit(() => {
        if (v === NEW) v = newEntity(base);
        if (v) part[key] = '$' + v; else delete part[key];
      }));
      row.dataset.ikSlot = key;
      row.title = hint;
      return row;
    };
    const weight = part => {
      const from = typeof part.weight === 'string' ? part.weight.replace(/^\$/, '') : '';
      const row = h.choice('Weight from', [['', 'Constant'], ...values.map(n => [n, n])], from, v => edit(() => {
        if (v) part.weight = '$' + v; else delete part.weight;
      }));
      row.title = 'How much of it: a constant, or a float or bool parameter of the graph (read each frame)';
      const out = [row];
      if (!from) out.push(h.numberRow('Weight', typeof part.weight === 'number' ? part.weight : 1,
        v => edit(() => { part.weight = Math.min(1, Math.max(0, v)); if (part.weight === 1) delete part.weight; }), 0.01));
      return out;
    };
    const text = (label, value, placeholder, set, hint) => {
      const f = el('input', { class: 'field', value: value || '', placeholder });
      f.addEventListener('change', () => edit(() => set(f.value.trim())));
      return el('div', { class: 'prop', title: hint }, el('label', {}, label), f);
    };
    const toggle = (name, label, hint, make) => {
      const b = el('input', { type: 'checkbox', class: 'check', checked: !!ik[name], 'data-ik': name });
      b.addEventListener('change', () => edit(k => { if (b.checked) k[name] = make(); else delete k[name]; }));
      rows.push(el('div', { class: 'prop an-ik-part', title: hint }, el('label', {}, label), b));
      return ik[name] && typeof ik[name] === 'object' ? ik[name] : null;
    };
    const feet = toggle('feet', 'Foot IK', 'Plants the feet on the ground under them: each foot lifted or lowered to it, the hips lowered for the lower foot, the feet tilted to the slope (legs found by name: foot_l, LeftFoot)',
      () => ({ reach: 0.45 }));
    if (feet) {
      rows.push(h.numberRow('Reach', feet.reach !== undefined ? feet.reach : 0.45, v => edit(() => { feet.reach = Math.min(2, Math.max(0.01, v)); }), 0.01));
      rows.push(...weight(feet));
    }
    const look = toggle('look', 'Look', 'The head (with the neck and spine) turns toward an entity', () => ({ target: '$' + newEntity('LookTarget') }));
    if (look) {
      rows.push(entity('Target', look, 'target', 'LookTarget', false, 'The entity it looks at'));
      rows.push(text('Bone', look.bone, 'head', v => { if (v && v !== 'head') look.bone = v; else delete look.bone; }, 'The bone that looks (the spine below it turns with it)'));
      rows.push(...weight(look));
    }
    const aim = toggle('aim', 'Aim', 'A bone (the hand holding a weapon) aims at an entity, or is held at one (a sight before the eyes) pointing where it faces',
      () => ({ hold: '$' + newEntity('AimHold'), bone: 'hand_r' }));
    if (aim) {
      rows.push(entity('Target', aim, 'target', 'AimTarget', true, 'The entity it aims at (none: where the hold faces)'));
      rows.push(entity('Hold', aim, 'hold', 'AimHold', true,
        'The entity the bone is held at (its position), pointing where it faces (its -Z, as a camera looks) unless a target is set; the other hand keeps its grip'));
      rows.push(text('Bone', aim.bone, 'hand_r', v => { aim.bone = v || 'hand_r'; }, 'The bone that aims: the hand holding the weapon'));
      const axis = h.vec3Row('Axis', Array.isArray(aim.axis) ? aim.axis : [0, 0, 0], v => edit(() => {
        if (v.some(x => x !== 0)) aim.axis = [...v]; else delete aim.axis;
      }));
      axis.title = 'Where the bone points, in its own space (the weapon\u2019s barrel); 0 0 0: ahead as the clip holds it';
      rows.push(axis);
      rows.push(text('Support', aim.support, 'the other hand', v => { if (v) aim.support = v; else delete aim.support; },
        'The hand that keeps its grip while held (empty: the other hand; none: no hand)'));
      const spine = h.numberRow('Spine', aim.spine !== undefined ? aim.spine : 1, v => edit(() => {
        aim.spine = Math.min(1, Math.max(0, v));
        if (aim.spine === 1) delete aim.spine;
      }), 0.01);
      spine.title = 'How much the spine turns toward the target: 1 the spine aims, 0 the arms alone bring the weapon to the hold (the body as the clip has it)';
      rows.push(spine);
      rows.push(...weight(aim));
    }
    const reaches = Array.isArray(ik.reach) ? ik.reach : [];
    reaches.forEach((r, i) => {
      rows.push(el('div', { class: 'prop an-ik-part' }, el('label', {}, `Reach ${i + 1}`),
        h.actionButton('Remove', () => edit(k => { k.reach.splice(i, 1); if (!k.reach.length) delete k.reach; }))));
      rows.push(text('Bone', r.bone, 'hand_l', v => { r.bone = v || 'hand_l'; }, 'The hand that reaches ("left hand" / "right hand": found by name)'));
      rows.push(entity('Target', r, 'target', 'HandTarget', false, 'The entity it reaches'));
      rows.push(...weight(r));
    });
    if (reaches.length < 4)
      rows.push(el('div', { class: 'prop' }, el('label', {}, ''), h.actionButton('Add Reach', () => edit(k => {
        (k.reach = Array.isArray(k.reach) ? k.reach : []).push({ bone: 'hand_l', target: '$' + newEntity('HandTarget') });
      }))));
    R.ik.replaceChildren(...rows);
  }
  function renderCrumbs() {
    const { el } = h;
    if (!S.doc) return R.crumbs.replaceChildren();
    const list = crumbs();
    R.crumbs.replaceChildren(...list.flatMap((c, i) => {
      const b = el('span', { class: 'an-crumb' + (i === list.length - 1 ? ' current' : ''), role: 'button' }, c.label);
      b.addEventListener('click', () => enter(c.view));
      return i ? [el('span', { class: 'muted' }, '›'), b] : [b];
    }));
  }
  // The selection's fields: a node's properties and inputs, a state, a transition, a parameter.
  function renderProps() {
    const { el } = h;
    const sel = [...S.selected];
    if (!S.doc || sel.length !== 1) return R.props.replaceChildren(el('div', { class: 'muted' }, sel.length ? `${sel.length} selected` : 'Nothing selected'));
    const key = sel[0];
    const rows = [];
    const num = (label, value, set, step = 0.01) => h.numberRow(label, Number(value) || 0, v => change(() => set(v)), step);
    const check = (label, value, set) => {
      const b = el('input', { type: 'checkbox', class: 'check', checked: !!value });
      b.addEventListener('change', () => change(() => set(b.checked)));
      return el('div', { class: 'prop' }, el('label', {}, label), b);
    };
    const pick = (label, options, value, set) => h.choice(label, options.map(o => Array.isArray(o) ? o : [o, o]), value, v => change(() => set(v)));
    const textField = (label, value, set) => {
      const f = el('input', { class: 'field', value: value || '' });
      f.addEventListener('change', () => change(() => set(f.value)));
      return el('div', { class: 'prop' }, el('label', {}, label), f);
    };
    const buttons = (...b) => el('div', { class: 'prop' }, el('label', {}, ''), el('div', { class: 'row-buttons' }, ...b.filter(Boolean)));
    if (key.startsWith('mask:')) {
      const name = key.slice(5), masks = S.doc.boneMasks || {}, m = masks[name];
      if (!m) return R.props.replaceChildren();
      rows.push(el('div', { class: 'an-sel-title' }, 'Bone Mask'));
      rows.push(textField('Name', name, v => {
        v = v.trim();
        if (!v || masks[v]) return;
        S.doc.boneMasks = Object.fromEntries(Object.entries(masks).map(([k, x]) => [k === name ? v : k, x]));
        for (const n of Object.values(S.doc.nodes)) if (n.type === 'boneMask' && n.mask === name) n.mask = v;
        S.selected = new Set(['mask:' + v]);
      }));
      rows.push(num('Default', m.default || 0, v => { m.default = Math.min(1, Math.max(0, v)); }));
      rows.push(check('Propagate', m.propagate !== false, v => { m.propagate = v; }));
      rows.push(el('div', { class: 'muted an-hint' }, 'A weight per bone (blank: its parent\u2019s with Propagate, else the default).'));
      // The skeleton's bones, indented by depth.
      const bones = (S.info && S.info.bones) || [];
      const depth = bones.map(() => 0);
      bones.forEach(([, parent], i) => { if (parent >= 0) depth[i] = depth[parent] + 1; });
      m.bones = m.bones && typeof m.bones === 'object' ? m.bones : {};
      bones.forEach(([bone], i) => {
        const f = el('input', { class: 'field an-bone-weight', value: m.bones[bone] !== undefined ? h.fmt(m.bones[bone], 2) : '', placeholder: '\u2013', 'data-bone': bone });
        f.addEventListener('change', () => change(() => {
          if (f.value.trim() === '') delete m.bones[bone];
          else m.bones[bone] = Math.min(1, Math.max(0, Number(f.value) || 0));
        }));
        const label = el('label', { class: 'an-bone', title: bone }, bone);
        label.dataset.depth = String(Math.min(depth[i], 12));
        rows.push(el('div', { class: 'prop an-bone-row' }, label, f));
      });
      rows.push(buttons(h.actionButton('Delete', () => change(() => {
        delete S.doc.boneMasks[name];
        for (const n of Object.values(S.doc.nodes)) if (n.type === 'boneMask' && n.mask === name) delete n.mask;
        S.selected.clear();
      }))));
    } else if (key.startsWith('param:')) {
      const name = key.slice(6), spec = S.doc.parameters[name];
      if (spec === undefined) return R.props.replaceChildren();
      const type = paramType(spec);
      const obj = () => (S.doc.parameters[name] = S.doc.parameters[name] && typeof S.doc.parameters[name] === 'object' ? S.doc.parameters[name] : { type });
      rows.push(el('div', { class: 'an-sel-title' }, 'Parameter'));
      rows.push(textField('Name', name, v => renameParam(name, v.trim())));
      rows.push(pick('Type', ['float', 'bool', 'id', 'entity'], type, v => {
        S.doc.parameters[name] = v === 'entity' ? { type: v } : { type: v, default: v === 'bool' ? false : v === 'id' ? '' : 0 };
      }));
      if (type === 'entity') rows.push(el('div', { class: 'muted an-hint' }, 'An entity reference: each Animator playing the graph binds one (scripts: Animation.SetEntity); the IK follows it.'));
      else if (type === 'bool') rows.push(check('Default', spec.default, v => { obj().default = v; }));
      else if (type === 'id') rows.push(textField('Default', spec.default, v => { obj().default = v; }));
      else rows.push(num('Default', spec.default, v => { obj().default = v; }));
      rows.push(buttons(h.actionButton('Delete', () => change(() => deleteParam(name)))));
    } else if (key.startsWith('state:')) {
      const st = stateByKey(key.slice(6));
      if (!st) return R.props.replaceChildren();
      const sm = nodeOf(st.machine);
      rows.push(el('div', { class: 'an-sel-title' }, 'State'));
      rows.push(textField('Name', st.name, v => renameState(st.machine, st.name, v.trim())));
      rows.push(buttons(
        h.actionButton(sm.default === st.name ? 'Default ✓' : 'Make Default', () => change(() => { sm.default = st.name; })),
        h.actionButton('Edit Pose', () => enter({ kind: 'flow', scope: st.machine + ':' + st.name }))));
      // The events it emits: on entering, on leaving, every update while fully in it.
      for (const [k, label] of [['entry', 'Entry events'], ['exit', 'Exit events'], ['fullyIn', 'Events while in']])
        rows.push(textField(label, ((st.s.events || {})[k] || []).join(', '), v => {
          const ids = v.split(',').map(x => x.trim()).filter(Boolean);
          st.s.events = Object.assign({}, st.s.events);
          if (ids.length) st.s.events[k] = ids; else delete st.s.events[k];
          if (!Object.keys(st.s.events).length) delete st.s.events;
        }));
      rows.push(buttons(h.actionButton(st.s.entry !== undefined ? 'Entry Condition' : 'Add Entry Condition', () => {
        if (st.s.entry === undefined) change(() => { st.s.entry = true; });
        enter({ kind: 'flow', scope: `entry:${st.machine}:${st.name}` });
      })));
      rows.push(el('div', { class: 'muted an-hint' }, 'Drag from a state’s right edge (or Ctrl+drag it) onto another state: a transition.'));
    } else if (key.startsWith('transition:')) {
      const tr = transitionById(key.slice(11));
      if (!tr) return R.props.replaceChildren();
      const t = tr.t;
      rows.push(el('div', { class: 'an-sel-title' }, `${tr.from} → ${t.to}`));
      rows.push(num('Duration', t.duration !== undefined ? t.duration : 0, v => { t.duration = Math.max(0, v); }));
      rows.push(num('Offset', t.offset || 0, v => { t.offset = v; }));
      rows.push(pick('Easing', ['linear', 'smooth'], t.easing || 'linear', v => { t.easing = v; }));
      rows.push(pick('Root Motion', ['blend', 'additive', 'ignoreSource', 'ignoreTarget'], t.rootMotion || 'blend', v => { t.rootMotion = v; }));
      rows.push(pick('Match', [['', 'Nothing'], ['syncEventIndex', 'Sync event index'], ['syncEventId', 'Sync event ID'], ['seconds', 'Seconds']],
        t.match || '', v => { if (v) t.match = v; else delete t.match; }));
      rows.push(check('Synchronized', t.sync, v => { t.sync = v; }));
      rows.push(check('Clamp', t.clamp, v => { t.clamp = v; }));
      rows.push(buttons(
        h.actionButton(t.when !== undefined ? 'Edit Condition' : 'Add Condition', () => {
          if (t.when === undefined) change(() => { t.when = true; });
          enter({ kind: 'flow', scope: 'when:' + t.id });
        }),
        h.actionButton('Delete', () => { S.selected = new Set([key]); deleteSelection(); })));
    } else if (key === '#result') {
      const r = resultOf(S.view.scope);
      rows.push(el('div', { class: 'an-sel-title' }, r ? r.label : 'Result'));
      if (r && r.type === 'bool') rows.push(inputRow({ get: r.get, set: r.set, type: 'bool', label: 'Value' }));
    } else {
      const n = nodeOf(key);
      if (!n) return R.props.replaceChildren();
      const t = TYPES[n.type] || {};
      rows.push(el('div', { class: 'an-sel-title' }, `${t.title || n.type} · ${key}`));
      for (const [k, kind, def, options] of t.props || []) {
        const v = n[k] !== undefined ? n[k] : def;
        if (kind === 'float' || kind === 'int') rows.push(num(k, v, x => { n[k] = kind === 'int' ? Math.round(x) : x; }, kind === 'int' ? 1 : 0.01));
        else if (kind === 'bool') rows.push(check(k, v, x => { n[k] = x; }));
        else if (kind === 'enum') rows.push(pick(k, options, v, x => { n[k] = x; }));
        else if (kind === 'clip') rows.push(pick('clip', [...new Set([...((S.info && S.info.clips) || []), v])], v, x => { n.clip = x; }));
        else if (kind === 'state') rows.push(pick('default', Object.keys(n.states || {}), v, x => { n.default = x; }));
        else if (kind === 'maskName') rows.push(pick('mask', [['', '(its own bones)'], ...Object.keys(S.doc.boneMasks || {})], v, x => { if (x) n.mask = x; else delete n.mask; }));
        else if (kind === 'string') rows.push(textField(k, v, x => { n[k] = x; }));
        else if (kind === 'strings') rows.push(textField(k, (v || []).join(', '), x => { n[k] = x.split(',').map(s => s.trim()).filter(Boolean); }));
        else if (kind === 'pair') {
          const p = Array.isArray(v) ? v : def;
          rows.push(num(k + ' (a)', p[0], x => { n[k] = [x, (Array.isArray(n[k]) ? n[k] : p)[1]]; }));
          rows.push(num(k + ' (b)', p[1], x => { n[k] = [(Array.isArray(n[k]) ? n[k] : p)[0], x]; }));
        }
      }
      // Its value inputs: a link, a parameter or a constant (pose inputs: wires only).
      for (const pin of inputsOf(n))
        if (pin.type !== 'pose') rows.push(inputRow({ get: () => getAt(n, pin.path), set: v => setAt(n, pin.path, v), type: pin.type, label: pin.label }));
      if (t.list) {
        const items = n[t.list.key] = Array.isArray(n[t.list.key]) ? n[t.list.key] : [];
        items.forEach((it, i) => (t.list.props || []).forEach(([pk, kind, pd, options]) => {
          const label = `${t.list.label(it, i).replace(/ \(.*\)$/, '')} ${pk}`;
          const v = it[pk] !== undefined ? it[pk] : pd;
          if (kind === 'bool') rows.push(check(label, v, x => { it[pk] = x; }));
          else if (kind === 'enum') rows.push(pick(label, options, v, x => { it[pk] = x; }));
          else if (kind === 'pair') {
            const pv = Array.isArray(v) ? v : pd;
            rows.push(num(label + ' x', pv[0], x => { it[pk] = [x, (Array.isArray(it[pk]) ? it[pk] : pv)[1]]; }));
            rows.push(num(label + ' y', pv[1], x => { it[pk] = [(Array.isArray(it[pk]) ? it[pk] : pv)[0], x]; }));
          } else rows.push(num(label, v, x => { it[pk] = x; }));
        }));
        if (n.type === 'blend2d') rows.push(blendSpace(n));
        rows.push(buttons(
          h.actionButton('Add ' + t.list.key.replace(/s$/, ''), () => change(() => items.push(t.list.item()))),
          items.length > (t.list.key === 'options' ? 1 : 2) ? h.actionButton('Remove last', () => change(() => items.pop())) : null));
      }
      if (n.type === 'stateMachine') rows.push(buttons(h.actionButton('Open', () => enter({ kind: 'machine', id: key }))));
      if (n.type === 'boneMask' && n.mask) rows.push(buttons(h.actionButton('Edit Mask', () => { S.selected = new Set(['mask:' + n.mask]); renderSide(); })));
      const error = errorFor(key);
      if (error) rows.push(el('div', { class: 'an-error' }, error));
    }
    R.props.replaceChildren(...rows);
  }
  // A blend space: its points (and their Delaunay triangles, as the compiler makes them) and the
  // input point, from its parameters; dragging the point sets them (the preview follows).
  function delaunay(pts) {
    const n = pts.length, tris = [];
    let span = 1e-9;
    for (const p of pts) span = Math.max(span, Math.abs(p[0]), Math.abs(p[1]));
    const x = pts.map((p, i) => p[0] + span * 1e-6 * i), y = pts.map((p, i) => p[1] + span * 1e-6 * (i * i % 7));
    const cross = (a, b, c) => (x[b] - x[a]) * (y[c] - y[a]) - (y[b] - y[a]) * (x[c] - x[a]);
    for (let a = 0; a < n; ++a) for (let b = a + 1; b < n; ++b) for (let c = b + 1; c < n; ++c) {
      const area = cross(a, b, c);
      if (Math.abs(area) < 1e-12 * span * span) continue;
      const [i0, i1, i2] = area < 0 ? [a, c, b] : [a, b, c];
      let empty = true;
      for (let d = 0; d < n && empty; ++d) {
        if (d === i0 || d === i1 || d === i2) continue;
        const ax = x[i0] - x[d], ay = y[i0] - y[d], bx = x[i1] - x[d], by = y[i1] - y[d], cx = x[i2] - x[d], cy = y[i2] - y[d];
        empty = (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) + (cx * cx + cy * cy) * (ax * by - bx * ay) <= 0;
      }
      if (empty) tris.push([i0, i1, i2]);
    }
    return tris;
  }
  function blendSpace(n) {
    const { el } = h;
    const canvas = el('canvas', { class: 'an-space', width: 220, height: 160, 'data-anim': 'blend-space' });
    const pts = (n.sources || []).map(s => (Array.isArray(s.value) ? s.value : [0, 0]).map(Number));
    const param = v => typeof v === 'string' && v.startsWith('$') ? v.slice(1) : null;
    const px = param(n.x), py = param(n.y);
    const valueOf = (name, fallback) => {
      if (!name) return fallback;
      if (S.state && S.state.params && name in S.state.params) return Number(S.state.params[name]);
      const spec = S.doc.parameters[name];
      return name in S.values ? Number(S.values[name]) : Number(spec && spec.default) || 0;
    };
    const bounds = () => {
      const xs = pts.map(p => p[0]), ys = pts.map(p => p[1]);
      const x0 = Math.min(...xs), x1 = Math.max(...xs), y0 = Math.min(...ys), y1 = Math.max(...ys);
      const pad = Math.max(x1 - x0, y1 - y0, 1) * 0.12;
      return [x0 - pad, x1 + pad, y0 - pad, y1 + pad];
    };
    const [x0, x1, y0, y1] = pts.length ? bounds() : [0, 1, 0, 1];
    const toC = p => [(p[0] - x0) / (x1 - x0) * 220, 160 - (p[1] - y0) / (y1 - y0) * 160];
    const fromC = (cx, cy) => [x0 + cx / 220 * (x1 - x0), y0 + (160 - cy) / 160 * (y1 - y0)];
    const paintSpace = () => {
      const g = canvas.getContext('2d');
      g.fillStyle = '#1a1a1a';
      g.fillRect(0, 0, 220, 160);
      g.strokeStyle = '#3a3a3a';
      for (const [a, b, c] of delaunay(pts)) {
        g.beginPath();
        g.moveTo(...toC(pts[a])); g.lineTo(...toC(pts[b])); g.lineTo(...toC(pts[c])); g.closePath();
        g.stroke();
      }
      g.font = '10px system-ui, sans-serif';
      pts.forEach((p, i) => {
        const [cx, cy] = toC(p);
        g.fillStyle = '#6fbf73';
        g.beginPath(); g.arc(cx, cy, 3.5, 0, Math.PI * 2); g.fill();
        g.fillStyle = '#aaa';
        g.fillText(String(i + 1), cx + 5, cy - 4);
      });
      const q = [valueOf(px, Number(n.x) || 0), valueOf(py, Number(n.y) || 0)];
      const [qx, qy] = toC(q);
      g.strokeStyle = '#4a90d9';
      g.lineWidth = 2;
      g.beginPath(); g.arc(qx, qy, 5, 0, Math.PI * 2); g.stroke();
      g.lineWidth = 1;
    };
    let dragging = false;
    const set = e => {
      const r = canvas.getBoundingClientRect();
      const [vx, vy] = fromC((e.clientX - r.left) * 220 / r.width, (e.clientY - r.top) * 160 / r.height);
      for (const [name, v] of [[px, vx], [py, vy]])
        if (name) { S.values[name] = +v.toFixed(3); h.send('editor.anim.param', { name, value: S.values[name] }); }
      paintSpace();
    };
    canvas.addEventListener('mousedown', e => { if (px || py) { dragging = true; set(e); } });
    document.addEventListener('mousemove', e => { if (dragging) set(e); });
    document.addEventListener('mouseup', () => { dragging = false; });
    paintSpace();
    R.spacePaint = paintSpace;
    return el('div', { class: 'prop an-space-row' }, el('label', {}, px || py ? 'Blend space (drag)' : 'Blend space'), canvas);
  }

  // An input's source: a linked node (drawn as a wire), a parameter, a constant, or none.
  function inputRow(pin) {
    const { el } = h;
    const v = pin.get();
    const params = Object.entries(S.doc.parameters).filter(([, s]) => paramType(s) === pin.type).map(([name]) => name);
    const mode = isRef(v) ? 'link' : typeof v === 'string' && v.startsWith('$') ? v : v === undefined || v === null ? '' : 'const';
    const options = [['', 'None'], ...params.map(p => ['$' + p, 'Parameter ' + p])];
    if (pin.type !== 'id' && pin.type !== 'mask') options.push(['const', 'Constant']);
    if (isRef(v)) options.push(['link', 'Node ' + refId(v)]);
    const sel = h.choice(pin.label, options, mode, x => change(() => {
      if (x === '') pin.set(undefined);
      else if (x === 'const') pin.set(pin.type === 'bool' ? true : 0);
      else if (x !== 'link') pin.set(x);
    }));
    if (mode !== 'const') return sel;
    if (pin.type === 'bool') {
      const b = el('input', { type: 'checkbox', class: 'check', checked: !!v });
      b.addEventListener('change', () => change(() => pin.set(b.checked)));
      return el('div', {}, sel, el('div', { class: 'prop' }, el('label', {}, ''), b));
    }
    return el('div', {}, sel, h.numberRow('', Number(v) || 0, x => change(() => pin.set(x)), 0.01));
  }
  // An error's node: its view, selected.
  function reveal(id) {
    if (!id || !S.doc) return;
    const base = id.replace(/\/.*$/, '');
    if (nodeOf(base)) { enter({ kind: 'flow', scope: editorOf(nodeOf(base)).graph }); S.selected = new Set([base]); }
    else if (stateByKey(base)) { enter({ kind: 'machine', id: stateByKey(base).machine }); S.selected = new Set(['state:' + base]); }
    else if (transitionById(base)) { enter({ kind: 'machine', id: transitionById(base).machine }); S.selected = new Set(['transition:' + base]); }
    renderSide();
    draw();
  }
  function renderPreview() {
    if (!R.root) return;
    const p = S.preview, o = p ? p.options : {};
    R.play.innerHTML = h.icon(o.paused ? 'play' : 'pause');
    for (const [key, b] of [['bones', R.bones], ['grid', R.grid], ['moveRoot', R.moveRoot]]) b.classList.toggle('on', !!o[key]);
    if (o.speed !== undefined) R.speed.value = String(o.speed);
    const message = p && !p.ok ? (p.error || 'Nothing to preview') : '';
    R.message.textContent = message;
    R.message.hidden = !message;
  }

  // ---------------------------------------------------------------- events (clip markup)

  // FNV-1a, as the engine hashes event names (the sampled events come as ids).
  const fnv = name => { let x = 2166136261; for (const b of new TextEncoder().encode(name)) { x ^= b; x = Math.imul(x, 16777619) >>> 0; } return x >>> 0; };
  const E = { model: '', file: '', clips: [], markup: { clips: {} }, clip: '', sel: null, error: '', seen: 0, flash: new Map() };
  const markupOf = clip => (E.markup.clips[clip] = E.markup.clips[clip] || { sync: [], events: [] });
  function saveEvents() {
    // Clips with nothing left go from the file.
    for (const [name, c] of Object.entries(E.markup.clips))
      if (!(c.sync || []).length && !(c.events || []).length) delete E.markup.clips[name];
    h.send('editor.anim.events.save', { model: E.model, text: JSON.stringify(E.markup, null, 2) + '\n' });
  }
  function onEvents(m) {
    if (m.model !== E.model) return;
    E.file = m.file;
    E.error = m.error || '';
    E.clips = m.clips || [];
    if (!E.error) {
      try { E.markup = m.text ? JSON.parse(m.text) : { clips: {} }; } catch { E.markup = { clips: {} }; }
      E.markup.clips = E.markup.clips && typeof E.markup.clips === 'object' ? E.markup.clips : {};
    }
    if (!E.clips.some(c => c.name === E.clip)) E.clip = E.clips.length ? E.clips[0].name : '';
    renderEvents();
  }
  function renderEvents() {
    if (!R.events) return;
    const { el } = h;
    R.eventClip.replaceChildren(...(E.clips.length ? E.clips : [{ name: '' }]).map(c => el('option', { value: c.name, selected: c.name === E.clip },
      c.name ? `${c.name}${c.duration ? ` (${h.fmt(c.duration, 2)} s)` : ''}` : 'No clips')));
    R.eventInfo.textContent = E.error ? E.error : E.model ? (E.file || '').replace(/^Assets\//, '') : 'No preview model';
    R.eventInfo.classList.toggle('an-error', !!E.error);
    // The selection's fields.
    const rows = [];
    const item = E.sel && E.clip && (markupOf(E.clip)[E.sel.row] || [])[E.sel.i];
    if (item) {
      const set = fn => { fn(item); saveEvents(); renderEvents(); drawTimeline(); };
      const text = (label, value, apply) => { const f = el('input', { class: 'field', value: value || '' }); f.addEventListener('change', () => set(it => apply(it, f.value.trim()))); return el('label', { class: 'an-ev-field' }, label, f); };
      const num = (label, value, apply) => { const f = el('input', { class: 'field', value: h.fmt(value || 0, 3) }); f.addEventListener('change', () => set(it => apply(it, Math.min(1, Math.max(0, Number(f.value) || 0))))); return el('label', { class: 'an-ev-field' }, label, f); };
      rows.push(text('ID', item.id, (it, v) => { it.id = v || 'Event'; }), num('At', item.at, (it, v) => { it.at = Math.min(v, 0.9999); }));
      if (E.sel.row === 'events') {
        const type = el('select', { class: 'field' }, ['marker', 'foot'].map(t => el('option', { value: t, selected: (item.type || 'marker') === t }, t)));
        type.addEventListener('change', () => set(it => { it.type = type.value; if (type.value !== 'foot') delete it.foot; else it.foot = it.foot || 'left'; }));
        rows.push(el('label', { class: 'an-ev-field' }, 'Type', type));
        if (item.type === 'foot') {
          const foot = el('select', { class: 'field' }, ['left', 'right'].map(t => el('option', { value: t, selected: item.foot === t }, t)));
          foot.addEventListener('change', () => set(it => { it.foot = foot.value; }));
          rows.push(el('label', { class: 'an-ev-field' }, 'Foot', foot));
        }
        rows.push(num('Duration', item.duration, (it, v) => { if (v > 0) it.duration = v; else delete it.duration; }));
      }
      rows.push(h.actionButton('Delete', () => deleteEventItem()));
    } else {
      rows.push(el('span', { class: 'muted' }, E.clip ? 'Double-click the top row: a sync event; the bottom row: an event. Drag to move.' : ''));
    }
    R.eventFields.replaceChildren(...rows);
    drawTimeline();
  }
  function deleteEventItem() {
    if (!E.sel || !E.clip) return;
    markupOf(E.clip)[E.sel.row].splice(E.sel.i, 1);
    E.sel = null;
    saveEvents();
    renderEvents();
  }
  const ROWS = { sync: [4, 22], events: [28, 58] };  // y ranges of the timeline's rows
  function drawTimeline() {
    const c = R.eventCanvas;
    if (!c || !c.clientWidth) return;
    const w = c.clientWidth, hh = c.clientHeight, dpr = window.devicePixelRatio || 1;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(hh * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(hh * dpr); }
    const g = c.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = '#1a1a1a';
    g.fillRect(0, 0, w, hh);
    const x = p => 8 + p * (w - 16);
    g.fillStyle = '#232323';
    g.fillRect(8, ROWS.sync[0], w - 16, ROWS.sync[1] - ROWS.sync[0]);
    g.fillRect(8, ROWS.events[0], w - 16, ROWS.events[1] - ROWS.events[0]);
    g.strokeStyle = '#333';
    g.beginPath();
    for (let i = 0; i <= 10; ++i) { g.moveTo(x(i / 10) + 0.5, ROWS.sync[0]); g.lineTo(x(i / 10) + 0.5, ROWS.events[1]); }
    g.stroke();
    if (!E.clip) return;
    g.font = '10px system-ui, sans-serif';
    g.textBaseline = 'middle';
    const m = markupOf(E.clip);
    (m.sync || []).forEach((e, i) => {
      const sel = E.sel && E.sel.row === 'sync' && E.sel.i === i;
      g.fillStyle = sel ? '#4a90d9' : '#d6a548';
      g.fillRect(x(e.at || 0), ROWS.sync[0], 2, ROWS.sync[1] - ROWS.sync[0]);
      g.fillText(e.id || '', x(e.at || 0) + 4, (ROWS.sync[0] + ROWS.sync[1]) / 2);
    });
    const now = performance.now();
    (m.events || []).forEach((e, i) => {
      const sel = E.sel && E.sel.row === 'events' && E.sel.i === i;
      const lit = now - (E.flash.get(fnv(e.id || '')) || -1e9) < 250;  // sampled lately
      g.fillStyle = sel ? '#4a90d9' : lit ? '#9be29e' : e.type === 'foot' ? '#6aa8e6' : '#c792ea';
      const x0 = x(e.at || 0);
      if (e.duration > 0) g.fillRect(x0, ROWS.events[0] + 4, Math.max(2, x(Math.min(1, (e.at || 0) + e.duration)) - x0), 10);
      g.beginPath();
      g.moveTo(x0, ROWS.events[0] + 2);
      g.lineTo(x0 + 5, ROWS.events[0] + 9);
      g.lineTo(x0 - 5, ROWS.events[0] + 9);
      g.fill();
      g.fillRect(x0, ROWS.events[0] + 2, 1.5, ROWS.events[1] - ROWS.events[0] - 4);
      g.fillText((e.id || '') + (e.type === 'foot' ? ` (${e.foot === 'right' ? 'R' : 'L'})` : ''), x0 + 4, ROWS.events[1] - 10);
    });
    // Playheads: the live clip nodes playing this clip.
    if (S.doc)
      for (const [id, n] of Object.entries(S.doc.nodes))
        if (n.type === 'clip' && n.clip === E.clip) {
          const d = S.debug.get(id);
          if (!d || !(d[0] & 2)) continue;
          g.fillStyle = '#7fd37f';
          g.fillRect(x(Math.min(1, d[1])), 0, 2, hh);
        }
  }
  function timelineHit(px, py) {
    if (!E.clip) return null;
    const w = R.eventCanvas.clientWidth, x = p => 8 + p * (w - 16);
    const row = py < ROWS.sync[1] + 2 ? 'sync' : 'events';
    const list = markupOf(E.clip)[row] || [];
    for (let i = list.length - 1; i >= 0; --i) if (Math.abs(x(list[i].at || 0) - px) < 6) return { row, i };
    return { row, i: -1 };
  }
  function buildEvents() {
    const { el } = h;
    R.eventClip = el('select', { class: 'field an-ev-clip', 'data-anim': 'event-clip' });
    R.eventClip.addEventListener('change', () => { E.clip = R.eventClip.value; E.sel = null; renderEvents(); });
    R.eventInfo = el('span', { class: 'muted an-ev-info' });
    R.eventCanvas = el('canvas', { class: 'an-ev-timeline', tabindex: '0', 'data-anim': 'timeline' });
    R.eventFields = el('div', { class: 'an-ev-fields' });
    R.events = el('div', { class: 'an-events' },
      el('div', { class: 'an-ev-head' }, el('span', { class: 'an-head' }, 'Events'), R.eventClip, el('span', { class: 'spacer' }), R.eventInfo),
      R.eventCanvas, R.eventFields);
    let evDrag = null;
    const toP = px => Math.min(0.9999, Math.max(0, (px - 8) / (R.eventCanvas.clientWidth - 16)));
    R.eventCanvas.addEventListener('mousedown', e => {
      R.eventCanvas.focus();
      const r = R.eventCanvas.getBoundingClientRect();
      const hit = timelineHit(e.clientX - r.left, e.clientY - r.top);
      E.sel = hit && hit.i >= 0 ? hit : null;
      evDrag = E.sel ? { moved: false } : null;
      renderEvents();
    });
    document.addEventListener('mousemove', e => {
      if (!evDrag || !E.sel) return;
      const r = R.eventCanvas.getBoundingClientRect();
      markupOf(E.clip)[E.sel.row][E.sel.i].at = Math.round(toP(e.clientX - r.left) * 1000) / 1000;
      evDrag.moved = true;
      drawTimeline();
    });
    document.addEventListener('mouseup', () => {
      if (evDrag && evDrag.moved) { saveEvents(); renderEvents(); }
      evDrag = null;
    });
    R.eventCanvas.addEventListener('dblclick', e => {
      if (!E.clip) return;
      const r = R.eventCanvas.getBoundingClientRect();
      const hit = timelineHit(e.clientX - r.left, e.clientY - r.top);
      if (!hit || hit.i >= 0) return;
      const list = (markupOf(E.clip)[hit.row] = markupOf(E.clip)[hit.row] || []);
      list.push(hit.row === 'sync' ? { id: 'Sync', at: toP(e.clientX - r.left) } : { id: 'Event', at: toP(e.clientX - r.left) });
      E.sel = { row: hit.row, i: list.length - 1 };
      saveEvents();
      renderEvents();
    });
    R.eventCanvas.addEventListener('keydown', e => {
      if (e.key === 'Delete' || e.key === 'Backspace') { deleteEventItem(); e.preventDefault(); e.stopPropagation(); }
    });
    new ResizeObserver(drawTimeline).observe(R.eventCanvas);
  }
  function readEvents(model) {
    if (!model || model === E.model) return;
    E.model = model;
    E.sel = null;
    E.markup = { clips: {} };
    h.send('editor.anim.events.read', { model });
  }

  // ---------------------------------------------------------------- the tab

  function init(helpers) { h = helpers; }

  function shown(on) {
    S.shown = on;
    if (on) build();
    if (on && S.path && !S.doc) h.send('editor.anim.graph.open', { path: S.path });
    if (!on) closePalette();
    h.reportLayout();
    if (on) { renderSide(); fitIfFresh(); draw(); }
  }
  // A graph opened from the Project panel or an Animator component.
  function open(path) {
    if (S.path !== path) {
      Object.assign(S, { path, doc: null, info: null, values: {}, undo: [], redo: [], views: {}, dirty: false, view: { kind: 'flow', scope: 'root' } });
      S.selected.clear();
    }
    h.showTab('scene', 'animator');
    h.send('editor.anim.graph.open', { path });
  }
  function onGraph(m) {
    if (m.path !== S.path) return;
    if (m.saveError) return h.setStatus(m.saveError, 'error');
    if (m.text !== undefined && (!S.doc || !S.dirty)) {
      try {
        normalize(JSON.parse(m.text));
        S.view = { kind: 'flow', scope: 'root' };
        S.views = {};
        // Compiled again as normalized: the engine's node ids are then the canvas's (live overlay).
        h.send('editor.anim.graph.compile', { path: S.path, text: text() });
      } catch (err) {
        S.doc = null;
        h.setStatus(`${S.path}: ${err.message}`, 'error');
      }
    }
    S.info = m;
    if (m.saved) { S.dirty = false; h.setStatus(`Saved ${S.path}`); }
    renderSide();
    fitIfFresh();
    draw();
  }
  function onPreview(m) {
    S.preview = m;
    if (m.ok) readEvents(m.model);
    renderPreview();
    if (R.root) renderTitle();
  }
  function onState(m) {
    S.state = m;
    S.debug = new Map((m.nodes || []).map(n => [n[0], n.slice(1)]));
    const now = performance.now();
    for (const e of m.events || []) { E.flash.set(e[0] >>> 0, now); ++E.seen; }
    if (S.shown) drawTimeline();
    if (R.status) {
      const state = (m.state || '').replace(/^.*:/, '');
      R.status.textContent = `${state ? state + ' · ' : ''}${h.fmt(m.time || 0, 2)} s`;
    }
    // The values as the graph has them (a script may set them too), except in a field being edited.
    for (const [name, v] of Object.entries(m.params || {})) {
      const input = R.inputs && R.inputs[name];
      if (!input || input === document.activeElement || (h.pressedIn && h.pressedIn(R.params))) continue;
      if (input.type === 'checkbox') input.checked = !!v;
      else if (Number(input.value) !== Number(h.fmt(v, 3))) input.value = h.fmt(v, 3);
    }
    if (!R.palette && !drag && S.shown) draw();  // (no redraws under the palette: CEF)
    if (R.spacePaint && R.props && R.props.contains(document.querySelector('[data-anim="blend-space"]'))) R.spacePaint();
  }

  // Scripts (tests): the canvas's edits by name, from editor.anim.do {op, ...}.
  function run(m) {
    if (!S.doc) return;
    const at = m.at || [0, 0];
    switch (m.op) {
      case 'view': enter(m.machine ? { kind: 'machine', id: m.machine } : { kind: 'flow', scope: m.scope || 'root' }); break;
      case 'add': addNode(m.nodeType, at[0], at[1], null, m.as); break;
      case 'link': {  // {from, to, input} (to "#result": the view's result)
        const box = m.to === '#result' ? resultBox() : flowNodes().find(b => b.id === m.to);
        const pin = !box || box.id === '#result' ? 0 : box.pins.findIndex(p => p.path.join('.') === m.input);
        if (canLink(m.from, box, pin)) link(m.from, box, pin);
        else h.setStatus(`link ${m.from} → ${m.to}: refused`, 'warning');
        break;
      }
      case 'set': change(() => { const n = nodeOf(m.node); if (n) setAt(n, String(m.path).split('.').map(k => /^\d+$/.test(k) ? Number(k) : k), m.value); }); break;
      case 'select': S.selected = new Set(m.keys || []); renderSide(); draw(); break;
      case 'delete': S.selected = new Set(m.keys || []); deleteSelection(); break;
      case 'state': if (nodeOf(m.machine)) { S.view = { kind: 'machine', id: m.machine }; addState(at[0], at[1], m.name); } break;
      case 'transition': addTransition(m.machine, m.from, m.to, m.when !== undefined ? { when: m.when } : {}); break;
      case 'mask': change(() => { (S.doc.boneMasks = S.doc.boneMasks || {})[m.name] = { default: m.default || 0, bones: m.bones || {}, propagate: m.propagate !== false }; }); break;
      case 'param': change(() => {
        S.doc.parameters[m.name] = m.paramType === 'entity' ? { type: 'entity' } : { type: m.paramType || 'float', default: m.default !== undefined ? m.default : 0 };
      }); break;
      case 'ik': change(() => {  // {part: feet|look|aim|reach, value (null: off)}
        S.doc.ik = S.doc.ik || {};
        if (m.value === null || m.value === undefined) delete S.doc.ik[m.part]; else S.doc.ik[m.part] = m.value;
        if (!Object.keys(S.doc.ik).length) delete S.doc.ik;
      }); break;
      case 'events': if (m.clip && E.model) { E.markup.clips[m.clip] = { sync: m.sync || [], events: m.events || [] }; E.clip = m.clip; saveEvents(); } break;
      case 'undo': undo(); break;
      case 'redo': redo(); break;
      case 'save': save(); break;
    }
  }

  function probe() {
    let links = 0;
    if (S.doc) for (const n of Object.values(S.doc.nodes)) for (const p of inputsOf(n)) if (isRef(getAt(n, p.path))) ++links;
    return {
      animatorShown: S.shown, animatorGraph: S.path, animatorErrors: S.info && S.info.errors ? S.info.errors.length : 0,
      animatorParams: S.doc ? Object.keys(S.doc.parameters).length : 0, animatorModel: S.preview && S.preview.ok ? S.preview.model : '',
      animatorState: S.state ? (S.state.state || '').replace(/^.*:/, '') : '', animatorNodes: S.doc ? Object.keys(S.doc.nodes).length : 0,
      animatorLinks: links, animatorDirty: S.dirty, animatorView: S.view.kind === 'machine' ? 'machine:' + S.view.id : S.view.scope,
      animatorUndo: S.undo.length, animatorLive: S.debug.size,
      animatorEventsModel: E.model, animatorEventsSeen: E.seen, animatorEventsError: E.error,
      animatorMarkupClips: Object.keys(E.markup.clips || {}).length,
      animatorMasks: S.doc ? Object.keys(S.doc.boneMasks || {}).length : 0, animatorBones: S.info && S.info.bones ? S.info.bones.length : 0,
      animatorIk: S.doc && S.doc.ik ? Object.keys(S.doc.ik).join(' ') : '',
      animatorEntities: S.doc ? Object.entries(S.doc.parameters).filter(([, s]) => paramType(s) === 'entity').map(([n]) => n).join(' ') : '',
    };
  }

  const active = () => S.shown && !!S.doc;
  return { init, shown, open, onGraph, onPreview, onState, onEvents, run, probe, active, undo, redo, save };
})();
