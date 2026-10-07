/* cyc_state.c - save states and the isolated-call snapshot; see cyc_state.h. */
#include "cyc_state.h"

#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_ramview.h"
#include "cyc_recomp.h"
#include "cyc_ring.h"
#include "cyc_trace.h"
#include "hw_fds.h"
#include "hw_internal.h"
#include "../../common/nes_fds.h"
#include "../include/mod_savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#define STATE_MAGIC   "CYCSTATE"
#define STATE_VERSION 2u
#define TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MOD_ID_BYTES  64u

static CycStateHost s_state_host;
void cyc_state_set_host(const CycStateHost *host) { if (host) s_state_host = *host; else memset(&s_state_host, 0, sizeof(s_state_host)); }

/* The sizes of the structures a state copies: a state is only ever loaded by
 * the build that wrote it, and a layout change must refuse it. */
static uint32_t layout_signature(void)
{
    size_t apu_size, hle_size;
    apu_state_ptr(&apu_size);
    fds_hle_state_ptr(&hle_size);
    const uint64_t sizes[] = { sizeof(Cpu6502), sizeof(HwMachine), sizeof(HwCart), sizeof(HwPpu), apu_size,
                               hle_size, sizeof(CycLine), cyc_native_ram_view_count
#if NESRECOMP_PPU_HLE
                               , 0x505055484c450001ull /* packed PPU schema, HLE only */
#endif
                             };
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
        for (int k = 0; k < 8; ++k) h = (h ^ (uint8_t)(sizes[i] >> (8 * k))) * 16777619u;
    return h;
}

typedef struct {
    char     magic[8];
    uint32_t version, prg_hash, cart_hash, layout, program_crc;
    uint32_t frame;                 /* cyc_ring_frame at the save */
} Header;

static uint32_t program_crc(void)
{
    const char *name = cyc_native_program_name ? cyc_native_program_name : "";
    return nes_crc32(0, (const uint8_t *)name, strlen(name));
}

/* ---- a growable output buffer ---- */
typedef struct { uint8_t *p; size_t len, cap; bool failed; } Buf;

static void put(Buf *b, const void *data, size_t n)
{
    if (b->failed) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1 << 20;
        while (cap < b->len + n) cap *= 2;
        uint8_t *grown = (uint8_t *)realloc(b->p, cap);
        if (!grown) { b->failed = true; return; }
        b->p = grown;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void chunk(Buf *b, uint32_t tag, const void *data, size_t n)
{
    uint32_t len = (uint32_t)n;
    put(b, &tag, 4);
    put(b, &len, 4);
    put(b, data, n);
}

/* ---- sections ---- */

static void cart_copy_out(HwCart *out)
{
    *out = hw_cart;
    out->prg = NULL;
    out->chr = NULL;
}

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) snprintf(err, n, "%s", msg);
}

