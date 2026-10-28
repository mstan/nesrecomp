/* cyc_ramview.c - compiled views of RAM code; see cyc_ramview.h. */
#include "cyc_ramview.h"

#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_ring.h"
#include "hw_internal.h"
#include "../../common/nes_fds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CycRamViewStats cyc_ramview_stats;
uint8_t cyc_ram_code_dirty;

enum { RV_UNKNOWN, RV_VALID, RV_INVALID, RV_UNUSABLE };

static bool      prg_ram;          /* the board has the FDS's linear PRG RAM */
static bool      cart_ram;         /* qualified fixed8 KiB physical SRAM */
static uint32_t  view_count, usable_count;
static uint8_t  *state;            /* per view */
static uint32_t *cand_off, *cand;  /* CSR: views with an instruction start at a physical byte */
static uint32_t *watch_off, *watch;/* CSR: views folding a physical byte */
static uint8_t  *watch_byte;       /* ... and the byte each of them folds there */
static uint32_t  frame_entries, frame_validated;

/* Physical RAM byte (hw_code_watch index) of a CPU address a view may use,
 * or -1. CPU RAM is taken at $0000-$07FF only: a view at a mirror address
 * would be a different instruction stream. */
static inline int phys_of(uint16_t a)
{
    if (a < 0x800) return a;
    if (cart_ram && a >= 0x6000 && a < 0x8000) return (int)(HW_CODE_PRG_RAM + a - 0x6000u);
    if (prg_ram && a >= 0x6000 && a < 0xE000) return (int)(HW_CODE_PRG_RAM + (a - 0x6000u));
    return -1;
}
static inline uint16_t addr_of(unsigned phys) { return (uint16_t)(phys < HW_CODE_PRG_RAM ? phys : 0x6000u + phys - HW_CODE_PRG_RAM); }
static inline const uint8_t *phys_ptr(unsigned phys)
{
    return phys < HW_CODE_PRG_RAM ? &hw.ram[phys] : &hw_cart.wram[phys - HW_CODE_PRG_RAM];
}

/* ---- capture state ---- */
#define CAP_CHUNKS   (HW_CODE_BYTES / 1024u)
#define CAP_SNAP     1026u          /* the chunk and the two bytes an instruction can reach past it */
#define CAP_GROUPS   256u           /* snapshots kept per chunk */
typedef struct { uint8_t len, b[3]; uint32_t count; } CapInsn;
typedef struct { uint16_t n, cap; CapInsn *v; } CapInsns;
typedef struct { uint8_t data[CAP_SNAP]; uint8_t pcs[128]; uint32_t count; } CapGroup;
typedef struct { CapGroup *g; unsigned n, cap; } CapChunk;
/* A store that changed a byte some view folds to a value no view holds
 * there: where, the value, and the image of the first view folding it. The
 * compiler reads operands rewritten like this at run time and isolates
 * rewritten opcodes (a value that a disk file holds there is an overlay). */
typedef struct { uint32_t image, count; uint16_t addr; uint8_t value, used; } CapVolatile;
static bool      capturing;
static CapInsns *cap_insn;         /* [HW_CODE_BYTES] */
static CapChunk  cap_chunk[CAP_CHUNKS];
static CapVolatile *cap_vol;       /* open-addressed set */
static uint32_t  cap_vol_n, cap_vol_cap;
static uint32_t  cap_dropped;

static void add_volatile(uint16_t addr, uint8_t value, uint32_t image, uint32_t count)
{
    if ((cap_vol_n + 1) * 2 > cap_vol_cap) {
        uint32_t cap = cap_vol_cap ? cap_vol_cap * 2 : 1024;
        CapVolatile *grown = (CapVolatile *)calloc(cap, sizeof(CapVolatile));
        if (!grown) { cap_dropped++; return; }
        for (uint32_t i = 0; i < cap_vol_cap; ++i) {
            if (!cap_vol[i].used) continue;
            uint32_t h = (cap_vol[i].image ^ cap_vol[i].addr * 2654435761u ^ cap_vol[i].value * 40503u) & (cap - 1);
            while (grown[h].used) h = (h + 1) & (cap - 1);
            grown[h] = cap_vol[i];
        }
        free(cap_vol);
        cap_vol = grown;
        cap_vol_cap = cap;
    }
    uint32_t h = (image ^ addr * 2654435761u ^ value * 40503u) & (cap_vol_cap - 1);
    while (cap_vol[h].used) {
        CapVolatile *v = &cap_vol[h];
        if (v->image == image && v->addr == addr && v->value == value) {
            v->count = v->count + count < v->count ? UINT32_MAX : v->count + count;
            return;
        }
        h = (h + 1) & (cap_vol_cap - 1);
    }
    CapVolatile *v = &cap_vol[h];
    v->used = 1;
    v->image = image;
    v->addr = addr;
    v->value = value;
    v->count = count;
    cap_vol_n++;
}

