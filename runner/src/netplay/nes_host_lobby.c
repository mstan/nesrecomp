/*
 * nes_host_lobby.c -- the NES adapter over recomp-ui's shared netplay backend
 * (recomp_netplay_host.h). Shape copied from snesrecomp origin/main
 * runner/src/netplay/snes_host_lobby.c: the backend owns create / join / list
 * / chat / seats / LAN / fill_launch over recomp-net's lobby client; what is
 * left here is what only the NES host knows:
 *
 *   - its identity (game_version = version + exe hash, content_fingerprint =
 *     ROM SHA-256; nes_netplay_identity.c),
 *   - the session configuration seal: the host's canonical session text rides
 *     the room's match_caps as "nes_session" and every peer applies it at
 *     launch (nes_netplay_session_*),
 *   - the persisted display name (config.ini [Netplay]),
 *   - the rollback engine's fork report.
 *
 * Replaces nes_launcher_netplay.c + lobby/nes_lobby_client.c (a 1.3k-line
 * private lobby client and a private WebSocket copy), which no longer
 * compiled against recomp-ui (cb_create lacked max_slots).
 *
 * Seats are SEAT-mapped (RECOMP_NETPLAY_SLOTS_SEAT): session slot == lobby
 * seat == controller port / character, so a player who moves seats changes
 * characters. The launch still carries occupied_mask.
 */
#include "nes_host_lobby.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "config.h"
#include "logical_input.h"
#include "nes_netplay.h"
#include "nes_netplay_identity.h"
#include "nes_netplay_rb.h"
#include "recomp_netplay_host.h"

#ifndef NESRECOMP_GAME_PLAYERS
#define NESRECOMP_GAME_PLAYERS 2
#endif

void nes_runner_register_session_keys(void);   /* main_runner.c */

static int s_inited;
static char s_game_name[96];

/* ---- match caps: the session configuration image ------------------------
 * ext holds the wire text without its header line (64 bytes). A text that
 * does not fit withholds the whole caps object rather than publishing a
 * truncated configuration. */
#define SESSION_HEADER_WIRE "nes-session/1;"

