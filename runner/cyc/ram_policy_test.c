/* Operand verdicts keep the physical bus, RAM and clock unchanged. Exact
 * background capture follows the hardware color pipe and survives states. */
#include "hw_internal.h"
#include "cpu6502.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_state.h"
#include "cyc_run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned failures, verdicts;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);failures++;}}while(0)
static uint8_t verdict(uint16_t pc,uint16_t addr,uint8_t value) {
    CHECK(addr==0x714);
    CHECK(pc==0x600 || pc==0x606 || pc==0x60e);
    CHECK(value==0x11);verdicts++;return 0x33;
}
static uint64_t operands(bool policy) {
    static const uint8_t body[]={
        0xad,0x14,0x07,0x8d,0x40,0x04, /* LDA $714; STA $440 */
        0xae,0x14,0x17,0x8e,0x41,0x04, /* LDX RAM mirror; STX $441 */
        0xa2,0x14,0xbc,0x00,0x07,0x8c,0x42,0x04, /* LDY $700,X */
        0xee,0x14,0x07, /* INC $714: read/old write/new write, never virtual */
        0xa9,0x77,0x8d,0x43,0x04, /* immediate, never virtual */
        0xad,0x02,0x20,0x60 /* PPU device, never virtual */
    };
    cyc_mod_set_ram_read_hook(policy?verdict:NULL);verdicts=0;
    for(unsigned i=0;i<sizeof body;i++)CHECK(cyc_mod_poke((uint16_t)(0x600+i),body[i]));
    CHECK(cyc_mod_poke(0x714,0x11));uint64_t clock=cyc_cycle_count();
    CycModStats before,after;cyc_mod_stats(&before);
    CHECK(cyc_mod_isolate_begin());CycModRegs r={0,0,0,0xfd,0x24,0};CHECK(cyc_mod_call(0x600,&r));
    CHECK(cyc_mod_peek(0x440)==(policy?0x33:0x11));
    CHECK(cyc_mod_peek(0x441)==(policy?0x33:0x11));
    CHECK(cyc_mod_peek(0x442)==(policy?0x33:0x11));
    CHECK(cyc_mod_peek(0x443)==0x77 && cyc_mod_peek(0x714)==0x12);
    CHECK(verdicts==(policy?3u:0u));cyc_mod_isolate_end();
    CHECK(cyc_cycle_count()==clock && cyc_mod_peek(0x714)==0x11);
    cyc_mod_stats(&after);return after.cycles-before.cycles;
}
static void background(void) {
    uint32_t captured[256*240];char err[256];uint8_t *state=NULL,*again=NULL;size_t n=0,m=0;
    cyc_mod_set_ram_read_hook(NULL);cyc_render_capture_background(true);
    for(int i=0;i<4;i++)cyc_run_frame();
    CHECK(cyc_render_background(captured));CHECK(!memcmp(captured,cyc_frame_argb(),sizeof captured));
    CHECK(cyc_state_save(&state,&n,err,sizeof err));
    CHECK(cyc_mod_isolate_begin());CycModRegs r={0,0,0,0xfd,0x24,0};CHECK(cyc_mod_call(0x600,&r));cyc_mod_isolate_end();
    CHECK(cyc_state_save(&again,&m,err,sizeof err));CHECK(n==m && !memcmp(state,again,n));free(again);
    for(int i=0;i<3;i++)cyc_run_frame();
    CHECK(cyc_state_load(state,n,err,sizeof err));CHECK(cyc_render_background(captured));CHECK(!memcmp(captured,cyc_frame_argb(),sizeof captured));
    free(state);cyc_render_capture_background(false);CHECK(!cyc_render_background(captured));
}
int main(void) {
    uint8_t image[16+16384+8192]={0};memcpy(image,"NES\032",4);image[4]=image[5]=1;
    static const uint8_t boot[]={0xa9,0x3f,0x8d,0x06,0x20,0xa9,0,0x8d,0x06,0x20,
        0xa9,0x22,0x8d,0x07,0x20,0xa9,0x16,0x8d,0x07,0x20,
        0xa9,0x08,0x8d,0x00,0x20,0xa9,0x0a,0x8d,0x01,0x20,0x4c,0x1e,0x80};
    memcpy(image+16,boot,sizeof boot);image[16+0x3ffc]=0;image[16+0x3ffd]=0x80;
    for(int i=0;i<8;i++)image[16+16384+i]=0xaa;
    CHECK(cyc_load_ines(image,sizeof image));cyc_power_on(0);
    uint64_t plain=operands(false);CHECK(plain==operands(true));cyc_mod_set_ram_read_hook(NULL);
    background();printf("ram_policy_test: %u failures\n",failures);return failures?1:0;
}
