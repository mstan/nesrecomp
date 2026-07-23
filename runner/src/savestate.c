/*
 * savestate.c — Save and restore full emulator state at NMI boundaries.
 *
 * State saved:
 *   CPU registers, work RAM, CHR RAM, PPU OAM, palette, nametable RAM,
 *   PPU registers, mapper (MMC1) registers, frame counter.
 *
 * Format: raw binary with 4-byte magic "NSSR" + 1-byte version.
 */
#include "savestate.h"
#include "nes_runtime.h"
#include "mapper.h"
#include "apu.h"
#include <stdio.h>
#include <string.h>

#define SS_MAGIC   "NSSR"
#define SS_VERSION 4
#define SS_APU_BLOB_CAP 256
#define SS_RUNTIME_BLOB_CAP 256

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
} SaveStateData;

/* runtime.c must expose latch state — we access via a get/set pair declared below */
void runtime_get_latch_state(uint8_t *ppuaddr_latch, uint8_t *scroll_latch);
void runtime_set_latch_state(uint8_t ppuaddr_latch, uint8_t scroll_latch);

int savestate_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[SaveState] Cannot open for write: %s\n", path); return 0; }

    /* Write magic + version */
    fwrite(SS_MAGIC, 1, 4, f);
    uint8_t ver = SS_VERSION;
    fwrite(&ver, 1, 1, f);

    SaveStateData ss;
    memset(&ss, 0, sizeof(ss));

    /* CPU */
    ss.A = g_cpu.A; ss.X = g_cpu.X; ss.Y = g_cpu.Y;
    ss.S = g_cpu.S; ss.P = g_cpu.P;
    ss.N = g_cpu.N; ss.V = g_cpu.V; ss.D = g_cpu.D;
    ss.I = g_cpu.I; ss.Z = g_cpu.Z; ss.C = g_cpu.C;

    /* RAM */
    memcpy(ss.ram, g_ram, sizeof(ss.ram));
    memcpy(ss.sram, g_sram, sizeof(ss.sram));
    memcpy(ss.chr_ram, g_chr_ram, sizeof(ss.chr_ram));
    memcpy(ss.ppu_oam, g_ppu_oam, sizeof(ss.ppu_oam));
    memcpy(ss.ppu_pal, g_ppu_pal, sizeof(ss.ppu_pal));
    memcpy(ss.ppu_nt,  g_ppu_nt,  sizeof(ss.ppu_nt));

    /* PPU registers */
    ss.ppuctrl     = g_ppuctrl;
    ss.ppumask     = g_ppumask;
    ss.ppustatus   = g_ppustatus;
    ss.oamaddr     = g_oamaddr;
    ss.ppuscroll_x = g_ppuscroll_x;
    ss.ppuscroll_y = g_ppuscroll_y;
    ss.ppuaddr     = g_ppuaddr;
    runtime_get_latch_state(&ss.ppuaddr_latch, &ss.scroll_latch);

    /* Mapper */
    mapper_get_state(&ss.mapper);

    {
        int n = runtime_get_state_blob(ss.runtime_blob, sizeof(ss.runtime_blob));
        if (n <= 0) { fclose(f); return 0; }
        ss.runtime_blob_size = (uint16_t)n;
        n = apu_get_state_blob(ss.apu_blob, sizeof(ss.apu_blob));
        if (n <= 0) { fclose(f); return 0; }
        ss.apu_blob_size = (uint16_t)n;
    }

    ss.controller1_buttons = g_controller1_buttons;
    ss.controller2_buttons = g_controller2_buttons;
    ss.ppuscroll_x_hud = g_ppuscroll_x_hud;
    ss.ppuscroll_y_hud = g_ppuscroll_y_hud;
    ss.ppuctrl_hud = g_ppuctrl_hud;
    ss.spr0_split_active = g_spr0_split_active;
    ss.spr0_reads_ctr = g_spr0_reads_ctr_legacy;
    memcpy(ss.oam_x16, g_oam_x16, sizeof(ss.oam_x16));
    memcpy(ss.ws_shadow_x16, g_ws_shadow_x16, sizeof(ss.ws_shadow_x16));
    ss.ws_obj_true_rel = g_ws_obj_true_rel;
    ss.ws_obj_rel8 = g_ws_obj_rel8;
    ss.ws_obj_ctx_valid = g_ws_obj_ctx_valid;
    ss.zapper_x = g_zapper_x;
    ss.zapper_y = g_zapper_y;
    ss.zapper_trigger = g_zapper_trigger;

    /* Frame count */
    ss.frame_count = g_frame_count;

    fwrite(&ss, 1, sizeof(ss), f);
    fclose(f);

    printf("[SaveState] Saved to %s (frame %llu)\n", path,
           (unsigned long long)g_frame_count);
    return 1;
}

int savestate_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[SaveState] Cannot open: %s\n", path); return 0; }

    /* Check magic + version */
    char magic[4];
    uint8_t ver;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, SS_MAGIC, 4) != 0) {
        fprintf(stderr, "[SaveState] Bad magic in %s\n", path);
        fclose(f); return 0;
    }
    if (fread(&ver, 1, 1, f) != 1 || ver != SS_VERSION) {
        fprintf(stderr, "[SaveState] Version mismatch in %s\n", path);
        fclose(f); return 0;
    }

    SaveStateData ss;
    if (fread(&ss, 1, sizeof(ss), f) != sizeof(ss)) {
        fprintf(stderr, "[SaveState] Truncated data in %s\n", path);
        fclose(f); return 0;
    }
    fclose(f);

    if (ss.runtime_blob_size == 0 || ss.runtime_blob_size > sizeof(ss.runtime_blob) ||
        ss.apu_blob_size == 0 || ss.apu_blob_size > sizeof(ss.apu_blob)) {
        fprintf(stderr, "[SaveState] Invalid subsystem state in %s\n", path);
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
        fprintf(stderr, "[SaveState] Could not restore subsystem state from %s\n", path);
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

    printf("[SaveState] Loaded from %s (frame %llu)\n", path,
           (unsigned long long)g_frame_count);
    return 1;
}
