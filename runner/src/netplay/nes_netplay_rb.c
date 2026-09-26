/*
 * nes_netplay_rb.c -- the NES half of rollback: what a snapshot is, what a
 * digest covers, how a pad row is laid out, and how one tick is kept off
 * screen during a replay. Everything that decides WHEN to rewind and talks to
 * the peers about it is recomp-net's episode driver (recomp_net/rb_driver.h).
 *
 * Replay model: INCREMENTAL. A NES tick is not a function that returns: the
 * generated RESET loop runs forever on the host C stack and the frame
 * callback (the NMI + one frame) is called nested inside guest code. So the
 * driver hands out one replayed tick per outer frame callback, and the host
 * runs it with the same code path it runs a live tick with (main_runner.c).
 * A baseline load happens at the top of a callback, the rest of that callback
 * replays the load tick, and the callback's end discards the stale native
 * stack and resumes the guest continuation the snapshot recorded
 * (docs/NETPLAY.md, "Replay model", for why this and not a fiber snapshot).
 */
#include "nes_netplay_rb.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nes_rb_state.h"
#include "nes_runtime.h"
#include "retcomm_rbengine/mono_ms.h"
#include "retcomm_rbengine/snap_ring.h"

void nes_rb_log_bridge(void);   /* main_runner.c */

#define RB_MAX_SLOTS 4
/* NES pads are 8 bits, active high (A=0x80 B=0x40 Select=0x20 Start=0x10
 * Up=0x08 Down=0x04 Left=0x02 Right=0x01; keybinds.c). input_hist's default
 * invent neutral is 0xFFFF (PSX, active low); 8-bit rows can never be 0xFFFF,
 * so that sentinel from an older peer maps to neutral. */
#define RB_PSX_NEUTRAL 0xFFFFu
#define RB_BUTTON_MASK 0x00FFu
/* Validation injector (FORCE_MISPREDICT) flips A|START: the title screen
 * reads START and gameplay reads A, so an injected mispredict always reaches
 * the guest. The driver's historical 0x0040 is B here, which the SMB title
 * screen ignores -- an episode it opened there would replay nothing visible. */
#define RB_INJECT_BITS 0x0090u

/* Live digests (state after tick t) for replays_changed. */
#define RB_LIVE_RING 512

static struct {
    NesNetplayRbBindings b;
    RNetRbDriver *drv;
    RbeSnapRing  *snaps;
    uint32_t      snap_depth;
    uint8_t       resolved[RB_MAX_SLOTS];
    int           last_admit;
    uint32_t      last_admit_tick;
    uint32_t      live_tick[RB_LIVE_RING];
    uint32_t      live_dig[RB_LIVE_RING];
    uint8_t       live_valid[RB_LIVE_RING];
    uint32_t      replays_changed;
    uint32_t      replays_same;
} g_rb;

static int rb_env_int(const char *name, const char *generic, int def, int lo, int hi)
{
    const char *v = getenv(name);
    long n;
    char *end;
    if (!v && generic)
        v = getenv(generic);
    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < lo || n > hi)
        return def;
    return (int)n;
}

static RNetRbDriver *rb_drv(void)
{
    if (!g_rb.drv)
        g_rb.drv = rnet_rb_driver_create();
    return g_rb.drv;
}

static int rb_slot_count(void)
{
    int n = g_rb.b.slot_count ? *g_rb.b.slot_count : 2;
    if (n < 1) n = 1;
    if (n > RB_MAX_SLOTS) n = RB_MAX_SLOTS;
    return n;
}

/* ── snapshots ─────────────────────────────────────────────────────────── */

static int rb_snap_serialize(void *ctx, uint32_t tick, uint8_t **out_data,
                             size_t *out_len)
{
    size_t n = 0;
    const uint8_t *img;
    uint8_t *copy;
    (void)ctx;
    (void)tick;
    /* The image the digest was taken over at this same point (cached until
     * guest code runs), so a snapshot and its digest cannot disagree. */
    img = nes_rb_state_image(&n);
    if (!img || !n)
        return 0;
    copy = (uint8_t *)malloc(n);
    if (!copy)
        return 0;
    memcpy(copy, img, n);
    *out_data = copy;
    *out_len = n;
    return 1;
}

