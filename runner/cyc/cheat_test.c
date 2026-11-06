/* cheat_test.c - QA pokes and holds (cyc_cheat.h): parsing, frame scheduling,
 * clamps, conditions, partial UNHOLD, seeking past a loaded state and the ring
 * events (CTest cyc_cheat_test). The machine is a flat RAM stand-in. */
#include "cyc_cheat.h"
#include "cyc_ring.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint8_t mem[0x10000];
static unsigned ring_events, last_addr;
static uint32_t last_value;

bool cyc_mod_peek_ok(uint16_t addr, uint8_t *value) { *value = mem[addr]; return true; }
bool cyc_mod_poke(uint16_t addr, uint8_t value)
{
    if (addr >= 0x2000 && addr < 0x6000) return false;
    mem[addr] = value;
    return true;
}
void cyc_ring_push(CycRingKind kind, uint16_t addr, uint32_t value)
{
    if (kind == CYC_EV_CHEAT) ring_events++, last_addr = addr, last_value = value;
}

static bool sched(long frame, const char *text)
{
    CycCheat c;
    char err[96];
    if (!cyc_cheat_parse(text, &c, err, sizeof err)) { printf("parse '%s': %s\n", text, err); return false; }
    return cyc_cheat_schedule(frame, &c);
}

static bool rejects(const char *text)
{
    CycCheat c;
    char err[96];
    return !cyc_cheat_parse(text, &c, err, sizeof err);
}

int main(void)
{
    CHECK(cyc_cheat_is_command("POKE 0 1"));
    CHECK(cyc_cheat_is_command("  hold_max 06C1 01"));
    CHECK(cyc_cheat_is_command("UNHOLD ALL"));
    CHECK(!cyc_cheat_is_command("START"));
    CHECK(!cyc_cheat_is_command("HOLDX 0 1"));
    CHECK(!cyc_cheat_is_command("DISK_INSERT"));

    CHECK(rejects("POKE"));
    CHECK(rejects("POKE 2002 01"));           /* a register, not RAM */
    CHECK(rejects("POKE 0010-000F 01"));      /* reversed range */
    CHECK(rejects("POKE 0010 100"));          /* byte out of range */
    CHECK(rejects("HOLD 0010 01 junk"));
    CHECK(rejects("HOLD 0010 01 IF 00B1 ~ 02"));
    CHECK(rejects("UNHOLD ALL extra"));
    CHECK(!rejects("POKE $0010 0x7F # comment"));

    CHECK(sched(2, "POKE 002A 05"));
    CHECK(sched(1, "HOLD 06C0 1C"));
    CHECK(sched(1, "HOLD_MAX 06D0-06DF 01"));
    CHECK(sched(1, "HOLD_MIN 00A8 03"));
    CHECK(sched(1, "HOLD_MAX 06C1 01 IF 00B1 >= 02"));
    CHECK(sched(4, "UNHOLD 06D4-06D5"));
    CHECK(sched(6, "UNHOLD ALL"));

    mem[0x06C0] = 0x05; mem[0x06D0] = 0x08; mem[0x06D4] = 0x08; mem[0x00A8] = 0x09; mem[0x06C1] = 0x10;
    cyc_cheat_frame(0);                       /* nothing due yet */
    CHECK(mem[0x06C0] == 0x05 && cyc_cheat_hold_count() == 0);
    cyc_cheat_frame(1);
    CHECK(mem[0x06C0] == 0x1C);               /* hold */
    CHECK(mem[0x06D0] == 0x01 && mem[0x06D4] == 0x01); /* clamp down */
    CHECK(mem[0x00A8] == 0x09);               /* already above the floor */
    CHECK(mem[0x06C1] == 0x10);               /* condition false: $B1 = 0 */
    CHECK(mem[0x002A] == 0x00);
    CHECK(ring_events == 3 && last_addr == 0x06D4);
    CHECK(cyc_cheat_hold_count() == 4);

    mem[0x00B1] = 0x02; mem[0x06C0] = 0x00; mem[0x00A8] = 0x00;
    unsigned before = ring_events;
    cyc_cheat_frame(2);
    CHECK(mem[0x002A] == 0x05);               /* one poke */
    CHECK(mem[0x06C1] == 0x01);               /* condition now true */
    CHECK(mem[0x06C0] == 0x1C && mem[0x00A8] == 0x03);
    CHECK(ring_events == before + 4);
    CHECK(last_addr == 0x06C1 && last_value == ((uint32_t)CYC_CHEAT_HOLD_MAX << 16 | 0x10 << 8 | 0x01));

    mem[0x002A] = 0x00;
    before = ring_events;
    cyc_cheat_frame(3);
    CHECK(mem[0x002A] == 0x00);               /* pokes do not repeat */
    CHECK(ring_events == before);             /* holds already satisfied: no events */

    cyc_cheat_frame(4);                       /* UNHOLD 06D4-06D5 splits 06D0-06DF */
    CHECK(cyc_cheat_hold_count() == 5);
    mem[0x06D4] = 0x09; mem[0x06D6] = 0x09; mem[0x06D3] = 0x09;
    cyc_cheat_frame(5);
    CHECK(mem[0x06D4] == 0x09 && mem[0x06D6] == 0x01 && mem[0x06D3] == 0x01);

    cyc_cheat_frame(6);
    CHECK(cyc_cheat_hold_count() == 0);

    /* A state loaded at frame 10 adopts earlier holds, skips earlier pokes. */
    cyc_cheat_reset();
    memset(mem, 0, sizeof mem);
    CHECK(sched(3, "POKE 0010 AA"));
    CHECK(sched(5, "HOLD 0011 BB"));
    CHECK(sched(12, "POKE 0012 CC"));
    cyc_cheat_seek(10);
    CHECK(cyc_cheat_hold_count() == 1);
    cyc_cheat_frame(10);
    CHECK(mem[0x0010] == 0 && mem[0x0011] == 0xBB && mem[0x0012] == 0);
    cyc_cheat_frame(12);
    CHECK(mem[0x0012] == 0xCC);

    /* Immediate (TCP) application. */
    CycCheat c;
    char err[96];
    CHECK(cyc_cheat_parse("POKE 6000 42", &c, err, sizeof err) && cyc_cheat_apply_now(&c) && mem[0x6000] == 0x42);
    char text[256];
    cyc_cheat_describe(text, sizeof text);
    CHECK(strstr(text, "hold 0011-0011 BB") != NULL);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("cyc_cheat_test: ok\n");
    return 0;
}
