/*
 * savestate.c — Save and restore full emulator state at NMI boundaries.
 *
 * State saved:
 *   CPU registers, work RAM, CHR RAM, PPU OAM, palette, nametable RAM,
 *   PPU registers, mapper (MMC1) registers, frame counter, plus a trailing
 *   id-keyed mod section (see mod_savestate.h) for architectural
 *   state mods keep outside guest RAM.
 *
 * V7: "NSSR", u8 version, u32 base size, base state, u16 mod count,
 * then {64-byte id, u32 payload size, payload} records. Older versions are
 * deliberately rejected. Savestates are build-specific, unlike game saves.
 */
#include "savestate.h"
#include "nes_runtime.h"
#include "mapper.h"
#include "apu.h"
#include "save_ram.h"
#include "mod_savestate.h"
#include "mod_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define SS_MAGIC   "NSSR"
#define SS_VERSION 7
#define SS_APU_BLOB_CAP 256
#define SS_RUNTIME_BLOB_CAP 256
/* On-disk id field width for a mod savestate record; must match
 * NES_MOD_SAVESTATE_ID_CAP in mod_savestate.c. */
#define SS_MOD_ID_CAP 64

/* PPU internals exposed from runtime.c */
extern uint16_t g_ppuaddr;
extern int      g_ppuaddr_latch_ss;  /* see below */
extern int      g_scroll_latch_ss;

/* We need the latch state — add accessors in runtime.c via a struct */
typedef struct {
    /* CPU */
    uint8_t A, X, Y, S, P;
    uint8_t N, V, D, I, Z, C;
    /* RAM */
    uint8_t ram[0x0800];
    uint8_t sram[0x2000];
    /* CHR */
    uint8_t chr_ram[0x2000];
    /* PPU */
    uint8_t ppu_oam[0x100];
    uint8_t ppu_pal[0x20];
    uint8_t ppu_nt[0x1000];
    uint8_t ppuctrl, ppumask, ppustatus, oamaddr;
    uint8_t ppuscroll_x, ppuscroll_y;
    uint16_t ppuaddr;
    uint8_t ppuaddr_latch;
    uint8_t scroll_latch;
    /* Mapper */
    MapperState mapper;
    /* Private runtime/APU architectural state. */
    uint16_t runtime_blob_size;
    uint8_t runtime_blob[SS_RUNTIME_BLOB_CAP];
    uint16_t apu_blob_size;
    uint8_t apu_blob[SS_APU_BLOB_CAP];
    /* Controller ports and render-visible sidecars. */
    uint8_t controller1_buttons, controller2_buttons;
    uint8_t ppuscroll_x_hud, ppuscroll_y_hud, ppuctrl_hud;
    int32_t spr0_split_active;
    int32_t spr0_reads_ctr;
    int16_t oam_x16[64];
    int16_t ws_shadow_x16[64];
    int16_t ws_obj_true_rel;
    uint8_t ws_obj_rel8, ws_obj_ctx_valid;
    int32_t zapper_x, zapper_y, zapper_trigger;
    /* Misc */
    uint64_t frame_count;
    /* Exact interrupted guest continuation. */
    uint16_t resume_pc;
    uint8_t resume_pc_valid;
    uint8_t resume_tick_charged;
} SaveStateData;

/* Size drift guard: the V7 image IS this struct. A field added, removed or
 * resized changes every file and every rollback digest partition boundary;
 * that must be a deliberate format bump (SS_VERSION), not an accident. */
_Static_assert(sizeof(SaveStateData) == 23720,
               "SaveStateData changed size: bump SS_VERSION and update this guard");

/* runtime.c must expose latch state — we access via a get/set pair declared below */
void runtime_get_latch_state(uint8_t *ppuaddr_latch, uint8_t *scroll_latch);
void runtime_set_latch_state(uint8_t ppuaddr_latch, uint8_t scroll_latch);

/* ------------------------------------------------------------------------
 * In-memory serializer. The file format is exactly this byte stream (magic,
 * version, base size, SaveStateData, mod section), so the file path and the
 * rollback ring cannot drift apart: both call savestate_serialize /
 * savestate_deserialize, and the file functions only add I/O around them.
 * ------------------------------------------------------------------------ */

enum { SS_MOD_MAX_RECORDS = 32 };
#define SS_HEADER_BYTES (4u + 1u + 4u)

