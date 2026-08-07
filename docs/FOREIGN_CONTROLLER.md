# Foreign movement controllers

NESRecomp can let a movement model taken from a *different* game drive the
player in an NES title, without turning the runner into a host for that other
game's engine.

The split is the whole idea:

| Question | Answered by |
|---|---|
| How does the character *want* to move? | the foreign controller |
| Where is the player *allowed* to go? | the NES game |

The NES game stays authoritative for its world — levels, tiles, enemies,
scrolling, scripted sequences, progression. The controller contributes
locomotion semantics only.

Because the boundary is game-agnostic, one controller works on any NES title
that provides an adapter, and one adapter accepts any controller.

Header: `runner/include/foreign_controller.h`.
Implementation: `runner/src/foreign_controller.c`.
Inert unless a game registers and selects a controller.

---

## Three layers

```text
         ForeignController            "how do I want to move"
                 |                     high-precision host x/y/vx/vy
                 |                     analog-shaped input
                 |                     no NES knowledge at all
      requested_dx / requested_dy
                 |
           game adapter               "where may the player go"
                 |                     ownership state machine
                 |                     digital -> analog stick
                 |                     one documented world-scale transform
                 |                     sync-back into the game's own RAM
      ForeignCollisionResult
                 |
         native game logic
```

A controller that touches NES RAM addresses, metatiles, PPU state, OAM or
enemy slots has broken the contract and stopped being reusable. Keep that
knowledge in the adapter.

---

## Writing a controller

```c
#include "foreign_controller.h"

enum { MY_IDLE, MY_WALK, MY_AIR };

static void my_reset(ForeignState *s) {
    s->state = MY_IDLE;
    s->facing = 1.0f;
}

static void my_tick(ForeignState *s, const ForeignInput *in,
                    ForeignMoveResult *out) {
    s->vx += (double)in->stick_x * MY_GROUND_ACCEL;
    out->requested_dx = s->vx;
    out->requested_dy = s->vy;
    out->vx = s->vx;
    out->vy = s->vy;
    out->state = s->state;
}

static void my_resolve(ForeignState *s, const ForeignCollisionResult *hit) {
    s->x += hit->actual_dx;
    s->y += hit->actual_dy;
    s->grounded = hit->grounded;
    if (hit->hit_ceiling && s->vy < 0.0) s->vy = 0.0;
    if (hit->hit_floor   && s->vy > 0.0) s->vy = 0.0;
}

static const char *my_state_name(ForeignMoveState st) {
    switch (st) {
        case MY_IDLE: return "IDLE";
        case MY_WALK: return "WALK";
        case MY_AIR:  return "AIR";
    }
    return "?";
}

static const ForeignController kMyController = {
    "my-game.my-character", "My Character",
    my_reset, my_tick, my_resolve, my_state_name,
};

/* Register before main(); a mod archive then selects it by plugin id. */
nes_foreign_register(&kMyController);
```

Sign conventions must come from the source you are porting, not from this
example. The engine assumes only that `+y` is *down*, matching NES screen
coordinates, and only inside `nes_foreign_sweep`.

---

## Ownership

Ownership is host policy, and it matters more than it looks. NES games run
scripted player sequences — pipes, death, flagpoles, level intros, forced
walks — that expect their own state transitions. Never blanket-suppress the
game's player update.

```c
nes_foreign_set_ownership(FOREIGN_OWNERSHIP_FOREIGN);   /* ordinary play */
nes_foreign_set_ownership(FOREIGN_OWNERSHIP_SCRIPTED);  /* pipe, death, ... */
nes_foreign_set_ownership(FOREIGN_OWNERSHIP_NATIVE);    /* mod disabled */
```

`nes_foreign_tick()` ticks the controller only under `FOREIGN`, but records a
trace row under every ownership, so a handoff appears in the history as a
transition rather than a gap.

---

## Own your state; do not contend for guest bytes

The controller's state lives host-side, and that is a rule, not a convenience.

An imported movement model needs variables the host game never had — a dash
timer, a jumpsquat counter, subpixel position, high-precision velocity. There
is always a temptation to find somewhere in guest RAM to keep them. Don't.

