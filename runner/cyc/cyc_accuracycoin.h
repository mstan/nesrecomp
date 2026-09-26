/*
 * cyc_accuracycoin.h - AccuracyCoin automation and result reporting.
 *
 * AccuracyCoin (https://github.com/100thCoin/AccuracyCoin) keeps a table of
 * test pages at $8100. Each page lists tests as: name, $FF, result address
 * (word), test entry (word). A result byte has PASS in bit 0, FAIL in bit 1,
 * and an error/success code in bits 2-7. Draw-only tests store results on
 * page 3 and are not scored.
 *
 * The driver presses Start on the main menu ("run every test"), watches the
 * RunningAllTests flag ($35) rise and fall, then reports.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  state;            /* internal driver phase */
    int  phase_frames;     /* frames spent in the current phase */
    int  quiet_frames;     /* consecutive frames with RunningAllTests clear */
    bool done;
    bool timed_out;
} AccCoinDriver;

void acccoin_driver_init(AccCoinDriver *d);
/* Call once per frame before the frame runs; returns the controller 1 byte
 * (A B Select Start Up Down Left Right, MSB first as TriCNES latches it). */
uint8_t acccoin_driver_tick(AccCoinDriver *d, const uint8_t *ram);

/*
 * Menu driver: walks to a page, puts the cursor on a row, then plays that test
 * over and over the way a player does - mashing A while tapping Down and Up
 * between this row and the one below it. The cursor never leaves those two
 * rows. Used to shake out controller-port behaviour that only shows up when
 * buttons move while a test is reading the port.
 */
typedef struct {
    int      state, phase_frames;
    int      page, row;          /* target page (0 based) and top row */
    unsigned rng;
    uint16_t result[2];          /* result addresses of row and row + 1 */
    uint8_t  last[2];            /* last seen result bytes */
    int      runs, passes;       /* tests seen finishing, and how many passed */
    long     frame;
    bool     dpad;               /* tap down/up between the two rows as well */
} SpamDriver;

void acccoin_spam_init(SpamDriver *d, const uint8_t *prg, size_t prg_len, int page, int row, unsigned seed,
                       bool dpad);
/* Call once per frame before the frame runs. Reports result changes to log. */
uint8_t acccoin_spam_tick(SpamDriver *d, const uint8_t *ram, FILE *log);

typedef struct {
    int pass, fail, skipped, not_run, total;
} AccCoinSummary;

/* Print one line per test and a summary. prg is the PRG ROM image. */
AccCoinSummary acccoin_report(const uint8_t *prg, size_t prg_len, const uint8_t *ram,
                              FILE *out, bool verbose);

#ifdef __cplusplus
}
#endif
