/*
 * cyc_host.c - host for NESRecomp --cycle-accurate builds, and for the
 * TriCNES oracle (built with CYC_ORACLE, see runner/cyc/oracle).
 *
 *   <exe> <rom.nes> [options]
 *     --interp-only        never run recompiled code (not in the oracle)
 *     --align N            CPU/PPU clock alignment 0-3 (default 0)
 *     --ram-init MODE      CPU RAM at power-on: pattern (default, the reference
 *                          console's), zeros or ones (what other emulators do)
 *     --scale N            window scale (default 3)
 *
 *   Headless (implied by any of the options below, or --headless):
 *     --frames N           frames to run (default 600; with --acccoin, a limit)
 *     --acccoin            drive AccuracyCoin's "run all tests" and report
 *     --spam PAGE ROW      play one AccuracyCoin test over and over the way a
 *                          player does: walk to PAGE (0 based) and ROW, then
 *                          mash A while tapping Down and Up between ROW and the
 *                          row below it. Reports every result the ROM writes.
 *     --spam-seed N        seed for which frames those taps land on
 *     --spam-no-dpad       mash A only, leaving the cursor where it is
 *     --input FILE         hold buttons on a schedule, so a headless run can be
 *                          driven into a game rather than sitting on its title
 *                          screen. One `<frame> [buttons]` line per change,
 *                          buttons being A, B, SELECT, START, UP, DOWN, LEFT and
 *                          RIGHT joined by + (none, or -, releases everything);
 *                          `2:` before the list sets controller 2. Each line
 *                          holds until the next one's frame. Both a recompiled
 *                          build and the oracle read the same file, so a run
 *                          driven this way stays comparable.
 *     --hash-out FILE      write, per frame, hashes of the observable trace
 *                          (cyc_trace.h) and of memory and the picture, the CPU
 *                          registers, and last a hash of the hardware internals;
 *                          recompiled, --interp-only and oracle runs of the same
 *                          ROM must match line for line
 *     --trace-frame N      with --trace-out, print every trace record of frame N
 *     --trace-out FILE
 *     --state-frame N      with --state-out, write every hashed hardware field
 *     --state-out FILE     at the end of frame N (cyc_hw_state_dump)
 *     --mem-frame N        with --mem-out, write the memories and picture the
 *     --mem-out FILE       mem= hash covers at the end of frame N
 *     --miss-log FILE      ROM addresses that began an instruction on the
 *                          interpreter, with counts, merged into FILE (use it
 *                          as game.toml [game] cycle_seed_file)
 *     --capture-log FILE   RAM code that ran on the interpreter with no
 *                          compiled view (instructions, chunk snapshots, bytes
 *                          stores changed under a view), merged into FILE (use
 *                          it as game.toml [game] cycle_capture_file)
 *     --ram-view-list FILE the program's compiled RAM views and their state at exit
 *     --screenshot FILE    save the last frame as PNG
 *     --shot-every N       also save every Nth frame, as FILE with the frame
 *                          number before its extension (shot.png -> shot_00120.png)
 *     --wav-out FILE       record the audio output (48 kHz mono)
 *     --console MODEL      the analog output stage the audio goes through
 *                          (cyc_core.h cyc_set_console): nes (front-loader:
 *                          90 Hz + 440 Hz high-pass, 14 kHz low-pass), famicom
 *                          (37 Hz high-pass) or default: famicom for boards
 *                          made only for the Famicom with expansion audio (FDS,
 *                          Namco 163, VRC6, VRC7), nes otherwise. A compiled
 *                          program's default is game.toml [game] console.
 *     --frame-log FILE     binary per-frame snapshot of CPU RAM, CIRAM, palette,
 *                          OAM, cartridge/PRG RAM, CHR RAM, the picture's
 *                          color indices and the FDS sound unit's state
 *                          (format: write_frame_log below)
 *     --frame-log-frames A:B  only frames A..B
 *     --frame-log-at WHEN  vblank (default: at the frame's end) or mesen (at
 *                          scanline 240 dot 0, where Mesen/nesref end a frame)
 *     --ring-out FILE      the always-on event ring (cyc_ring.h) at exit
 *     --ring-frames A:B    only events of frames A..B
 *
 *   Famicom Disk System (<exe> <image.fds|.qd>, or no image for a program
 *   compiled with game.toml [fds] image):
 *     --fds-bios FILE      the RAM Adapter BIOS; default game.toml [fds] bios,
 *                          else bios/disksys.rom beside the image or here.
 *                          Only the image bios/disksys.toml (or the compiled
 *                          program) identifies is accepted: 8192 bytes, CRC32.
 *     --fds-boot-disk S    side in the drive at power-on: none, or a side (0 =
 *                          disk 1 side A, 1 = 1B, ...; A, B, 1A, 2B also work).
 *                          Default 0; nesref's default is none.
 *     --fds-event F:ACTION eject, insert[=SIDE] or select=SIDE before frame F
 *                          (nesref DISK_EJECT / DISK_INSERT / DISK_SELECT at
 *                          f=F). --input files take `F DISK_EJECT`,
 *                          `F DISK_INSERT [SIDE]` and `F DISK_SELECT SIDE`.
 *     --fds-profile P      mesen (default, nesref's core), mesen2, hardware
 *     --fds-crc MODE       computed (real CRC-16 in the side streams, default)
 *                          or mesen (Mesen's constant $4D $62)
 *     --fds-crc-check      report CRC mismatches in $4030.4 in any profile
 *     --fds-write-protect  the disk's write-protect tab is broken off
 *     --fds-write-at W     where a written byte lands: head (default) or mesen
 *                          (two bytes behind, as nesref's core; hw_fds.c)
 *     --save-file FILE     the disk save: what the game writes to its disk,
 *                          loaded at start and saved when the drive stops, on
 *                          eject and at exit (cyc_fds_save.inc). The image is
 *                          never written. A windowed run without it saves to
 *                          <exe dir>/saves/<image stem>.fdssave; --no-save
 *                          turns that off.
 *     --fds-import-ips FILE  start from a Mesen/nesref disk save (<stem>.ips)
 *     --fds-export-ips FILE  also write the disk as a Mesen .ips at each save
 *     --fds-hle LIST       the HLE tier (common/nes_fds_hle.h): boot-skip,
 *                          auto-swap, fast-load, all (the three), off, and
 *                          no-... of each; auto-insert / no-auto-insert (LLE,
 *                          default on: insert side A when the boot waits for
 *                          a disk with the drive empty). Overrides
 *                          NESRECOMP_FDS_HLE, which overrides game.toml [fds]
 *                          hle; HLE axes default off. An axis the BIOS or image
 *                          cannot support is refused with the reason.
 *     --boot-state-out FILE  the whole machine at the game's first instruction
 *                          (after the BIOS's jump into it; with the boot skip,
 *                          where the skip's boot jumps): cpu, hardware, memories
 *     --state-at-pc PC FILE  the same after the first run of the instruction at
 *                          PC ($8000 up; for deriving what the BIOS leaves)
 *     --realtime           headless: pace frames at the console's 60.0988 Hz as
 *                          the window does (fast load then skips the pacing of
 *                          load frames), and report the wall-clock time of loads
 *
 * Without CYC_WITH_SDL the host is always headless.
 */
#include "cyc_accuracycoin.h"
#include "cyc_core.h"
#include "cyc_host.h"
#include "../../common/nes_cart.h"
#include "cyc_png.h"
#include "cyc_ring.h"
#include "cyc_trace.h"
#include "../../common/nes_fds.h"
#include "../../common/nes_fds_hle.h"
#include "../../common/nes_fds_boot.h"

#ifndef CYC_ORACLE
#include "cpu6502.h"
#include "cyc_ramview.h"
#include "cyc_recomp.h"
#include "cyc_run.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(CYC_WITH_SDL) && !defined(CYC_ORACLE)
int cyc_sdl_main(const char *title, int scale);
#endif
#ifdef CYC_ORACLE
void cyc_oracle_address_report(void *file);
#endif

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

