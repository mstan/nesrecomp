/* cyc_ring.c - see cyc_ring.h. */
#include "cyc_ring.h"

#include "hw_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t cyc_ring_frame;
uint8_t  cyc_ring_muted;

static CycRingEvent *ring;          /* allocated on first use, never freed */
static uint64_t ring_total;
static uint64_t kind_totals[CYC_EV_KINDS];

void cyc_ring_reset(void)
{
    ring_total = 0;
    memset(kind_totals, 0, sizeof(kind_totals));
    cyc_ring_frame = 0;
}

void cyc_ring_push(CycRingKind kind, uint16_t addr, uint32_t value)
{
    if ((unsigned)kind >= CYC_EV_KINDS || cyc_ring_muted) return;
    kind_totals[kind]++;
    if (!ring) {
        ring = (CycRingEvent *)calloc(CYC_RING_CAPACITY, sizeof(CycRingEvent));
        if (!ring) return;
    }
    if ((kind == CYC_EV_FDS_READ || kind == CYC_EV_RAM_INTERP || kind == CYC_EV_FDS_ENV) && ring_total) {
        CycRingEvent *last = &ring[(ring_total - 1) & (CYC_RING_CAPACITY - 1)];
        if (last->kind == kind && (last->addr == addr || kind == CYC_EV_RAM_INTERP) &&
            (last->value == value || kind == CYC_EV_FDS_ENV) && last->frame == cyc_ring_frame &&
            last->repeat != UINT32_MAX) {
            last->repeat++;
            last->value = value;
            return;
        }
    }
    CycRingEvent *e = &ring[ring_total & (CYC_RING_CAPACITY - 1)];
    e->cycle = hw.cycles;
    e->frame = cyc_ring_frame;
    e->kind = (uint16_t)kind;
    e->addr = addr;
    e->value = value;
    e->repeat = 0;
    ring_total++;
}

void cyc_ring_push_len(CycRingKind kind, uint16_t addr, uint32_t value, uint32_t length)
{
    if (cyc_ring_muted) return;
    cyc_ring_push(kind, addr, value);
    if (ring && ring_total && ring[(ring_total - 1) & (CYC_RING_CAPACITY - 1)].kind == (uint16_t)kind)
        ring[(ring_total - 1) & (CYC_RING_CAPACITY - 1)].repeat = length;
}

static bool has_length(unsigned kind)
{
    return kind == CYC_EV_FDS_WRITE_RUN || kind == CYC_EV_FDS_WRITE_BLOCK || kind == CYC_EV_FDS_SAVE ||
           kind == CYC_EV_FDS_LOAD || kind == CYC_EV_FDS_IDREQ || kind == CYC_EV_FDS_IDBYTES ||
           kind == CYC_EV_FDS_SPAN || kind == CYC_EV_FDS_HLE || kind == CYC_EV_MOD_HOOK || kind == CYC_EV_MOD_CALL ||
           kind == CYC_EV_MOD_FAIL || kind == CYC_EV_STATE || kind == CYC_EV_VIDEO;
}

uint64_t cyc_ring_total(void) { return ring_total; }

uint64_t cyc_ring_oldest(void)
{
    return ring_total > CYC_RING_CAPACITY ? ring_total - CYC_RING_CAPACITY : 0;
}

uint64_t cyc_ring_kind_total(CycRingKind kind)
{
    return (unsigned)kind < CYC_EV_KINDS ? kind_totals[kind] : 0;
}

bool cyc_ring_get(uint64_t index, CycRingEvent *out)
{
    if (!ring || index >= ring_total || index < cyc_ring_oldest()) return false;
    *out = ring[index & (CYC_RING_CAPACITY - 1)];
    return true;
}

const char *cyc_ring_kind_name(unsigned kind)
{
    static const char *const NAMES[CYC_EV_KINDS] = {
        "none", "fds.read", "fds.write", "fds.irq", "fds.ack", "fds.byte", "fds.motor",
        "fds.rewind", "fds.ready", "fds.end", "fds.side", "fds.crc",
        "fds.wrun", "fds.wblock", "fds.save", "fds.load",
        "view.valid", "view.reject", "view.invalid", "view.exit", "ram.interp", "view.frame",
        "fds.env", "fds.audio",
        "fds.idreq", "fds.idbytes", "fds.span", "fds.hle",
        "mod.hook", "mod.call", "mod.fail", "state", "video", "cheat",
    };
    return kind < CYC_EV_KINDS ? NAMES[kind] : "?";
}

