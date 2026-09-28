/* fds_hle_plan_test.c - the FDS HLE tier's pure decisions (common/nes_fds_hle.h):
 * request parsing, source precedence, the whole plan matrix (every
 * combination of requests and capability facts), the disk-ID matching the
 * auto swap uses, the boot skip and auto insert axes, the built-in boot model,
 * and the boot load analysis (common/nes_fds_boot.h) over synthetic streams. */
#include "../../common/nes_fds_hle.h"
#include "../../common/nes_fds_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static NesFdsHleAsk ask(const char *s)
{
    NesFdsHleAsk a;
    CHECK(nes_fds_hle_parse(s, &a, NULL));
    return a;
}

static void parsing(void)
{
    NesFdsHleAsk a = ask(NULL);
    CHECK(a.auto_swap == -1 && a.fast_load == -1);
    a = ask("");
    CHECK(a.auto_swap == -1 && a.fast_load == -1);
    a = ask("auto-swap");
    CHECK(a.auto_swap == 1 && a.fast_load == -1);
    a = ask("fast-load");
    CHECK(a.auto_swap == -1 && a.fast_load == 1);
    a = ask("auto-swap,fast-load");
    CHECK(a.auto_swap == 1 && a.fast_load == 1);
    a = ask("  Auto_Swap + FAST-LOAD ");
    CHECK(a.auto_swap == 1 && a.fast_load == 1);
    a = ask("all");
    CHECK(a.auto_swap == 1 && a.fast_load == 1);
    a = ask("on");
    CHECK(a.auto_swap == 1 && a.fast_load == 1);
    a = ask("off");
    CHECK(a.auto_swap == 0 && a.fast_load == 0);
    a = ask("none");
    CHECK(a.auto_swap == 0 && a.fast_load == 0);
    a = ask("all,no-fast-load");
    CHECK(a.auto_swap == 1 && a.fast_load == 0);
    a = ask("all no-auto-swap");
    CHECK(a.auto_swap == 0 && a.fast_load == 1);
    a = ask("fast-load,off");            /* later words win */
    CHECK(a.auto_swap == 0 && a.fast_load == 0);
    const char *bad;
    CHECK(!nes_fds_hle_parse("auto-swap,turbo", &a, &bad) && bad && !strncmp(bad, "turbo", 5));
    CHECK(!nes_fds_hle_parse("autoswap", &a, &bad) && bad && !strcmp(bad, "autoswap"));
    CHECK(!nes_fds_hle_parse("fast-loads", &a, &bad));
    CHECK(!nes_fds_hle_parse("auto-swa", &a, &bad));
}

static NesFdsHleRequest capable(void)
{
    NesFdsHleRequest r;
    memset(&r, 0, sizeof(r));
    r.config = r.env = r.cli = r.live = NES_FDS_HLE_ASK_NONE;
    r.is_fds = true;
    r.have_anchor = true;
    r.sides = 2;
    r.sides_with_id = 2;
    r.have_boot = r.have_jump = true;
    return r;
}

static void precedence(void)
{
    static const int8_t V[3] = { -1, 0, 1 };
    static const char *const FROM[4] = { "game.toml", "env", "cli", "toggle" };
    /* Every combination of the four sources for both axes: the last source that
     * says something decides, and says where the decision came from. */
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < 3; ++e)
            for (int l = 0; l < 3; ++l)
                for (int t = 0; t < 3; ++t) {
                    NesFdsHleRequest r = capable();
                    r.config.auto_swap = V[c]; r.env.auto_swap = V[e]; r.cli.auto_swap = V[l]; r.live.auto_swap = V[t];
                    r.config.fast_load = V[t]; r.env.fast_load = V[l]; r.cli.fast_load = V[e]; r.live.fast_load = V[c];
                    NesFdsHlePlan p = nes_fds_hle_plan(r);
                    int8_t s[4] = { V[c], V[e], V[l], V[t] }, f[4] = { V[t], V[l], V[e], V[c] };
                    int8_t ws = 0, wf = 0;
                    const char *fs = "default", *ff = "default";
                    for (int i = 0; i < 4; ++i) {
                        if (s[i] >= 0) { ws = s[i]; fs = FROM[i]; }
                        if (f[i] >= 0) { wf = f[i]; ff = FROM[i]; }
                    }
                    CHECK(p.auto_swap == (ws == 1));
                    CHECK(p.fast_load == (wf == 1));
                    CHECK(!strcmp(p.auto_swap_from, fs));
                    CHECK(!strcmp(p.fast_load_from, ff));
                    CHECK(!p.auto_swap_denied && !p.fast_load_denied);
                    CHECK(p.observe);
                }
}

