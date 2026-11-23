#include "hw_internal.h"
#include "hw.h"
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(bool ok,const char *message) { if(!ok){fprintf(stderr,"FAIL: %s\n",message);exit(1);} }
static void read_cycle(void) { hw_cycle_start(0,HW_READ);(void)hw_read(0);hw_cycle_finish(true); }
static void write_cycle(uint16_t address,uint8_t value) { hw_cycle_start(address,HW_WRITE);hw_write(address,value);hw_cycle_finish(true); }
#if NESRECOMP_PPU_FRAME_PAL
int main(int argc,char **argv)
{
    bool pal=argc>1 && !strcmp(argv[1],"--pal");
#else
int main(void)
{
#endif
    static uint8_t rom[16+32768+8192];
    static uint16_t expected[256*240];
    memcpy(rom,"NES\032",4);rom[4]=2;rom[5]=1;
    for(unsigned tile=0;tile<256;++tile)for(unsigned row=0;row<8;++row){
        rom[16+32768+tile*16+row]=(uint8_t)(0x81u^(tile*17u+row*29u));
        rom[16+32768+tile*16+8+row]=(uint8_t)(0x55u^(tile*11u+row*19u));
    }
#if NESRECOMP_PPU_FRAME_PAL
    const unsigned targets[]={20,56,64,65,66,128,248,255,256,257,258};
    for(unsigned align=0;align<(pal?5u:4u);++align)for(unsigned target=0;target<sizeof(targets)/sizeof(targets[0]);++target){
        check(cyc_load_ines(rom,sizeof(rom)),"load NROM");check(cyc_set_region(pal?CYC_REGION_PAL:CYC_REGION_NTSC),"region");cyc_power_on((uint8_t)align);
        check(!strcmp(cyc_ppu_implementation(),pal?"FRAME-PAL":"FRAME"),"explicit selected region provider");
#else
    const unsigned targets[]={20,56,64,128,248,257,258};
    for(unsigned align=0;align<4;++align)for(unsigned target=0;target<sizeof(targets)/sizeof(targets[0]);++target){
        check(cyc_load_ines(rom,sizeof(rom)),"load NROM");cyc_power_on((uint8_t)align);
#endif
        for(unsigned i=0;i<sizeof(ppu.ciram);++i)ppu.ciram[i]=(uint8_t)(i*7u+(i>>5));
        for(unsigned i=0;i<32;++i)ppu.palette[i]=(uint8_t)i;
        memset(ppu.oam,0xff,sizeof(ppu.oam));
#if NESRECOMP_PPU_FRAME_PAL
        for(unsigned slot=0;pal && slot<16;++slot) {
            ppu.oam[slot*4]=76;ppu.oam[slot*4+1]=(uint8_t)slot;
            ppu.oam[slot*4+2]=(uint8_t)((slot&3)|((slot&1)?0x40:0));
            ppu.oam[slot*4+3]=(uint8_t)(32+slot*9);
        }
#endif
        write_cycle(0x4017,0x40);write_cycle(0x2000,0);write_cycle(0x2001,0x1e);
        while(ppu.scanline!=80 || ppu.dot<targets[target])read_cycle();
        check(ppu.scanline==80,"mid-line position");
#if NESRECOMP_PPU_FRAME_PAL
        uint64_t prefixes=ppu_native_oam_prefixes;
        uint8_t *file_state=NULL;size_t file_size=0;char error[128];
        if(targets[target]==128)
            check(cyc_state_save(&file_state,&file_size,error,sizeof(error)),"current-build file-state capture");
        CycSnapshot *snapshot=cyc_snapshot_new();check(snapshot!=NULL,"snapshot allocation");
        cyc_snapshot_take(snapshot);
        if(pal && targets[target]>=66 && targets[target]<=248)
            check(ppu_native_oam_prefixes>prefixes,"capture materializes pending canonical OAM prefix");
#else
        CycSnapshot *snapshot=cyc_snapshot_new();check(snapshot!=NULL,"snapshot allocation");
        cyc_snapshot_take(snapshot);
#endif
        unsigned dot=ppu.dot;
        for(unsigned i=0;i<1000;++i)read_cycle();
        uint64_t hardware=cyc_hw_state_hash(),memory=cyc_mem_state_hash();
        memcpy(expected,hw_frame_index,sizeof(expected));
        cyc_snapshot_restore(snapshot);
        for(unsigned i=0;i<1000;++i)read_cycle();
        check(hardware==cyc_hw_state_hash(),"current-build hardware replay");
        check(memory==cyc_mem_state_hash(),"current-build memory replay");
        check(!memcmp(expected,hw_frame_index,sizeof(expected)),"current-build picture replay");
#if NESRECOMP_PPU_FRAME_PAL
        if(file_state) {
            check(cyc_state_load(file_state,file_size,error,sizeof(error)),"current-build file-state restore");
            for(unsigned i=0;i<1000;++i)read_cycle();
            check(hardware==cyc_hw_state_hash(),"file-state hardware replay");
            check(memory==cyc_mem_state_hash(),"file-state memory replay");
            check(!memcmp(expected,hw_frame_index,sizeof(expected)),"file-state picture replay");
            free(file_state);
        }
        /* Packet execution and individual timestamp advances consume the
         * same real inputs, including clear/object and line-end boundaries. */
        if(!pal) {
            cyc_snapshot_restore(snapshot);
            frame_ppu_ticks(1,97);
            hardware=cyc_hw_state_hash();memory=cyc_mem_state_hash();
            memcpy(expected,hw_frame_index,sizeof(expected));
            cyc_snapshot_restore(snapshot);
            for(unsigned tick=1;tick<97;++tick)frame_ppu_ticks(tick,tick+1);
            check(hardware==cyc_hw_state_hash(),"packet/timestamp hardware equivalence");
            check(memory==cyc_mem_state_hash(),"packet/timestamp memory equivalence");
            check(!memcmp(expected,hw_frame_index,sizeof(expected)),"packet/timestamp picture equivalence");
        }
        if(pal && align==0 && targets[target]==128) {
            /* Rendering can resume after dot65 with a retained ninth-object
             * latch and an empty secondary buffer. That latch suppresses
             * selection even when the next primary Y is in range. */
            cyc_snapshot_restore(snapshot);ppu_sync_observers();
            ppu.eval_nine=1;ppu.eval_tick=ppu.eval_wrapped=0;
            ppu.oam_addr=ppu.oam2_addr=ppu.oam2_full=0;
            for(unsigned cycle=0;cycle<32;++cycle)read_cycle();
            ppu_sync_observers();
            check(!ppu.oam2_full && ppu.oam2_addr==0,"retained ninth-object latch suppresses new selection");
        }
#else
        /* Packet execution and individual timestamp advances consume the
         * same real inputs, including clear/object and line-end boundaries. */
        cyc_snapshot_restore(snapshot);
        frame_ppu_ticks(1,97);
        hardware=cyc_hw_state_hash();memory=cyc_mem_state_hash();
        memcpy(expected,hw_frame_index,sizeof(expected));
        cyc_snapshot_restore(snapshot);
        for(unsigned tick=1;tick<97;++tick)frame_ppu_ticks(tick,tick+1);
        check(hardware==cyc_hw_state_hash(),"packet/timestamp hardware equivalence");
        check(memory==cyc_mem_state_hash(),"packet/timestamp memory equivalence");
        check(!memcmp(expected,hw_frame_index,sizeof(expected)),"packet/timestamp picture equivalence");
#endif
        cyc_snapshot_free(snapshot);
        printf("PASS align=%u dot=%u native picture snapshot replay\n",align,dot);
    }
    /* A real address copy can land between a tile's nametable and plane
     * reads. Restore the pending input operation through every CPU/PPU
     * alignment; no derived picture prefix is reconstructed from memory. */
#if NESRECOMP_PPU_FRAME_PAL
    for(unsigned align=0;align<(pal?5u:4u);++align) {
        check(cyc_load_ines(rom,sizeof(rom)),"load pending-input NROM");check(cyc_set_region(pal?CYC_REGION_PAL:CYC_REGION_NTSC),"pending-input region");cyc_power_on((uint8_t)align);
#else
    for(unsigned align=0;align<4;++align) {
        check(cyc_load_ines(rom,sizeof(rom)),"load pending-input NROM");cyc_power_on((uint8_t)align);
#endif
        for(unsigned i=0;i<sizeof(ppu.ciram);++i)ppu.ciram[i]=(uint8_t)(i*7u+(i>>5));
        for(unsigned i=0;i<32;++i)ppu.palette[i]=(uint8_t)i;
        memset(ppu.oam,0xff,sizeof(ppu.oam));
        write_cycle(0x4017,0x40);write_cycle(0x2000,0);write_cycle(0x2001,0x1e);
        while(ppu.scanline!=80 || ppu.dot<323)read_cycle();
        write_cycle(0x2006,0);write_cycle(0x2006,0);
        check(ppu.w2006_delay!=0,"address copy still pending");
        CycSnapshot *snapshot=cyc_snapshot_new();check(snapshot!=NULL,"pending-input snapshot allocation");
        cyc_snapshot_take(snapshot);
        for(unsigned i=0;i<1000;++i)read_cycle();
        uint64_t hardware=cyc_hw_state_hash(),memory=cyc_mem_state_hash();
        memcpy(expected,hw_frame_index,sizeof(expected));
        cyc_snapshot_restore(snapshot);
        for(unsigned i=0;i<1000;++i)read_cycle();
        check(hardware==cyc_hw_state_hash(),"pending-input hardware replay");
        check(memory==cyc_mem_state_hash(),"pending-input memory replay");
        check(!memcmp(expected,hw_frame_index,sizeof(expected)),"pending-input picture replay");
        cyc_snapshot_free(snapshot);
        printf("PASS align=%u pending input/current-build replay\n",align);
    }
    return 0;
}
