// Profiler tab (engine side: engine/editor/profiler_stream.h, engine/core/profiler.h).
//
// Top: one chart per module over the last 600 frames: CPU main-thread time by category, GPU
// time by section (timestamp queries), and a chart per counter module (Rendering, Shadows,
// Virtual Texturing, Physics, Animation, UI, Memory). Click or drag a chart to select a frame;
// that pauses the view (frames keep arriving) until Current.
// Bottom: the selected frame as a Timeline (every thread plus the GPU), a Hierarchy (merged call
// tree, sortable), its Counters (with min / avg / max over the history) and GPU Memory.
// Built the first time the tab opens; subscribed to the engine only while it is visible.
'use strict';

const Profiler = (() => {
  // editor.js helpers (block-scoped there, so handed over by Profiler.init).
  let $, el, send, store, isTextField, toggleMaximize;
  const MAX_FRAMES = 600;
  const LANE_LABEL = 112, ROW = 17, RULER = 18;
  // Category colors, Unity-like (index = engine profiler::Category).
  const CAT_COLORS = ['#7d7d7d', '#6bb04b', '#4a90d9', '#e08a3c', '#3cbfae', '#b06bd8', '#8a9bb0', '#cdb64e'];
  const CAT_STACK = [1, 2, 3, 4, 5, 6, 0, 7];  // bottom to top; Wait on top
  const PALETTE = ['#4a90d9', '#6bb04b', '#e08a3c', '#b06bd8', '#3cbfae', '#cdb64e', '#e2584e', '#8a9bb0', '#c97a9e',
    '#5fb3d6', '#a3c463', '#d6935f', '#7f8fe0', '#56c29a', '#cf6f6f', '#9a9a5a', '#d58fd0', '#6fa0a0'];
  const DEFAULT_MODULES = { CPU: true, GPU: true, Rendering: true, Memory: true };

  const S = {
    built: false, visible: false, recording: true, subscribed: false,
    names: [], cats: [], counters: [], threads: [], gpuTiming: true,
    frames: [], pending: [], follow: true, selected: -1,
    detail: null, detailPending: -1, detailAsked: 0,
    view: 'timeline', modules: Object.assign({}, DEFAULT_MODULES), hidden: new Set(),  // from the store in build()
    colors: new Map(),
    hierSource: 'cpu', hierThread: -1, hierSort: ['total', -1], hierSearch: '', collapsed: new Set(), hierNode: null,
    tl: { start: 0, end: 0, auto: true, hover: null, picked: null, lanes: [] },
    memory: null, memoryAsked: 0,
    drawQueued: false,
  };
  const R = {};  // DOM refs

  // ------------------------------------------------------------------ helpers

  // Callbacks the page's message loop does not wrap (keys, animation frames, resize): report
  // errors with their stack (window.onerror only sees an opaque "Script error.").
  const on = (node, type, fn, options) => node.addEventListener(type, guard(fn), options);
  const guard = fn => (...args) => {
    try {
      return fn(...args);
    } catch (err) {
      send('ui.error', { message: 'profiler: ' + String(err && err.message), source: String(err && err.stack) });
    }
  };

  // Dynamic styles go on after the element exists (never as el()'s style prop): an inline
  // style attribute is blocked by the page's CSP, which leaves elements like the colour swatches
  // empty, and that is one of the layouts that crash CEF 151's renderer.
  const css = (node, props) => { Object.assign(node.style, props); return node; };
  const nameOf = id => (S.names[id] && S.names[id].name) || `#${id}`;
  const catOf = id => (S.names[id] && S.names[id].cat) || 0;
  const ms = (v, d = 2) => Number.isFinite(v) ? v.toFixed(d) : '–';
  function colorFor(key) {
    if (!S.colors.has(key)) S.colors.set(key, PALETTE[S.colors.size % PALETTE.length]);
    return S.colors.get(key);
  }
  function niceMax(v) {
    for (const s of [0.5, 1, 2, 4, 8, 16.67, 33.33, 66.67, 100, 200, 500, 1000]) if (v <= s) return s;
    return Math.ceil(v / 1000) * 1000;
  }
  function niceCount(v) {
    if (!(v > 0)) return 1;
    const p = Math.pow(10, Math.floor(Math.log10(v)));
    for (const m of [1, 2, 2.5, 5, 10]) if (v <= m * p) return m * p;
    return 10 * p;
  }
  function human(v, unit) {
    if (!Number.isFinite(v)) return '–';
    if (unit === 'ms') return v.toFixed(3) + ' ms';
    if (unit === 'MB') return v >= 1024 ? (v / 1024).toFixed(2) + ' GB' : v.toFixed(1) + ' MB';
    const a = Math.abs(v);
    const s = a >= 1e9 ? (v / 1e9).toFixed(2) + ' G' : a >= 1e6 ? (v / 1e6).toFixed(2) + ' M' : a >= 1e4 ? (v / 1e3).toFixed(1) + ' k' : String(Math.round(v * 100) / 100);
    return unit && !unit.includes(' ') ? `${s} ${unit}` : s;
  }
  function bytes(b) {
    return b >= 1 << 30 ? (b / (1 << 30)).toFixed(2) + ' GB' : b >= 1 << 20 ? (b / (1 << 20)).toFixed(1) + ' MB' : (b / 1024).toFixed(1) + ' KB';
  }
  const frameAt = i => S.frames.find(f => f.i === i);
  function counterModules() {
    const out = [];
    for (const c of S.counters) if (c && !out.includes(c.module)) out.push(c.module);
    return out;
  }
  function setupCanvas(canvas) {
    const w = canvas.clientWidth, h = canvas.clientHeight, dpr = window.devicePixelRatio || 1;
    if (!w || !h) return null;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const g = canvas.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    g.font = '10px "Segoe UI", system-ui, sans-serif';
    g.textBaseline = 'middle';
    return { g, w, h };
  }
  function queueDraw() {
    if (S.drawQueued) return;
    S.drawQueued = true;
    requestAnimationFrame(guard(() => { S.drawQueued = false; drawCharts(); updateToolbar(); }));
  }

  // ------------------------------------------------------------------ engine messages

  function applyTables(p) {
    if (p.categories) S.cats = p.categories;
    if (p.names) for (const [id, name, cat] of p.names) S.names[id] = { name, cat };
    if (p.counters) p.counters.forEach((c, k) => { S.counters[p.counterBase + k] = { name: c[0], unit: c[1], module: c[2] }; });
    if (p.threadNames) S.threads = p.threadNames;
  }

  function onFrames(p) {
    applyTables(p);
    S.gpuTiming = p.gpuTiming !== false;
    const target = S.follow ? S.frames : S.pending;
    const last = S.frames.length ? S.frames[S.frames.length - 1].i : -1;
    const lastPending = S.pending.length ? S.pending[S.pending.length - 1].i : last;
    for (const f of p.frames) {
      if (f[0] <= (S.follow ? last : lastPending)) continue;  // resent history after a re-subscribe
      target.push({ i: f[0], cpu: f[1], gpu: f[2], cat: f[3], sec: f[4], c: f[5] });
    }
    if (S.frames.length > MAX_FRAMES) S.frames.splice(0, S.frames.length - MAX_FRAMES);
    if (S.pending.length > MAX_FRAMES) S.pending.splice(0, S.pending.length - MAX_FRAMES);
    if (S.follow && S.frames.length) {
      // The newest frame with GPU timings (a frame whose marks timed out has an empty GPU lane).
      let pick = S.frames.length - 1;
      for (let k = pick; k >= Math.max(0, S.frames.length - 30); --k) if (S.frames[k].gpu > 0) { pick = k; break; }
      S.selected = S.frames[pick].i;
      if (performance.now() - S.detailAsked > 500) requestDetail(S.selected);
    }
    if (S.visible) {
      if (!R.modules.childElementCount || R.modules.dataset.count !== String(2 + counterModules().length)) buildModules();
      queueDraw();
    }
  }

  function onDetail(p) {
    applyTables(p);
    if (p.index !== S.detailPending && S.detail && S.detail.index === p.index) return;
    S.detailPending = -1;
    if (p.missing) return;
    S.detail = parseDetail(p);
    if (S.tl.auto) { S.tl.start = 0; S.tl.end = Math.max(S.detail.cpuMs, S.detail.gpuMs, 0.01); }
    renderView();
  }

  function onMemory(p) {
    S.memory = p.resources.map(([module, name, kind, size]) => ({ module, name, kind, size }));
    if (S.view === 'memory') renderView();
  }

  function requestDetail(index) {
    if (index < 0 || S.detailPending === index) return;
    S.detailPending = index;
    S.detailAsked = performance.now();
    send('profiler.frame', { index });
  }

  // Zones arrive per thread in closing order with their depth; rebuild parents (by start time)
  // to get self times and the merged hierarchy.
  function parseDetail(p) {
    const threads = [];
    for (const [thread, flat] of p.threads) {
      const zones = [];
      for (let k = 0; k < flat.length; k += 4)
        zones.push({ name: flat[k], depth: flat[k + 1], s: flat[k + 2] / 1000, e: flat[k + 3] / 1000, self: 0, parent: null, kids: [] });
      zones.sort((a, b) => a.s - b.s || a.depth - b.depth);
      const stack = [];
      for (const z of zones) {
        while (stack.length && (stack[stack.length - 1].depth >= z.depth || stack[stack.length - 1].e < z.s)) stack.pop();
        z.parent = stack.length ? stack[stack.length - 1] : null;
        if (z.parent) z.parent.kids.push(z);
        stack.push(z);
      }
      for (const z of zones) z.self = Math.max(0, (z.e - z.s) - z.kids.reduce((t, k) => t + (k.e - k.s), 0));
      const maxDepth = zones.reduce((m, z) => Math.max(m, z.depth), 0);
      threads.push({ id: thread, name: S.threads[thread] || `Thread ${thread}`, zones, maxDepth });
    }
    threads.sort((a, b) => (a.name === 'Main' ? -1 : b.name === 'Main' ? 1 : a.name.localeCompare(b.name, undefined, { numeric: true })));
    const gpu = [];
    for (let k = 0; k < p.gpu.length; k += 4) gpu.push({ name: p.gpu[k], level: p.gpu[k + 1], s: p.gpu[k + 2], e: p.gpu[k + 3], self: 0, kids: [] });
    let section = null;
    for (const z of gpu) {
      if (z.level === 0) section = z;
      else if (section) { z.parent = section; section.kids.push(z); }
    }
    for (const z of gpu) z.self = Math.max(0, (z.e - z.s) - z.kids.reduce((t, k) => t + (k.e - k.s), 0));
    return { index: p.index, cpuMs: p.cpuMs, gpuMs: p.gpuDone ? p.gpuMs : 0, threads, gpu, categoryMs: p.categoryMs, counters: p.counters };
  }

  // ------------------------------------------------------------------ DOM

  function build() {
    // Page preferences, read once the tab first opens (after Profiler.init).
    S.view = store.get('profView', 'timeline');
    S.modules = Object.assign({}, DEFAULT_MODULES, store.get('profModules', {}));
    S.hidden = new Set(store.get('profHiddenSeries', []));
    const root = $('#profiler');
    const tool = (label, onClick, iconName) => {
      const node = el('span', { class: 'ptool', html: iconName ? icon(iconName) : '' }, label);
      on(node, 'mousedown', e => { e.preventDefault(); onClick(node); });
      return node;
    };
    R.record = tool('Record', () => setRecording(!S.recording));
    R.current = tool('Current', () => resume());
    R.prev = tool('', () => step(-1), 'chevRight');
    R.next = tool('', () => step(1), 'chevRight');
    R.prev.classList.add('pprev');
    R.next.classList.add('pnext');
    R.clear = tool('Clear', () => { S.frames = []; S.pending = []; S.detail = null; S.selected = -1; S.follow = true; queueDraw(); renderView(); });
    R.frameInfo = el('span', { class: 'pinfo' });
    R.note = el('span', { class: 'pnote' });
    R.max = tool('Maximize', () => setMaximized(!S.maximized), 'maximize');
    const toolbar = el('div', { class: 'ptoolbar' }, R.record, R.current, R.prev, R.next, R.clear, el('span', { class: 'psep' }), R.frameInfo, el('span', { class: 'pspacer' }), R.note, R.max);

    R.modules = el('div', { class: 'pmodules' });
    R.charts = el('div', { class: 'pcharts' });
    const top = el('div', { class: 'ptop' }, R.modules, R.charts);

    R.tabs = el('div', { class: 'ptabs' });
    for (const [id, label] of [['timeline', 'Timeline'], ['hierarchy', 'Hierarchy'], ['counters', 'Counters'], ['memory', 'Memory']]) {
      const t = el('span', { class: 'ptab', 'data-view': id }, label);
      on(t, 'mousedown', e => { e.preventDefault(); setView(id); });
      R.tabs.append(t);
    }
    R.viewTools = el('span', { class: 'pviewtools' });
    R.view = el('div', { class: 'pview' });
    const splitter = el('div', { class: 'psplit' });
    const bottom = el('div', { class: 'pbottom' }, el('div', { class: 'ptabbar' }, R.tabs, R.viewTools), R.view);
    root.replaceChildren(toolbar, top, splitter, bottom);
    root.tabIndex = 0;
    on(root, 'keydown', e => {
      if (isTextField(e.target)) return;
      if (e.key === 'ArrowLeft') { step(-1); e.preventDefault(); }
      else if (e.key === 'ArrowRight') { step(1); e.preventDefault(); }
    });
    // Splitter between the charts and the frame view.
    on(splitter, 'mousedown', down => {
      down.preventDefault();
      const h0 = top.getBoundingClientRect().height, y0 = down.clientY;
      const move = e => { top.style.height = Math.max(90, h0 + e.clientY - y0) + 'px'; queueDraw(); renderView(); };
      const up = () => { document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up); store.set('profTopH', top.style.height); };
      document.addEventListener('mousemove', move);
      document.addEventListener('mouseup', up);
    });
    const savedH = store.get('profTopH', '');
    if (savedH) top.style.height = savedH;
    new ResizeObserver(guard(() => { queueDraw(); if (S.view === 'timeline') drawTimeline(); })).observe(root);
    S.built = true;
    buildModules();
    setView(S.view);
  }

  function buildModules() {
    const mods = ['CPU', 'GPU', ...counterModules()];
    R.modules.dataset.count = String(mods.length);
    R.modules.replaceChildren(...mods.map(m => {
      const row = el('div', { class: 'pmod' + (S.modules[m] ? ' on' : '') }, el('span', { class: 'pcheck' }), el('span', {}, m));
      on(row, 'mousedown', e => {
        e.preventDefault();
        S.modules[m] = !S.modules[m];
        store.set('profModules', S.modules);
        buildModules();
        buildCharts();
      });
      return row;
    }));
    buildCharts();
  }

  function buildCharts() {
    const mods = ['CPU', 'GPU', ...counterModules()].filter(m => S.modules[m]);
    R.charts.replaceChildren(...mods.map(m => {
      // Title and legend are drawn into the canvas too: HTML over a canvas that redraws every
      // 100 ms crashes CEF 151's renderer (exit 0xC0000409), so each chart is canvas only.
      const canvas = el('canvas');
      const box = el('div', { class: 'pchart', 'data-module': m }, canvas);
      box.legendHits = [];
      const pick = e => {
        const r = canvas.getBoundingClientRect();
        const n = Math.max(S.frames.length, 2);
        const k = Math.round((e.clientX - r.left) / r.width * (MAX_FRAMES - 1) - (MAX_FRAMES - n));
        const f = S.frames[Math.max(0, Math.min(S.frames.length - 1, k))];
        if (f) select(f.i);
      };
      on(canvas, 'mousedown', down => {
        down.preventDefault();
        const r = canvas.getBoundingClientRect();
        const mx = down.clientX - r.left, my = down.clientY - r.top;
        const hit = box.legendHits.find(h => mx >= h.x && mx < h.x + h.w && my >= h.y && my < h.y + h.h);
        if (hit) {  // a legend entry: show / hide that series
          if (S.hidden.has(hit.series)) S.hidden.delete(hit.series); else S.hidden.add(hit.series);
          store.set('profHiddenSeries', [...S.hidden]);
          queueDraw();
          return;
        }
        pick(down);
        const up = () => { document.removeEventListener('mousemove', pick); document.removeEventListener('mouseup', up); };
        document.addEventListener('mousemove', pick);
        document.addEventListener('mouseup', up);
      });
      return box;
    }));
    queueDraw();
  }

  // ------------------------------------------------------------------ toolbar / selection

  function subscribe() {
    const want = S.visible && S.recording;
    if (want === S.subscribed) return;
    S.subscribed = want;
    send('profiler.subscribe', { on: want });
  }
  function setRecording(on) { S.recording = on; subscribe(); updateToolbar(); }
  function select(index) {
    S.follow = false;
    S.selected = index;
    requestDetail(index);
    queueDraw();
  }
  function resume() {
    S.frames.push(...S.pending);
    S.pending = [];
    if (S.frames.length > MAX_FRAMES) S.frames.splice(0, S.frames.length - MAX_FRAMES);
    S.follow = true;
    S.tl.auto = true;
    if (S.frames.length) { S.selected = S.frames[S.frames.length - 1].i; requestDetail(S.selected); }
    queueDraw();
  }
  function step(d) {
    if (!S.frames.length) return;
    const k = S.frames.findIndex(f => f.i === S.selected);
    const f = S.frames[Math.max(0, Math.min(S.frames.length - 1, (k < 0 ? S.frames.length - 1 : k) + d))];
    select(f.i);
  }
  // The editor's dock maximize (Shift+Space on a hovered dock) applied to the bottom dock.
  const dock = () => $('#profiler').closest('.dock');
  function setMaximized(on) {
    if (dock().classList.contains('maximized') !== on) toggleMaximize(dock());
    updateToolbar();
    queueDraw();
    renderView();
  }
  function updateToolbar() {
    if (!S.built) return;
    R.record.classList.toggle('on', S.recording);
    R.record.classList.toggle('rec', S.recording);
    R.current.classList.toggle('disabled', S.follow);
    const f = frameAt(S.selected);
    const pending = S.pending.length ? `  ·  ${S.pending.length} new` : '';
    R.frameInfo.textContent = f
      ? `Frame ${f.i}  ·  CPU ${ms(f.cpu)} ms (${ms(f.cpu > 0 ? 1000 / f.cpu : 0, 0)} fps)  ·  GPU ${S.gpuTiming && f.gpu > 0 ? ms(f.gpu) + ' ms' : '–'}${pending}`
      : (S.recording ? 'Waiting for frames…' : 'Not recording');
    R.note.textContent = S.gpuTiming ? '' : 'GPU timing unavailable on this device';
    R.max.lastChild.textContent = dock().classList.contains('maximized') ? 'Restore' : 'Maximize';
  }

  // ------------------------------------------------------------------ charts

  // A chart's legend entries: [color, label, value at the selected frame, series key, shown].
  function chartLegend(module, selFrame) {
    if (module === 'CPU')
      return CAT_STACK.map(cat => [CAT_COLORS[cat], S.cats[cat] || '?', `${ms(selFrame ? selFrame.cat[cat] : NaN)} ms`, null]);
    if (module === 'GPU') {
      if (!S.gpuTiming) return [['#666', 'GPU timing unavailable', '', null]];
      const seen = new Map();
      for (const f of S.frames.slice(-120)) for (let q = 0; q < f.sec.length; q += 2) if (!seen.has(f.sec[q])) seen.set(f.sec[q], 0);
      if (selFrame) for (let q = 0; q < selFrame.sec.length; q += 2) seen.set(selFrame.sec[q], selFrame.sec[q + 1]);
      return [...seen].map(([id, v]) => [colorFor('gpu:' + id), nameOf(id), `${ms(v, 3)} ms`, null]);
    }
    const items = [];
    S.counters.forEach((info, id) => {
      if (!info || info.module !== module) return;
      const key = `${module}/${info.name}`;
      items.push([colorFor('counter:' + key), info.name, human(selFrame ? selFrame.c[id] : NaN, info.unit), key, !S.hidden.has(key), id]);
    });
    return items;
  }

  function drawCharts() {
    if (!S.visible || !S.built) return;
    const n = S.frames.length;
    const x0 = MAX_FRAMES - n;
    const sel = S.frames.findIndex(f => f.i === S.selected);
    const selFrame = S.frames[sel];
    for (const box of R.charts.children) {
      const module = box.dataset.module;
      const c = setupCanvas(box.querySelector('canvas'));
      if (!c) continue;
      const { g, w, h } = c;
      const bw = w / MAX_FRAMES;
      const X = k => (x0 + k) * bw;
      // Legend on top, then the plot in the space below it.
      const legend = chartLegend(module, selFrame);
      const plotTop = drawLegend(g, w, module, legend, box) + 3;
      const plotH = Math.max(h - plotTop - 1, 8);
      g.fillStyle = '#1b1b1b';
      g.fillRect(0, plotTop, w, h - plotTop);
      if (module === 'CPU' || module === 'GPU') {
        const cpu = module === 'CPU';
        let max = 0;
        for (const f of S.frames) max = Math.max(max, cpu ? f.cpu : f.gpu);
        const top = niceMax(max * 1.05 || 1);
        const Y = v => h - v / top * plotH;
        // Stacked bars, one per frame.
        S.frames.forEach((f, k) => {
          let acc = 0;
          if (cpu) {
            for (const cat of CAT_STACK) {
              const v = f.cat[cat] || 0;
              if (v <= 0) continue;
              g.fillStyle = CAT_COLORS[cat];
              g.fillRect(X(k), Y(acc + v), Math.max(bw, 1), Y(acc) - Y(acc + v));
              acc += v;
            }
          } else {
            for (let q = 0; q < f.sec.length; q += 2) {
              const v = f.sec[q + 1];
              if (v <= 0) continue;
              g.fillStyle = colorFor('gpu:' + f.sec[q]);
              g.fillRect(X(k), Y(acc + v), Math.max(bw, 1), Y(acc) - Y(acc + v));
              acc += v;
            }
          }
        });
        guides(g, w, h, top, Y, 'ms');
      } else {
        // Counter module: one line per counter, each scaled to its own range.
        for (const [color, , , , shown, id] of legend) {
          if (!shown) continue;
          let max = 0;
          for (const f of S.frames) if (Number.isFinite(f.c[id])) max = Math.max(max, f.c[id]);
          const top = niceCount(max * 1.05);
          g.strokeStyle = color;
          g.lineWidth = 1.25;
          g.beginPath();
          let started = false;
          S.frames.forEach((f, k) => {
            const val = f.c[id];
            if (!Number.isFinite(val)) { started = false; return; }
            const x = X(k) + bw / 2, y = h - 2 - val / top * (plotH - 3);
            if (started) g.lineTo(x, y); else { g.moveTo(x, y); started = true; }
          });
          g.stroke();
        }
      }
      // Selected frame.
      if (sel >= 0) {
        g.fillStyle = 'rgba(255,255,255,0.85)';
        g.fillRect(Math.floor(X(sel) + bw / 2), plotTop, 1, h - plotTop);
      }
    }
  }
  function guides(g, w, h, top, Y, unit) {
    g.strokeStyle = 'rgba(255,255,255,0.10)';
    g.fillStyle = '#8c8c8c';
    g.lineWidth = 1;
    const marks = [top, top / 2].concat([16.67, 33.33].filter(v => v < top * 0.92 && v > top * 0.08));
    for (const v of marks) {
      const y = Math.round(Y(v)) + 0.5;
      g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke();
      g.fillText(`${v >= 10 ? v.toFixed(1) : v.toFixed(2)} ${unit}`, w - 52, Math.max(7, y - 6));
    }
  }
  // Title, then the legend entries flowing in rows: swatch, name, value at the selected frame.
  // Returns the y below the legend (where the plot starts).
  function drawLegend(g, w, title, items, box) {
    box.legendHits = [];
    g.font = '600 10.5px "Segoe UI", system-ui, sans-serif';
    g.fillStyle = '#ececec';
    g.fillText(title, 6, 8);
    g.font = '10.5px "Segoe UI", system-ui, sans-serif';
    let x = 6, y = 22;
    for (const [color, label, value, series, shown] of items) {
      const nameW = g.measureText(label).width, valueW = g.measureText(value).width;
      const itemW = 12 + nameW + 4 + valueW + 14;
      if (x > 6 && x + itemW > w - 6) {
        x = 6;
        y += 14;
      }
      g.globalAlpha = shown === false ? 0.35 : 1;
      g.fillStyle = color;
      g.fillRect(x, y - 4, 8, 8);
      g.fillStyle = '#9a9a9a';
      g.fillText(label, x + 12, y);
      g.fillStyle = '#ececec';
      g.fillText(value, x + 16 + nameW, y);
      g.globalAlpha = 1;
      if (series) box.legendHits.push({ x: x - 2, y: y - 7, w: itemW - 8, h: 14, series });
      x += itemW;
    }
    return y + 8;
  }

  // ------------------------------------------------------------------ frame views

  function setView(view) {
    S.view = view;
    store.set('profView', view);
    for (const t of R.tabs.children) t.classList.toggle('active', t.dataset.view === view);
    R.view.replaceChildren();
    R.view.className = 'pview pview-' + view;
    if (view === 'memory' && performance.now() - S.memoryAsked > 1000) { S.memoryAsked = performance.now(); send('profiler.memory'); }
    renderView();
  }

  function renderView() {
    if (!S.built || !S.visible) return;
    if (S.view === 'timeline') renderTimeline();
    else if (S.view === 'hierarchy') renderHierarchy();
    else if (S.view === 'counters') renderCounters();
    else renderMemory();
  }

  // ---- timeline

  function renderTimeline() {
    if (!R.tlCanvas || !R.view.contains(R.tlCanvas)) {
      R.tlCanvas = el('canvas', { class: 'ptl' });
      R.tlTip = el('div', { class: 'ptip', hidden: true });
      R.tlInfo = el('span', { class: 'ptlinfo' });
      R.tlScroll = el('div', { class: 'ptlscroll' }, R.tlCanvas);
      R.view.replaceChildren(R.tlScroll, R.tlTip);
      R.viewTools.replaceChildren(R.tlInfo);
      timelineInput();
    }
    drawTimeline();
  }

  function timelineLanes() {
    const d = S.detail;
    const lanes = [];
    if (!d) return lanes;
    let y = RULER;
    for (const t of d.threads) {
      lanes.push({ label: t.name, kind: 'cpu', zones: t.zones, rows: t.maxDepth + 1, y });
      y += (t.maxDepth + 1) * ROW + 6;
    }
    if (d.gpu.length) {
      lanes.push({ label: 'GPU', kind: 'gpu', zones: d.gpu, rows: 2, y });
      y += 2 * ROW + 6;
    }
    S.tl.height = y + 4;
    return lanes;
  }

  function drawTimeline() {
    if (S.view !== 'timeline' || !R.tlCanvas) return;
    const d = S.detail;
    const lanes = S.tl.lanes = timelineLanes();
    R.tlCanvas.style.height = Math.max(S.tl.height || 0, R.tlScroll.clientHeight - 2) + 'px';
    const c = setupCanvas(R.tlCanvas);
    if (!c) return;
    const { g, w, h } = c;
    g.fillStyle = '#1e1e1e';
    g.fillRect(0, 0, w, h);
    if (!d) {
      g.fillStyle = '#8c8c8c';
      g.fillText(S.frames.length ? 'Loading frame…' : 'No frames yet', 12, 30);
      return;
    }
    R.tlInfo.textContent = `Frame ${d.index}  ·  CPU ${ms(d.cpuMs)} ms  ·  GPU ${d.gpuMs > 0 ? ms(d.gpuMs) + ' ms (own clock, from the first pass)' : '–'}  ·  wheel: zoom · drag: pan · double-click: fit`;
    const span = Math.max(S.tl.end - S.tl.start, 1e-6);
    const X = t => LANE_LABEL + (t - S.tl.start) / span * (w - LANE_LABEL - 4);
    // Ruler.
    g.fillStyle = '#262626';
    g.fillRect(0, 0, w, RULER);
    const stepMs = niceCount(span / 8);
    g.fillStyle = '#8c8c8c';
    g.strokeStyle = 'rgba(255,255,255,0.07)';
    for (let t = Math.ceil(S.tl.start / stepMs) * stepMs; t <= S.tl.end; t += stepMs) {
      const x = Math.round(X(t)) + 0.5;
      g.beginPath(); g.moveTo(x, RULER - 4); g.lineTo(x, h); g.stroke();
      g.fillText(`${+t.toFixed(4)} ms`, x + 3, RULER / 2);
    }
    // Lanes.
    for (const lane of lanes) {
      g.fillStyle = '#242424';
      g.fillRect(0, lane.y, w, lane.rows * ROW + 2);
      g.save();
      g.beginPath();
      g.rect(LANE_LABEL, lane.y, w - LANE_LABEL, lane.rows * ROW + 2);
      g.clip();
      for (const z of lane.zones) {
        const row = lane.kind === 'gpu' ? z.level : z.depth;
        const x1 = X(z.s), x2 = X(z.e);
        if (x2 < LANE_LABEL || x1 > w) continue;
        const bx = Math.max(x1, LANE_LABEL), bw = Math.max(x2 - bx, 1);
        const y = lane.y + 1 + row * ROW;
        g.fillStyle = lane.kind === 'gpu' ? colorFor('gpu:' + (z.level === 0 ? z.name : z.parent ? z.parent.name : z.name)) : CAT_COLORS[catOf(z.name)];
        if (lane.kind === 'gpu' && z.level === 1) g.globalAlpha = 0.75;
        g.fillRect(bx, y, bw, ROW - 2);
        g.globalAlpha = 1;
        if (S.tl.picked === z || S.tl.hover === z) {
          g.strokeStyle = S.tl.picked === z ? '#fff' : 'rgba(255,255,255,0.6)';
          g.strokeRect(bx + 0.5, y + 0.5, bw - 1, ROW - 3);
        }
        if (bw > 34) {
          g.fillStyle = '#101010';
          const label = `${nameOf(z.name)} ${ms(z.e - z.s, 3)}`;
          g.fillText(fitText(g, label, bw - 6), bx + 3, y + (ROW - 2) / 2);
        }
      }
      g.restore();
      g.fillStyle = '#2c2c2c';
      g.fillRect(0, lane.y, LANE_LABEL - 2, lane.rows * ROW + 2);
      g.fillStyle = '#cfcfcf';
      g.fillText(fitText(g, lane.label, LANE_LABEL - 10), 6, lane.y + ROW / 2 + 1);
    }
  }
  function fitText(g, text, width) {
    if (g.measureText(text).width <= width) return text;
    let lo = 0, hi = text.length;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (g.measureText(text.slice(0, m) + '…').width <= width) lo = m; else hi = m - 1; }
    return lo > 0 ? text.slice(0, lo) + '…' : '';
  }
  function zoneAt(mx, my) {
    const w = R.tlCanvas.clientWidth;
    const span = Math.max(S.tl.end - S.tl.start, 1e-6);
    const t = S.tl.start + (mx - LANE_LABEL) / (w - LANE_LABEL - 4) * span;
    for (const lane of S.tl.lanes) {
      if (my < lane.y || my >= lane.y + lane.rows * ROW + 2) continue;
      const row = Math.floor((my - lane.y - 1) / ROW);
      let best = null;
      for (const z of lane.zones) {
        if ((lane.kind === 'gpu' ? z.level : z.depth) !== row) continue;
        const tol = 2 / (w - LANE_LABEL) * span;
        if (t >= z.s - tol && t <= z.e + tol) best = z;
      }
      return best ? { zone: best, lane } : null;
    }
    return null;
  }
  function timelineInput() {
    const cv = R.tlCanvas;
    on(cv, 'wheel', e => {
      e.preventDefault();
      const r = cv.getBoundingClientRect();
      const w = cv.clientWidth;
      const span = S.tl.end - S.tl.start;
      const f = Math.max(0, Math.min(1, (e.clientX - r.left - LANE_LABEL) / (w - LANE_LABEL - 4)));
      const at = S.tl.start + f * span;
      const k = e.deltaY > 0 ? 1.25 : 0.8;
      const ns = Math.max(span * k, 0.0005);
      S.tl.start = at - f * ns;
      S.tl.end = S.tl.start + ns;
      S.tl.auto = false;
      drawTimeline();
    }, { passive: false });
    on(cv, 'mousedown', down => {
      down.preventDefault();
      R.view.parentElement.parentElement.focus();
      const r = cv.getBoundingClientRect();
      const hit = zoneAt(down.clientX - r.left, down.clientY - r.top);
      S.tl.picked = hit ? hit.zone : null;
      const x0 = down.clientX, s0 = S.tl.start, e0 = S.tl.end, w = cv.clientWidth;
      const move = e => {
        const dt = (e.clientX - x0) / (w - LANE_LABEL - 4) * (e0 - s0);
        S.tl.start = s0 - dt; S.tl.end = e0 - dt; S.tl.auto = false;
        drawTimeline();
      };
      const up = () => { document.removeEventListener('mousemove', move); document.removeEventListener('mouseup', up); };
      document.addEventListener('mousemove', move);
      document.addEventListener('mouseup', up);
      drawTimeline();
    });
    on(cv, 'dblclick', () => { S.tl.auto = true; if (S.detail) { S.tl.start = 0; S.tl.end = Math.max(S.detail.cpuMs, S.detail.gpuMs, 0.01); } drawTimeline(); });
    on(cv, 'mousemove', e => {
      const r = cv.getBoundingClientRect();
      const hit = zoneAt(e.clientX - r.left, e.clientY - r.top);
      const z = hit ? hit.zone : null;
      if (z !== S.tl.hover) { S.tl.hover = z; drawTimeline(); }
      if (!z) { R.tlTip.hidden = true; return; }
      const gpu = hit.lane.kind === 'gpu';
      const cat = gpu ? 'GPU' + (z.level === 0 ? ' section' : ' pass') : (S.cats[catOf(z.name)] || '');
      R.tlTip.replaceChildren(el('b', {}, nameOf(z.name)),
        el('div', {}, `${ms(z.e - z.s, 3)} ms total  ·  ${ms(z.self, 3)} ms self`),
        el('div', { class: 'muted' }, `${hit.lane.label}  ·  ${cat}  ·  ${ms(z.s, 3)} → ${ms(z.e, 3)} ms`));
      R.tlTip.hidden = false;
      const vr = R.view.getBoundingClientRect();
      R.tlTip.style.left = Math.min(e.clientX - vr.left + 14, vr.width - 260) + 'px';
      R.tlTip.style.top = (e.clientY - vr.top + 14) + 'px';
    });
    on(cv, 'mouseleave', () => { R.tlTip.hidden = true; if (S.tl.hover) { S.tl.hover = null; drawTimeline(); } });
  }

  // ---- hierarchy

  function hierarchySources() {
    const d = S.detail;
    if (!d) return [];
    return [...d.threads.map(t => [`cpu:${t.id}`, `CPU · ${t.name}`]), ...(d.gpu.length ? [['gpu', 'GPU']] : [])];
  }
  function mergedTree(zones, frameMs, gpu) {
    const root = { name: -1, total: 0, self: 0, calls: 0, max: 0, kids: new Map(), key: '' };
    const add = (node, z, parentKey) => {
      let n = node.kids.get(z.name);
      const key = parentKey + '/' + z.name;
      if (!n) { n = { name: z.name, total: 0, self: 0, calls: 0, max: 0, kids: new Map(), key }; node.kids.set(z.name, n); }
      const d = z.e - z.s;
      n.total += d; n.self += z.self; n.calls += 1; n.max = Math.max(n.max, d);
      for (const k of z.kids) add(n, k, key);
    };
    for (const z of zones) if (gpu ? z.level === 0 : !z.parent) add(root, z, '');
    root.total = frameMs;
    return root;
  }
  function renderHierarchy() {
    const d = S.detail;
    const sources = hierarchySources();
    let source = sources.find(s => s[0] === S.hierSource) ? S.hierSource : (sources.find(s => s[1].endsWith('Main')) || sources[0] || [''])[0];
    S.hierSource = source;
    if (!R.hierSelect || !R.viewTools.contains(R.hierSelect)) {
      R.hierSelect = el('select', { class: 'field psel' });
      on(R.hierSelect, 'change', () => { S.hierSource = R.hierSelect.value; renderHierarchy(); });
      R.hierSearch = el('input', { class: 'field psearch', placeholder: 'Search', value: S.hierSearch });
      on(R.hierSearch, 'input', () => { S.hierSearch = R.hierSearch.value; renderHierarchy(); });
      R.viewTools.replaceChildren(R.hierSelect, R.hierSearch);
    }
    const options = sources.map(([v, t]) => `${v}=${t}`).join('|');
    if (R.hierSelect.dataset.options !== options) {
      R.hierSelect.dataset.options = options;
      R.hierSelect.replaceChildren(...sources.map(([v, t]) => el('option', { value: v }, t)));
    }
    R.hierSelect.value = source;
    if (!d) { R.view.replaceChildren(el('div', { class: 'pempty' }, 'Select a frame in the charts above.')); return; }
    const gpu = source === 'gpu';
    const thread = gpu ? null : d.threads.find(t => `cpu:${t.id}` === source);
    const frameMs = gpu ? d.gpuMs : d.cpuMs;
    const tree = mergedTree(gpu ? d.gpu : thread ? thread.zones : [], frameMs, gpu);
    const q = S.hierSearch.trim().toLowerCase();
    const matches = n => !q || nameOf(n.name).toLowerCase().includes(q) || [...n.kids.values()].some(matches);
    const [col, dir] = S.hierSort;
    const sortKids = n => [...n.kids.values()].sort((a, b) => col === 'name' ? dir * nameOf(a.name).localeCompare(nameOf(b.name)) : dir * (a[col] - b[col]));
    const rows = [];
    const walk = (n, depth) => {
      for (const k of sortKids(n)) {
        if (!matches(k)) continue;
        const open = q ? true : !S.collapsed.has(k.key);
        rows.push([k, depth, open]);
        if (open) walk(k, depth + 1);
      }
    };
    walk(tree, 0);
    const header = el('div', { class: 'prow phead' });
    for (const [key, label] of [['name', 'Name'], ['totalPct', 'Total'], ['selfPct', 'Self'], ['calls', 'Calls'], ['total', 'Time ms'], ['self', 'Self ms'], ['max', 'Max call ms']]) {
      const sortKey = key === 'totalPct' ? 'total' : key === 'selfPct' ? 'self' : key;
      const cell = el('span', { class: 'pcell' + (sortKey === col ? ' sorted' : '') }, label + (sortKey === col && key !== 'totalPct' && key !== 'selfPct' ? (dir < 0 ? ' ▾' : ' ▴') : ''));
      on(cell, 'mousedown', e => {
        e.preventDefault();
        S.hierSort = [sortKey, sortKey === col ? -dir : (sortKey === 'name' ? 1 : -1)];
        renderHierarchy();
      });
      header.append(cell);
    }
    const body = el('div', { class: 'pbody' });
    const pct = v => frameMs > 0 ? (v / frameMs * 100).toFixed(1) + '%' : '–';
    for (const [n, depth, open] of rows) {
      const arrow = el('span', { class: 'parrow' + (n.kids.size ? '' : ' leaf'), html: n.kids.size ? icon(open ? 'chev' : 'chevRight') : '' });
      const nameCell = css(el('span', { class: 'pcell pname' }, arrow,
        css(el('i', { class: 'pdot' }), { background: gpu ? colorFor('gpu:' + n.name) : CAT_COLORS[catOf(n.name)] }), nameOf(n.name)),
        { paddingLeft: `${4 + depth * 14}px` });
      const row = el('div', { class: 'prow' + (S.hierNode === n.key ? ' selected' : '') }, nameCell,
        el('span', { class: 'pcell' }, pct(n.total)), el('span', { class: 'pcell' }, pct(n.self)), el('span', { class: 'pcell' }, String(n.calls)),
        el('span', { class: 'pcell' }, ms(n.total, 3)), el('span', { class: 'pcell' }, ms(n.self, 3)), el('span', { class: 'pcell' }, ms(n.max, 3)));
      on(row, 'mousedown', e => {
        e.preventDefault();
        S.hierNode = n.key;
        if (e.target.closest('.parrow') && n.kids.size) { if (S.collapsed.has(n.key)) S.collapsed.delete(n.key); else S.collapsed.add(n.key); }
        renderHierarchy();
      });
      body.append(row);
    }
    const scroll = R.view.querySelector('.pbody');
    const keep = scroll ? scroll.scrollTop : 0;
    R.view.replaceChildren(header, body);
    body.scrollTop = keep;
  }

  // ---- counters

  function renderCounters() {
    R.viewTools.replaceChildren();
    const f = frameAt(S.selected);
    const header = el('div', { class: 'prow phead pcounters' }, ...['Module', 'Counter', 'Selected', 'Min', 'Avg', 'Max'].map(t => el('span', { class: 'pcell' }, t)));
    const body = el('div', { class: 'pbody' });
    S.counters.forEach((info, id) => {
      if (!info) return;
      let min = Infinity, max = -Infinity, sum = 0, n = 0;
      for (const fr of S.frames) { const v = fr.c[id]; if (Number.isFinite(v)) { min = Math.min(min, v); max = Math.max(max, v); sum += v; ++n; } }
      body.append(el('div', { class: 'prow pcounters' }, el('span', { class: 'pcell muted' }, info.module), el('span', { class: 'pcell' }, info.name),
        el('span', { class: 'pcell strong' }, human(f ? f.c[id] : NaN, info.unit)), el('span', { class: 'pcell' }, n ? human(min, info.unit) : '–'),
        el('span', { class: 'pcell' }, n ? human(sum / n, info.unit) : '–'), el('span', { class: 'pcell' }, n ? human(max, info.unit) : '–')));
    });
    const scroll = R.view.querySelector('.pbody');
    const keep = scroll ? scroll.scrollTop : 0;
    R.view.replaceChildren(header, body);
    body.scrollTop = keep;
  }

  // ---- memory

  function renderMemory() {
    const refresh = el('span', { class: 'ptool' }, 'Refresh');
    on(refresh, 'mousedown', e => { e.preventDefault(); S.memoryAsked = performance.now(); send('profiler.memory'); });
    R.viewTools.replaceChildren(refresh);
    if (!S.memory) { R.view.replaceChildren(el('div', { class: 'pempty' }, 'Loading GPU memory…')); return; }
    const total = S.memory.reduce((t, r) => t + r.size, 0);
    const modules = new Map();
    for (const r of S.memory) modules.set(r.module, (modules.get(r.module) || 0) + r.size);
    const bar = el('div', { class: 'pmembar' }, ...[...modules].sort((a, b) => b[1] - a[1]).map(([m, b]) =>
      css(el('span'), { flex: String(b), background: colorFor('mem:' + m) })));
    const mods = el('div', { class: 'pmemmods' }, el('b', {}, `GPU resources: ${bytes(total)} in ${S.memory.length} resources`),
      ...[...modules].sort((a, b) => b[1] - a[1]).map(([m, b]) => el('span', { class: 'pleg' }, css(el('i'), { background: colorFor('mem:' + m) }),
        el('span', { class: 'pleg-name' }, m), el('b', {}, `${bytes(b)} (${(b / total * 100).toFixed(1)}%)`))));
    const header = el('div', { class: 'prow phead pmem' }, ...['Module', 'Resource', 'Kind', 'Size'].map(t => el('span', { class: 'pcell' }, t)));
    const body = el('div', { class: 'pbody' }, ...S.memory.map(r => el('div', { class: 'prow pmem' },
      el('span', { class: 'pcell muted' }, r.module), el('span', { class: 'pcell' }, r.name || '(unnamed)'),
      el('span', { class: 'pcell muted' }, r.kind), el('span', { class: 'pcell strong' }, bytes(r.size)))));
    R.view.replaceChildren(el('div', { class: 'pmemtop' }, mods, bar), header, body);
  }

  // ------------------------------------------------------------------ public

  return {
    init(helpers) { ({ $, el, send, store, isTextField, toggleMaximize } = helpers); },
    onFrames, onDetail, onMemory,
    // The tab became visible / hidden.
    shown: guard(on => {
      S.visible = on;
      if (on && !S.built) build();
      subscribe();
      if (on) { queueDraw(); renderView(); }
    }),
    // tests (editor.probe)
    probe() {
      const d = S.detail;
      return {
        profFrames: S.frames.length, profGpuTiming: S.gpuTiming, profSubscribed: S.subscribed,
        profDetail: d ? d.index : -1, profThreads: d ? d.threads.length : 0, profThreadNames: S.threads.length,
        profZones: d ? d.threads.reduce((t, x) => t + x.zones.length, 0) : 0,
        profGpuMarks: d ? d.gpu.length : 0, profSelected: S.selected, profFollow: S.follow, profView: S.view,
        profHierRows: R.view && S.view === 'hierarchy' ? R.view.querySelectorAll('.pbody .prow').length : 0,
        profCounters: S.counters.filter(Boolean).length,
      };
    },
  };
})();