static int caps_write(const RNetLobbyMatchCaps *caps, char *out, size_t cap, void *ctx)
{
    char esc[160];
    const char *body = (const char *)caps->ext.bytes;
    int n;
    (void)ctx;
    if (!body[0]) return 0;
    if (!memchr(caps->ext.bytes, 0, sizeof(caps->ext.bytes))) return -1;
    rnet_lobby_json_escape(body, esc, sizeof(esc));
    n = snprintf(out, cap, ",\"nes_session\":\"%s\"", esc);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static void caps_parse(const char *caps_json, RNetLobbyMatchCaps *caps, void *ctx)
{
    char v[RNET_LOBBY_CAPS_EXT_BYTES];
    (void)ctx;
    if (rnet_lobby_json_get_str(caps_json, "nes_session", v, sizeof(v)))
        snprintf((char *)caps->ext.bytes, sizeof(caps->ext.bytes), "%s", v);
}

static const RNetLobbyCapsCodec s_codec = { caps_write, caps_parse, NULL };

/* Host: publish the configuration this peer will run. */
static void fill_caps(void *ctx, const RecompLauncherCSettings *settings,
                      RNetLobbyMatchCaps *caps)
{
    char text[512], wire[512];
    const char *body;
    (void)ctx;
    (void)settings;
    nes_netplay_session_describe_offer(text, (int)sizeof(text));
    nes_netplay_session_to_wire(text, wire, (int)sizeof(wire));
    body = wire;
    if (!strncmp(body, SESSION_HEADER_WIRE, strlen(SESSION_HEADER_WIRE)))
        body += strlen(SESSION_HEADER_WIRE);
    memset(caps->ext.bytes, 0, sizeof(caps->ext.bytes));
    if (strlen(body) >= sizeof(caps->ext.bytes)) {
        fprintf(stderr, "nes_lobby: session configuration '%s' exceeds the caps "
                        "field (%u bytes) — the room publishes none\n",
                body, (unsigned)sizeof(caps->ext.bytes) - 1u);
        /* No NUL anywhere: caps_write refuses and the caps are withheld. */
        memset(caps->ext.bytes, 'x', sizeof(caps->ext.bytes));
        return;
    }
    snprintf((char *)caps->ext.bytes, sizeof(caps->ext.bytes), "%s", body);
}

static int exe_dir_path(void *ctx, const char *leaf, char *out, size_t cap)
{
    char dir[1024];
    (void)ctx;
    nesrecomp_exe_dir(dir, sizeof(dir));
    return snprintf(out, cap, "%s%s", dir, leaf) < (int)cap;
}

static int name_store(void *ctx, const char *name)
{
    (void)ctx;
    snprintf(g_nes_config.netplay_player_name, sizeof(g_nes_config.netplay_player_name),
             "%s", name && name[0] ? name : "Player");
    return 1;
}

static int name_load(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    if (!g_nes_config.netplay_player_name[0]) return 0;
    snprintf(out, cap, "%s", g_nes_config.netplay_player_name);
    return 1;
}

static int last_fork(void *ctx, uint32_t *tick, const char **partition,
                     uint32_t *mine, uint32_t *theirs)
{
    (void)ctx;
    if (!nes_netplay_rb_last_fork(tick, partition))
        return 0;
    nes_netplay_rb_fork_digests(mine, theirs);
    return 1;
}

const RecompLauncherCNetplayCallbacks *nes_host_lobby_init(const char *game_name,
                                                           const char *rom_path)
{
    RecompNetplayHostHooks h;
    if (rom_path && rom_path[0])
        (void)nes_netplay_identity_set_rom_file(rom_path);
    nes_runner_register_session_keys();
    if (s_inited)
        return recomp_netplay_host_callbacks();
    snprintf(s_game_name, sizeof(s_game_name), "%s",
             game_name && game_name[0] ? game_name : "NES Game");
    rnet_lobby_set_caps_codec(&s_codec);
    memset(&h, 0, sizeof(h));
    h.game_name = s_game_name;
    h.game_version = nes_netplay_identity_game_version();
    h.content_fingerprint = nes_netplay_identity_rom_sha256()[0]
                                ? nes_netplay_identity_rom_sha256() : NULL;
    h.default_lobby_name = "NES Netplay";
    h.lan_registry_path = "netplay_lan_lobby.txt";
    h.platform = "nes";
    h.legacy_env_prefix = "NES_NET_";
    h.max_players = NESRECOMP_GAME_PLAYERS < 2 ? 2
                  : (NESRECOMP_GAME_PLAYERS > NESRECOMP_INPUT_SEATS
                         ? NESRECOMP_INPUT_SEATS : NESRECOMP_GAME_PLAYERS);
    h.slot_policy = RECOMP_NETPLAY_SLOTS_SEAT;
    h.input_player = 0;
    h.exe_dir_path = exe_dir_path;
    h.name_store = name_store;
    h.name_load = name_load;
    h.caps_codec = &s_codec;
    h.fill_match_caps = fill_caps;
    h.last_fork = last_fork;
    h.rematch_set_ready = 0;
    /* No mod hooks: an NES netplay launch is vanilla (the mod runtime's
     * netplay commit), and what a match may vary is exactly the session
     * configuration above -- a room whose host requires mods is refused
     * (last_error "mods_unsupported") rather than started as a desync. */
    h.mods = NULL;
    if (recomp_netplay_host_init(&h) != 0)
        return NULL;
    s_inited = 1;
    fprintf(stderr, "nes_lobby: identity game=\"%s\" version=%s rom_sha256=%s max_players=%d\n",
            s_game_name, h.game_version, h.content_fingerprint ? h.content_fingerprint : "(unknown)",
            h.max_players);
    return recomp_netplay_host_callbacks();
}

int nes_host_lobby_config_from_launch(const RecompLauncherCNetplayLaunch *l,
                                      NesNetplayConfig *c)
{
    const RNetLobbyMatchCaps *caps;
    if (!l || !l->enabled || !c) return 0;
    nes_netplay_config_defaults(c);
    c->enabled = 1;
    c->local_slot = l->local_slot;
    c->slot_count = l->player_count > 0 ? l->player_count : (l->max_slots > 0 ? l->max_slots : 2);
    c->occupied_mask = l->occupied_mask;
    c->spectator = l->is_spectator;
    c->spectator_wire_slot = l->spectator_wire_slot;
    c->input_player = l->input_player < 0 ? 0 : l->input_player;
    c->input_delay = l->input_delay;
    c->input_prediction = l->input_prediction;
    /* Every NES match runs the rollback driver; a room that settled delay
     * (a LAN room settles no mode) pins prediction off instead. */
    c->rollback = 1;
    c->session_id = l->session_id;
    c->force_turn = l->force_turn;
    c->force_input_relay = l->force_input_relay;
    c->transport = 2;
    snprintf(c->bind_hostport, sizeof(c->bind_hostport), "%s", l->bind_hostport);
    snprintf(c->peer_hostport, sizeof(c->peer_hostport), "%s", l->peer_hostport);
    caps = rnet_lobby_match_caps();
    if (caps && caps->valid && caps->ext.bytes[0] &&
        memchr(caps->ext.bytes, 0, sizeof(caps->ext.bytes))) {
        char wire[160];
        snprintf(wire, sizeof(wire), "%s%s", SESSION_HEADER_WIRE, (const char *)caps->ext.bytes);
        nes_netplay_session_from_wire(wire, c->session_config, (int)sizeof(c->session_config));
    } else if (!l->force_input_relay && l->local_slot != 0) {
        /* LAN rooms carry no caps: every peer runs its OWN configuration and
         * the in-band mod-set handshake refuses the match if they differ. */
    }
    return 1;
}

void nes_host_lobby_returned(RecompLauncherCGameInfo *gi, const char *error_code)
{
    if (!s_inited) return;
    if (error_code && error_code[0])
        recomp_netplay_host_set_runtime_error(error_code);
    if (gi) recomp_netplay_host_begin_soft_return(gi, 1);
    else recomp_netplay_host_prepare_rematch();
}

void nes_host_lobby_shutdown(void)
{
    if (!s_inited) return;
    recomp_netplay_host_shutdown();
    s_inited = 0;
}

/* ---- headless room (NES_LOBBY_SELFTEST) --------------------------------- */

static uint32_t st_now(void) { return SDL_GetTicks(); }

int nes_host_lobby_selftest_role(void)
{
    const char *r = getenv("NES_LOBBY_SELFTEST");
    if (!r) return 0;
    if (!strcmp(r, "host")) return 1;
    if (!strcmp(r, "guest")) return 2;
    return 0;
}

static int count_members(int spectators)
{
    int i, n = 0, m = rnet_lobby_member_count();
    for (i = 0; i < m; ++i) {
        RNetLobbyMember mem;
        if (rnet_lobby_member_get(i, &mem) && (mem.is_spectator ? 1 : 0) == spectators)
            n++;
    }
    return n;
}
static int players_seated(void)
{
    /* LAN rooms live in recomp-ui's rnet_lan_* modules, not the lobby
     * client's member table: count them through the callback table. */
    if (getenv("NES_LOBBY_SELFTEST_LAN")) {
        const RecompLauncherCNetplayCallbacks *cb = recomp_netplay_host_callbacks();
        return cb ? cb->member_count(NULL) : 0;
    }
    return count_members(0);
}
static int spectators_seated(void) { return count_members(1); }

int nes_host_lobby_selftest_room(int round, NesNetplayConfig *cfg)
{
    const RecompLauncherCNetplayCallbacks *cb = recomp_netplay_host_callbacks();
    const int role = nes_host_lobby_selftest_role();
    const int is_host = role == 1;
    const char *lobby = getenv("NES_LOBBY_SELFTEST_LOBBY");
    const char *name = getenv("NES_LOBBY_SELFTEST_NAME");
    const char *seats_env = getenv("NES_LOBBY_SELFTEST_SEATS");
    const char *url = getenv("RNET_LOBBY_URL");
    const int seats = seats_env ? atoi(seats_env) : 2;
    RecompLauncherCSettings settings;
    RecompLauncherCNetplayLaunch launch;
    uint32_t deadline = st_now() + 90000u, next_list = 0;
    int joined = is_host, ready_sent = 0, started = 0;
    const int spectate = getenv("NES_LOBBY_SELFTEST_SPECTATE") != NULL;
    const char *ws = getenv("NES_LOBBY_SELFTEST_EXPECT_SPECTATORS");
    const int want_spectators = ws ? atoi(ws) : 0;
    uint32_t next_move = 0;

    if (!cb || !role) return -1;
    if (!lobby || !lobby[0]) lobby = "nes-selftest";
    memset(&settings, 0, sizeof(settings));
    memset(&launch, 0, sizeof(launch));
    if (round == 1 && getenv("NES_LOBBY_SELFTEST_LAN")) {
        /* LAN / Direct IP room (recomp-ui's rnet_lan_* modules, no server):
         * two seats, the delay settled and nothing else. */
        const char *pe = getenv("NES_LOBBY_SELFTEST_LAN_PORT");
        int port = pe ? atoi(pe) : 17790, rc = -1;
        cb->set_player_name(NULL, name && name[0] ? name : (is_host ? "HostTest" : "GuestTest"));
        if (is_host) {
            char ep[64];
            snprintf(ep, sizeof(ep), "127.0.0.1:%d", port);
            rc = cb->create(NULL, lobby, ep, "", &settings, 1, 2);
        } else {
            char gb[64], id[80];
            snprintf(gb, sizeof(gb), "0.0.0.0:%d", port + 1);
            snprintf(id, sizeof(id), "lan:127.0.0.1:%d", port);
            while (st_now() < deadline) {   /* the host may not be listening yet */
                rc = cb->join(NULL, id, "", gb);
                if (rc == 0) break;
                SDL_Delay(250);
            }
        }
        fprintf(stderr, "[lobby-selftest] %s round=1 lan %s rc=%d\n",
                is_host ? "host" : "guest", is_host ? "create" : "join", rc);
        if (rc != 0) return -5;
        joined = 1;
    } else if (round == 1) {
        cb->set_player_name(NULL, name && name[0] ? name : (is_host ? "HostTest" : "GuestTest"));
        if (url && url[0]) cb->set_lobby_url(NULL, url);
        if (cb->connect(NULL) != 0) {
            fprintf(stderr, "[lobby-selftest] connect failed\n");
            return -2;
        }
        while (!rnet_lobby_player_id()[0]) {
            cb->pump(NULL);
            if (st_now() > deadline) return -3;
            SDL_Delay(10);
        }
        if (is_host) {
            char ep[64] = "0.0.0.0:7777";
            if (getenv("NES_LOBBY_SELFTEST_ALLOW_SPECTATORS"))
                rnet_lobby_set_allow_spectators(1);
            if (cb->create(NULL, lobby, ep, "", &settings, 0, seats) != 0) {
                fprintf(stderr, "[lobby-selftest] create failed\n");
                return -5;
            }
        }
        joined = is_host;
    }
    while (st_now() < deadline) {
        cb->pump(NULL);
        if (!joined && st_now() >= next_list) {
            int i, n;
            cb->request_list(NULL);
            next_list = st_now() + 400u;
            n = cb->list_count(NULL);
            for (i = 0; i < n; ++i) {
                RecompLauncherCNetplayLobby row;
                if (cb->list_get(NULL, i, &row) && !strcmp(row.name, lobby)) {
                    char gb[64] = "";
                    if (cb->join(NULL, row.lobby_id, "", gb) == 0) {
                        joined = 1;
                        fprintf(stderr, "[lobby-selftest] guest joined %s\n", row.lobby_id);
                    }
                    break;
                }
            }
        }
        if (spectate && joined && cb->in_lobby(NULL) && !rnet_lobby_local_is_spectator() &&
            st_now() >= next_move) {
            /* Take a gallery seat: simulate the match, contribute nothing. */
            int g = rnet_lobby_spectator_slot(0);
            next_move = st_now() + 500u;
            if (g >= 0) (void)rnet_lobby_seat_move_self(g);
        }
        if (!spectate && joined && cb->in_lobby(NULL) && !ready_sent &&
            (!is_host || players_seated() >= seats)) {
            ready_sent = cb->set_ready(NULL, 1) == 0;
        }
        if (is_host && ready_sent && !started && players_seated() >= seats &&
            spectators_seated() >= want_spectators && cb->all_ready(NULL)) {
            started = cb->request_start(NULL, &settings) == 0;
            fprintf(stderr, "[lobby-selftest] host round=%d start=%d members=%d\n",
                    round, started, cb->member_count(NULL));
        }
        {
            static uint32_t next_dbg;
            if (st_now() >= next_dbg) {
                next_dbg = st_now() + 2000u;
                fprintf(stderr, "[lobby-selftest] %s round=%d in_lobby=%d members=%d players=%d "
                                "spectators=%d local_ready=%d all_ready=%d ready_sent=%d last_error=%s\n",
                        is_host ? "host" : "guest", round, cb->in_lobby(NULL),
                        cb->member_count(NULL), players_seated(), spectators_seated(),
                        cb->local_ready(NULL), cb->all_ready(NULL), ready_sent,
                        cb->last_error && cb->last_error(NULL) ? cb->last_error(NULL) : "");
            }
        }
        if (cb->launch_pending(NULL)) {
            int ok = cb->fill_launch(NULL, &launch);
            fprintf(stderr, "[lobby-selftest] %s round=%d fill_launch=%d slot=%d players=%d "
                            "session=%u relay=%d bind=%s peer=%s\n",
                    is_host ? "host" : "guest", round, ok, launch.local_slot,
                    launch.player_count, (unsigned)launch.session_id,
                    launch.force_input_relay, launch.bind_hostport, launch.peer_hostport);
            if (!ok) return -11;
            cb->clear_launch_pending(NULL);
            return nes_host_lobby_config_from_launch(&launch, cfg) ? 0 : -12;
        }
        SDL_Delay(10);
    }
    fprintf(stderr, "[lobby-selftest] round=%d TIMED OUT waiting for a launch\n", round);
    return -10;
}

void nes_host_lobby_selftest_report(int round)
{
    const RecompLauncherCNetplayCallbacks *cb = recomp_netplay_host_callbacks();
    const char *e;
    int i;
    if (!cb) return;
    for (i = 0; i < 20; ++i) { cb->pump(NULL); SDL_Delay(10); }
    e = cb->last_error ? cb->last_error(NULL) : NULL;
    fprintf(stderr, "[lobby-selftest] %s after match %d back in the room: in_lobby=%d "
                    "is_host=%d members=%d last_error=\"%s\"\n",
            nes_host_lobby_selftest_role() == 1 ? "host" : "guest", round,
            cb->in_lobby(NULL), cb->is_host(NULL), cb->member_count(NULL), e ? e : "");
}

void nes_host_lobby_selftest_linger(unsigned ms)
{
    const RecompLauncherCNetplayCallbacks *cb = recomp_netplay_host_callbacks();
    uint32_t end = st_now() + ms;
    if (!cb) return;
    while (st_now() < end) { cb->pump(NULL); SDL_Delay(10); }
}
