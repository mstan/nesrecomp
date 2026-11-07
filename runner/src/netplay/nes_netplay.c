/*
 * nes_netplay.c -- NES session facade over recomp-net. See nes_netplay.h and
 * docs/NETPLAY.md. Shape follows snesrecomp runner/src/netplay/snes_netplay.c
 * (origin/main 8a06b70); what is NES-specific is the pad layout (versioned two bytes per
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
#ifdef NESRECOMP_CYCLE_NET
#include "cyc_net.h"
#include "cyc_core.h"
#include "cyc_recomp.h"
static const uint8_t *np_sram(size_t *len) { return cyc_net_sram(len); }
static int np_sram_load(const void *p,size_t len) { return cyc_net_sram_load(p,len); }
static int np_sram_active(void) { return cyc_nvram_size(0)||cyc_nvram_size(1); }
#else
#include "nes_runtime.h"
#include "save_ram.h"
static const uint8_t *np_sram(size_t *len) { *len=sizeof(g_sram);return g_sram; }
static int np_sram_load(const void *p,size_t len) {
    if(len!=sizeof(g_sram))return 0;memcpy(g_sram,p,len);save_ram_sync_snapshot();return 1;
}
static int np_sram_active(void) { return save_ram_active(); }
#endif
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
    NesNetplayInput staged;
    int      admitted;          /* a tick was admitted and has not finished */
    int      sram_needed, sram_done, sram_sent;
    int      guest_sandbox;
    int      return_requested;
    char     return_why[96];
    char     last_error[96];
    char     session_text[512];
    char     expected_reference[64];
    uint8_t  local_caps[96],caps[4][96];
    uint32_t caps_seen;
    int caps_started,caps_sent,boot_sent;
    uint8_t *boot_blob;
    uint32_t start_ms;
    uint32_t cur_tick;          /* the tick whose rows were last published */
} NetplayState;

/* Bounded QA source sampled at the driver's wire tick, before publication.
 * Format: tick HEX_NES_PAD HEX_GAME_BITS. No memory/scene commands. */
static struct {uint32_t tick;NesNetplayInput input;} s_test_input[4096];
static unsigned s_test_count;
static int test_input_load(void) {
    s_test_count=0;const char *path=getenv("NES_NET_TEST_INPUT");if(!path||!*path)return 1;
    FILE *f=fopen(path,"r");if(!f)return 0;char line[128];int ok=1;
    while(fgets(line,sizeof line,f)) {
        if(line[0]=='#'||line[0]=='\n'||line[0]=='\r')continue;
        unsigned tick,pad,game;char tail;
        if(sscanf(line,"%u %x %x %c",&tick,&pad,&game,&tail)!=3||pad>255||game>3||s_test_count==4096||
           (s_test_count&&tick<=s_test_input[s_test_count-1].tick)){ok=0;break;}
        s_test_input[s_test_count].tick=tick;s_test_input[s_test_count++].input=nes_netplay_input((uint8_t)pad,game);
    }
    fclose(f);return ok;
}
static NesNetplayInput test_input_at(uint32_t tick,NesNetplayInput physical) {
    if(!s_test_count)return physical;
    unsigned lo=0,hi=s_test_count;while(lo<hi){unsigned mid=(lo+hi)/2;if(s_test_input[mid].tick<=tick)lo=mid+1;else hi=mid;}
    return lo?s_test_input[lo-1].input:0;
}
static NetplayState g_np;
static int s_require_caps,s_required_players,s_input_override=-1;
void nes_netplay_set_input_player(int player){if(player>=0&&g_np.cur_tick)return;s_input_override=player>=0&&player<4?player:-1;if(g_np.active&&s_input_override>=0)g_np.input_player=s_input_override;}
void nes_netplay_require_capabilities(int required){s_require_caps=required!=0;}
void nes_netplay_require_player_count(int required){s_required_players=required;}
static const char *core_contract(void) {
#ifdef NESRECOMP_CYCLE_NET
    return "cycle-lle-c1";
#else
    return "legacy-c1";
#endif
}
static int contract_get(char *out,int cap){return snprintf(out,cap,"2:03ff:%s",core_contract());}
static int contract_apply(const char *v){char mine[64];contract_get(mine,sizeof mine);return !strcmp(v,mine);}
void nes_netplay_register_input_contract(void){nes_netplay_session_register("input",contract_get,contract_apply,NULL);}

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
    NesNetplayInput input=st->spectator?0:test_input_at(tick,st->staged);
    out->bytes[0]=(uint8_t)input;out->bytes[1]=(uint8_t)(input>>8);
    out->valid = 1;
}

