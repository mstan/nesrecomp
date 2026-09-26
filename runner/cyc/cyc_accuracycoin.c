/*
 * cyc_accuracycoin.c - AccuracyCoin automation and result reporting.
 * See cyc_accuracycoin.h.
 */
#include "cyc_accuracycoin.h"

#include <string.h>

#define ACC_TABLE_ADDR      0x8100
#define ACC_RAM_DEBUG_EC    0xEC   /* reaches $0A once the main menu is live */
#define ACC_RAM_RUNNING_ALL 0x35   /* RunningAllTests */

#define BTN_START 0x10

enum {
    DRV_WAIT_MENU,
    DRV_PRESS_START,
    DRV_WAIT_RUN_BEGIN,
    DRV_WAIT_RUN_END,
    DRV_SETTLE,
    DRV_DONE,
};

void acccoin_driver_init(AccCoinDriver *d) {
    memset(d, 0, sizeof(*d));
    d->state = DRV_WAIT_MENU;
}

static void drv_goto(AccCoinDriver *d, int state) {
    d->state = state;
    d->phase_frames = 0;
}

uint8_t acccoin_driver_tick(AccCoinDriver *d, const uint8_t *ram) {
    uint8_t buttons = 0;
    d->phase_frames++;
    switch (d->state) {
    case DRV_WAIT_MENU:
        if (ram[ACC_RAM_DEBUG_EC] >= 0x0A && d->phase_frames > 30)
            drv_goto(d, DRV_PRESS_START);
        else if (d->phase_frames > 60 * 60) {
            d->timed_out = true;
            d->done = true;
            drv_goto(d, DRV_DONE);
        }
        break;
    case DRV_PRESS_START:
        buttons = BTN_START;
        if (d->phase_frames >= 4)
            drv_goto(d, DRV_WAIT_RUN_BEGIN);
        break;
    case DRV_WAIT_RUN_BEGIN:
        if (ram[ACC_RAM_RUNNING_ALL] == 1)
            drv_goto(d, DRV_WAIT_RUN_END);
        else if (d->phase_frames > 120)
            drv_goto(d, DRV_PRESS_START);
        break;
    case DRV_WAIT_RUN_END:
        /* Some tests temporarily clobber zero page (e.g. Implied Dummy Reads),
         * so the flag must stay clear for a full second before we trust it. */
        if (ram[ACC_RAM_RUNNING_ALL] == 0 && ram[ACC_RAM_DEBUG_EC] >= 0x0A) {
            if (++d->quiet_frames >= 60)
                drv_goto(d, DRV_SETTLE);
        } else {
            d->quiet_frames = 0;
        }
        if (d->phase_frames > 60 * 60 * 20) {
            d->timed_out = true;
            drv_goto(d, DRV_SETTLE);
        }
        break;
    case DRV_SETTLE:
        if (d->phase_frames > 120) {
            d->done = true;
            drv_goto(d, DRV_DONE);
        }
        break;
    default:
        break;
    }
    return buttons;
}

/* ------------------------------------------------------------------------- */
/* Menu driver                                                               */
/* ------------------------------------------------------------------------- */

#define ACC_RAM_TAB      0x14   /* menuTabXPos: which page is shown */
#define ACC_RAM_CURSOR   0x16   /* menuCursorYPos: $FF is the page number */
#define ACC_RAM_HEIGHT   0x17   /* menuHeight: tests on this page */

#define BTN_A     0x80
#define BTN_UP    0x08
#define BTN_DOWN  0x04
#define BTN_LEFT  0x02
#define BTN_RIGHT 0x01

enum { SPAM_WAIT_MENU, SPAM_PAGE, SPAM_ENTER, SPAM_ROW, SPAM_MASH };

static uint8_t prg_at(const uint8_t *prg, size_t prg_len, uint16_t addr);
static uint16_t prg_word(const uint8_t *prg, size_t prg_len, uint16_t addr);
static uint16_t read_name(const uint8_t *prg, size_t prg_len, uint16_t addr, char *buf, size_t cap);

/* The result address of one entry of one page, or 0 if there is no such row. */
static uint16_t entry_result(const uint8_t *prg, size_t prg_len, int page, int row) {
    uint16_t p = prg_word(prg, prg_len, (uint16_t)(ACC_TABLE_ADDR + page * 2));
    char name[96];
    p = read_name(prg, prg_len, p, name, sizeof(name));
    for (int i = 0; prg_at(prg, prg_len, p) != 0xFF; i++) {
        p = read_name(prg, prg_len, p, name, sizeof(name));
        uint16_t result = prg_word(prg, prg_len, p);
        p = (uint16_t)(p + 4);
        if (i == row) return result;
    }
    return 0;
}

void acccoin_spam_init(SpamDriver *d, const uint8_t *prg, size_t prg_len, int page, int row, unsigned seed,
                       bool dpad) {
    memset(d, 0, sizeof(*d));
    d->state = SPAM_WAIT_MENU;
    d->page = page;
    d->row = row;
    d->rng = seed ? seed : 1;
    d->dpad = dpad;
    d->result[0] = entry_result(prg, prg_len, page, row);
    d->result[1] = entry_result(prg, prg_len, page, row + 1);
}

static unsigned spam_rand(SpamDriver *d) {
    d->rng ^= d->rng << 13;
    d->rng ^= d->rng >> 17;
    d->rng ^= d->rng << 5;
    return d->rng;
}

/* A press is only seen on the frame the button goes down (the ROM acts on
 * controller_New), so every tap has to be released again. */
static uint8_t tap(SpamDriver *d, uint8_t button) {
    return (spam_rand(d) & 1) ? button : 0;
}