static int rb_snap_deserialize(void *ctx, uint32_t tick, const uint8_t *data,
                               size_t len)
{
    (void)ctx;
    (void)tick;
    return nes_rb_state_load(data, len);
}

static const RbeSnapVTable g_snap_vt = {
    NULL, &rb_snap_serialize, &rb_snap_deserialize
};

static int rb_host_snap_save(void *ctx, uint32_t tick)
{
    (void)ctx;
    return g_rb.snaps ? rbe_snap_ring_save(g_rb.snaps, tick, &g_snap_vt) : 0;
}

static int rb_host_snap_load(void *ctx, uint32_t tick)
{
    (void)ctx;
    return g_rb.snaps ? rbe_snap_ring_load(g_rb.snaps, tick, &g_snap_vt) : 0;
}

static int rb_host_snap_has(void *ctx, uint32_t tick)
{
    (void)ctx;
    return g_rb.snaps ? rbe_snap_ring_has(g_rb.snaps, tick) : 0;
}

static int rb_host_snap_oldest(void *ctx, uint32_t *oldest)
{
    (void)ctx;
    if (!g_rb.snaps || rbe_snap_ring_count(g_rb.snaps) == 0)
        return 0;
    *oldest = rbe_snap_ring_oldest_tick(g_rb.snaps);
    return 1;
}

static void rb_host_snap_drop_after(void *ctx, uint32_t tick)
{
    (void)ctx;
    if (g_rb.snaps)
        (void)rbe_snap_ring_drop_after(g_rb.snaps, tick);
}

/* ── one tick ──────────────────────────────────────────────────────────── */

static void rb_host_publish(void *ctx, uint32_t tick, const RNetRbFrame *rows,
                            int slots, int replay)
{
    int i;
    (void)ctx;
    memset(g_rb.resolved, 0, sizeof(g_rb.resolved));
    for (i = 0; i < slots && i < RB_MAX_SLOTS; ++i)
        g_rb.resolved[i] = (uint8_t)(rows[i].buttons & RB_BUTTON_MASK);
    g_rb.last_admit_tick = tick;
    if (getenv("NES_RB_DEBUG_ROWS"))
        fprintf(stderr, "RB_ROWS tick=%u replay=%d rows=%02x %02x %02x %02x\n",
                (unsigned)tick, replay, g_rb.resolved[0], g_rb.resolved[1],
                g_rb.resolved[2], g_rb.resolved[3]);
    if (g_rb.b.publish)
        g_rb.b.publish(tick, g_rb.resolved, slots > RB_MAX_SLOTS ? RB_MAX_SLOTS : slots,
                       replay);
}

/* Resim replays ticks the player has already seen and heard. The runner
 * presents nothing and drops the audio the replay produces (main_runner.c,
 * rb_resim) -- recomp-ai-rules/NETPLAY.md §1: the presented image is never
 * simulation. */
static void rb_host_resim_begin(void *ctx)
{
    (void)ctx;
    if (g_rb.b.resim) g_rb.b.resim(1);
}

static void rb_host_resim_end(void *ctx)
{
    (void)ctx;
    if (g_rb.b.resim) g_rb.b.resim(0);
}

/* ── digests ───────────────────────────────────────────────────────────── */

static uint32_t rb_host_digest_master(void *ctx)
{
    (void)ctx;
    return nes_rb_state_digest_master();
}

static void rb_host_digest_parts(void *ctx, RNetRbDigestParts *out)
{
    NesRbDigest d;
    (void)ctx;
    nes_rb_state_digest(&d);
    out->master = d.master;
    out->part[0] = d.part[0];
    out->part[1] = d.part[1];
    out->part[2] = d.part[2];
}

