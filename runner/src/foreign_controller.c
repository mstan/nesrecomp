/*
 * foreign_controller.c ??? registry, swept collision, and the always-on
 * movement trace ring for host-side imported movement controllers.
 *
 * See foreign_controller.h for the contract. Nothing here reads NES state:
 * the module is deliberately free of runtime.h so a controller written
 * against it cannot quietly grow a dependency on one game's memory map.
 */
#include "foreign_controller.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Registry                                                                */
/* ---------------------------------------------------------------------- */

#define FOREIGN_MAX_CONTROLLERS 16
#define FOREIGN_SERIALIZE_VERSION 1u

/* Payload layout (all multi-byte lengths are little-endian):
 *   u8 version, u8 id_length, u16 foreign_state_length, u16 private_length,
 *   u8 flags,
 *   id bytes (not NUL terminated), raw ForeignState, private bytes.
 *
 * The raw state is deliberately length-tagged. Save states are local native
 * snapshots rather than a cross-architecture interchange format; a build
 * whose ForeignState layout changed must reject rather than memcpy it. */
#define FOREIGN_SERIALIZE_HEADER_SIZE 7u
#define FOREIGN_SERIALIZE_PRIVATE_PRESENT 0x01u

static const ForeignController *s_controllers[FOREIGN_MAX_CONTROLLERS];
static int                      s_controller_count = 0;

typedef struct ForeignPrivateStateHooks {
    const char *controller_id;
    ForeignControllerPrivateStateGet get;
    ForeignControllerPrivateStateSet set;
} ForeignPrivateStateHooks;

static ForeignPrivateStateHooks s_private_states[FOREIGN_MAX_CONTROLLERS];
static int                      s_private_state_count = 0;

static const ForeignController *s_active = NULL;
static ForeignState             s_state;
static ForeignOwnership         s_ownership = FOREIGN_OWNERSHIP_NATIVE;

static int controller_index(const char *id) {
    int i;
    if (!id || !id[0]) return -1;
    for (i = 0; i < s_controller_count; i++) {
        if (strcmp(s_controllers[i]->id, id) == 0) return i;
    }
    return -1;
}

static const ForeignPrivateStateHooks *private_hooks_for_active(void) {
    int i;
    if (!s_active) return NULL;
    for (i = 0; i < s_private_state_count; i++) {
        if (strcmp(s_private_states[i].controller_id, s_active->id) == 0)
            return &s_private_states[i];
    }
    return NULL;
}

int nes_foreign_register(const ForeignController *controller) {
    if (!controller || !controller->id || !controller->id[0]) return 0;
    if (!controller->tick) return 0;
    for (int i = 0; i < s_controller_count; i++) {
        if (strcmp(s_controllers[i]->id, controller->id) != 0) continue;
        /* Idempotent re-registration of the same implementation is fine;
         * two different implementations claiming one id is not. */
        return s_controllers[i] == controller;
    }
    if (s_controller_count >= FOREIGN_MAX_CONTROLLERS) return 0;
    s_controllers[s_controller_count++] = controller;
    return 1;
}

int nes_foreign_register_private_state(const char *controller_id,
                                       ForeignControllerPrivateStateGet get,
                                       ForeignControllerPrivateStateSet set) {
    int i, index;
    index = controller_index(controller_id);
    if (index < 0 || !get || !set) return 0;
    for (i = 0; i < s_private_state_count; i++) {
        if (strcmp(s_private_states[i].controller_id, controller_id) != 0)
            continue;
        s_private_states[i].get = get;
        s_private_states[i].set = set;
        return 1;
    }
    if (s_private_state_count >= FOREIGN_MAX_CONTROLLERS) return 0;
    /* Keep the registered controller's stable id pointer, not a caller-owned
     * temporary spelling that merely compared equal during registration. */
    s_private_states[s_private_state_count].controller_id =
        s_controllers[index]->id;
    s_private_states[s_private_state_count].get = get;
    s_private_states[s_private_state_count].set = set;
    s_private_state_count++;
    return 1;
}

int nes_foreign_select(const char *id) {
    if (!id || !id[0]) {
        s_active = NULL;
        memset(&s_state, 0, sizeof(s_state));
        return 1;
    }
    for (int i = 0; i < s_controller_count; i++) {
        if (strcmp(s_controllers[i]->id, id) != 0) continue;
        s_active = s_controllers[i];
        memset(&s_state, 0, sizeof(s_state));
        s_state.facing = 1.0f;
        if (s_active->reset) s_active->reset(&s_state);
        return 1;
    }
    return 0;
}

const ForeignController *nes_foreign_active(void) { return s_active; }

ForeignState *nes_foreign_state(void) { return s_active ? &s_state : NULL; }

static void write_u16le(uint8_t *dst, unsigned value) {
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)((value >> 8) & 0xffu);
}

