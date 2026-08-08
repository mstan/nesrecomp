/*
 * foreign_controller.h — host-side movement controllers imported from
 * another game.
 *
 * A "foreign controller" answers one question: HOW DOES THE PLAYER CHARACTER
 * WANT TO MOVE.  It never answers WHERE THE PLAYER IS ALLOWED TO GO — that
 * stays with the host game, whose tiles, collision and scripted sequences
 * remain authoritative.  Keeping those two halves apart is the whole point of
 * this module: a controller ported from one game becomes reusable across NES
 * titles instead of fusing with one game's RAM map.
 *
 * A controller therefore must NOT know about:
 *   NES RAM addresses, metatiles, PPU state, OAM, enemy slots,
 *   or the source game's stage geometry, renderer and object allocator.
 *
 * The host keeps the authoritative high-precision state during foreign
 * control and projects it into the game's own coordinate representation at
 * the adapter boundary, via constants the adapter names explicitly.  Do not
 * quantize acceleration, friction or air drift to the game's integer
 * coordinates every frame; accumulate host-side and project once.
 *
 * Determinism: tick exactly once per emulated game tick.  No wall-clock delta
 * time, no render-frame-driven physics.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- input -- */

/*
 * Analog-shaped input, because foreign controllers generally distinguish
 * behavior by stick magnitude even when the host console's pad is digital.
 * A digital host builds synthetic magnitudes in its adapter rather than
 * hardcoding every input to 1.0 forever.
 */
typedef struct {
    float stick_x;      /* -1.0 .. +1.0, positive right */
    float stick_y;      /* -1.0 .. +1.0, positive up */
    int   jump_pressed; /* rising edge this tick */
    int   jump_held;
    int   down_pressed; /* rising edge this tick */
    int   attack_pressed;
    int   raw_buttons;  /* host pad bits, for the trace ring only */
} ForeignInput;

/* ---------------------------------------------------------------- state -- */

/* Who is driving the player this tick. */
typedef enum {
    FOREIGN_OWNERSHIP_NATIVE = 0,   /* the host game's own movement code */
    FOREIGN_OWNERSHIP_FOREIGN = 1,  /* the registered foreign controller */
    FOREIGN_OWNERSHIP_SCRIPTED = 2, /* a host cutscene/pipe/death sequence */
} ForeignOwnership;

/*
 * Controller-defined locomotion state.  The engine treats it as an opaque
 * integer and only records it; each controller publishes its own enum and a
 * name table so traces stay readable.
 */
typedef int ForeignMoveState;

/*
 * Why the character is off the ground, as host truth.
 *
 * A host that keeps its own jump trigger and its own ledge detection -- which
 * is the normal case, because those decisions are tangled up with the host's
 * level scripting -- can only report `grounded = 0`. That is ambiguous: a
 * launched jump wants an upward impulse and a walked-off ledge must not get
 * one, and a controller cannot tell them apart from `grounded` alone. Guessing
 * from the input is wrong too, since the host acts on the pad a frame after the
 * controller sees it.
 *
 * So the host says which. The controller reads this when it notices `grounded`
 * has gone to 0 without its own state machine having initiated the departure.
 */
typedef enum {
    FOREIGN_AIR_NONE = 0,     /* grounded, or the controller's own departure */
    FOREIGN_AIR_LAUNCHED = 1, /* host started a jump: apply jump velocity */
    FOREIGN_AIR_FELL = 2,     /* host reports airborne with no impulse */
} ForeignAirCause;

/*
 * Jumpsquat handshake -- the one place the controller takes "when" back.
 *
 * air_cause above covers the normal division of labour: the host keeps its jump
 * trigger and the controller supplies the physics. But a fighter with a
 * jumpsquat (Smash's KneeBend, and most fighting games) decides jump HEIGHT
 * from what the button does DURING the squat -- released early is a short hop,
 * held is a full hop. A host that launches on the button's rising edge gives
 * that window no room to exist, so the fighter can only ever full-hop.
 *
 * So the controller announces the window and the host defers to it:
 *
 *   CHARGING  the controller is in jumpsquat. The host must WITHHOLD its own
 *             jump trigger -- suppress the button, do not cancel the input.
 *   LAUNCH    the controller is leaving the ground on THIS tick. The host must
 *             fire its jump now, presenting the button even if the player has
 *             already let go (a short hop is exactly that case).
 *   NONE      no jump being prepared; the host's trigger behaves normally.
 *
 * The host still owns HOW: which byte carries the button, when in its frame it
 * is safe to write, and which of its own routines must not see the change.
 * Scope that write in time via a function hook, as with any other guest byte.
 *
 * A host that ignores this field keeps M3 behaviour -- full hops only -- so it
 * is additive for existing adapters.
 */
