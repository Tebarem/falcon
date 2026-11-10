// The Inspector's particle and decal components (engine/vfx/vfx_components.h), built from the
// schema the engine sends with them (ins.vfx: each component's fields, their types, groups and
// ranges, and its values). One section per component, its fields in groups; a feature's group
// (Color, Size, Force, Collision, ...) has its toggle in the group's header and shows its fields
// only when on. Particle values (s&box's ParticleFloat) are a constant, a random range, a curve
// over the particle's life or a pick between two curves; colours likewise with gradients.
//
// Edits go out as editor.vfx.set {id, component, fields: {key: value}} (one undo step); drags as
// editor.vfx.preview, then a set on release.

const Particles = (() => {
  // editor.js's helpers (Particles.init).
  let el, send, tip, icon, numberInput, numberRow, scrub, fmt, setRange, colorField, vec3Row, choice, actionButton, component, componentMore,
    toSrgb, openPopover, closePopover, popoverFor;
  const MODES = [['constant', 'Value'], ['range', 'Range'], ['curve', 'Curve'], ['curveRange', '2 Curves']];
  const EVALS = [['life', 'Over Life'], ['seed', 'Per Particle'], ['frame', 'Every Frame']];
  const GRADIENT_MODES = [['constant', 'Color'], ['range', 'Range'], ['gradient', 'Gradient'], ['gradientRange', '2 Gradients']];
  // The groups a component's fields fall in, in their order (the feature groups collapse).
  const collapsedGroups = new Set();

  // ---- values: what the engine sends (as saved) into editable objects and back

  function floatValue(v) {
    if (typeof v === 'number') return { mode: 'constant', eval: 'seed', a: v, b: v, curveA: [[0, v], [1, v]], curveB: [[0, v], [1, v]] };
    const o = { mode: v.mode || 'constant', eval: v.eval || 'life', a: v.a ?? 0, b: v.b ?? v.a ?? 0, curveA: v.curveA || [[0, 0], [1, 1]], curveB: v.curveB || [[0, 1], [1, 1]] };
    if (o.mode === 'curve' || o.mode === 'curveRange') {
      const vals = o.curveA.map(k => k[1]);
      o.a = vals.length ? Math.min(...vals) : 0;
      o.b = vals.length ? Math.max(...vals) : 1;
    }
    return o;
  }
  function floatJson(o) {
    if (o.mode === 'constant') return o.a;
    if (o.mode === 'range') return { mode: 'range', eval: o.eval, a: o.a, b: o.b };
    const out = { mode: o.mode, eval: o.eval, curveA: o.curveA };
    if (o.mode === 'curveRange') out.curveB = o.curveB;
    return out;
  }
  function gradientValue(v) {
    if (Array.isArray(v)) return { mode: 'constant', eval: 'life', a: v.slice(), b: v.slice(), gradientA: { colors: [[0, v[0], v[1], v[2]]], alphas: [[0, v[3] ?? 1]] }, gradientB: null };
    const g = { mode: v.mode || 'constant', eval: v.eval || 'life', a: v.a || [1, 1, 1, 1], b: v.b || v.a || [1, 1, 1, 1],
      gradientA: v.gradientA || { colors: [[0, 1, 1, 1], [1, 1, 1, 1]], alphas: [[0, 1], [1, 1]] }, gradientB: v.gradientB || null };
    if (!g.gradientB) g.gradientB = JSON.parse(JSON.stringify(g.gradientA));
    return g;
  }
  function gradientJson(g) {
    if (g.mode === 'constant') return g.a;
    if (g.mode === 'range') return { mode: 'range', eval: g.eval, a: g.a, b: g.b };
    const out = { mode: g.mode, eval: g.eval, gradientA: g.gradientA };
    if (g.mode === 'gradientRange') out.gradientB = g.gradientB;
    return out;
  }

  // Hermite through the keys, the slopes made smooth from the neighbours (Catmull-Rom), as the
  // engine evaluates them when keys carry those slopes.
  function smoothTangents(keys) {
    keys.sort((a, b) => a[0] - b[0]);
    return keys.map((k, i) => {
      const p = keys[Math.max(i - 1, 0)], n = keys[Math.min(i + 1, keys.length - 1)];
      const s = n[0] > p[0] ? (n[1] - p[1]) / (n[0] - p[0]) : 0;
      return [k[0], k[1], s, s];
    });
  }
  function evalCurve(keys, t) {
    if (!keys.length) return 0;
    if (keys.length === 1 || t <= keys[0][0]) return keys[0][1];
    const last = keys[keys.length - 1];
    if (t >= last[0]) return last[1];
    let i = 1;
    while (i < keys.length - 1 && keys[i][0] < t) ++i;
    const k0 = keys[i - 1], k1 = keys[i], span = k1[0] - k0[0];
    if (span <= 1e-6) return k1[1];
    const s = (t - k0[0]) / span, s2 = s * s, s3 = s2 * s;
    const out0 = k0[3] ?? k0[2] ?? 0, in1 = k1[2] ?? 0;
    return (2 * s3 - 3 * s2 + 1) * k0[1] + (s3 - 2 * s2 + s) * span * out0 + (-2 * s3 + 3 * s2) * k1[1] + (s3 - s2) * span * in1;
  }
  function evalGradient(g, t) {
    const lerp = (a, b, s) => a + (b - a) * s;
    const pick = (keys, n) => {
      if (!keys.length) return Array(n).fill(1);
      if (t <= keys[0][0]) return keys[0].slice(1);
      const last = keys[keys.length - 1];
      if (t >= last[0]) return last.slice(1);
      let i = 1;
      while (i < keys.length - 1 && keys[i][0] < t) ++i;
      const k0 = keys[i - 1], k1 = keys[i], s = k1[0] > k0[0] ? (t - k0[0]) / (k1[0] - k0[0]) : 1;
      return k0.slice(1).map((v, c) => lerp(v, k1[c + 1], s));
    };
    return [...pick(g.colors, 3), pick(g.alphas, 1)[0]];
  }
  const css = c => `rgb(${c.slice(0, 3).map(v => Math.round(Math.min(1, Math.max(0, toSrgb(v))) * 255)).join(',')})`;

  // ---- popovers (closed by a click outside or Escape, as the colour picker)

  const openPop = (anchor, node) => openPopover(anchor, node);

  // A curve's preview (a small canvas); clicking it opens the editor.
  function curvePreview(keys, range, onCommit, color = '#5aa0ff') {
    const W = 120, H = 22;
    const c = el('canvas', { class: 'vfx-curve-thumb', width: W, height: H, role: 'button' });
    const g = c.getContext('2d');
    const [lo, hi] = range;
    g.strokeStyle = color;
    g.lineWidth = 1.5;
    g.beginPath();
    for (let i = 0; i <= 40; ++i) {
      const t = i / 40, v = evalCurve(keys, t), y = H - 2 - (H - 4) * (hi > lo ? (v - lo) / (hi - lo) : 0.5);
      if (i) g.lineTo(t * W, y); else g.moveTo(t * W, y);
    }
    g.stroke();
    c.addEventListener('mousedown', e => {
      e.stopPropagation();
      if (popoverFor(c)) { closePopover(); return; }
      curveEditor(c, keys, range, onCommit);
    });
    return tip(c, 'Click to edit the curve');
  }

  // The curve editor: drag keys, double-click to add one, right-click to take one away; the value
  // range is the pair of fields above it. A key's slopes stay smooth.
  function curveEditor(anchor, initial, initialRange, onCommit) {
    const W = 280, H = 150, pad = 10;
    let keys = initial.map(k => k.slice(0, 2));
    let [lo, hi] = initialRange;
    if (!(hi > lo)) hi = lo + 1;
    const canvas = el('canvas', { class: 'vfx-curve', width: W, height: H });
    const toX = t => pad + t * (W - 2 * pad), toY = v => H - pad - (H - 2 * pad) * (v - lo) / (hi - lo);
    const fromX = x => Math.min(1, Math.max(0, (x - pad) / (W - 2 * pad))), fromY = y => lo + (hi - lo) * (H - pad - y) / (H - 2 * pad);
    const draw = () => {
      const g = canvas.getContext('2d');
      g.clearRect(0, 0, W, H);
      g.strokeStyle = 'rgba(255,255,255,0.1)';
      g.beginPath();
      for (let i = 0; i <= 4; ++i) { const y = pad + (H - 2 * pad) * i / 4, x = pad + (W - 2 * pad) * i / 4; g.moveTo(pad, y); g.lineTo(W - pad, y); g.moveTo(x, pad); g.lineTo(x, H - pad); }
      g.stroke();
      const smooth = smoothTangents(keys.map(k => k.slice()));
      g.strokeStyle = '#5aa0ff';
      g.lineWidth = 1.5;
      g.beginPath();
      for (let i = 0; i <= 100; ++i) { const t = i / 100, x = toX(t), y = toY(evalCurve(smooth, t)); if (i) g.lineTo(x, y); else g.moveTo(x, y); }
      g.stroke();
      g.fillStyle = '#dfe8ff';
      keys.forEach(k => { g.beginPath(); g.arc(toX(k[0]), toY(k[1]), 3.5, 0, Math.PI * 2); g.fill(); });
    };
    const commit = () => onCommit(smoothTangents(keys.map(k => k.slice())), [lo, hi]);
    const hit = e => {
      const r = canvas.getBoundingClientRect(), x = (e.clientX - r.left) * W / r.width, y = (e.clientY - r.top) * H / r.height;
      return { x, y, index: keys.findIndex(k => Math.hypot(toX(k[0]) - x, toY(k[1]) - y) < 7) };
    };
    canvas.addEventListener('mousedown', down => {
      down.stopPropagation();
      const at = hit(down);
      if (down.button === 2) {
        if (at.index >= 0 && keys.length > 1) { keys.splice(at.index, 1); draw(); commit(); }
        return;
      }
      if (down.button !== 0 || at.index < 0) return;
      down.preventDefault();
      const key = keys[at.index];
      const move = e => {
        const r = canvas.getBoundingClientRect();
        key[0] = fromX((e.clientX - r.left) * W / r.width);
        key[1] = fromY((e.clientY - r.top) * H / r.height);
        keys.sort((a, b) => a[0] - b[0]);
        draw();
      };
      const up = () => { document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up); commit(); };
      document.addEventListener('mousemove', move);
      document.addEventListener('mouseup', up);
    });
    canvas.addEventListener('contextmenu', e => e.preventDefault());
    canvas.addEventListener('dblclick', e => {
      const at = hit(e);
      if (at.index >= 0 || keys.length >= 8) return;
      keys.push([fromX(at.x), fromY(at.y)]);
      keys.sort((a, b) => a[0] - b[0]);
      draw();
      commit();
    });
    const rangeRow = el('div', { class: 'vfx-range' },
      el('span', {}, 'From'), numberInput(lo, v => { lo = v; if (!(hi > lo)) hi = lo + 1; draw(); commit(); }),
      el('span', {}, 'to'), numberInput(hi, v => { hi = v; if (!(hi > lo)) lo = hi - 1; draw(); commit(); }));
    const preset = (label, make) => actionButton(label, () => { keys = make(); draw(); commit(); }, 'vfx-preset');
    const presets = el('div', { class: 'vfx-presets' },
      preset('Up', () => [[0, lo], [1, hi]]), preset('Down', () => [[0, hi], [1, lo]]),
      preset('Bell', () => [[0, lo], [0.5, hi], [1, lo]]), preset('Fade out', () => [[0, hi], [0.7, hi], [1, lo]]),
      preset('Flat', () => [[0, hi], [1, hi]]));
    const hint = el('div', { class: 'vfx-hint' }, 'Drag keys · double-click adds one · right-click removes');
    const node = el('div', { class: 'color-pop vfx-pop' }, rangeRow, canvas, presets, hint);
    node.addEventListener('mousedown', e => e.stopPropagation());
    openPop(anchor, node);
    draw();
  }

  // A gradient's bar; clicking opens its editor: colour stops under the bar, alpha stops over it.
  function gradientBar(g, onCommit) {
    const W = 160, H = 16;
    const c = el('canvas', { class: 'vfx-gradient-thumb', width: W, height: H, role: 'button' });
    paintGradient(c, g);
    c.addEventListener('mousedown', e => {
      e.stopPropagation();
      if (popoverFor(c)) { closePopover(); return; }
      gradientEditor(c, g, onCommit);
    });
    return tip(c, 'Click to edit the gradient');
  }
  function paintGradient(canvas, g) {
    const ctx = canvas.getContext('2d'), W = canvas.width, H = canvas.height;
    for (let x = 0; x < W; x += 6)  // the checkerboard alpha shows over
      for (let y = 0; y < H; y += 6) { ctx.fillStyle = ((x + y) / 6) % 2 ? '#555' : '#888'; ctx.fillRect(x, y, 6, 6); }
    for (let x = 0; x < W; ++x) {
      const c = evalGradient(g, x / (W - 1));
      ctx.fillStyle = css(c);
      ctx.globalAlpha = Math.min(1, Math.max(0, c[3]));
      ctx.fillRect(x, 0, 1, H);
    }
    ctx.globalAlpha = 1;
  }
  function gradientEditor(anchor, initial, onCommit) {
    const g = JSON.parse(JSON.stringify(initial));
    const W = 280;
    const bar = el('canvas', { class: 'vfx-gradient', width: W, height: 24 });
    const colorStops = el('div', { class: 'vfx-stops below' }), alphaStops = el('div', { class: 'vfx-stops above' });
    let picked = null;  // ['colors' | 'alphas', index]
    const details = el('div', { class: 'vfx-stop-detail' });
    const commit = () => onCommit(JSON.parse(JSON.stringify(g)));
    const paint = () => {
      paintGradient(bar, g);
      colorStops.replaceChildren(...g.colors.map((k, i) => stop('colors', i, k, css(evalGradient(g, k[0])))));
      alphaStops.replaceChildren(...g.alphas.map((k, i) => stop('alphas', i, k, `rgba(255,255,255,${k[1]})`)));
      details.replaceChildren(...detail());
    };
    function stop(list, index, k, fill) {
      const s = el('span', { class: 'vfx-stop' + (picked && picked[0] === list && picked[1] === index ? ' on' : '') });
      s.style.left = `${k[0] * W}px`;
      s.style.background = fill;
      s.addEventListener('mousedown', down => {
        down.stopPropagation();
        if (down.button === 2) {
          if (g[list].length > 1) { g[list].splice(index, 1); picked = null; paint(); commit(); }
          return;
        }
        picked = [list, index];
        const r = bar.getBoundingClientRect();
        const move = e => { k[0] = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)); paint(); };
        const up = () => {
          document.removeEventListener('mousemove', move);
          document.removeEventListener('mouseup', up);
          const key = g[list][index];
          g[list].sort((a, b) => a[0] - b[0]);
          picked = [list, g[list].indexOf(key)];
          paint();
          commit();
        };
        document.addEventListener('mousemove', move);
        document.addEventListener('mouseup', up);
        paint();
      });
      s.addEventListener('contextmenu', e => e.preventDefault());
      return s;
    }
    function detail() {
      if (!picked) return [el('span', { class: 'vfx-hint' }, 'Click a stop to edit it · double-click the bar adds one · right-click removes')];
      const [list, i] = picked, k = g[list][i];
      if (!k) return [];
      const at = numberRow('Location', k[0] * 100, v => { k[0] = Math.min(1, Math.max(0, v / 100)); g[list].sort((a, b) => a[0] - b[0]); picked = [list, g[list].indexOf(k)]; paint(); commit(); }, 0.5, 0, 1);
      if (list === 'alphas')
        return [at, numberRow('Alpha', k[1], v => { k[1] = Math.min(1, Math.max(0, v)); paint(); commit(); }, 0.01, 0, 2)];
      return [at, el('div', { class: 'prop' }, el('label', {}, 'Color'), colorField(k.slice(1, 4), c => { k[1] = c[0]; k[2] = c[1]; k[3] = c[2]; paintGradient(bar, g); },
        c => { k[1] = c[0]; k[2] = c[1]; k[3] = c[2]; paint(); commit(); }))];
    }
    bar.addEventListener('dblclick', e => {
      const r = bar.getBoundingClientRect(), t = Math.min(1, Math.max(0, (e.clientX - r.left) / r.width));
      const c = evalGradient(g, t);
      const list = e.clientY - r.top < r.height / 2 ? 'alphas' : 'colors';
      if (g[list].length >= 8) return;
      const key = list === 'alphas' ? [t, c[3]] : [t, c[0], c[1], c[2]];
      g[list].push(key);
      g[list].sort((a, b) => a[0] - b[0]);
      picked = [list, g[list].indexOf(key)];
      paint();
      commit();
    });
    const node = el('div', { class: 'color-pop vfx-pop' }, el('div', { class: 'vfx-gradient-box' }, alphaStops, bar, colorStops), details);
    node.addEventListener('mousedown', e => { if (!e.target.closest('.color-field')) e.stopPropagation(); });
    openPop(anchor, node);
    paint();
  }

  // ---- fields

  // A particle value: its mode, then what that mode needs (one or two numbers, one or two curves)
  // and, but for constants, what it is evaluated at.
  function floatField(f, value, set, preview) {
    const o = floatValue(value);
    const range = [f.min ?? 0, f.max ?? 1];
    const send = () => set(floatJson(o));
    const mode = el('select', { class: 'field vfx-mode' }, MODES.map(([v, t]) => el('option', { value: v, selected: o.mode === v }, t)));
    mode.addEventListener('change', () => {
      o.mode = mode.value;
      if (o.mode === 'range' && o.b === o.a) o.b = o.a + (range[1] > range[0] ? (range[1] - range[0]) * 0.25 : 1);
      if (o.mode === 'curve' || o.mode === 'curveRange') {
        o.eval = 'life';
        if (o.curveA.every(k => k[1] === o.curveA[0][1])) o.curveA = [[0, o.a], [1, o.a]];
      }
      send();
    });
    const body = [];
    const num = (key, digits = 3) => {
      const input = numberInput(o[key], v => { o[key] = v; send(); }, digits);
      return tip(el('span', { class: 'vfx-num' }, input), o.mode === 'range' ? (key === 'a' ? 'From' : 'To') : f.label);
    };
    // The editor's value range: the keys' (from 0 when they are above it).
    const curveRange = () => {
      const vals = [...o.curveA, ...(o.mode === 'curveRange' ? o.curveB : [])].map(k => k[1]);
      const lo = Math.min(0, ...vals), hi = Math.max(...vals);
      return [lo, hi - lo > 1e-3 ? hi : lo + 1];
    };
    const digits = f.type === 'uint' ? 0 : 3;
    if (o.mode === 'constant') body.push(num('a', digits));
    else if (o.mode === 'range') body.push(num('a', digits), num('b', digits));
    else {
      body.push(curvePreview(o.curveA, curveRange(), keys => { o.curveA = keys; send(); }));
      if (o.mode === 'curveRange') body.push(curvePreview(o.curveB, curveRange(), keys => { o.curveB = keys; send(); }, '#ffb35a'));
    }
    if (o.mode !== 'constant') {
      const ev = el('select', { class: 'field vfx-eval' }, EVALS.map(([v, t]) => el('option', { value: v, selected: o.eval === v }, t)));
      ev.addEventListener('change', () => { o.eval = ev.value; send(); });
      body.push(tip(ev, 'Over Life: along the particle\'s life; Per Particle: picked once for each; Every Frame: a new pick each frame'));
    }
    return el('div', { class: 'prop vfx-prop' }, el('label', {}, f.label), el('div', { class: 'vfx-value' }, mode, ...body));
  }

  function gradientField(f, value, set) {
    const g = gradientValue(value);
    const send = () => set(gradientJson(g));
    const mode = el('select', { class: 'field vfx-mode' }, GRADIENT_MODES.map(([v, t]) => el('option', { value: v, selected: g.mode === v }, t)));
    mode.addEventListener('change', () => { g.mode = mode.value; send(); });
    const body = [];
    const color = key => {
      const c = g[key];
      const alpha = numberInput(c[3] ?? 1, v => { c[3] = Math.min(1, Math.max(0, v)); send(); }, 2);
      return el('span', { class: 'vfx-color' }, colorField(c.slice(0, 3), () => {}, rgb => { c[0] = rgb[0]; c[1] = rgb[1]; c[2] = rgb[2]; send(); }), tip(alpha, 'Alpha'));
    };
    if (g.mode === 'constant') body.push(color('a'));
    else if (g.mode === 'range') body.push(color('a'), color('b'));
    else {
      body.push(gradientBar(g.gradientA, v => { g.gradientA = v; send(); }));
      if (g.mode === 'gradientRange') body.push(gradientBar(g.gradientB, v => { g.gradientB = v; send(); }));
    }
    return el('div', { class: 'prop vfx-prop' }, el('label', {}, f.label), el('div', { class: 'vfx-value' }, mode, ...body));
  }

  function assetField(f, value, set, kind) {
    const paths = Assets.paths(kind);
    const sel = el('select', { class: 'field' },
      el('option', { value: '', selected: !value }, kind === 'material' ? 'None (white)' : 'None'),
      ...(value && !paths.includes(value) ? [el('option', { value, selected: true }, `${value} (missing)`)] : []),
      ...paths.map(p => el('option', { value: p, selected: p === value }, p.replace(/^.*\//, '').replace(/\.[^.]*$/, ''))));
    sel.addEventListener('change', () => set(sel.value));
    return el('div', { class: 'prop' }, el('label', {}, f.label), sel);
  }

  // A mesh of the scene (the Project panel's: shapes, rocks, scene and model meshes), by name.
  function meshField(f, value, set) {
    const names = [...new Set(Assets.meshes())].sort();
    const sel = el('select', { class: 'field', 'data-vfx-field': f.key },
      el('option', { value: '', selected: !value }, 'None'),
      ...(value && !names.includes(value) ? [el('option', { value, selected: true }, `${value} (missing)`)] : []),
      ...names.map(n => el('option', { value: n, selected: n === value }, n)));
    sel.addEventListener('change', () => set(sel.value));
    return el('div', { class: 'prop' }, el('label', {}, f.label), sel);
  }

  function fieldRow(f, values, set, preview) {
    const v = values[f.key];
    const row = (() => {
      switch (f.type) {
      case 'float':
      case 'uint':
      case 'int': {
        const digits = f.type === 'float' ? 3 : 0;
        if (f.max > f.min && f.max - f.min <= 10 && f.type === 'float')
          return slider(f, v, set, preview);
        return numberRow(f.label, v, x => set(f.type === 'float' ? x : Math.round(x)), f.max > f.min ? (f.max - f.min) / 300 : 0.01,
          f.type === 'int' ? -Infinity : f.min ?? -Infinity, digits);
      }
      case 'bool': {
        const box = el('input', { type: 'checkbox', class: 'check', checked: !!v, 'data-vfx-field': f.key });
        box.addEventListener('change', () => set(box.checked));
        return el('div', { class: 'prop' }, el('label', {}, f.label), box);
      }
      case 'enum': {
        const options = f.options.split('|').map(o => [o, o.charAt(0).toUpperCase() + o.slice(1)]);
        return choice(f.label, options, v, set);
      }
      case 'vec3':
        return vec3Row(f.label, v, set);
      case 'color':
        return el('div', { class: 'prop' }, el('label', {}, f.label),
          el('span', { class: 'vfx-color' }, colorField(v.slice(0, 3), c => preview([...c, v[3] ?? 1]), c => set(v.length > 3 ? [...c, v[3]] : c)),
            ...(v.length > 3 ? [tip(numberInput(v[3], a => set([v[0], v[1], v[2], Math.min(1, Math.max(0, a))]), 2), 'Alpha')] : [])));
      case 'pfloat':
        return floatField(f, v, set, preview);
      case 'pvec3':
        return el('div', { class: 'vfx-vec' }, el('div', { class: 'vfx-sub' }, f.label),
          ...['X', 'Y', 'Z'].map((axis, i) => floatField({ ...f, label: axis }, v[i], x => { const n = v.slice(); n[i] = x; set(n); }, x => { const n = v.slice(); n[i] = x; preview(n); })));
      case 'pgradient':
        return gradientField(f, v, set);
      case 'material':
        return assetField(f, v, set, 'material');
      case 'prefab':
        return assetField(f, v, set, 'prefab');
      case 'mesh':
        return meshField(f, v, set);
      default: {
        const input = el('input', { class: 'field', value: v || '', spellcheck: 'false' });
        input.addEventListener('change', () => set(input.value.trim()));
        return el('div', { class: 'prop' }, el('label', {}, f.label), input);
      }
      }
    })();
    return f.tip ? tip(row, f.tip) : row;
  }

  // A bounded number: its slider previews while dragged, and sets on release.
  function slider(f, value, set, preview) {
    let v = value;
    const step = (f.max - f.min) / 200;
    const range = el('input', { type: 'range', min: f.min, max: f.max, step, value: v });
    const input = numberInput(v, x => { v = x; setRange(range, v); set(v); }, 2);
    range.addEventListener('input', () => { v = +range.value; input.value = fmt(v, 2); preview(v); });
    range.addEventListener('change', () => { v = +range.value; set(v); });
    const lab = el('label', { class: 'scrub' }, f.label);
    scrub(lab, () => v, x => { v = Math.min(f.max, Math.max(f.min, x)); setRange(range, v); input.value = fmt(v, 2); preview(v); },
      x => { v = Math.min(f.max, Math.max(f.min, x)); set(v); }, (f.max - f.min) / 300);
    return el('div', { class: 'prop' }, lab, el('div', { class: 'slider-row' }, range, input));
  }

  // ---- a component's section

  function section(ins, c) {
    const sendSet = (key, value) => send('editor.vfx.set', { id: ins.id, component: c.json, fields: { [key]: value } });
    const sendPreview = (key, value) => send('editor.vfx.preview', { id: ins.id, component: c.json, fields: { [key]: value } });
    const values = c.values;
    const rows = [];
    const groups = new Map();
    for (const f of c.schema) {
      if (!groups.has(f.group)) groups.set(f.group, []);
      groups.get(f.group).push(f);
    }
    for (const [group, fields] of groups) {
      const toggle = fields.find(f => f.feature);
      const body = fields.filter(f => f !== toggle).map(f => fieldRow(f, values, v => sendSet(f.key, v), v => sendPreview(f.key, v)));
      if (!group) {
        rows.push(...body);
        continue;
      }
      const key = `${c.name}.${group}`;
      const on = toggle ? !!values[toggle.key] : true;
      const open = on && !collapsedGroups.has(key);
      const head = el('div', { class: 'vfx-group-head' + (open ? ' open' : ''), role: 'button' }, el('span', { class: 'arrow', html: icon('chev') }));
      if (toggle) {
        const box = el('input', { type: 'checkbox', class: 'check', checked: on, 'data-vfx-feature': toggle.key });
        box.addEventListener('mousedown', e => e.stopPropagation());
        box.addEventListener('change', () => sendSet(toggle.key, box.checked));
        head.append(box);
      }
      head.append(el('span', {}, group));
      const block = el('div', { class: 'vfx-group-body', hidden: !open }, body);
      head.addEventListener('click', () => {
        if (!on) return;
        if (collapsedGroups.has(key)) collapsedGroups.delete(key); else collapsedGroups.add(key);
        block.hidden = collapsedGroups.has(key);
        head.classList.toggle('open', !block.hidden);
      });
      rows.push(el('div', { class: 'vfx-group' }, head, block));
    }
    // The effect: its particles now, and its preview's controls. A part: the effect it works for.
    const fx = ins.vfx.effect;
    const extra = [];
    if (c.kind === 'effect') {
      extra.push(el('div', { class: 'prop' }, el('label', {}, 'Preview'), el('div', { class: 'row-buttons' },
        actionButton('↻ Restart', () => send('editor.vfx.restart', { id: ins.id })),
        actionButton(fx && !fx.playing ? '▶ Play' : '❙❙ Pause', () => send('editor.vfx.pause', { id: ins.id })),
        el('span', { class: 'muted', 'data-vfx-count': '' }, fx ? `${fx.particles} particle${fx.particles === 1 ? '' : 's'}` : ''))));
    } else if (c.kind !== 'decal') {
      extra.push(fx ? el('div', { class: 'prop hint' }, el('label', {}, 'Effect'), el('span', {}, fx.own ? 'This entity\'s' : fx.name))
        : el('div', { class: 'prop hint warn' }, el('label', {}, 'Effect'), el('span', {}, 'None: add a Particle Effect here or above')));
    }
    if (c.warning)  // (a Model Renderer's mesh it can't draw this session)
      extra.push(el('div', { class: 'prop hint warn', 'data-vfx-warning': '' }, el('label', {}, ''), el('span', {}, c.warning)));
    return componentMore(component(c.label, 'vfx.' + c.name, [...extra, ...rows], el('span', { html: icon(c.icon) })), ins, c.name).node;
  }

  return {
    init(h) {
      ({ el, send, tip, icon, numberInput, numberRow, scrub, fmt, setRange, colorField, vec3Row, choice, actionButton, component, componentMore,
        toSrgb, openPopover, closePopover, popoverFor } = h);
    },
    sections: ins => (ins.vfx ? ins.vfx.components.map(c => section(ins, c)) : []),
    // Their names: the generic component list leaves them out (they have sections of their own).
    names: ['ParticleEffect', 'ParticleSphereEmitter', 'ParticleBoxEmitter', 'ParticleConeEmitter', 'ParticleRingEmitter',
      'ParticleSpriteRenderer', 'ParticleTrailRenderer', 'ParticleLightRenderer', 'ParticleAttractor', 'ParticleNoise', 'ParticleScript', 'Decal',
      'ParticleModelRenderer'],
  };
})();
