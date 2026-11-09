#!/usr/bin/env node
// twitch_bridge.mjs - Twitch chat into a running nesrecomp game window.
//
//   node twitch_bridge.mjs --profile profiles/smb1.json --channel NAME --port 4370
//   node twitch_bridge.mjs --profile profiles/mm3.json --stdin --port 4371
//
// Start the game with --tcp PORT first (any order works: both sides retry).
// --channel  the Twitch channel to read (anonymous, read-only: no token)
// --stdin    type chat yourself: "name: text" (as a viewer) or "text" (as the broadcaster)
// --both     Twitch and stdin together
// --mode     anarchy (commands run directly), vote (chat votes every round) or both;
//            overrides the profile's vote.mode. Mods switch with !vote on|off|now
// See README.md for the profile format.

import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath } from 'node:url';
import { connectGame } from './game.mjs';
import { createEngine } from './engine.mjs';
import { connectTwitch } from './irc.mjs';

function args(argv) {
  const out = { port: 4370, stdin: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const val = () => {
      if (i + 1 >= argv.length) throw new Error(`${a} needs a value`);
      return argv[++i];
    };
    if (a === '--profile') out.profile = val();
    else if (a === '--channel') out.channel = val();
    else if (a === '--port') out.port = Number(val());
    else if (a === '--stdin') out.stdin = true;
    else if (a === '--both') out.both = true;
    else if (a === '--mode') out.mode = val();
    else if (a === '--help' || a === '-h') out.help = true;
    else throw new Error(`unknown argument ${a}`);
  }
  return out;
}

function usage() {
  const here = path.dirname(fileURLToPath(import.meta.url));
  const profiles = fs.readdirSync(path.join(here, 'profiles')).filter((f) => f.endsWith('.json'));
  console.log(`usage: node twitch_bridge.mjs --profile FILE (--channel NAME | --stdin | --channel NAME --both) [--port 4370] [--mode anarchy|vote|both]
profiles here: ${profiles.join(', ')}`);
}

function main() {
  let o;
  try {
    o = args(process.argv.slice(2));
  } catch (e) {
    console.error(e.message);
    usage();
    process.exit(2);
  }
  if (o.help || !o.profile || (!o.channel && !o.stdin)) {
    usage();
    process.exit(o.help ? 0 : 2);
  }
  const here = path.dirname(fileURLToPath(import.meta.url));
  const file = fs.existsSync(o.profile) ? o.profile : path.join(here, 'profiles', o.profile);
  const profile = JSON.parse(fs.readFileSync(file, 'utf8'));
  if (o.mode) profile.vote = { ...profile.vote, mode: o.mode };
  const log = (s) => console.log(`[${new Date().toLocaleTimeString()}] ${s}`);

  let engine;
  const game = connectGame({
    port: o.port,
    onStatus: (s) => {
      log(s);
      if (game?.connected) engine.configure().catch((e) => log(`chat_config: ${e.message}`));
    },
  });
  engine = createEngine(profile, { game, log });
  log(`${engine.profile.name}: ${engine.profile.commands.size} commands; ${engine.helpLine()}`);
  const v = engine.profile.vote;
  log(`voting: ${v.mode}${v.mode === 'anarchy' ? ' (mods: !vote on)' : ''}, ${v.options} options, ` +
      `${v.durationMs / 1000}s rounds every ${(v.durationMs + v.gapMs) / 1000}s`);
  const stopVotes = engine.runVotes();

  const feed = (msg) => {
    if (!game.connected) return; // nothing to show it on
    engine.handle(msg);
  };

  let irc = null;
  if (o.channel) irc = connectTwitch({ channel: o.channel, onMessage: feed, onStatus: (s) => log(`twitch: ${s}`) });
  if (o.stdin || o.both) {
    const rl = readline.createInterface({ input: process.stdin });
    rl.on('line', (line) => {
      const m = /^([A-Za-z0-9_]{1,25}):\s*(.*)$/.exec(line);
      const viewer = !!m;
      const user = m ? m[1] : 'broadcaster';
      feed({ user, login: user.toLowerCase(), text: m ? m[2] : line, color: '', bits: 0, rewardId: '',
             broadcaster: !viewer, mod: false, vip: false, sub: false });
    });
    rl.on('close', () => { if (!irc) setTimeout(() => process.exit(0), 500); });
  }
  process.on('SIGINT', () => {
    stopVotes();
    irc?.close();
    game.close();
    process.exit(0);
  });
}

main();