static void describe(FILE *f, const CycRingEvent *e)
{
    switch (e->kind) {
    case CYC_EV_FDS_READ: case CYC_EV_FDS_WRITE:
        fprintf(f, "%04X %02X", e->addr, e->value & 0xFF);
        break;
    case CYC_EV_FDS_IRQ:
        fprintf(f, "%s%s", e->value & CYC_FDS_IRQ_TIMER ? "timer" : "", e->value & CYC_FDS_IRQ_DISK ? "disk" : "");
        break;
    case CYC_EV_FDS_IRQ_ACK:
        fprintf(f, "%04X cleared=%s%s", e->addr, e->value & CYC_FDS_IRQ_TIMER ? "timer " : "",
                e->value & CYC_FDS_IRQ_DISK ? "disk" : "");
        break;
    case CYC_EV_FDS_BYTE:
        fprintf(f, "pos=%u %s=%02X%s%s%s%s%s", e->value & 0xFFFFFF, e->addr & CYC_FDS_BYTE_WRITE ? "wrote" : "read",
                e->value >> 24, e->addr & CYC_FDS_BYTE_GAP_END ? " gap-end" : "",
                e->addr & CYC_FDS_BYTE_TRANSFER ? " transfer" : "", e->addr & CYC_FDS_BYTE_IRQ ? " irq" : "",
                e->addr & CYC_FDS_BYTE_CRC ? " crc" : "", e->addr & CYC_FDS_BYTE_STORED ? " stored" : "");
        break;
    case CYC_EV_FDS_MOTOR:
        fprintf(f, "%s%s", e->value ? "on" : "off", e->addr ? " (end of side)" : "");
        break;
    case CYC_EV_FDS_REWIND: fprintf(f, "delay=%u", e->value); break;
    case CYC_EV_FDS_READY: fprintf(f, "pos=%u", e->value); break;
    case CYC_EV_FDS_END: fprintf(f, "length=%u", e->value); break;
    case CYC_EV_FDS_SIDE:
        if (e->value == 0xFF) fprintf(f, "ejected");
        else fprintf(f, "side %u inserted", e->value);
        if (e->addr == 1) fprintf(f, " (power-on)");
        else if (e->addr == 2) fprintf(f, " (hle)");
        break;
    case CYC_EV_FDS_CRC: fprintf(f, "%s acc=%04X", e->addr ? "bad" : "good", e->value & 0xFFFF); break;
    case CYC_EV_FDS_WRITE_RUN:
        fprintf(f, "side=%u from=%u stored=%u changed=%u", e->value >> 24, e->value & 0xFFFFFF, e->repeat, e->addr);
        break;
    case CYC_EV_FDS_WRITE_BLOCK:
        fprintf(f, "side=%u code=%u mark=%u length=%u", e->addr >> 8, e->addr & 0xFF, e->value, e->repeat);
        break;
    case CYC_EV_FDS_SAVE: {
        static const char *const WHY[] = { "?", "idle", "eject", "exit", "timeout" };
        unsigned why = e->addr >> 1;
        fprintf(f, "%s (%s) sides=%u bytes=%u", e->addr & 1 ? "saved" : "FAILED", why < 5 ? WHY[why] : "?",
                e->value, e->repeat);
        break;
    }
    case CYC_EV_FDS_LOAD:
        fprintf(f, "%s sides=%u bytes=%u", e->addr ? "mesen-ips" : "save-file", e->value, e->repeat);
        break;
    case CYC_EV_VIEW_VALID: case CYC_EV_VIEW_REJECT: case CYC_EV_VIEW_EXIT:
        fprintf(f, "pc=%04X view=%u", e->addr, e->value);
        break;
    case CYC_EV_VIEW_INVALID: fprintf(f, "store=%04X view=%u", e->addr, e->value); break;
    case CYC_EV_RAM_INTERP: fprintf(f, "pc=%04X chunk=%04X", e->addr, e->value); break;
    case CYC_EV_VIEW_FRAME: fprintf(f, "entries=%u validated=%u", e->value, e->addr); break;
    case CYC_EV_FDS_ENV: fprintf(f, "%s gain=%u", e->addr ? "mod" : "volume", e->value); break;
    case CYC_EV_FDS_AUDIO: fprintf(f, "wave_steps=%u mod_steps=%u", e->value, e->addr); break;
    case CYC_EV_FDS_IDREQ:
        fprintf(f, "id@%04X matches=%X drive=", e->addr, e->value);
        if (e->repeat == 0xFF) fprintf(f, "empty");
        else fprintf(f, "%u", e->repeat);
        break;
    case CYC_EV_FDS_IDBYTES:
        fprintf(f, "id=");
        for (unsigned i = 0; i < 10; ++i) {
            uint32_t word = i < 4 ? e->value : i < 8 ? e->repeat : e->addr;
            fprintf(f, "%s%02X", i ? " " : "", (word >> (8 * (i & 3))) & 0xFF);
        }
        break;
    case CYC_EV_FDS_SPAN:
        fprintf(f, "first=%u frames=%u load_frames=%u%s", e->value, e->repeat, e->addr & 0x7FFF,
                e->addr & 0x8000 ? " fast" : "");
        break;
    case CYC_EV_FDS_HLE:
        switch (e->addr) {
        case CYC_FDS_HLE_CONFIG:
            fprintf(f, "config auto_swap=%u fast_load=%u id_check=%04X", e->value & 1, (e->value >> 1) & 1, e->repeat);
            break;
        case CYC_FDS_HLE_KEEP: fprintf(f, "keep side=%u matches=%X", e->value, e->repeat); break;
        case CYC_FDS_HLE_SWAP: fprintf(f, "swap side=%u matches=%X", e->value, e->repeat); break;
        case CYC_FDS_HLE_AMBIGUOUS: fprintf(f, "ambiguous matches=%X drive=%u", e->value, e->repeat); break;
        case CYC_FDS_HLE_NOMATCH: fprintf(f, "nomatch drive=%u", e->repeat); break;
        case CYC_FDS_HLE_WAIT: fprintf(f, "wait put_back=%u poll_frames=%u", e->value, e->repeat); break;
        case CYC_FDS_HLE_EJECT: fprintf(f, "eject side=%u hold=%u", e->value, e->repeat); break;
        case CYC_FDS_HLE_INSERT: fprintf(f, "insert side=%u round=%u", e->value, e->repeat); break;
        case CYC_FDS_HLE_CANCEL: fprintf(f, "cancel side=%u step=%u", e->value, e->repeat); break;
        default: fprintf(f, "code=%u value=%u length=%u", e->addr, e->value, e->repeat); break;
        }
        break;
    case CYC_EV_MOD_HOOK:
        fprintf(f, "site=%u pc=%04X fired=%u handled=%u", e->value & 0xFFFF, e->addr, e->repeat, e->value >> 16);
        break;
    case CYC_EV_MOD_CALL: fprintf(f, "routine=%04X calls=%u instructions=%u", e->addr, e->value, e->repeat); break;
    case CYC_EV_MOD_FAIL: {
        static const char *const WHY[] = { "?", "budget", "stack", "jam" };
        fprintf(f, "routine=%04X %s instructions=%u", e->addr, e->value < 4 ? WHY[e->value] : "?", e->repeat);
        break;
    }
    case CYC_EV_STATE: {
        static const char *const WHAT[] = { "save", "load", "refused" };
        fprintf(f, "%s frame=%u bytes=%u", e->addr < 3 ? WHAT[e->addr] : "?", e->value, e->repeat);
        break;
    }
    case CYC_EV_VIDEO: fprintf(f, "width=%u mode=%u drawable=%u", e->addr, e->value, e->repeat); break;
    case CYC_EV_CHEAT: {
        static const char *const MODE[] = { "poke", "hold", "hold_max", "hold_min" };
        unsigned m = e->value >> 16;
        fprintf(f, "%s addr=%04X old=%02X new=%02X", m < 4 ? MODE[m] : "?", e->addr, (e->value >> 8) & 0xFF,
                e->value & 0xFF);
        break;
    }
    default: fprintf(f, "addr=%04X value=%08X", e->addr, e->value); break;
    }
}

