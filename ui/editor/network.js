// The Network tab: the running game's session (Falcon.Network.Status), four times a second while
// it shows: its role and mode, the players with their ping, loss and traffic, and the host's,
// prediction's or rollback's numbers. Play with clients from the Play bar (Clients, Conditions)
// to see a session from the editor.
const NetworkPanel = (() => {
  let h = null;          // the editor's helpers (init)
  let root = null;
  let visible = false;
  let last = null;       // the last status (net.status's)

  function init(helpers) {
    h = helpers;
  }

  function shown(on) {
    if (on === visible)
      return;
    visible = on;
    if (on && !root) {
      root = document.getElementById('network');
      render();
    }
    h.send('net.subscribe', { on });
  }

  function onStatus(p) {
    last = p;
    if (visible)
      render();
  }

  const row = (label, value) => h.el('div', { class: 'net-row' }, h.el('span', { class: 'net-label' }, label), h.el('span', { class: 'net-value' }, String(value)));

  function render() {
    if (!root)
      return;
    const s = last && last.status;
    if (!last || !last.playing) {
      root.replaceChildren(h.el('div', { class: 'empty' },
        'Play to see the game\'s network. The Play bar\'s Clients starts client processes of this scene with it (Project Settings > Network: how sessions run).'));
      return;
    }
    if (!s || s.role === 'none') {
      root.replaceChildren(h.el('div', { class: 'empty' }, 'The game isn\'t in a session (no Clients on the Play bar, and it doesn\'t host or connect itself).'));
      return;
    }
    const parts = [
      h.el('div', { class: 'net-summary' },
        row('Role', s.role), row('Mode', s.mode), row('Tick', s.tick), row('Port', s.port || '—'), row('Objects', s.objects),
        row('Send rate', `${s.sendRate}/s`), row('Budget', `${s.bandwidthKbps} kbit/s a client`)),
    ];
    const table = h.el('table', { class: 'net-players' },
      h.el('thead', {}, h.el('tr', {}, ['Player', 'Name', 'Ping', 'Loss', 'Sent', 'Received', 'Steam'].map(t => h.el('th', {}, t)))),
      h.el('tbody', {}, (s.players || []).map(c => h.el('tr', {},
        h.el('td', {}, String(c.id)), h.el('td', {}, c.name + (c.bot ? ' (bot)' : '')), h.el('td', {}, `${c.ping} ms`),
        h.el('td', {}, `${c.loss}%`), h.el('td', {}, `${c.sentKB} KB`), h.el('td', {}, `${c.receivedKB} KB`), h.el('td', {}, c.steamId || '')))));
    parts.push(h.el('div', { class: 'section-title' }, `Players (${(s.players || []).length})`), table);
    if (s.host)
      parts.push(h.el('div', { class: 'section-title' }, 'The host\'s sends'), h.el('div', { class: 'net-summary' },
        row('Encode', `${s.host.encodeMs} ms`), row('Clients', `${s.host.clientsMs} ms`), row('Send', `${s.host.sendMs} ms`),
        row('Out', `${s.host.outKBs} KB/s`), row('Objects a player', s.host.objectsPerPlayer)));
    if (s.prediction)
      parts.push(h.el('div', { class: 'section-title' }, 'Prediction'), h.el('div', { class: 'net-summary' },
        row('Predicted ticks', s.prediction.predicted), row('Checked', s.prediction.checked), row('Corrected', s.prediction.corrected)));
    if (s.rollback)
      parts.push(h.el('div', { class: 'section-title' }, 'Rollback'), h.el('div', { class: 'net-summary' },
        row('Started', s.rollback.started ? 'yes' : 'waiting for players'), row('Waiting for inputs', s.rollback.waiting ? 'yes' : 'no'),
        row('Rollbacks', s.rollback.rollbacks), row('Ticks simulated again', s.rollback.resimulated), row('Inputs too late', s.rollback.tooLate),
        row('Hashes checked', s.rollback.hashChecks), row('Desyncs', s.rollback.desyncs)));
    root.replaceChildren(...parts);
  }

  // (editor tests: test.probe)
  function probe() {
    const s = last && last.status;
    return {
      networkShown: visible,
      networkPlayers: s && s.players ? s.players.length : 0,
      networkRole: s ? s.role : '',
      networkMode: s ? s.mode : '',
    };
  }

  return { init, shown, onStatus, probe };
})();