/* One heap buffer for mod get() calls: the hook contract promises a
 * NES_MOD_SAVESTATE_BLOB_CAP-byte buffer, and allocating it per save costs a
 * 1 MiB malloc on every rollback snapshot. Single-threaded like the runner. */
static uint8_t *s_mod_scratch;

static uint8_t *mod_scratch(void) {
    if (!s_mod_scratch)
        s_mod_scratch = (uint8_t *)malloc(NES_MOD_SAVESTATE_BLOB_CAP);
    return s_mod_scratch;
}

static void fill_state(SaveStateData *ss) {
    memset(ss, 0, sizeof(*ss));

    /* CPU */
    ss->A = g_cpu.A; ss->X = g_cpu.X; ss->Y = g_cpu.Y;
    ss->S = g_cpu.S; ss->P = g_cpu.P;
    ss->N = g_cpu.N; ss->V = g_cpu.V; ss->D = g_cpu.D;
    ss->I = g_cpu.I; ss->Z = g_cpu.Z; ss->C = g_cpu.C;

    /* RAM */
    memcpy(ss->ram, g_ram, sizeof(ss->ram));
    memcpy(ss->sram, g_sram, sizeof(ss->sram));
    memcpy(ss->chr_ram, g_chr_ram, sizeof(ss->chr_ram));
    memcpy(ss->ppu_oam, g_ppu_oam, sizeof(ss->ppu_oam));
    memcpy(ss->ppu_pal, g_ppu_pal, sizeof(ss->ppu_pal));
    memcpy(ss->ppu_nt,  g_ppu_nt,  sizeof(ss->ppu_nt));

    /* PPU registers */
    ss->ppuctrl     = g_ppuctrl;
    ss->ppumask     = g_ppumask;
    ss->ppustatus   = g_ppustatus;
    ss->oamaddr     = g_oamaddr;
    ss->ppuscroll_x = g_ppuscroll_x;
    ss->ppuscroll_y = g_ppuscroll_y;
    ss->ppuaddr     = g_ppuaddr;
    runtime_get_latch_state(&ss->ppuaddr_latch, &ss->scroll_latch);

    /* Mapper */
    mapper_get_state(&ss->mapper);

    ss->controller1_buttons = g_controller1_buttons;
    ss->controller2_buttons = g_controller2_buttons;
    ss->ppuscroll_x_hud = g_ppuscroll_x_hud;
    ss->ppuscroll_y_hud = g_ppuscroll_y_hud;
    ss->ppuctrl_hud = g_ppuctrl_hud;
    ss->spr0_split_active = g_spr0_split_active;
    ss->spr0_reads_ctr = g_spr0_reads_ctr_legacy;
    memcpy(ss->oam_x16, g_oam_x16, sizeof(ss->oam_x16));
    memcpy(ss->ws_shadow_x16, g_ws_shadow_x16, sizeof(ss->ws_shadow_x16));
    ss->ws_obj_true_rel = g_ws_obj_true_rel;
    ss->ws_obj_rel8 = g_ws_obj_rel8;
    ss->ws_obj_ctx_valid = g_ws_obj_ctx_valid;
    ss->zapper_x = g_zapper_x;
    ss->zapper_y = g_zapper_y;
    ss->zapper_trigger = g_zapper_trigger;

    /* Frame count */
    ss->frame_count = g_frame_count;
    {
        int tick_charged = 0;
        ss->resume_pc_valid =
            (uint8_t)runtime_get_savestate_resume(&ss->resume_pc, &tick_charged);
        ss->resume_tick_charged = (uint8_t)(tick_charged != 0);
    }
}

size_t savestate_base_size(void) { return sizeof(SaveStateData); }

/* Partition boundaries inside the serialized image, for the rollback digest
 * (nes_rb_state.c): [0, cpu_end) = header + CPU + work RAM + SRAM;
 * [cpu_end, ppu_end) = CHR / OAM / palette / nametables / PPU registers /
 * mapper / runtime timing blob; [ppu_end, image end) = APU blob, controller
 * ports, render/zapper sidecars, frame count, resume point, mod records and
 * anything a caller appends. Every byte of the image lands in exactly one. */
void savestate_partitions(size_t *cpu_end, size_t *ppu_end) {
    if (cpu_end) *cpu_end = SS_HEADER_BYTES + offsetof(SaveStateData, chr_ram);
    if (ppu_end) *ppu_end = SS_HEADER_BYTES + offsetof(SaveStateData, apu_blob_size);
}

