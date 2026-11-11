/* A memory-only native service against the independent per-access adapter. */
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_state.h"
#include "hw_internal.h"
#include <inttypes.h>
#include <math.h>
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
static void audio_bus(unsigned cycles)
{
    for(unsigned i=0;i<cycles;i++) {
        hw_cycle_start(0,HW_READ);(void)hw_read(0);hw_cycle_finish(false);
    }
}
static void audio_write(uint16_t address,uint8_t value)
{
    hw_cycle_start(address,HW_WRITE);hw_write(address,value);hw_cycle_finish(false);
}
static void audio_states(void)
{
    static uint8_t image[16+32768+8192];
    memcpy(image,"NES\032",4);image[4]=2;image[5]=1;
    memset(image+16,0xea,32768);
    for(unsigned i=0x7ffa;i<0x8000;i+=2){image[16+i]=0;image[17+i]=0x80;}
    for(unsigned pal=0;pal<2;pal++)for(unsigned console=0;console<2;console++)for(unsigned rate=0;rate<2;rate++) {
        unsigned hz=rate?48000:44100;
        check(cyc_load_ines(image,sizeof image),"audio synthetic cartridge");
        check(cyc_set_region(pal?CYC_REGION_PAL:CYC_REGION_NTSC),"audio region");
        cyc_power_on(0);cpu.do_reset=cpu.power_on=0;cpu.pc=0x8000;
        cyc_set_console(console?CYC_CONSOLE_FAMICOM:CYC_CONSOLE_NES);
        check(cyc_audio_enable((int)hz),"audio rate");
        audio_write(0x4015,4);audio_write(0x4008,0xff);audio_write(0x400a,0x40);audio_write(0x400b,0xf8);
        audio_bus(12345);
        /* Wrap a nonempty ring and retain a nonzero analog filter history. */
        for(unsigned i=0;i<33000;i++)apu_debug_emit((i&127)/128.0);
        int16_t drain[17];check(cyc_audio_read(drain,17)==17,"partial ring drain");
        size_t output_len=apu_output_save(NULL,0);
        uint8_t *before=malloc(output_len),*after=malloc(output_len),*bad=malloc(output_len);
        check(before && after && bad,"audio record allocation");
        apu_output_save(before,output_len);
        uint8_t *state=NULL;size_t state_len=0;char error[256];
        check(cyc_state_save(&state,&state_len,error,sizeof error),"audio state save");
        CycSnapshot *snapshot=cyc_snapshot_new();check(snapshot!=NULL,"audio snapshot");
        cyc_snapshot_take(snapshot);
        check(cyc_mod_isolate_begin(),"audio isolation begin");audio_bus(9999);apu_debug_emit(0.75);
        cyc_mod_isolate_end();apu_output_save(after,output_len);
        check(!memcmp(before,after,output_len),"isolation restores phase, filters and pending ring");
        static int16_t expected[40000],actual[40000];
        /* Test the next sample count too, without a full ring masking it. */
        while(cyc_audio_read(expected,40000)) {}
        free(state);state=NULL;
        check(cyc_state_save(&state,&state_len,error,sizeof error),"empty ring state save");
        cyc_snapshot_take(snapshot);
        audio_bus(19001);size_t expected_len=cyc_audio_read(expected,40000);
        check(expected_len>0 && expected_len<1024,"phase-dependent sample count");
        cyc_snapshot_restore(snapshot);audio_bus(19001);size_t actual_len=cyc_audio_read(actual,40000);
        check(actual_len==expected_len && !memcmp(expected,actual,actual_len*sizeof *actual),"snapshot PCM continuation");
        /* Reset host sampling just as a cold process would, then load. */
        check(cyc_audio_enable((int)hz),"reset sampler");
        check(cyc_state_load(state,state_len,error,sizeof error),"audio state load");
        audio_bus(19001);actual_len=cyc_audio_read(actual,40000);
        check(actual_len==expected_len && !memcmp(expected,actual,actual_len*sizeof *actual),"state PCM sample count and values");
        apu_output_save(before,output_len);
        check(!apu_output_load(before,output_len-1,true),"short audio record refused");
        check(!apu_output_load(NULL,output_len,true),"missing audio record refused");
        memcpy(bad,before,output_len);uint32_t invalid=32768;memcpy(bad+16,&invalid,4);
        check(!apu_output_load(bad,output_len,true),"out of range ring refused");
        memcpy(bad,before,output_len);double invalid_phase=NAN;memcpy(bad+32,&invalid_phase,8);
        check(!apu_output_load(bad,output_len,true),"nonfinite phase refused");
        apu_output_save(after,output_len);check(!memcmp(before,after,output_len),"refusals preserve output");
        check(cyc_audio_enable(32000),"different current rate");
        check(!apu_output_load(before,output_len,true),"different output configuration refused");
        check(cyc_audio_enable((int)hz),"restore current rate");
        check(apu_output_load(before,output_len,true),"same configuration restored");
        CycConsole other=console?CYC_CONSOLE_NES:CYC_CONSOLE_FAMICOM;
        cyc_set_console(other);
        check(apu_output_load(before,output_len,true),"console choice is presentation policy");
        check(cyc_console()==other,"load preserves current console choice");
        /* A malformed output section must refuse before restoring core RAM. */
        size_t at=32;bool found=false;
        while(at+8<=state_len) {
            uint32_t length;memcpy(&length,state+at+4,4);
            check(length<=state_len-at-8,"valid test state chunks");
            if(!memcmp(state+at,"AOUT",4)) {memcpy(state+at+8+16,&invalid,4);found=true;break;}
            at+=8+length;
        }
        check(found,"sampler section present");
        hw.ram[7]^=0x39;uint64_t identity=state_hash();
        check(!cyc_state_load(state,state_len,error,sizeof error),"malformed sampler state refused");
        check(state_hash()==identity,"state refusal atomic for core and output");
        cyc_snapshot_free(snapshot);free(before);free(after);free(bad);free(state);
        printf("audio-state pal=%u console=%u rate=%u samples=%zu PASS\n",pal,console,hz,actual_len);
    }
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"--audio-state")){audio_states();return 0;}
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
        /* Normal snapshots retain their universal full-storage contract. */
        CycSnapshot *full = cyc_snapshot_new();
        check(full != NULL, "normal snapshot allocation");
        uint8_t unused = hw_cart.wram[sizeof hw_cart.wram - 1];
        cyc_snapshot_take(full);
        hw_cart.wram[sizeof hw_cart.wram - 1] ^= 0xa5;
        cyc_snapshot_restore(full);
        check(hw_cart.wram[sizeof hw_cart.wram - 1] == unused, "normal snapshot restores all RAM");
        cyc_snapshot_free(full);
        host_clock=cyc_cycle_count();
        for(unsigned q=0;q<128;q++) {
            static const uint32_t ram_sizes[] = { 0x2000, 0x8000, 0x20000 };
            hw_cart.wram_len = ram_sizes[(q / 16) % 3];
            hw_cart.wram_bank = hw_cart.wram_len - 0x2000;
            hw_cart.has_wram = (q & 32) == 0;
            hw_cart.wram_readable = hw_cart.wram_writable = (q & 64) == 0;
            uint64_t before=state_hash();
            uint64_t trace_hash=cyc_trace_hash;uint32_t trace_cycle=cyc_trace_cycle;
            check(cyc_mod_isolate_begin(),"scope begin");
            if (hw_cart.has_wram)
                check(cyc_mod_poke(0x7fff, (uint8_t)(q + 0x58)), "last physical RAM bank mutation");
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
