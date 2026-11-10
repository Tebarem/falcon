// Sandbox HUD. The game (C#) sends hud.stats; buttons send hud.fountain / hud.kick back.
// The game keeps the mouse until Tab opens the menu (ui.input asks the engine for pointer input).
function send(type, payload = {}) {
  if (window.falcon && typeof window.falcon.postMessage === 'function') {
    window.falcon.postMessage(JSON.stringify({ type, ...payload }));
  }
}

function setText(id, text) {
  const element = document.getElementById(id);
  if (element.textContent !== text) {
    element.textContent = text;  // unchanged text: no repaint
  }
}

function setMenu(open) {
  document.body.classList.toggle('menu-open', open);
  send('ui.input', { pointer: open });
}

document.getElementById('toggle-fountain').addEventListener('click', () => send('hud.fountain'));
document.getElementById('kick').addEventListener('click', () => send('hud.kick'));
document.addEventListener('keydown', event => {
  if (event.key === 'Tab') {
    event.preventDefault();
    if (!event.repeat) {
      setMenu(!document.body.classList.contains('menu-open'));
    }
  }
});

// window.falcon is provided (read-only) by cef-lite; a plain browser gets a stand-in.
if (!window.falcon) window.falcon = {};
window.falcon.onMessage = message => {
  const parsed = JSON.parse(message);
  // Several messages can arrive together as one ui.batch.
  for (const m of parsed.type === 'ui.batch' ? parsed.messages : [parsed]) {
    onHudMessage(m);
  }
};

function onHudMessage(m) {
  if (m.type !== 'hud.stats') {
    return;
  }
  setText('fps', String(m.fps));
  setText('bodies', String(m.bodies));
  setText('levitating', String(m.levitating));
  setText('fountain', m.fountain ? 'on' : 'off');
  document.getElementById('fountain').className = m.fountain ? 'on' : 'off';
}

send('hud.ready');
