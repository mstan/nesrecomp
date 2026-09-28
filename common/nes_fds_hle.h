/* nes_fds_hle.h - the Famicom Disk System HLE tier's decisions, shared by the
 * compiler (which checks game.toml [fds] hle) and the cycle runtime.
 *
 * LLE is the baseline: the real BIOS runs recompiled against the modelled RAM
 * Adapter and drive, and that machine is the reference the oracle checks. The
 * HLE tier is opt-in conveniences on top of it, modelled on psxrecomp's
 * psx_bios_hle_plan() (runtime/include/bios_hle_plan.h): ONE pure function
 * decides every axis from what was asked for and what the loaded BIOS and disk
 * image can support, and every call site uses its answer. Off by default; with
 * every axis off the machine runs exactly as without the tier.
 *
 * Axes (independent: refusing one never changes the other):
 *
 *   auto_swap  when the program asks for a disk side that is not in the drive,
 *              eject and insert the side it asked for, with the drive lines a
 *              player's swap produces (runner/cyc/hw_fds_hle.c). Needs the
 *              BIOS's disk-ID check anchor (the entry of the routine every
 *              ID-checking BIOS call runs, and where the caller's disk ID
 *              pointer is): the request is the ID the program passes, matched
 *              against every side's disk header. A BIOS with no known anchor,
 *              a one-sided image, or a side without a readable header makes the
 *              axis structurally unavailable: asking for it is refused and
 *              reported, never forced.
 *
 *   fast_load  while the drive is loading, the host runs the machine as fast as
 *              it can instead of at 60 frames per second. The machine is the
 *              same LLE machine, frame for frame: only presentation and pacing
 *              change, so everything the program can see is what LLE produces.
 *              Needs only the drive; with an anchor a load also covers the
 *              BIOS's wait before it starts the motor.
 *
 *   boot_skip  power on straight into the game: the BIOS's logo, disk load and
 *              license screen do not run, the boot files are loaded as the
 *              BIOS's LoadFiles would load them and the machine is left as the
 *              BIOS leaves it when it jumps to the game's reset vector
 *              (runner/cyc/cyc_fds_skip.c, common/nes_fds_boot.h). Needs the
 *              BIOS's boot model (its boot LoadFiles call, license loop and
 *              jump into the game, verified against its code) and a boot the
 *              model reproduces exactly on this image, with the boot side in
 *              the drive at power-on; anything else is refused with the reason
 *              and the BIOS boots the disk itself. Decided once, at power-on.
 *
 *   auto_insert  with the drive empty at power-on, put disk 1 side A in when
 *              the BIOS's boot starts waiting for a disk (PLEASE SET DISK
 *              CARD: it polls $4032 with no disk), through the drive's normal
 *              insert, at a frame boundary: the timed action of a player
 *              (runner/cyc/hw_fds_boot.c). It is LLE, not HLE: on by default,
 *              and "off" / "all" leave it alone. A host disk change or the
 *              game starting ends it. Needs the BIOS's jump into the game (the
 *              end of the boot) to be known.
 *
 * Requests come from four sources; a later one overrides an earlier one per
 * axis: game.toml [fds] hle (compiled into the program), the environment
 * (NESRECOMP_FDS_HLE), the command line (--fds-hle) and a live toggle (the SDL
 * host's keys; boot_skip only acts at power-on). Each is a list of words:
 * off / none, on / all (auto_swap, fast_load and boot_skip together),
 * auto-swap, fast-load, boot-skip, auto-insert, and each with no- in front,
 * separated by commas, spaces or +.
 *
 * Header-only C11, no runtime state: the whole decision matrix is unit-tested
 * directly (runner/cyc/fds_hle_plan_test.c). */
#ifndef NES_FDS_HLE_H
#define NES_FDS_HLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One source's request: -1 says nothing about the axis, 0 off, 1 on. */
typedef struct {
    int8_t auto_swap, fast_load, boot_skip, auto_insert;
} NesFdsHleAsk;

#define NES_FDS_HLE_ASK_NONE ((NesFdsHleAsk){ -1, -1, -1, -1 })