#ifndef CYC_ORACLE
static void write_miss_log(const char *path) {
    /* Merge with an existing log so runs accumulate; the recompiler reads the
     * first column (game.toml [game] cycle_seed_file). */
    size_t slots = cyc_run_miss_slots();
    unsigned bank_count = (unsigned)(slots / 32768);
    unsigned new_addrs = 0;
    for (size_t i = 0; i < slots; i++) new_addrs += cyc_run_miss[i] != 0;
    FILE *mf = fopen(path, "r");
    if (mf) {
        char line[128];
        unsigned bank, addr, count;
        while (fgets(line, sizeof(line), mf)) {
            if (line[0] == '#') continue;
            /* `BB:AAAA [count]`, or `AAAA [count]` from a run of an NROM
             * program or a hand-written seed: no bank means the one this
             * cartridge has there now. */
            int got = sscanf(line, "4k:%x:%x %u", &bank, &addr, &count);
            if (got >= 2) {
                if (got < 3) count = 0;
            } else if ((got = sscanf(line, "%x:%x %u", &bank, &addr, &count)) >= 2) {
                if (bank >= (bank_count + 1)/2) continue;
                bank = (bank * 2 + ((addr >> 12) & 1)) & (bank_count - 1);
                if (got < 3) count = 0;
            } else {
                got = sscanf(line, "%x %u", &addr, &count);
                if (got < 1 || addr < 0x8000 || addr > 0xffff) continue;
                bank = hw_prg_bank4((uint16_t)addr);
                count = got == 2 ? count : 0;
            }
            if (addr < 0x8000 || addr > 0xffff || bank >= bank_count) continue;
            unsigned index = ((bank * 8 + ((addr >> 12) & 7)) << 12) | (addr & 0xfff);
            if (index >= slots) continue;
            uint32_t add = count ? count : 1;
            uint32_t *slot = &cyc_run_miss[index];
            *slot = *slot + add < add ? UINT32_MAX : *slot + add;
        }
        fclose(mf);
    }
    mf = fopen(path, "w");
    if (mf) {
        fprintf(mf, "# ROM instruction starts executed by the interpreter: PRG bank, address, count.\n"
                    "# Written by %s --miss-log (merged across runs).\n",
                cyc_native_program_name ? cyc_native_program_name : "cyc_interp");
        for (size_t i = 0; i < slots; i++) {
            if (!cyc_run_miss[i]) continue;
            unsigned bank;
            uint16_t addr;
            cyc_run_miss_decode((unsigned)i, &bank, &addr);
            fprintf(mf, "4k:%02X:%04X %u\n", bank, addr, cyc_run_miss[i]);
        }
        /* RAM instruction starts (CPU RAM, and cartridge RAM such as the FDS's
         * PRG RAM), as comments: the recompiler skips them, since code the
         * program writes at run time is not in the ROM image to compile. They
         * are here to explain a coverage gap seeds cannot close. */
        if (cyc_run_ram_miss) {
            unsigned ram_addrs = 0;
            for (int a = 0; a < 0x10000; a++) ram_addrs += cyc_run_ram_miss[a] != 0;
            if (ram_addrs) {
                unsigned stable = 0, varied = 0;
                fprintf(mf, "#\n# %u RAM addresses also started an instruction this run (not seedable).\n"
                            "# 'opcodes' is how many distinct opcode bytes ever began an instruction\n"
                            "# there; 1 means only operands change at that address.\n"
                            "# address count opcodes\n", ram_addrs);
                for (int a = 0; a < 0x10000; a++) {
                    if (!cyc_run_ram_miss[a]) continue;
                    unsigned ops = 0;
                    if (cyc_run_ram_opcodes) {
                        const uint8_t *set = cyc_run_ram_opcodes + (size_t)a * 32;
                        for (int b = 0; b < 32; b++)
                            for (int k = 0; k < 8; k++) ops += (set[b] >> k) & 1;
                    }
                    if (ops > 1) varied++; else stable++;
                    fprintf(mf, "# %04X %u %u\n", a, cyc_run_ram_miss[a], ops);
                }
                fprintf(mf, "# %u addresses with one opcode, %u with several\n", stable, varied);
            }
        }
        fclose(mf);
    }
    printf("miss-log: %u ROM addresses started an instruction on the interpreter this run -> %s\n", new_addrs,
           path);
}
#endif

/* ---- --input: a button schedule ---- */

/* A NES pad, MSB first: A B Select Start Up Down Left Right (cyc_core.h). */
typedef struct {
    long    frame;
    uint8_t port, buttons;
} InputStep;

static InputStep *input_steps;
static int        input_count, input_next;
static uint8_t    input_held[2];

/* ---- FDS disk events: nesref's DISK_EJECT / DISK_SELECT / DISK_INSERT ----
 * Applied between frames, before frame F runs (after F frames), which is
 * where nesref applies them ("f=F", after F retro_runs). */
typedef struct {
    long frame;
    char action;   /* 'E'ject, 'S'elect, 'I'nsert */
    int  side;     /* -1: the selected side */
} DiskEvent;

static DiskEvent *disk_events;
static int        disk_event_count, disk_event_next;
static unsigned   disk_selected;

/* A side: an index (0 = disk 1 side A), or A, B, 1A, 1B, 2A, ... as nesref. */
static int parse_side(const char *s) {
    if (!s || !*s) return -1;
    if (s[0] >= '1' && s[0] <= '9' && (s[1] == 'A' || s[1] == 'a' || s[1] == 'B' || s[1] == 'b') && !s[2])
        return (s[0] - '1') * 2 + (s[1] == 'B' || s[1] == 'b');
    if ((s[0] == 'A' || s[0] == 'a') && !s[1]) return 0;
    if ((s[0] == 'B' || s[0] == 'b') && !s[1]) return 1;
    char *end;
    long v = strtol(s, &end, 10);
    return end != s && !*end && v >= 0 && v < 256 ? (int)v : -1;
}

static bool add_disk_event(long frame, const char *action, const char *arg) {
    DiskEvent e = { frame, 0, -1 };
    if (!strcmp(action, "eject") || !strcmp(action, "DISK_EJECT")) e.action = 'E';
    else if (!strcmp(action, "insert") || !strcmp(action, "DISK_INSERT")) e.action = 'I';
    else if (!strcmp(action, "select") || !strcmp(action, "DISK_SELECT")) e.action = 'S';
    else return false;
    if (arg && *arg && (e.side = parse_side(arg)) < 0) return false;
    if (e.action == 'S' && e.side < 0) return false;
    DiskEvent *grown = (DiskEvent *)realloc(disk_events, sizeof(DiskEvent) * (size_t)(disk_event_count + 1));
    if (!grown) return false;
    disk_events = grown;
    /* Frame order; events of one frame keep their order. */
    int i = disk_event_count++;
    while (i > 0 && disk_events[i - 1].frame > frame) { disk_events[i] = disk_events[i - 1]; --i; }
    disk_events[i] = e;
    return true;
}

static bool parse_disk_event_arg(const char *spec) {
    char buf[64], *colon, *eq;
    snprintf(buf, sizeof(buf), "%s", spec);
    if (!(colon = strchr(buf, ':'))) return false;
    *colon = 0;
    char *end;
    long frame = strtol(buf, &end, 10);
    if (end == buf || *end || frame < 0) return false;
    char *action = colon + 1;
    if ((eq = strchr(action, '='))) *eq++ = 0;
    return add_disk_event(frame, action, eq);
}

static bool name_is(const char *s, size_t n, const char *name) {
    size_t i = 0;
    for (; i < n && name[i]; i++) {
        char a = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
        if (a != name[i]) return false;
    }
    return i == n && !name[i];
}

static uint8_t parse_buttons(const char *s) {
    static const struct { const char *name; uint8_t bit; } NAMES[] = {
        { "A", 0x80 },     { "B", 0x40 },    { "SELECT", 0x20 }, { "START", 0x10 },
        { "UP", 0x08 },    { "DOWN", 0x04 }, { "LEFT", 0x02 },   { "RIGHT", 0x01 },
    };
    uint8_t b = 0;
    while (*s) {
        while (*s == '+' || *s == ' ' || *s == '\t') s++;
        size_t n = 0;
        while (s[n] && s[n] != '+' && s[n] != ' ' && s[n] != '\t' && s[n] != '\n' && s[n] != '\r') n++;
        if (!n) break;
        for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
            if (name_is(s, n, NAMES[i].name)) {
                b |= NAMES[i].bit;
                break;
            }
        }
        s += n;
    }
    return b;
}

static bool load_input(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    int cap = 64;
    input_steps = (InputStep *)malloc(sizeof(InputStep) * cap);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char *end;
        long frame = strtol(p, &end, 10);
        if (end == p) continue;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "DISK_", 5)) {
            char action[32] = "", arg[32] = "";
            if (sscanf(p, "%31s %31s", action, arg) < 1 || !add_disk_event(frame, action, arg)) {
                fprintf(stderr, "%s: bad disk command: %s", path, p);
                fclose(f);
                return false;
            }
            continue;
        }
        uint8_t port = 0;
        if (p[0] == '2' && p[1] == ':') port = 1, p += 2;
        else if (p[0] == '1' && p[1] == ':') p += 2;
        if (input_count == cap) {
            cap *= 2;
            input_steps = (InputStep *)realloc(input_steps, sizeof(InputStep) * cap);
        }
        input_steps[input_count].frame = frame;
        input_steps[input_count].port = port;
        input_steps[input_count].buttons = (*p == '-') ? 0 : parse_buttons(p);
        input_count++;
    }
    fclose(f);
    printf("input: %d steps, %d disk events from %s\n", input_count, disk_event_count, path);
    return true;
}

static void input_tick(long frame) {
    while (input_next < input_count && input_steps[input_next].frame <= frame) {
        input_held[input_steps[input_next].port] = input_steps[input_next].buttons;
        input_next++;
    }
    cyc_set_controller(0, input_held[0]);
    cyc_set_controller(1, input_held[1]);
}

