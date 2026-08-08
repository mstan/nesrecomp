/*
 * foreign_controller.c ??? registry, swept collision, and the always-on
 * movement trace ring for host-side imported movement controllers.
 *
 * See foreign_controller.h for the contract. Nothing here reads NES state:
 * the module is deliberately free of runtime.h so a controller written
 * against it cannot quietly grow a dependency on one game's memory map.
 */
#include "foreign_controller.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Registry                                                                */
/* ---------------------------------------------------------------------- */

#define FOREIGN_MAX_CONTROLLERS 16

static const ForeignController *s_controllers[FOREIGN_MAX_CONTROLLERS];
static int                      s_controller_count = 0;

static const ForeignController *s_active = NULL;
static ForeignState             s_state;
static ForeignOwnership         s_ownership = FOREIGN_OWNERSHIP_NATIVE;

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
               "air_cause,jump_phase,hit_wall,hit_ceiling,hit_floor,"
               "imposed,imposed_vy,collision_flags,native_x,native_y\n");
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
                "%u,%u,%u,%u,%u,%u,%.6f,0x%08X,%d,%d\n",
                (unsigned long long)e->frame, e->ownership, e->state, name,
                (unsigned)(e->raw_buttons & 0xFF), e->stick_x, e->stick_y,
                e->x, e->y, e->vx, e->vy,
                e->requested_dx, e->requested_dy,
                e->resolved_dx, e->resolved_dy,
                e->grounded, e->fast_fall,
                e->air_cause, e->jump_phase,
                e->hit_wall, e->hit_ceiling, e->hit_floor,
                e->has_imposed_vy, e->imposed_vy, e->collision_flags,
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
    nes_foreign_trace_push(&entry);

    return driving;
}

void nes_foreign_resolve(const ForeignCollisionResult *hit) {
    if (!hit) return;
    if (s_active && s_active->resolve) s_active->resolve(&s_state, hit);

    if (s_ftring_head == 0) return;
    ForeignTraceEntry *e = &s_ftring[(s_ftring_head - 1) % FTRING_N];
    e->resolved_dx     = hit->actual_dx;
    e->resolved_dy     = hit->actual_dy;
    e->has_imposed_vy  = (uint8_t)(hit->has_imposed_vy ? 1 : 0);
    e->imposed_vy      = hit->has_imposed_vy ? hit->imposed_vy : 0.0;
    e->collision_flags = hit->flags;
    e->grounded        = (uint8_t)(hit->grounded ? 1 : 0);
    e->hit_wall        = (uint8_t)(hit->hit_wall ? 1 : 0);
    e->hit_ceiling     = (uint8_t)(hit->hit_ceiling ? 1 : 0);
    e->hit_floor       = (uint8_t)(hit->hit_floor ? 1 : 0);
    /* Post-resolve position, so one row is self-consistent. */
    e->x  = s_state.x;
    e->y  = s_state.y;
    e->vx = s_state.vx;
    e->vy = s_state.vy;
}