/* Name the field a byte offset of the image belongs to ("ram+0x01a3"), so a
 * determinism probe reports WHAT diverged instead of a bare offset. */
typedef struct { const char *name; size_t off, size; } SsField;
#define SSF(f) { #f, offsetof(SaveStateData, f), sizeof(((SaveStateData *)0)->f) }
static const SsField s_ss_fields[] = {
    SSF(A), SSF(X), SSF(Y), SSF(S), SSF(P), SSF(N), SSF(V), SSF(D), SSF(I),
    SSF(Z), SSF(C), SSF(ram), SSF(sram), SSF(chr_ram), SSF(ppu_oam),
    SSF(ppu_pal), SSF(ppu_nt), SSF(ppuctrl), SSF(ppumask), SSF(ppustatus),
    SSF(oamaddr), SSF(ppuscroll_x), SSF(ppuscroll_y), SSF(ppuaddr),
    SSF(ppuaddr_latch), SSF(scroll_latch), SSF(mapper),
    SSF(runtime_blob_size), SSF(runtime_blob), SSF(apu_blob_size),
    SSF(apu_blob), SSF(controller1_buttons), SSF(controller2_buttons),
    SSF(ppuscroll_x_hud), SSF(ppuscroll_y_hud), SSF(ppuctrl_hud),
    SSF(spr0_split_active), SSF(spr0_reads_ctr), SSF(oam_x16),
    SSF(ws_shadow_x16), SSF(ws_obj_true_rel), SSF(ws_obj_rel8),
    SSF(ws_obj_ctx_valid), SSF(zapper_x), SSF(zapper_y), SSF(zapper_trigger),
    SSF(frame_count), SSF(resume_pc), SSF(resume_pc_valid),
    SSF(resume_tick_charged),
};
#undef SSF

void savestate_describe_offset(const uint8_t *image, size_t len, size_t off,
                               char *out, size_t cap) {
    if (!out || !cap) return;
    if (off < SS_HEADER_BYTES) { snprintf(out, cap, "header+%zu", off); return; }
    if (off < SS_HEADER_BYTES + sizeof(SaveStateData)) {
        size_t o = off - SS_HEADER_BYTES;
        for (size_t i = 0; i < sizeof s_ss_fields / sizeof s_ss_fields[0]; ++i) {
            if (o >= s_ss_fields[i].off && o < s_ss_fields[i].off + s_ss_fields[i].size) {
                snprintf(out, cap, "%s+0x%04zx", s_ss_fields[i].name,
                         o - s_ss_fields[i].off);
                return;
            }
        }
        snprintf(out, cap, "base-padding+%zu", o);
        return;
    }
    /* Walk the mod section to name the record. */
    {
        size_t p = SS_HEADER_BYTES + sizeof(SaveStateData);
        uint16_t count = 0;
        if (!image || len < p + 2) { snprintf(out, cap, "tail+%zu", off - p); return; }
        memcpy(&count, image + p, 2);
        if (off < p + 2) { snprintf(out, cap, "mod_count"); return; }
        p += 2;
        for (uint16_t i = 0; i < count && p + SS_MOD_ID_CAP + 4 <= len; ++i) {
            uint32_t rl = 0;
            char id[SS_MOD_ID_CAP];
            memcpy(id, image + p, SS_MOD_ID_CAP);
            id[SS_MOD_ID_CAP - 1] = 0;
            memcpy(&rl, image + p + SS_MOD_ID_CAP, 4);
            size_t body = p + SS_MOD_ID_CAP + 4;
            if (off < body + rl) {
                if (off < body) snprintf(out, cap, "mod[%s].header", id);
                else snprintf(out, cap, "mod[%s]+0x%04zx", id, off - body);
                return;
            }
            p = body + rl;
        }
        snprintf(out, cap, "trailer+%zu", off - p);
    }
}

size_t savestate_serialize(uint8_t *buf, size_t cap) {
    return savestate_serialize_ex(buf, cap, NULL);
}