/* ---- the write watch ---- */

static void on_code_write(unsigned phys, uint8_t value)
{
    bool known = false;
    for (uint32_t k = watch_off[phys]; k < watch_off[phys + 1]; ++k) {
        uint32_t i = watch[k];
        known = known || watch_byte[k] == value;
        if (state[i] == RV_VALID) {
            state[i] = RV_UNKNOWN;
            cyc_ram_code_dirty = 1;
            cyc_ramview_stats.invalidated++;
            cyc_ring_push(CYC_EV_VIEW_INVALID, addr_of(phys), i);
        } else if (state[i] == RV_INVALID) {
            state[i] = RV_UNKNOWN;   /* the store may have made RAM match */
        }
    }
    /* A value no compiled view holds here: the program rewrote code (a
     * value some view holds is that view's code being loaded). */
    if (capturing && !known && watch_off[phys] < watch_off[phys + 1])
        add_volatile(addr_of(phys), value, cyc_native_ram_views[watch[watch_off[phys]]]->image, 1);
}

static bool view_usable(const CycRamView *v)
{
    for (unsigned e = 0; e < v->entry_count; ++e)
        if (phys_of(v->entries[e]) < 0) return false;
    for (unsigned r = 0; r < v->run_count; ++r) {
        uint16_t a = v->runs[2 * r], n = v->runs[2 * r + 1];
        int first = phys_of(a), last = phys_of((uint16_t)(a + n - 1));
        if (!n || first < 0 || last < 0 || last - first != n - 1) return false;
        if (a < 0x200 && a + n > 0x100) return false;          /* never the stack page */
    }
    return true;
}

uint32_t cyc_ramview_count(void) { return usable_count; }

void cyc_ramview_power_on(void)
{
    memset(&cyc_ramview_stats, 0, sizeof(cyc_ramview_stats));
    frame_entries = frame_validated = 0;
    cyc_ram_code_dirty = 0;
    prg_ram = hw_cart.mapper == NES_FDS_MAPPER;
    cart_ram = hw_cart_code_ram_supported();
    hw_code_cart_watch = false;
    view_count = cyc_native_ram_view_count;
    free(state); free(cand_off); free(cand); free(watch_off); free(watch); free(watch_byte);
    state = NULL; cand_off = cand = watch_off = watch = NULL;
    watch_byte = NULL;
    usable_count = 0;
    memset(hw_code_watch, 0, sizeof(hw_code_watch));
    hw_code_write = on_code_write;
    if (!view_count) {
        cand_off = (uint32_t *)calloc(HW_CODE_BYTES + 1, sizeof(uint32_t));
        watch_off = (uint32_t *)calloc(HW_CODE_BYTES + 1, sizeof(uint32_t));
        return;
    }
    state = (uint8_t *)malloc(view_count);
    cand_off = (uint32_t *)calloc(HW_CODE_BYTES + 1, sizeof(uint32_t));
    watch_off = (uint32_t *)calloc(HW_CODE_BYTES + 1, sizeof(uint32_t));
    size_t entries = 0, folded = 0;
    for (uint32_t i = 0; i < view_count; ++i) {
        const CycRamView *v = cyc_native_ram_views[i];
        state[i] = view_usable(v) ? RV_UNKNOWN : RV_UNUSABLE;
        if (state[i] == RV_UNUSABLE) continue;
        usable_count++;
        if (cart_ram && v->entry_count && v->entries[0] >= 0x6000) hw_code_cart_watch = true;
        for (unsigned e = 0; e < v->entry_count; ++e) cand_off[phys_of(v->entries[e]) + 1]++, entries++;
        for (unsigned r = 0; r < v->run_count; ++r)
            for (unsigned k = 0; k < v->runs[2 * r + 1]; ++k)
                watch_off[phys_of((uint16_t)(v->runs[2 * r] + k)) + 1]++, folded++;
    }
    for (unsigned p = 0; p < HW_CODE_BYTES; ++p) {
        cand_off[p + 1] += cand_off[p];
        watch_off[p + 1] += watch_off[p];
    }
    cand = (uint32_t *)malloc(sizeof(uint32_t) * (entries ? entries : 1));
    watch = (uint32_t *)malloc(sizeof(uint32_t) * (folded ? folded : 1));
    watch_byte = (uint8_t *)malloc(folded ? folded : 1);
    uint32_t *cfill = (uint32_t *)malloc(sizeof(uint32_t) * HW_CODE_BYTES);
    uint32_t *wfill = (uint32_t *)malloc(sizeof(uint32_t) * HW_CODE_BYTES);
    memcpy(cfill, cand_off, sizeof(uint32_t) * HW_CODE_BYTES);
    memcpy(wfill, watch_off, sizeof(uint32_t) * HW_CODE_BYTES);
    for (uint32_t i = 0; i < view_count; ++i) {
        if (state[i] == RV_UNUSABLE) continue;
        const CycRamView *v = cyc_native_ram_views[i];
        for (unsigned e = 0; e < v->entry_count; ++e) cand[cfill[phys_of(v->entries[e])]++] = i;
        const uint8_t *b = v->bytes;
        for (unsigned r = 0; r < v->run_count; ++r)
            for (unsigned k = 0; k < v->runs[2 * r + 1]; ++k) {
                unsigned p = (unsigned)phys_of((uint16_t)(v->runs[2 * r] + k));
                watch_byte[wfill[p]] = *b++;
                watch[wfill[p]++] = i;
                hw_code_watch[p] = 1;
            }
    }
    free(cfill);
    free(wfill);
}

