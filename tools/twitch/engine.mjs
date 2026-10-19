// engine.mjs - what a chat message does to the game, from a game profile
// (profiles/*.json, described in README.md): every message goes to the chat
// panel, and a !command that passes its permission, cooldown and game-state
// checks runs its steps through the window's chat_* requests (cyc_chat.h).

const BUTTONS = { a: 0x80, b: 0x40, select: 0x20, start: 0x10, up: 0x08, down: 0x04, left: 0x02, right: 0x01 };
const NAME_COLORS = ['FF4A4A', '4A8CFF', '3CC83C', 'FFB43C', 'C864FF', '3CD2D2', 'FF6EB4', 'B4DC3C'];

// "0x79F", "$079F", 1951 -> 1951
export function num(v, what = 'number') {
  if (typeof v === 'number' && Number.isInteger(v)) return v;
  if (typeof v === 'string') {
    const s = v.trim();
    const n = /^\$[0-9a-f]+$/i.test(s) ? parseInt(s.slice(1), 16) : Number(s);
    if (Number.isInteger(n)) return n;
  }
  throw new Error(`not a ${what}: ${JSON.stringify(v)}`);
}

// "a+b", ["left","a"], 0x82 -> NES button bits
export function buttons(v) {
  if (typeof v === 'number') return v;
  const names = Array.isArray(v) ? v : String(v).split(/[+, ]+/).filter(Boolean);
  let bits = 0;
  for (const n of names) {
    const b = BUTTONS[n.toLowerCase()];
    if (b === undefined) throw new Error(`unknown button ${n}`);
    bits |= b;
  }
  return bits;
}

// Chat text the in-game font can draw: printable ASCII, accents dropped.
export function printable(s, max = 200) {
  const ascii = s
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/[^\x20-\x7e]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  return ascii.length > max ? ascii.slice(0, max - 3) + '...' : ascii;
}

// The viewer's Twitch colour, lifted toward white when too dark to read on
// the panel's dark rows; a stable pick from a palette when they have none.
export function nameColor(msg) {
  const c = /^#?([0-9a-f]{6})$/i.exec(msg.color ?? '');
  if (c) {
    let [r, g, b] = [0, 2, 4].map((i) => parseInt(c[1].slice(i, i + 2), 16));
    const luma = (0.299 * r + 0.587 * g + 0.114 * b) / 255;
    if (luma < 0.45) [r, g, b] = [r, g, b].map((v) => Math.round(v + (255 - v) * 0.45));
    return [r, g, b].map((v) => v.toString(16).padStart(2, '0')).join('').toUpperCase();
  }
  let h = 0;
  for (const ch of msg.login ?? msg.user) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
  return NAME_COLORS[h % NAME_COLORS.length];
}

// One step of a command -> [request, fields] for the window, or a wait.
function compileStep(step, where) {
  const keys = Object.keys(step);
  if (keys.length !== 1) throw new Error(`${where}: a step has exactly one of ram, pad, call, banner, wait_ms`);
  const [kind] = keys;
  const s = step[kind];
  switch (kind) {
    case 'ram': {
      const f = { addr: num(s.addr, 'address') };
      const ops = ['set', 'add', 'or', 'and'].filter((k) => k in s);
      if (ops.length !== 1) throw new Error(`${where}: ram needs one of set, add, or, and`);
      f[ops[0]] = num(s[ops[0]]);
      for (const k of ['min', 'max', 'frames', 'every', 'if_addr', 'if_eq']) if (k in s) f[k] = num(s[k]);
      if (s.label) f.label = printable(s.label, 38);
      return ['chat_ram', f];
    }
    case 'pad': {
      const f = { buttons: buttons(s.buttons), mode: s.mode ?? 'press', frames: num(s.frames ?? 30) };
      if (!['press', 'tap', 'block', 'invert'].includes(f.mode)) throw new Error(`${where}: pad mode ${f.mode}`);
      if (s.label) f.label = printable(s.label, 38);
      return ['chat_pad', f];
    }
    case 'call': {
      const f = { addr: num(s.addr, 'address') };
      for (const k of ['a', 'x', 'y']) if (k in s) f[k] = num(s[k]);
      return ['chat_call', f];
    }
    case 'banner':
      return ['chat_banner', typeof s === 'string' ? { text: s } : { ...s }];
    case 'wait_ms':
      return ['wait', { ms: num(s) }];
    default:
      throw new Error(`${where}: unknown step ${kind}`);
  }
}

