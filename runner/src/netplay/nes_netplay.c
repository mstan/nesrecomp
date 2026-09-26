/*
 * nes_netplay.c -- NES session facade over recomp-net. See nes_netplay.h and
 * docs/NETPLAY.md. Shape follows snesrecomp runner/src/netplay/snes_netplay.c
 * (origin/main 8a06b70); what is NES-specific is the pad layout (one byte per
 * seat), the pre-boot SRAM barrier and the session-configuration seal.
 */
#include "nes_netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "nes_netplay_identity.h"
#include "nes_session_config.h"
#include "nes_netplay_rb.h"
#include "nes_rb_state.h"
#include "nes_runtime.h"
#include "save_ram.h"
#include "logical_input.h"
#include "recomp_net/recomp_net.h"

/* ── state ─────────────────────────────────────────────────────────────── */

typedef struct {
    RNetSession *session;
    int      active;
    int      slot_count;
    int      local_slot;
    int      spectator;
    int      input_player;
    int      input_delay;
    int      input_prediction;
    int      rollback;
    uint32_t occupied_mask;
    uint32_t session_id;
    int      relay, hub;
    uint8_t  staged;
    int      admitted;          /* a tick was admitted and has not finished */
    int      sram_needed, sram_done, sram_sent;
    int      guest_sandbox;
    int      return_requested;
    char     return_why[96];
    char     last_error[96];
    char     session_text[512];
    uint32_t start_ms;
    uint32_t cur_tick;          /* the tick whose rows were last published */
} NetplayState;

static NetplayState g_np;
static NesNetplayConfig g_pending;
static int g_pending_valid;
static NesNetplayPublishFn g_publish_hook;
static NesNetplayResimFn g_resim_hook;

void nes_netplay_set_publish_hook(NesNetplayPublishFn fn) { g_publish_hook = fn; }
void nes_netplay_set_resim_hook(NesNetplayResimFn fn) { g_resim_hook = fn; }

void nes_netplay_set_pending_config(const NesNetplayConfig *c)
{
    if (!c) { g_pending_valid = 0; memset(&g_pending, 0, sizeof(g_pending)); return; }
    g_pending = *c;
    g_pending_valid = 1;
}

int nes_netplay_take_pending_config(NesNetplayConfig *c)
{
    if (!c || !g_pending_valid) return 0;
    *c = g_pending;
    g_pending_valid = 0;
    return 1;
}

int nes_netplay_pending(void) { return g_pending_valid; }

void nes_netplay_config_defaults(NesNetplayConfig *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->slot_count = 2;
    c->input_delay = 2;
    c->rollback = 1;
    c->session_id = 1;
    snprintf(c->bind_hostport, sizeof(c->bind_hostport), "0.0.0.0:7777");
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    char *end;
    long n;
    if (!v || !v[0]) return def;
    n = strtol(v, &end, 0);
    return (end == v) ? def : (int)n;
}

