/* PAL cursor versus coupled clock: five phases, fixed-PRG boards, CHR-bank
 * writes, delayed PPU I/O, both DMAs, APU modes and snapshot continuation. */
#include "hw_internal.h"
#include "cpu6502.h"
#include "cyc_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(bool ok, const char *why) { if (!ok) { fprintf(stderr,"FAIL: %s\n",why); exit(1); } }
static void access_bus(uint16_t a, uint8_t v, bool w) {
    hw_cycle_start(a,w?HW_WRITE:HW_READ); if(w)hw_write(a,v);else(void)hw_read(a);hw_cycle_finish(false);
}
static void stimulus(unsigned i) {
    (void)cpu_fetch_rom(0x8123,0xff);
    uint16_t a=(uint16_t)(i&0x7ff);
    uint8_t v=cpu_read(a,CYC_POLL);
    cpu_write(a,(uint8_t)(v+i),CYC_DONE);
    (void)cpu_read_rom(0x8124,0xff,CYC_POLL|CYC_DONE);
    switch(i%1031) {
    case 0: cpu_write(0x2001,(i&2048)?0x1e:0,CYC_DONE);break;
    case 1: cpu_write(0x2000,(uint8_t)((i>>5)&0x7f),CYC_DONE);break;
    case 2: cpu_write(0x2005,(uint8_t)i,CYC_DONE);break;
    case 3: cpu_write(0x2005,(uint8_t)(i>>4),CYC_DONE);break;
    case 4: cpu_write(0x4014,2,CYC_DONE);break;
    case 5: cpu_write(0x4017,(i&4096)?0:0xc0,CYC_DONE);break;
    case 6: cpu_write(0x4010,0x8f,CYC_DONE);break;
    case 7: cpu_write(0x4013,1,CYC_DONE);break;
    case 8: cpu_write(0x4015,(i&8192)?0x1f:0x0f,CYC_DONE);break;
    case 9: (void)cpu_read(0x4015,CYC_POLL|CYC_DONE);break;
    case 10:(void)cpu_read(0x2002,CYC_POLL|CYC_DONE);break;
    case 11:cpu_write(0x2006,0x20,CYC_DONE);break;
    case 12:cpu_write(0x2006,(uint8_t)i,CYC_DONE);break;
    case 13:cpu_write(0x2007,(uint8_t)i,CYC_DONE);break;
    case 14:(void)cpu_read(0x2007,CYC_POLL|CYC_DONE);break;
    case 15:if(hw_cart.mapper==3)cpu_write(0x8000,(uint8_t)((i>>10)&3),CYC_DONE);break;
    }
}
int main(int argc,char **argv) {
    bool candidate=argc>1&&!strcmp(argv[1],"candidate");
    static uint8_t image[16+32768+32768];
    memcpy(image,"NES\032",4);image[4]=2;image[5]=4;
    memset(image+16,0xff,32768);
    for(size_t i=16+32768;i<sizeof(image);++i)image[i]=(uint8_t)(i*37+(i>>3));
    for(unsigned board=0;board<2;++board)for(unsigned phase=0;phase<5;++phase){
        image[6]=board?0x30:0;
        check(cyc_load_ines(image,sizeof(image)),"load");check(cyc_set_region(CYC_REGION_PAL),"PAL");
        cyc_power_on((uint8_t)phase);check(cyc_audio_enable(48000),"audio");
        check(cyc_zapper_attach(2),"Zapper");cyc_set_zapper(120,100,true);
        cpu.i=1;cpu.do_reset=cpu.power_on=0;
        for(unsigned i=0;i<40000;++i)access_bus(0,0,false);
        access_bus(0x4017,0xc0,true);access_bus(0x4015,0x0f,true);
        for(unsigned i=0;i<16;++i)access_bus((uint16_t)(0x4000+i),(uint8_t)(i*31+7),true);
        access_bus(0x2001,0x1e,true);
        hw_native_set_pal_intervals(candidate);
        printf("board=%u phase=%u\n",board?3:0,phase);
        hw_native_begin();
        for(unsigned i=1;i<=50000;++i){
            stimulus(i);
            if(i%127==0){
                hw_native_end();
                printf("%u %016llx %016llx %u%u%u\n",i,(unsigned long long)cyc_hw_state_hash(),
                    (unsigned long long)cyc_mem_state_hash(),cpu.do_irq,cpu.do_nmi,cpu.nmi_edge);
                hw_native_begin();
            }
            if(i==20000){
                CycSnapshot *s=cyc_snapshot_new();check(s!=NULL,"snapshot");cyc_snapshot_take(s);
                for(unsigned j=60000;j<60313;++j)stimulus(j);
                hw_native_end();uint64_t h=cyc_hw_state_hash(),m=cyc_mem_state_hash();
                cyc_snapshot_restore(s);hw_native_begin();
                for(unsigned j=60000;j<60313;++j)stimulus(j);
                hw_native_end();check(h==cyc_hw_state_hash()&&m==cyc_mem_state_hash(),"snapshot replay");
                cyc_snapshot_free(s);hw_native_begin();
            }
        }
        hw_native_end();check(!candidate||hw_native_interval_cycles>1000,"PAL batching exercised");
        int16_t pcm[8192];size_t n=cyc_audio_read(pcm,8192);uint64_t h=1469598103934665603ULL;
        for(size_t i=0;i<n;++i){h=(h^(uint8_t)pcm[i])*1099511628211ULL;h=(h^(uint8_t)((uint16_t)pcm[i]>>8))*1099511628211ULL;}
        printf("pcm %zu %016llx\n",n,(unsigned long long)h);
    }
    return 0;
}
