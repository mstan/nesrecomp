#ifndef NES_LOBBY_CLIENT_H
#define NES_LOBBY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NES_LOBBY_ID_LEN 40
#define NES_LOBBY_NAME_LEN 64
#define NES_LOBBY_VERSION_LEN 32
#define NES_LOBBY_ENDPOINT_LEN 64
#define NES_LOBBY_MAX_LIST 32
#define NES_LOBBY_MAX_MEMBERS 4

#ifndef NES_GAME_VERSION
#define NES_GAME_VERSION "dev"
#endif

typedef struct NesLobbyRow {
    char     lobby_id[NES_LOBBY_ID_LEN];
    char     name[NES_LOBBY_NAME_LEN];
    char     game_name[NES_LOBBY_NAME_LEN];
    char     game_version[NES_LOBBY_VERSION_LEN];
    int      player_count;
    int      max_slots;
    int      has_password;
} NesLobbyRow;

typedef struct NesLobbyMember {
    int  slot;
    char player_id[NES_LOBBY_ID_LEN];
    char display_name[NES_LOBBY_NAME_LEN];
    int  ready;
} NesLobbyMember;

/*
 * Host-authoritative sim settings negotiated over the lobby.
 * Guests apply these on launch so both peers boot with matching caps.
 */
typedef struct NesLobbyMatchCaps {
    int  valid;            /* 1 when a host blob was received / set */
    int  widescreen;       /* 0/1 */
    int  widescreen_hud;   /* 0/1 */
    int  ignore_aspect;    /* 0/1 */
    int  input_delay;      /* recomp-net delay frames (0-16, default 2) */
    int  ws_extra;         /* widescreen margin; 0 = game default / env force */
} NesLobbyMatchCaps;

typedef struct NesLobbyJoinInfo {
    int      ok;
    char     lobby_id[NES_LOBBY_ID_LEN];
    uint32_t session_id;
    int      local_slot;
    char     host_endpoint[NES_LOBBY_ENDPOINT_LEN];
    char     guest_endpoint[NES_LOBBY_ENDPOINT_LEN];
    char     bind_hostport[NES_LOBBY_ENDPOINT_LEN];
    char     peer_hostport[NES_LOBBY_ENDPOINT_LEN];
    int      player_count;
    int      max_slots;
    char     last_error[64]; /* need_password | bad_password | â€¦ */
} NesLobbyJoinInfo;

/* Default URL when NES_NET_LOBBY_URL unset:
 * ws://netplay.technicallycomputers.ca:8765 */
const char *nes_lobby_default_url(void);

int  nes_lobby_connect(const char *ws_url); /* 0 ok */
void nes_lobby_disconnect(void);
int  nes_lobby_connected(void);

void nes_lobby_set_display_name(const char *name);
const char *nes_lobby_display_name(void);
const char *nes_lobby_player_id(void);

/* Non-blocking pump â€” call every frame from the launcher. */
void nes_lobby_pump(void);

/* Title + release pin used for create/join matching and list filters. */
void nes_lobby_set_game_identity(const char *game_name,
                                  const char *game_version);
const char *nes_lobby_game_version(void);

void nes_lobby_request_list(void);
int  nes_lobby_list_count(void);
int  nes_lobby_list_get(int index, NesLobbyRow *out);

/*
 * Create lobby. host_bind e.g. "0.0.0.0:7777". password may be NULL/empty.
 * match_caps may be NULL (legacy); when non-NULL and valid, sent to the server
 * so guests join with the host's sim settings.
 * Returns 0 if request sent; poll nes_lobby_join_info() / in_lobby().
 */
int  nes_lobby_create(const char *name, const char *game_name,
                      const char *game_version, const char *password,
                      const char *host_bind,
                      const NesLobbyMatchCaps *match_caps);

int  nes_lobby_join(const char *lobby_id, const char *password,
                    const char *guest_bind);

int  nes_lobby_leave(void);

int  nes_lobby_in_lobby(void);
int  nes_lobby_is_host(void);
/* Filled after create/join/lobby_update; peer endpoints for PsxNetplayConfig. */
const NesLobbyJoinInfo *nes_lobby_join_info(void);

/* Latest host match_caps (valid==0 until create/join/launch delivers one). */
const NesLobbyMatchCaps *nes_lobby_match_caps(void);

/* Host: push updated caps while in lobby (clears ready via lobby_update). */
int  nes_lobby_set_match_caps(const NesLobbyMatchCaps *caps);

/* Live member table from lobby_update (and create/join). */
int  nes_lobby_member_count(void);
int  nes_lobby_member_get(int index, NesLobbyMember *out);

/* Local ready flag (from last lobby_update matching our player_id). */
int  nes_lobby_local_ready(void);
/* True when every seated player is ready and player_count >= 2. */
int  nes_lobby_all_ready(void);

/* Toggle ready in the current lobby. */
int  nes_lobby_set_ready(int ready);

/*
 * Host: ask server to broadcast launch. When match_caps is non-NULL and valid,
 * it is attached to start so launch freezes the latest host settings.
 */
int  nes_lobby_request_start(const NesLobbyMatchCaps *match_caps);

/*
 * Set when server sends op:launch. Both host and guests should boot netplay.
 * Cleared by nes_lobby_clear_launch_pending() after consuming.
 */
int  nes_lobby_launch_pending(void);
void nes_lobby_clear_launch_pending(void);

/*
 * ICE signaling relay (MotK WS op:signal). text is SDP/candidate (max 2047).
 * send returns 0 if queued/written; poll returns 1 when an inbound signal was
 * copied out (LOCAL_* types as emitted by the peer â€” remap to REMOTE_* before
 * rnet_session_push_signal).
 */
int  nes_lobby_send_signal(int type, int flag, const char *text);
int  nes_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap);

#ifdef __cplusplus
}
#endif

#endif /* NES_LOBBY_CLIENT_H */