/* ---- dispatch ---- */

static bool matches(const CycRamView *v)
{
    const uint8_t *b = v->bytes;
    for (unsigned r = 0; r < v->run_count; ++r) {
        uint16_t n = v->runs[2 * r + 1];
        if (memcmp(phys_ptr((unsigned)phys_of(v->runs[2 * r])), b, n)) return false;
        b += n;
    }
    return true;
}

int cyc_ramview_find(uint16_t pc)
{
    if (hw_prg_modified && !(cart_ram && pc >= 0x6000 && pc < 0x8000)) return -1;
    if (cart_ram && pc >= 0x6000 && !hw_cart_code_ram_ready()) return -1;
    int phys = phys_of(pc);
    if (phys < 0 || !cand_off) return -1;
    uint32_t lo = cand_off[phys], hi = cand_off[phys + 1];
    if (lo == hi) return -1;
    for (uint32_t k = lo; k < hi; ++k)
        if (state[cand[k]] == RV_VALID) return (int)cand[k];
    for (uint32_t k = lo; k < hi; ++k) {
        uint32_t i = cand[k];
        if (state[i] != RV_UNKNOWN) continue;
        if (matches(cyc_native_ram_views[i])) {
            state[i] = RV_VALID;
            cyc_ramview_stats.validated++;
            frame_validated++;
            cyc_ring_push(CYC_EV_VIEW_VALID, pc, i);
            return (int)i;
        }
        state[i] = RV_INVALID;
        cyc_ramview_stats.rejected++;
        cyc_ring_push(CYC_EV_VIEW_REJECT, pc, i);
    }
    return -1;
}

void cyc_ramview_run(int index)
{
    do {
        cyc_ram_code_dirty = 0;
        cyc_ramview_stats.entries++;
        frame_entries++;
        /* The SRAM view already proved its folded bytes and both read gates.
         * Share the ROM service's device deadlines; general accesses, mapper
         * stores and all callbacks commit the cursor before observing state. */
        bool native_cart = cart_ram && cpu.pc >= 0x6000 && cpu.pc < 0x8000;
        if (native_cart) hw_native_begin();
        cyc_native_ram_views[index]->fn();
        if (native_cart) hw_native_end();
        if (cyc_ram_code_dirty) {
            cyc_ramview_stats.code_write_exits++;
            cyc_ring_push(CYC_EV_VIEW_EXIT, cpu.pc, (uint32_t)index);
        }
    } while (!hw_frame_done && !cyc_hook_hit && !cpu.jammed && (index = cyc_ramview_find(cpu.pc)) >= 0);
}