void nes_netplay_config_apply_env(NesNetplayConfig *c)
{
    const char *v;
    if (!c) return;
    v = getenv("NES_NETPLAY");
    if (v && *v) c->enabled = atoi(v) != 0;
    c->local_slot = env_int("NES_NET_SLOT", c->local_slot);
    c->slot_count = env_int("NES_NET_SLOTS", c->slot_count);
    c->input_player = env_int("NES_NET_INPUT_PLAYER", c->input_player);
    c->input_delay = env_int("NES_NET_DELAY", c->input_delay);
    c->input_prediction = env_int("NES_NET_PREDICTION", c->input_prediction);
    c->session_id = (uint32_t)env_int("NES_NET_SESSION_ID", (int)c->session_id);
    c->occupied_mask = (uint32_t)env_int("NES_NET_OCCUPIED", (int)c->occupied_mask);
    c->spectator = env_int("NES_NET_SPECTATOR", c->spectator);
    c->spectator_wire_slot = env_int("NES_NET_SPECTATOR_SLOT", c->spectator_wire_slot);
    c->force_input_relay = env_int("NES_NET_RELAY", c->force_input_relay);
    v = getenv("NES_NET_MODE");
    if (v && (!strcmp(v, "rollback") || !strcmp(v, "rb"))) c->rollback = 1;
    else if (v && !strcmp(v, "delay")) c->rollback = 0;
    v = getenv("NES_NET_BIND");
    if (v && *v) snprintf(c->bind_hostport, sizeof(c->bind_hostport), "%s", v);
    v = getenv("NES_NET_PEER");
    if (v && *v) snprintf(c->peer_hostport, sizeof(c->peer_hostport), "%s", v);
    v = getenv("NES_NET_TRANSPORT");
    if (v && (!strcmp(v, "ice") || !strcmp(v, "ICE"))) c->transport = 1;
    else if (v && (!strcmp(v, "lan") || !strcmp(v, "LAN"))) c->transport = 2;
    v = getenv("NES_NET_SESSION_CONFIG");
    if (v && *v)
        nes_netplay_session_from_wire(v, c->session_config, (int)sizeof(c->session_config));
}

/* ── recomp-net host vtable ─────────────────────────────────────────────── */

static void host_sample_local(rnet_u32 tick, RNetInputSample *out, void *ctx)
{
    NetplayState *st = (NetplayState *)ctx;
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->size = NES_NETPLAY_PAD_BYTES;
    out->bytes[0] = st->spectator ? 0u : st->staged;
    out->valid = 1;
}

static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots,
                         void *ctx)
{
    /* Unused: the rollback driver resolves and publishes rows itself. */
    (void)tick; (void)by_slot; (void)slots; (void)ctx;
}

/* ── rollback bindings ─────────────────────────────────────────────────── */

static void np_rb_publish(uint32_t tick, const uint8_t *rows, int slots, int replay)
{
    g_np.cur_tick = tick;
    g_np.admitted = 1;
    if (g_publish_hook) g_publish_hook(rows, slots, replay);
}

static void np_rb_resim(int begin)
{
    if (g_resim_hook) g_resim_hook(begin);
}

static void np_rb_refused(const char *code)
{
    fprintf(stderr, "nes_netplay: match refused (%s) — returning to the lobby\n",
            code ? code : "?");
    nes_netplay_request_return_to_lobby(code ? code : "refused");
}

/* The mod-set handshake carries the session configuration image. */
static int np_modset_check(const char *want, char *reason, uint32_t cap)
{
    char mine[512];
    nes_netplay_session_describe(mine, (int)sizeof(mine));
    if (want && !strcmp(want, mine)) return 0;
    if (reason && cap) snprintf(reason, cap, "session configuration differs from the host's");
    return 1;
}