#ifndef CYC_ORACLE
static void disk_tick(long frame) {
    while (disk_event_next < disk_event_count && disk_events[disk_event_next].frame <= frame) {
        const DiskEvent *e = &disk_events[disk_event_next++];
        bool ok;
        if (e->action == 'E') {
            int side = cyc_fds_side();
            ok = cyc_fds_eject();
            if (ok) disk_selected = (unsigned)side;
            printf("[cyc disk] f=%ld eject%s\n", frame, ok ? "" : " (drive already empty)");
            if (ok) cyc_host_disk_ejected();
        } else if (e->action == 'S') {
            ok = cyc_fds_side() < 0 && (unsigned)e->side < cyc_fds_side_count();
            if (ok) disk_selected = (unsigned)e->side;
            printf("[cyc disk] f=%ld select side %d%s\n", frame, e->side,
                   ok ? "" : " (refused: drive not empty or no such side)");
        } else {
            unsigned side = e->side >= 0 ? (unsigned)e->side : disk_selected;
            ok = cyc_fds_insert(side);
            if (ok) disk_selected = side;
            printf("[cyc disk] f=%ld insert side %u%s\n", frame, side,
                   ok ? "" : " (refused: drive not empty or no such side)");
        }
    }
}

/* The BIOS identity: the one a compiled program was built from, else the one
 * the BIOS's own .toml records (bios/disksys.toml: size, crc32), else the
 * known disksys.rom (common/nes_fds.h). */
static uint32_t bios_expected_crc(const char *bios_path, uint32_t *size) {
    *size = NES_FDS_BIOS_BYTES;
    if (cyc_native_fds_bios_crc32) return cyc_native_fds_bios_crc32;
    char toml[1024];
    snprintf(toml, sizeof(toml), "%s", bios_path);
    char *dot = strrchr(toml, '.'), *slash = strrchr(toml, '/'), *bslash = strrchr(toml, '\\');
    if (dot && dot > slash && dot > bslash) *dot = 0;
    strncat(toml, ".toml", sizeof(toml) - strlen(toml) - 1);
    FILE *f = fopen(toml, "r");
    uint32_t crc = NES_FDS_BIOS_CRC32;
    if (f) {
        char line[256];
        unsigned v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, " crc32 = \"0x%x\"", &v) == 1 || sscanf(line, " crc32 = 0x%x", &v) == 1) crc = v;
            else if (sscanf(line, " size = %u", &v) == 1) *size = v;
        }
        fclose(f);
    }
    return crc;
}

static uint8_t *load_bios(const char *explicit_path, const char *image_path, size_t *size, char *used,
                          size_t used_len) {
    char beside[1024];
    const char *slash = strrchr(image_path, '/'), *bslash = strrchr(image_path, '\\');
    const char *sep = slash > bslash ? slash : bslash;
    snprintf(beside, sizeof(beside), "%.*sbios/disksys.rom", sep ? (int)(sep - image_path + 1) : 0, image_path);
    const char *candidates[] = { explicit_path, explicit_path ? NULL : cyc_native_fds_bios_path,
                                 explicit_path ? NULL : beside, explicit_path ? NULL : "bios/disksys.rom" };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (!candidates[i]) continue;
        uint8_t *data = read_file(candidates[i], size);
        if (!data) continue;
        snprintf(used, used_len, "%s", candidates[i]);
        return data;
    }
    return NULL;
}

/* ---- the FDS HLE tier: requests, capability facts, the plan ---- */
static NesFdsHleRequest hle_req;
static NesFdsHlePlan    hle_plan;
static CycFdsHle        hle_core;
static char             hle_text[48];

/* The disk-ID check anchor of the BIOS: the built-in one for a known image
 * (verified against its code), else hle_id_check / hle_id_pointer in the
 * BIOS's identity file (<bios>.toml), which must name a JSR in the BIOS. */
static NesFdsHleAnchor hle_anchor(const char *bios_path, uint32_t bios_crc) {
    NesFdsHleAnchor a = nes_fds_hle_builtin_anchor(bios_crc);
    if (!a.id_check) {
        char toml[1024];
        snprintf(toml, sizeof(toml), "%s", bios_path);
        char *dot = strrchr(toml, '.'), *slash = strrchr(toml, '/'), *bslash = strrchr(toml, '\\');
        if (dot && dot > slash && dot > bslash) *dot = 0;
        strncat(toml, ".toml", sizeof(toml) - strlen(toml) - 1);
        FILE *f = fopen(toml, "r");
        if (f) {
            char line[256];
            unsigned v;
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, " hle_id_check = \"0x%x\"", &v) == 1 || sscanf(line, " hle_id_check = 0x%x", &v) == 1)
                    a.id_check = (uint16_t)v;
                else if (sscanf(line, " hle_id_pointer = \"0x%x\"", &v) == 1 ||
                         sscanf(line, " hle_id_pointer = 0x%x", &v) == 1)
                    a.id_pointer = (uint8_t)v;
            }
            fclose(f);
        }
        if (a.id_check) { a.check_len = 1; a.check[0] = 0x20; }   /* a JSR */
    }
    uint8_t b;
    if (a.id_check && a.id_check < 0xE000) a.id_check = 0;
    for (unsigned i = 0; a.id_check && i < a.check_len; ++i)
        if (!cyc_debug_peek((uint16_t)(a.id_check + i), &b) || b != a.check[i]) a.id_check = 0;
    return a;
}

/* The BIOS's boot model (common/nes_fds_hle.h): the built-in one for a known
 * image, else hle_boot_load / hle_boot_load_entry / hle_boot_jump in the
 * BIOS's identity file. Verified against the code: the call is JSR
 * load_entry, the jump is JMP ($DFFC), the license loop starts as the model
 * says; a part that does not verify is dropped. */
static NesFdsBootModel boot_model(const char *bios_path, uint32_t bios_crc) {
    NesFdsBootModel m = nes_fds_boot_builtin_model(bios_crc);
    if (!m.load_call && !m.jump) {
        char toml[1024];
        snprintf(toml, sizeof(toml), "%s", bios_path);
        char *dot = strrchr(toml, '.'), *slash = strrchr(toml, '/'), *bslash = strrchr(toml, '\\');
        if (dot && dot > slash && dot > bslash) *dot = 0;
        strncat(toml, ".toml", sizeof(toml) - strlen(toml) - 1);
        FILE *f = fopen(toml, "r");
        if (f) {
            char line[256];
            unsigned v;
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, " hle_boot_load = \"0x%x\"", &v) == 1) m.load_call = (uint16_t)v;
                else if (sscanf(line, " hle_boot_load_entry = \"0x%x\"", &v) == 1) m.load_entry = (uint16_t)v;
                else if (sscanf(line, " hle_boot_jump = \"0x%x\"", &v) == 1) m.jump = (uint16_t)v;
                else if (sscanf(line, " hle_boot_mask_store = \"0x%x\"", &v) == 1) m.mask_store = (uint16_t)v;
            }
            fclose(f);
        }
        m.load_check[0] = 0x20; m.load_check[1] = (uint8_t)m.load_entry; m.load_check[2] = (uint8_t)(m.load_entry >> 8);
        m.jump_check[0] = 0x6C; m.jump_check[1] = 0xFC; m.jump_check[2] = 0xDF;
        m.mask_check[0] = 0x8D; m.mask_check[1] = 0x01; m.mask_check[2] = 0x20;
    }
    uint8_t b;
    for (unsigned i = 0; m.load_call && i < 3; ++i)
        if (m.load_call < 0xE000 || !cyc_debug_peek((uint16_t)(m.load_call + i), &b) || b != m.load_check[i])
            m.load_call = 0;
    for (unsigned i = 0; m.jump && i < 3; ++i)
        if (m.jump < 0xE000 || !cyc_debug_peek((uint16_t)(m.jump + i), &b) || b != m.jump_check[i]) m.jump = 0;
    for (unsigned i = 0; m.loop_branch && i < 4; ++i)
        if (!cyc_debug_peek((uint16_t)(m.loop_entry + i), &b) || b != m.loop_check[i]) m.loop_branch = 0;
    for (unsigned i = 0; m.mask_store && i < 3; ++i)
        if (!cyc_debug_peek((uint16_t)(m.mask_store + i), &b) || b != m.mask_check[i]) m.mask_store = 0;
    if (!m.load_call) m.load_entry = 0;
    return m;
}

/* A proof record for this disk: built in, or listed in hle_boot_proven in the
 * BIOS's identity file (common/nes_fds_boot.h). */