void cyc_ramview_frame_end(void)
{
    if (frame_entries || frame_validated)
        cyc_ring_push(CYC_EV_VIEW_FRAME, (uint16_t)(frame_validated > 0xFFFF ? 0xFFFF : frame_validated), frame_entries);
    frame_entries = frame_validated = 0;
}

size_t cyc_ramview_validity_size(void) { return state ? view_count : 0; }
void   cyc_ramview_validity_get(uint8_t *buf) { if (state) memcpy(buf, state, view_count); }
void   cyc_ramview_validity_set(const uint8_t *buf) { if (state) memcpy(state, buf, view_count); }

void cyc_ramview_revalidate(void)
{
    for (uint32_t i = 0; state && i < view_count; ++i)
        if (state[i] != RV_UNUSABLE) state[i] = RV_UNKNOWN;
    cyc_ram_code_dirty = 0;
}

void cyc_ramview_list(void *file)
{
    FILE *f = (FILE *)file;
    static const char *const NAMES[] = { "unknown", "valid", "invalid", "unusable" };
    fprintf(f, "# RAM views: index hash first-last entries folded-bytes runs state\n");
    for (uint32_t i = 0; i < view_count; ++i) {
        const CycRamView *v = cyc_native_ram_views[i];
        unsigned bytes = 0;
        for (unsigned r = 0; r < v->run_count; ++r) bytes += v->runs[2 * r + 1];
        fprintf(f, "view %u %08X %04X-%04X %u %u %u %s\n", i, v->hash, v->entries[0], v->entries[v->entry_count - 1],
                v->entry_count, bytes, v->run_count, state ? NAMES[state[i]] : "-");
    }
}

/* ---- capture ---- */

void cyc_ramview_capture_start(void)
{
    if (capturing) return;
    cap_insn = (CapInsns *)calloc(HW_CODE_BYTES, sizeof(CapInsns));
    capturing = cap_insn != NULL;
}

bool cyc_ramview_capturing(void) { return capturing; }

static uint8_t peek(uint16_t a)
{
    uint8_t v = 0;
    cyc_debug_peek(a, &v);
    return v;
}

/* An address whose operand bytes have been seen to differ between runs of
 * one opcode: only its opcode identifies the code there. */
static bool operand_varies(unsigned phys, uint8_t opcode)
{
    const CapInsns *s = &cap_insn[phys];
    const CapInsn *first = NULL;
    for (unsigned k = 0; k < s->n; ++k) {
        const CapInsn *c = &s->v[k];
        if (c->b[0] != opcode) continue;
        if (!first) first = c;
        else if (c->len != first->len || memcmp(c->b, first->b, c->len)) return true;
    }
    return false;
}

static void add_insn(unsigned phys, const uint8_t *b, uint8_t len, uint32_t count)
{
    CapInsns *s = &cap_insn[phys];
    for (unsigned k = 0; k < s->n; ++k) {
        CapInsn *c = &s->v[k];
        if (c->len == len && !memcmp(c->b, b, len)) {
            c->count = c->count + count < c->count ? UINT32_MAX : c->count + count;
            return;
        }
    }
    if (s->n == s->cap) {
        unsigned cap = s->cap ? s->cap * 2u : 2u;
        if (cap > 0xFFFF) { cap_dropped++; return; }
        CapInsn *grown = (CapInsn *)realloc(s->v, sizeof(CapInsn) * cap);
        if (!grown) { cap_dropped++; return; }
        s->v = grown;
        s->cap = (uint16_t)cap;
    }
    CapInsn *c = &s->v[s->n++];
    memset(c, 0, sizeof(*c));
    c->len = len;
    memcpy(c->b, b, len);
    c->count = count;
}

/* Does snapshot `data` (of the chunk at base) hold the instruction at pc as
 * its bytes b? Only the opcode counts where operands vary. */
static bool snapshot_holds(const uint8_t *data, uint16_t base, uint16_t pc, const uint8_t *b, uint8_t len)
{
    unsigned off = (unsigned)(pc - base);
    int phys = phys_of(pc);
    if (phys >= 0 && operand_varies((unsigned)phys, b[0])) len = 1;
    return off + len <= CAP_SNAP && !memcmp(data + off, b, len);
}