static unsigned read_u16le(const uint8_t *src) {
    return (unsigned)src[0] | ((unsigned)src[1] << 8);
}

int nes_foreign_serialize_active(uint8_t *buf, int capacity) {
    const ForeignPrivateStateHooks *private_hooks;
    size_t id_length;
    int private_length = 0;
    size_t header_and_id;
    size_t total;

    if (!s_active) return 0;
    if (!buf || capacity < 0) return -1;
    id_length = strlen(s_active->id);
    if (id_length == 0 || id_length > 255u || sizeof(ForeignState) > 65535u)
        return -1;
    header_and_id = FOREIGN_SERIALIZE_HEADER_SIZE + id_length;
    if (header_and_id > (size_t)capacity ||
        sizeof(ForeignState) > (size_t)capacity - header_and_id)
        return -1;

    private_hooks = private_hooks_for_active();
    if (private_hooks) {
        const size_t private_capacity =
            (size_t)capacity - header_and_id - sizeof(ForeignState);
        private_length = private_hooks->get(
            &s_state, buf + header_and_id + sizeof(ForeignState),
            private_capacity > (size_t)INT_MAX ? INT_MAX : (int)private_capacity);
        if (private_length < 0 || (size_t)private_length > private_capacity ||
            private_length > 65535)
            return -1;
    }

    total = header_and_id + sizeof(ForeignState) + (size_t)private_length;
    buf[0] = FOREIGN_SERIALIZE_VERSION;
    buf[1] = (uint8_t)id_length;
    write_u16le(buf + 2, (unsigned)sizeof(ForeignState));
    write_u16le(buf + 4, (unsigned)private_length);
    buf[6] = private_hooks ? FOREIGN_SERIALIZE_PRIVATE_PRESENT : 0u;
    memcpy(buf + FOREIGN_SERIALIZE_HEADER_SIZE, s_active->id, id_length);
    memcpy(buf + header_and_id, &s_state, sizeof(s_state));
    return (int)total;
}

int nes_foreign_deserialize_active(const uint8_t *buf, int length) {
    const ForeignPrivateStateHooks *private_hooks;
    unsigned id_length, state_length, private_length, flags;
    size_t header_and_id, total;
    ForeignState restored;

    if (!s_active || !buf || length < (int)FOREIGN_SERIALIZE_HEADER_SIZE)
        return 0;
    if (buf[0] != FOREIGN_SERIALIZE_VERSION) return 0;
    id_length = buf[1];
    state_length = read_u16le(buf + 2);
    private_length = read_u16le(buf + 4);
    flags = buf[6];
    if (id_length == 0 || state_length != sizeof(ForeignState) ||
        (flags & ~FOREIGN_SERIALIZE_PRIVATE_PRESENT) != 0u)
        return 0;
    header_and_id = FOREIGN_SERIALIZE_HEADER_SIZE + (size_t)id_length;
    if (header_and_id > (size_t)length ||
        (size_t)state_length > (size_t)length - header_and_id ||
        (size_t)private_length > (size_t)length - header_and_id -
                                  (size_t)state_length)
        return 0;
    total = header_and_id + (size_t)state_length + (size_t)private_length;
    if (total != (size_t)length) return 0;
    if (strlen(s_active->id) != id_length ||
        memcmp(buf + FOREIGN_SERIALIZE_HEADER_SIZE, s_active->id, id_length) != 0)
        return 0;
    private_hooks = private_hooks_for_active();
    if (((flags & FOREIGN_SERIALIZE_PRIVATE_PRESENT) != 0u) !=
        (private_hooks != NULL))
        return 0;

    memcpy(&restored, buf + header_and_id, sizeof(restored));
    if (private_hooks &&
        !private_hooks->set(&restored,
                            buf + header_and_id + state_length,
                            (int)private_length))
        return 0;
    s_state = restored;
    return 1;
}

void nes_foreign_set_ownership(ForeignOwnership ownership) {
    s_ownership = ownership;
}

ForeignOwnership nes_foreign_ownership(void) { return s_ownership; }

/* ---------------------------------------------------------------------- */
/* Swept collision                                                         */
/* ---------------------------------------------------------------------- */