static inline bool nes_fds_hle_word(const char *s, size_t n, const char *w)
{
    size_t i = 0;
    for (; i < n && w[i]; ++i) {
        char c = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] + 32) : s[i];
        if (c == '_') c = '-';
        if (c != w[i]) return false;
    }
    return i == n && !w[i];
}

/* Parse a request list into *ask (starting from "says nothing"). False on an
 * unknown word; *bad (if given) then points at it. NULL or "" says nothing. */
static inline bool nes_fds_hle_parse(const char *s, NesFdsHleAsk *ask, const char **bad)
{
    *ask = NES_FDS_HLE_ASK_NONE;
    if (bad) *bad = NULL;
    if (!s) return true;
    while (*s) {
        while (*s == ',' || *s == ' ' || *s == '+' || *s == '\t') ++s;
        size_t n = 0;
        while (s[n] && s[n] != ',' && s[n] != ' ' && s[n] != '+' && s[n] != '\t') ++n;
        if (!n) break;
        if (nes_fds_hle_word(s, n, "off") || nes_fds_hle_word(s, n, "none"))
            ask->auto_swap = ask->fast_load = ask->boot_skip = 0;
        else if (nes_fds_hle_word(s, n, "on") || nes_fds_hle_word(s, n, "all"))
            ask->auto_swap = ask->fast_load = ask->boot_skip = 1;
        else if (nes_fds_hle_word(s, n, "auto-swap")) ask->auto_swap = 1;
        else if (nes_fds_hle_word(s, n, "no-auto-swap")) ask->auto_swap = 0;
        else if (nes_fds_hle_word(s, n, "fast-load")) ask->fast_load = 1;
        else if (nes_fds_hle_word(s, n, "no-fast-load")) ask->fast_load = 0;
        else if (nes_fds_hle_word(s, n, "boot-skip")) ask->boot_skip = 1;
        else if (nes_fds_hle_word(s, n, "no-boot-skip")) ask->boot_skip = 0;
        else if (nes_fds_hle_word(s, n, "auto-insert")) ask->auto_insert = 1;
        else if (nes_fds_hle_word(s, n, "no-auto-insert")) ask->auto_insert = 0;
        else {
            if (bad) *bad = s;
            return false;
        }
        s += n;
    }
    return true;
}

/* ---- per-BIOS capability facts ----
 *
 * The disk-ID check anchor of a BIOS image: the entry of the routine that
 * compares a disk's header with the ID the caller asked for, and the zero-page
 * address holding the pointer to that 10-byte ID when it runs. For the RAM
 * Adapter BIOS (disksys.rom, CRC32 5E607DCF) that is $E445 (JSR $E6E3: start the
 * drive and read block 1's "*NINTENDO-HVC*", then compare its next 10 bytes with
 * ($00),Y, $FF matching anything; $E451-$E461). Its five callers are every
 * ID-checking call: LoadFiles ($E1F8 -> $E21A), AppendFile / WriteFile
 * ($E237/$E239 -> $E26B, $E290, $E2AB), CheckFileCount / AdjustFileCount
 * ($E2B7/$E2BB -> $E2F7) and SetFileCount ($E301/$E305 -> $E2AB), each after
 * $E3E7/$E3EA stored the ID pointer from the caller's inline arguments at
 * $00/$01. GetDiskInfo ($E32A -> $E346) reads the header without an ID and so
 * never passes it. It is the address Mesen's own auto-insert watches (FDS.cpp
 * ReadRAM). check[] is the code expected there, verified at load.
 * Another BIOS declares its anchor in its identity file (<bios>.toml:
 * hle_id_check / hle_id_pointer), as the synthetic test BIOSes do. */
typedef struct {
    uint16_t id_check;          /* 0: no anchor */
    uint8_t  id_pointer;
    uint8_t  check_len;
    uint8_t  check[3];
} NesFdsHleAnchor;

