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
 *     --screenshot FILE    save the last frame as PNG
 *     --shot-every N       also save every Nth frame, as FILE with the frame
 *                          number before its extension (shot.png -> shot_00120.png)
 *     --wav-out FILE       record the audio output (48 kHz mono)
 *
 * Without CYC_WITH_SDL the host is always headless.
 */
#include "cyc_accuracycoin.h"
#include "cyc_core.h"
#include "../../common/nes_cart.h"
#include "cyc_png.h"
#include "cyc_trace.h"

#ifndef CYC_ORACLE
#include "cyc_recomp.h"
#include "cyc_run.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        /* RAM instruction starts, as comments: the recompiler skips them, since
         * code the program writes at run time is not in the ROM image to
         * compile. They are here to explain a coverage gap seeds cannot close. */
        if (cyc_run_ram_miss) {
            unsigned ram_addrs = 0;
            for (int a = 0; a < 0x2000; a++) ram_addrs += cyc_run_ram_miss[a] != 0;
            if (ram_addrs) {
                unsigned stable = 0, varied = 0;
                fprintf(mf, "#\n# %u RAM addresses also started an instruction this run (not seedable).\n"
                            "# 'opcodes' is how many distinct opcode bytes ever began an instruction\n"
                            "# there; 1 means only operands change at that address.\n"
                            "# address count opcodes\n", ram_addrs);
                for (int a = 0; a < 0x2000; a++) {
                    if (!cyc_run_ram_miss[a]) continue;
                    unsigned ops = 0;
                    if (cyc_run_ram_opcodes) {
                        const uint8_t *set = cyc_run_ram_opcodes + (a & 0x7FF) * 32;
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
    printf("input: %d steps from %s\n", input_count, path);
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

int main(int argc, char **argv) {
    const char *rom_path = NULL, *hash_out = NULL, *trace_out = NULL, *screenshot = NULL, *state_out = NULL,
               *wav_out = NULL, *mem_out = NULL;
    const char *save_file=NULL;
    long mem_frame = -1, shot_every = 0;
#ifndef CYC_ORACLE
    const char *miss_log = NULL;
#endif
    int align = 0, scale = 3;
    long frames = 600, trace_frame = -1, state_frame = -1;
    int spam_page = -1, spam_row = 0;
    const char *input_file = NULL;
    unsigned spam_seed = 1;
    bool spam_dpad = true;
    bool acccoin = false, headless = false, frames_given = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spam") && i + 2 < argc) {
            spam_page = atoi(argv[++i]);
            spam_row = atoi(argv[++i]);
            headless = true;
        }
        else if (!strcmp(argv[i], "--spam-seed") && i + 1 < argc) spam_seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--spam-no-dpad")) spam_dpad = false;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_file = argv[++i], headless = true;
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
        else if (!strcmp(argv[i], "--mem-frame") && i + 1 < argc) mem_frame = atol(argv[++i]), headless = true;
        else if (!strcmp(argv[i], "--mem-out") && i + 1 < argc) mem_out = argv[++i], headless = true;
#ifndef CYC_ORACLE
        else if (!strcmp(argv[i], "--interp-only")) cyc_run_native = false;
        else if (!strcmp(argv[i], "--miss-log") && i + 1 < argc) miss_log = argv[++i], headless = true;
#endif
        else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s <rom.nes> [--interp-only] [--align N] [--scale N]\n"
                        "       [--save-file FILE] (raw battery RAM/EEPROM, loaded and atomically saved)\n"
                        "       headless: [--frames N] [--acccoin] [--hash-out FILE]\n"
                        "       [--spam PAGE ROW] [--spam-seed N] [--spam-no-dpad] [--input FILE]\n"
                        "       [--trace-frame N --trace-out FILE] [--state-frame N --state-out FILE]\n"
                        "       [--miss-log FILE] [--screenshot FILE [--shot-every N]] [--wav-out FILE]\n",
                argv[0]);
        return 2;
    }
    size_t size;
    uint8_t *image = read_file(rom_path, &size);
    NesCartInfo cart_info;
    if (!image || !nes_cart_image(image, size, &cart_info) || !cyc_load_ines(image, size)) {
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
    if (!save_load(save_file,0)) return 2;
    cyc_power_on((uint8_t)align);
#ifndef CYC_ORACLE
    cyc_run_power_on();
#endif

#if defined(CYC_WITH_SDL) && !defined(CYC_ORACLE)
    if (!headless) {
        int result=cyc_sdl_main(cyc_native_program_name ? cyc_native_program_name : rom_path, scale);
        return save_write(save_file,0)?result:2;
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
    if (miss_log) {
        cyc_run_miss = (uint32_t *)calloc(cyc_run_miss_slots(), sizeof(uint32_t));
        cyc_run_ram_miss = (uint32_t *)calloc(0x2000, sizeof(uint32_t));
        cyc_run_ram_opcodes = (uint8_t *)calloc(0x800 * 32, 1);
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
    for (;;) {
        if (acccoin && drv.done) break;
        if ((!acccoin || frames_given) && frame >= frames) break;
        if (spam_page >= 0) cyc_set_controller(0, acccoin_spam_tick(&spam, cyc_cpu_ram(), stdout));
        else if (acccoin) cyc_set_controller(0, acccoin_driver_tick(&drv, cyc_cpu_ram()));
        else if (input_count) input_tick(frame);
        cyc_trace_file = (trace_f && frame == trace_frame) ? trace_f : NULL;
#ifdef CYC_ORACLE
        cyc_oracle_run_frame();
#else
        cyc_run_frame();
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
    /* Where the rest of the cycles went, so a coverage gap can be acted on:
     * ROM cycles become native by seeding them, RAM cycles never can. */
    if (cyc_run_native && cycles && cyc_run_native_cycles != cycles) {
        double pct = 100.0 / (double)cycles;
        printf("  interpreted: ROM %llu (%.1f%%, add to cycle_seed_file)"
               "  RAM %llu (%.1f%%, written at run time, not compilable)",
               (unsigned long long)cyc_run_interp_rom_cycles, (double)cyc_run_interp_rom_cycles * pct,
               (unsigned long long)cyc_run_interp_ram_cycles, (double)cyc_run_interp_ram_cycles * pct);
        if (cyc_run_interp_other_cycles)
            printf("  $2000-$7FFF %llu (%.1f%%)", (unsigned long long)cyc_run_interp_other_cycles,
                   (double)cyc_run_interp_other_cycles * pct);
        printf("\n");
    }
    if (cyc_run_miss) write_miss_log(miss_log);
#endif

    if (wav_f) {
        wav_header(wav_f, WAV_RATE, wav_samples);
        fclose(wav_f);
    }
    if (hash_f) fclose(hash_f);
    if (trace_f) fclose(trace_f);
    if (screenshot && !cyc_write_png(screenshot, cyc_frame_argb(), 256, 240))
        fprintf(stderr, "cannot write %s\n", screenshot);

    if (!save_write(save_file,0)) return 2;
    if (acccoin) {
        const uint8_t *prg = image + cart_info.data_offset;
        size_t prg_len = (size_t)cart_info.prg_size;
        AccCoinSummary s = acccoin_report(prg, prg_len, cyc_cpu_ram(), stdout, false);
        return s.fail == 0 && s.not_run == 0 ? 0 : 1;
    }
    return 0;
}