/* ── pads ──────────────────────────────────────────────────────────────── */

static void rb_host_decode(void *ctx, int slot, const RNetInputSample *in,
                           RNetRbFrame *out)
{
    (void)ctx;
    (void)slot;
    out->buttons = (uint16_t)(in->size >= 1 ? in->bytes[0] : 0u);
    out->stick_x = 0;
    out->stick_y = 0;
    out->analog = 0;
}

static void rb_host_sanitize(void *ctx, int slot, RNetRbFrame *f)
{
    (void)ctx;
    (void)slot;
    if (f->buttons == RB_PSX_NEUTRAL)
        f->buttons = 0u;
    f->buttons &= RB_BUTTON_MASK;
    f->stick_x = 0;
    f->stick_y = 0;
    f->analog = 0;
}

static void rb_host_neutral(void *ctx, int slot, RNetRbFrame *out)
{
    (void)ctx;
    (void)slot;
    out->buttons = 0u;
}

/* ── session control ───────────────────────────────────────────────────── */

static void rb_host_boot_digest_noted(void *ctx)
{
    NesRbDigest d;
    uint16_t pc = 0;
    int charged = 0;
    (void)ctx;
    nes_rb_state_digest(&d);
    (void)runtime_get_savestate_resume(&pc, &charged);
    fprintf(stderr,
            "nes_netplay: RB boot parts master=%08x cpu_wram=%08x ppu=%08x "
            "apu_io_mods=%08x\n",
            (unsigned)d.master, (unsigned)d.part[0], (unsigned)d.part[1],
            (unsigned)d.part[2]);
    fprintf(stderr,
            "nes_netplay: RB boot taken at frame=%llu cycles=%llu resume_pc=$%04X "
            "charged=%d\n",
            (unsigned long long)g_frame_count, (unsigned long long)g_nes_cycles,
            (unsigned)pc, charged);
}

static void rb_host_return_to_lobby(void *ctx)
{
    const char *why = rnet_rb_driver_refusal(g_rb.drv);
    (void)ctx;
    if (g_rb.b.refused)
        g_rb.b.refused(why ? why : "refused");
}

static uint32_t rb_host_now_ms(void *ctx)
{
    (void)ctx;
    return rbe_mono_ms();
}

/* ── coordinated stop ──────────────────────────────────────────────────── */

static volatile sig_atomic_t s_quiesce_signalled;

#if !defined(_WIN32)
static void rb_on_sigusr1(int sig)
{
    (void)sig;
    s_quiesce_signalled = 1;
}
#endif

static void rb_install_quiesce_signal(void)
{
#if !defined(_WIN32)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = rb_on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
#endif
}

int nes_netplay_rb_draining(void)
{
    return g_rb.drv && rnet_rb_driver_quiesce_state(g_rb.drv) == RNET_RB_QUIESCE_DRAINING;
}

int nes_netplay_rb_quiesced(void)
{
    RNetRbQuiesce q;
    if (!g_rb.drv) return 0;
    q = rnet_rb_driver_quiesce_state(g_rb.drv);
    return q == RNET_RB_QUIESCE_DRAINED || q == RNET_RB_QUIESCE_TIMED_OUT;
}

