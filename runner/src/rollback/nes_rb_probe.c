/*
 * nes_rb_probe.c -- NES_RB_PROBE: the rollback determinism probe.
 *
 * Save the snapshot at tick K, run N ticks recording the digest before each,
 * load K, run the SAME N ticks again (the same input rows forced at the same
 * point), and compare. It uses exactly the machinery a rollback episode uses:
 * nes_rb_state_save / _load at the top of an outer frame callback, the replay
 * of the load tick in the rest of that callback, the discarded native stack
 * and the guest-resume continuation. A divergence names the first tick and
 * the first differing image byte by field ("ram+0x01a3", "mod[smb.coop]+...").
 *
 * Symmetry: right after the load, the machine is re-serialized and compared
 * byte for byte with the saved image -- a load that does not reproduce what
 * was saved (a field restored differently, a record that does not round
 * trip) is reported before any tick runs.
 *
 *   NES_RB_PROBE=<start>:<span>:<count>[:<gap>]   probe windows
 *   NES_RB_PROBE=digest                            digest + snapshot every
 *                                                  tick, never load: the
 *                                                  "digest must not perturb
 *                                                  the guest" check
 *
 * Log lines (parsed by tools/rb_probe.sh):
 *   RB_PROBE #p tick=K span=N sym=ok|FAIL result=OK|DIVERGED ...
 *   RB_PROBE_SUMMARY probes=P ok=O diverged=D symmetry_fail=S
 *
 * Offline only: refuses to arm while a netplay session is active.
 */
#include "nes_rb_probe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes_rb_state.h"
#include "nes_runtime.h"
#include "logical_input.h"

void nes_rb_log_bridge(void);   /* main_runner.c */

#define PROBE_MAX_SPAN 240

enum { P_OFF = 0, P_IDLE, P_RUN1, P_RUN2, P_DIGEST_ONLY, P_MUTATION };

static struct {
    int mode;
    uint32_t start, span, count, gap;
    uint32_t tick;          /* outer callbacks seen */
    uint32_t next_start;
    uint32_t done;
    uint32_t i;             /* ticks run in the current pass */
    uint8_t *saved;         /* image at K */
    size_t saved_len;
    uint8_t *img1[PROBE_MAX_SPAN + 1];  /* run-1 image before tick K+i */
    size_t   len1[PROBE_MAX_SPAN + 1];
    uint8_t  rows[PROBE_MAX_SPAN][4];
    int      sym_ok;
    long     first_div;
    char     div_what[96];
    int      div_part;
    uint32_t ok, diverged, sym_fail;
    int      replaying;
} P;

static void probe_summary(void)
{
    if (P.mode == P_OFF) return;
    if (P.mode == P_DIGEST_ONLY)
        fprintf(stderr, "RB_PROBE_SUMMARY mode=digest ticks=%u\n", (unsigned)P.tick);
    else
        fprintf(stderr, "RB_PROBE_SUMMARY probes=%u ok=%u diverged=%u symmetry_fail=%u\n",
                (unsigned)P.done, (unsigned)P.ok, (unsigned)P.diverged,
                (unsigned)P.sym_fail);
    nes_rb_state_log_timing("probe");
    nes_rb_log_bridge();
}

void nes_rb_probe_init(void)
{
    const char *e = getenv("NES_RB_PROBE");
    memset(&P, 0, sizeof(P));
    if (!e || !e[0]) return;
    if (!strcmp(e, "digest")) {
        P.mode = P_DIGEST_ONLY;
    } else if (!strncmp(e, "mutation", 8)) {
        /* NES_RB_PROBE=mutation[:tick]: flip one byte in each partition's
         * state, prove the digest names that partition and nothing else,
         * restore it, prove the digest returns. */
        P.mode = P_MUTATION;
        P.start = (e[8] == ':') ? (uint32_t)atoi(e + 9) : 200u;
    } else {
        unsigned a = 0, b = 0, c = 0, d = 0;
        int n = sscanf(e, "%u:%u:%u:%u", &a, &b, &c, &d);
        if (n < 3 || b == 0 || b > PROBE_MAX_SPAN || c == 0) {
            fprintf(stderr, "RB_PROBE: bad NES_RB_PROBE='%s' "
                            "(want start:span:count[:gap], span 1..%d)\n",
                    e, PROBE_MAX_SPAN);
            return;
        }
        P.mode = P_IDLE;
        P.start = a; P.span = b; P.count = c; P.gap = (n >= 4) ? d : 15;
        P.next_start = a;
    }
    fprintf(stderr, "RB_PROBE armed: %s\n", e);
    atexit(probe_summary);
}

int nes_rb_probe_active(void) { return P.mode != P_OFF; }
int nes_rb_probe_replaying(void) { return P.replaying; }