static void matrix(void)
{
    /* Every request x every capability fact. */
    for (int want_swap = 0; want_swap < 2; ++want_swap)
        for (int want_fast = 0; want_fast < 2; ++want_fast)
            for (int is_fds = 0; is_fds < 2; ++is_fds)
                for (int anchor = 0; anchor < 2; ++anchor)
                    for (unsigned sides = 0; sides <= 4; ++sides)
                        for (unsigned with_id = 0; with_id <= sides; ++with_id) {
                            NesFdsHleRequest r = capable();
                            r.cli.auto_swap = (int8_t)want_swap;
                            r.cli.fast_load = (int8_t)want_fast;
                            r.is_fds = is_fds;
                            r.have_anchor = anchor;
                            r.sides = sides;
                            r.sides_with_id = with_id;
                            NesFdsHlePlan p = nes_fds_hle_plan(r);
                            bool swap_ok = is_fds && anchor && sides >= 2 && with_id == sides;
                            CHECK(p.auto_swap == (want_swap && swap_ok));
                            CHECK(p.auto_swap_denied == (want_swap && !swap_ok));
                            CHECK((p.auto_swap_why != NULL) == p.auto_swap_denied);
                            /* fast load depends on its own request and the drive only */
                            CHECK(p.fast_load == (want_fast && is_fds));
                            CHECK(p.fast_load_denied == (want_fast && !is_fds));
                            CHECK((p.fast_load_why != NULL) == p.fast_load_denied);
                            CHECK(p.observe == (is_fds && anchor));
                            /* the axes are independent: flipping one request never
                             * changes the other axis's outcome */
                            NesFdsHleRequest q = r;
                            q.cli.auto_swap = (int8_t)!want_swap;
                            NesFdsHlePlan pq = nes_fds_hle_plan(q);
                            CHECK(pq.fast_load == p.fast_load && pq.fast_load_denied == p.fast_load_denied);
                            q = r;
                            q.cli.fast_load = (int8_t)!want_fast;
                            pq = nes_fds_hle_plan(q);
                            CHECK(pq.auto_swap == p.auto_swap && pq.auto_swap_denied == p.auto_swap_denied);
                            /* pure: the same request gives the same plan */
                            NesFdsHlePlan again = nes_fds_hle_plan(r);
                            CHECK(!memcmp(&again, &p, sizeof(p)));
                        }
    /* The reasons, one per missing capability, in order. */
    NesFdsHleRequest r = capable();
    r.cli.auto_swap = 1;
    r.is_fds = false;
    CHECK(strstr(nes_fds_hle_plan(r).auto_swap_why, "not a Famicom Disk System"));
    r = capable(); r.cli.auto_swap = 1; r.have_anchor = false;
    CHECK(strstr(nes_fds_hle_plan(r).auto_swap_why, "anchor"));
    r = capable(); r.cli.auto_swap = 1; r.sides = r.sides_with_id = 1;
    CHECK(strstr(nes_fds_hle_plan(r).auto_swap_why, "one side"));
    r = capable(); r.cli.auto_swap = 1; r.sides_with_id = 1;
    CHECK(strstr(nes_fds_hle_plan(r).auto_swap_why, "header"));
    /* Nothing asked: nothing on, nothing denied, whatever the capabilities. */
    r = capable();
    NesFdsHlePlan p = nes_fds_hle_plan(r);
    CHECK(!p.auto_swap && !p.fast_load && !p.auto_swap_denied && !p.fast_load_denied);
    CHECK(!strcmp(p.auto_swap_from, "default") && !strcmp(p.fast_load_from, "default"));
    /* A live toggle cannot force a refused axis. */
    r = capable(); r.have_anchor = false; r.live.auto_swap = 1;
    p = nes_fds_hle_plan(r);
    CHECK(!p.auto_swap && p.auto_swap_denied && !strcmp(p.auto_swap_from, "toggle"));
}

