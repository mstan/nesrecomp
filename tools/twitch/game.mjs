// game.mjs - the game window's TCP debug server (runner/cyc/cyc_tcp.h) as a
// promise API, reconnecting while the game is closed or restarted.
//
//   const game = connectGame({ port, onStatus });
//   await game.send('chat', { user, text });   // resolves with the reply, rejects with err
//
// The server takes one client at a time: while the bridge is connected no
// other TCP script can drive the window.

import net from 'node:net';

export function connectGame({ host = '127.0.0.1', port, onStatus = () => {}, timeoutMs = 3000 }) {
  let sock = null;
  let ready = false;
  let closed = false;
  let nextId = 1;
  let buf = '';
  const pending = new Map();

  const fail = (why) => {
    for (const [, p] of pending) p.reject(new Error(why));
    pending.clear();
  };

  const open = () => {
    if (closed) return;
    sock = net.connect({ host, port });
    sock.setEncoding('utf8');
    sock.on('connect', () => {
      ready = true;
      onStatus(`game connected on ${host}:${port}`);
    });
    sock.on('data', (chunk) => {
      buf += chunk;
      for (let nl; (nl = buf.indexOf('\n')) >= 0; ) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (!line) continue;
        let reply;
        try {
          reply = JSON.parse(line);
        } catch {
          continue;
        }
        const p = pending.get(reply.id);
        if (!p) continue;
        pending.delete(reply.id);
        clearTimeout(p.timer);
        if (reply.ok) p.resolve(reply);
        else p.reject(new Error(reply.err ?? 'error'));
      }
    });
    sock.on('error', () => {});
    sock.on('close', () => {
      if (ready) onStatus('game disconnected; waiting for it');
      ready = false;
      buf = '';
      fail('game disconnected');
      if (!closed) setTimeout(open, 2000);
    });
  };
  open();

  return {
    get connected() {
      return ready;
    },
    send(cmd, fields = {}) {
      return this.batch([[cmd, fields]]).then((r) => r[0]);
    },
    // Several requests in one write: the window reads them in one poll, so
    // they all apply in the same gap between two frames.
    batch(requests) {
      if (!ready) return Promise.reject(new Error('game not connected'));
      let text = '';
      const replies = requests.map(([cmd, fields = {}]) => {
        const id = nextId++;
        text += JSON.stringify({ cmd, id, ...fields }) + '\n';
        return new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            pending.delete(id);
            reject(new Error(`${cmd}: no reply`));
          }, timeoutMs);
          pending.set(id, { resolve, reject, timer });
        });
      });
      sock.write(text);
      return Promise.all(replies);
    },
    close() {
      closed = true;
      sock?.destroy();
    },
  };
}