// [{addr, eq|ne|min|max, mask}] or the name of a profile condition.
function compileWhen(when, conditions, where) {
  if (when === undefined) return [];
  if (typeof when === 'string') {
    if (!conditions[when]) throw new Error(`${where}: no condition named ${when}`);
    return conditions[when];
  }
  return when.map((c) => {
    const out = { addr: num(c.addr, 'address'), mask: c.mask === undefined ? 0xff : num(c.mask) };
    for (const k of ['eq', 'ne', 'min', 'max']) if (k in c) out[k] = num(c[k]);
    return out;
  });
}

export function compileProfile(profile) {
  const conditions = {};
  for (const [name, list] of Object.entries(profile.conditions ?? {})) conditions[name] = compileWhen(list, {}, name);
  const commands = new Map();
  for (const [name, c] of Object.entries(profile.commands ?? {})) {
    const where = `!${name}`;
    const variants = (c.variants ?? [c]).map((v, i) => ({
      when: compileWhen(v.when ?? c.when, conditions, `${where} variant ${i}`),
      steps: (v.steps ?? []).map((s) => compileStep(s, where)),
      banner: v.banner ?? c.banner,
    }));
    commands.set(name.toLowerCase(), {
      name,
      help: c.help ?? '',
      who: c.who ?? 'everyone',
      bits: c.bits ?? 0,
      cooldownMs: c.cooldown_ms ?? profile.command_cooldown_ms ?? 10000,
      variants,
      title: printable(c.title ?? name.toUpperCase(), 20),
      votable: c.vote ?? ((c.who ?? 'everyone') === 'everyone' && !c.bits),
      lastUsed: -Infinity,
    });
  }
  const aliases = new Map(Object.entries(profile.aliases ?? {}).map(([a, n]) => [a.toLowerCase(), n.toLowerCase()]));
  for (const [a, n] of aliases) if (!commands.has(n)) throw new Error(`alias !${a}: no command !${n}`);
  const rewards = new Map(Object.entries(profile.rewards ?? {}).map(([id, n]) => [id, n.toLowerCase()]));
  const v = profile.vote ?? {};
  const mode = v.mode ?? 'anarchy';
  if (!['anarchy', 'vote', 'both'].includes(mode)) throw new Error(`vote.mode ${mode}: anarchy, vote or both`);
  const pool = (v.pool ?? [...commands.values()].filter((c) => c.votable).map((c) => c.name)).map((n) => n.toLowerCase());
  for (const n of pool) if (!commands.has(n)) throw new Error(`vote.pool: no command !${n}`);
  const vote = {
    mode,
    pool,
    options: Math.max(2, Math.min(5, v.options ?? 3)),
    durationMs: v.duration_ms ?? 20000,
    gapMs: v.gap_ms ?? 15000,
    title: printable(v.title ?? 'CHAT VOTE', 22),
    y: v.y ?? 20,
    showVotes: v.show_votes ?? false,
  };
  return {
    vote,
    name: profile.name ?? 'game',
    prefix: profile.prefix ?? '!',
    chat: profile.chat ?? {},
    globalCooldownMs: profile.cooldown_ms ?? 1500,
    userCooldownMs: profile.user_cooldown_ms ?? 8000,
    commands,
    aliases,
    rewards,
  };
}

function allowed(cmd, msg) {
  if (msg.broadcaster) return true;
  switch (cmd.who) {
    case 'broadcaster': return false;
    case 'mod': return msg.mod;
    case 'vip': return msg.mod || msg.vip;
    case 'sub': return msg.mod || msg.vip || msg.sub;
    default: return true;
  }
}

function holds(cond, byte) {
  const v = byte & cond.mask;
  return (cond.eq === undefined || v === cond.eq) && (cond.ne === undefined || v !== cond.ne) &&
    (cond.min === undefined || v >= cond.min) && (cond.max === undefined || v <= cond.max);
}

function fill(template, msg, cmd) {
  return printable(String(template).replaceAll('{user}', msg.user).replaceAll('{command}', cmd.name), 60);
}