static int np_modset_adopt(const char *want, char *reason, uint32_t cap)
{
    /* Session settlement is ephemeral (NETPLAY.md §4): nothing is written to
     * the player's settings. The launch path applies the host's image before
     * boot, so reaching here means it could not be honoured at all. */
    (void)want;
    if (reason && cap) snprintf(reason, cap, "session configurations are applied at launch, not adopted");
    return 1;
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

static void np_set_error(const char *e)
{
    snprintf(g_np.last_error, sizeof(g_np.last_error), "%s", e ? e : "");
}

int nes_netplay_start(const NesNetplayConfig *cfg)
{
    RNetConfig rcfg;
    RNetHostVTable host;
    int seats, rc;

    if (!cfg || !cfg->enabled) return -1;
    if (g_np.session) nes_netplay_shutdown();
    memset(&g_np, 0, sizeof(g_np));

    seats = cfg->slot_count > 0 ? cfg->slot_count : 2;
    if (seats < 2) seats = 2;
    /* Abort rather than degrade: a game built for fewer seats than the room
     * would read neutral for the extra players on one peer only if it
     * silently narrowed the session. */
    if (seats > NES_NETPLAY_MAX_SLOTS || seats > NESRECOMP_INPUT_SEATS) {
        fprintf(stderr, "nes_netplay: %d seats requested but this build routes %d "
                        "— refusing to start\n", seats, NESRECOMP_INPUT_SEATS);
        np_set_error("seats_unsupported");
        return -1;
    }
    if (cfg->transport == 1) {
        fprintf(stderr, "nes_netplay: ICE transport requested, but this build carries "
                        "LAN / lobby-relay UDP only — refusing rather than falling back\n");
        np_set_error("ice_not_built");
        return -4;
    }

    /* The session configuration image: the host's when one was handed in
     * (lobby caps or NES_NET_SESSION_CONFIG), else this peer's own settings.
     * Applied on every peer BEFORE boot, never persisted. */
    {
        char why[160], offer[512];
        const char *text = cfg->session_config;
        if (!text[0]) {
            /* No image handed in (the host of an env-driven match, or a LAN
             * room, which carries no caps): run this peer's own offer; the
             * mod-set handshake refuses the match if the peers differ. */
            nes_netplay_session_describe_offer(offer, (int)sizeof(offer));
            text = offer;
        }
        if (!nes_netplay_session_apply(text, why, (int)sizeof(why))) {
            fprintf(stderr, "nes_netplay: cannot apply the host's session "
                            "configuration (%s) — refusing to start\n", why);
            np_set_error("session_config_refused");
            return -6;
        }
    }
    nes_netplay_session_describe(g_np.session_text, (int)sizeof(g_np.session_text));

    rnet_config_init_defaults(&rcfg);
    rcfg.slot_count = (rnet_u8)seats;
    if (cfg->spectator) {
        if (cfg->spectator_wire_slot < seats || cfg->spectator_wire_slot > 0xff) {
            fprintf(stderr, "nes_netplay: spectator without a usable relay slot "
                            "(%d, seats=%d) — refusing to start\n",
                    cfg->spectator_wire_slot, seats);
            np_set_error("spectator_slot");
            return -1;
        }
        if (!cfg->force_input_relay) {
            fprintf(stderr, "nes_netplay: spectating needs the lobby server's UDP "
                            "input relay — refusing to start\n");
            np_set_error("spectator_needs_relay");
            return -5;
        }
        rcfg.local_slot = (rnet_u8)seats;
        rcfg.wire_slot = (rnet_u8)cfg->spectator_wire_slot;
    } else {
        int slot = cfg->local_slot < 0 ? 0 : cfg->local_slot;
        if (slot >= seats) slot = seats - 1;
        rcfg.local_slot = (rnet_u8)slot;
    }
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0
                                : (cfg->input_delay > 20 ? 20 : cfg->input_delay));
    rcfg.session_id = cfg->session_id ? cfg->session_id : 1u;
    rcfg.occupied_mask = cfg->occupied_mask;

    memset(&host, 0, sizeof(host));
    host.sample_local = host_sample_local;
    host.publish = host_publish;
    host.ctx = &g_np;
    g_np.session = rnet_session_create(&rcfg, &host);
    if (!g_np.session) { np_set_error("session_create"); return -2; }

    /* Transport. Two seats dial each other; with more, seat 0 hubs the LAN
     * session (recomp-net's host-as-relay) unless the lobby server's UDP
     * relay carries the match, in which case every seat dials the relay. */
    g_np.relay = cfg->force_input_relay ? 1 : 0;
    if (seats > 2 && !g_np.relay && rcfg.local_slot == 0) {
        g_np.hub = 1;
        rc = rnet_session_start_lan_hub(g_np.session, cfg->bind_hostport);
    } else {
        rc = rnet_session_start_lan(g_np.session, cfg->bind_hostport, cfg->peer_hostport);
    }
    if (rc != 0) {
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
        np_set_error("transport_start");
        return -3;
    }

    g_np.active = 1;
    g_np.slot_count = seats;
    g_np.local_slot = rcfg.local_slot;
    g_np.spectator = cfg->spectator ? 1 : 0;
    g_np.input_player = cfg->input_player;
    g_np.input_delay = rcfg.input_delay;
    g_np.input_prediction = cfg->input_prediction;
    if (g_np.input_prediction && g_np.input_prediction < 2) g_np.input_prediction = 2;
    if (g_np.input_prediction > 32) g_np.input_prediction = 32;
    g_np.rollback = cfg->rollback ? 1 : 0;
    g_np.occupied_mask = cfg->occupied_mask;
    g_np.session_id = rcfg.session_id;
    g_np.start_ms = SDL_GetTicks();

    /* Host-authoritative SRAM: every peer boots with the host's cartridge
     * RAM, transferred before any guest code runs; guests persist only into a
     * sandbox, so a peer's personal save can neither enter the match nor be
     * overwritten by it. Forced for a title without battery RAM by
     * NES_NET_SRAM_SYNC=1 (how the transfer is exercised on SMB). */
    g_np.sram_needed = env_int("NES_NET_SRAM_SYNC", -1);
    if (g_np.local_slot != 0) {
        save_ram_set_sandbox("netplay");
        g_np.guest_sandbox = 1;
    }

    /* "Delay" is the rollback driver with prediction pinned off, so both
     * modes run the same episode protocol and the same wire messages. */
#if defined(_WIN32)
    _putenv_s("NES_RB_LOCKSTEP", g_np.rollback ? "" : "1");
#else
    if (g_np.rollback) unsetenv("NES_RB_LOCKSTEP");
    else setenv("NES_RB_LOCKSTEP", "1", 1);
#endif
    {
        NesNetplayRbBindings b;
        memset(&b, 0, sizeof(b));
        b.session = &g_np.session;
        b.local_slot = &g_np.local_slot;
        b.slot_count = &g_np.slot_count;
        b.input_delay = &g_np.input_delay;
        b.input_prediction = &g_np.input_prediction;
        b.occupied_mask = g_np.occupied_mask;
        b.force_turn = cfg->force_turn;
        b.publish = np_rb_publish;
        b.resim = np_rb_resim;
        b.refused = np_rb_refused;
        nes_netplay_rb_bind(&b);
        nes_netplay_rb_set_identity(nes_netplay_identity_build_fp(),
                                    nes_netplay_identity_content_fp());
        nes_netplay_rb_set_modset(g_np.session_text, np_modset_check, np_modset_adopt);
        if (!nes_netplay_rb_start()) {
            fprintf(stderr, "nes_netplay: rollback driver failed to start — "
                            "refusing the session (there is no other admit path)\n");
            nes_netplay_rb_bind(NULL);
            rnet_session_destroy(g_np.session);
            g_np.session = NULL;
            g_np.active = 0;
            np_set_error("driver_start");
            return -7;
        }
    }

    {
        char wire[512];
        nes_netplay_session_to_wire(g_np.session_text, wire, (int)sizeof(wire));
        fprintf(stderr,
                "nes_netplay: started transport=%s slot=%d slots=%d spectator=%d "
                "session=%u delay=%u prediction=%d mode=%s bind=%s peer=%s "
                "identity=%s rom=%.16s session_config=\"%s\"\n",
                nes_netplay_transport_name(), g_np.local_slot, g_np.slot_count,
                g_np.spectator, (unsigned)g_np.session_id, (unsigned)g_np.input_delay,
                g_np.input_prediction, g_np.rollback ? "rollback" : "lockstep",
                cfg->bind_hostport, cfg->peer_hostport[0] ? cfg->peer_hostport : "(hub)",
                nes_netplay_identity_game_version(), nes_netplay_identity_rom_sha256(),
                wire);
    }
    return 0;
}

