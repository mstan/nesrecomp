/* The console output stage (hw_apu.c audio_emit, cyc_set_console).
 *
 *   - Which model each board gets by default, and that cyc_set_console
 *     overrides it (also while audio is on).
 *   - Each model's frequency response, from sines pushed straight into the
 *     stage (apu_debug_emit), against the transfer function of the
 *     documented RC sections (nes: high-pass 90 Hz, high-pass 440 Hz, low-pass
 *     14 kHz; famicom: high-pass 37 Hz), and the famicom model's -3 dB corner.
 *   - Each model's step response against those sections simulated here.
 *   - The model changes only the audio: the machine's trace and hardware
 *     hashes are identical under both.
 *
 *   cyc_console_audio_test [fds board fixtures dir]
 */
#include "cyc_core.h"
#include "cyc_run.h"
#include "hw_internal.h"
#include "../../common/nes_fds.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

#define RATE 48000
#define PI 3.14159265358979323846

/* A minimal iNES image: 32 KiB PRG of NOPs, reset at $8000 (JMP $8000), 8 KiB CHR ROM. */
static uint8_t cart[16 + 32768 + 8192];
static void make_cart(int mapper)
{
    memset(cart, 0, sizeof(cart));
    memcpy(cart, "NES\x1a", 4);
    cart[4] = 2;
    cart[5] = 1;
    cart[6] = (uint8_t)((mapper & 15) << 4);
    cart[7] = (uint8_t)(mapper & 0xF0);
    uint8_t *prg = cart + 16;
    memset(prg, 0xEA, 32768);
    prg[0] = 0x4C; prg[1] = 0x00; prg[2] = 0x80;
    for (int v = 0x7FFA; v < 0x8000; v += 2) { prg[v] = 0x00; prg[v + 1] = 0x80; }
}

/* ---- the documented sections, simulated independently of hw_apu.c ------ */

typedef struct { int hp; double fc; } Section;
static const Section NES[] = { {1, 90.0}, {1, 440.0}, {0, 14000.0} };
static const Section FAMICOM[] = { {1, 37.0} };

static double coef(Section s)
{
    double dt = 1.0 / RATE, rc = 1.0 / (2 * PI * s.fc);
    return s.hp ? rc / (rc + dt) : dt / (rc + dt);
}

/* |H(e^jw)| of the sections' difference equations: a high-pass is
 * a (1 - z^-1) / (1 - a z^-1), a low-pass a / (1 - (1 - a) z^-1). */
static double model_gain(const Section *s, int n, double f)
{
    double w = 2 * PI * f / RATE, c = cos(w), sn = sin(w), g = 1.0;
    for (int k = 0; k < n; k++) {
        double a = coef(s[k]);
        if (s[k].hp) {
            double num = a * sqrt((1 - c) * (1 - c) + sn * sn);
            double den = sqrt((1 - a * c) * (1 - a * c) + (a * sn) * (a * sn));
            g *= num / den;
        } else {
            double b = 1 - a;
            g *= a / sqrt((1 - b * c) * (1 - b * c) + (b * sn) * (b * sn));
        }
    }
    return g;
}

/* Pushes n levels through the runtime's stage and returns the samples. */
static int16_t out[RATE * 2];
static size_t emit_all(const double *level, size_t n)
{
    size_t got = 0;
    for (size_t i = 0; i < n; i++) {
        apu_debug_emit(level[i]);
        got += apu_audio_read(out + got, RATE * 2 - got);
    }
    return got;
}

static double level_buf[RATE * 2];

static void reset_stage(CycConsole console)
{
    /* Flush the stage's memory with a second of silence. */
    cyc_set_console(console);
    CHECK(cyc_audio_enable(RATE));
    for (int i = 0; i < RATE * 2; i++) level_buf[i] = 0;
    emit_all(level_buf, RATE * 2);
    int16_t drain[64];
    while (apu_audio_read(drain, 64)) {}
}

/* The runtime's gain at f: a 0.5-amplitude sine for 1.5 s, amplitude fitted over the last second. */
static double measured_gain(CycConsole console, double f)
{
    reset_stage(console);
    size_t n = RATE * 3 / 2;
    for (size_t i = 0; i < n; i++) level_buf[i] = 0.5 * sin(2 * PI * f * (double)i / RATE);
    CHECK(emit_all(level_buf, n) == n);
    double re = 0, im = 0;
    for (size_t i = RATE / 2; i < n; i++) {
        re += out[i] * cos(2 * PI * f * (double)i / RATE);
        im += out[i] * sin(2 * PI * f * (double)i / RATE);
    }
    return 2 * sqrt(re * re + im * im) / (double)(n - RATE / 2) / (0.5 * 30000.0);
}