static bool boot_proven(const char *bios_path, uint32_t disk_id) {
    if (nes_fds_boot_proven(disk_id)) return true;
    char toml[1024];
    snprintf(toml, sizeof(toml), "%s", bios_path);
    char *dot = strrchr(toml, '.'), *slash = strrchr(toml, '/'), *bslash = strrchr(toml, '\\');
    if (dot && dot > slash && dot > bslash) *dot = 0;
    strncat(toml, ".toml", sizeof(toml) - strlen(toml) - 1);
    FILE *f = fopen(toml, "r");
    bool found = false;
    if (f) {
        char line[256];
        unsigned v;
        /* hle_boot_proven = ["0x...", ...] on one line */
        while (fgets(line, sizeof(line), f)) {
            const char *q = strstr(line, "hle_boot_proven");
            if (!q || !(q = strchr(q, '='))) continue;
            for (; (q = strstr(q, "0x")) != NULL; q += 2)
                if (sscanf(q, "0x%x", &v) == 1 && v == disk_id) found = true;
        }
        fclose(f);
    }
    return found;
}

static CycFdsBootModel boot_core_model(const NesFdsBootModel *m) {
    CycFdsBootModel c;
    memset(&c, 0, sizeof(c));
    c.load_call = m->load_call; c.load_entry = m->load_entry; c.jump = m->jump;
    c.loop_branch = m->loop_branch; c.loop_entry = m->loop_entry; c.loop_top = m->loop_top;
    c.loop_counter = m->loop_counter; c.timer_divider = m->timer_divider; c.fast_last = m->fast_last;
    c.slow_last = m->slow_last; c.divider_reload = m->divider_reload;
    c.scroll = m->scroll; c.scroll_step = m->scroll_step; c.scroll_limit = m->scroll_limit;
    c.license = m->license; c.license_vram = m->license_vram; c.license_len = m->license_len;
    c.mask_store = m->mask_store;
    return c;
}

static void hle_describe(void) {
    /* The drive bar's font: capitals, digits, space and -.:/()= */
    const NesFdsHlePlan *p = &hle_plan;
    snprintf(hle_text, sizeof(hle_text), "HLE%s%s%s%s%s", p->boot_skip ? " BOOT" : "", p->auto_swap ? " SWAP" : "",
             p->fast_load ? " FAST" : "", !p->auto_swap && !p->fast_load && !p->boot_skip ? " OFF" : "",
             p->auto_swap_denied ? " (NO SWAP)" : "");
}

/* The boot axes are decided once, before power-on; said whenever the boot
 * skip was asked for or auto insert was asked for or will act. */
static void boot_banner(void) {
    const NesFdsHlePlan *p = &hle_plan;
    const NesFdsHleAsk *asks[4] = { &hle_req.config, &hle_req.env, &hle_req.cli, &hle_req.live };
    bool skip_asked = false, insert_asked = false;
    for (int i = 0; i < 4; ++i) {
        skip_asked |= asks[i]->boot_skip >= 0;
        insert_asked |= asks[i]->auto_insert >= 0;
    }
    bool insert_acts = p->auto_insert && cyc_fds_side() < 0 && cyc_fds_side_count() > 0;
    if (!skip_asked && !insert_asked && !insert_acts) return;
    printf("fds boot: boot-skip %s (%s)%s%s, auto-insert %s (%s)%s%s%s\n",
           p->boot_skip ? "on" : p->boot_skip_denied ? "REFUSED" : "off", p->boot_skip_from,
           p->boot_skip_denied ? ": " : "", p->boot_skip_denied ? p->boot_skip_why : "",
           p->auto_insert ? "on" : p->auto_insert_denied ? "REFUSED" : "off", p->auto_insert_from,
           p->auto_insert_denied ? ": " : "", p->auto_insert_denied ? p->auto_insert_why : "",
           p->auto_insert && cyc_fds_side() >= 0 ? " (a disk is in: nothing to do)" : "");
}

static void hle_apply(bool banner) {
    hle_plan = nes_fds_hle_plan(hle_req);
    hle_core.auto_swap = hle_plan.auto_swap;
    hle_core.fast_load = hle_plan.fast_load;
    cyc_fds_hle_configure(&hle_core);
    hle_describe();
    if (!banner) return;
    /* Said whenever any source asked anything, so a refusal is never silent. */
    const NesFdsHlePlan *p = &hle_plan;
    const NesFdsHleAsk *asks[4] = { &hle_req.config, &hle_req.env, &hle_req.cli, &hle_req.live };
    bool asked = false;
    for (int i = 0; i < 4; ++i) asked |= asks[i]->auto_swap >= 0 || asks[i]->fast_load >= 0;
    if (asked)
        printf("fds hle: auto-swap %s (%s)%s%s, fast-load %s (%s)%s%s\n",
               p->auto_swap ? "on" : p->auto_swap_denied ? "REFUSED" : "off", p->auto_swap_from,
               p->auto_swap_denied ? ": " : "", p->auto_swap_denied ? p->auto_swap_why : "",
               p->fast_load ? "on" : p->fast_load_denied ? "REFUSED" : "off", p->fast_load_from,
               p->fast_load_denied ? ": " : "", p->fast_load_denied ? p->fast_load_why : "");
}

const char *cyc_host_hle_toggle(int axis) {
    if (axis == 0) hle_req.live.auto_swap = hle_plan.auto_swap ? 0 : 1;
    else hle_req.live.fast_load = hle_plan.fast_load ? 0 : 1;
    hle_apply(true);
    fflush(stdout);
    return hle_text;
}

const char *cyc_host_hle_text(void) { return hle_text; }

/* --realtime and the window: the console's frame rate, and fast load. */
static const double FRAME_SECONDS = 1.0 / 60.0988;
double cyc_host_frame_seconds(void) { return FRAME_SECONDS; }
bool   cyc_host_frame_unpaced(void) { return hle_plan.fast_load && cyc_fds_hle_loading(); }

static double wall_seconds(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static void wait_until(double t) {
    for (;;) {
        double left = t - wall_seconds();
        if (left <= 0) return;
#ifdef _WIN32
        if (left > 0.002) Sleep((DWORD)((left - 0.001) * 1000));
#else
        if (left > 0.002) usleep((useconds_t)((left - 0.001) * 1e6));
#endif
    }
}

/* --frame-log: "CYCFRAME" + u32 version (2), then per frame
 *   u32 frame, u32 cycles low, u32 cycles high, u32 lengths[6] (CPU RAM,
 *   CIRAM, cartridge RAM, CHR RAM, picture bytes, FDS sound unit state),
 *   u8 palette[32], u8 oam[256], then the six blobs; the picture is 256x240
 *   little-endian u16 color indices (color | emphasis << 6), the FDS state
 *   cyc_fds_audio_state() (0 bytes on a cartridge). Version 1 had no FDS
 *   length or blob. */
static void write_frame_log(FILE *f, long frame) {
    size_t ciram_len, cart_len, chr_len;
    const uint8_t *ciram = cyc_ppu_ciram(&ciram_len), *cart = cyc_cart_ram(&cart_len), *chr = cyc_chr_ram(&chr_len);
    uint8_t audio[CYC_FDS_AUDIO_STATE_BYTES];
    size_t audio_len = cyc_is_fds() ? cyc_fds_audio_state(audio) : 0;
    uint64_t cycles = cyc_cycle_count();
    uint32_t head[9] = { (uint32_t)frame, (uint32_t)cycles, (uint32_t)(cycles >> 32), 0x800,
                         (uint32_t)ciram_len, (uint32_t)cart_len, (uint32_t)chr_len, 256 * 240 * 2,
                         (uint32_t)audio_len };
    fwrite(head, sizeof(head), 1, f);
    fwrite(cyc_ppu_palette(), 1, 32, f);
    fwrite(cyc_ppu_oam(), 1, 256, f);
    fwrite(cyc_cpu_ram(), 1, 0x800, f);
    fwrite(ciram, 1, ciram_len, f);
    if (cart_len) fwrite(cart, 1, cart_len, f);
    if (chr_len) fwrite(chr, 1, chr_len, f);
    fwrite(cyc_frame_index(), 2, 256 * 240, f);
    if (audio_len) fwrite(audio, 1, audio_len, f);
}
#endif

#ifndef CYC_ORACLE
/* --frame-log-at mesen: the record is taken at Mesen's frame end instead. */
static FILE *observe_log;
static long  observe_frame, observe_first, observe_last;
static void observe_frame_log(void) {
    if (observe_frame >= observe_first && (observe_last < 0 || observe_frame <= observe_last))
        write_frame_log(observe_log, observe_frame);
}
#endif

#ifndef CYC_ORACLE
/* --boot-state-out: the whole machine at the FDS game's first instruction
 * (after the BIOS's jump into it, or where the boot skip starts it): CPU
 * registers and interrupt latches, every hashed hardware field
 * (cyc_hw_state_dump) and the memories and picture (cyc_mem_state_dump). */
static const char *boot_state_path, *watch_state_path;
static long        boot_state_frame;
static unsigned    watch_state_pc;
static void write_state_file(const char *path);
static void write_boot_state(void) {
    /* the watch (--state-at-pc) stops first when it comes before the entry */
    if (watch_state_path && cyc_fds_boot_watch_hit()) {
        write_state_file(watch_state_path);
        watch_state_path = NULL;
        cyc_fds_boot_watch(0);
        hw_entry_stop = boot_state_path != NULL;
        CycFdsBootStatus st;
        cyc_fds_boot_status(&st);
        if (!st.entered) return;
    }
    if (!boot_state_path) return;
    CycFdsBootStatus st;
    cyc_fds_boot_status(&st);
    if (!st.entered) return;
    write_state_file(boot_state_path);
    boot_state_path = NULL;
    hw_entry_stop = watch_state_path != NULL;
}
static void write_state_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    CycFdsBootStatus st;
    cyc_fds_boot_status(&st);
    fprintf(f, "boot.frame %ld\nboot.skipped %u\nboot.entry %04X\nboot.line %u\nboot.dot %u\n", boot_state_frame,
            st.skipped, st.entry_pc, st.entry_line, st.entry_dot);
    fprintf(f, "cpu.pc %04X\ncpu.a %02X\ncpu.x %02X\ncpu.y %02X\ncpu.s %02X\ncpu.p %02X\ncpu.nmi_input %u\n"
               "cpu.nmi_edge %u\ncpu.do_nmi %u\ncpu.do_irq %u\ncpu.do_reset %u\n",
            cpu.pc, cpu.a, cpu.x, cpu.y, cpu.s, cpu_get_p(0) & 0xCF, cpu.nmi_input, cpu.nmi_edge, cpu.do_nmi,
            cpu.do_irq, cpu.do_reset);
    cyc_hw_state_dump(f);
    cyc_mem_state_dump(f);
    fclose(f);
    printf("boot state: %s game entry at $%04X, frame %ld, line %u dot %u -> %s\n", st.skipped ? "skipped" : "BIOS",
           st.entry_pc, boot_state_frame, st.entry_line, st.entry_dot, path);
}
#endif

