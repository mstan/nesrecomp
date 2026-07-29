#include "nes_launcher_netplay.h"

#ifdef RECOMP_LAUNCHER
#include "nes_lobby_client.h"
#include "nes_netplay.h"
#include "config.h"
#include "recomp_net/recomp_net.h"
#include <stdio.h>
#include <string.h>

#define MAX_LOCAL_ADDRESSES 32
static char s_game[64] = "NES Game";
static char s_version[32] = "dev";
static int s_hosting_lan, s_joined_lan;
static RecompLauncherCNetplayLaunch s_lan_launch;
static int s_last_launch_was_lan;
static RNetIpv4Address s_addresses[MAX_LOCAL_ADDRESSES];
static int s_address_count;
static char s_external_ip[RNET_IPV4_ADDRESS_TEXT_MAX];

static const char *lan_path(void) { return "netplay_lan_lobby.txt"; }
static int read_lan(RNetLanLobby *s) {
    return rnet_lan_lobby_read(lan_path(),s_game,s_version,s)==RNET_LAN_LOBBY_OK;
}
static NesLobbyMatchCaps caps(const RecompLauncherCSettings *s) {
    NesLobbyMatchCaps c;memset(&c,0,sizeof(c));c.valid=1;
    c.widescreen=s?s->widescreen!=0:0;c.input_delay=2;return c;
}
static int publish_lan(const char *name,const char *ep,const char *pw) {
    RNetLanLobby s;memset(&s,0,sizeof(s));
    snprintf(s.name,sizeof(s.name),"%s",name&&*name?name:"LAN Lobby");
    snprintf(s.game,sizeof(s.game),"%s",s_game);snprintf(s.game_version,sizeof(s.game_version),"%s",s_version);
    snprintf(s.endpoint,sizeof(s.endpoint),"%s",ep&&*ep?ep:"127.0.0.1:7777");
    snprintf(s.host_name,sizeof(s.host_name),"%s",nes_lobby_display_name()[0]?nes_lobby_display_name():"Host");
    snprintf(s.password,sizeof(s.password),"%s",pw?pw:"");s.host_slot=0;
    if(rnet_lan_lobby_publish(lan_path(),&s)!=RNET_LAN_LOBBY_OK)return 0;
    s_hosting_lan=1;s_joined_lan=0;memset(&s_lan_launch,0,sizeof(s_lan_launch));return 1;
}
static int lan_row(RecompLauncherCNetplayLobby *o) {
    RNetLanLobby s;if(!o||!read_lan(&s))return 0;memset(o,0,sizeof(*o));
    snprintf(o->lobby_id,sizeof(o->lobby_id),"lan:%s",s.endpoint);
    snprintf(o->name,sizeof(o->name),"LAN - %s",s.name[0]?s.name:"Lobby");
    snprintf(o->game_name,sizeof(o->game_name),"%s",s.game);snprintf(o->game_version,sizeof(o->game_version),"%s",s.game_version);
    o->player_count=s.joiner_name[0]?2:1;o->max_slots=2;o->has_password=s.password[0]!=0;return 1;
}
static int use_lan_members(RNetLanLobby *s) {
    if(!read_lan(s))return 0;if(s_joined_lan)return 1;
    return s_hosting_lan&&(s->joiner_name[0]||nes_lobby_member_count()<2);
}
static const char *cb_url(void *x){(void)x;return g_nes_config.netplay_lobby_url[0]?g_nes_config.netplay_lobby_url:nes_lobby_default_url();}
static void cb_set_url(void*x,const char*u){(void)x;snprintf(g_nes_config.netplay_lobby_url,sizeof(g_nes_config.netplay_lobby_url),"%s",u&&*u?u:nes_lobby_default_url());}
static int cb_connect(void*x){(void)x;nes_lobby_set_game_identity(s_game,s_version);return nes_lobby_connect(cb_url(0));}
static int cb_connected(void*x){(void)x;return nes_lobby_connected();}
static void cb_pump(void*x){(void)x;nes_lobby_pump();}
static void cb_set_name(void*x,const char*n){(void)x;snprintf(g_nes_config.netplay_player_name,sizeof(g_nes_config.netplay_player_name),"%s",n&&*n?n:"Player");nes_lobby_set_display_name(g_nes_config.netplay_player_name);}
static const char *cb_name(void*x){(void)x;return nes_lobby_display_name();}
static void cb_request(void*x){(void)x;nes_lobby_request_list();}
static int cb_count(void*x){RecompLauncherCNetplayLobby l;(void)x;return nes_lobby_list_count()+(lan_row(&l)?1:0);}
static int cb_get(void*x,int i,RecompLauncherCNetplayLobby*o){NesLobbyRow r;int n;(void)x;if(!o||i<0)return 0;n=nes_lobby_list_count();if(i>=n)return i==n?lan_row(o):0;if(!nes_lobby_list_get(i,&r))return 0;memset(o,0,sizeof(*o));snprintf(o->lobby_id,sizeof(o->lobby_id),"%s",r.lobby_id);snprintf(o->name,sizeof(o->name),"%s",r.name);snprintf(o->game_name,sizeof(o->game_name),"%s",r.game_name);snprintf(o->game_version,sizeof(o->game_version),"%s",r.game_version);o->player_count=r.player_count;o->max_slots=r.max_slots;o->has_password=r.has_password;return 1;}
static int cb_addr(void*x,int i,RecompLauncherCNetplayLocalAddress*o){(void)x;if(!o||i<0)return 0;if(i==0){s_address_count=rnet_ipv4_enumerate(s_addresses,MAX_LOCAL_ADDRESSES);if(s_address_count<0)s_address_count=0;if(s_address_count>MAX_LOCAL_ADDRESSES)s_address_count=MAX_LOCAL_ADDRESSES;}if(i>=s_address_count)return 0;memset(o,0,sizeof(*o));snprintf(o->address,sizeof(o->address),"%s",s_addresses[i].address);snprintf(o->label,sizeof(o->label),"%s",s_addresses[i].interface_label);return 1;}
static int cb_local_ip(void*x,char*o,size_t z){RecompLauncherCNetplayLocalAddress a;if(!o||!z||!cb_addr(x,0,&a))return 0;snprintf(o,z,"%s",a.address);return *o!=0;}
static int cb_external_ip(void*x,char*o,size_t z){RNetExternalIpv4Config c;int rc;(void)x;if(!o||!z)return 0;if(!s_external_ip[0]){rnet_external_ipv4_config_init(&c);c.timeout_ms=900;rc=rnet_external_ipv4_discover(&c,s_external_ip,sizeof(s_external_ip));if(rc!=RNET_EXTERNAL_IPV4_OK){snprintf(o,z,"Unavailable");return 0;}}snprintf(o,z,"%s",s_external_ip);return *o!=0;}
static int cb_create(void*x,const char*n,const char*ep,const char*pw,const RecompLauncherCSettings*s){NesLobbyMatchCaps c=caps(s);const char*e=ep&&*ep?ep:"0.0.0.0:7777";(void)x;if(!publish_lan(n,e,pw))return -1;return nes_lobby_create(n&&*n?n:"Netplay Lobby",s_game,s_version,pw?pw:"",e,&c);}
static int cb_join(void*x,const char*id,const char*pw){RNetLanLobby s;(void)x;memset(&s_lan_launch,0,sizeof(s_lan_launch));if(id&&strncmp(id,"lan:",4)==0){const char*n=nes_lobby_display_name();if(rnet_lan_lobby_join(lan_path(),s_game,s_version,pw?pw:"",n&&*n?n:"Player",&s)!=RNET_LAN_LOBBY_OK)return -1;s_hosting_lan=0;s_joined_lan=1;return 0;}s_hosting_lan=s_joined_lan=0;return nes_lobby_join(id,pw?pw:"","0.0.0.0:0");}
static int cb_leave(void*x){(void)x;if(s_hosting_lan)(void)rnet_lan_lobby_leave(lan_path(),1);else if(s_joined_lan)(void)rnet_lan_lobby_leave(lan_path(),0);s_hosting_lan=s_joined_lan=0;memset(&s_lan_launch,0,sizeof(s_lan_launch));return nes_lobby_leave();}
static int cb_in(void*x){(void)x;return s_hosting_lan||s_joined_lan||nes_lobby_in_lobby();}
static int cb_host(void*x){(void)x;return(s_hosting_lan||s_joined_lan)?s_hosting_lan:nes_lobby_is_host();}
static int cb_member_count(void*x){RNetLanLobby s;(void)x;return use_lan_members(&s)?2:nes_lobby_member_count();}
static int cb_member_get(void*x,int i,RecompLauncherCNetplayMember*o){RNetLanLobby s;NesLobbyMember m;(void)x;if(!o)return 0;memset(o,0,sizeof(*o));if(use_lan_members(&s)){if(i<0||i>1)return 0;o->slot=i?1-s.host_slot:s.host_slot;o->ready=i==0||s.joiner_name[0];o->is_host=i==0;snprintf(o->display_name,sizeof(o->display_name),"%s",i?s.joiner_name:s.host_name);return 1;}if(!nes_lobby_member_get(i,&m))return 0;o->slot=m.slot;o->ready=m.ready;o->is_host=m.slot==0;snprintf(o->display_name,sizeof(o->display_name),"%s",m.display_name);return 1;}
static int cb_move(void*x,int a,int b){RNetLanLobby s;(void)x;if(s_hosting_lan&&a>=0&&a<2&&b>=0&&b<2&&a!=b&&read_lan(&s))return rnet_lan_lobby_set_host_slot(lan_path(),1-s.host_slot);return -1;}
static int cb_local_ready(void*x){(void)x;return(s_hosting_lan||s_joined_lan)?1:nes_lobby_local_ready();}
static int cb_all_ready(void*x){RNetLanLobby s;(void)x;return use_lan_members(&s)?s.joiner_name[0]!=0:nes_lobby_all_ready();}
static int cb_ready(void*x,int r){(void)x;return(s_hosting_lan||s_joined_lan)?0:nes_lobby_set_ready(r);}
static int cb_start(void*x,const RecompLauncherCSettings*s){RNetLanLobby l;NesLobbyMatchCaps c=caps(s);(void)x;if(s_hosting_lan&&use_lan_members(&l)&&l.joiner_name[0])return rnet_lan_lobby_set_started(lan_path(),1);return nes_lobby_request_start(&c);}
static int cb_pending(void*x){RNetLanLobby s;const char*c,*p;(void)x;if((s_hosting_lan||s_joined_lan)&&!s_lan_launch.enabled&&read_lan(&s)&&s.started){memset(&s_lan_launch,0,sizeof(s_lan_launch));s_lan_launch.enabled=1;s_lan_launch.local_slot=s_hosting_lan?s.host_slot:1-s.host_slot;s_lan_launch.session_id=1;s_lan_launch.input_delay=2;if(s_hosting_lan){c=strrchr(s.endpoint,':');p=c?c+1:"7777";snprintf(s_lan_launch.bind_hostport,sizeof(s_lan_launch.bind_hostport),"0.0.0.0:%s",p);}else{snprintf(s_lan_launch.bind_hostport,sizeof(s_lan_launch.bind_hostport),"0.0.0.0:0");snprintf(s_lan_launch.peer_hostport,sizeof(s_lan_launch.peer_hostport),"%s",s.endpoint);}}return s_lan_launch.enabled||nes_lobby_launch_pending();}
static void cb_clear(void*x){(void)x;memset(&s_lan_launch,0,sizeof(s_lan_launch));nes_lobby_clear_launch_pending();}
static int cb_fill(void*x,RecompLauncherCNetplayLaunch*o){const NesLobbyJoinInfo*j;const NesLobbyMatchCaps*c;(void)x;if(!o)return 0;if(s_lan_launch.enabled){*o=s_lan_launch;s_last_launch_was_lan=1;return 1;}j=nes_lobby_join_info();if(!j||!j->ok)return 0;c=nes_lobby_match_caps();memset(o,0,sizeof(*o));o->enabled=1;o->local_slot=j->local_slot;o->session_id=j->session_id;o->input_delay=c&&c->valid?c->input_delay:2;snprintf(o->bind_hostport,sizeof(o->bind_hostport),"%s",j->bind_hostport);snprintf(o->peer_hostport,sizeof(o->peer_hostport),"%s",j->peer_hostport);s_last_launch_was_lan=0;return 1;}