void nes_foreign_sweep(double x, double y,
                       double dx, double dy,
                       double max_step,
                       ForeignSweepProbe probe, void *user,
                       ForeignCollisionResult *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!probe) {
        out->actual_dx = dx;
        out->actual_dy = dy;
        return;
    }
    if (!(max_step > 0.0)) max_step = 1.0;

    const double distance = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
    int steps = (int)ceil(distance / max_step);
    if (steps < 1) steps = 1;
    /* Bound the substep count so a wild velocity cannot stall a frame; the
     * step then exceeds max_step, which the caller can detect from the
     * residual it did not get. */
    if (steps > 4096) steps = 4096;

    const double step_x = dx / (double)steps;
    const double step_y = dy / (double)steps;

    double cur_x = x;
    double cur_y = y;
    int blocked_x = 0;
    int blocked_y = 0;

    for (int i = 0; i < steps && !(blocked_x && blocked_y); i++) {
        /* Axis-separated so sliding along a wall or ceiling keeps the other
         * axis moving, which is what every 2D platformer expects. */
        if (!blocked_x && step_x != 0.0) {
            if (probe(cur_x + step_x, cur_y, user)) blocked_x = 1;
            else cur_x += step_x;
        }
        if (!blocked_y && step_y != 0.0) {
            if (probe(cur_x, cur_y + step_y, user)) blocked_y = 1;
            else cur_y += step_y;
        }
    }

    out->actual_dx = cur_x - x;
    out->actual_dy = cur_y - y;
    out->hit_wall  = blocked_x;
    if (blocked_y) {
        /* Screen-space convention: +y is down, matching NES coordinates. */
        if (dy > 0.0) { out->hit_floor = 1; out->grounded = 1; }
        else if (dy < 0.0) { out->hit_ceiling = 1; }
    }
}

/* ---------------------------------------------------------------------- */
/* Always-on trace ring                                                    */
/* ---------------------------------------------------------------------- */

/* ~16k ticks is ~4.5 minutes of 60Hz play retained at ~1.8MB. Eviction keeps
 * it bounded; targeted dumps pull the requested slice. */
#define FTRING_N 16384

static ForeignTraceEntry s_ftring[FTRING_N];
static uint32_t          s_ftring_head = 0;

void nes_foreign_trace_push(const ForeignTraceEntry *entry) {
    if (!entry) return;
    s_ftring[s_ftring_head % FTRING_N] = *entry;
    s_ftring_head++;
}

void nes_foreign_trace_note_native(int32_t native_x, int32_t native_y) {
    if (s_ftring_head == 0) return;
    ForeignTraceEntry *e = &s_ftring[(s_ftring_head - 1) % FTRING_N];
    e->native_x = native_x;
    e->native_y = native_y;
}

void nes_foreign_trace_note_reseed(void) {
    if (s_ftring_head == 0) return;
    s_ftring[(s_ftring_head - 1) % FTRING_N].reseeded = 1;
}

void nes_foreign_trace_note_flags(uint32_t flags) {
    if (s_ftring_head == 0) return;
    s_ftring[(s_ftring_head - 1) % FTRING_N].collision_flags |= flags;
}

int nes_foreign_trace_count(void) {
    return (int)(s_ftring_head < FTRING_N ? s_ftring_head : FTRING_N);
}

int nes_foreign_trace_last(int n, ForeignTraceEntry *dst) {
    if (!dst) return 0;
    uint32_t avail = s_ftring_head < FTRING_N ? s_ftring_head : FTRING_N;
    uint32_t take  = (n > 0 && (uint32_t)n < avail) ? (uint32_t)n : avail;
    for (uint32_t i = 0; i < take; i++)
        dst[i] = s_ftring[(s_ftring_head - take + i) % FTRING_N];
    return (int)take;
}

int nes_foreign_trace_write_csv(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "frame,ownership,state,state_name,buttons,stick_x,stick_y,"
               "x,y,vx,vy,req_dx,req_dy,res_dx,res_dy,grounded,fast_fall,"
               "air_cause,jump_phase,reseeded,hit_wall,hit_ceiling,hit_floor,"
               "attack_connected,action_events,action_feedback,imposed,"
               "imposed_vy,collision_flags,"
               "native_x,native_y\n");
    const uint32_t n = s_ftring_head < FTRING_N ? s_ftring_head : FTRING_N;
    for (uint32_t i = 0; i < n; i++) {
        const ForeignTraceEntry *e = &s_ftring[(s_ftring_head - n + i) % FTRING_N];
        const char *name = "";
        if (s_active && s_active->state_name) {
            const char *resolved = s_active->state_name(e->state);
            if (resolved) name = resolved;
        }
        fprintf(f,
                "%llu,%u,%d,%s,0x%02X,%.4f,%.4f,"
                "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%u,"
                "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.6f,0x%08X,%d,%d\n",
                (unsigned long long)e->frame, e->ownership, e->state, name,
                (unsigned)(e->raw_buttons & 0xFF), e->stick_x, e->stick_y,
                e->x, e->y, e->vx, e->vy,
                e->requested_dx, e->requested_dy,
                e->resolved_dx, e->resolved_dy,
                e->grounded, e->fast_fall,
                e->air_cause, e->jump_phase, e->reseeded,
                e->hit_wall, e->hit_ceiling, e->hit_floor,
                e->attack_connected, e->action_event_count,
                e->action_feedback_count, e->has_imposed_vy, e->imposed_vy,
                e->collision_flags,
                e->native_x, e->native_y);
    }
    fclose(f);
    return 1;
}