static bool parse_range(const char *s, long *a, long *b) {
    char *end;
    *a = strtol(s, &end, 10);
    if (end == s || *end != ':') return false;
    const char *t = end + 1;
    *b = strtol(t, &end, 10);
    return end != t && !*end && *a >= 0 && *b >= *a;
}

static void write_hash_line(FILE *f, long frame) {
    CycCpuState s;
    cyc_cpu_state(&s);
    fprintf(f, "%ld trace=%016llX mem=%016llX PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X int=%u%u cycles=%llu "
               "hw=%016llX\n",
            frame, (unsigned long long)cyc_trace_hash, (unsigned long long)cyc_mem_state_hash(), s.pc, s.a, s.x, s.y,
            s.s, s.p, s.do_nmi, s.do_irq, (unsigned long long)cyc_cycle_count(),
            (unsigned long long)cyc_hw_state_hash());
}

/* 16-bit mono PCM WAV, header rewritten with the final length on close. */
static void wav_header(FILE *f, int rate, uint32_t samples) {
    uint32_t data = samples * 2;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0};
    uint32_t v[] = {36 + data, (uint32_t)rate, (uint32_t)rate * 2};
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 24, &v[1], 4);
    memcpy(h + 28, &v[2], 4);
    h[32] = 2, h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
}

/* "shots/run.png" + frame 120 -> "shots/run_00120.png" */
static void numbered_path(char *buf, size_t n, const char *base, long frame) {
    const char *slash = strrchr(base, '/'), *bslash = strrchr(base, '\\');
    const char *name = slash > bslash ? slash : bslash;
    const char *dot = strrchr(name ? name : base, '.');
    int stem = (int)(dot ? (size_t)(dot - base) : strlen(base));
    snprintf(buf, n, "%.*s_%05ld%s", stem, base, frame, dot ? dot : ".png");
}

#include "cyc_save.inc"
#ifndef CYC_ORACLE
#include "cyc_fds_save.inc"
#endif

int main(int argc, char **argv) {
    const char *rom_path = NULL, *hash_out = NULL, *trace_out = NULL, *screenshot = NULL, *state_out = NULL,
               *wav_out = NULL, *mem_out = NULL;
    const char *save_file=NULL, *datach_save=NULL, *barcode=NULL;
    long barcode_frame=0; unsigned barcode_speed=1000;
    long mem_frame = -1, shot_every = 0;
#ifndef CYC_ORACLE
    const char *miss_log = NULL, *capture_log = NULL, *view_list = NULL;
#endif
    int align = 0, scale = 3;
    CycConsole console = CYC_CONSOLE_DEFAULT;
    bool console_given = false;
    long frames = 600, trace_frame = -1, state_frame = -1;
    int spam_page = -1, spam_row = 0;
    const char *input_file = NULL;
    unsigned spam_seed = 1;
    bool spam_dpad = true;
    bool acccoin = false, headless = false, frames_given = false;
#ifndef CYC_ORACLE
    const char *ring_out = NULL, *frame_log = NULL, *fds_bios = NULL;
    const char *fds_import_ips = NULL, *fds_export_ips = NULL, *fds_hle_arg = NULL;
    bool frame_log_mesen = false, no_save = false, realtime = false;
    long ring_first = 0, ring_last = -1, log_first = 0, log_last = -1;
    CycFdsOptions fds_opt;
    cyc_fds_default_options(&fds_opt);
    cyc_ring_dump_at_exit_from_env();
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spam") && i + 2 < argc) {
            spam_page = atoi(argv[++i]);
            spam_row = atoi(argv[++i]);
            headless = true;
        }
        else if (!strcmp(argv[i], "--spam-seed") && i + 1 < argc) spam_seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--spam-no-dpad")) spam_dpad = false;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_file = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--datach-save-file") && i+1<argc) datach_save=argv[++i];
        else if (!strcmp(argv[i], "--barcode") && i+1<argc) { barcode=argv[++i]; headless=true; }
        else if (!strcmp(argv[i], "--barcode-frame") && i+1<argc) barcode_frame=atol(argv[++i]);
        else if (!strcmp(argv[i], "--barcode-module-cycles") && i+1<argc) barcode_speed=(unsigned)strtoul(argv[++i],NULL,10);
        else if (!strcmp(argv[i], "--save-file") && i + 1 < argc) save_file=argv[++i];
        else if (!strcmp(argv[i], "--align") && i + 1 < argc) align = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ram-init") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "zeros")) cyc_ram_init = CYC_RAM_ZEROS;
            else if (!strcmp(m, "ones")) cyc_ram_init = CYC_RAM_ONES;
            else if (!strcmp(m, "pattern")) cyc_ram_init = CYC_RAM_PATTERN;
            else { fprintf(stderr, "--ram-init: zeros, ones or pattern\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]), frames_given = headless = true;
        else if (!strcmp(argv[i], "--acccoin")) acccoin = headless = true;
        else if (!strcmp(argv[i], "--hash-out") && i + 1 < argc) hash_out = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--trace-frame") && i + 1 < argc) trace_frame = atol(argv[++i]), headless = true;
        else if (!strcmp(argv[i], "--trace-out") && i + 1 < argc) trace_out = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) shot_every = atol(argv[++i]), headless = true;
        else if (!strcmp(argv[i], "--state-frame") && i + 1 < argc) state_frame = atol(argv[++i]), headless = true;
        else if (!strcmp(argv[i], "--state-out") && i + 1 < argc) state_out = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--wav-out") && i + 1 < argc) wav_out = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--console") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "nes")) console = CYC_CONSOLE_NES;
            else if (!strcmp(v, "famicom")) console = CYC_CONSOLE_FAMICOM;
            else if (!strcmp(v, "default")) console = CYC_CONSOLE_DEFAULT;
            else { fprintf(stderr, "--console: nes, famicom or default\n"); return 2; }
            console_given = true;
        }
        else if (!strcmp(argv[i], "--mem-frame") && i + 1 < argc) mem_frame = atol(argv[++i]), headless = true;
        else if (!strcmp(argv[i], "--mem-out") && i + 1 < argc) mem_out = argv[++i], headless = true;