typedef enum {
    FOREIGN_JUMP_NONE = 0,
    FOREIGN_JUMP_CHARGING = 1,
    FOREIGN_JUMP_LAUNCH = 2,
} ForeignJumpPhase;

typedef struct {
    ForeignMoveState state;
    unsigned         state_frame;  /* ticks spent in `state` */

    double x;      /* high-precision host world coordinates */
    double y;
    double vx;
    double vy;

    float facing;  /* -1.0 left, +1.0 right */

    int grounded;
    int fast_fall;

    /* Host-written, controller-read. See ForeignAirCause. */
    ForeignAirCause air_cause;

    /* Controller-written, host-read. See ForeignJumpPhase. Publish it every
     * tick, including the ticks it is NONE, so the host never acts on a stale
     * LAUNCH. */
    ForeignJumpPhase jump_phase;
} ForeignState;

/* What the controller wants to happen this tick, before host collision. */
typedef struct {
    double           requested_dx;
    double           requested_dy;
    double           vx;
    double           vy;
    ForeignMoveState state;
} ForeignMoveResult;

/* What the host's collision actually permitted. Fed straight back in. */
typedef struct {
    double   actual_dx;
    double   actual_dy;
    int      grounded;
    int      hit_ceiling;
    int      hit_floor;
    int      hit_wall;

    /*
     * The host IMPOSED a vertical velocity of its own this tick.
     *
     * Blocking a move is not the only thing a host game does to a character's
     * vertical motion. It also launches: a stomp bounce off an enemy, a spring,
     * a shattered block, a ceiling that kills the jump. Host games typically
     * signal all of these by writing their own vertical-velocity variable and
     * expecting their own integrator to pick it up next frame.
     *
     * A controller that has taken over vertical integration never reads that
     * variable back, so every one of those events is discarded SILENTLY -- the
     * character sails through the ceiling and does not bounce off anything.
     * Position readback cannot substitute: a host killing a jump usually only
     * changes the velocity, leaving the position exactly where the controller
     * put it, so a position diff sees nothing at all.
     *
     * `imposed_vy` is in the controller's own units and sign convention. The
     * adapter converts from the host's representation, as with every other
     * quantity crossing this boundary.
     */
    int      has_imposed_vy;
    double   imposed_vy;

    uint32_t flags;      /* host-defined, recorded in the trace ring */
} ForeignCollisionResult;

/* ----------------------------------------------------------- controller -- */

typedef struct ForeignController {
    /* Stable id, matching the trusted-plugin id that selects this character. */
    const char *id;
    /* Human-readable, for logs and the trace ring header. */
    const char *name;

    /* Reset to power-on/respawn state. Must not allocate. */
    void (*reset)(ForeignState *state);

    /* Advance one tick and report the desired motion. Must be deterministic
     * and must not read host game memory. */
    void (*tick)(ForeignState *state, const ForeignInput *input,
                 ForeignMoveResult *out);

    /* Reconcile with what the host's collision permitted. */
    void (*resolve)(ForeignState *state, const ForeignCollisionResult *hit);

    /* Optional: name a ForeignMoveState for traces. May be NULL. */
    const char *(*state_name)(ForeignMoveState state);
} ForeignController;

/*
 * Register a controller. Controllers are statically linked and registered
 * before main(); mod archives select one by id and never supply native code.
 * Returns 1 on success, 0 if the id is empty or already registered with a
 * different implementation.
 */
int nes_foreign_register(const ForeignController *controller);

/* Select the active controller by id, or NULL to deactivate. Returns 1 when
 * the id resolved. Selecting a controller also resets its state. */
int nes_foreign_select(const char *id);

/* The active controller, or NULL when none is selected. */
const ForeignController *nes_foreign_active(void);

/* Mutable state of the active controller, or NULL when none is selected. */
ForeignState *nes_foreign_state(void);

/* Ownership is host policy: the adapter raises it for ordinary controllable
 * play and lowers it for scripted sequences. Defaults to NATIVE. */
void             nes_foreign_set_ownership(ForeignOwnership ownership);
ForeignOwnership nes_foreign_ownership(void);

/* ------------------------------------------------------ swept collision -- */