static uint8_t *dup_image(size_t *len)
{
    size_t n = 0;
    const uint8_t *img = nes_rb_state_image(&n);
    uint8_t *c;
    if (!img) return NULL;
    c = (uint8_t *)malloc(n);
    if (c) memcpy(c, img, n);
    if (len) *len = n;
    return c;
}

static void free_run(void)
{
    uint32_t i;
    for (i = 0; i <= PROBE_MAX_SPAN; ++i) { free(P.img1[i]); P.img1[i] = NULL; }
    free(P.saved);
    P.saved = NULL;
}

/* Top of an outer frame callback: the state BEFORE this callback's tick. */
void nes_rb_probe_top(void)
{
    if (P.mode == P_OFF) return;
    P.tick++;
    if (P.mode == P_MUTATION) {
        if (P.tick == P.start) {
            struct { const char *name; uint8_t *p; int part; } m[] = {
                { "g_ram[0x0100]", &g_ram[0x100], 0 }, { "g_cpu.A", &g_cpu.A, 0 },
                { "g_sram[3]", &g_sram[3], 0 }, { "g_ppu_oam[5]", &g_ppu_oam[5], 1 },
                { "g_ppu_nt[10]", &g_ppu_nt[10], 1 }, { "g_ppu_pal[4]", &g_ppu_pal[4], 1 },
                { "g_controller2_buttons", &g_controller2_buttons, 2 },
                { "g_logical_input[3]", &g_logical_input[3], 2 },
            };
            int ok = 0, n = (int)(sizeof m / sizeof m[0]), i, k;
            NesRbDigest base, mut, back;
            nes_rb_state_invalidate();
            nes_rb_state_digest(&base);
            for (i = 0; i < n; ++i) {
                uint8_t keep = *m[i].p;
                int good = 1;
                *m[i].p ^= 0x5a;
                nes_rb_state_invalidate();
                nes_rb_state_digest(&mut);
                *m[i].p = keep;
                nes_rb_state_invalidate();
                nes_rb_state_digest(&back);
                if (mut.master == base.master) good = 0;
                for (k = 0; k < 3; ++k)
                    if ((mut.part[k] != base.part[k]) != (k == m[i].part)) good = 0;
                if (back.master != base.master) good = 0;
                ok += good;
                fprintf(stderr, "RB_MUTATION %-22s part=%s master %08x->%08x restored=%s %s\n",
                        m[i].name, nes_rb_part_name(m[i].part), (unsigned)base.master,
                        (unsigned)mut.master, back.master == base.master ? "yes" : "NO",
                        good ? "ok" : "FAIL");
            }
            fprintf(stderr, "RB_MUTATION_SUMMARY %d/%d\n", ok, n);
        }
        return;
    }
    if (P.mode == P_DIGEST_ONLY) {
        /* Snapshot + digest exactly as a live netplay tick would, never load. */
        size_t n = 0;
        uint8_t *c = dup_image(&n);
        (void)nes_rb_state_digest_master();
        free(c);
        return;
    }
    if (P.mode == P_IDLE) {
        if (P.done >= P.count || P.tick < P.next_start) return;
        P.saved = dup_image(&P.saved_len);
        P.img1[0] = dup_image(&P.len1[0]);
        if (!P.saved || !P.img1[0]) { free_run(); P.mode = P_OFF; return; }
        P.i = 0;
        P.mode = P_RUN1;
        return;
    }
    if (getenv("NES_RB_PROBE_TRACE") && (P.mode == P_RUN1 || P.mode == P_RUN2) && P.i < 3)
        fprintf(stderr, "RB_PROBE_TRACE run%d i=%u top cycles=%llu\n",
                P.mode == P_RUN1 ? 1 : 2, (unsigned)P.i + 1u,
                (unsigned long long)g_nes_cycles);
    if (P.mode == P_RUN1) {
        P.i++;
        P.img1[P.i] = dup_image(&P.len1[P.i]);
        if (P.i < P.span) return;
        /* Rewind to K. The rest of this callback replays tick K; its end
         * discards the native stack and resumes the saved continuation. */
        if (!nes_rb_state_load(P.saved, P.saved_len)) {
            fprintf(stderr, "RB_PROBE #%u tick=%u LOAD FAILED\n", (unsigned)P.done,
                    (unsigned)(P.tick - P.span));
            free_run();
            P.mode = P_OFF;
            return;
        }
        {
            size_t n = 0;
            const uint8_t *now = nes_rb_state_image(&n);
            char what[96];
            long d = nes_rb_state_first_diff(P.saved, P.saved_len, now, n,
                                             what, sizeof(what));
            P.sym_ok = (d < 0);
            if (!P.sym_ok) {
                fprintf(stderr, "RB_PROBE #%u symmetry FAIL: load did not reproduce the "
                                "saved image, first byte %ld (%s)\n",
                        (unsigned)P.done, d, what);
                P.sym_fail++;
            }
        }
        P.i = 0;
        P.first_div = -1;
        P.div_what[0] = 0;
        P.replaying = 1;
        P.mode = P_RUN2;
        return;
    }
    if (P.mode == P_RUN2) {
        size_t n = 0;
        const uint8_t *now;
        P.i++;
        now = nes_rb_state_image(&n);
        if (P.first_div < 0 && now && P.img1[P.i]) {
            char what[96];
            long d = nes_rb_state_first_diff(P.img1[P.i], P.len1[P.i], now, n,
                                             what, sizeof(what));
            if (d >= 0) {
                NesRbDigest a, b;
                int k;
                nes_rb_digest_image(P.img1[P.i], P.len1[P.i], &a);
                nes_rb_digest_image(now, n, &b);
                P.div_part = -1;
                for (k = 0; k < 3; ++k)
                    if (a.part[k] != b.part[k]) { P.div_part = k; break; }
                P.first_div = (long)P.i;
                snprintf(P.div_what, sizeof(P.div_what), "%s run1=%02x run2=%02x", what,
                         (d < (long)P.len1[P.i]) ? P.img1[P.i][d] : 0,
                         (d < (long)n) ? now[d] : 0);
                /* Every differing field at the first divergent tick (grouped),
                 * so one run names the whole carrier set, not just byte one. */
                if (P.len1[P.i] == n) {
                    char last[96] = "", cur[96];
                    size_t b;
                    int shown = 0;
                    fprintf(stderr, "RB_PROBE #%u first divergent tick +%u fields:",
                            (unsigned)P.done, (unsigned)P.i);
                    for (b = 0; b < n && shown < 24; ++b) {
                        char *plus;
                        if (P.img1[P.i][b] == now[b]) continue;
                        nes_rb_state_describe(P.img1[P.i], n, b, cur, sizeof(cur));
                        (void)plus;
                        if (strcmp(cur, last)) {
                            fprintf(stderr, " %s(@%zu %02x->%02x)", cur, b,
                                    P.img1[P.i][b], now[b]);
                            snprintf(last, sizeof(last), "%s", cur);
                            shown++;
                        }
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
        if (P.i < P.span) return;
        {
            uint32_t k0 = P.tick - 2 * P.span;
            if (P.first_div < 0 && P.sym_ok) {
                P.ok++;
                fprintf(stderr, "RB_PROBE #%u tick=%u span=%u sym=ok result=OK\n",
                        (unsigned)P.done, (unsigned)k0, (unsigned)P.span);
            } else {
                P.diverged += (P.first_div >= 0);
                fprintf(stderr,
                        "RB_PROBE #%u tick=%u span=%u sym=%s result=%s first=+%ld "
                        "part=%s field=%s\n",
                        (unsigned)P.done, (unsigned)k0, (unsigned)P.span,
                        P.sym_ok ? "ok" : "FAIL",
                        P.first_div >= 0 ? "DIVERGED" : "OK",
                        P.first_div, P.first_div >= 0 ? nes_rb_part_name(P.div_part) : "-",
                        P.first_div >= 0 ? P.div_what : "-");
            }
        }
        P.done++;
        P.replaying = 0;
        free_run();
        P.next_start = P.tick + P.gap;
        P.mode = P_IDLE;
    }
}

/* Right before the tick's guest code: record run 1's rows, force them in
 * run 2, so both passes see identical published input whatever the input
 * sources (script, TCP, pads) do after the rewind. */
void nes_rb_probe_pre_tick(void)
{
    if (getenv("NES_RB_PROBE_TRACE") && (P.mode == P_RUN1 || P.mode == P_RUN2) && P.i < 3)
        fprintf(stderr, "RB_PROBE_TRACE run%d i=%u pre_nmi cycles=%llu frame=%llu\n",
                P.mode == P_RUN1 ? 1 : 2, (unsigned)P.i,
                (unsigned long long)g_nes_cycles, (unsigned long long)g_frame_count);
    if (P.mode == P_RUN1 && P.i < PROBE_MAX_SPAN) {
        P.rows[P.i][0] = g_controller1_buttons;
        P.rows[P.i][1] = g_controller2_buttons;
        P.rows[P.i][2] = g_logical_input[2];
        P.rows[P.i][3] = g_logical_input[3];
    } else if (P.mode == P_RUN2 && P.i < PROBE_MAX_SPAN) {
        g_controller1_buttons = P.rows[P.i][0];
        g_controller2_buttons = P.rows[P.i][1];
        g_logical_input[2] = P.rows[P.i][2];
        g_logical_input[3] = P.rows[P.i][3];
    }
}
