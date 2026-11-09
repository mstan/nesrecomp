/* Actual chat handlers/parser with controlled session status and guest API. */
#define cyc_tcp_ok unobserved_tcp_ok
#define cyc_tcp_err unobserved_tcp_err
#include "../cyc_tcp.c"
#undef cyc_tcp_ok
#undef cyc_tcp_err
#include "cyc_mod.h"
#include "cyc_net.h"
#include <assert.h>
#undef assert
#define assert(value) do { if (!(value)) { fprintf(stderr,"chat contract failed: %s\n",#value); return 1; } } while (0)
static unsigned errors, calls, writes;
static bool active;
static uint8_t ram[2048];
void cyc_tcp_ok(int id, const char *fields) { (void)id; (void)fields; }
void cyc_tcp_err(int id, const char *message) { (void)id; (void)message; errors++; }
int nes_netplay_active(void) { return active; }
uint8_t cyc_mod_peek(uint16_t addr) { return ram[addr & 2047]; }
bool cyc_mod_peek_ok(uint16_t addr, uint8_t *out) { if(addr>=2048)return false;*out=ram[addr];return true; }
bool cyc_mod_poke(uint16_t addr, uint8_t value) { writes++;ram[addr & 2047]=value;return true; }
void cyc_mod_regs(CycModRegs *out) { memset(out,0,sizeof(*out)); }
bool cyc_mod_call_commit(uint16_t routine, CycModRegs *regs) { (void)routine;(void)regs;calls++;return true; }
#include "../cyc_chat.c"
int main(void) {
    ram[162]=156;
    /* TCP cannot enable the feature. Default OFF refuses immediate and queued
     * mutation, display requests, and a show=true configuration bypass. */
    tcp_ram(1,"{\"addr\":162,\"add\":-4}");
    tcp_pad(1,"{\"buttons\":1,\"frames\":60}");
    tcp_call(1,"{\"addr\":32768}");
    tcp_chat(1,"{\"text\":\"disabled\"}");
    tcp_banner(1,"{\"text\":\"disabled\"}");
    tcp_poll(1,"{\"options\":\"A|B\"}");
    tcp_config(1,"{\"show\":true}");
    assert(errors==6 && writes==0 && calls==0 && !s_enabled);
    assert(s_msg_count==0 && !s_banner.life && s_poll.count==0);
    assert(cyc_chat_frame(128)==128);
    chat_activate();
    tcp_ram(1,"{\"addr\":162,\"add\":-4}");assert(ram[162]==152 && writes==1);
    tcp_pad(1,"{\"buttons\":1,\"frames\":60}");assert(cyc_chat_frame(0)==1);
    tcp_ram(1,"{\"addr\":162,\"add\":-1,\"frames\":30}");
    active=true;unsigned old_errors=errors,old_writes=writes;
    tcp_ram(1,"{\"addr\":162,\"set\":1}");
    tcp_pad(1,"{\"buttons\":128,\"frames\":30}");
    tcp_call(1,"{\"addr\":32768}");
    assert(errors==old_errors+3 && writes==old_writes && calls==0 && ram[162]==152);
    assert(cyc_chat_frame(128)==128 && writes==old_writes);
    active=false;assert(cyc_chat_frame(0)==0 && writes==old_writes);
    tcp_chat(1,"{\"text\":\"enabled\"}");
    tcp_banner(1,"{\"text\":\"enabled\"}");
    tcp_poll(1,"{\"options\":\"A|B\"}");
    tcp_pad(1,"{\"buttons\":1,\"frames\":60}");
    tcp_ram(1,"{\"addr\":162,\"add\":-1,\"frames\":30}");
    assert(s_msg_count && s_banner.life && s_poll.count && s_pad[0].on && s_ram[0].on);
    chat_reset();
    assert(!s_enabled && !s_msg_count && !s_banner.life && !s_poll.count);
    assert(cyc_chat_frame(128)==128 && writes==old_writes);
    chat_activate();
    assert(cyc_chat_frame(0)==0 && writes==old_writes);
    return 0;
}