static inline NesFdsHleAnchor nes_fds_hle_builtin_anchor(uint32_t bios_crc32)
{
    NesFdsHleAnchor a = { 0, 0, 0, { 0, 0, 0 } };
    if (bios_crc32 == 0x5E607DCFu) {
        a.id_check = 0xE445;
        a.id_pointer = 0x00;
        a.check_len = 3;
        a.check[0] = 0x20; a.check[1] = 0xE3; a.check[2] = 0xE6;   /* JSR $E6E3 */
    }
    return a;
}

/* ---- the disk ID a program asks for ----
 *
 * A disk header (block 1, 56 bytes: code $01, "*NINTENDO-HVC*", then maker,
 * game name x3, game type, revision, side, disk number, disk type, and one more
 * byte) matches a requested ID when each of the 10 ID bytes is $FF or equals
 * header byte 15 + i: the BIOS's comparison ($E44E-$E471). */
#define NES_FDS_HLE_ID_BYTES 10u

static inline bool nes_fds_hle_id_matches(const uint8_t id[NES_FDS_HLE_ID_BYTES], const uint8_t block1[56])
{
    for (unsigned i = 0; i < NES_FDS_HLE_ID_BYTES; ++i)
        if (id[i] != 0xFF && id[i] != block1[15 + i]) return false;
    return true;
}

/* Block 1 as the drive would read it from a side's stream: the first block
 * after the lead-in (zeros, then the $80 mark). False if there is none. */
static inline bool nes_fds_hle_block1(const uint8_t *stream, uint32_t len, uint8_t out[56])
{
    uint32_t p = 0;
    while (p < len && stream[p] == 0) ++p;
    if (p >= len || stream[p] != 0x80 || p + 1 + 56 > len || stream[p + 1] != 0x01) return false;
    memcpy(out, stream + p + 1, 56);
    return true;
}

/* ---- the plan ---- */
typedef struct {
    NesFdsHleAsk config, env, cli, live;   /* game.toml, NESRECOMP_FDS_HLE, --fds-hle, toggle */
    bool     is_fds;                       /* the program runs the RAM Adapter */
    bool     have_anchor;                  /* the BIOS's disk-ID check anchor is known and verified */
    unsigned sides;                        /* sides in the image */
    unsigned sides_with_id;                /* sides whose disk header (block 1) reads */
    bool     have_boot;                    /* the BIOS's boot model is known and verified */
    bool     have_jump;                    /* the BIOS's jump into the game is known and verified */
    /* Why this image's boot is outside what the skip reproduces (the runtime's
     * boot analysis: nes_fds_boot_read, the license check, the boot side in
     * the drive at power-on); NULL: reproducible, or not analysed because the
     * skip was not asked for. */
    const char *boot_why;
} NesFdsHleRequest;

typedef struct {
    bool auto_swap, fast_load, boot_skip, auto_insert;
    /* Asked for and refused, and why: a silent downgrade is how psxrecomp's
     * boot-skip bug hid; hosts print these. */
    bool auto_swap_denied, fast_load_denied, boot_skip_denied, auto_insert_denied;
    const char *auto_swap_why, *fast_load_why, *boot_skip_why, *auto_insert_why;
    /* Which source decided each axis: "default", "game.toml", "env", "cli", "toggle". */
    const char *auto_swap_from, *fast_load_from, *boot_skip_from, *auto_insert_from;
    /* The always-on disk-ID request observation (ring events) runs: it needs
     * only the anchor, whatever was asked for. */
    bool observe;
} NesFdsHlePlan;

static inline int8_t nes_fds_hle_pick_or(int8_t fallback, int8_t config, int8_t env, int8_t cli, int8_t live,
                                         const char **from)
{
    *from = "default";
    int8_t v = fallback;
    if (config >= 0) { v = config; *from = "game.toml"; }
    if (env >= 0)    { v = env;    *from = "env"; }
    if (cli >= 0)    { v = cli;    *from = "cli"; }
    if (live >= 0)   { v = live;   *from = "toggle"; }
    return v;
}

