/* Stable raw save layout: PRG NVRAM then CHR NVRAM. Serial boards expose
 * EEPROM bytes instead of pretending they are CPU-addressable work RAM. */
#ifndef NES_NVRAM_H
#define NES_NVRAM_H
#include "nes_cart.h"
#include "nes_eeprom.h"
typedef struct { uint8_t *data[2]; size_t size[2]; } NesNvram;
static inline NesNvram nes_nvram_region(const NesCartInfo *c, uint8_t *prg,
                                        uint8_t *chr, NesEeprom *ee, unsigned region)
{
    NesNvram n={{NULL,NULL},{0,0}};
    if (region) return n;
    if ((c->mapper==16 || c->mapper==159)) { n.data[0]=ee[0].data; n.size[0]=ee[0].size; }
    else {
        n.data[0]=prg+c->prg_ram; n.size[0]=c->prg_nvram;
        n.data[1]=chr+c->chr_ram; n.size[1]=c->chr_nvram;
    }
    return n;
}
static inline bool nes_nvram_transfer(NesNvram n, void *buffer, size_t size, bool import)
{
    if (size!=n.size[0]+n.size[1] || (!buffer && size)) return false;
    uint8_t *b=(uint8_t *)buffer;
    for (unsigned i=0;i<2;++i) if (n.size[i]) {
        if (import) memcpy(n.data[i],b,n.size[i]); else memcpy(b,n.data[i],n.size[i]);
        b+=n.size[i];
    }
    return true;
}
#endif