bool cyc_state_save(uint8_t **out, size_t *len, char *err, size_t err_len)
{
    Buf b = { 0 };
    Header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, STATE_MAGIC, 8);
    h.version = hw_pal() ? 4u : hw_zapper.port ? 3u : STATE_VERSION;
    h.prg_hash = cyc_prg_hash();
    h.cart_hash = cyc_native_cart_hash;
    h.layout = layout_signature();
    h.program_crc = program_crc();
    h.frame = cyc_ring_frame;
    put(&b, &h, sizeof(h));

    chunk(&b, TAG('C', 'P', 'U', ' '), &cpu, sizeof(cpu));
    chunk(&b, TAG('H', 'W', ' ', ' '), &hw, sizeof(hw));
    chunk(&b, TAG('T', 'I', 'M', 'E'), &hw_extra_timing, sizeof(hw_extra_timing));
    if (hw_pal()) chunk(&b, TAG('R', 'E', 'G', 'N'), &hw_region_timing, sizeof(hw_region_timing));
    if (hw_zapper.port) chunk(&b, TAG('Z', 'A', 'P', 'P'), &hw_zapper, sizeof(hw_zapper));
    HwCart *cart = (HwCart *)malloc(sizeof(HwCart));
    if (!cart) { free(b.p); set_err(err, err_len, "out of memory"); return false; }
    cart_copy_out(cart);
    chunk(&b, TAG('C', 'A', 'R', 'T'), cart, sizeof(*cart));
    free(cart);
    if (hw_cart.chr_ram_len) chunk(&b, TAG('C', 'H', 'R', 'R'), hw_cart.chr + hw_cart.chr_ram_base, hw_cart.chr_ram_len);
    chunk(&b, TAG('P', 'P', 'U', ' '), &ppu, sizeof(ppu));
    chunk(&b, TAG('P', 'I', 'C', 'I'), hw_frame_index, sizeof(hw_frame_index));
    chunk(&b, TAG('P', 'I', 'C', 'L'), hw_frame_lines, sizeof(hw_frame_lines));
    chunk(&b, TAG('P', 'I', 'C', 'B'), hw_frame_bg, sizeof(hw_frame_bg));
    size_t apu_size;
    void *apu = apu_state_ptr(&apu_size);
    chunk(&b, TAG('A', 'P', 'U', ' '), apu, apu_size);
    /* The comparison trace's running hash and cycle number (cyc_trace.h), so
     * a loaded run's --hash-out continues the saved run's lines exactly. */
    uint8_t trace[12];
    memcpy(trace, &cyc_trace_hash, 8);
    memcpy(trace + 8, &cyc_trace_cycle, 4);
    chunk(&b, TAG('T', 'R', 'A', 'C'), trace, sizeof(trace));
    if (cyc_is_fds()) {
        size_t need = fds_media_serialize(NULL, 0);
        uint8_t *m = (uint8_t *)malloc(need);
        if (!m) { free(b.p); set_err(err, err_len, "out of memory"); return false; }
        fds_media_serialize(m, need);
        chunk(&b, TAG('F', 'D', 'S', 'M'), m, need);
        free(m);
        size_t hle_size;
        void *hle = fds_hle_state_ptr(&hle_size);
        chunk(&b, TAG('F', 'D', 'S', 'H'), hle, hle_size);
    }
    if (s_state_host.save) {
        size_t need = s_state_host.save(NULL, 0);
        uint8_t *hb = (uint8_t *)malloc(need ? need : 1);
        if (!hb || s_state_host.save(hb, need) != need) { free(hb); free(b.p); set_err(err, err_len, "host section failed"); return false; }
        chunk(&b, TAG('H', 'O', 'S', 'T'), hb, need);
        free(hb);
    }
    /* Mods: one record per registered hook; a hook that cannot serialize
     * fails the save rather than dropping its state. */
    uint8_t *blob = (uint8_t *)malloc(NES_MOD_SAVESTATE_BLOB_CAP + MOD_ID_BYTES);
    if (!blob) { free(b.p); set_err(err, err_len, "out of memory"); return false; }
    for (int i = 0; i < nes_mod_savestate_hook_count(); ++i) {
        const char *id = nes_mod_savestate_hook_id_at(i);
        NESModSavestateGet get = nes_mod_savestate_hook_get_at(i);
        if (!id || !get) continue;
        memset(blob, 0, MOD_ID_BYTES);
        snprintf((char *)blob, MOD_ID_BYTES, "%s", id);
        int n = get(blob + MOD_ID_BYTES, NES_MOD_SAVESTATE_BLOB_CAP);
        if (n < 0 || n > NES_MOD_SAVESTATE_BLOB_CAP) {
            char msg[160];
            snprintf(msg, sizeof(msg), "mod state '%s' does not fit; nothing saved", id);
            free(blob);
            free(b.p);
            set_err(err, err_len, msg);
            return false;
        }
        chunk(&b, TAG('M', 'O', 'D', ' '), blob, MOD_ID_BYTES + (size_t)n);
    }
    free(blob);
    chunk(&b, TAG('E', 'N', 'D', ' '), NULL, 0);
    if (b.failed) { free(b.p); set_err(err, err_len, "out of memory"); return false; }
    *out = b.p;
    *len = b.len;
    cyc_ring_push_len(CYC_EV_STATE, CYC_STATE_EV_SAVE, cyc_ring_frame, (uint32_t)b.len);
    return true;
}

typedef struct {
    uint32_t tag, len;
    const uint8_t *data;
} Chunk;

#define MAX_CHUNKS 256