size_t savestate_serialize_ex(uint8_t *buf, size_t cap, size_t *need) {
    SaveStateData ss;
    size_t off = 0;
    uint32_t base_size = (uint32_t)sizeof(SaveStateData);
    uint8_t ver = SS_VERSION;

    if (need) *need = 0;
    if (!buf || cap < SS_HEADER_BYTES + sizeof(ss) + 2u) {
        if (need) *need = SS_HEADER_BYTES + sizeof(ss) + 2u;
        return 0;
    }
    fill_state(&ss);
    {
        int n = runtime_get_state_blob(ss.runtime_blob, sizeof(ss.runtime_blob));
        if (n <= 0) return 0;
        ss.runtime_blob_size = (uint16_t)n;
        n = apu_get_state_blob(ss.apu_blob, sizeof(ss.apu_blob));
        if (n <= 0) return 0;
        ss.apu_blob_size = (uint16_t)n;
    }
    memcpy(buf + off, SS_MAGIC, 4); off += 4;
    buf[off++] = ver;
    memcpy(buf + off, &base_size, sizeof base_size); off += sizeof base_size;
    memcpy(buf + off, &ss, sizeof ss); off += sizeof ss;

    /* Mod section: id-keyed records from mod_savestate.c. Missing
     * architectural state would make a successful-looking save unusable, so
     * serialization failures fail it. */
    {
        uint16_t mod_count = 0;
        size_t count_pos = off;
        uint8_t *blob = mod_scratch();
        if (!blob) return 0;
        off += sizeof mod_count;
        int hook_total = nes_mod_savestate_hook_count();
        for (int i = 0; i < hook_total && mod_count < SS_MOD_MAX_RECORDS; i++) {
            const char *id = nes_mod_savestate_hook_id_at(i);
            NESModSavestateGet get = nes_mod_savestate_hook_get_at(i);
            if (!id || !get) continue;
            int len = get(blob, NES_MOD_SAVESTATE_BLOB_CAP);
            if (len < 0 || len > NES_MOD_SAVESTATE_BLOB_CAP) {
                fprintf(stderr,
                        "[SaveState] Mod hook '%s' state does not fit; save failed\n",
                        id);
                return 0;
            }
            if (off > cap || cap - off < (size_t)SS_MOD_ID_CAP + 4u + (size_t)len) {
                /* Out of room: keep counting so the caller learns the exact
                 * size to allocate, then report failure. */
                off += (size_t)SS_MOD_ID_CAP + 4u + (size_t)len;
                mod_count++;
                continue;
            }
            char id_field[SS_MOD_ID_CAP] = {0};
            uint32_t blob_len = (uint32_t)len;
            strncpy(id_field, id, SS_MOD_ID_CAP - 1);
            memcpy(buf + off, id_field, SS_MOD_ID_CAP); off += SS_MOD_ID_CAP;
            memcpy(buf + off, &blob_len, sizeof blob_len); off += sizeof blob_len;
            if (len) memcpy(buf + off, blob, (size_t)len);
            off += (size_t)len;
            mod_count++;
        }
        if (off > cap) {
            if (need) *need = off;
            return 0;
        }
        memcpy(buf + count_pos, &mod_count, sizeof mod_count);
    }
    if (need) *need = off;
    return off;
}