static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots,
                         void *ctx)
{
    /* Unused: the rollback driver resolves and publishes rows itself. */
    (void)tick; (void)by_slot; (void)slots; (void)ctx;
}

/* ── rollback bindings ─────────────────────────────────────────────────── */

static void np_rb_publish(uint32_t tick, const NesNetplayInput *rows, int slots, int replay)
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
    if(!test_input_load()){np_set_error("test_input_invalid");return -1;}
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

    /* The lobby carries a full SHA-256 reference. The reliable preboot blob
     * carries the full image, so source identities never get truncated. */
    {
        char why[160],offer[512],ref[64];
        const char *text=cfg->session_config;
        int host=!cfg->spectator && cfg->local_slot==0;
        if(!strncmp(text,"sha256:",7)) {
            if(strlen(text)!=50){np_set_error("session_reference_invalid");return -6;}
            snprintf(g_np.expected_reference,sizeof g_np.expected_reference,"%s",text);text="";
        } else if(*text)nes_netplay_session_reference(text,g_np.expected_reference);
        if(host) {
            if(!*text){int n=nes_netplay_session_describe_offer(offer,sizeof offer);if(n<=0||n>=sizeof offer){np_set_error("session_config_size");return -6;}text=offer;}
            if(!nes_netplay_session_apply(text,why,sizeof why)) {
                fprintf(stderr,"nes_netplay: cannot apply session: %s\n",why);np_set_error("session_config_refused");return -6;
            }
            int n=nes_netplay_session_describe(g_np.session_text,sizeof g_np.session_text);
            if(n<=0||n>=sizeof g_np.session_text){np_set_error("session_config_size");return -6;}
            nes_netplay_session_reference(g_np.session_text,ref);
            if(g_np.expected_reference[0]&&strcmp(ref,g_np.expected_reference)){np_set_error("session_reference_changed");return -6;}
        }
    }

    if(s_required_players && (seats!=s_required_players || (cfg->occupied_mask && cfg->occupied_mask!=((1u<<seats)-1u)))){np_set_error("profile_player_count");return -6;}
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
    g_np.input_player = s_input_override>=0?s_input_override:cfg->input_player;
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
#ifndef NESRECOMP_CYCLE_NET
        save_ram_set_sandbox("netplay");
#endif
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

/* Bootstrap records use explicit little-endian fields. The existing opaque
 * guest-upload operation carries NESCAP2, never memory-card or artwork bytes.
 * The host's reliable SRAM operation carries NESBOOT2 and confirms all seats. */