#ifndef CYC_ORACLE
        else if (!strcmp(argv[i], "--interp-only")) cyc_run_native = false;
        else if (!strcmp(argv[i], "--ring-out") && i + 1 < argc) ring_out = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--frame-log") && i + 1 < argc) frame_log = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--frame-log-at") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "mesen")) frame_log_mesen = true;
            else if (strcmp(v, "vblank")) { fprintf(stderr, "--frame-log-at: vblank or mesen\n"); return 2; }
        }
        else if ((!strcmp(argv[i], "--ring-frames") || !strcmp(argv[i], "--frame-log-frames")) && i + 1 < argc) {
            bool ring = argv[i][2] == 'r';
            if (!parse_range(argv[++i], ring ? &ring_first : &log_first, ring ? &ring_last : &log_last)) {
                fprintf(stderr, "%s: expected FIRST:LAST\n", argv[i - 1]);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--fds-bios") && i + 1 < argc) fds_bios = argv[++i];
        else if (!strcmp(argv[i], "--fds-boot-disk") && i + 1 < argc) {
            const char *v = argv[++i];
            fds_opt.boot_side = !strcmp(v, "none") ? -1 : parse_side(v);
            if (fds_opt.boot_side < 0 && strcmp(v, "none")) { fprintf(stderr, "--fds-boot-disk: none or a side\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--fds-event") && i + 1 < argc) {
            if (!parse_disk_event_arg(argv[++i])) {
                fprintf(stderr, "--fds-event: FRAME:eject, FRAME:insert[=SIDE] or FRAME:select=SIDE\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--fds-profile") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "mesen")) fds_opt.profile = CYC_FDS_PROFILE_MESEN;
            else if (!strcmp(v, "mesen2")) fds_opt.profile = CYC_FDS_PROFILE_MESEN2;
            else if (!strcmp(v, "hardware")) fds_opt.profile = CYC_FDS_PROFILE_HARDWARE;
            else { fprintf(stderr, "--fds-profile: mesen, mesen2 or hardware\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--fds-crc") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "computed")) fds_opt.stream_crc = CYC_FDS_CRC_COMPUTED;
            else if (!strcmp(v, "mesen")) fds_opt.stream_crc = CYC_FDS_CRC_MESEN;
            else { fprintf(stderr, "--fds-crc: computed or mesen\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--fds-crc-check")) fds_opt.crc_check = true;
        else if (!strcmp(argv[i], "--fds-write-protect")) fds_opt.write_protect = true;
        else if (!strcmp(argv[i], "--fds-write-at") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "head")) fds_opt.write_at = CYC_FDS_WRITE_HEAD;
            else if (!strcmp(v, "mesen")) fds_opt.write_at = CYC_FDS_WRITE_MESEN;
            else { fprintf(stderr, "--fds-write-at: head or mesen\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--fds-import-ips") && i + 1 < argc) fds_import_ips = argv[++i];
        else if (!strcmp(argv[i], "--fds-export-ips") && i + 1 < argc) fds_export_ips = argv[++i];
        else if (!strcmp(argv[i], "--no-save")) no_save = true;
        else if (!strcmp(argv[i], "--fds-hle") && i + 1 < argc) fds_hle_arg = argv[++i];
        else if (!strcmp(argv[i], "--boot-state-out") && i + 1 < argc) boot_state_path = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--state-at-pc") && i + 2 < argc) {
            watch_state_pc = (unsigned)strtoul(argv[++i], NULL, 16);
            watch_state_path = argv[++i];
            headless = true;
        }
        else if (!strcmp(argv[i], "--realtime")) realtime = headless = true;
        else if (!strcmp(argv[i], "--miss-log") && i + 1 < argc) miss_log = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--capture-log") && i + 1 < argc) capture_log = argv[++i], headless = true;
        else if (!strcmp(argv[i], "--ram-view-list") && i + 1 < argc) view_list = argv[++i], headless = true;
#endif
        else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    bool bios_only = false;
#ifndef CYC_ORACLE
    if (!rom_path) rom_path = cyc_native_fds_image_path;   /* game.toml [fds] image */
    /* A program compiled from the BIOS alone (NESRecomp --fds-bios-only, the
     * runner/cyc/fds-bios showcase) runs it with no disk. */
    bios_only = !rom_path && cyc_native_fds_bios_crc32;
#endif
    if (!rom_path && !bios_only) {
        fprintf(stderr, "usage: %s <rom.nes | disk.fds> [--interp-only] [--align N] [--scale N]\n"
                        "       [--save-file FILE] (raw battery RAM/EEPROM, loaded and atomically saved)\n"
                        "       [--datach-save-file FILE] (shared internal EEPROM)\n"
                        "       [--barcode DIGITS --barcode-frame N --barcode-module-cycles N]\n"
                        "       headless: [--frames N] [--acccoin] [--hash-out FILE]\n"
                        "       [--spam PAGE ROW] [--spam-seed N] [--spam-no-dpad] [--input FILE]\n"
                        "       [--trace-frame N --trace-out FILE] [--state-frame N --state-out FILE]\n"
                        "       [--miss-log FILE] [--capture-log FILE] [--ram-view-list FILE]\n"
                        "       [--screenshot FILE [--shot-every N]] [--wav-out FILE]\n"
                        "       [--console nes|famicom|default]\n"
                        "       [--frame-log FILE [--frame-log-frames A:B]] [--ring-out FILE [--ring-frames A:B]]\n"
                        "       FDS: [--fds-bios FILE] [--fds-boot-disk none|SIDE] [--fds-event F:ACTION]\n"
                        "            [--fds-profile mesen|mesen2|hardware] [--fds-crc computed|mesen]\n"
                        "            [--fds-crc-check] [--fds-write-protect] [--fds-write-at head|mesen]\n"
                        "            [--save-file FILE | --no-save] [--fds-import-ips FILE] [--fds-export-ips FILE]\n"
                        "            [--fds-hle boot-skip,auto-swap,fast-load,auto-insert|all|off] [--realtime]\n"
                        "            [--boot-state-out FILE] [--state-at-pc PC FILE]\n",
                argv[0]);
        return 2;
    }
    size_t size = 0;
    uint8_t *image = bios_only ? NULL : read_file(rom_path, &size);
    NesCartInfo cart_info;
    NesFdsImage fds_image;
    memset(&fds_image, 0, sizeof(fds_image));
    const char *ext = bios_only ? NULL : strrchr(rom_path, '.');
    bool qd = ext && (!strcmp(ext, ".qd") || !strcmp(ext, ".QD"));
    bool fds = bios_only || (image && !nes_cart_image(image, size, &cart_info) &&
                             nes_fds_image(image, size, qd ? NES_FDS_QD : NES_FDS_NONE, &fds_image));
    if (bios_only) rom_path = "";
    if (fds) {
#ifdef CYC_ORACLE
        fprintf(stderr, "%s is a Famicom Disk System image; the TriCNES oracle has no RAM Adapter "
                        "(the FDS oracle is nesref)\n", rom_path);
        return 2;
#else
        if (acccoin || spam_page >= 0) { fprintf(stderr, "--acccoin/--spam need a cartridge\n"); return 2; }
        size_t bios_size = 0;
        char bios_path[1024];
        uint8_t *bios = load_bios(fds_bios, rom_path, &bios_size, bios_path, sizeof(bios_path));
        if (bios_only) fds_opt.boot_side = -1;
        if (!bios) {
            fprintf(stderr, "%s: no FDS BIOS (give --fds-bios FILE, or put disksys.rom in bios/)\n", rom_path);
            return 2;
        }
        uint32_t want_size, want_crc = bios_expected_crc(bios_path, &want_size);
        uint32_t crc = nes_crc32(0, bios, bios_size);
        if (bios_size != want_size || bios_size != NES_FDS_BIOS_BYTES || crc != want_crc) {
            fprintf(stderr, "%s is not the expected FDS BIOS (%zu bytes, CRC32 %08X; expected %u bytes, CRC32 %08X)\n",
                    bios_path, bios_size, crc, want_size, want_crc);
            return 2;
        }
        fds_opt.qd = qd;
        if (!cyc_load_fds(bios, bios_size, bios_only ? NULL : image, size, &fds_opt)) {
            fprintf(stderr, "cannot load disk image %s\n", rom_path);
            return 2;
        }
        free(bios);
        /* The disk save: explicit, or the windowed default; never the image. */
        const char *disk_save = save_file;
#if defined(CYC_WITH_SDL)
        if (!disk_save && !headless && !no_save) disk_save = fds_default_save_path(rom_path);
#endif
        if (no_save) disk_save = NULL;
        const char *mine[] = { disk_save, fds_export_ips, fds_import_ips };
        for (size_t k = 0; k < 3; ++k)
            if (mine[k] && !save_paths_distinct(mine[k], rom_path)) {
                fprintf(stderr, "%s is the disk image; saves never write the image\n", mine[k]);
                return 2;
            }
        if (disk_save && fds_export_ips && !save_paths_distinct(disk_save, fds_export_ips)) {
            fprintf(stderr, "the disk save and --fds-export-ips must be different files\n");
            return 2;
        }
        if (bios_only) disk_save = fds_import_ips = fds_export_ips = NULL;
        if (!bios_only && !fds_save_open(disk_save, fds_import_ips, fds_export_ips, image, size, &fds_opt)) return 2;
        save_file = NULL;              /* the FDS has no cartridge NVRAM */
        nes_fds_cart_info(&cart_info);
        /* The HLE plan: requests from game.toml, the environment and the
         * command line, capability facts from the BIOS and the disk. */
        const char *bad = NULL;
        if (!nes_fds_hle_parse(cyc_native_fds_hle, &hle_req.config, &bad)) {
            fprintf(stderr, "game.toml [fds] hle: unknown word at '%s'\n", bad);
            return 2;
        }
        if (!nes_fds_hle_parse(getenv("NESRECOMP_FDS_HLE"), &hle_req.env, &bad)) {
            fprintf(stderr, "NESRECOMP_FDS_HLE: unknown word at '%s' (boot-skip, auto-swap, fast-load, auto-insert, "
                            "all, off)\n", bad);
            return 2;
        }
        if (!nes_fds_hle_parse(fds_hle_arg, &hle_req.cli, &bad)) {
            fprintf(stderr, "--fds-hle: unknown word at '%s' (boot-skip, auto-swap, fast-load, auto-insert, all, "
                            "off)\n", bad);
            return 2;
        }
        hle_req.live = NES_FDS_HLE_ASK_NONE;
        NesFdsHleAnchor anchor = hle_anchor(bios_path, crc);
        hle_core.id_check = anchor.id_check;
        hle_core.id_pointer = anchor.id_pointer;
        hle_req.is_fds = true;
        hle_req.have_anchor = anchor.id_check != 0;
        hle_req.sides = cyc_fds_side_count();
        for (unsigned s = 0; s < hle_req.sides; ++s) {
            uint32_t len;
            uint8_t block1[56];
            const uint8_t *stream = cyc_fds_side_stream(s, &len);
            hle_req.sides_with_id += stream && nes_fds_hle_block1(stream, len, block1);
        }
        /* The boot model; the boot skip's analysis runs only when it is asked
         * for (it runs the BIOS's intro once, before power-on). */
        NesFdsBootModel bm = boot_model(bios_path, crc);
        hle_req.have_boot = bm.load_call && bm.jump;
        hle_req.have_jump = bm.jump != 0;
        hle_req.boot_why = NULL;
        static char disk_why[96];
        if (nes_fds_hle_plan(hle_req).boot_skip) {
            uint32_t disk_id = nes_fds_boot_disk_id(&fds_image);
            if (!boot_proven(bios_path, disk_id)) {
                snprintf(disk_why, sizeof(disk_why), "no equivalence proof for this disk (side A CRC32 %08X)", disk_id);
                hle_req.boot_why = disk_why;
            } else {
                CycFdsBootModel cm = boot_core_model(&bm);
                hle_req.boot_why = cyc_fds_skip_prepare(&cm, (uint8_t)align);
            }
        }
        hle_apply(true);
        boot_banner();
        CycFdsBoot boot_core = { bm.jump, hle_plan.boot_skip, hle_plan.boot_skip_denied, hle_plan.auto_insert };
        cyc_fds_boot_configure(&boot_core);
        cyc_fds_skip_enable(hle_plan.boot_skip);
        printf("fds: %s, %u side%s, BIOS %s (CRC32 %08X), drive %s\n", bios_only ? "no disk (the BIOS alone)" : rom_path,
               cyc_fds_side_count(),
               cyc_fds_side_count() == 1 ? "" : "s", bios_path, crc,
               cyc_fds_side() < 0 ? "empty" : "loaded");
#endif
    } else if (!image || !nes_cart_image(image, size, &cart_info) || !cyc_load_ines(image, size)) {
        fprintf(stderr, "cannot load %s (invalid or unsupported cartridge; see runner/cyc/MAPPERS.md)\n", rom_path);
        return 2;
    }