static void add_group_pc(unsigned chunk, uint16_t pc, const uint8_t *snapshot_or_null, const uint8_t *b, uint8_t len,
                         uint32_t count)
{
    CapChunk *c = &cap_chunk[chunk];
    uint16_t base = addr_of(chunk * 1024u);
    for (unsigned g = c->n; g-- > 0;) {
        CapGroup *G = &c->g[g];
        if (!snapshot_holds(G->data, base, pc, b, len)) continue;
        unsigned off = pc - base;
        G->pcs[off >> 3] |= (uint8_t)(1u << (off & 7));
        G->count = G->count + count < G->count ? UINT32_MAX : G->count + count;
        return;
    }
    if (c->n >= CAP_GROUPS) { cap_dropped++; return; }
    if (c->n == c->cap) {
        unsigned cap = c->cap ? c->cap * 2u : 2u;
        CapGroup *grown = (CapGroup *)realloc(c->g, sizeof(CapGroup) * cap);
        if (!grown) { cap_dropped++; return; }
        c->g = grown;
        c->cap = cap;
    }
    CapGroup *G = &c->g[c->n++];
    memset(G, 0, sizeof(*G));
    if (snapshot_or_null) memcpy(G->data, snapshot_or_null, CAP_SNAP);
    else for (unsigned k = 0; k < CAP_SNAP; ++k) G->data[k] = peek((uint16_t)(base + k));
    unsigned off = pc - base;
    G->pcs[off >> 3] |= (uint8_t)(1u << (off & 7));
    G->count = count;
}

void cyc_ramview_interp(uint16_t pc)
{
    cyc_ramview_stats.interp_insns++;
    cyc_ring_push(CYC_EV_RAM_INTERP, pc, pc & 0xFC00u);
    if (!capturing) return;
    int phys = phys_of(pc);
    if (phys < 0 || (pc >= 0x100 && pc < 0x200)) return;
    if (cart_ram && pc >= 0x6000 && !hw_cart_code_ram_ready()) return;
    uint8_t b[3];
    b[0] = peek(pc);
    uint8_t len = cpu6502_op_length[b[0]];
    if (!len || len > sizeof(b)) return;
    for (unsigned k = 1; k < len; ++k) b[k] = peek((uint16_t)(pc + k));
    add_insn((unsigned)phys, b, len, 1);
    add_group_pc((unsigned)phys / 1024u, pc, NULL, b, len, 1);
}

/* ---- capture file ----
 *
 *   # comments
 *   insn ADDR BYTES COUNT        an instruction as the interpreter ran it: its
 *                                address and its 1-3 bytes (hex, no spaces)
 *   code ADDR HASH COUNT PCS     a snapshot of the 1KB chunk at ADDR, taken when
 *                                the instructions at PCS (comma-separated) ran;
 *   data HEX                     the 1026 snapshot bytes (the chunk and the two
 *                                bytes past it); HASH is their FNV-1a
 *   volatile ADDR VALUE IMAGE COUNT
 *                                a store of VALUE changed ADDR, a byte compiled
 *                                views fold, to a value none of them holds there;
 *                                IMAGE: the first such view's image (CycRamView.image)
 */

