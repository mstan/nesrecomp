#include "nes_netplay.h"
#include "nes_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_net/recomp_net.h"
#if defined(NES_HAS_LOBBY_CLIENT)
#include "nes_lobby_client.h"
#endif

typedef struct {
    RNetSession *session;
    uint8_t staged_buttons, published[2];
    uint32_t staged_digest, latched_tick, desync_tick, digest[2];
    int staged_valid, active, local_slot, input_player, needs_advance;
    int latched, use_ice, state_desync;
} NesNetplayState;
static NesNetplayState g_np;
static NesNetplayConfig g_pending;
static int g_pending_valid;

void nes_netplay_set_pending_config(const NesNetplayConfig *c) {
    if (!c) { g_pending_valid=0; memset(&g_pending,0,sizeof(g_pending)); return; }
    g_pending=*c; g_pending_valid=1;
}
int nes_netplay_take_pending_config(NesNetplayConfig *c) {
    if (!c||!g_pending_valid)return 0;*c=g_pending;g_pending_valid=0;return 1;
}

void nes_netplay_config_defaults(NesNetplayConfig *c) {
    if (!c) return; memset(c,0,sizeof(*c)); c->input_delay=2; c->session_id=1;
    strcpy(c->bind_hostport,"0.0.0.0:7777");
}
static unsigned env_u(const char *n,unsigned d) {
    const char *v=getenv(n); return v&&*v?(unsigned)strtoul(v,0,10):d;
}
void nes_netplay_apply_env(NesNetplayConfig *c) {
    const char *v; if(!c)return;
    v=getenv("NES_NETPLAY"); if(v&&*v)c->enabled=atoi(v)!=0;
    c->local_slot=(int)env_u("NES_NET_SLOT",(unsigned)c->local_slot);
    c->input_player=(int)env_u("NES_NET_INPUT_PLAYER",(unsigned)c->input_player);
    c->input_delay=(int)env_u("NES_NET_INPUT_DELAY",(unsigned)c->input_delay);
    c->session_id=env_u("NES_NET_SESSION",c->session_id);
    v=getenv("NES_NET_BIND"); if(v&&*v)snprintf(c->bind_hostport,sizeof(c->bind_hostport),"%s",v);
    v=getenv("NES_NET_PEER"); if(v&&*v)snprintf(c->peer_hostport,sizeof(c->peer_hostport),"%s",v);
    v=getenv("NES_NET_TRANSPORT");
    if(v&&(!strcmp(v,"ice")||!strcmp(v,"ICE")))c->transport=1;
    else if(v&&(!strcmp(v,"lan")||!strcmp(v,"LAN")))c->transport=2;
}
static void encode(uint8_t b,uint32_t d,RNetInputSample *o,rnet_u32 t) {
    memset(o,0,sizeof(*o)); o->tick=t; o->size=NES_NETPLAY_PAD_BYTES; o->valid=1;
    o->bytes[0]=b; o->bytes[1]=(rnet_u8)d; o->bytes[2]=(rnet_u8)(d>>8);
    o->bytes[3]=(rnet_u8)(d>>16); o->bytes[4]=(rnet_u8)(d>>24);
}
static uint32_t digest(const RNetInputSample *i) {
    if(!i||!i->valid||i->size<5)return 0;
    return (uint32_t)i->bytes[1]|((uint32_t)i->bytes[2]<<8)|
           ((uint32_t)i->bytes[3]<<16)|((uint32_t)i->bytes[4]<<24);
}
static void sample(rnet_u32 t,RNetInputSample *o,void *ctx) {
    NesNetplayState *s=(NesNetplayState*)ctx;
    encode(s->staged_valid?s->staged_buttons:0,
           s->staged_valid?s->staged_digest:nes_runtime_state_digest(),o,t);
}
static void publish(rnet_u32 t,const RNetInputSample *p,int n,void *ctx) {
    NesNetplayState *s=(NesNetplayState*)ctx; int i;
    s->published[0]=s->published[1]=0; if(!p||n<2)return;
    for(i=0;i<n&&i<2;i++){if(p[i].valid&&p[i].size)s->published[i]=p[i].bytes[0];s->digest[i]=digest(&p[i]);}
    if(p[0].size>=5&&p[1].size>=5&&s->digest[0]!=s->digest[1]){
        s->state_desync=1;s->desync_tick=t;
    }
}
#if defined(NES_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
static void on_signal(const RNetSignal *m,void *ctx) {
    (void)ctx;if(m)(void)nes_lobby_send_signal((int)m->type,(int)m->flag,m->text);
}
static void drain_signals(void) {
    int type,flag;char text[2048];
    while(g_np.session&&nes_lobby_poll_signal(&type,&flag,text,sizeof(text))){
        RNetSignal s;memset(&s,0,sizeof(s));
        if(type==(int)RNET_SIGNAL_LOCAL_SDP)type=(int)RNET_SIGNAL_REMOTE_SDP;
        else if(type==(int)RNET_SIGNAL_LOCAL_CANDIDATE)type=(int)RNET_SIGNAL_REMOTE_CANDIDATE;
        s.type=(RNetSignalType)type;s.flag=(rnet_u8)flag;snprintf(s.text,sizeof(s.text),"%s",text);
        rnet_session_push_signal(g_np.session,&s);
    }
}
#else
static void drain_signals(void){}
#endif
static int private_peer(const char *hp) {
    char h[64];const char *c;size_t n;unsigned a=0,b=0;
    if(!hp||!*hp)return 1;c=strrchr(hp,':');n=c?(size_t)(c-hp):strlen(hp);
    if(n>=sizeof(h))n=sizeof(h)-1;memcpy(h,hp,n);h[n]=0;
    if(!strcmp(h,"localhost"))return 1;if(sscanf(h,"%u.%u",&a,&b)<1)return 0;
    return a==127||a==10||(a==192&&b==168)||(a==172&&b>=16&&b<=31);
}
static int use_ice(const NesNetplayConfig *c) {
    if(c->transport==2)return 0;
#if defined(RNET_ENABLE_ICE) && defined(NES_HAS_LOBBY_CLIENT)
    if(!nes_lobby_connected()||!nes_lobby_in_lobby())return 0;
    return c->transport==1||!private_peer(c->peer_hostport);
#else
    (void)c;return 0;
#endif
}
int nes_netplay_start(const NesNetplayConfig *c) {
    RNetConfig rc;RNetHostVTable h;int ice;
    if(!c||!c->enabled)return -1;nes_netplay_shutdown();rnet_config_init_defaults(&rc);
    rc.slot_count=2;rc.local_slot=(rnet_u8)(c->local_slot==1);rc.input_delay=(rnet_u8)(c->input_delay<0?0:c->input_delay>16?16:c->input_delay);
    rc.session_id=c->session_id?c->session_id:1;rc.protocol_magic=0x4e45534eu;ice=use_ice(c);
    memset(&h,0,sizeof(h));h.sample_local=sample;h.publish=publish;h.ctx=&g_np;
#if defined(NES_HAS_LOBBY_CLIENT) && defined(RNET_ENABLE_ICE)
    if(ice)h.on_signal=on_signal;
#endif
    g_np.session=rnet_session_create(&rc,&h);if(!g_np.session)return -2;
    if(ice){
#if defined(RNET_ENABLE_ICE)
        RNetIceConfig ic;rnet_ice_config_init_defaults(&ic);ic.controlling=rc.local_slot==0;
        if(rnet_session_start_ice(g_np.session,&ic)!=0){rnet_session_destroy(g_np.session);g_np.session=0;return -3;}
#endif
    }else if(rnet_session_start_lan(g_np.session,c->bind_hostport,c->peer_hostport)!=0){
        rnet_session_destroy(g_np.session);g_np.session=0;return -4;
    }
    g_np.active=1;g_np.local_slot=rc.local_slot;g_np.input_player=c->input_player==1;g_np.use_ice=ice;
    fprintf(stderr,"nes_netplay: %s slot=%d session=%u delay=%u bind=%s peer=%s\n",
            ice?"ice":"lan",g_np.local_slot,(unsigned)rc.session_id,(unsigned)rc.input_delay,c->bind_hostport,c->peer_hostport);
    return 0;
}
void nes_netplay_shutdown(void) {
    if(g_np.session){(void)rnet_session_send_bye(g_np.session);rnet_session_destroy(g_np.session);}
    memset(&g_np,0,sizeof(g_np));
}
int nes_netplay_active(void){return g_np.active&&g_np.session!=0;}
int nes_netplay_is_running(void){return nes_netplay_active()&&rnet_session_is_running(g_np.session);}
int nes_netplay_local_slot(void){return nes_netplay_active()?g_np.local_slot:-1;}
int nes_netplay_input_player(void){return nes_netplay_active()?g_np.input_player:0;}
uint32_t nes_netplay_sim_tick(void){return nes_netplay_active()?rnet_session_sim_tick(g_np.session):0;}
void nes_netplay_stage_local(uint8_t b){
    uint32_t t;if(!nes_netplay_active())return;t=nes_netplay_is_running()?rnet_session_sim_tick(g_np.session):0;
    if(g_np.latched&&g_np.latched_tick==t)return;g_np.staged_buttons=b;g_np.staged_digest=nes_runtime_state_digest();
    g_np.staged_valid=1;g_np.latched=1;g_np.latched_tick=t;
}
int nes_netplay_needs_local_sample(void){
    uint32_t t;if(!nes_netplay_active())return 0;t=nes_netplay_is_running()?rnet_session_sim_tick(g_np.session):0;
    return !(g_np.latched&&g_np.latched_tick==t);
}
int nes_netplay_poll_admit(void){
    rnet_u32 t;if(!nes_netplay_active())return 1;
#if defined(NES_HAS_LOBBY_CLIENT)
    if(nes_lobby_connected())nes_lobby_pump();
#endif
    drain_signals();rnet_session_pump(g_np.session);if(!rnet_session_is_running(g_np.session))return 0;
    if(g_np.needs_advance)return !g_np.state_desync;t=rnet_session_sim_tick(g_np.session);
    if(!rnet_session_try_admit(g_np.session,t))return 0;g_np.needs_advance=1;return !g_np.state_desync;
}
void nes_netplay_finish_frame(void){
    if(!nes_netplay_active()||!g_np.needs_advance)return;rnet_session_advance(g_np.session);
    g_np.needs_advance=0;g_np.latched=0;g_np.staged_valid=0;
}
uint8_t nes_netplay_published_buttons(int s){return s>=0&&s<2?g_np.published[s]:0;}
void nes_netplay_wait_recv(int ms){if(nes_netplay_active())(void)rnet_session_wait_recv(g_np.session,ms);}
int nes_netplay_input_desync(uint32_t*t,uint32_t*l,uint32_t*r){return nes_netplay_active()&&rnet_session_input_desync(g_np.session,t,l,r);}
int nes_netplay_state_desync(uint32_t*t,uint32_t*a,uint32_t*b){
    if(!g_np.state_desync)return 0;if(t)*t=g_np.desync_tick;if(a)*a=g_np.digest[0];if(b)*b=g_np.digest[1];return 1;
}
int nes_netplay_peer_disconnected(uint32_t ms){return nes_netplay_active()&&rnet_session_peer_disconnected(g_np.session,ms?ms:1500);}
