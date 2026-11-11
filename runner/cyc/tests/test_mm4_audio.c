/* Runtime-only donor fixture. No copyrighted data belongs in this tree.
 * Compile with CYC_MM4_AUDIO_TEST plus cyc_mm4_{audio,donor}.c.
 * test_mm4_audio ROM trace HEX_SFX FRAMES
 * test_mm4_audio ROM wav HEX_SFX SECONDS OUT.wav
 * test_mm4_audio ROM state
 * trace/wav retain the final MM2 standalone check's output for exact A/B. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cyc_mm4_audio.h"
#include "cyc_mm4_donor.h"

static unsigned char muted;
void cyc_mod_set_apu_audio_mute(unsigned char mask) { muted = mask; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int continuation(void) {
    uint8_t state[8192], final[8192], actual[8192], bad[8192];
    int16_t expected[800], output[800];
    const int sounds[] = {0x21, 0x22, 0x27, 0x3D};
    for (unsigned s = 0; s < sizeof sounds / sizeof *sounds; ++s) {
        for (int ownership = 0; ownership <= 1; ++ownership) {
            for (int prefix = 0; prefix < 90; ++prefix) {
                cyc_mm4_audio_configure(1, ownership); cyc_mm4_audio_reset();
                cyc_mm4_audio_play(sounds[s]);
                for (int i = 0; i < prefix; ++i) {
                    memset(output, 0, sizeof output);
                    cyc_mm4_audio_frame_end(); cyc_mm4_audio_mix(output, 800);
                }
                int size = cyc_mm4_audio_state_get(state, sizeof state);
                CHECK(size > 0 && size <= (int)sizeof state);
                CHECK(cyc_mm4_audio_state_valid(state, size));
                CHECK(!cyc_mm4_audio_state_valid(state, size - 1));
                memcpy(bad, state, size); bad[0] ^= 1;
                CHECK(!cyc_mm4_audio_state_set(bad, size));
                CHECK(cyc_mm4_audio_state_get(actual, sizeof actual) == size);
                CHECK(!memcmp(state, actual, size));
                memset(expected, 0, sizeof expected);
                cyc_mm4_audio_frame_end(); cyc_mm4_audio_mix(expected, 800);
                unsigned char mask = muted, owned = cyc_mm4_audio_owned_channels();
                CHECK(cyc_mm4_audio_state_get(final, sizeof final) == size);
                CHECK(cyc_mm4_audio_state_set(state, size));
                memset(output, 0, sizeof output);
                cyc_mm4_audio_frame_end(); cyc_mm4_audio_mix(output, 800);
                CHECK(!memcmp(expected, output, sizeof output));
                CHECK(muted == mask && cyc_mm4_audio_owned_channels() == owned);
                CHECK(!ownership ? muted == 0 : muted == owned);
                CHECK(cyc_mm4_audio_state_get(actual, sizeof actual) == size);
                CHECK(!memcmp(final, actual, size));
            }
        }
    }
    /* Queued effects survive snapshots; an opt-out rejects the enabled profile. */
    cyc_mm4_audio_configure(1, 1); cyc_mm4_audio_reset();
    cyc_mm4_audio_play(0x22);
    int n = cyc_mm4_audio_state_get(state, sizeof state);
    cyc_mm4_audio_configure(0, 1);
    CHECK(!cyc_mm4_audio_state_set(state, n) && muted == 0);
    cyc_mm4_audio_configure(1, 1);
    CHECK(cyc_mm4_audio_state_set(state, n));
    puts("720 queued, staged, active and tail continuations passed; invalid/profile snapshots refused");
    return 0;
}

int main(int argc, char **argv) {
    char why[200] = {0};
    if (argc < 3) { fputs("usage: test_mm4_audio ROM state|reject|trace|wav ...\n", stderr); return 2; }
    int expected = (!strcmp(argv[2], "state") || !strcmp(argv[2], "inactive")) ? 3 : !strcmp(argv[2], "reject") ? 4 :
        !strcmp(argv[2], "trace") ? 5 : !strcmp(argv[2], "wav") ? 6 : 0;
    if (!expected || argc != expected) { fputs("invalid mode or argument count\n", stderr); return 2; }
    if (!cyc_mm4_donor_load(argv[1], why, sizeof why)) {
        fprintf(stderr, "%s\n", why); return 2;
    }
    const uint8_t *resident = cyc_mm4_donor.prg;
    CHECK(cyc_mm4_donor_load(argv[1], why, sizeof why) && cyc_mm4_donor.prg == resident);
    CHECK(!cyc_mm4_donor_load("", why, sizeof why) && cyc_mm4_donor.prg == resident);
    if (!strcmp(argv[2], "inactive")) {
        uint8_t snapshot[8192], after[8192]; int16_t samples[800] = {0};
        cyc_mm4_audio_configure(1, 1); cyc_mm4_audio_reset(); cyc_mm4_audio_play(0x22);
        for (int i = 0; i < 3; i++) { cyc_mm4_audio_frame_end(); cyc_mm4_audio_mix(samples, 800); }
        cyc_mm4_audio_configure(0, 1); cyc_mm4_audio_reset();
        int size = cyc_mm4_audio_state_get(snapshot, sizeof snapshot);
        cyc_mm4_donor_unload();
        CHECK(size > 0 && cyc_mm4_audio_state_valid(snapshot, size) && cyc_mm4_audio_state_set(snapshot, size));
        cyc_mm4_audio_frame_end();
        CHECK(muted == 0 && !cyc_mm4_audio_owned_channels());
        CHECK(cyc_mm4_audio_state_get(after, sizeof after) == size && !memcmp(snapshot, after, size));
        puts("Disabled current-build audio snapshot needs no donor and stays silent"); return 0;
    }
    if (!strcmp(argv[2], "reject")) {
        CHECK(argc == 4 && !cyc_mm4_donor_load(argv[3], why, sizeof why));
        CHECK(cyc_mm4_donor.prg == resident && why[0]);
        puts("Changed/missing source refused with verified resident image intact");
        return 0;
    }
    if (!strcmp(argv[2], "state")) return continuation();
    if (argc < 5) return 2;
    int sfx = (int)strtol(argv[3], NULL, 16);
    if (!strcmp(argv[2], "trace")) { cyc_mm4_audio_selftest_dump(stdout, sfx, atoi(argv[4])); return 0; }
    if (strcmp(argv[2], "wav") || argc < 6) return 2;
    size_t n = (size_t)(atof(argv[4]) * 48000);
    int16_t *pcm = (int16_t *)calloc(n, sizeof *pcm);
    if (!pcm) return 2;
    cyc_mm4_audio_selftest_render(sfx, pcm, n);
    FILE *f = fopen(argv[5], "wb");
    if (!f) { free(pcm); return 2; }
    uint32_t bytes = (uint32_t)(n * 2), rate = 48000, br = rate * 2, fmt = 16;
    uint16_t pcmfmt = 1, ch = 1, al = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); uint32_t sz = 36 + bytes; fwrite(&sz, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(pcm, 2, n, f);
    fclose(f); free(pcm); return 0;
}