static void test_response(void)
{
    static const double freqs[] = { 20, 37, 60, 90, 200, 440, 1000, 3000, 5000, 10000, 14000, 20000 };
    printf("frequency response (dB): f, nes measured / expected, famicom measured / expected\n");
    for (size_t k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
        double f = freqs[k];
        double mn = measured_gain(CYC_CONSOLE_NES, f), en = model_gain(NES, 3, f);
        double mf = measured_gain(CYC_CONSOLE_FAMICOM, f), ef = model_gain(FAMICOM, 1, f);
        printf("  %6.0f Hz  %7.2f / %7.2f   %7.2f / %7.2f\n", f, 20 * log10(mn), 20 * log10(en),
               20 * log10(mf), 20 * log10(ef));
        /* int16 rounding of a >= -40 dB sine: within 0.05 dB (0.6%). */
        CHECK(fabs(mn / en - 1) < 0.006);
        CHECK(fabs(mf / ef - 1) < 0.006);
    }
    /* The famicom model is 3.01 dB down at 37 Hz, and flat (within 0.1 dB) from 440 Hz up. */
    double g37 = measured_gain(CYC_CONSOLE_FAMICOM, 37.0);
    CHECK(fabs(20 * log10(g37) + 3.01) < 0.05);
    CHECK(20 * log10(measured_gain(CYC_CONSOLE_FAMICOM, 440.0)) > -0.1);
    CHECK(20 * log10(measured_gain(CYC_CONSOLE_FAMICOM, 20000.0)) > -0.1);
    /* The nes model passes less than the famicom model at every frequency. */
    for (size_t k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++)
        CHECK(model_gain(NES, 3, freqs[k]) < model_gain(FAMICOM, 1, freqs[k]));
}

static void test_step(void)
{
    /* A 0.5 step: the runtime's samples against the sections simulated here. */
    for (int m = 0; m < 2; m++) {
        CycConsole console = m ? CYC_CONSOLE_FAMICOM : CYC_CONSOLE_NES;
        const Section *s = m ? FAMICOM : NES;
        int ns = m ? 1 : 3;
        reset_stage(console);
        size_t n = RATE / 10;
        for (size_t i = 0; i < n; i++) level_buf[i] = 0.5;
        CHECK(emit_all(level_buf, n) == n);
        double prev_in[3] = {0}, state[3] = {0};
        int worst = 0;
        for (size_t i = 0; i < n; i++) {
            double v = 0.5;
            for (int k = 0; k < ns; k++) {
                double a = coef(s[k]);
                if (s[k].hp) { double o = a * (state[k] + v - prev_in[k]); prev_in[k] = v; state[k] = o; v = o; }
                else { state[k] += a * (v - state[k]); v = state[k]; }
            }
            int want = (int)(v * 30000.0);
            int d = abs(out[i] - want);
            if (d > worst) worst = d;
        }
        CHECK(worst <= 1);
        if (m) {
            /* First-order high-pass: after one time constant RC = 1/(2 pi 37 Hz) = 4.30 ms,
             * the step has decayed to 1/e. */
            double tau = RATE / (2 * PI * 37.0);
            int i = (int)(tau + 0.5);
            double ratio = (double)out[i] / out[0];
            printf("famicom step: %d at 0, %d after %d samples (%.4f of the start, e^-1 = %.4f)\n",
                   out[0], out[i], i, ratio, exp(-1.0));
            CHECK(fabs(ratio / exp(-(double)i / tau) - 1) < 0.01);
        } else {
            int peak = 0;
            for (int i = 0; i < 20; i++) if (out[i] > peak) peak = out[i];
            printf("nes step: peak %d, %d after 2.3 ms (the 440 Hz section's time constant is 0.36 ms)\n",
                   peak, out[110]);
        }
        printf("  step response within %d count of the documented sections\n", worst);
    }
}

static void synth_reset(CycAudioSynthesis mode)
{
    cyc_audio_enable(0);
    cyc_set_audio_synthesis(mode);
    CHECK(cyc_audio_enable(RATE));
    CHECK(cyc_audio_synthesis() == mode);
    int16_t drain[64];
    while (apu_audio_read(drain, 64)) {}
}

/* A sine at the output rate, fed one value per CPU cycle. Returns its gain
 * through the selected synthesizer and console stage, fitted over the second
 * half so the startup edge and the kernel's lookahead are not sampled. */
