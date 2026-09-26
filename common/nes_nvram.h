/* Stable raw save layout: PRG NVRAM then CHR NVRAM. Serial boards expose
 * EEPROM bytes instead of pretending they are CPU-addressable work RAM.
 * Battery-backed MMC5 ExRAM is appended after PRG/CHR NVRAM. */
#ifndef NES_NVRAM_H
#define NES_NVRAM_H
#include "nes_cart.h"
#include "nes_eeprom.h"
typedef struct { uint8_t *data[3]; size_t size[3]; } NesNvram;
static inline NesNvram nes_nvram_region(const NesCartInfo *c, uint8_t *prg,
                                        uint8_t *chr, NesEeprom *ee, uint8_t *exram, unsigned region)
{
    NesNvram n={{NULL,NULL,NULL},{0,0,0}};
    if (c->mapper==157 && region<=1) {
        unsigned chip=region?0:1;
        n.data[0]=ee[chip].data; n.size[0]=ee[chip].size; return n;
    }
    if (region) return n;
    if ((c->mapper==16 || c->mapper==159)) { n.data[0]=ee[0].data; n.size[0]=ee[0].size; }
    else {
        n.data[0]=prg+c->prg_ram; n.size[0]=c->prg_nvram;
        n.data[1]=chr+c->chr_ram; n.size[1]=c->chr_nvram;
        if (c->mapper==5 && (c->battery || c->prg_nvram)) { n.data[2]=exram; n.size[2]=1024; }
    }
    return n;
}
static inline bool nes_nvram_transfer(NesNvram n, void *buffer, size_t size, bool import)
{
    if (size!=n.size[0]+n.size[1]+n.size[2] || (!buffer && size)) return false;
    uint8_t *b=(uint8_t *)buffer;
    for (unsigned i=0;i<3;++i) if (n.size[i]) {
        if (import) memcpy(n.data[i],b,n.size[i]); else memcpy(b,n.data[i],n.size[i]);
        b+=n.size[i];
    }
    return true;
}
#endif