static void anchors(void)
{
    NesFdsHleAnchor a = nes_fds_hle_builtin_anchor(0x5E607DCFu);
    CHECK(a.id_check == 0xE445 && a.id_pointer == 0 && a.check_len == 3);
    CHECK(a.check[0] == 0x20 && a.check[1] == 0xE3 && a.check[2] == 0xE6);
    a = nes_fds_hle_builtin_anchor(0x12345678u);
    CHECK(a.id_check == 0);
}

static void matching(void)
{
    uint8_t block1[56] = { 0x01 };
    memcpy(block1 + 1, "*NINTENDO-HVC*", 14);
    const uint8_t hdr[10] = { 0xB1, 'O', 'T', 'O', 0x20, 0x00, 0x01, 0x00, 0x00, 0x0F };
    memcpy(block1 + 15, hdr, 10);
    uint8_t id[10];
    memcpy(id, hdr, 10);
    CHECK(nes_fds_hle_id_matches(id, block1));
    for (unsigned i = 0; i < 10; ++i) {
        memcpy(id, hdr, 10);
        id[i] ^= 1;
        CHECK(!nes_fds_hle_id_matches(id, block1));      /* every byte is compared */
        id[i] = 0xFF;
        CHECK(nes_fds_hle_id_matches(id, block1));       /* ...unless the ID says $FF */
    }
    memset(id, 0xFF, 10);
    CHECK(nes_fds_hle_id_matches(id, block1));
    /* block 1 out of a drive stream */
    uint8_t stream[200];
    memset(stream, 0, sizeof(stream));
    stream[100] = 0x80;
    memcpy(stream + 101, block1, 56);
    uint8_t out[56];
    CHECK(nes_fds_hle_block1(stream, sizeof(stream), out) && !memcmp(out, block1, 56));
    CHECK(!nes_fds_hle_block1(stream, 150, out));         /* cut short */
    stream[101] = 0x02;
    CHECK(!nes_fds_hle_block1(stream, sizeof(stream), out));   /* not block 1 */
    stream[101] = 0x01;
    stream[100] = 0x81;
    CHECK(!nes_fds_hle_block1(stream, sizeof(stream), out));   /* no gap mark */
    memset(stream, 0, sizeof(stream));
    CHECK(!nes_fds_hle_block1(stream, sizeof(stream), out));   /* blank */
}

