// irc.mjs - read a Twitch channel's chat anonymously over Twitch's IRC
// WebSocket (no account, no token: the "justinfan" read-only login).
//
//   const irc = connectTwitch({ channel, onMessage, onStatus });
//   irc.close();
//
// onMessage({ user, login, text, color, bits, rewardId, mod, broadcaster, vip, sub, id })

const URL = 'wss://irc-ws.chat.twitch.tv:443';

// Tag values escape ; space \ CR LF (IRCv3 message-tags).
function unescapeTag(v) {
  return v.replace(/\\(.)/g, (_, c) => ({ ':': ';', s: ' ', '\\': '\\', r: '\r', n: '\n' })[c] ?? c);
}

// One IRC line: @tags :prefix COMMAND params... :trailing
export function parseIrcLine(line) {
  let rest = line;
  const tags = {};
  if (rest.startsWith('@')) {
    const sp = rest.indexOf(' ');
    for (const kv of rest.slice(1, sp).split(';')) {
      const eq = kv.indexOf('=');
      if (eq < 0) tags[kv] = '';
      else tags[kv.slice(0, eq)] = unescapeTag(kv.slice(eq + 1));
    }
    rest = rest.slice(sp + 1);
  }
  let prefix = '';
  if (rest.startsWith(':')) {
    const sp = rest.indexOf(' ');
    prefix = rest.slice(1, sp);
    rest = rest.slice(sp + 1);
  }
  let trailing = null;
  const colon = rest.indexOf(' :');
  if (colon >= 0) {
    trailing = rest.slice(colon + 2);
    rest = rest.slice(0, colon);
  } else if (rest.startsWith(':')) {
    trailing = rest.slice(1);
    rest = '';
  }
  const parts = rest.split(' ').filter(Boolean);
  const command = parts.shift() ?? '';
  if (trailing !== null) parts.push(trailing);
  return { tags, prefix, command, params: parts };
}

// A PRIVMSG as the bridge sees it, or null for anything else.
export function chatMessage(msg) {
  if (msg.command !== 'PRIVMSG') return null;
  const t = msg.tags;
  const login = msg.prefix.split('!')[0];
  const badges = (t.badges ?? '').split(',').map((b) => b.split('/')[0]);
  let text = msg.params[1] ?? '';
  // /me actions arrive as CTCP ACTION
  const action = /^\u0001ACTION (.*)\u0001$/.exec(text);
  if (action) text = action[1];
  return {
    id: t.id ?? '',
    login,
    user: t['display-name'] || login,
    text,
    color: t.color || '',
    bits: Number(t.bits ?? 0) || 0,
    rewardId: t['custom-reward-id'] ?? '',
    broadcaster: badges.includes('broadcaster'),
    mod: t.mod === '1' || badges.includes('moderator'),
    vip: t.vip === '1' || badges.includes('vip'),
    sub: t.subscriber === '1' || badges.includes('subscriber'),
  };
}

export function connectTwitch({ channel, onMessage, onStatus = () => {}, WebSocketImpl = globalThis.WebSocket }) {
  const chan = '#' + channel.replace(/^#/, '').toLowerCase();
  let ws = null;
  let closed = false;
  let backoff = 1000;
  let timer = null;

  const open = () => {
    if (closed) return;
    onStatus(`connecting to ${chan}`);
    ws = new WebSocketImpl(URL);
    ws.onopen = () => {
      backoff = 1000;
      ws.send('CAP REQ :twitch.tv/tags twitch.tv/commands');
      ws.send('PASS SCHMOOPIIE');
      ws.send(`NICK justinfan${10000 + Math.floor(Math.random() * 80000)}`);
      ws.send(`JOIN ${chan}`);
    };
    ws.onmessage = (ev) => {
      for (const line of String(ev.data).split('\r\n')) {
        if (!line) continue;
        const msg = parseIrcLine(line);
        if (msg.command === 'PING') ws.send(`PONG :${msg.params[0] ?? 'tmi.twitch.tv'}`);
        else if (msg.command === 'RECONNECT') ws.close();
        else if (msg.command === 'JOIN') onStatus(`joined ${chan}`);
        else if (msg.command === 'NOTICE') onStatus(`notice: ${msg.params.at(-1)}`);
        else {
          const chat = chatMessage(msg);
          if (chat) onMessage(chat);
        }
      }
    };
    ws.onclose = () => {
      if (closed) return;
      onStatus(`disconnected; retrying in ${backoff / 1000}s`);
      timer = setTimeout(open, backoff);
      backoff = Math.min(backoff * 2, 30000);
    };
    ws.onerror = () => {}; // onclose follows
  };
  open();
  return {
    close() {
      closed = true;
      clearTimeout(timer);
      ws?.close();
    },
  };
}
