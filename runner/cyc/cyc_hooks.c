/* cyc_hooks.c - mod hook sites on the cycle backend; see cyc_hooks.h. */
#include "cyc_hooks.h"

#include "cyc_core.h"
#include "cyc_recomp.h"
#include "cyc_ring.h"
#include "hw_internal.h"
#include "../include/mod_function_hooks.h"
#include "../../common/nes_cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t cyc_hooks_armed;
int32_t cyc_hook_passed = -1;
bool    cyc_hook_hit;

#define MAX_BINDINGS 8          /* plugins bound to one site */

typedef struct {
    int      plugin[MAX_BINDINGS];   /* registry indices (mod_function_hooks.h) */
    int      plugins;
    uint32_t frame_fired, frame_handled;
    CycHookStats stats;
} Site;

static uint8_t  s_bits[0x10000 / 8];     /* an address some site is at */
static Site    *s_sites;
static bool     s_validated, s_suspended;
static CycHookStats s_total;

static void update_armed(void)
{
    cyc_hooks_armed = (uint8_t)(s_validated && !s_suspended && cyc_native_hook_site_count &&
                                nes_mod_function_hooks_active() > 0);
    if (!cyc_hooks_armed) cyc_hook_passed = -1;
}

bool cyc_hooks_validate(void)
{
    uint32_t n = cyc_native_hook_site_count;
    free(s_sites);
    s_sites = n ? (Site *)calloc(n, sizeof(Site)) : NULL;
    memset(s_bits, 0, sizeof(s_bits));
    memset(&s_total, 0, sizeof(s_total));
    for (uint32_t i = 0; i < n; ++i) s_bits[cyc_native_hook_sites[i].addr >> 3] |= (uint8_t)(1u << (cyc_native_hook_sites[i].addr & 7));
    bool ok = true;
    for (int r = 0; r < nes_mod_function_hook_count(); ++r) {
        const char *id = NULL;
        uint16_t addr = 0;
        nes_mod_function_hook_at(r, &id, &addr, NULL, NULL);
        int bound = 0;
        /* A site that names the plugin's id takes it; a site without an id
         * takes any plugin registered at its address. */
        for (uint32_t i = 0; i < n; ++i) {
            const CycHookSite *site = &cyc_native_hook_sites[i];
            bool named = site->id && id && !strcmp(site->id, id);
            if (!(named || (!site->id && site->addr == addr))) continue;
            if (site->addr != addr) {
                fprintf(stderr, "[cyc hooks] plugin %s registered at $%04X, but game.toml declares that site at $%04X\n",
                        id ? id : "?", addr, site->addr);
                ok = false;
                continue;
            }
            if (s_sites[i].plugins == MAX_BINDINGS) {
                fprintf(stderr, "[cyc hooks] more than %d plugins at site %s $%04X\n", MAX_BINDINGS,
                        site->id ? site->id : "", site->addr);
                ok = false;
                continue;
            }
            s_sites[i].plugin[s_sites[i].plugins++] = r;
            bound++;
        }
        if (!bound) {
            fprintf(stderr, "[cyc hooks] plugin %s registered at $%04X: this program declares no such "
                            "[[mod_function_hook]] site (game.toml)\n", id ? id : "?", addr);
            ok = false;
        }
    }
    s_validated = ok;
    nes_mod_function_hooks_set_listener(update_armed);
    update_armed();
    return ok;
}

bool cyc_hooks_due(uint16_t pc)
{
    return (s_bits[pc >> 3] >> (pc & 7)) & 1 && cyc_hook_passed != (int32_t)pc &&
           !(cpu.do_nmi | cpu.do_irq | cpu.do_reset);
}

static bool holds(const CycHookSite *site)
{
    uint8_t b[16];
    if (site->len > sizeof(b)) return false;
    for (unsigned k = 0; k < site->len; ++k)
        if (!cyc_debug_peek((uint16_t)(site->addr + k), &b[k])) return false;
    return !site->len || nes_crc32(0, b, site->len) == site->crc32;
}

/* A handled routine returns as its RTS would: pull the return address and go
 * on past the JSR (no bus cycles: the plugin replaced the routine). */
static void return_from_routine(void)
{
    uint8_t lo = hw.ram[0x100 | (uint8_t)(cpu.s + 1)];
    uint8_t hi = hw.ram[0x100 | (uint8_t)(cpu.s + 2)];
    cpu.s = (uint8_t)(cpu.s + 2);
    cpu.pc = (uint16_t)((lo | hi << 8) + 1);
    if (cyc_cpu_rts_observer) cyc_cpu_rts_observer();
}

void cyc_hooks_fire(uint16_t pc)
{
    for (uint32_t i = 0; i < cyc_native_hook_site_count; ++i) {
        const CycHookSite *site = &cyc_native_hook_sites[i];
        Site *s = &s_sites[i];
        if (site->addr != pc || !s->plugins) continue;
        if (!holds(site)) {
            s->stats.mismatched++;
            s_total.mismatched++;
            continue;
        }
        for (int b = 0; b < s->plugins; ++b) {
            int enabled = 0;
            NESModFunctionEntryCallback cb = NULL;
            nes_mod_function_hook_at(s->plugin[b], NULL, NULL, &enabled, &cb);
            if (!enabled || !cb) continue;
            s->stats.fired++;
            s_total.fired++;
            s->frame_fired++;
            if (cb(pc)) {
                s->stats.handled++;
                s_total.handled++;
                s->frame_handled++;
                return_from_routine();
                return;
            }
            /* A callback may leave the program elsewhere (a mod's own control
             * transfer); the rest do not run at a site no longer current. */
            if (cpu.pc != pc) return;
        }
    }
}

void cyc_hooks_suspend(bool suspended)
{
    s_suspended = suspended;
    update_armed();
}

void cyc_hooks_frame_end(void)
{
    for (uint32_t i = 0; s_sites && i < cyc_native_hook_site_count; ++i) {
        Site *s = &s_sites[i];
        if (!s->frame_fired) continue;
        uint32_t handled = s->frame_handled > 0xFFFF ? 0xFFFF : s->frame_handled;
        cyc_ring_push_len(CYC_EV_MOD_HOOK, cyc_native_hook_sites[i].addr, i | handled << 16, s->frame_fired);
        s->frame_fired = s->frame_handled = 0;
    }
}

void cyc_hooks_stats(int site, CycHookStats *out)
{
    if (site < 0) *out = s_total;
    else if (s_sites && (uint32_t)site < cyc_native_hook_site_count) *out = s_sites[site].stats;
    else memset(out, 0, sizeof(*out));
}