/* ---- the boot axes ---- */
static void boot_axes(void)
{
    NesFdsHleAsk a = ask("boot-skip");
    CHECK(a.boot_skip == 1 && a.auto_swap == -1 && a.fast_load == -1 && a.auto_insert == -1);
    a = ask("all");
    CHECK(a.boot_skip == 1 && a.auto_swap == 1 && a.fast_load == 1 && a.auto_insert == -1);
    a = ask("off");
    CHECK(a.boot_skip == 0 && a.auto_swap == 0 && a.fast_load == 0 && a.auto_insert == -1);
    a = ask("all,no-boot-skip,no-auto-insert");
    CHECK(a.boot_skip == 0 && a.auto_swap == 1 && a.auto_insert == 0);
    a = ask("Auto_Insert");
    CHECK(a.auto_insert == 1);
    const char *bad;
    CHECK(!nes_fds_hle_parse("boot-skipper", &a, &bad) && bad);
    CHECK(!nes_fds_hle_parse("bootskip", &a, &bad) && bad);
    /* boot skip: defaults off, precedence like the others, refusals in order */
    NesFdsHleRequest r = capable();
    NesFdsHlePlan p = nes_fds_hle_plan(r);
    CHECK(!p.boot_skip && !p.boot_skip_denied && !strcmp(p.boot_skip_from, "default"));
    static const int8_t V[3] = { -1, 0, 1 };
    static const char *const FROM[4] = { "game.toml", "env", "cli", "toggle" };
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < 3; ++e)
            for (int l = 0; l < 3; ++l) {
                r = capable();
                r.config.boot_skip = V[c]; r.env.boot_skip = V[e]; r.cli.boot_skip = V[l];
                int8_t s3[3] = { V[c], V[e], V[l] }, w = 0;
                const char *from = "default";
                for (int i = 0; i < 3; ++i) if (s3[i] >= 0) { w = s3[i]; from = FROM[i]; }
                p = nes_fds_hle_plan(r);
                CHECK(p.boot_skip == (w == 1) && !strcmp(p.boot_skip_from, from));
                /* independent of the other axes */
                NesFdsHleRequest q = r;
                q.cli.auto_swap = 1; q.cli.fast_load = 1; q.have_anchor = false;
                CHECK(nes_fds_hle_plan(q).boot_skip == p.boot_skip);
            }
    for (int fds = 0; fds < 2; ++fds)
        for (int model = 0; model < 2; ++model)
            for (int why = 0; why < 2; ++why) {
                r = capable();
                r.cli.boot_skip = 1;
                r.is_fds = fds; r.have_boot = model; r.boot_why = why ? "the image says no" : NULL;
                p = nes_fds_hle_plan(r);
                bool ok = fds && model && !why;
                CHECK(p.boot_skip == ok && p.boot_skip_denied == !ok && (p.boot_skip_why != NULL) == !ok);
                if (!fds) CHECK(strstr(p.boot_skip_why, "Famicom"));
                else if (!model) CHECK(strstr(p.boot_skip_why, "boot model"));
                else if (why) CHECK(!strcmp(p.boot_skip_why, "the image says no"));
                /* refusing the skip changes no other axis */
                NesFdsHleRequest q = r;
                q.cli.boot_skip = 0;
                NesFdsHlePlan pq = nes_fds_hle_plan(q);
                CHECK(pq.auto_swap == p.auto_swap && pq.fast_load == p.fast_load && pq.auto_insert == p.auto_insert);
            }
    /* auto insert: on by default; quietly off without the jump, loudly refused when asked */
    r = capable();
    p = nes_fds_hle_plan(r);
    CHECK(p.auto_insert && !strcmp(p.auto_insert_from, "default"));
    r.have_jump = false;
    p = nes_fds_hle_plan(r);
    CHECK(!p.auto_insert && !p.auto_insert_denied && !p.auto_insert_why);
    r.cli.auto_insert = 1;
    p = nes_fds_hle_plan(r);
    CHECK(!p.auto_insert && p.auto_insert_denied && strstr(p.auto_insert_why, "jump"));
    r = capable(); r.env.auto_insert = 0;
    p = nes_fds_hle_plan(r);
    CHECK(!p.auto_insert && !p.auto_insert_denied && !strcmp(p.auto_insert_from, "env"));
    r = capable(); r.cli.auto_swap = r.cli.fast_load = r.cli.boot_skip = 0;   /* "off" */
    CHECK(nes_fds_hle_plan(r).auto_insert);
    r = capable(); r.is_fds = false;
    CHECK(!nes_fds_hle_plan(r).auto_insert);
    /* the built-in boot model */
    NesFdsBootModel m = nes_fds_boot_builtin_model(0x5E607DCFu);
    CHECK(m.load_call == 0xEF59 && m.load_entry == 0xE1F8 && m.jump == 0xEE9F && m.loop_entry == 0xEFAF);
    CHECK(m.load_check[0] == 0x20 && m.jump_check[0] == 0x6C && m.mask_store == 0xE553);
    m = nes_fds_boot_builtin_model(0x12345678u);
    CHECK(!m.load_call && !m.jump && !m.loop_branch);
    CHECK(nes_fds_boot_proven(0xF04CD4CDu) && !nes_fds_boot_proven(0x7345C69Cu) && !nes_fds_boot_proven(0));
}