static int parse(const uint8_t *data, size_t len, Chunk *c, char *err, size_t err_len)
{
    Header h;
    if (len < sizeof(h)) { set_err(err, err_len, "not a save state"); return -1; }
    memcpy(&h, data, sizeof(h));
    if (memcmp(h.magic, STATE_MAGIC, 8)) { set_err(err, err_len, "not a save state"); return -1; }
    if (h.version < 1u || h.version > 4u) { set_err(err, err_len, "save state of another version"); return -1; }
    if (h.prg_hash != cyc_prg_hash() || h.cart_hash != cyc_native_cart_hash || h.program_crc != program_crc()) {
        set_err(err, err_len, "save state of another program");
        return -1;
    }
    if (h.layout != layout_signature()) { set_err(err, err_len, "save state of another build"); return -1; }
    size_t at = sizeof(h);
    int n = 0;
    for (;;) {
        if (len - at < 8 || n == MAX_CHUNKS) { set_err(err, err_len, "truncated save state"); return -1; }
        memcpy(&c[n].tag, data + at, 4);
        memcpy(&c[n].len, data + at + 4, 4);
        at += 8;
        if (len - at < c[n].len) { set_err(err, err_len, "truncated save state"); return -1; }
        c[n].data = data + at;
        at += c[n].len;
        if (c[n].tag == TAG('E', 'N', 'D', ' ')) break;
        n++;
    }
    if (at != len) { set_err(err, err_len, "trailing bytes after the save state"); return -1; }
    return n;
}

static const Chunk *find(const Chunk *c, int n, uint32_t tag)
{
    for (int i = 0; i < n; ++i)
        if (c[i].tag == tag) return &c[i];
    return NULL;
}

static bool sized(const Chunk *c, size_t size) { return c && c->len == size; }