static void put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned portable_profile(void) {
#ifdef NESRECOMP_CYCLE_NET
    return !strcmp(cyc_ppu_implementation(),"LLE")?1u:0u;
#else
    return 2u;
#endif
}
static void make_caps(uint8_t out[96]) {
    memset(out,0,96);memcpy(out,"NESCAP2",7);
    uint32_t arch=0,features=0,backend=0;
#if defined(_M_X64)||defined(__x86_64__)
    arch=2;
#elif defined(_M_IX86)||defined(__i386__)
    arch=1;
#elif defined(_M_ARM64)||defined(__aarch64__)
    arch=4;
#elif defined(_M_ARM)||defined(__arm__)
    arch=3;
#endif
    features=(SDL_HasSSE2()?1u:0u)|(SDL_HasSSE3()?2u:0u)|(SDL_HasSSE41()?4u:0u)|
        (SDL_HasSSE42()?8u:0u)|(SDL_HasAVX()?16u:0u)|(SDL_HasAVX2()?32u:0u)|(SDL_HasNEON()?64u:0u);
#ifdef NESRECOMP_CYCLE_NET
    backend=1u|(cyc_native_prg_hash?2u:0u);
#else
    backend=4u;
#endif
    put32(out+8,arch);put32(out+12,features);put32(out+16,backend);put32(out+20,portable_profile());
    put32(out+24,1);put32(out+28,(uint32_t)SDL_GetCPUCount());
    memcpy(out+32,nes_netplay_identity_build_sha256(),32);
    const char *hex=nes_netplay_identity_rom_sha256();
    for(int i=0;i<32 && strlen(hex)==64;i++){unsigned v=0;sscanf(hex+i*2,"%2x",&v);out[64+i]=(uint8_t)v;}
}
static int accept_caps(const uint8_t *p,size_t n) {
    if(n!=96||memcmp(p,"NESCAP2",8)||get32(p+24)!=1||!portable_profile()||
       !(get32(p+20)&portable_profile())||memcmp(p+32,g_np.local_caps+32,64))return 0;
    uint8_t any=0;for(int i=32;i<64;i++)any|=p[i];return any!=0;
}
static void log_caps(int seat,const uint8_t *p) {
    fprintf(stderr,"NET_HOST_CAPS seat=%d compiled_arch=%u cpu_features=%08x backends=%x qualified_profiles=%x cores=%u\n",
        seat,get32(p+8),get32(p+12),get32(p+16),get32(p+20),get32(p+28));
}
static int bootstrap_apply(const uint8_t *p,size_t n) {
    if(n<448||memcmp(p,"NESBOOT2",8)||get32(p+8)!=NES_NETPLAY_INPUT_VERSION||
       get32(p+12)!=(uint32_t)g_np.slot_count||get32(p+20)!=portable_profile())return 0;
    uint32_t mask=g_np.occupied_mask?g_np.occupied_mask:(1u<<g_np.slot_count)-1u;
    if(get32(p+16)!=mask)return 0;
    uint32_t config_len=get32(p+24),storage_len=get32(p+28);
    if(config_len<2||config_len>sizeof g_np.session_text||n!=448u+config_len+storage_len||p[448+config_len-1]||
       memchr(p+448,0,config_len-1))return 0;
    uint8_t hash[32];nes_sha256(p+448,config_len-1,hash);if(memcmp(hash,p+32,32))return 0;
    for(int seat=0;seat<g_np.slot_count;seat++)if(mask&(1u<<seat)) {
        if(!accept_caps(p+64+96*seat,96))return 0;log_caps(seat,p+64+96*seat);
    }
    char ref[64],why[160];nes_netplay_session_reference((const char *)p+448,ref);
    if(g_np.expected_reference[0]&&strcmp(ref,g_np.expected_reference))return 0;
    if(!nes_netplay_session_apply((const char *)p+448,why,sizeof why)) {
        fprintf(stderr,"nes_netplay: host configuration unavailable: %s\n",why);return 0;
    }
    if(s_required_players && (g_np.slot_count!=s_required_players || mask!=((1u<<g_np.slot_count)-1u))){np_set_error("profile_player_count");return 0;}
    if(g_np.spectator&&s_require_caps){np_set_error("capabilities_spectator_unqualified");return 0;}
    int len=nes_netplay_session_describe(g_np.session_text,sizeof g_np.session_text);
    if(len<=0||len>=sizeof g_np.session_text||strcmp(g_np.session_text,(const char *)p+448))return 0;
#if NESRECOMP_ENABLE_MODS
    extern const char *nes_mod_local_only_reason(void);
    const char *local=nes_mod_local_only_reason();if(local){fprintf(stderr,"nes_netplay: local-only feature: %s\n",local);return 0;}
#endif
    if(storage_len&&!np_sram_load(p+448+config_len,storage_len))return 0;
    fprintf(stderr,"NET_PROFILE_SEALED profile=%s revision=1 input=2 reference=%s exact_build=1\n",core_contract(),ref);
    return 1;
}
static int np_sram_barrier_step(void) {
    const void *data=NULL;size_t size=0;rnet_u8 op=0,slot=0;int from=-1;
    uint32_t mask=g_np.occupied_mask?g_np.occupied_mask:(1u<<g_np.slot_count)-1u;
    if(g_np.sram_done)return 1;
    if(!g_np.caps_started) {
        make_caps(g_np.local_caps);g_np.caps_started=1;
        if(!portable_profile()||!accept_caps(g_np.local_caps,96)){np_set_error("host_capability_unqualified");return -1;}
        if(!g_np.spectator){memcpy(g_np.caps[g_np.local_slot],g_np.local_caps,96);g_np.caps_seen|=1u<<g_np.local_slot;}
    }
    if(g_np.local_slot!=0 && !g_np.spectator && !g_np.caps_sent) {
        if(rnet_session_state_begin(g_np.session,RNET_STATE_OP_MEMCARD,0,g_np.local_caps,96)==0)g_np.caps_sent=1;
    }
    while(rnet_session_state_take_ready_from(g_np.session,&from,&op,&slot,&data,&size)) {
        if(op==RNET_STATE_OP_MEMCARD) {
            if(from!=g_np.local_slot) {
                if(g_np.local_slot!=0||from<0||from>=g_np.slot_count||!(mask&(1u<<from))||!accept_caps(data,size)) {
                    np_set_error("peer_capability_mismatch");return -1;
                }
                memcpy(g_np.caps[from],data,96);g_np.caps_seen|=1u<<from;
            }
            rnet_session_state_finish_from(g_np.session,from,0);
        } else if(op==RNET_STATE_OP_SRAM) {
            if(!bootstrap_apply(data,size)){if(!g_np.last_error[0])np_set_error("bootstrap_profile_mismatch");return -1;}
            rnet_session_state_finish_from(g_np.session,from,0);g_np.sram_done=1;return 1;
        } else {np_set_error("unexpected_bootstrap_blob");return -1;}
    }
    if(g_np.local_slot==0 && (g_np.caps_seen&mask)==mask && !g_np.boot_sent) {
        size_t storage_len=0;const uint8_t *storage=g_np.sram_needed?np_sram(&storage_len):NULL;
        size_t config_len=strlen(g_np.session_text)+1,total=448+config_len+storage_len;
        if((g_np.sram_needed&&!storage)||config_len>512){np_set_error("bootstrap_storage");return -1;}
        free(g_np.boot_blob);g_np.boot_blob=(uint8_t *)calloc(1,total);if(!g_np.boot_blob)return -1;
        uint8_t *p=g_np.boot_blob;memcpy(p,"NESBOOT2",8);put32(p+8,NES_NETPLAY_INPUT_VERSION);put32(p+12,g_np.slot_count);
        put32(p+16,mask);put32(p+20,portable_profile());put32(p+24,(uint32_t)config_len);put32(p+28,(uint32_t)storage_len);
        nes_sha256((const uint8_t *)g_np.session_text,config_len-1,p+32);memcpy(p+64,g_np.caps,sizeof g_np.caps);
        memcpy(p+448,g_np.session_text,config_len);if(storage_len)memcpy(p+448+config_len,storage,storage_len);
        if(!bootstrap_apply(p,total)){np_set_error("host_profile_refused");return -1;}
        if(rnet_session_state_begin(g_np.session,RNET_STATE_OP_SRAM,0,p,total)!=0)return 0;
        g_np.boot_sent=1;
    }
    return 0;
}

int nes_netplay_boot_barrier(void)
{
    uint32_t t0 = SDL_GetTicks();
    const uint32_t timeout = (uint32_t)env_int("NES_NET_CONNECT_TIMEOUT_MS", 30000);
    if (!nes_netplay_active()) return 0;
    if (g_np.sram_needed < 0)
        g_np.sram_needed = np_sram_active() ? 1 : 0;
    /* Capability/config settlement is mandatory even without cartridge SRAM. */
    for (;;) {
        SDL_Event ev;
        np_pump();
        if (rnet_session_is_running(g_np.session)) {
            int r = np_sram_barrier_step();
            if (r < 0) return -1;
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
#ifndef NESRECOMP_CYCLE_NET
    if (g_np.guest_sandbox) save_ram_set_sandbox(NULL);
#endif
    nes_netplay_session_restore();
    free(g_np.boot_blob);g_np.boot_blob=NULL;
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

void nes_netplay_stage_local(NesNetplayInput pad)
{
    g_np.staged = pad & NES_NETPLAY_INPUT_MASK;
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
