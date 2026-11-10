// editor_game_ui: tells the game it is ready, and what the game answered.
FalconUi.onMessage = json => {
  const m = JSON.parse(json);
  document.getElementById('said').textContent = m.type;
  FalconUi.send(JSON.stringify({ type: 'got', what: m.type }));
};
FalconUi.send(JSON.stringify({ type: 'ready' }));