static void np_pump(void)
{
    if (g_np.session)
        rnet_session_pump(g_np.session);
}

static int np_sram_barrier_step(void)
{
    const void *data = NULL;
    size_t size = 0;
    rnet_u8 op = 0, slot = 0;
    if (g_np.sram_done) return 1;
    if (g_np.local_slot == 0) {
        if (!g_np.sram_sent) {
            if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_SRAM, 0, g_sram,
                                         sizeof(g_sram)) != 0) {
                fprintf(stderr, "nes_netplay: SRAM transfer could not start\n");
                return -1;
            }
            g_np.sram_sent = 1;
            fprintf(stderr, "nes_netplay: sending host SRAM (%u bytes)\n",
                    (unsigned)sizeof(g_sram));
        }
        /* Done when the session hands the finished transfer back, and the
         * host must FINISH it (as the guest does): a transfer left open kept
         * the session in its state-transfer mode, the host stopped sending
         * input, and the guest waited at tick 8 until "peer gone" (measured
         * with NES_NET_SRAM_SYNC=1, 2026-09-25). */
        if (rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size)) {
            rnet_session_state_finish(g_np.session, 0);
            g_np.sram_done = 1;
            fprintf(stderr, "nes_netplay: host SRAM delivered to every peer (%u bytes)\n",
                    (unsigned)sizeof(g_sram));
        }
        return g_np.sram_done;
    }
    if (rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size)) {
        if (op == RNET_STATE_OP_SRAM && data && size == sizeof(g_sram)) {
            memcpy(g_sram, data, size);
            save_ram_sync_snapshot();
            g_np.sram_done = 1;
            fprintf(stderr, "nes_netplay: applied host SRAM (%u bytes) before boot\n",
                    (unsigned)size);
        } else {
            fprintf(stderr, "nes_netplay: unexpected state blob op=%u size=%u\n",
                    (unsigned)op, (unsigned)size);
        }
        rnet_session_state_finish(g_np.session, 0);
    }
    return g_np.sram_done;
}