static uint32_t fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static char *read_line(FILE *f)
{
    size_t cap = 256, n = 0;
    char *s = (char *)malloc(cap);
    int ch;
    while (s && (ch = fgetc(f)) != EOF) {
        if (n + 2 > cap) {
            char *grown = (char *)realloc(s, cap *= 2);
            if (!grown) { free(s); return NULL; }
            s = grown;
        }
        if (ch == '\n') break;
        if (ch != '\r') s[n++] = (char)ch;
    }
    if (!s) return NULL;
    if (ch == EOF && !n) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool parse_hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        int hi = hexval(s[2 * i]), lo = hi < 0 ? -1 : hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static void merge_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line, *pending = NULL;
    while ((line = pending ? pending : read_line(f)) != NULL) {
        pending = NULL;
        unsigned addr, count, value, image;
        char hex[16], hash[16];
        int used = 0;
        if (sscanf(line, "insn %x %15s %u", &addr, hex, &count) == 3) {
            uint8_t b[3];
            size_t len = strlen(hex) / 2;
            int phys = addr <= 0xFFFF ? phys_of((uint16_t)addr) : -1;
            if (phys >= 0 && len >= 1 && len <= 3 && parse_hex(hex, b, len)) add_insn((unsigned)phys, b, (uint8_t)len, count);
        } else if (sscanf(line, "volatile %x %x %x %u", &addr, &value, &image, &count) == 4) {
            if (addr <= 0xFFFF && value <= 0xFF) add_volatile((uint16_t)addr, (uint8_t)value, image, count);
        } else if (sscanf(line, "code %x %15s %u %n", &addr, hash, &count, &used) == 3 && used) {
            size_t pcs_len = strlen(line + used) + 1;
            char *pcs = (char *)malloc(pcs_len);
            if (pcs) memcpy(pcs, line + used, pcs_len);
            char *data = read_line(f);
            int phys = addr <= 0xFFFF ? phys_of((uint16_t)addr) : -1;
            uint8_t snap[CAP_SNAP];
            if (data && !strncmp(data, "data ", 5) && strlen(data + 5) == 2 * CAP_SNAP && parse_hex(data + 5, snap, CAP_SNAP) &&
                phys >= 0 && !((unsigned)phys & 1023u) && pcs) {
                for (char *t = strtok(pcs, ","); t; t = strtok(NULL, ",")) {
                    unsigned pc = (unsigned)strtoul(t, NULL, 16);
                    unsigned off = pc - addr;
                    if (off >= 1024) continue;
                    uint8_t len = cpu6502_op_length[snap[off]];
                    add_group_pc((unsigned)phys / 1024u, (uint16_t)pc, snap, snap + off, len, count);
                    count = 0;   /* the group's count goes with its first pc */
                }
            } else if (data && strncmp(data, "data ", 5)) {
                pending = data;  /* not this snapshot's data line: parse it next */
                data = NULL;
            }
            free(pcs);
            free(data);
        }
        free(line);
    }
    fclose(f);
}

long cyc_ramview_capture_write(const char *path, const char *program)
{
    if (!capturing) return -1;
    merge_file(path);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# RAM code that ran on the interpreter with no compiled view (cycle backend).\n"
               "# Written by %s --capture-log (merged across runs); game.toml [game] cycle_capture_file.\n"
               "# insn ADDR BYTES COUNT | code ADDR HASH COUNT PCS + data HEX (1026 bytes) | volatile ADDR VALUE IMAGE COUNT\n",
            program ? program : "cyc_interp");
    long insns = 0;
    for (unsigned p = 0; p < HW_CODE_BYTES; ++p)
        for (unsigned k = 0; k < cap_insn[p].n; ++k) {
            const CapInsn *c = &cap_insn[p].v[k];
            fprintf(f, "insn %04X ", addr_of(p));
            for (unsigned j = 0; j < c->len; ++j) fprintf(f, "%02X", c->b[j]);
            fprintf(f, " %u\n", c->count);
            insns++;
        }
    for (unsigned ch = 0; ch < CAP_CHUNKS; ++ch)
        for (unsigned g = 0; g < cap_chunk[ch].n; ++g) {
            const CapGroup *G = &cap_chunk[ch].g[g];
            uint16_t base = addr_of(ch * 1024u);
            fprintf(f, "code %04X %08X %u ", base, fnv(G->data, CAP_SNAP), G->count);
            bool first = true;
            for (unsigned off = 0; off < 1024; ++off)
                if ((G->pcs[off >> 3] >> (off & 7)) & 1) {
                    fprintf(f, "%s%04X", first ? "" : ",", (unsigned)(base + off));
                    first = false;
                }
            fprintf(f, "\ndata ");
            for (unsigned k = 0; k < CAP_SNAP; ++k) fprintf(f, "%02X", G->data[k]);
            fputc('\n', f);
        }
    for (uint32_t i = 0; i < cap_vol_cap; ++i)
        if (cap_vol[i].used)
            fprintf(f, "volatile %04X %02X %08X %u\n", cap_vol[i].addr, cap_vol[i].value, cap_vol[i].image, cap_vol[i].count);
    if (cap_dropped) fprintf(f, "# %u captures dropped (too many snapshots of one chunk)\n", cap_dropped);
    fclose(f);
    return insns;
}