NES games pack their scratch RAM hard, and a byte that looks free usually is
not. Worse, the same byte is often reachable under several names: in SMB
`$0086` is `Player_X_Position` *and* `SprObject_X_Position` (the player is
object slot 0), and `$0034` is a plain variable *and* an indexed array base
that collide at index 0. Renaming cannot separate them — there is one byte,
and the coupling is in the ROM.

So:

- **New state the controller invents lives in `ForeignState`.** It costs
  nothing, it is full precision, and it cannot collide with anything.
- **Write a guest byte only when the game must observe it** — position for
  collision and camera, facing for rendering. Document each one.
- **Scope those writes in time, not in space.** Taking over a routine with
  `[[mod_function_hook]]` puts the write inside the window where the game's
  own code expects that meaning; whatever else shares the byte writes it
  before reading it, exactly as it already does in the unmodified game.
- **Relocating a guest variable is a last resort**, and means hooking its
  consumer rather than hunting for free RAM.

## Coordinates and scale

Keep the authoritative position host-side at full precision, and project into
the game's representation at the adapter boundary through constants the
adapter names explicitly. NES player coordinates are typically a page plus a
pixel; quantizing acceleration, friction and air drift to that every frame
destroys the behaviour you ported.

There are two defensible readings of "authentic":

1. numerically identical source units — meaningless across unrelated world
   scales;
2. behaviourally identical after **one** uniform spatial scale conversion.

Use (2). Derive one scale from a stable reference (character height against
platform geometry versus character height against NES tile geometry), keep the
controller's constants internally authentic, and apply the scale once at the
boundary. Do not individually retune run speed, jump speed, gravity and air
acceleration — that is how a port becomes a hand-tuned approximation wearing
the original's name.

---

## Swept collision

A foreign controller often moves far more per tick than the host game's own
character, and the host's collision may only make assumptions valid at its
native speed.

```c
ForeignCollisionResult hit;
nes_foreign_sweep(state->x, state->y,
                  move.requested_dx, move.requested_dy,
                  2.0,                  /* never step more than 2px */
                  my_tile_blocked, ctx, /* the GAME's tiles, unchanged */
                  &hit);
nes_foreign_resolve(&hit);
```

The probe is called with candidate absolute positions and returns nonzero when
blocked. Axes are stepped separately so the character slides along walls and
ceilings instead of stopping dead. The game's tiles remain the only source of
truth; this only controls how finely they are asked.

Do not clamp the controller to the host character's speed to make collision
work — that defeats the point of importing the movement model.

---

## Trace ring

The movement history is an **always-on ring**, on the same contract as the
frame-event ring in `nes_runtime.h`: it records from power-on and is never
armed at probe time. Query a window of it; do not start a recording and then
try to reproduce the event.

| Access | How |
|---|---|
| Live query | TCP `ftring` (`{"n":256}`), newest-first-bounded, oldest-first order |
| Exit dump | `NESRECOMP_FTRING_DUMP=<path>` writes CSV at exit |
| In-process | `nes_foreign_trace_last(n, dst)` / `nes_foreign_trace_write_csv(path)` |

Each row carries frame, ownership, state (with the controller's own name),
buttons, stick x/y, position, velocity, requested delta, resolved delta,
grounded, fast-fall, collision flags, and the native coordinates that were
written back — so one row shows both what the controller intended and what the
game actually saw.

Compiled into Release. If an investigation needs a field that is not here,
**add the field**. Do not work around a missing field with a one-shot
attach-and-time probe.

---

## Per-tick integration

```c
void game_on_frame(uint64_t frame) {
    ForeignInput in = adapter_read_input();

    ForeignMoveResult move;
    if (!nes_foreign_tick(frame, &in, &move)) return;  /* host owns the player */

    ForeignCollisionResult hit;
    nes_foreign_sweep(/* ... the game's tiles ... */, &hit);
    nes_foreign_resolve(&hit);

    adapter_sync_to_game_ram();
    nes_foreign_trace_note_native(game_player_x(), game_player_y());
}
```

Tick exactly once per emulated game tick. No wall-clock delta time, no
render-frame-driven physics: a controller ported from a fixed-timestep game
must keep its per-frame model or it is no longer that game's movement.

Save states are a real hazard here, because the controller's authoritative
state lives outside NES RAM. Restoring RAM while leaving host state from a
different frame produces a character that is in two places at once. Serialize
the controller state alongside the save, or reconstruct only at safe
synchronization points and document the limitation.