bool cyc_state_load(const uint8_t *data, size_t len, char *err, size_t err_len)
{
    Chunk *c = (Chunk *)malloc(sizeof(Chunk) * MAX_CHUNKS);
    if (!c) { set_err(err, err_len, "out of memory"); return false; }
    int n = parse(data, len, c, err, err_len);
    bool ok = n >= 0;
    size_t apu_size = 0, hle_size = 0;
    void *apu = apu_state_ptr(&apu_size);
    void *hle = fds_hle_state_ptr(&hle_size);
    const Chunk *ccpu = NULL, *chw = NULL, *ccart = NULL, *cchr = NULL, *cppu = NULL, *cpi = NULL, *cpl = NULL,
                *cpb = NULL, *capu = NULL, *cmed = NULL, *chle = NULL, *chost = NULL, *ctrace = NULL, *ctime = NULL, *czapper = NULL, *cregion = NULL;
    HwExtraTiming timing = {0};
    HwRegionTiming region = {0};
    HwZapper zapper = {0};
    zapper.x = zapper.y = -1;
    if (ok) {
        ccpu = find(c, n, TAG('C', 'P', 'U', ' '));
        chw = find(c, n, TAG('H', 'W', ' ', ' '));
        ccart = find(c, n, TAG('C', 'A', 'R', 'T'));
        cchr = find(c, n, TAG('C', 'H', 'R', 'R'));
        cppu = find(c, n, TAG('P', 'P', 'U', ' '));
        cpi = find(c, n, TAG('P', 'I', 'C', 'I'));
        cpl = find(c, n, TAG('P', 'I', 'C', 'L'));
        cpb = find(c, n, TAG('P', 'I', 'C', 'B'));
        capu = find(c, n, TAG('A', 'P', 'U', ' '));
        cmed = find(c, n, TAG('F', 'D', 'S', 'M'));
        chle = find(c, n, TAG('F', 'D', 'S', 'H'));
        chost = find(c, n, TAG('H', 'O', 'S', 'T'));
        ctrace = find(c, n, TAG('T', 'R', 'A', 'C'));
        ctime = find(c, n, TAG('T', 'I', 'M', 'E'));
        czapper = find(c, n, TAG('Z', 'A', 'P', 'P'));
        cregion = find(c, n, TAG('R', 'E', 'G', 'N'));
        ok = sized(ccpu, sizeof(cpu)) && sized(chw, sizeof(hw)) && sized(ccart, sizeof(HwCart)) &&
             sized(cppu, sizeof(ppu)) && sized(cpi, sizeof(hw_frame_index)) && sized(cpl, sizeof(hw_frame_lines)) &&
             sized(cpb, sizeof(hw_frame_bg)) && sized(capu, apu_size) && sized(ctrace, 12) &&
             (hw_cart.chr_ram_len ? sized(cchr, hw_cart.chr_ram_len) : !cchr);
        if (!ok) set_err(err, err_len, "save state sections do not match this machine");
        if (ok) {
            Header header;
            memcpy(&header, data, sizeof(header));
            ok = header.version == 1u ? !ctime : sized(ctime, sizeof(timing));
            if (ok && ctime) memcpy(&timing, ctime->data, sizeof(timing));
            ok = ok && timing.extra_scanlines <= 262 && timing.line <= 262 && timing.active <= 1 &&
                 (!timing.extra_scanlines || cyc_extra_scanlines_supported()) &&
                 (!timing.active || timing.line != 0) &&
                 (timing.active || timing.line == 0) &&
                 !timing.reserved[0] && !timing.reserved[1] && !timing.reserved[2];
            if (!ok) set_err(err, err_len, "invalid CPU budget enhancement state");
            if (ok) {
                ok = header.version == 3u ? sized(czapper, sizeof(zapper)) : header.version == 4u ? (!czapper || sized(czapper, sizeof(zapper))) : !czapper;
                if (ok && czapper) {
                    memcpy(&zapper, czapper->data, sizeof(zapper));
                    ok = zapper.port >= 1 && zapper.port <= 2 && zapper.trigger <= 1 &&
                         ((zapper.x == -1 && zapper.y == -1) ||
                          (zapper.x >= 0 && zapper.x < 256 && zapper.y >= 0 && zapper.y < 240)) &&
                         !zapper.reserved[0] && !zapper.reserved[1] &&
                         (zapper.light_until <= zapper.dots || zapper.light_until - zapper.dots <= 20u * 341u);
                }
                if (!ok) set_err(err, err_len, "invalid Zapper state");
            }
            if (ok) {
                ok = header.version == 4u ? sized(cregion, sizeof(region)) : !cregion;
                if (ok && cregion) memcpy(&region, cregion->data, sizeof(region));
                ok = ok && region.region == hw_region_timing.region &&
                     (region.region == CYC_REGION_NTSC ? region.phase == 0 : region.phase < 5) &&
                     !region.reserved[0] && !region.reserved[1] && !region.reserved[2] &&
                     !region.reserved[3] && !region.reserved[4] && !region.reserved[5];
                if (ok && header.version == 4u) {
                    HwMachine machine;
                    HwPpu picture;
                    memcpy(&machine, chw->data, sizeof(machine));
                    memcpy(&picture, cppu->data, sizeof(picture));
                    ok = region.region == CYC_REGION_PAL && machine.tick <= 16 && machine.align < 4 &&
                         picture.scanline <= 311 && picture.dot <= 340;
                }
                if (!ok) set_err(err, err_len, "save state has incompatible region or invalid clock phase");
            }
        }
    }
    if (ok && cyc_is_fds()) {
        ok = cmed && sized(chle, hle_size) && fds_media_deserialize(cmed->data, cmed->len, false);
        if (!ok) set_err(err, err_len, "save state of another disk image or FDS options");
    } else if (ok && (cmed || chle)) {
        ok = false;
        set_err(err, err_len, "save state of a disk program");
    }
    if (ok && (s_state_host.load ? !chost || !s_state_host.load(chost->data, chost->len, false) : chost != NULL)) {
        ok = false;
        set_err(err, err_len, "save state's host section does not match this host");
    }
    /* Every mod validates its record (or its absence) before anything changes. */
    for (int i = 0; ok && i < nes_mod_savestate_hook_count(); ++i) {
        NESModSavestateValidate validate = nes_mod_savestate_hook_validate_at(i);
        if (!validate) continue;
        const char *id = nes_mod_savestate_hook_id_at(i);
        const Chunk *rec = NULL;
        for (int k = 0; k < n; ++k)
            if (c[k].tag == TAG('M', 'O', 'D', ' ') && c[k].len >= MOD_ID_BYTES &&
                !strncmp((const char *)c[k].data, id, MOD_ID_BYTES))
                rec = &c[k];
        if (!validate(rec ? rec->data + MOD_ID_BYTES : NULL, rec ? (int)(rec->len - MOD_ID_BYTES) : 0)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "mod state '%s' is incompatible; nothing loaded", id);
            set_err(err, err_len, msg);
            ok = false;
        }
    }
    if (!ok) {
        free(c);
        cyc_ring_push_len(CYC_EV_STATE, CYC_STATE_EV_REFUSED, cyc_ring_frame, 0);
        return false;
    }

    /* Apply: the machine, then the host, then the mods (which see the loaded
     * machine). */
    memcpy(&cpu, ccpu->data, sizeof(cpu));
    memcpy(&hw, chw->data, sizeof(hw));
    hw_extra_timing = timing;
    hw_zapper = zapper;
    hw_region_timing = region;
    uint8_t *prg = hw_cart.prg, *chr = hw_cart.chr;
    memcpy(&hw_cart, ccart->data, sizeof(HwCart));
    hw_cart.prg = prg;
    hw_cart.chr = chr;
    if (cchr) memcpy(hw_cart.chr + hw_cart.chr_ram_base, cchr->data, cchr->len);
    memcpy(&ppu, cppu->data, sizeof(ppu));
    memcpy(hw_frame_index, cpi->data, sizeof(hw_frame_index));
    memcpy(hw_frame_lines, cpl->data, sizeof(hw_frame_lines));
    memcpy(hw_frame_bg, cpb->data, sizeof(hw_frame_bg));
    memcpy(apu, capu->data, apu_size);
    memcpy(&cyc_trace_hash, ctrace->data, 8);
    memcpy(&cyc_trace_cycle, ctrace->data + 8, 4);
    if (cyc_is_fds()) {
        fds_media_deserialize(cmed->data, cmed->len, true);
        CycFdsHle cfg;
        memcpy(&cfg, hle, sizeof(cfg));            /* the plan is the host's: keep it */
        memcpy(hle, chle->data, hle_size);
        memcpy(hle, &cfg, sizeof(cfg));
    }
    ppu_state_reloaded();
    hw_cart_sound_reloaded();
    cyc_ramview_revalidate();
    hw_frame_done = hw_observe_hit = hw_frame_end_hit = false;
    cyc_hook_passed = -1;
    cyc_hook_hit = false;
    Header h;
    memcpy(&h, data, sizeof(h));
    cyc_ring_frame = h.frame;
    if (s_state_host.load) s_state_host.load(chost->data, chost->len, true);
    for (int i = 0; i < nes_mod_savestate_hook_count(); ++i) {
        const char *id = nes_mod_savestate_hook_id_at(i);
        NESModSavestateSet set = nes_mod_savestate_hook_find_set(id);
        if (!set) continue;
        const Chunk *rec = NULL;
        for (int k = 0; k < n; ++k)
            if (c[k].tag == TAG('M', 'O', 'D', ' ') && c[k].len >= MOD_ID_BYTES &&
                !strncmp((const char *)c[k].data, id, MOD_ID_BYTES))
                rec = &c[k];
        /* No record: the mod starts from its empty state (it was not in the
         * running game when the state was saved). */
        if (!set(rec ? rec->data + MOD_ID_BYTES : NULL, rec ? (int)(rec->len - MOD_ID_BYTES) : 0))
            fprintf(stderr, "[cyc state] mod '%s' did not take its state\n", id);
    }
    cyc_ring_push_len(CYC_EV_STATE, CYC_STATE_EV_LOAD, cyc_ring_frame, (uint32_t)len);
    free(c);
    return true;
}