static inline int8_t nes_fds_hle_pick(int8_t config, int8_t env, int8_t cli, int8_t live, const char **from)
{
    *from = "default";
    int8_t v = 0;
    if (config >= 0) { v = config; *from = "game.toml"; }
    if (env >= 0)    { v = env;    *from = "env"; }
    if (cli >= 0)    { v = cli;    *from = "cli"; }
    if (live >= 0)   { v = live;   *from = "toggle"; }
    return v;
}

/* Pure: the same request always gives the same plan. */
static inline NesFdsHlePlan nes_fds_hle_plan(NesFdsHleRequest r)
{
    NesFdsHlePlan p;
    memset(&p, 0, sizeof(p));
    int8_t want_swap = nes_fds_hle_pick(r.config.auto_swap, r.env.auto_swap, r.cli.auto_swap, r.live.auto_swap,
                                        &p.auto_swap_from);
    int8_t want_fast = nes_fds_hle_pick(r.config.fast_load, r.env.fast_load, r.cli.fast_load, r.live.fast_load,
                                        &p.fast_load_from);
    p.observe = r.is_fds && r.have_anchor;

    /* Axis 1: auto swap. Structurally unavailable unless the request can be
     * read (anchor) and answered (two or more sides, each with a header). */
    if (want_swap > 0) {
        if (!r.is_fds) p.auto_swap_why = "not a Famicom Disk System program";
        else if (!r.have_anchor) p.auto_swap_why = "no disk-ID check anchor is known for this BIOS";
        else if (r.sides < 2) p.auto_swap_why = "the image has one side";
        else if (r.sides_with_id < r.sides) p.auto_swap_why = "a side has no readable disk header";
        if (p.auto_swap_why) p.auto_swap_denied = true;
        else p.auto_swap = true;
    }

    /* Axis 2: fast load. Decided from its own request only, never from axis 1's
     * outcome: it needs nothing but the drive. */
    if (want_fast > 0) {
        if (!r.is_fds) {
            p.fast_load_why = "not a Famicom Disk System program";
            p.fast_load_denied = true;
        } else {
            p.fast_load = true;
        }
    }

    /* Axis 3: boot skip. Structurally unavailable unless the BIOS's boot is
     * modelled and this image's boot is one the model reproduces. */
    int8_t want_skip = nes_fds_hle_pick(r.config.boot_skip, r.env.boot_skip, r.cli.boot_skip, r.live.boot_skip,
                                        &p.boot_skip_from);
    if (want_skip > 0) {
        if (!r.is_fds) p.boot_skip_why = "not a Famicom Disk System program";
        else if (!r.have_boot) p.boot_skip_why = "no boot model is known for this BIOS";
        else if (r.boot_why) p.boot_skip_why = r.boot_why;
        if (p.boot_skip_why) p.boot_skip_denied = true;
        else p.boot_skip = true;
    }

    /* Axis 4: auto insert. LLE (a timed player action), so on by default; it
     * needs to know where the boot ends. The default asks quietly: only an
     * explicit request is reported as refused. */
    int8_t want_insert = nes_fds_hle_pick_or(1, r.config.auto_insert, r.env.auto_insert, r.cli.auto_insert,
                                             r.live.auto_insert, &p.auto_insert_from);
    if (want_insert > 0) {
        const char *why = !r.is_fds ? "not a Famicom Disk System program"
                        : !r.have_jump ? "the BIOS's jump into the game is not known" : NULL;
        if (!why) p.auto_insert = true;
        else if (strcmp(p.auto_insert_from, "default")) {
            p.auto_insert_denied = true;
            p.auto_insert_why = why;
        }
    }
    return p;
}

