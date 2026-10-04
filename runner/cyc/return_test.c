/* A routine observer runs after the actual RTS bus cycles, and isolated
 * observers require explicit permission. It cannot leak scratch machine state. */
#include "hw_internal.h"
#include "cpu6502.h"
#include "cyc_mod.h"
#include "cyc_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned failures,returns;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);failures++;}}while(0)
static void returned(void) {
    CHECK((cpu.pc==0x0603 && cpu.s==0xfb) || (cpu.pc==0x5fff && cpu.s==0xfd));
    CHECK(cyc_mod_poke(0x20,0x73));returns++;
}
static void program(void) {
    static const uint8_t body[]={0x20,0x10,0x06,0xa9,0x42,0x60};
    for(unsigned i=0;i<sizeof body;i++)CHECK(cyc_mod_poke((uint16_t)(0x600+i),body[i]));
    CHECK(cyc_mod_poke(0x610,0xa9));CHECK(cyc_mod_poke(0x611,0x19));CHECK(cyc_mod_poke(0x612,0x60));
}
static void isolate(bool hooks) {
    char err[256];uint8_t *before=NULL,*after=NULL;size_t n=0,m=0;
    CHECK(cyc_state_save(&before,&n,err,sizeof err));
    unsigned old=returns;CHECK(cyc_mod_isolate_begin());
    cyc_mod_allow_isolated_hooks(hooks);
    CycModRegs r={0,0,0,0xfd,0x24,0};
    CHECK(cyc_mod_call(0x600,&r) && r.a==0x42 && r.s==0xfd);
    CHECK(returns-old==(hooks?2u:0u));
    if(hooks)CHECK(cyc_mod_peek(0x20)==0x73);
    cyc_mod_isolate_end();
    CHECK(cyc_state_save(&after,&m,err,sizeof err));CHECK(n==m && !memcmp(before,after,n));
    free(before);free(after);
}
int main(void) {
    uint8_t image[16+16384]={0};memcpy(image,"NES\032",4);image[4]=1;
    CHECK(cyc_load_ines(image,sizeof image));cyc_power_on(0);program();
    cyc_mod_set_return_hook(returned);
    isolate(false);isolate(true);isolate(false); /* permission does not persist */
    cpu.pc=0x600;cpu.s=0xfd-2;cpu.power_on=cpu.do_reset=cpu.do_nmi=cpu.do_irq=0;
    hw.ram[0x1fc]=0xfe;hw.ram[0x1fd]=0x5f;
    uint64_t start=cyc_cycle_count();unsigned old=returns;
    for(unsigned i=0;i<8 && cpu.pc!=0x5fff;i++)cpu_interp_step();
    CHECK(cpu.pc==0x5fff && cpu.s==0xfd && cpu.a==0x42);
    CHECK(cyc_cycle_count()-start==22 && returns-old==2);
    cyc_mod_set_return_hook(NULL);
    CHECK(cyc_cpu_rts_observer==NULL);
    printf("return_test: %u failures\n",failures);return failures?1:0;
}
