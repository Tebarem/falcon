// falcon-ui.js: for a game's HTML UI (UI/Game/index.html): the same page works in a game (--ui UI/Game:
// the engine's bridge) and in the editor while playing (framed over the game's view).
//
//   FalconUi.send(json)          a message (JSON text) for the game: Ui.TryReceive
//   FalconUi.onMessage = json => {}  the game's messages (Ui.Send)
'use strict';
const FalconUi = (() => {
  const framed = window.parent !== window;
  const ui = {
    send(json) {
      if (framed) parent.postMessage({ falcon: 'ui', data: json }, '*');
      else if (window.falcon) window.falcon.postMessage(json);
    },
    onMessage: null,
  };
  if (framed)
    addEventListener('message', e => { if (e.source === parent && e.data && e.data.falcon === 'ui' && ui.onMessage) ui.onMessage(e.data.data); });
  else if (window.falcon)
    window.falcon.onMessage = json => { if (ui.onMessage) ui.onMessage(json); };
  return ui;
})();