static const char *s_ftring_dump_path = NULL;

static void ftring_dump_atexit(void) {
    /* Only spend the write when something was actually recorded. */
    if (s_ftring_head == 0) return;
    (void)nes_foreign_trace_write_csv(s_ftring_dump_path);
}

void nes_foreign_trace_init_dump(void) {
    const char *e = getenv("NESRECOMP_FTRING_DUMP");
    if (e && *e) { s_ftring_dump_path = e; atexit(ftring_dump_atexit); }
}

/* ---------------------------------------------------------------------- */
/* Per-tick driver                                                         */
/* ---------------------------------------------------------------------- */

int nes_foreign_tick(uint64_t frame, const ForeignInput *input,
                     ForeignMoveResult *out) {
    ForeignInput zero;
    if (!input) { memset(&zero, 0, sizeof(zero)); input = &zero; }

    ForeignMoveResult local;
    memset(&local, 0, sizeof(local));

    const int driving = (s_active != NULL &&
                         s_ownership == FOREIGN_OWNERSHIP_FOREIGN);
    if (driving) {
        local.state = s_state.state;
        s_active->tick(&s_state, input, &local);
        if (local.actions.count > FOREIGN_ACTION_EVENT_CAPACITY)
            local.actions.count = FOREIGN_ACTION_EVENT_CAPACITY;
        s_state.state = local.state;
        if (out) *out = local;
    } else {
        /*
         * The controller is not ticking, so it cannot retract a jump phase it
         * published earlier. Clear it here rather than leaving the host to
         * defer to a jumpsquat that will never end: ownership can drop to
         * SCRIPTED mid-squat (a pipe, a death, a powerup) and a stale CHARGING
         * would go on suppressing the host's jump button indefinitely.
         */
        s_state.jump_phase = FOREIGN_JUMP_NONE;
    }

    /* A row is recorded every tick, including while the host game owns the
     * player, so the ring shows handoffs instead of an unexplained gap. */
    ForeignTraceEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.frame        = frame;
    entry.ownership    = (uint8_t)s_ownership;
    entry.raw_buttons  = input->raw_buttons;
    entry.stick_x      = input->stick_x;
    entry.stick_y      = input->stick_y;
    entry.state        = s_state.state;
    entry.grounded     = (uint8_t)(s_state.grounded ? 1 : 0);
    entry.air_cause    = (uint8_t)s_state.air_cause;
    /* Post-tick, so a LAUNCH row is the row the controller left the ground on
     * -- which is the row the host fired its trigger on. */
    entry.jump_phase   = (uint8_t)s_state.jump_phase;
    entry.fast_fall    = (uint8_t)(s_state.fast_fall ? 1 : 0);
    entry.x            = s_state.x;
    entry.y            = s_state.y;
    entry.vx           = s_state.vx;
    entry.vy           = s_state.vy;
    entry.requested_dx = local.requested_dx;
    entry.requested_dy = local.requested_dy;
    entry.action_event_count = (uint8_t)local.actions.count;
    nes_foreign_trace_push(&entry);

    return driving;
}

void nes_foreign_resolve(const ForeignCollisionResult *hit) {
    ForeignCollisionResult bounded;
    if (!hit) return;
    bounded = *hit;
    if (bounded.action_feedback.count > FOREIGN_ACTION_FEEDBACK_CAPACITY)
        bounded.action_feedback.count = FOREIGN_ACTION_FEEDBACK_CAPACITY;
    if (s_active && s_active->resolve) s_active->resolve(&s_state, &bounded);

    if (s_ftring_head == 0) return;
    ForeignTraceEntry *e = &s_ftring[(s_ftring_head - 1) % FTRING_N];
    e->resolved_dx     = bounded.actual_dx;
    e->resolved_dy     = bounded.actual_dy;
    e->has_imposed_vy  = (uint8_t)(bounded.has_imposed_vy ? 1 : 0);
    e->imposed_vy      = bounded.has_imposed_vy ? bounded.imposed_vy : 0.0;
    e->collision_flags = bounded.flags;
    e->grounded        = (uint8_t)(bounded.grounded ? 1 : 0);
    e->hit_wall        = (uint8_t)(bounded.hit_wall ? 1 : 0);
    e->hit_ceiling     = (uint8_t)(bounded.hit_ceiling ? 1 : 0);
    e->hit_floor       = (uint8_t)(bounded.hit_floor ? 1 : 0);
    e->attack_connected = (uint8_t)(bounded.attack_connected ? 1 : 0);
    e->action_feedback_count = (uint8_t)bounded.action_feedback.count;
    /* Post-resolve position, so one row is self-consistent. */
    e->x  = s_state.x;
    e->y  = s_state.y;
    e->vx = s_state.vx;
    e->vy = s_state.vy;
}
