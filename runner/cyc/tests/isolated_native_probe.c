/* A memory-only native service against the independent per-access adapter. */
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_state.h"
#include "hw_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t host_clock;
static void check(bool ok, const char *why)
{ if (!ok) { fprintf(stderr,"FAIL: %s\n",why); exit(1); } }
static uint8_t operand(uint16_t pc,uint16_t addr,uint8_t value)
{
    (void)pc;(void)addr;
    check(!hw_native_interval.pending && cyc_cycle_count()==host_clock,"operand observer boundary");
    return value^0x39;
}
static void returned(void)
{
    check(!hw_native_interval.pending && cyc_cycle_count()==host_clock,"return observer boundary");
}
static uint64_t state_hash(void)
{
    uint8_t *bytes=NULL;size_t size=0;char error[256];
    check(cyc_state_save(&bytes,&size,error,sizeof error),"serialize current build");
    uint64_t h=UINT64_C(1469598103934665603);
    for(size_t i=0;i<size;i++)h=(h^bytes[i])*UINT64_C(1099511628211);
    free(bytes);return h;
}
int main(int argc,char **argv)
{
    bool reference=argc>1 && !strcmp(argv[1],"--reference");
    static uint8_t image[16+65536+16384];
    static const unsigned boards[]={0,1,4,66,2};
    static const unsigned budgets[]={0,1,2,7,31,62,63,65535};
    memcpy(image,"NES\032",4);
    for(size_t i=16;i<sizeof image;i++)image[i]=(uint8_t)(i*37+(i>>3));
    for(unsigned board=0;board<5;board++)for(unsigned align=0;align<4;align++)for(unsigned pal=0;pal<2;pal++) {
        unsigned mapper=boards[board];image[4]=mapper>=4?4:2;image[5]=mapper>=4?2:1;
        image[6]=(uint8_t)(mapper<<4);image[7]=(uint8_t)(mapper&0xf0);
        check(cyc_load_ines(image,16+image[4]*16384u+image[5]*8192u),"load synthetic board");
        check(cyc_set_region(pal?CYC_REGION_PAL:CYC_REGION_NTSC),"select region");
        cyc_power_on((uint8_t)align);cpu.do_reset=cpu.power_on=0;
        check(cyc_region()==(pal?CYC_REGION_PAL:CYC_REGION_NTSC),"effective region");
        check(cyc_audio_enable(44100),"audio enable");
        host_clock=cyc_cycle_count();
        for(unsigned q=0;q<128;q++) {
            uint64_t before=state_hash();
            uint64_t trace_hash=cyc_trace_hash;uint32_t trace_cycle=cyc_trace_cycle;
            check(cyc_mod_isolate_begin(),"scope begin");
            hw_isolated_cycles=0;hw_isolated_budget=budgets[q/16];
            cyc_trace_enabled=(q&1)!=0;
            if(q&2)hw.irq_line=1;
            if(q&4)cpu.nmi_edge=1;
            if(q&8)check(cyc_mod_set_rom_read_override(0x8124,0x6b),"read substitution");
            cyc_mod_set_ram_read_hook(q&16?operand:NULL);
            cyc_mod_set_return_hook(q&16?returned:NULL);cyc_mod_allow_isolated_hooks(true);
            hw_prg_data_modified=(q&32)!=0;
            hw_native_begin_checked_rom();
            if(reference)hw_native_interval.enabled=false;
            uint64_t cycles=0,reads=0;
#define STEP(expression) do { expression; ++cycles; \
    check(cyc_cycle_count()==host_clock,"no authoritative device time in service"); \
    check(hw_frame_done==(cycles>hw_isolated_budget),"exact budget exhaustion cycle"); } while(0)
            for(unsigned i=0;i<96;i++) {
                uint16_t addr=(uint16_t)((q*13+i*37)&0x1fff);
                switch(i%12) {
                case 0: STEP((void)cpu_fetch_rom(0x8123,hw_cart_prg_read(0x8123)));break;
                case 1: STEP(reads+=cpu_read(addr,CYC_POLL));break;
                case 2: STEP(cpu_write(addr,(uint8_t)(q+i),CYC_DONE));break;
                case 3: STEP(reads+=cpu_read_checked_rom(0x8124,hw_cart_prg_read(0x8124),CYC_POLL));break;
                case 4: STEP(cpu_write(0x2005,(uint8_t)i,CYC_DONE));break;
                case 5: STEP(reads+=cpu_read(0x2002,CYC_POLL));break;
                case 6: STEP(cpu_write(0x4015,(uint8_t)i,CYC_DONE));break;
                case 7: STEP(reads+=cpu_read(0x4015,CYC_POLL));break;
                case 8: STEP(cpu_write(0x8000,(uint8_t)i,CYC_DONE));break;
                case 9: STEP(cpu_write(0x6000,(uint8_t)(q+i),CYC_DONE));break;
                case 10: {
                    uint8_t v;STEP(v=cpu_read(addr,CYC_POLL));
                    reads+=cpu_ram_read_value(0x8123,addr,v);break;
                }
                default:
                    STEP(reads+=cpu_read_code_ram(addr,(uint8_t)i,CYC_DONE));
                    if(cyc_cpu_rts_observer)cyc_cpu_rts_observer();break;
                }
            }
#undef STEP
            hw_native_end();
            check(hw_isolated_cycles==cycles,"complete guest service cycles");
            printf("%u %u %u %u %" PRIu64 " %016" PRIx64 "\n",mapper,align,pal,q,reads,state_hash());
            cyc_trace_enabled=false;
            cyc_mod_clear_rom_read_overrides();cyc_mod_set_ram_read_hook(NULL);cyc_mod_set_return_hook(NULL);
            hw_prg_data_modified=false;cyc_mod_isolate_end();
            /* This fixture deliberately re-enables the normally muted trace
             * inside the scope. Undo its own diagnostic policy, which is not
             * part of the isolation snapshot, before comparing host state. */
            cyc_trace_hash=trace_hash;cyc_trace_cycle=trace_cycle;
            check(state_hash()==before,"exact authoritative host restoration");
        }
    }
    if(!reference)check(hw_native_isolated_cycles>100,"isolated native service used");
    return 0;
}