/* ---- the boot load analysis over synthetic streams ---- */
enum { LEN = 65500 };
static uint8_t st[70000];
static uint32_t at;

static void put_block(const uint8_t *b, uint32_t n, uint32_t gap, bool bad_crc)
{
    at += gap;                                   /* zeros */
    st[at] = 0x80;
    memcpy(st + at + 1, b, n);
    uint16_t crc = nes_fds_crc16(0, st + at, 1 + n);
    if (bad_crc) crc ^= 0x1234;
    st[at + 1 + n] = (uint8_t)crc;
    st[at + 2 + n] = (uint8_t)(crc >> 8);
    at += 3 + n;
}

typedef struct { uint8_t id; uint16_t addr, size; uint8_t type; } Spec;

static uint32_t build(const Spec *f, unsigned n, unsigned amount, uint8_t side, uint8_t boot, uint32_t gap, int bad)
{
    memset(st, 0, sizeof(st));
    at = 0;
    uint8_t b1[56] = { 1 };
    memcpy(b1 + 1, "*NINTENDO-HVC*", 14);
    b1[21] = side; b1[22] = 0; b1[25] = boot;
    put_block(b1, 56, 3537, bad == 1);
    uint8_t b2[2] = { 2, (uint8_t)amount };
    put_block(b2, 2, gap, false);
    for (unsigned k = 0; k < n; ++k) {
        uint8_t h[16] = { 3, (uint8_t)k, f[k].id, 'F', 'I', 'L', 'E', ' ', ' ', ' ', ' ',
                          (uint8_t)f[k].addr, (uint8_t)(f[k].addr >> 8), (uint8_t)f[k].size,
                          (uint8_t)(f[k].size >> 8), f[k].type };
        put_block(h, 16, 122, false);
        static uint8_t d[0x10000];
        d[0] = 4;
        for (uint32_t i = 0; i < f[k].size; ++i) d[1 + i] = (uint8_t)(i * 7 + k);
        put_block(d, 1u + f[k].size, 122, bad == 2 && k == n - 1);
    }
    return LEN;
}