/*
 * Substep a requested motion so a controller moving many pixels per tick
 * cannot tunnel through geometry a host game only ever tested at its own
 * native speed.
 *
 * `probe` receives a candidate absolute position and returns nonzero when it
 * is blocked. The helper walks the request in steps no longer than
 * `max_step`, stopping at the last unblocked position, and reports which
 * axes were stopped. The host's tiles remain the only source of truth; this
 * only controls how finely they are asked.
 */
typedef int (*ForeignSweepProbe)(double x, double y, void *user);

void nes_foreign_sweep(double x, double y,
                       double dx, double dy,
                       double max_step,
                       ForeignSweepProbe probe, void *user,
                       ForeignCollisionResult *out);

/* ------------------------------------------------------------ trace ring -- */

/*
 * ALWAYS-ON movement history, compiled into Release builds, on the same
 * contract as the frame-event ring in nes_runtime.h: capture begins at
 * power-on and never has to be armed. Probes QUERY a window of this history
 * (TCP `ftring`, or an exit dump via NESRECOMP_FTRING_DUMP=<path>); they must
 * never arm a recording and then try to reproduce the event.
 *
 * If an investigation needs a field that is not here, ADD THE FIELD — do not
 * work around it with a one-shot attach-and-time probe.
 */
typedef struct {
    uint64_t frame;
    int32_t  raw_buttons;
    float    stick_x;
    float    stick_y;

    int32_t  state;
    uint8_t  ownership;
    uint8_t  grounded;
    uint8_t  fast_fall;

    /*
     * What the host's collision actually said, straight from the
     * ForeignCollisionResult. Without these a row can show a controller
     * insisting it is rising while the host pins it against a ceiling, and the
     * ring looks like it agrees with the controller. Measured on SMB1's 1-1
     * brick row before the adapter fed the refusal back.
     */
    uint8_t  hit_wall;
    uint8_t  hit_ceiling;
    uint8_t  hit_floor;
    uint8_t  air_cause;   /* ForeignAirCause at tick time */
    /*
     * ForeignJumpPhase after the tick. A jumpsquat is invisible in `state`
     * alone once the host is deferring to it -- CHARGING rows and a plain
     * grounded row look identical -- and the whole question a short-hop
     * investigation asks is "did the window open, and how long was it".
     */
    uint8_t  jump_phase;
    uint8_t  pad[1];

    double   x;
    double   y;
    double   vx;
    double   vy;

    double   requested_dx;
    double   requested_dy;
    double   resolved_dx;
    double   resolved_dy;

    /* Host-imposed vertical velocity, 0 when the host imposed none. A bounce
     * that fails to happen is invisible in every other column: the controller's
     * own vy simply carries on as though nothing occurred. */
    double   imposed_vy;

    uint32_t collision_flags;
    int32_t  native_x;   /* host game's own coordinates, post-sync */
    int32_t  native_y;
} ForeignTraceEntry;

/* Append one tick. Called by nes_foreign_tick(); a host that drives a
 * controller by hand may call it directly. */
void nes_foreign_trace_push(const ForeignTraceEntry *entry);

/* Record the host coordinates written back for this tick, so a trace row
 * carries both the controller's intent and what the game actually saw. */
void nes_foreign_trace_note_native(int32_t native_x, int32_t native_y);

/* Copy the newest `n` entries, oldest-first. Pass n <= 0 for everything
 * retained. Returns the number copied. */
int  nes_foreign_trace_last(int n, ForeignTraceEntry *dst);

/* Entries retained (bounded by the ring capacity). */
int  nes_foreign_trace_count(void);

/* Arm NESRECOMP_FTRING_DUMP=<path>. Called once during runtime init. */
void nes_foreign_trace_init_dump(void);

/* Write the retained history as CSV. Returns 1 on success. */
int  nes_foreign_trace_write_csv(const char *path);

/* ------------------------------------------------------------- per tick -- */

/*
 * Drive the active controller for one tick and record it.
 *
 * `frame` is the host's frame counter, used only for the trace. When
 * ownership is not FOREIGN the controller is not ticked, but a row is still
 * recorded so the ring shows the handoff rather than a silent gap.
 *
 * Returns 1 when `out` was filled by the controller, 0 otherwise.
 */
int nes_foreign_tick(uint64_t frame, const ForeignInput *input,
                     ForeignMoveResult *out);

/* Feed the host's collision outcome back into the active controller and
 * update the row already recorded for this tick. */
void nes_foreign_resolve(const ForeignCollisionResult *hit);

#ifdef __cplusplus
}
#endif