static double synth_sine_gain(CycAudioSynthesis mode, double f, double *rms)
{
    reset_stage(CYC_CONSOLE_NES);
    synth_reset(mode);
    const double cpu_hz = cyc_cpu_hz();
    size_t n = (size_t)(0.5 * cpu_hz); /* half a second of CPU cycles */
    size_t got = 0;
    for (size_t i = 0; i < n; i++) {
        double level = 0.5 * sin(2 * PI * f * (double)i / cpu_hz);
        apu_debug_synthesis_level(level);
        got += apu_audio_read(out + got, RATE * 2 - got);
    }
    CHECK(got > RATE / 4);
    double re = 0, im = 0, power = 0;
    size_t from = got / 2;
    for (size_t i = from; i < got; i++) {
        re += out[i] * cos(2 * PI * f * (double)i / RATE);
        im += out[i] * sin(2 * PI * f * (double)i / RATE);
        power += (double)out[i] * out[i];
    }
    *rms = sqrt(power / (double)(got - from));
    return 2 * sqrt(re * re + im * im) / (double)(got - from) / (0.5 * 30000.0);
}

static void test_synthesis(void)
{
    /* Opt-in selection and its reset boundary. */
    synth_reset(CYC_AUDIO_SYNTH_LEGACY);
    synth_reset(CYC_AUDIO_SYNTH_BAND_LIMITED);

    double legacy_1k, legacy_30k, legacy_47k, rms;
    double band_1k, band_30k, band_47k;
    legacy_1k = synth_sine_gain(CYC_AUDIO_SYNTH_LEGACY, 1000, &rms);
    legacy_30k = synth_sine_gain(CYC_AUDIO_SYNTH_LEGACY, 30000, &rms);
    legacy_47k = synth_sine_gain(CYC_AUDIO_SYNTH_LEGACY, 47000, &rms);
    band_1k = synth_sine_gain(CYC_AUDIO_SYNTH_BAND_LIMITED, 1000, &rms);
    band_30k = synth_sine_gain(CYC_AUDIO_SYNTH_BAND_LIMITED, 30000, &rms);
    band_47k = synth_sine_gain(CYC_AUDIO_SYNTH_BAND_LIMITED, 47000, &rms);
    printf("synthesis: 1 kHz legacy %.4f band %.4f; 30 kHz %.6f -> %.6f; 47 kHz %.6f -> %.6f\n",
           legacy_1k, band_1k, legacy_30k, band_30k, legacy_47k, band_47k);

    /* In-band content survives: this is a cleaner reconstruction, not a mute
     * or a different output stage. */
    CHECK(fabs(band_1k / legacy_1k - 1) < 0.05);
    /* Above the output Nyquist the band-limited path rejects what would fold
     * back into the audible band; the box average leaks it. */
    CHECK(band_30k < legacy_30k * 0.02);
    CHECK(band_47k < legacy_47k * 0.02);
}

static void test_defaults(const char *fds_dir)
{
    static const struct { int mapper; CycConsole want; } CASES[] = {
        { 0, CYC_CONSOLE_NES }, { 1, CYC_CONSOLE_NES }, { 4, CYC_CONSOLE_NES }, { 5, CYC_CONSOLE_NES },
        { 69, CYC_CONSOLE_NES }, { 19, CYC_CONSOLE_FAMICOM }, { 24, CYC_CONSOLE_FAMICOM },
        { 26, CYC_CONSOLE_FAMICOM }, { 85, CYC_CONSOLE_FAMICOM },
    };
    for (size_t k = 0; k < sizeof(CASES) / sizeof(CASES[0]); k++) {
        make_cart(CASES[k].mapper);
        CHECK(cyc_load_ines(cart, sizeof(cart)));
        cyc_set_console(CYC_CONSOLE_DEFAULT);
        CHECK(cyc_console() == CASES[k].want);
        cyc_set_console(CYC_CONSOLE_NES);
        CHECK(cyc_console() == CYC_CONSOLE_NES);
        cyc_set_console(CYC_CONSOLE_FAMICOM);
        CHECK(cyc_console() == CYC_CONSOLE_FAMICOM);
        printf("mapper %3d: %s by default\n", CASES[k].mapper, cyc_console_name(CASES[k].want));
    }
    if (fds_dir) {
        char path[1024];
        size_t bios_size, image_size;
        snprintf(path, sizeof(path), "%s/bios/disksys.rom", fds_dir);
        FILE *f = fopen(path, "rb");
        CHECK(f);
        static uint8_t bios[NES_FDS_BIOS_BYTES], image[1 << 20];
        bios_size = fread(bios, 1, sizeof(bios), f);
        fclose(f);
        snprintf(path, sizeof(path), "%s/disk.fds", fds_dir);
        CHECK((f = fopen(path, "rb")) != NULL);
        image_size = fread(image, 1, sizeof(image), f);
        fclose(f);
        CHECK(cyc_load_fds(bios, bios_size, image, image_size, NULL));
        cyc_set_console(CYC_CONSOLE_DEFAULT);
        CHECK(cyc_console() == CYC_CONSOLE_FAMICOM);
        cyc_set_console(CYC_CONSOLE_NES);
        CHECK(cyc_console() == CYC_CONSOLE_NES);
        printf("FDS: famicom by default\n");
    }
    /* A change while audio is on takes effect at once. */
    make_cart(0);
    CHECK(cyc_load_ines(cart, sizeof(cart)));
    reset_stage(CYC_CONSOLE_NES);
    cyc_set_console(CYC_CONSOLE_FAMICOM);
    for (int i = 0; i < 4; i++) level_buf[i] = 0.5;
    CHECK(emit_all(level_buf, 4) == 4);
    CHECK(abs(out[0] - (int)(0.5 * coef(FAMICOM[0]) * 30000.0)) <= 1);
}