// The engine. game.send(cmd, fields) talks to the window; now() is a clock in ms.
export function createEngine(profile, { game, now = () => Date.now(), log = () => {}, sleep, random = Math.random } = {}) {
  const p = compileProfile(profile);
  const wait = sleep ?? ((ms) => new Promise((r) => setTimeout(r, ms)));
  const users = new Map();
  let lastEffect = -Infinity;
  let queue = Promise.resolve();

  async function readByte(addr) {
    const r = await game.send('read_ram', { addr, len: 1 });
    return parseInt(r.hex, 16);
  }

  async function variantFor(cmd) {
    for (const v of cmd.variants) {
      let ok = true;
      for (const c of v.when) if (!holds(c, await readByte(c.addr))) { ok = false; break; }
      if (ok) return v;
    }
    return null;
  }

  // force: chat's vote chose it - no permission or cooldown check, and the
  // vote's own pseudo-user gets no user cooldown.
  async function run(cmd, msg, { force = false } = {}) {
    const t = now();
    if (!force && !allowed(cmd, msg)) return { ran: false, why: 'not allowed' };
    if (!force && cmd.bits && msg.bits < cmd.bits && !msg.reward) return { ran: false, why: `needs ${cmd.bits} bits` };
    const privileged = msg.broadcaster || msg.mod;
    if (!force && !privileged && !msg.reward && !msg.bits) {
      if (p.vote.mode === 'vote') return { ran: false, why: 'vote mode: commands win by vote' };
      if (t - lastEffect < p.globalCooldownMs) return { ran: false, why: 'global cooldown' };
      const u = users.get(msg.login) ?? -Infinity;
      if (t - u < p.userCooldownMs) return { ran: false, why: `user cooldown ${Math.ceil((p.userCooldownMs - (t - u)) / 1000)}s` };
      if (t - cmd.lastUsed < cmd.cooldownMs) return { ran: false, why: `cooldown ${Math.ceil((cmd.cooldownMs - (t - cmd.lastUsed)) / 1000)}s` };
    }
    const v = await variantFor(cmd);
    if (!v) return { ran: false, why: 'not now (game state)' };
    lastEffect = cmd.lastUsed = t;
    if (!force) users.set(msg.login, t);
    // steps between waits go as one batch: one gap between frames, so a state
    // change made of several stores is never seen half done
    let batch = [];
    const flush = async () => {
      if (batch.length) await game.batch(batch);
      batch = [];
    };
    for (const [req, fields] of v.steps) {
      if (req === 'wait') {
        await flush();
        await wait(fields.ms);
      } else {
        batch.push([req, req === 'chat_banner' ? { ...fields, text: fill(fields.text, msg, cmd) } : fields]);
      }
    }
    if (v.banner) batch.push(['chat_banner', { text: fill(v.banner, msg, cmd), color: 'FFD25A', ms: 2500 }]);
    await flush();
    return { ran: true };
  }

  function lookup(text) {
    if (!text.startsWith(p.prefix)) return null;
    const word = text.slice(p.prefix.length).split(/\s+/)[0].toLowerCase();
    const name = p.aliases.get(word) ?? word;
    return { word, cmd: p.commands.get(name) };
  }

  function helpLine() {
    return 'COMMANDS: ' + [...p.commands.values()].filter((c) => c.who === 'everyone').map((c) => p.prefix + c.name).join(' ');
  }

  // ---- voting: a round offers a few commands, chat answers 1..N (or the
  // command, or !vote N), one vote per viewer (the last one counts), and the
  // winner runs as if the broadcaster had typed it ----
  const round = { open: false, options: [], votes: new Map(), endsAt: 0, lastShown: 0 };
  let voteOn = p.vote.mode !== 'anarchy';

  const tally = () => round.options.map((_, i) => [...round.votes.values()].filter((v) => v === i).length);

  async function showPoll(extra = {}) {
    round.lastShown = now();
    await game.send('chat_poll', {
      title: p.vote.title,
      options: round.options.map((c) => c.title).join('|'),
      votes: tally().join('|'),
      ms: Math.max(500, round.endsAt - now()),
      y: p.vote.y,
      ...extra,
    });
  }

  // Start a round: options drawn at random from the pool.
  function startVote() {
    const job = queue.then(async () => {
      const pool = p.vote.pool.map((n) => p.commands.get(n));
      const options = [];
      const count = Math.min(p.vote.options, pool.length);
      while (options.length < count) options.push(pool.splice(Math.floor(random() * pool.length), 1)[0]);
      Object.assign(round, { open: true, options, votes: new Map(), endsAt: now() + p.vote.durationMs });
      log(`vote: ${options.map((c, i) => `${i + 1}=${c.name}`).join(' ')}`);
      await showPoll();
      return options.map((c) => c.name);
    });
    queue = job.catch(() => {});
    return job;
  }

  // Close the round: the most voted option that can run now runs (ties at random).
  function finishVote() {
    const job = queue.then(async () => {
      if (!round.open) return null;
      round.open = false;
      const counts = tally();
      const order = round.options.map((c, i) => ({ c, i, n: counts[i], r: random() }))
        .filter((o) => o.n > 0)
        .sort((a, b) => b.n - a.n || a.r - b.r);
      const chat = { user: 'CHAT', login: '', broadcaster: false, mod: false, bits: 0 };
      for (const o of order) {
        const r = await run(o.c, chat, { force: true });
        log(`vote: ${o.c.name} (${o.n} votes): ${r.ran ? 'ran' : r.why}`);
        if (!r.ran) continue;
        round.endsAt = now() + 4000;
        await showPoll({ winner: o.i, countdown: false, title: 'CHAT CHOSE' });
        await game.send('chat_banner', { text: `CHAT CHOSE ${o.c.title}!`, color: '7CFF7C', ms: 2500 });
        return { winner: o.c.name, votes: counts };
      }
      round.endsAt = now() + 3000;
      await showPoll({ countdown: false, title: order.length ? 'NOTHING COULD RUN' : 'NO VOTES' });
      return { winner: null, votes: counts };
    });
    queue = job.catch(() => {});
    return job;
  }

  // A vote, if this message is one; true when it was counted.
  function castVote(msg, text, hit) {
    if (!round.open || !msg.login) return false;
    let i = -1;
    const n = /^(?:!vote\s+)?#?([1-9])$/i.exec(text.trim());
    if (n) i = Number(n[1]) - 1;
    else if (hit?.cmd) i = round.options.indexOf(hit.cmd);
    if (i < 0 || i >= round.options.length) return false;
    round.votes.set(msg.login, i);
    return true;
  }

  // !vote on|off|now, for the broadcaster and moderators.
  function voteControl(msg, text) {
    const m = /^!vote\s+(on|off|now)$/i.exec(text.trim());
    if (!m || !(msg.broadcaster || msg.mod)) return null;
    voteOn = m[1].toLowerCase() !== 'off';
    if (m[1].toLowerCase() === 'off') round.open = false;
    return m[1].toLowerCase();
  }

  // One chat message: shown, then (serialised with the others) acted on.
  function handle(msg) {
    const text = printable(msg.text);
    const rewardCmd = msg.rewardId ? p.rewards.get(msg.rewardId) : undefined;
    if (msg.rewardId && !rewardCmd) log(`unmapped channel-point reward id ${msg.rewardId} (add it to "rewards")`);
    const job = queue.then(async () => {
      const hit = rewardCmd ? { word: rewardCmd, cmd: p.commands.get(rewardCmd) } : lookup(text);
      // a counted vote shows in the poll, not as a chat line (unless vote.show_votes)
      const voted = !rewardCmd && !msg.bits && castVote(msg, text, hit);
      if (text && (!voted || p.vote.showVotes)) {
        await game.send('chat', { user: printable(msg.user, 24), color: nameColor(msg), text }).catch((e) => log(`chat: ${e.message}`));
      }
      if (voted) {
        if (now() - round.lastShown > 300) await showPoll().catch(() => {});
        return { voted: true };
      }
      const control = voteControl(msg, text);
      if (control) {
        log(`${msg.user}: vote ${control}`);
        if (control === 'off') await game.send('chat_poll', {}).catch(() => {});
        return { vote: control };
      }
      if (!hit) return null;
      if (hit.word === 'commands' || hit.word === 'help') {
        if (!hit.cmd) await game.send('chat', { user: '', color: 'FFD25A', text: helpLine() }).catch(() => {});
        return { command: hit.word, ran: !hit.cmd };
      }
      if (!hit.cmd) return null;
      try {
        const r = await run(hit.cmd, { ...msg, reward: !!rewardCmd });
        log(`${msg.user} !${hit.cmd.name}: ${r.ran ? 'ran' : r.why}`);
        return { command: hit.cmd.name, ...r };
      } catch (e) {
        log(`${msg.user} !${hit.cmd.name}: failed: ${e.message}`);
        return { command: hit.cmd.name, ran: false, why: e.message };
      }
    });
    queue = job.catch(() => {});
    return job;
  }

  async function configure() {
    const c = p.chat;
    const fields = {};
    for (const k of ['show', 'side', 'valign', 'offset', 'lines', 'ms', 'scale']) if (k in c) fields[k] = c[k];
    if (Object.keys(fields).length) await game.send('chat_config', fields);
  }

  // The vote loop the bridge runs: a round every duration + gap while voting is on.
  function runVotes() {
    let timer = null;
    const next = (ms) => { timer = setTimeout(step, ms); };
    const step = async () => {
      try {
        if (!voteOn || !game.connected) return next(2000);
        if (!round.open) {
          await startVote();
          return next(p.vote.durationMs);
        }
        await finishVote();
        next(p.vote.gapMs);
      } catch (e) {
        log(`vote: ${e.message}`);
        next(5000);
      }
    };
    next(p.vote.gapMs);
    return () => clearTimeout(timer);
  }

  return {
    handle, configure, helpLine, profile: p, startVote, finishVote, runVotes,
    get voting() { return voteOn; },
  };
}
