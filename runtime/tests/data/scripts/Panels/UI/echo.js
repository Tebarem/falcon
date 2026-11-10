// editor_library_ui: says hello to its C# side, and tells it what came back.
FalconPanel.onMessage = v => {
  document.getElementById('status').textContent = 'got ' + v;
  FalconPanel.send('received ' + v);
};
FalconPanel.send('hello');