bool cyc_state_save_file(const char *path, char *err, size_t err_len)
{
    uint8_t *data;
    size_t len;
    if (!cyc_state_save(&data, &len, err, err_len)) return false;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f) ok = fflush(f) == 0 && ok, fclose(f);
    free(data);
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) {
        remove(tmp);
        set_err(err, err_len, "cannot write the save state file");
    }
    return ok;
}

bool cyc_state_load_file(const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, err_len, "no save state there"); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    bool ok = data && fread(data, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(data); set_err(err, err_len, "cannot read the save state file"); return false; }
    ok = cyc_state_load(data, (size_t)n, err, err_len);
    free(data);
    return ok;
}

/* ---- the in-process snapshot ---- */

struct CycSnapshot {
    Cpu6502   cpu;
    HwMachine hw;
    HwExtraTiming timing;
    HwZapper zapper;
    HwRegionTiming region;
    HwCart   *cart;
    uint8_t  *chr, *apu, *media, *hle, *sound, *views;
    size_t    chr_len, apu_len, media_len, hle_len, sound_len, views_len;
    HwPpu     ppu;
    CycRamViewStats view_stats;
    uint8_t   code_dirty;
    bool      frame_done, observe_hit, frame_end_hit;
    int       dma_stalls;
};

CycSnapshot *cyc_snapshot_new(void)
{
    CycSnapshot *s = (CycSnapshot *)calloc(1, sizeof(CycSnapshot));
    if (!s) return NULL;
    size_t apu_len, hle_len;
    apu_state_ptr(&apu_len);
    fds_hle_state_ptr(&hle_len);
    s->cart = (HwCart *)malloc(sizeof(HwCart));
    s->chr_len = hw_cart.chr_ram_len;
    s->chr = (uint8_t *)malloc(s->chr_len ? s->chr_len : 1);
    s->apu_len = apu_len;
    s->apu = (uint8_t *)malloc(apu_len);
    s->media_len = fds_media_snapshot_size();
    s->media = (uint8_t *)malloc(s->media_len);
    s->hle_len = hle_len;
    s->hle = (uint8_t *)malloc(hle_len);
    s->sound_len = hw_cart_sound_snapshot_size();
    s->sound = (uint8_t *)malloc(s->sound_len ? s->sound_len : 1);
    s->views_len = cyc_ramview_validity_size();
    s->views = (uint8_t *)malloc(s->views_len ? s->views_len : 1);
    if (!s->cart || !s->chr || !s->apu || !s->media || !s->hle || !s->sound || !s->views) {
        cyc_snapshot_free(s);
        return NULL;
    }
    return s;
}