static RecompLauncherCNetplayCallbacks s_callbacks={0,cb_url,cb_set_url,cb_connect,cb_connected,cb_pump,cb_set_name,cb_name,cb_request,cb_count,cb_get,cb_local_ip,cb_external_ip,cb_create,cb_join,cb_leave,cb_in,cb_host,cb_member_count,cb_member_get,cb_move,cb_local_ready,cb_all_ready,cb_ready,cb_start,cb_pending,cb_clear,cb_fill,cb_addr};
const RecompLauncherCNetplayCallbacks *nes_launcher_netplay_callbacks(const char*g,const char*v){snprintf(s_game,sizeof(s_game),"%s",g&&*g?g:"NES Game");snprintf(s_version,sizeof(s_version),"%s",v&&*v?v:"dev");nes_lobby_set_game_identity(s_game,s_version);nes_lobby_set_display_name(g_nes_config.netplay_player_name[0]?g_nes_config.netplay_player_name:"Player");return &s_callbacks;}
void nes_launcher_netplay_seed_settings(RecompLauncherCSettings*s){if(!s)return;snprintf(s->netplay_player_name,sizeof(s->netplay_player_name),"%s",g_nes_config.netplay_player_name[0]?g_nes_config.netplay_player_name:"Player");}
void nes_launcher_netplay_persist_settings(const RecompLauncherCSettings*s){if(!s)return;snprintf(g_nes_config.netplay_player_name,sizeof(g_nes_config.netplay_player_name),"%s",s->netplay_player_name);}
int nes_launcher_netplay_consume_launch(const RecompLauncherCSettings*s,int*ws){NesNetplayConfig c;const NesLobbyMatchCaps*mc;if(!s||!s->netplay_launch.enabled)return 0;nes_netplay_config_defaults(&c);c.enabled=1;c.local_slot=s->netplay_launch.local_slot;c.input_player=s->netplay_launch.input_player;c.session_id=s->netplay_launch.session_id;c.input_delay=s->netplay_launch.input_delay;c.transport=s_last_launch_was_lan?2:1;snprintf(c.bind_hostport,sizeof(c.bind_hostport),"%s",s->netplay_launch.bind_hostport);snprintf(c.peer_hostport,sizeof(c.peer_hostport),"%s",s->netplay_launch.peer_hostport);nes_netplay_set_pending_config(&c);mc=nes_lobby_match_caps();if(ws)*ws=mc&&mc->valid?mc->widescreen:s->widescreen;return 1;}
void nes_launcher_netplay_returned_to_lobby(void){nes_netplay_shutdown();nes_lobby_set_ready(0);nes_lobby_clear_launch_pending();}
#endif
