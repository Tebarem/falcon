// falcon-panel.js: for a library's editor window page (UI/*.html, opened by an [EditorWindow] C#
// class). Include it with <script src="falcon-panel.js"></script> (a copy beside the page: a new
// library gets one).
//
//   FalconPanel.send(value)          a JSON value to the window's C# side (EditorWindow.OnMessage)
//   FalconPanel.onMessage = v => {}  what its C# side sends (EditorWindow.Send)
//
// The page takes the editor's colours as CSS variables (--app, --panel, --text, --text-dim,
// --accent, --line, --field, --button, ...), so `background: var(--panel)` matches the editor.
'use strict';
const FalconPanel = (() => {
  const panel = {
    send(value) { parent.postMessage({ falcon: 'panel', data: value === undefined ? null : value }, '*'); },
    onMessage: null,
  };
  addEventListener('message', e => {
    const m = e.data;
    if (!m || e.source !== parent) return;
    if (m.falcon === 'theme') {
      for (const [name, value] of Object.entries(m.vars || {})) document.documentElement.style.setProperty(name, value);
      document.documentElement.classList.add('falcon-themed');
    } else if (m.falcon === 'panel' && panel.onMessage) {
      panel.onMessage(m.data);
    }
  });
  return panel;
})();