void cyc_ring_dump(void *file, uint32_t first_frame, uint32_t last_frame)
{
    FILE *f = (FILE *)file;
    fprintf(f, "# cyc event ring: %llu events recorded, %llu held (oldest index %llu), capacity %u\n",
            (unsigned long long)ring_total, (unsigned long long)(ring_total - cyc_ring_oldest()),
            (unsigned long long)cyc_ring_oldest(), CYC_RING_CAPACITY);
    fprintf(f, "# totals since power-on (repeats included):");
    for (unsigned k = 1; k < CYC_EV_KINDS; ++k)
        if (kind_totals[k]) fprintf(f, " %s=%llu", cyc_ring_kind_name(k), (unsigned long long)kind_totals[k]);
    fprintf(f, "\n# index frame cycle kind detail [xN folded repeats]\n");
    for (uint64_t i = cyc_ring_oldest(); i < ring_total; ++i) {
        const CycRingEvent *e = &ring[i & (CYC_RING_CAPACITY - 1)];
        if (e->frame < first_frame || e->frame > last_frame) continue;
        fprintf(f, "%llu %u %llu %s ", (unsigned long long)i, e->frame, (unsigned long long)e->cycle,
                cyc_ring_kind_name(e->kind));
        describe(f, e);
        if (e->repeat && !has_length(e->kind)) fprintf(f, " x%u", e->repeat + 1);
        fputc('\n', f);
    }
}

static const char *exit_path;

static void dump_at_exit(void)
{
    FILE *f = fopen(exit_path, "w");
    if (!f) return;
    cyc_ring_dump(f, 0, UINT32_MAX);
    fclose(f);
}

void cyc_ring_dump_at_exit_from_env(void)
{
    const char *p = getenv("NESRECOMP_CYC_RING_DUMP");
    if (p && *p && !exit_path) {
        exit_path = p;
        atexit(dump_at_exit);
    }
}