int nes_netplay_boot_barrier(void)
{
    uint32_t t0 = SDL_GetTicks();
    const uint32_t timeout = (uint32_t)env_int("NES_NET_CONNECT_TIMEOUT_MS", 30000);
    if (!nes_netplay_active()) return 0;
    if (g_np.sram_needed < 0)
        g_np.sram_needed = save_ram_active() ? 1 : 0;
    if (!g_np.sram_needed) g_np.sram_done = 1;
    for (;;) {
        SDL_Event ev;
        np_pump();
        if (rnet_session_is_running(g_np.session)) {
            int r = np_sram_barrier_step();
            if (r < 0) { np_set_error("sram_sync"); return -1; }
            if (r > 0) break;
        }
        if (SDL_WasInit(SDL_INIT_EVENTS)) {
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT ||
                    (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)) {
                    np_set_error("left_before_boot");
                    return -1;
                }
            }
        }
        if (timeout && (uint32_t)(SDL_GetTicks() - t0) > timeout) {
            fprintf(stderr, "nes_netplay: peers not connected after %u ms — "
                            "returning to the lobby\n", (unsigned)timeout);
            np_set_error("connect_timeout");
            return -1;
        }
        SDL_Delay(1);
    }
    fprintf(stderr, "nes_netplay: boot barrier passed after %u ms (sram=%s)\n",
            (unsigned)(SDL_GetTicks() - t0),
            g_np.sram_needed ? "host-authoritative" : "none");
    return 0;
}

void nes_netplay_shutdown(void)
{
    nes_netplay_rb_shutdown();
    nes_netplay_rb_bind(NULL);
    if (g_np.session) {
        (void)rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        g_np.session = NULL;
    }
    if (g_np.guest_sandbox) save_ram_set_sandbox(NULL);
    nes_netplay_session_restore();
    {
        char keep_err[96];
        snprintf(keep_err, sizeof(keep_err), "%s", g_np.last_error);
        memset(&g_np, 0, sizeof(g_np));
        snprintf(g_np.last_error, sizeof(g_np.last_error), "%s", keep_err);
    }
}