static void boot_reads(void)
{
    static const uint8_t ID[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF };
    uint8_t list[20];
    memset(list, 0xFF, sizeof(list));
    static NesFdsBoot b;
    const Spec ok[] = { { 0, 0x2800, 0xE0, 2 }, { 1, 0x0000, 0x100, 1 }, { 2, 0x6000, 0x200, 0 },
                        { 3, 0x0100, 0x10, 0 }, { 4, 0xFFF0, 0x20, 0 }, { 0x20, 0x7000, 0x10, 0 },
                        { 5, 0x0200, 0x40, 0 }, { 6, 0xE000, 0x10, 0 } };
    uint32_t n = build(ok, 8, 8, 0, 0x0F, 122, 0);
    CHECK(nes_fds_boot_read(st, n, ID, list, false, &b) && !b.why);
    CHECK(b.files == 8 && b.amount == 8 && b.boot_code == 0x0F);
    static const uint8_t HOW[8] = { NES_FDS_BOOT_LOAD, NES_FDS_BOOT_LOAD, NES_FDS_BOOT_LOAD, NES_FDS_BOOT_SKIP_LOW,
                                    NES_FDS_BOOT_SKIP_WRAP, NES_FDS_BOOT_NOT_ASKED, NES_FDS_BOOT_LOAD,
                                    NES_FDS_BOOT_LOAD };
    for (unsigned k = 0; k < 8; ++k) CHECK(b.file[k].how == HOW[k] && b.file[k].size == ok[k].size);
    CHECK(b.requested == 7 && b.loaded == 5);
    CHECK(b.end_pos == at && b.last_byte == st[at - 1]);
    CHECK(st[b.file[2].data_pos] == 0 * 7 + 2 && b.file[2].data_crc_ok && b.file[2].header_crc_ok);
    /* the list: only the IDs it names */
    uint8_t named[20];
    memset(named, 0xFF, sizeof(named));
    named[0] = 0x20; named[1] = 2;
    CHECK(nes_fds_boot_read(st, n, ID, named, false, &b));
    CHECK(b.file[5].how == NES_FDS_BOOT_LOAD && b.file[2].how == NES_FDS_BOOT_LOAD &&
          b.file[0].how == NES_FDS_BOOT_NOT_ASKED && b.requested == 2);
    /* the boot code decides */
    build(ok, 8, 8, 0, 0x01, 122, 0);
    CHECK(nes_fds_boot_read(st, n, ID, list, false, &b) && b.file[2].how == NES_FDS_BOOT_NOT_ASKED);
    /* hidden files are not read */
    build(ok, 8, 3, 0, 0x0F, 122, 0);
    CHECK(nes_fds_boot_read(st, n, ID, list, false, &b) && b.files == 3);
    /* refusals */
    build(ok, 8, 8, 1, 0x0F, 122, 0);
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_ID);
    build(ok, 8, 9, 0, 0x0F, 122, 0);
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_BLOCK);
    build(ok, 8, 8, 0, 0x0F, 20, 0);
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_GAP);
    build(ok, 8, 8, 0, 0x0F, 122, 1);
    CHECK(nes_fds_boot_read(st, n, ID, list, false, &b));             /* CRCs unchecked */
    CHECK(!nes_fds_boot_read(st, n, ID, list, true, &b) && b.why_code == NES_FDS_BOOT_WHY_CRC);
    build(ok, 8, 8, 0, 0x0F, 122, 2);
    CHECK(!nes_fds_boot_read(st, n, ID, list, true, &b) && b.why_code == NES_FDS_BOOT_WHY_CRC);
    const Spec io[] = { { 0, 0x5FF0, 0x20, 0 } }, low[] = { { 0, 0x0700, 0x200, 0 } }, empty[] = { { 0, 0x7000, 0, 0 } },
               wrap_ok[] = { { 0, 0x0100, 0x40, 0 } }, rom[] = { { 0, 0xE000, 0x100, 0 } };
    build(io, 1, 1, 0, 0x0F, 122, 0);
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_IO);
    build(low, 1, 1, 0, 0x0F, 122, 0);
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_LOW);
    build(empty, 1, 1, 0, 0x0F, 122, 0);
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_EMPTY);
    build(wrap_ok, 1, 1, 0, 0x0F, 122, 0);                                      /* dropped by the BIOS: fine */
    CHECK(nes_fds_boot_read(st, n, ID, list, false, &b) && b.file[0].how == NES_FDS_BOOT_SKIP_LOW);
    build(rom, 1, 1, 0, 0x0F, 122, 0);                                          /* the ROM ignores it */
    CHECK(nes_fds_boot_read(st, n, ID, list, false, &b) && b.file[0].how == NES_FDS_BOOT_LOAD);
    build(rom, 1, 1, 0, 0x0F, 122, 0);
    CHECK(!nes_fds_boot_read(st, 69000, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_TURNING);
    memset(st, 0, sizeof(st));
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_NO_HEADER);
    build(ok, 8, 8, 0, 0x0F, 122, 0);
    st[3537 + 1 + 3] ^= 1;                                                       /* *NINTENDO-HVC* */
    CHECK(!nes_fds_boot_read(st, n, ID, list, false, &b) && b.why_code == NES_FDS_BOOT_WHY_SIGNATURE);
}

int main(void)
{
    parsing();
    precedence();
    matrix();
    anchors();
    matching();
    boot_axes();
    boot_reads();
    printf("fds_hle_plan_test: %u checks passed\n", checks);
    return 0;
}