#ifndef CYC_ORACLE
    if (cyc_native_program_name && nes_cart_identity(&cart_info) != cyc_native_cart_hash) {
        fprintf(stderr, "Cartridge metadata differs from the compiled program; regenerate native code\n");
        return 2;
    }
    if (cyc_native_program_name && cyc_prg_hash() != cyc_native_prg_hash) {
        fprintf(stderr, "PRG ROM does not match the ROM '%s' was recompiled from (hash %08X, expected %08X)\n",
                cyc_native_program_name, cyc_prg_hash(), cyc_native_prg_hash);
        return 2;
    }
#endif
    if (!save_paths_distinct(save_file,datach_save)) {
        fprintf(stderr,"cartridge and Datach saves must use different files\n"); return 2;
    }
    if (!save_load(save_file,0) || !save_load(datach_save,1)) return 2;
    if (barcode && (barcode_frame<0 || !cyc_scan_barcode(barcode,barcode_speed))) {
        fprintf(stderr,"barcode requires Datach, 8/12/13 digits with valid checksum, and a positive module duration\n"); return 2;
    }
#ifndef CYC_ORACLE
    if (!console_given) console = (CycConsole)cyc_native_console;   /* game.toml [game] console */
#else
    (void)console_given;
#endif
    cyc_set_console(console);
    cyc_power_on((uint8_t)align);
#ifndef CYC_ORACLE
    cyc_run_power_on();
    fds_save_powered_on();
#endif

#if defined(CYC_WITH_SDL) && !defined(CYC_ORACLE)
    if (!headless) {
        int result=cyc_sdl_main(cyc_native_program_name ? cyc_native_program_name : rom_path, scale);
        bool disk_ok=fds_save_flush(CYC_FDS_SAVE_EXIT);
        return save_write(save_file,0) && save_write(datach_save,1) && disk_ok?result:2;
    }
#else
    (void)scale;
    (void)headless;
#endif

    FILE *hash_f = NULL, *trace_f = NULL;
    if (hash_out) {
        hash_f = fopen(hash_out, "w");
        cyc_trace_enabled = true;
    }
    if (trace_out && trace_frame >= 0) {
        trace_f = fopen(trace_out, "w");
        cyc_trace_enabled = true;
    }
#ifndef CYC_ORACLE
    if (capture_log) cyc_ramview_capture_start();
    if (miss_log) {
        cyc_run_miss = (uint32_t *)calloc(cyc_run_miss_slots(), sizeof(uint32_t));
        cyc_run_ram_miss = (uint32_t *)calloc(0x10000, sizeof(uint32_t));
        cyc_run_ram_opcodes = (uint8_t *)calloc((size_t)0x10000 * 32, 1);
    }
    FILE *frame_log_f = NULL;
    if (frame_log) {
        if (!(frame_log_f = fopen(frame_log, "wb"))) { fprintf(stderr, "cannot write %s\n", frame_log); return 2; }
        uint32_t version = 2;
        fwrite("CYCFRAME", 1, 8, frame_log_f);
        fwrite(&version, 4, 1, frame_log_f);
        if (frame_log_mesen) {
            observe_log = frame_log_f;
            observe_first = log_first;
            observe_last = log_last;
            hw_observe_line = 240;
            cyc_run_observer = observe_frame_log;
        }
    }
#endif
    enum { WAV_RATE = 48000 };
    FILE *wav_f = NULL;
    uint32_t wav_samples = 0;
    if (wav_out) {
        if (!cyc_audio_enable(WAV_RATE)) {
            fprintf(stderr, "%s has no audio output\n", cyc_hw_name());
        } else if ((wav_f = fopen(wav_out, "wb")) != NULL) {
            wav_header(wav_f, WAV_RATE, 0);
            printf("audio: %d Hz, %s output stage%s\n", WAV_RATE, cyc_console_name(cyc_console()),
                   console == CYC_CONSOLE_DEFAULT ? " (the board's default)" : "");
        }
    }

    AccCoinDriver drv;
    acccoin_driver_init(&drv);
    SpamDriver spam;
    if (spam_page >= 0) {
        const uint8_t *prg = image + cart_info.data_offset;
        acccoin_spam_init(&spam, prg, (size_t)cart_info.prg_size, spam_page, spam_row, spam_seed, spam_dpad);
    }
    if (input_file && !load_input(input_file)) return 2;
    long frame = 0;
#ifndef CYC_ORACLE
    if ((boot_state_path || watch_state_path) && cyc_is_fds()) {
        hw_entry_stop = true;
        cyc_run_entry_observer = write_boot_state;
        if (watch_state_path) cyc_fds_boot_watch((uint16_t)watch_state_pc);
    }
#endif
#ifndef CYC_ORACLE
    double run_start = wall_seconds(), next_frame = run_start, load_wall = 0;
    long load_frames = 0, unpaced_frames = 0;