int nes_netplay_active(void) { return g_np.active && g_np.session != NULL; }
int nes_netplay_is_running(void) { return nes_netplay_active() && rnet_session_is_running(g_np.session); }
int nes_netplay_rollback_active(void) { return nes_netplay_active() && g_np.rollback; }
int nes_netplay_local_slot(void) { return nes_netplay_active() ? g_np.local_slot : -1; }
int nes_netplay_slot_count(void) { return nes_netplay_active() ? g_np.slot_count : 2; }
int nes_netplay_is_spectator(void) { return nes_netplay_active() && g_np.spectator; }
int nes_netplay_is_host(void) { return nes_netplay_active() && g_np.local_slot == 0; }
int nes_netplay_input_player(void) { return nes_netplay_active() ? g_np.input_player : 0; }
uint32_t nes_netplay_current_tick(void) { return nes_netplay_active() ? g_np.cur_tick : 0; }
uint32_t nes_netplay_sim_tick(void) { return nes_netplay_active() ? nes_netplay_rb_sim_tick() : 0; }

const char *nes_netplay_transport_name(void)
{
    if (!g_np.active) return "none";
    if (g_np.relay) return "relay";
    return g_np.hub ? "lan-hub" : "lan";
}

void nes_netplay_stage_local(uint8_t pad)
{
    g_np.staged = pad;
}

int nes_netplay_poll_admit(void)
{
    int a;
    if (!nes_netplay_active()) return NES_NETPLAY_ADMIT_LIVE;
    np_pump();
    a = nes_netplay_rb_poll_admit();
    if (a == RNET_RB_ADMIT_REPLAY) return NES_NETPLAY_ADMIT_REPLAY;
    if (a == RNET_RB_ADMIT_LIVE) return NES_NETPLAY_ADMIT_LIVE;
    /* A peer that left or went silent ends the match here, not in a hang. */
    if (!nes_netplay_rb_draining() && !nes_netplay_rb_quiesced() &&
        rnet_session_is_running(g_np.session) &&
        rnet_session_peer_disconnected(g_np.session, 1500)) {
        RNetSessionStats st;
        memset(&st, 0, sizeof(st));
        rnet_session_get_stats(g_np.session, &st);
        fprintf(stderr, "nes_netplay: peer gone (%s; packets_rx=%u state_busy=%d) — "
                        "returning to the lobby\n",
                rnet_session_peer_disconnected(g_np.session, 0) ? "BYE" : "silent 1.5 s",
                (unsigned)st.packets_rx, st.state_busy);
        nes_netplay_request_return_to_lobby("peer_disconnected");
    }
    return NES_NETPLAY_ADMIT_STALL;
}

void nes_netplay_frame_end(void)
{
    if (!nes_netplay_active() || !g_np.admitted) return;
    g_np.admitted = 0;
    nes_netplay_rb_finish_frame();
}

void nes_netplay_request_return_to_lobby(const char *why)
{
    if (g_np.return_requested) return;
    g_np.return_requested = 1;
    snprintf(g_np.return_why, sizeof(g_np.return_why), "%s", why ? why : "match_ended");
    np_set_error(g_np.return_why);
}

int nes_netplay_return_to_lobby_requested(const char **why)
{
    if (why) *why = g_np.return_why;
    return g_np.return_requested;
}

const char *nes_netplay_last_error(void) { return g_np.last_error; }

void nes_netplay_request_quiesce(void) { if (nes_netplay_active()) nes_netplay_rb_request_quiesce(); }

int nes_netplay_quiesced(void) { return nes_netplay_active() && nes_netplay_rb_quiesced(); }
int nes_netplay_draining(void) { return nes_netplay_active() && nes_netplay_rb_draining(); }

void nes_netplay_log_summary(void)
{
    if (!g_np.active) return;
    nes_netplay_rb_log_summary();
}
