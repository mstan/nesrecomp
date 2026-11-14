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
int main(void)
{
    static uint8_t rom[16+32768+8192];
    static uint16_t expected[256*240];
    memcpy(rom,"NES\032",4);rom[4]=2;rom[5]=1;
    for(unsigned tile=0;tile<256;++tile)for(unsigned row=0;row<8;++row){
        rom[16+32768+tile*16+row]=(uint8_t)(0x81u^(tile*17u+row*29u));
        rom[16+32768+tile*16+8+row]=(uint8_t)(0x55u^(tile*11u+row*19u));
    }
    const unsigned targets[]={20,128,257,258};
    for(unsigned align=0;align<4;++align)for(unsigned target=0;target<4;++target){
        check(cyc_load_ines(rom,sizeof(rom)),"load NROM");cyc_power_on((uint8_t)align);
        for(unsigned i=0;i<sizeof(ppu.ciram);++i)ppu.ciram[i]=(uint8_t)(i*7u+(i>>5));
        for(unsigned i=0;i<32;++i)ppu.palette[i]=(uint8_t)i;
        memset(ppu.oam,0xff,sizeof(ppu.oam));
        write_cycle(0x4017,0x40);write_cycle(0x2000,0);write_cycle(0x2001,0x1e);
        while(ppu.scanline!=80 || ppu.dot<targets[target])read_cycle();
        check(ppu.scanline==80,"mid-line position");
        CycSnapshot *snapshot=cyc_snapshot_new();check(snapshot!=NULL,"snapshot allocation");
        cyc_snapshot_take(snapshot);
        unsigned dot=ppu.dot;
        for(unsigned i=0;i<1000;++i)read_cycle();
        uint64_t hardware=cyc_hw_state_hash(),memory=cyc_mem_state_hash();
        memcpy(expected,hw_frame_index,sizeof(expected));
        cyc_snapshot_restore(snapshot);
        for(unsigned i=0;i<1000;++i)read_cycle();
        check(hardware==cyc_hw_state_hash(),"current-build hardware replay");
        check(memory==cyc_mem_state_hash(),"current-build memory replay");
        check(!memcmp(expected,hw_frame_index,sizeof(expected)),"current-build picture replay");
        cyc_snapshot_free(snapshot);
        printf("PASS align=%u dot=%u native picture snapshot replay\n",align,dot);
    }
    /* A nonempty retained input must survive restore while its first pixels
     * are still pending, independently of the global derived row cache. */
    check(cyc_load_ines(rom,sizeof(rom)),"load retained-prefix NROM");cyc_power_on(0);
    for(unsigned i=0;i<sizeof(ppu.ciram);++i)ppu.ciram[i]=(uint8_t)(i*7u+(i>>5));
    for(unsigned i=0;i<32;++i)ppu.palette[i]=(uint8_t)i;
    memset(ppu.oam,0xff,sizeof(ppu.oam));
    ppu.scanline=193;ppu.dot=10;ppu.frame_line_v=0;ppu.fine_x=5;
    ppu.show_bg=ppu.show_spr=1;ppu_state_reloaded();
    ppu_write(0x2001,0);check(ppu.frame_hold_status==1,"retained pattern captured");
    ppu.dot=334;ppu_write(0x2001,0x18);
    check(ppu.frame_hold_status==2 && ppu.frame_prefix_line==194,"late-resume prefix pending");
    ppu.scanline=194;ppu.dot=4;ppu_state_reloaded();
    CycSnapshot *snapshot=cyc_snapshot_new();check(snapshot!=NULL,"prefix snapshot allocation");
    cyc_snapshot_take(snapshot);
    for(unsigned i=0;i<1000;++i)read_cycle();
    uint64_t hardware=cyc_hw_state_hash(),memory=cyc_mem_state_hash();
    memcpy(expected,hw_frame_index,sizeof(expected));
    cyc_snapshot_restore(snapshot);
    for(unsigned i=0;i<1000;++i)read_cycle();
    check(hardware==cyc_hw_state_hash(),"retained-prefix hardware replay");
    check(memory==cyc_mem_state_hash(),"retained-prefix memory replay");
    check(!memcmp(expected,hw_frame_index,sizeof(expected)),"retained-prefix picture replay");
    cyc_snapshot_free(snapshot);puts("PASS retained-pattern prefix current-build replay");
    return 0;
}