#endif
    for (;;) {
        if (acccoin && drv.done) break;
        if ((!acccoin || frames_given) && frame >= frames) break;
        if (barcode && frame==barcode_frame) cyc_scan_barcode(barcode,barcode_speed);
        if (spam_page >= 0) cyc_set_controller(0, acccoin_spam_tick(&spam, cyc_cpu_ram(), stdout));
        else if (acccoin) cyc_set_controller(0, acccoin_driver_tick(&drv, cyc_cpu_ram()));
        else if (input_count) input_tick(frame);
        cyc_trace_file = (trace_f && frame == trace_frame) ? trace_f : NULL;
#ifdef CYC_ORACLE
        cyc_oracle_run_frame();
#else
        if (disk_event_count) disk_tick(frame);
        observe_frame = frame;
        boot_state_frame = frame;
        double frame_start = wall_seconds();
        cyc_run_frame();
        fds_save_frame(frame + 1);
        if (cyc_is_fds() && cyc_fds_hle_loading()) {
            load_frames++;
            unpaced_frames += cyc_host_frame_unpaced();   /* what the window runs unpaced */
            if (!realtime || cyc_host_frame_unpaced()) load_wall += wall_seconds() - frame_start;
        }
        if (realtime) {
            /* The window's pacing: a frame is shown every 1/60.0988 s, except
             * that fast load runs load frames back to back. */
            if (cyc_host_frame_unpaced()) next_frame = wall_seconds();
            else {
                next_frame += FRAME_SECONDS;
                wait_until(next_frame);
                if (cyc_is_fds() && cyc_fds_hle_loading()) load_wall += wall_seconds() - frame_start;
            }
        }
        if (frame_log_f && !frame_log_mesen && frame >= log_first && (log_last < 0 || frame <= log_last))
            write_frame_log(frame_log_f, frame);
#endif
        if (hash_f) write_hash_line(hash_f, frame);
        if (wav_f) {
            int16_t pcm[4096];
            size_t n;
            while ((n = cyc_audio_read(pcm, 4096)) > 0) {
                fwrite(pcm, sizeof(int16_t), n, wav_f);
                wav_samples += (uint32_t)n;
            }
        }
        if (state_out && frame == state_frame) {
            FILE *sf = fopen(state_out, "w");
            if (sf) {
                cyc_hw_state_dump(sf);
                fclose(sf);
            }
        }
        if (screenshot && shot_every > 0 && frame % shot_every == 0) {
            char path[1024];
            numbered_path(path, sizeof path, screenshot, frame);
            if (!cyc_write_png(path, cyc_frame_argb(), 256, 240)) fprintf(stderr, "cannot write %s\n", path);
        }
        if (mem_out && frame == mem_frame) {
            FILE *mf = fopen(mem_out, "w");
            if (mf) {
                cyc_mem_state_dump(mf);
                fclose(mf);
            }
        }
        frame++;
    }
    cyc_trace_file = NULL;
    if (spam_page >= 0)
        printf("spam: %d results, %d passed, %d failed\n", spam.runs, spam.passes, spam.runs - spam.passes);

#ifdef CYC_ORACLE
    printf("mode=oracle frames=%ld cycles=%llu%s\n", frame, (unsigned long long)cyc_cycle_count(),
           acccoin && drv.timed_out ? " TIMEOUT" : "");
    if (getenv("CYC_ORACLE_ADDRESS_REPORT")) cyc_oracle_address_report(stdout);
#else
    uint64_t cycles = cyc_cycle_count();
    printf("mode=%s frames=%ld cycles=%llu native_cycles=%llu (%.1f%%)%s\n",
           cyc_run_native ? "native" : "interp-only", frame, (unsigned long long)cycles,
           (unsigned long long)cyc_run_native_cycles, cycles ? 100.0 * (double)cyc_run_native_cycles / (double)cycles : 0.0,
           acccoin && drv.timed_out ? " TIMEOUT" : "");
    /* Where the cycles went, so a coverage gap can be acted on: ROM cycles
     * become native by seeding them (cycle_seed_file), RAM code by capturing
     * it (cycle_capture_file) when no compiled view of it exists yet. */
    const CycRamViewStats *rv = &cyc_ramview_stats;
    if (cycles && (cyc_ramview_count() || rv->native_cycles)) {
        double pct = 100.0 / (double)cycles;
        uint64_t rom = cyc_run_native_cycles - rv->native_cycles;
        printf("  native: ROM %llu (%.1f%%)  RAM views %llu (%.1f%%)\n", (unsigned long long)rom, (double)rom * pct,
               (unsigned long long)rv->native_cycles, (double)rv->native_cycles * pct);
    }
    if (cyc_run_native && cycles && cyc_run_native_cycles != cycles) {
        double pct = 100.0 / (double)cycles;
        printf("  interpreted: ROM %llu (%.1f%%, add to cycle_seed_file)"
               "  RAM %llu (%.1f%%, CPU RAM code no view covers: --capture-log)",
               (unsigned long long)cyc_run_interp_rom_cycles, (double)cyc_run_interp_rom_cycles * pct,
               (unsigned long long)cyc_run_interp_ram_cycles, (double)cyc_run_interp_ram_cycles * pct);
        if (cyc_run_interp_prg_ram_cycles)
            printf(cyc_is_fds() ? "  PRG RAM %llu (%.1f%%, disk code no view covers: --capture-log)"
                                : "  $6000+ %llu (%.1f%%, cartridge RAM, or ROM below $8000)",
                   (unsigned long long)cyc_run_interp_prg_ram_cycles, (double)cyc_run_interp_prg_ram_cycles * pct);
        if (cyc_run_interp_other_cycles)
            printf("  $2000-$5FFF %llu (%.1f%%)", (unsigned long long)cyc_run_interp_other_cycles,
                   (double)cyc_run_interp_other_cycles * pct);
        printf("\n");
    }
    if (cyc_native_ram_view_count)
        printf("  ram views: %u compiled (%u usable here), %llu entries, %llu validated, %llu rejected, "
               "%llu invalidated by stores, %llu block exits after code stores, %llu RAM instructions interpreted\n",
               cyc_native_ram_view_count, cyc_ramview_count(), (unsigned long long)rv->entries,
               (unsigned long long)rv->validated, (unsigned long long)rv->rejected, (unsigned long long)rv->invalidated,
               (unsigned long long)rv->code_write_exits, (unsigned long long)rv->interp_insns);
    bool disk_ok = fds_save_flush(CYC_FDS_SAVE_EXIT);
    if (cyc_is_fds())
        printf("fds: side %d in the drive at exit, %u disk bytes written, %u disk save%s written%s%s, "
               "%llu FDS events recorded\n", cyc_fds_side(), cyc_fds_disk_writes(), fds_save.saves,
               fds_save.saves == 1 ? "" : "s", fds_save.path ? " to " : " (no --save-file: in memory only)",
               fds_save.path ? fds_save.path : "", (unsigned long long)cyc_ring_total());
    if (cyc_is_fds()) {
        CycFdsBootStatus bs;
        cyc_fds_boot_status(&bs);
        if (bs.entered)
            printf("fds boot: %s, game entry $%04X at frame %u (PPU line %u dot %u)", bs.skipped ? "boot skip" : "BIOS",
                   bs.entry_pc, bs.entry_frame, bs.entry_line, bs.entry_dot);
        else if (bs.auto_inserted || hle_plan.boot_skip)
            printf("fds boot: %s, game not started", bs.skipped ? "boot skip" : "BIOS");
        if (bs.auto_inserted) printf(", side A auto-inserted at the end of frame %u", bs.insert_frame);
        if (bs.entered || bs.auto_inserted || hle_plan.boot_skip) printf("\n");
        CycFdsHleStatus st;
        cyc_fds_hle_status(&st);
        printf("fds hle: %s; %u disk-ID request%s seen, %u auto swap%s, %u disk bump%s, %u load span%s, %ld load "
               "frames (%ld unpaced) (%.2f s at 60 fps) took %.2f s%s; run %.2f s\n", hle_text, st.requests,
               st.requests == 1 ? "" : "s", st.swaps, st.swaps == 1 ? "" : "s", st.bumps, st.bumps == 1 ? "" : "s",
               st.spans, st.spans == 1 ? "" : "s", load_frames, unpaced_frames,
               (double)load_frames * FRAME_SECONDS, load_wall, realtime ? " (paced)" : " (unpaced)",
               wall_seconds() - run_start);
    }
    if (ring_out) {
        FILE *rf = fopen(ring_out, "w");
        if (!rf) { fprintf(stderr, "cannot write %s\n", ring_out); return 2; }
        cyc_ring_dump(rf, (uint32_t)ring_first, ring_last < 0 ? UINT32_MAX : (uint32_t)ring_last);
        fclose(rf);
    }
    if (frame_log_f) fclose(frame_log_f);
    if (cyc_run_miss) write_miss_log(miss_log);
    if (capture_log) {
        long n = cyc_ramview_capture_write(capture_log, cyc_native_program_name);
        if (n < 0) { fprintf(stderr, "cannot write %s\n", capture_log); return 2; }
        printf("capture-log: %llu RAM instructions ran with no view this run; %ld instruction variants -> %s\n",
               (unsigned long long)rv->interp_insns, n, capture_log);
    }
    if (view_list) {
        FILE *vf = fopen(view_list, "w");
        if (!vf) { fprintf(stderr, "cannot write %s\n", view_list); return 2; }
        cyc_ramview_list(vf);
        fclose(vf);
    }
#endif

    if (wav_f) {
        wav_header(wav_f, WAV_RATE, wav_samples);
        fclose(wav_f);
    }
    if (hash_f) fclose(hash_f);
    if (trace_f) fclose(trace_f);
    if (screenshot && !cyc_write_png(screenshot, cyc_frame_argb(), 256, 240))
        fprintf(stderr, "cannot write %s\n", screenshot);

#ifndef CYC_ORACLE
    if (!disk_ok) return 2;
#endif
    if (!save_write(save_file,0) || !save_write(datach_save,1)) return 2;
    if (acccoin) {
        const uint8_t *prg = image + cart_info.data_offset;
        size_t prg_len = (size_t)cart_info.prg_size;
        AccCoinSummary s = acccoin_report(prg, prg_len, cyc_cpu_ram(), stdout, false);
        return s.fail == 0 && s.not_run == 0 ? 0 : 1;
    }
    return 0;
}
