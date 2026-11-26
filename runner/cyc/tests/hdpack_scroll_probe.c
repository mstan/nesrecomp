/* Authored backgrounds consume recorded PPU coordinates, not control bits.
 * ROM-free literal controls cover page, split, alternative and reset semantics. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define NESRECOMP_GAME_NO_HDPACK 1
#define NESRECOMP_CYCLE_HDPACK 1
#define NESRECOMP_CYCLE_HDPACK_MODS 1
#ifndef HDPACK_SCROLL_SOURCE
#define HDPACK_SCROLL_SOURCE "../../src/hdpack.c"
#endif
#include HDPACK_SCROLL_SOURCE

HwMachine hw;
CycLine hw_frame_lines[240];
uint32_t cyc_ring_frame;
/* Unrelated exported services are never exercised by this fixture. PE linkers
 * retain their references: fail immediately if a tested path reaches one. */
HwCart hw_cart;
HwPpu ppu;
uint32_t hw_palette_argb[512];
unsigned char *stbi_load(const char *p,int *w,int *h,int *c,int n) {
    (void)p;(void)w;(void)h;(void)c;(void)n;abort();
}
void stbi_image_free(void *p) { (void)p;abort(); }
const char *stbi_failure_reason(void) { abort(); }
void hdpack_audio_reset(void) { abort(); }
int hdpack_audio_define(int m,unsigned a,unsigned t,const char *d,const char *f) {
    (void)m;(void)a;(void)t;(void)d;(void)f;abort();
}
int hdpack_audio_validate_sources(void) { abort(); }
uint32_t hw_cart_mmc5_chr_index(uint16_t a) { (void)a;abort(); }
uint16_t hw_cart_nt_a10(uint16_t a) { (void)a;abort(); }
int cyc_video_width(void) { abort(); }
const uint32_t *cyc_frame_argb(void) { abort(); }
const CycLine *cyc_frame_lines(void) { return hw_frame_lines; }

static int failures;
#define CHECK(value, name) do { if (!(value)) { \
    fprintf(stderr, "FAIL: %s\n", name); ++failures; } } while (0)

static void record_line(int row, unsigned x, unsigned y, unsigned ctrl) {
    CycLine *l=&hw_frame_lines[row];
    unsigned coarse_x=((x>>3)+2)&63;
    l->v=(uint16_t)((coarse_x&31)|((coarse_x&32)<<5)|
        (((y%240)>>3)<<5)|((y/240)<<11)|((y&7)<<12));
    l->fine_x=x&7;l->ctrl=(uint8_t)ctrl;
}
static HdBg backgrounds[4];
static HdBg *sorted[4],*active[4];
static HdBgImg image;
static uint32_t pixels[8*64];
static void setup(void) {
    memset(backgrounds,0,sizeof backgrounds);memset(hw_frame_lines,0,sizeof hw_frame_lines);
    memset(&image,0,sizeof image);memset(pixels,0,sizeof pixels);
    image.w=8;image.h=64;image.px=pixels;
    s_scale=1;s_native_w=8;s_bg_count=1;s_nact=0;
    s_bgs=backgrounds;s_bg_sorted=sorted;s_actbg=active;
    s_hud_rows=0;s_band_mid=s_band_fg=0;
    for(int i=0;i<4;++i) {
        sorted[i]=&backgrounds[i];backgrounds[i].img=&image;
        backgrounds[i].priority=i*10;backgrounds[i].brightness=255;
    }
    backgrounds[0].hscroll=backgrounds[0].vscroll=1;
}
int main(void) {
    setup();record_line(0,0,277,0);record_line(239,300,36,0);
    eval_backgrounds_for_frame();
    CHECK(backgrounds[0].offx==300,"recorded horizontal page retained");
    CHECK(backgrounds[0].offy==277,"recorded vertical page retained");
    record_line(239,300,36,3);eval_backgrounds_for_frame();
    CHECK(backgrounds[0].offx==300 && backgrounds[0].offy==277,
          "pattern/control bits do not invent nametable pages");

    setup();s_bg_count=4;
    for(int i=0;i<3;++i)backgrounds[i].priority=12;
    backgrounds[3].priority=30;
    /* Failed first alternative must not hide the next authored match. */
    HdCond no={0};no.type=CT_MEM_CHECK_CONST;no.mask=255;no.operandA=0x20;
    no.operandB=1;no.op=OP_EQ;
    HdCond *conditions[]={&no};int neg[]={0};
    backgrounds[0].conds=conditions;backgrounds[0].neg=neg;backgrounds[0].ncond=1;
    eval_backgrounds_for_frame();
    CHECK(s_nact==2 && active[0]==&backgrounds[1] && active[1]==&backgrounds[3],
          "first matching alternative per priority, distinct priorities retained");
    /* Comparator has an explicit authored-order tie break, independent of qsort. */
    CHECK(bg_priority_cmp(&sorted[0],&sorted[1])<0,
          "equal-priority alternatives preserve manifest order");

    setup();s_bg_count=2;backgrounds[1].priority=39;
    for(unsigned i=0;i<sizeof pixels/sizeof pixels[0];++i)pixels[i]=0xff112233;
    record_line(0,0,0,0);record_line(64,0,301,0);record_line(239,0,476,0);
    eval_backgrounds_for_frame();
    CHECK(s_hud_rows==64,"authored stationary HUD extent retained");
    CHECK(backgrounds[0].offy==301,"room samples first content scanline after fixed HUD");
    CHECK(backgrounds[1].offy==0,"stationary HUD remains stationary");
    /* First content line, rather than last line or subtracting HUD twice. */
    record_line(64,0,17,0);eval_backgrounds_for_frame();
    CHECK(backgrounds[0].offy==17,"vertical transition follows current split scroll");

    s_nact=3;s_band_mid=2;s_band_fg=3;s_hud_rows=64;s_actbg=NULL;
    eval_backgrounds_for_frame();
    CHECK(s_nact==0 && s_band_mid==0 && s_band_fg==0 && s_hud_rows==0,
          "empty active table clears stale bands and HUD");
    if(failures)return 1;
    puts("PASS: native HD background pages, alternatives, split scrolling and empty-table reset");
    return 0;
}
