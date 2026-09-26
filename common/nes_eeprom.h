/* Xicor X24C01/X24C02 serial EEPROM. These are not generic 24LCxx chips:
 * both have FOUR-byte write pages; X24C01 sends a seven-bit word address
 * and R/W instead of an I2C device address. All words are MSB first.
 * Xicor datasheets and measured Bandai wiring:
 * https://seesaawiki.jp/famicomcartridge/d/Bandai%20LZ93D50%20standard
 * Board decoding is separate in the runtime and oracle; this chip is shared.
 */
#ifndef NES_EEPROM_H
#define NES_EEPROM_H
#include <stdint.h>
#include <string.h>

enum { EE_IDLE, EE_COMMAND, EE_ADDRESS, EE_WRITE, EE_ACK, EE_READ, EE_HOST_ACK };
enum { NES_EEPROM_WRITE_CYCLES = 8949 }; /* ceil(5 ms * NTSC CPU clock) */
typedef struct {
    uint8_t data[256];
    uint16_t size;
    uint8_t scl, sda, drive, state, after_ack, bits, shift, address;
    uint8_t page[4], dirty, page_base, ack_phase;
    uint64_t busy_until;
} NesEeprom;

static inline void nes_eeprom_reset(NesEeprom *e)
{
    /* A power cut discards an unfinished programming cycle. */
    e->scl = e->sda = 1; e->drive = 0;
    e->state = EE_IDLE; e->after_ack = e->bits = e->shift = 0;
    e->address = e->dirty = e->page_base = e->ack_phase = 0;
    e->busy_until = 0;
    memset(e->page, 0, sizeof(e->page));
}
static inline void nes_eeprom_init(NesEeprom *e, unsigned size)
{
    memset(e, 0, sizeof(*e));
    e->size = (uint16_t)size;
    memset(e->data, 255, sizeof(e->data));
    nes_eeprom_reset(e);
}
static inline void nes_eeprom_clock(NesEeprom *e, uint64_t cycle)
{
    if (e->busy_until && cycle >= e->busy_until) {
        for (unsigned i=0; i<4; ++i)
            if (e->dirty & (1u<<i)) e->data[e->page_base+i] = e->page[i];
        e->busy_until = 0; e->dirty = 0;
    }
}
static inline unsigned nes_eeprom_read(const NesEeprom *e)
{
    return !e->drive; /* open-drain device output; board resolves the bus */
}
static inline void nes_eeprom_received(NesEeprom *e)
{
    unsigned next = EE_IDLE;
    if (e->state == EE_COMMAND) {
        if (e->size == 128) {
            e->address = e->shift >> 1;
            next = e->shift & 1 ? EE_READ : EE_WRITE;
        } else if ((e->shift & 0xfe) == 0xa0) {
            next = e->shift & 1 ? EE_READ : EE_ADDRESS;
        }
    } else if (e->state == EE_ADDRESS) {
        e->address = e->shift;
        next = EE_WRITE;
    } else {
        e->page_base = e->address & 0xfc;
        e->page[e->address & 3] = e->shift;
        e->dirty |= 1u << (e->address & 3);
        e->address = e->page_base | ((e->address+1) & 3);
        next = EE_WRITE;
    }
    e->after_ack = (uint8_t)next;
    e->state = EE_ACK; e->ack_phase = 0; e->bits = 0;
}
static inline void nes_eeprom_lines(NesEeprom *e, unsigned scl, unsigned sda, uint64_t cycle)
{
    if (!e->size) return;
    scl = !!scl; sda = !!sda;
    nes_eeprom_clock(e, cycle);
    if (e->busy_until) { e->scl=scl; e->sda=sda; return; }
    /* START/STOP require a transition on the resolved SDA wire. The master
     * releasing SDA while the chip holds ACK low is neither event. */
    if (scl && e->scl && sda != e->sda && !e->drive) {
        if (!sda) {
            e->state=EE_COMMAND; e->bits=e->shift=e->dirty=0;
        } else {
            e->state=EE_IDLE;
            if (e->dirty) e->busy_until=cycle+NES_EEPROM_WRITE_CYCLES;
        }
    } else if (scl && !e->scl) {
        if (e->state==EE_COMMAND || e->state==EE_ADDRESS || e->state==EE_WRITE) {
            e->shift=(uint8_t)((e->shift<<1)|sda);
            if (++e->bits==8) nes_eeprom_received(e);
        } else if (e->state==EE_READ) ++e->bits;
        else if (e->state==EE_HOST_ACK) {
            e->after_ack = sda ? EE_IDLE : EE_READ;
            e->ack_phase=1;
        } else if (e->state==EE_ACK && e->ack_phase==1) e->ack_phase=2;
    } else if (!scl && e->scl) {
        if (e->state==EE_ACK) {
            if (!e->ack_phase) {
                e->drive=e->after_ack!=EE_IDLE; e->ack_phase=1;
            } else if (e->ack_phase==2) {
                e->drive=0; e->state=e->after_ack; e->bits=0;
                if (e->state==EE_READ) e->drive=!(e->data[e->address]&128);
            }
        } else if (e->state==EE_READ) {
            if (e->bits==8) {
                e->address=(e->address+1)&(e->size-1);
                e->drive=0; e->state=EE_HOST_ACK; e->ack_phase=0;
            } else e->drive=!(e->data[e->address] & (128u>>e->bits));
        } else if (e->state==EE_HOST_ACK && e->ack_phase) {
            e->state=e->after_ack; e->bits=0;
            e->drive=e->state==EE_READ && !(e->data[e->address]&128);
        }
    }
    e->scl=(uint8_t)scl; e->sda=(uint8_t)sda;
}
/* Datach's two devices have independent clocks but share an open-drain SDA
 * wire. Settle changes in either output while preserving actual clock edges. */
static inline void nes_eeprom_pair(NesEeprom *e, unsigned scl0, unsigned scl1,
                                   unsigned master_sda, uint64_t cycle)
{
    for (unsigned pass=0;pass<3;++pass) {
        unsigned wire=!!master_sda && !e[0].drive && !e[1].drive;
        nes_eeprom_lines(&e[0],scl0,wire,cycle);
        nes_eeprom_lines(&e[1],scl1,wire,cycle);
    }
}
#endif
