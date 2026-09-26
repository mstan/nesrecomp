/* EAN light/dark stimulus for Datach's photodiode. GS1 General Specifications
 * section 5.2: seven-module symbols, parity, guards and modulo-10 check digit.
 * https://www.gs1.org/standards/barcodes-epcrfid-id-keys/gs1-general-specifications
 * This is a simulated swipe at a chosen speed, not a decoder inside the ASIC. */
#ifndef NES_BARCODE_H
#define NES_BARCODE_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef struct {
    uint8_t light[192];
    unsigned length, cycles_per_module;
    uint64_t start;
} NesBarcode;
static inline void nes_barcode_pattern(NesBarcode *b, unsigned pattern, unsigned width)
{
    while (width) b->light[b->length++]=!((pattern>>--width)&1);
}
static inline bool nes_barcode_scan(NesBarcode *b, const char *text, unsigned interval, uint64_t cycle)
{
    if (!text || !interval || interval>1000000) return false;
    size_t n=strlen(text); unsigned digits[13],sum=0;
    if (n!=8 && n!=12 && n!=13) return false;
    for (unsigned i=0;i<n;++i) {
        if (text[i]<'0' || text[i]>'9') return false;
        digits[i]=text[i]-'0';
        sum+=digits[i]*(((n-1-i)&1)?3:1);
    }
    if (sum%10) return false;
    if (n==12) { memmove(digits+1,digits,12*sizeof(*digits)); digits[0]=0; n=13; }
    static const uint8_t left[10]={13,25,19,61,35,49,47,59,55,11};
    static const uint8_t parity[10]={0,11,13,14,19,25,28,21,22,26};
    memset(b,0,sizeof(*b)); b->start=cycle; b->cycles_per_module=interval;
    for (unsigned i=0;i<33;++i) b->light[b->length++]=1;
    nes_barcode_pattern(b,5,3);
    unsigned count=n==8?4:6,first=n==8?0:1;
    for (unsigned i=0;i<count;++i) {
        unsigned pattern=left[digits[first+i]];
        if (n==13 && ((parity[digits[0]]>>(5-i))&1)) {
            unsigned reversed=0;
            for (unsigned bit=0;bit<7;++bit) reversed=(reversed<<1)|((~pattern>>bit)&1);
            pattern=reversed;
        }
        nes_barcode_pattern(b,pattern,7);
    }
    nes_barcode_pattern(b,10,5);
    for (unsigned i=first+count;i<n;++i) nes_barcode_pattern(b,left[digits[i]]^127,7);
    nes_barcode_pattern(b,5,3);
    for (unsigned i=0;i<32;++i) b->light[b->length++]=1;
    return true;
}
static inline unsigned nes_barcode_read(const NesBarcode *b, uint64_t cycle)
{
    if (!b->cycles_per_module || cycle<b->start) return 0;
    uint64_t module=(cycle-b->start)/b->cycles_per_module;
    return module<b->length ? b->light[module]*8 : 0;
}
#endif