/* ---- the BIOS's boot model (the boot skip, auto insert) ----
 *
 * Where the known BIOS boots, verified against its code at load. For
 * disksys.rom (CRC32 5E607DCF), from its reset path ($EE24):
 *   $EF59 JSR $E1F8 (LoadFiles) + inline $EFF5/$EFF5: the boot load, with
 *         the disk ID FF FF FF FF FF FF 00 00 FF FF and the list $FF (every
 *         file with ID <= the boot code). Up to that call the boot depends on
 *         nothing on the disk (measured: the whole machine at that call is
 *         identical for every owner image), so the skip starts there.
 *   $EF65 BEQ $EFAF after the license check ($F431: the disk's KYODAKU- file
 *         at PPU $2800 against the BIOS's copy at $ED37, 224 bytes).
 *   $EFAF LDA #$20 / STA $A2, $EFB3: the license screen loop, one frame per
 *         pass (JSR $E1B2 waits for the NMI), until the timer $A2, counted
 *         down by $E9D3 every tenth frame (divider $80), reaches zero; each
 *         pass moves the scroll $FC by 2 below $B0. The skip runs its RAM
 *         effects forward and lets the BIOS run the last pass itself.
 *   $E553 STA $2001 in LoadFiles's PPU-file path: rendering off, mid-frame.
 *   $EE9F JMP ($DFFC): into the game.
 * Another BIOS declares its model in its identity file (<bios>.toml:
 * hle_boot_load, hle_boot_load_entry, hle_boot_jump; no license loop). */
typedef struct {
    uint16_t load_call;         /* JSR LoadFiles with two inline pointers: disk ID, file list; 0: none */
    uint16_t load_entry;        /* LoadFiles */
    uint16_t jump;              /* JMP ($DFFC) into the game; 0: none */
    uint16_t loop_branch;       /* the branch that enters the license loop (0: no loop to skip) */
    uint16_t loop_entry;        /* where it lands: LDA #count / STA counter */
    uint16_t loop_top;          /* the first instruction of a pass */
    uint8_t  loop_counter;      /* zero-page timer the loop waits on */
    uint8_t  timer_divider;     /* $E9D3's divider (the timer array starts after it) */
    uint8_t  fast_last, slow_last, divider_reload;   /* $E9D3: every frame to fast_last, every reload+1 to slow_last */
    uint8_t  scroll, scroll_step, scroll_limit;       /* the pass's scroll update */
    uint16_t license;           /* the BIOS's copy of the license screen (0: none) */
    uint16_t license_vram;      /* where the disk's copy must be */
    uint8_t  license_len;
    /* LoadFiles's STA $2001 turning rendering off for a PPU file ($E54D-$E553):
     * mid-frame, on the 2C02 that records an OAM row to corrupt when rendering
     * restarts, which the skip takes from its pre-run (0: none) */
    uint16_t mask_store;
    uint8_t  load_check[3], jump_check[3], loop_check[4], mask_check[3];
} NesFdsBootModel;

static inline NesFdsBootModel nes_fds_boot_builtin_model(uint32_t bios_crc32)
{
    NesFdsBootModel m;
    memset(&m, 0, sizeof(m));
    if (bios_crc32 == 0x5E607DCFu) {
        m.load_call = 0xEF59; m.load_entry = 0xE1F8;
        m.load_check[0] = 0x20; m.load_check[1] = 0xF8; m.load_check[2] = 0xE1;        /* JSR $E1F8 */
        m.jump = 0xEE9F;
        m.jump_check[0] = 0x6C; m.jump_check[1] = 0xFC; m.jump_check[2] = 0xDF;        /* JMP ($DFFC) */
        m.loop_branch = 0xEF65; m.loop_entry = 0xEFAF; m.loop_top = 0xEFB3;
        m.loop_check[0] = 0xA9; m.loop_check[1] = 0x20; m.loop_check[2] = 0x85; m.loop_check[3] = 0xA2;
        m.loop_counter = 0xA2;
        m.timer_divider = 0x80; m.fast_last = 0x9F; m.slow_last = 0xBF; m.divider_reload = 9;
        m.scroll = 0xFC; m.scroll_step = 2; m.scroll_limit = 0xB0;
        m.license = 0xED37; m.license_vram = 0x2800; m.license_len = 0xE0;
        m.mask_store = 0xE553;
        m.mask_check[0] = 0x8D; m.mask_check[1] = 0x01; m.mask_check[2] = 0x20;      /* STA $2001 */
    }
    return m;
}

#endif /* NES_FDS_HLE_H */