static void test_machine_unchanged(void)
{
    /* The same program under both console models and both synthesizers: the
     * traced bus, memories and hardware state match frame for frame; only the
     * samples differ. */
    make_cart(0);
    uint64_t hash[2][2][30];
    for (int m = 0; m < 2; m++) {
        for (int s = 0; s < 2; s++) {
            CHECK(cyc_load_ines(cart, sizeof(cart)));
            cyc_set_console(m ? CYC_CONSOLE_FAMICOM : CYC_CONSOLE_NES);
            cyc_set_audio_synthesis(s ? CYC_AUDIO_SYNTH_BAND_LIMITED : CYC_AUDIO_SYNTH_LEGACY);
            cyc_power_on(0);
            cyc_run_power_on();
            CHECK(cyc_audio_enable(RATE));
            for (int f = 0; f < 30; f++) {
                cyc_run_frame();
                int16_t drain[4096];
                while (cyc_audio_read(drain, 4096)) {}
                hash[m][s][f] = cyc_mem_state_hash() ^ (cyc_hw_state_hash() * 31);
            }
        }
    }
    CHECK(!memcmp(hash[0][0], hash[0][1], sizeof(hash[0][0])));
    CHECK(!memcmp(hash[0][0], hash[1][0], sizeof(hash[0][0])));
    CHECK(!memcmp(hash[0][0], hash[1][1], sizeof(hash[0][0])));
}

static void apu_write_reg(uint16_t addr, uint8_t value)
{
    hw_cycle_start(addr, HW_WRITE);
    hw_write(addr, value);
    hw_cycle_finish(false);
}

static double frame_rms(void)
{
    cyc_run_frame();
    static int16_t pcm[4096];
    size_t n, total = 0;
    double sum = 0;
    while ((n = cyc_audio_read(pcm, 4096)) > 0)
        for (size_t i = 0; i < n; i++, total++) sum += (double)pcm[i] * pcm[i];
    return total ? sqrt(sum / (double)total) : 0;
}

static void test_channel_mute(void)
{
    /* The audio-only channel mute (apu_set_audio_mute): a triangle tone goes silent in the stream while muted and
     * comes back when released, and the machine's hashes are the same as in a run that never muted. */
    make_cart(0);
    uint64_t hash[2][12];
    double rms_on = 0, rms_muted = 0, rms_back = 0;
    for (int run = 0; run < 2; run++) {
        CHECK(cyc_load_ines(cart, sizeof(cart)));
        cyc_set_console(CYC_CONSOLE_NES);
        cyc_power_on(0);
        cyc_run_power_on();
        CHECK(cyc_audio_enable(RATE));
        apu_write_reg(0x4015, 0x04);
        apu_write_reg(0x4008, 0xFF);
        apu_write_reg(0x400A, 0x40);
        apu_write_reg(0x400B, 0xF8);
        for (int f = 0; f < 12; f++) {
            if (run == 0) apu_set_audio_mute(f >= 4 && f < 8 ? 0x04 : 0);
            double r = frame_rms();
            if (run == 0 && f == 3) rms_on = r;
            if (run == 0 && f == 7) rms_muted = r;
            if (run == 0 && f == 11) rms_back = r;
            hash[run][f] = cyc_mem_state_hash() ^ (cyc_hw_state_hash() * 31);
        }
    }
    apu_set_audio_mute(0);
    CHECK(rms_on > 100.0);
    CHECK(rms_muted < rms_on * 0.02);
    CHECK(rms_back > rms_on * 0.5);
    CHECK(!memcmp(hash[0], hash[1], sizeof(hash[0])));
}

int main(int argc, char **argv)
{
    CHECK(cyc_audio_synthesis() == CYC_AUDIO_SYNTH_LEGACY);
    test_defaults(argc > 1 ? argv[1] : NULL);
    test_response();
    test_step();
    test_synthesis();
    test_machine_unchanged();
    test_channel_mute();
    printf("cyc_console_audio_test: %u checks passed\n", checks);
    return 0;
}