void cyc_snapshot_free(CycSnapshot *s)
{
    if (!s) return;
    free(s->cart); free(s->chr); free(s->apu); free(s->media); free(s->hle); free(s->sound); free(s->views);
    free(s);
}

void cyc_snapshot_take(CycSnapshot *s)
{
    size_t n;
    s->cpu = cpu;
    s->hw = hw;
    s->timing = hw_extra_timing;
    s->zapper = hw_zapper;
    s->region = hw_region_timing;
    memcpy(s->cart, &hw_cart, sizeof(HwCart));
    if (s->chr_len) memcpy(s->chr, hw_cart.chr + hw_cart.chr_ram_base, s->chr_len);
    s->ppu = ppu;
    memcpy(s->apu, apu_state_ptr(&n), s->apu_len);
    fds_media_snapshot(s->media);
    memcpy(s->hle, fds_hle_state_ptr(&n), s->hle_len);
    if (s->sound_len) hw_cart_sound_snapshot(s->sound);
    if (s->views_len) cyc_ramview_validity_get(s->views);
    s->view_stats = cyc_ramview_stats;
    s->code_dirty = cyc_ram_code_dirty;
    s->frame_done = hw_frame_done;
    s->observe_hit = hw_observe_hit;
    s->frame_end_hit = hw_frame_end_hit;
    s->dma_stalls = hw_dma_stalls;
}

const uint8_t *cyc_snapshot_cpu_ram(const CycSnapshot *s) { return s->hw.ram; }

const uint8_t *cyc_snapshot_cart_ram(const CycSnapshot *s, size_t *len)
{
    size_t n = hw_cart.mapper == NES_FDS_MAPPER ? 0x8000u : hw_cart.has_wram ? hw_cart.wram_len : 0u;
    if (n > sizeof(s->cart->wram)) n = sizeof(s->cart->wram);
    *len = n;
    return s->cart->wram;
}

void cyc_snapshot_restore(const CycSnapshot *s)
{
    size_t n;
    cpu = s->cpu;
    hw = s->hw;
    hw_extra_timing = s->timing;
    hw_zapper = s->zapper;
    hw_region_timing = s->region;
    memcpy(&hw_cart, s->cart, sizeof(HwCart));
    if (s->chr_len) memcpy(hw_cart.chr + hw_cart.chr_ram_base, s->chr, s->chr_len);
    ppu = s->ppu;
    memcpy(apu_state_ptr(&n), s->apu, s->apu_len);
    fds_media_restore(s->media);
    memcpy(fds_hle_state_ptr(&n), s->hle, s->hle_len);
    if (s->sound_len) hw_cart_sound_restore(s->sound);
    if (s->views_len) cyc_ramview_validity_set(s->views);
    cyc_ramview_stats = s->view_stats;
    cyc_ram_code_dirty = s->code_dirty;
    hw_frame_done = s->frame_done;
    hw_observe_hit = s->observe_hit;
    hw_frame_end_hit = s->frame_end_hit;
    hw_dma_stalls = s->dma_stalls;
    ppu_state_reloaded();
}
