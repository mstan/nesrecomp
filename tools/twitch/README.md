# Twitch chat in the game

Twitch chat shows up inside the game window, and viewers can use `!commands`
that change the game: power-ups, health, lives, gravity, swapped controls and
so on. There are two parts:

- **The window** (`runner/cyc/cyc_chat.c`). It draws the chat panel, a big
  banner and timers for effects that are running, on top of the picture.
  It takes `chat_*` requests on its TCP debug port (listed in `cyc_chat.h`).
- **The bridge** (`twitch_bridge.mjs`). It reads a Twitch channel anonymously
  (no account and no token, so it can only read) and turns each message into
  those requests. What each command does comes from a game profile in
  `profiles/`.

## Running it

```powershell
# 1. the game, with its TCP port open (localhost only)
SuperMarioBrosRecomp.exe smb1.nes --tcp 4370
# 2. the bridge (Node 22+)
node tools/twitch/twitch_bridge.mjs --profile smb1.json --channel YOUR_CHANNEL --port 4370
```

You can start them in either order, because both sides keep retrying. To test
without Twitch, use `--stdin` and type lines yourself: `name: text` is sent as
a viewer, and a plain line is sent as the broadcaster (no cooldowns). Use
`--both` to read Twitch and stdin at the same time.

The TCP server serves one client at a time. While the bridge is connected, no
other TCP script can drive the window.

## Profiles

```jsonc
{
  "name": "Super Mario Bros.",
  "chat": { "side": "left", "valign": "top", "offset": 18, "lines": 6, "ms": 20000 },
  "cooldown_ms": 1500,          // gap between any two effects
  "user_cooldown_ms": 8000,     // per viewer
  "command_cooldown_ms": 10000, // default per command
  "conditions": { "playing": [{ "addr": "0x0770", "eq": 1 }] },
  "aliases": { "1up": "oneup" },
  "rewards": { "<custom-reward-id>": "star" },  // channel points
  "commands": {
    "star": {
      "who": "everyone",        // everyone | sub | vip | mod | broadcaster
      "bits": 0,                // a cheer of at least this many bits is required
      "cooldown_ms": 60000,
      "when": "playing",        // a condition name, or [{addr, eq|ne|min|max, mask}]
      "banner": "{user} GAVE MARIO A STAR!",
      "steps": [
        { "ram":  { "addr": "0x079F", "set": "0x23" } },
        { "pad":  { "buttons": "a+right", "mode": "press", "frames": 20 } },
        { "call": { "addr": "0x85F1" } },
        { "banner": { "text": "...", "color": "FFD25A", "ms": 2500 } },
        { "wait_ms": 500 }
      ]
    },
    "powerup": { "variants": [ { "when": [...], "steps": [...] }, { "when": [...], "steps": [...] } ] }
  }
}
```

- A **`ram`** step does one of `set`, `add`, `or` or `and` on a byte. `add`
  stays within `min`..`max`. Add `frames` to repeat it every frame (or every
  `every` frames) for that long. Add `if_addr`/`if_eq` to skip it while
  another byte has a different value. Add `label` to show it with a timer.
- A **`pad`** step changes player 1's buttons for `frames`. The modes are
  `press`, `tap` (8 frames pressed, 8 released), `block` and `invert` (swaps
  Left/Right and/or Up/Down).
- A **`call`** step runs one of the game's own routines right away and keeps
  what it changed in RAM. This is how SMB refreshes Mario's palette.
- The steps between two waits are sent in one write. That way they all apply
  in the same gap between two frames, and the game never sees a change made
  of several stores half done.
- `variants` are tried in order. The first one whose `when` holds runs.
- The broadcaster and moderators skip cooldowns, and so do messages that
  carry bits or redeem a channel-point reward. The bridge logs the id of any
  reward it doesn't know, so you can copy it into `rewards`.
- `!commands` (or `!help`) shows the commands everyone can use in the chat
  panel.

## Voting

```jsonc
"vote": { "mode": "both", "options": 3, "duration_ms": 20000, "gap_ms": 15000,
          "y": 22, "title": "CHAT VOTE", "pool": ["star", "moon", "invert"], "show_votes": false }
```

A round puts `options` commands from `pool` on a ballot. The default pool is
every command that everyone can use and that needs no bits. The game shows
the ballot as a poll panel `y` percent of the way down the screen, with live
bars and a countdown. Viewers vote with `1`, `2`, `3`, with `!vote 2`, or
with the command itself (`!star`). Each viewer has one vote, and their latest
vote counts.

When the round ends, the option with the most votes runs, ignoring its
cooldown. Ties are broken at random. If the winner can't run right now (its
`when` doesn't hold), the next option in line runs instead. A new round
starts every `duration_ms + gap_ms`. Counted votes don't show up as chat
lines unless `show_votes` is set.

- `mode`: `anarchy` means only direct commands. `vote` means only votes for
  viewers; the broadcaster, moderators, bits and rewards still run commands
  directly. `both` runs votes and direct commands together. The bridge's
  `--mode` flag overrides it.
- The broadcaster and moderators can type `!vote off`, `!vote on` or
  `!vote now` (same as `on`).

`smb1.json` and `mm3.json` were checked against the real games:
`prove-it/twitch/smb-live.mjs` and `mm3-live.mjs` in the nes-decomp
workspace. Tests: `node --test tools/twitch/twitch.test.mjs`.