int savestate_deserialize(const uint8_t *buf, size_t len, int flags) {
    SaveStateData ss;
    size_t off = 0;
    uint32_t state_size = 0;
    const char *what = (flags & SAVESTATE_LOAD_QUIET) ? "rollback snapshot" : "state";

    if (!buf || len < SS_HEADER_BYTES || memcmp(buf, SS_MAGIC, 4) != 0) {
        fprintf(stderr, "[SaveState] Bad magic in %s\n", what);
        return 0;
    }
    off = 4;
    if (buf[off++] != SS_VERSION) {
        fprintf(stderr, "[SaveState] Version mismatch in %s\n", what);
        return 0;
    }
    memcpy(&state_size, buf + off, sizeof state_size); off += sizeof state_size;
    if (state_size != sizeof ss || len - off < sizeof ss) {
        fprintf(stderr, "[SaveState] Truncated data in %s\n", what);
        return 0;
    }
    memcpy(&ss, buf + off, sizeof ss); off += sizeof ss;

    /* Records are parsed here but restored later, after NES RAM/CPU/PPU state
     * is applied below, per the get/set contract in mod_savestate.h. They are
     * applied straight from `buf` (which outlives this call). */
    const char    *mod_ids[SS_MOD_MAX_RECORDS];
    const uint8_t *mod_blobs[SS_MOD_MAX_RECORDS];
    int            mod_lens[SS_MOD_MAX_RECORDS];
    int            mod_n = 0;
    {
        uint16_t mod_count = 0;
        if (len - off < sizeof mod_count) {
            fprintf(stderr, "[SaveState] Truncated mod section in %s\n", what);
            return 0;
        }
        memcpy(&mod_count, buf + off, sizeof mod_count); off += sizeof mod_count;
        if (mod_count > SS_MOD_MAX_RECORDS) return 0;
        for (uint16_t i = 0; i < mod_count; i++) {
            uint32_t rec_len = 0;
            if (len - off < (size_t)SS_MOD_ID_CAP + 4u) {
                fprintf(stderr, "[SaveState] Truncated mod record in %s\n", what);
                return 0;
            }
            const char *id = (const char *)(buf + off);
            off += SS_MOD_ID_CAP;
            memcpy(&rec_len, buf + off, sizeof rec_len); off += sizeof rec_len;
            if (rec_len > NES_MOD_SAVESTATE_BLOB_CAP || len - off < rec_len) {
                fprintf(stderr, "[SaveState] Truncated mod record in %s\n", what);
                return 0;
            }
            if (!id[0] || !memchr(id, '\0', SS_MOD_ID_CAP)) return 0;
            for (int j = 0; j < mod_n; j++)
                if (!strcmp(id, mod_ids[j])) return 0;
            mod_ids[mod_n] = id;
            mod_blobs[mod_n] = buf + off;
            mod_lens[mod_n] = (int)rec_len;
            mod_n++;
            off += rec_len;
        }
    }

    for (int i=0;i<nes_mod_savestate_hook_count();++i) {
        NESModSavestateValidate validate=nes_mod_savestate_hook_validate_at(i);
        if (!validate) continue;
        const char *id=nes_mod_savestate_hook_id_at(i);
        int record=-1;
        for (int j=0;j<mod_n;++j) if (!strcmp(id,mod_ids[j])) { record=j; break; }
        if (!validate(record<0?NULL:mod_blobs[record],record<0?0:mod_lens[record])) {
            fprintf(stderr,"[SaveState] Incompatible or invalid mod state '%s'; load rejected before changing the game\n",id);
            return 0;
        }
    }

    if (ss.runtime_blob_size == 0 || ss.runtime_blob_size > sizeof(ss.runtime_blob) ||
        ss.apu_blob_size == 0 || ss.apu_blob_size > sizeof(ss.apu_blob)) {
        fprintf(stderr, "[SaveState] Invalid subsystem state in %s\n", what);
        return 0;
    }

    /* CPU */
    g_cpu.A = ss.A; g_cpu.X = ss.X; g_cpu.Y = ss.Y;
    g_cpu.S = ss.S; g_cpu.P = ss.P;
    g_cpu.N = ss.N; g_cpu.V = ss.V; g_cpu.D = ss.D;
    g_cpu.I = ss.I; g_cpu.Z = ss.Z; g_cpu.C = ss.C;

    /* RAM */
    memcpy(g_ram,    ss.ram,     sizeof(ss.ram));
    memcpy(g_sram,   ss.sram,    sizeof(ss.sram));
    memcpy(g_chr_ram, ss.chr_ram, sizeof(ss.chr_ram));
    memcpy(g_ppu_oam, ss.ppu_oam, sizeof(ss.ppu_oam));
    memcpy(g_ppu_pal, ss.ppu_pal, sizeof(ss.ppu_pal));
    memcpy(g_ppu_nt,  ss.ppu_nt,  sizeof(ss.ppu_nt));
    save_ram_sync_snapshot();

    /* PPU registers */
    g_ppuctrl     = ss.ppuctrl;
    g_ppumask     = ss.ppumask;
    g_ppustatus   = ss.ppustatus;
    g_oamaddr     = ss.oamaddr;
    g_ppuscroll_x = ss.ppuscroll_x;
    g_ppuscroll_y = ss.ppuscroll_y;
    g_ppuaddr     = ss.ppuaddr;
    runtime_set_latch_state(ss.ppuaddr_latch, ss.scroll_latch);

    /* Mapper */
    mapper_set_state(&ss.mapper);

    if (!runtime_set_state_blob(ss.runtime_blob, ss.runtime_blob_size) ||
        !apu_set_state_blob(ss.apu_blob, ss.apu_blob_size)) {
        fprintf(stderr, "[SaveState] Could not restore subsystem state from %s\n", what);
        return 0;
    }

    g_controller1_buttons = ss.controller1_buttons;
    g_controller2_buttons = ss.controller2_buttons;
    g_ppuscroll_x_hud = ss.ppuscroll_x_hud;
    g_ppuscroll_y_hud = ss.ppuscroll_y_hud;
    g_ppuctrl_hud = ss.ppuctrl_hud;
    g_spr0_split_active = ss.spr0_split_active;
    g_spr0_reads_ctr_legacy = ss.spr0_reads_ctr;
    memcpy(g_oam_x16, ss.oam_x16, sizeof(ss.oam_x16));
    memcpy(g_ws_shadow_x16, ss.ws_shadow_x16, sizeof(ss.ws_shadow_x16));
    g_ws_obj_true_rel = ss.ws_obj_true_rel;
    g_ws_obj_rel8 = ss.ws_obj_rel8;
    g_ws_obj_ctx_valid = ss.ws_obj_ctx_valid;
    g_zapper_x = ss.zapper_x;
    g_zapper_y = ss.zapper_y;
    g_zapper_trigger = ss.zapper_trigger;

    /* Frame count */
    g_frame_count = ss.frame_count;

    /* PCM overlay cursors are host delivery state, like the audio bridge, not
     * emulated state. Keeping them would let a pre-load voice continue over a
     * rewound world and duplicate any cue the restored fighter reaches. */
    nes_mod_audio_stop_all();

    if (ss.resume_pc_valid) {
        runtime_request_guest_resume(ss.resume_pc, ss.resume_tick_charged != 0);
        /* A save taken later in the SAME callback (the rollback driver
         * re-keys the load tick right after loading it) must record the
         * restored continuation, not the one the discarded timeline was
         * interrupted at. */
        runtime_rebase_frame_resume(ss.resume_pc, ss.resume_tick_charged != 0);
    }

    /* Mod section restore. Deliberately last: NES RAM/CPU/PPU state above is
     * fully applied first, so a hook's set() sees the same post-load world a
     * game_post_nmi() callback would. Unknown ids are a stderr warning, not a
     * load failure — a save made with a mod that is no longer registered
     * (disabled, uninstalled) must still load the rest of the state. */
    for (int i = 0; i < mod_n; i++) {
        NESModSavestateSet set = nes_mod_savestate_hook_find_set(mod_ids[i]);
        if (!set) {
            fprintf(stderr,
                    "[SaveState] No mod hook registered for '%s'; state skipped\n",
                    mod_ids[i]);
            continue;
        }
        if (!set(mod_blobs[i], mod_lens[i])) {
            fprintf(stderr,
                    "[SaveState] Mod hook '%s' rejected its saved state\n",
                    mod_ids[i]);
        }
    }
    return 1;
}