void nes_netplay_rb_request_quiesce(void)
{
    if (g_rb.drv && rnet_rb_driver_quiesce_state(g_rb.drv) == RNET_RB_QUIESCE_NONE) {
        fprintf(stderr, "nes_netplay: match tick limit — draining rollback\n");
        rnet_rb_driver_request_quiesce(g_rb.drv);
    }
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

void nes_netplay_rb_bind(const NesNetplayRbBindings *b)
{
    if (!b) {
        memset(&g_rb.b, 0, sizeof(g_rb.b));
        return;
    }
    g_rb.b = *b;
}

int nes_netplay_rb_start(void)
{
    RNetRbDriverConfig cfg;
    RNetRbHost host;
    RNetRbDriver *drv = rb_drv();

    nes_netplay_rb_shutdown();
    {
        NesNetplayRbBindings keep = g_rb.b;
        memset(&g_rb, 0, sizeof(g_rb));
        g_rb.b = keep;
        g_rb.drv = drv;
    }
    if (!drv) {
        fprintf(stderr, "nes_netplay: RB start refused — could not allocate "
                        "the rollback driver\n");
        return 0;
    }
    if (!g_rb.b.session) {
        fprintf(stderr, "nes_netplay: RB start with no session binding — "
                        "call nes_netplay_rb_bind() first\n");
        return 0;
    }
    /* Depth floor 16 for the reason snesrecomp measured (a shallower ring
     * NACKs episodes whose load tick aged out at 200 ms). One snapshot per
     * tick is ~170 KB for SMB with its mod records, so the default 40 slots
     * is ~7 MB. */
    g_rb.snap_depth = (uint32_t)rb_env_int("NES_RB_SNAP_DEPTH", "RNET_RB_SNAP_DEPTH",
                                           (int)RBE_SNAP_RING_DEFAULT_DEPTH, 16, 240);
    g_rb.snaps = rbe_snap_ring_create(g_rb.snap_depth);
    if (!g_rb.snaps)
        return 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.session = g_rb.b.session;
    cfg.local_slot = g_rb.b.local_slot;
    cfg.slot_count = g_rb.b.slot_count;
    cfg.input_delay = g_rb.b.input_delay;
    cfg.input_prediction = g_rb.b.input_prediction;
    cfg.force_turn = g_rb.b.force_turn;
    cfg.occupied_mask = g_rb.b.occupied_mask;
    cfg.replay_mode = RNET_RB_REPLAY_INCREMENTAL;
    cfg.part_names[0] = nes_rb_part_name(0);
    cfg.part_names[1] = nes_rb_part_name(1);
    cfg.part_names[2] = nes_rb_part_name(2);
    cfg.snap_depth = g_rb.snap_depth;
    cfg.log_prefix = "nes_netplay";
    cfg.env_alias = "NES_RB";
    cfg.inject_flip_bits = (uint16_t)rb_env_int("NES_RB_INJECT_BITS", NULL,
                                                (int)RB_INJECT_BITS, 1, 0xFF);

    memset(&host, 0, sizeof(host));
    host.snap_save = &rb_host_snap_save;
    host.snap_load = &rb_host_snap_load;
    host.snap_has = &rb_host_snap_has;
    host.snap_oldest = &rb_host_snap_oldest;
    host.snap_drop_after = &rb_host_snap_drop_after;
    host.publish = &rb_host_publish;
    host.run_tick = NULL;   /* INCREMENTAL: the runner runs the tick */
    host.resim_begin = &rb_host_resim_begin;
    host.resim_end = &rb_host_resim_end;
    host.digest_master = &rb_host_digest_master;
    host.digest_parts = &rb_host_digest_parts;
    host.decode_sample = &rb_host_decode;
    host.sanitize_row = &rb_host_sanitize;
    host.neutral_row = &rb_host_neutral;
    host.boot_digest_noted = &rb_host_boot_digest_noted;
    host.request_return_to_lobby = &rb_host_return_to_lobby;
    host.now_ms = &rb_host_now_ms;

    if (!rnet_rb_driver_start(drv, &cfg, &host)) {
        rbe_snap_ring_destroy(g_rb.snaps);
        g_rb.snaps = NULL;
        return 0;
    }
    s_quiesce_signalled = 0;
    rb_install_quiesce_signal();
    return 1;
}

void nes_netplay_rb_shutdown(void)
{
    if (g_rb.drv)
        rnet_rb_driver_shutdown(g_rb.drv);
    if (g_rb.snaps) {
        rbe_snap_ring_destroy(g_rb.snaps);
        g_rb.snaps = NULL;
    }
}

void nes_netplay_rb_stage_local(uint8_t pad)
{
    /* The driver samples the local seat through the session's host vtable
     * (nes_netplay.c host_sample_local); nothing to keep here. */
    (void)pad;
}

int nes_netplay_rb_poll_admit(void)
{
    int a;
    if (s_quiesce_signalled && g_rb.drv &&
        rnet_rb_driver_quiesce_state(g_rb.drv) == RNET_RB_QUIESCE_NONE) {
        fprintf(stderr, "nes_netplay: SIGUSR1 — draining rollback, then exiting\n");
        rnet_rb_driver_request_quiesce(g_rb.drv);
    }
    a = (int)rnet_rb_driver_poll_admit(g_rb.drv);
    g_rb.last_admit = a;
    return a;
}

void nes_netplay_rb_finish_frame(void)
{
    /* The admitted tick has run: the current state is the state after it.
     * Record a live tick's digest, and grade a replayed one against the live
     * run it corrected -- the evidence that a correction reached the guest. */
    if (g_rb.last_admit == RNET_RB_ADMIT_LIVE ||
        g_rb.last_admit == RNET_RB_ADMIT_REPLAY) {
        uint32_t t = g_rb.last_admit_tick;
        uint32_t d = nes_rb_state_digest_master();
        uint32_t i = t % RB_LIVE_RING;
        {
            /* NES_NET_SHOT_TICK=N: the state after tick N, every time tick N
             * finishes (live, then each replay) -- the last line is what this
             * peer confirmed; a harness compares it across peers. */
            static long shot = -2;
            if (shot == -2) {
                const char *e = getenv("NES_NET_SHOT_TICK");
                shot = (e && e[0]) ? atol(e) : -1;
            }
            if (shot >= 0 && (long)t == shot) {
                NesRbDigest dd;
                nes_rb_state_digest(&dd);
                fprintf(stderr, "NET_TICK_DIGEST tick=%u %s master=%08x cpu_wram=%08x ppu=%08x apu_io_mods=%08x\n",
                        (unsigned)t, g_rb.last_admit == RNET_RB_ADMIT_REPLAY ? "replay" : "live",
                        (unsigned)dd.master, (unsigned)dd.part[0], (unsigned)dd.part[1],
                        (unsigned)dd.part[2]);
            }
        }
        if (g_rb.last_admit == RNET_RB_ADMIT_LIVE) {
            g_rb.live_tick[i] = t;
            g_rb.live_dig[i] = d;
            g_rb.live_valid[i] = 1;
        } else if (g_rb.live_valid[i] && g_rb.live_tick[i] == t) {
            if (getenv("NES_RB_DEBUG_ROWS"))
                fprintf(stderr, "RB_REPLAY_GRADE tick=%u live=%08x replay=%08x\n",
                        (unsigned)t, (unsigned)g_rb.live_dig[i], (unsigned)d);
            if (g_rb.live_dig[i] != d) g_rb.replays_changed++;
            else g_rb.replays_same++;
            g_rb.live_dig[i] = d;   /* the corrected run is the live one now */
        }
    }
    g_rb.last_admit = RNET_RB_ADMIT_STALL;
    rnet_rb_driver_finish_frame(g_rb.drv);
}

/* ── tick cost (NETPLAY_FIELDS) ─────────────────────────────────────────── */

#define RB_COST_N 8192
static float s_cost[2][RB_COST_N];
static uint64_t s_cost_n[2];

void nes_netplay_rb_note_tick_cost(int replay, double us)
{
    int k = replay ? 1 : 0;
    s_cost[k][s_cost_n[k] % RB_COST_N] = (float)us;
    s_cost_n[k]++;
}

static int rb_cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void rb_cost_pct(int k, unsigned *p50, unsigned *p99)
{
    static float tmp[RB_COST_N];
    size_t n = s_cost_n[k] < RB_COST_N ? (size_t)s_cost_n[k] : RB_COST_N;
    *p50 = *p99 = 0;
    if (!n) return;
    memcpy(tmp, s_cost[k], n * sizeof(float));
    qsort(tmp, n, sizeof(float), rb_cmpf);
    *p50 = (unsigned)tmp[(n - 1) / 2];
    *p99 = (unsigned)tmp[(size_t)((double)(n - 1) * 0.99)];
}

/* ── diagnostics ───────────────────────────────────────────────────────── */

void nes_netplay_rb_set_identity(uint32_t build_fp, uint32_t content_fp)
{
    rnet_rb_driver_set_identity(rb_drv(), build_fp, content_fp);
}

void nes_netplay_rb_set_modset(const char *text, RNetRbModSetCheckFn check,
                               RNetRbModSetAdoptFn adopt)
{
    rnet_rb_driver_set_modset(rb_drv(), text, check, adopt);
}

uint32_t nes_netplay_rb_sim_tick(void) { return g_rb.drv ? rnet_rb_driver_sim_tick(g_rb.drv) : 0; }
uint32_t nes_netplay_rb_episode_count(void) { return g_rb.drv ? rnet_rb_driver_episode_count(g_rb.drv) : 0; }
uint32_t nes_netplay_rb_invent_count(void) { return g_rb.drv ? rnet_rb_driver_invent_count(g_rb.drv) : 0; }
uint64_t nes_netplay_rb_resim_ticks(void) { return g_rb.drv ? rnet_rb_driver_resim_ticks(g_rb.drv) : 0; }
uint32_t nes_netplay_rb_desync_count(void) { return g_rb.drv ? rnet_rb_driver_desync_count(g_rb.drv) : 0; }
uint32_t nes_netplay_rb_replays_changed(void) { return g_rb.replays_changed; }
int nes_netplay_rb_in_resim(void) { return g_rb.drv ? rnet_rb_driver_in_resim(g_rb.drv) : 0; }
const char *nes_netplay_rb_refusal(void) { return g_rb.drv ? rnet_rb_driver_refusal(g_rb.drv) : NULL; }

int nes_netplay_rb_last_fork(uint32_t *tick, const char **partition)
{
    return g_rb.drv ? rnet_rb_driver_last_fork(g_rb.drv, tick, partition) : 0;
}

int nes_netplay_rb_fork_digests(uint32_t *mine, uint32_t *theirs)
{
    return g_rb.drv ? rnet_rb_driver_fork_digests(g_rb.drv, mine, theirs) : 0;
}

void nes_netplay_rb_log_summary(void)
{
    if (!g_rb.drv) return;
    fprintf(stderr,
            "NETPLAY_DRIVER sim=%u episodes=%u invents=%u promotes=%u "
            "resim_ticks=%llu desyncs=%u replays_changed=%u replays_same=%u "
            "confirmed=%u rtt_ms=%u\n",
            (unsigned)rnet_rb_driver_sim_tick(g_rb.drv),
            (unsigned)rnet_rb_driver_episode_count(g_rb.drv),
            (unsigned)rnet_rb_driver_invent_count(g_rb.drv),
            (unsigned)rnet_rb_driver_promote_count(g_rb.drv),
            (unsigned long long)rnet_rb_driver_resim_ticks(g_rb.drv),
            (unsigned)rnet_rb_driver_desync_count(g_rb.drv),
            (unsigned)g_rb.replays_changed, (unsigned)g_rb.replays_same,
            (unsigned)rnet_rb_driver_confirmed_through(g_rb.drv),
            (unsigned)rnet_rb_driver_rtt_estimate_ms(g_rb.drv));
    {
        unsigned l50, l99, r50, r99;
        rb_cost_pct(0, &l50, &l99);
        rb_cost_pct(1, &r50, &r99);
        fprintf(stderr, "NETPLAY_FIELDS live=%llu live_us p50=%u p99=%u replay=%llu "
                        "replay_us p50=%u p99=%u\n",
                (unsigned long long)s_cost_n[0], l50, l99,
                (unsigned long long)s_cost_n[1], r50, r99);
    }
    nes_rb_state_log_timing("netplay");
    nes_rb_log_bridge();
}