uint8_t acccoin_spam_tick(SpamDriver *d, const uint8_t *ram, FILE *log) {
    d->frame++;
    d->phase_frames++;
    switch (d->state) {
    case SPAM_WAIT_MENU:
        /* The menu is live once the cursor sits on the page number. */
        if (ram[ACC_RAM_DEBUG_EC] >= 0x0A && d->phase_frames > 30) {
            d->state = SPAM_PAGE;
            d->phase_frames = 0;
        }
        return 0;
    case SPAM_PAGE:
        /* Left and right change pages, but only from the page number row. */
        if (ram[ACC_RAM_TAB] == (uint8_t)d->page) {
            d->state = SPAM_ENTER;
            d->phase_frames = 0;
            return 0;
        }
        return tap(d, BTN_RIGHT);
    case SPAM_ENTER:
        /* Down from the page number puts the cursor on row 0. */
        if (ram[ACC_RAM_CURSOR] != 0xFF) {
            d->state = SPAM_ROW;
            d->phase_frames = 0;
            return 0;
        }
        return tap(d, BTN_DOWN);
    case SPAM_ROW:
        if (ram[ACC_RAM_CURSOR] >= (uint8_t)d->row) {
            d->state = SPAM_MASH;
            d->phase_frames = 0;
            if (log)
                fprintf(log, "spam: page %d row %d, results $%03X and $%03X\n", d->page + 1, d->row,
                        d->result[0], d->result[1]);
            return 0;
        }
        return tap(d, BTN_DOWN);
    default:
        break;
    }

    /* Mashing. Report every result the ROM writes, then mash on. */
    for (int i = 0; i < 2; i++) {
        uint8_t r = ram[d->result[i] & 0x7FF];
        uint8_t was = d->last[i];
        d->last[i] = r;
        /* RunTest writes 3 ("....") before the test and the result after it, so
         * a run finished when 3 turns into anything else. */
        if (was != 3 || r == 3 || r == 0) continue;
        d->runs++;
        if (r & 1) d->passes++;
        else if (log)
            fprintf(log, "frame %ld: row %d FAIL error %u ($%03X=%02X) after %d results\n", d->frame,
                    d->row + i, r >> 2, d->result[i], r, d->runs);
    }

    /* A starts the test under the cursor. Down and up move between this row
     * and the next one only, so the cursor never climbs past the target. */
    uint8_t b = tap(d, BTN_A);
    if (d->dpad) b |= tap(d, ram[ACC_RAM_CURSOR] == (uint8_t)d->row ? BTN_DOWN : BTN_UP);
    return b;
}

/* ------------------------------------------------------------------------- */
/* Results                                                                   */
/* ------------------------------------------------------------------------- */

static uint8_t prg_at(const uint8_t *prg, size_t prg_len, uint16_t addr) {
    return prg[(addr - 0x8000) & (prg_len - 1)];
}

static uint16_t prg_word(const uint8_t *prg, size_t prg_len, uint16_t addr) {
    return (uint16_t)(prg_at(prg, prg_len, addr) | (prg_at(prg, prg_len, (uint16_t)(addr + 1)) << 8));
}

/* Copy a $FF-terminated ASCII string; returns the address after the terminator. */
static uint16_t read_name(const uint8_t *prg, size_t prg_len, uint16_t addr, char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        uint8_t c = prg_at(prg, prg_len, addr++);
        if (c == 0xFF) break;
        if (n + 1 < cap) buf[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    buf[n] = 0;
    return addr;
}

static char code_char(uint8_t code) {
    return (char)(code < 10 ? '0' + code : 'A' + (code - 10));
}

AccCoinSummary acccoin_report(const uint8_t *prg, size_t prg_len, const uint8_t *ram,
                              FILE *out, bool verbose) {
    AccCoinSummary s = {0};
    uint16_t first_suite = prg_word(prg, prg_len, ACC_TABLE_ADDR);
    int pages = (first_suite - ACC_TABLE_ADDR) / 2;
    for (int page = 0; page < pages; page++) {
        char name[96];
        uint16_t p = prg_word(prg, prg_len, (uint16_t)(ACC_TABLE_ADDR + page * 2));
        p = read_name(prg, prg_len, p, name, sizeof(name));
        if (verbose) fprintf(out, "Page %d: %s\n", page + 1, name);
        while (prg_at(prg, prg_len, p) != 0xFF) {
            char test[96];
            p = read_name(prg, prg_len, p, test, sizeof(test));
            uint16_t result_addr = prg_word(prg, prg_len, p);
            p = (uint16_t)(p + 4);
            if ((result_addr >> 8) == 3) continue;  /* draw-only test */
            uint8_t r = ram[result_addr & 0x7FF];
            const char *verdict;
            char detail[16] = "";
            s.total++;
            if (r == 0xFF) {
                verdict = "SKIP";
                s.skipped++;
            } else if (r & 1) {
                verdict = "PASS";
                s.pass++;
                if (r >> 2) snprintf(detail, sizeof(detail), " (code %c)", code_char(r >> 2));
            } else if (r & 2) {
                verdict = "FAIL";
                s.fail++;
                snprintf(detail, sizeof(detail), " (error %c)", code_char(r >> 2));
            } else {
                verdict = "----";
                s.not_run++;
            }
            if (verbose || !(r & 1))
                fprintf(out, "  [%s] %-28s $%03X=%02X%s\n", verdict, test, result_addr, r, detail);
        }
    }
    fprintf(out, "AccuracyCoin: %d/%d passed, %d failed, %d skipped, %d not run\n",
            s.pass, s.total, s.fail, s.skipped, s.not_run);
    return s;
}