/* Growable scratch for the file path. The rollback path owns its own. */
static uint8_t *s_file_buf;
static size_t   s_file_cap;

int savestate_save(const char *path) {
    size_t n = 0;
    if (!s_file_buf) {
        s_file_cap = SS_HEADER_BYTES + sizeof(SaveStateData) + 64u * 1024u;
        s_file_buf = (uint8_t *)malloc(s_file_cap);
        if (!s_file_buf) { s_file_cap = 0; return 0; }
    }
    {
        size_t need = 0;
        n = savestate_serialize_ex(s_file_buf, s_file_cap, &need);
        if (!n && need > s_file_cap) {
            uint8_t *nb = (uint8_t *)realloc(s_file_buf, need);
            if (!nb) return 0;
            s_file_buf = nb;
            s_file_cap = need;
            n = savestate_serialize_ex(s_file_buf, s_file_cap, &need);
        }
        if (!n) return 0;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[SaveState] Cannot open for write: %s\n", path); return 0; }
    if (fwrite(s_file_buf, 1, n, f) != n) { fclose(f); return 0; }
    if (fclose(f) != 0) return 0;

    printf("[SaveState] Saved to %s (frame %llu)\n", path,
           (unsigned long long)g_frame_count);
    return 1;
}

int savestate_load(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;
    int ok;
    if (!f) { fprintf(stderr, "[SaveState] Cannot open: %s\n", path); return 0; }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (uint8_t *)malloc(size ? (size_t)size : 1u);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "[SaveState] Truncated data in %s\n", path);
        free(buf); fclose(f); return 0;
    }
    fclose(f);
    ok = savestate_deserialize(buf, (size_t)size, 0);
    free(buf);
    if (!ok) {
        fprintf(stderr, "[SaveState] Load of %s rejected\n", path);
        return 0;
    }
    printf("[SaveState] Loaded from %s (frame %llu)\n", path,
           (unsigned long long)g_frame_count);
    return 1;
}
