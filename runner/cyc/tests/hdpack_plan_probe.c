/* Compare the production plan with the retained independent pixel compositor.
 * No assets or game ROM required. Assertions stay active in release builds. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define NESRECOMP_GAME_NO_HDPACK 1
#define NESRECOMP_HDPACK_LAYER_PLANS 1
#include "../../src/hdpack.c"

#ifdef NESRECOMP_HDPACK_GPU_PROBE
extern void hdpack_gpu_probe_check(const HdJobFrame *, unsigned,
                                   const uint32_t *, size_t);
#endif

#ifdef NESRECOMP_HDPACK_CACHE_PROBE
#include "../cyc_hd_gpu_cache.h"
static void check_page_cache(void)
{
    /* The cache never reads or owns source pixels; synthetic large source
     * extents exercise metadata/ROI admission without allocating that image. */
    uint32_t source=0xff123456;
    HdJobImage images[3]={{&source,8,8,255},{&source,8,8,255},{&source,8,8,255}};
    HdJobPixel pixels[4]={{0}};
    HdJobBackground bg={2,0,0,255,0};
    HdJobFrame f={0};f.pack_generation=1;f.width=f.height=f.source_scale=1;
    f.image_count=3;f.images=images;f.pixels=pixels;f.backgrounds=&bg;
    pixels[0].flags=HD_JOB_IMAGE|(HD_JOB_IMAGE<<HD_JOB_SP_SHIFT);
    pixels[0].bg_image=0;pixels[0].sp_image=1;
    HdJobSprite extra={2,0,0,HD_JOB_IMAGE<<HD_JOB_SP_SHIFT};
    f.sprites=&extra;f.sprite_count=1;pixels[0].flags|=1u<<HD_JOB_EXTRA_SHIFT;
    HdGpuPageCache layered={0};
    assert(!cyc_hd_gpu_cache_prepare(&layered,&f,2,1000000));
    assert(cyc_hd_gpu_cache_prepare(&layered,&f,3,1000000) && layered.needed_count==3);
    pixels[0].flags|=1u<<HD_JOB_EXTRA_START;
    assert(!cyc_hd_gpu_cache_prepare(&layered,&f,3,1000000));
    pixels[0].flags&=~(1u<<HD_JOB_EXTRA_START);f.sprites=NULL;
    assert(!cyc_hd_gpu_cache_prepare(&layered,&f,3,1000000));f.sprites=&extra;
    extra.flags|=1;
    assert(!cyc_hd_gpu_cache_prepare(&layered,&f,3,1000000));extra.flags&=~1u;
    cyc_hd_gpu_cache_destroy(&layered);
    f.sprites=NULL;f.sprite_count=0;pixels[0].flags&=~(1u<<HD_JOB_EXTRA_SHIFT);
    HdGpuPageCache c={0};uint32_t slot;bool upload;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(c.page_count==3 && c.needed_count==2);
    assert(cyc_hd_gpu_cache_reserve(&c,0,&slot,&upload) && upload && slot==0);
    assert(cyc_hd_gpu_cache_reserve(&c,1,&slot,&upload) && upload && slot==1);
    assert(cyc_hd_gpu_cache_reserve(&c,1,&slot,&upload) && !upload && slot==1);
    pixels[0].flags=HD_JOB_IMAGE;pixels[0].bg_image=2;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(cyc_hd_gpu_cache_reserve(&c,2,&slot,&upload) && upload && slot==0);
    assert(c.page_slots[0]==HD_GPU_PAGE_ABSENT && c.page_slots[1]==1);
    /* Page1 is older than page2, but the entire new needed set protects it
     * before page0 is reserved (page0 is deliberately first in needed_ids). */
    pixels[0].flags=HD_JOB_IMAGE|(HD_JOB_IMAGE<<HD_JOB_SP_SHIFT);pixels[0].bg_image=0;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(c.needed_ids[0]==0 && c.needed_ids[1]==1);
    assert(cyc_hd_gpu_cache_reserve(&c,0,&slot,&upload) && upload && slot==0);
    assert(cyc_hd_gpu_cache_reserve(&c,1,&slot,&upload) && !upload && slot==1);
    uint32_t old_slots[2],old_pages[3];uint64_t old_tick=c.tick;
    memcpy(old_slots,c.slot_pages,sizeof old_slots);memcpy(old_pages,c.page_slots,sizeof old_pages);
    f.background_count=f.under_end=f.mid_end=1;
    assert(!cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(!memcmp(old_slots,c.slot_pages,sizeof old_slots));
    assert(!memcmp(old_pages,c.page_slots,sizeof old_pages));
    assert(c.tick==old_tick && c.needed_count==0);
    assert(!cyc_hd_gpu_cache_reserve(&c,1,&slot,&upload));
    f.background_count=f.under_end=f.mid_end=0;++f.pack_generation;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    for(unsigned i=0;i<3;++i)assert(c.page_slots[i]==HD_GPU_PAGE_ABSENT);
    assert(cyc_hd_gpu_cache_reserve(&c,0,&slot,&upload) && upload);
    assert(cyc_hd_gpu_cache_reserve(&c,1,&slot,&upload) && upload);
    c.tick=UINT64_MAX;
    assert(cyc_hd_gpu_cache_reserve(&c,1,&slot,&upload) && !upload);
    assert(c.tick>c.slot_count && c.tick<UINT64_MAX);
    assert(!cyc_hd_gpu_cache_prepare(&c,&f,2049,1000000));
    assert(!cyc_hd_gpu_cache_prepare(&c,&f,2,11));
    cyc_hd_gpu_cache_destroy(&c);
    assert(!c.image_meta && !c.page_slots && !c.slot_pages && !c.needed_count);

    images[0]=(HdJobImage){&source,9216,3072,255};
    images[1]=(HdJobImage){&source,512,512,255};
    memset(pixels,0,sizeof pixels);f.image_count=2;f.width=f.height=1;f.source_scale=64;
    pixels[0].flags=(HD_JOB_IMAGE<<HD_JOB_SP_SHIFT)|HD_JOB_HMIRROR|HD_JOB_VMIRROR;
    pixels[0].sp_image=1;pixels[0].sp_uv=2u<<8;
    bg=(HdJobBackground){0,2,3,255,0};f.background_count=f.under_end=f.mid_end=1;
    ++f.pack_generation;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(c.page_count==72*24+16 && c.needed_count==2);
    assert(c.needed[73] && c.needed[72*24+7]);
    pixels[0].flags&=~(uint32_t)HD_JOB_VMIRROR;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(c.needed_count==2 && c.needed[72*24+7]);
    /* Signed clipping and the HUD exclusion affect only under-backgrounds. */
    memset(pixels,0,sizeof pixels);f.width=f.height=2;f.hud_rows=1;
    bg.offset_x=bg.offset_y=-1;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(c.needed_count==1 && c.needed[0]);
    f.hud_rows=2;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000) && !c.needed_count);
    f.under_end=0;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(c.needed_count==1 && c.needed[0]);
    bg.offset_x=INT32_MAX;bg.offset_y=INT32_MIN;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000) && !c.needed_count);
    cyc_hd_gpu_cache_destroy(&c);

    /* A source cell crossing a page corner needs four pages, not its whole
     * 8x8-cell image. Output scale is intentionally absent from the API. */
    images[0]=(HdJobImage){&source,504,504,255};f.image_count=1;
    f.width=f.height=1;f.source_scale=63;f.hud_rows=0;
    f.background_count=f.under_end=f.mid_end=0;
    pixels[0].flags=HD_JOB_IMAGE;pixels[0].bg_image=0;pixels[0].bg_uv=2|(2u<<8);
    ++f.pack_generation;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,4,1000000));
    assert(c.page_count==16 && c.needed_count==4);
    assert(c.needed[0] && c.needed[1] && c.needed[4] && c.needed[5]);
    cyc_hd_gpu_cache_destroy(&c);

    images[0]=(HdJobImage){&source,129,1,255};f.source_scale=1;f.width=2;f.height=1;
    memset(pixels,0,sizeof pixels);bg=(HdJobBackground){0,127,0,255,0};
    f.background_count=f.under_end=f.mid_end=1;++f.pack_generation;
    assert(cyc_hd_gpu_cache_prepare(&c,&f,2,1000000));
    assert(c.page_count==2 && c.needed_count==2 && c.needed[0] && c.needed[1]);
    uint64_t generation=c.pack_generation;
    images[0].width=128*1025;images[0].height=128*1024;++f.pack_generation;
    assert(!cyc_hd_gpu_cache_prepare(&c,&f,2,UINT64_MAX));
    assert(c.pack_generation==generation);
    cyc_hd_gpu_cache_destroy(&c);
    cyc_hd_gpu_cache_destroy(&c);
}
#endif

uint8_t g_ram[0x800], g_ppuscroll_x, g_ppuscroll_y, g_ppuctrl;
uint64_t g_frame_count;
unsigned char *stbi_load(const char *p,int *w,int *h,int *c,int n) {
    (void)p;(void)w;(void)h;(void)c;(void)n;return NULL;
}
void stbi_image_free(void *p) { free(p); }
const char *stbi_failure_reason(void) { return "no file I/O in this fixture"; }

static uint8_t patterns[3][16];
static unsigned reader_calls;
static void probe_pixel_reader(const void *context,size_t offset,HdPixel *out) {
    assert(offset<17*240);reader_calls++;
    *out=((const HdPixel *)context)[offset];
}
static HdCond *condition(HdCondType type, int cached) {
    HdCond *c=calloc(1,sizeof(*c));assert(c);
    c->type=type;c->use_cache=cached;c->cached_frame=UINT64_MAX;c->mask=0xff;
    s_cond_all=realloc(s_cond_all,(s_cond_count+1)*sizeof(*s_cond_all));assert(s_cond_all);
    s_cond_all[s_cond_count++]=c;return c;
}
static void tile(int key,int palette,int variant,int def,HdCond *a,HdCond *b,int neg) {
    HdTile *t=calloc(1,sizeof(*t));assert(t);
    t->key.is_chr_ram=s_is_chr_ram;t->key.tile_index=key+4;t->key.palette=palette;
    memcpy(t->key.tile16,patterns[key],16);
    t->w=t->h=8*s_scale;t->brightness=variant%3==0?350:variant%3==1?100:255;
    t->data=malloc((size_t)t->w*t->h*4);assert(t->data);
    for(int y=0;y<t->h;y++)for(int x=0;x<t->w;x++) {
        unsigned alpha=(x+y+variant)%5==0?0:(x^y^variant)&1?128:255;
        t->data[y*t->w+x]=premul((alpha<<24)|((unsigned)(variant*31+x*7)&255)<<16|
            ((unsigned)(variant*17+y*11)&255)<<8|((unsigned)(variant*41+x+y)&255));
        if(alpha!=255)t->has_transparent=1;
    }
    t->fully_transparent=variant==9;
    t->ncond=(a!=NULL)+(b!=NULL);
    if(t->ncond) {
        t->conds=calloc(t->ncond,sizeof(*t->conds));t->neg=calloc(t->ncond,sizeof(*t->neg));
        assert(t->conds&&t->neg);t->conds[0]=a;t->neg[0]=neg;
        if(b)t->conds[1]=b;
    }
    register_tile(t,def);
}

static void setup(int ram,int version,int scale) {
    s_active=1;s_is_chr_ram=ram;s_version=version;s_scale=scale;s_native_w=17;
    s_pixels=calloc(17*240,sizeof(*s_pixels));assert(s_pixels);
    HdCond *mem=condition(CT_MEM_CHECK_CONST,1);mem->operandA=0x20;mem->operandB=3;
    HdCond *pos=condition(CT_POS_X,0);pos->op=OP_GT;pos->operandA=7;
    HdCond *origin=condition(CT_ORIGIN_Y,0);origin->op=OP_GE;origin->operandA=32;
    HdCond *near=condition(CT_TILE_NEARBY,0);near->x=-1;near->y=1;
    near->ignore_palette=1;near->tile_index=ram?-1:4;memcpy(near->tile16,patterns[0],16);
    HdCond *flip=condition(CT_HMIRROR,0);
    HdCond *at=condition(CT_TILE_AT_POS,1);at->x=5;at->y=10;
    at->ignore_palette=1;at->tile_index=ram?-1:5;memcpy(at->tile16,patterns[1],16);
    for(int k=0;k<2;k++) {
        int pal=version<100?0x010203:0x0f010203;
        tile(k,pal,6,0,pos,mem,1);tile(k,pal,5,0,near,at,0);
        tile(k,pal,4,0,origin,mem,0);tile(k,pal,3,0,mem,NULL,0);
        tile(k,pal,2,0,pos,NULL,0);tile(k,pal,1,0,NULL,NULL,0);
        int sp=version<100?0x111213:(int)0xff111213u;
        tile(k,sp,9,0,pos,mem,0);tile(k,sp,7,0,flip,NULL,0);
        tile(k,sp,8,0,NULL,NULL,0);
    }
    /* Palette-independent definitions retain their own conditional gate. */
    tile(0,0x12345678,10,1,mem,NULL,1);
    s_bg_count=6;s_bgs=calloc(6,sizeof(*s_bgs));
    s_bg_sorted=calloc(6,sizeof(*s_bg_sorted));s_actbg=calloc(6,sizeof(*s_actbg));
    s_bgimg_count=4;s_bgimgs=calloc(4,sizeof(*s_bgimgs));
    assert(s_bgs&&s_bg_sorted&&s_actbg&&s_bgimgs);
    for(int i=0;i<3;i++) {
        HdBgImg *im=calloc(1,sizeof(*im));assert(im);s_bgimgs[i?i+1:0]=im;
        im->w=(17+9)*scale;im->h=(32+i*109)*scale;
        im->px=malloc((size_t)im->w*im->h*4);assert(im->px);
        for(int y=0;y<im->h;y++)for(int x=0;x<im->w;x++) {
            int xx=scale==10 && i==0?x%128:x,yy=scale==10 && i==0?y%128:y;
            unsigned a=(xx+yy+i)%7==0?0:((xx^yy)&1)?64:255;
            im->px[y*im->w+x]=premul((a<<24)|((unsigned)(xx*9+i*51)&255)<<16|
                ((unsigned)(yy*7+i*31)&255)<<8|((unsigned)(xx+yy*3)&255));
        }
        if(scale==10 && i==0) {
            im->pages=compact_image_pages(NULL,im->px,(unsigned)im->w,(unsigned)im->h);assert(im->pages);
            free(im->px);im->px=NULL;
        }
    }
    /* Failed source loads remain cached, but referencing backgrounds are
     * absent. This shared asset failure must not disable the valid jobs. */
    s_bgimgs[1]=calloc(1,sizeof(*s_bgimgs[1]));assert(s_bgimgs[1]);
    static const int priorities[]={0,19,20,29,30,39};
    for(int i=0;i<6;i++) {
        HdBg *b=&s_bgs[i];s_bg_sorted[i]=b;b->img=s_bgimgs[i%3?(i%3)+1:0];
        b->priority=priorities[i];b->brightness=i%3==0?350:i%3==1?100:255;
        b->blend=i%3;b->left=i-3;b->top=i==5?0:i*7-15;
        b->hscroll=(i-2)*0.5f;b->vscroll=(i-3)*0.25f;
        if(i==5){b->img=s_bgimgs[0];b->hscroll=b->vscroll=0;}
        if(i==3) {
            b->ncond=1;b->conds=malloc(sizeof(*b->conds));b->neg=calloc(1,sizeof(*b->neg));
            assert(b->conds&&b->neg);b->conds[0]=mem;
        }
    }
    s_use_render_plan=1;build_key_groups();
}

static void check_retained_jobs(const HdJobFrame *frame, const uint32_t *expected)
{
    size_t count=(size_t)frame->width*frame->height*frame->source_scale*frame->source_scale;
    uint32_t *out=malloc(count*4);assert(out);
    assert(!hdpack_render_jobs(frame,frame->source_scale,out,
                               (size_t)frame->width*frame->source_scale));
    assert(!memcmp(out,expected,count*4));free(out);
}

static void check_job_scales(const HdJobFrame *frame,const uint32_t *expected)
{
    static const unsigned scales[]={1,2,4,16,64};
    size_t source_pitch=(size_t)frame->width*frame->source_scale;
    for(unsigned i=0;i<sizeof(scales)/sizeof(scales[0]);++i) {
        unsigned scale=scales[i];size_t pitch=(size_t)frame->width*scale;
        size_t height=(size_t)frame->height*scale;
        uint32_t *out=malloc(pitch*height*4);assert(out);
        assert(!hdpack_render_jobs(frame,scale,out,pitch));
        for(size_t y=0;y<height;++y)for(size_t x=0;x<pitch;++x) {
            size_t sx=(x/scale)*frame->source_scale+(x%scale)*frame->source_scale/scale;
            size_t sy=(y/scale)*frame->source_scale+(y%scale)*frame->source_scale/scale;
            assert(out[y*pitch+x]==expected[sy*source_pitch+sx]);
        }
        free(out);
    }
    uint32_t untouched=0x12345678;
    assert(hdpack_render_jobs(frame,0,&untouched,1)<0 && untouched==0x12345678);
    assert(hdpack_render_jobs(frame,65,&untouched,1)<0 && untouched==0x12345678);
    assert(hdpack_render_jobs(frame,1,&untouched,0)<0 && untouched==0x12345678);
}

static void check_manifest_priority(void) {
    HdPixel current={0};HdKey key={0};key.tile_index=4;key.palette=0x0f010203;
    s_is_chr_ram=0;s_version=108;s_scale=1;
    HdCond *mem=condition(CT_MEM_CHECK_CONST,1);mem->operandA=0x20;mem->operandB=1;
    g_ram[0x20]=1;
    tile(0,key.palette,1,0,NULL,NULL,0);tile(0,key.palette,2,0,mem,NULL,0);
    assert(find_tile(&key,0,0,&current,0)==s_all[0]); // first manifest definition wins
    build_key_groups();s_plan_epoch=1;
    assert(find_tile_planned(&key,0,0,&current,0)==s_all[0]);
    hdpack_unload();s_is_chr_ram=0;s_version=108;s_scale=1;
    mem=condition(CT_MEM_CHECK_CONST,1);mem->operandA=0x20;mem->operandB=1;g_ram[0x20]=0;
    tile(0,key.palette,1,0,mem,NULL,0);tile(0,0x12345678,2,1,NULL,NULL,0);
    tile(1,0x12345678,3,1,NULL,NULL,0);tile(2,key.palette,4,0,NULL,NULL,0);
    tile(2,0x87654321,5,1,mem,NULL,0);
    char map[]="5,6";process_fallback(map);
    build_key_groups();s_plan_epoch=1;
    assert(!find_tile(&key,0,0,&current,0)); // exact list's failed gate is not a missing key
    assert(!find_tile_planned(&key,0,0,&current,0));
    key.tile_index=5;
    assert(find_tile(&key,0,0,&current,0)==s_all[3]); // fallback precedes source's wildcard
    assert(find_tile_planned(&key,0,0,&current,0)==s_all[3]);
    key.palette=0x11111111;
    assert(!find_tile(&key,0,0,&current,0)); // fallback wildcard still checks its gate
    assert(!find_tile_planned(&key,0,0,&current,0));
    char replacement[]="5,4";process_fallback(replacement);key.palette=0x0f010203;
    assert(!find_tile(&key,0,0,&current,0)); // last fallback definition wins; no recursive lookup
    hdpack_unload();
}

static void check_addition_resolution(void) {
    s_active=1;s_version=108;s_is_chr_ram=0;s_scale=1;s_native_w=17;
    s_pixels=calloc(17*240,sizeof(*s_pixels));assert(s_pixels);
    HdSprite original={0};original.has=1;original.index=4;original.p0=0xff;
    original.p1=0x11;original.p2=0x12;original.p3=0x13;
    original.hm=original.vm=1;original.ox=3;original.oy=4;
    s_pixels[9*17+9].sp[0]=original;
    for(int i=0;i<6;i++) {
        char text[128];snprintf(text,sizeof text,"4,FF111213,1,1,%X,0F010203",5+i);
        process_addition(text);
    }
    HdPixel *bg=&s_pixels[5*17+5];bg->bg_has=1;bg->bg_index=6;
    bg->bg_p0=15;bg->bg_p1=1;bg->bg_p2=2;bg->bg_p3=3;bg->bg_ox=2;bg->bg_oy=7;
    char bg_rule[]="6,0F010203,1,1,C,FF212223";process_addition(bg_rule);
    assert(hdpack_reuses_pixel_source());
    assert(resolve_sprites());
    HdSprite expected[HD_MAX_SPRITES];memcpy(expected,sprites_at(8*17+8,&s_pixels[8*17+8]),sizeof expected);
    for(int i=0;i<HD_MAX_SPRITES;i++) {
        HdSprite *sp=&expected[i];
        assert(sp->has && sp->index==5+i && sp->transparent && sp->hm && sp->vm);
        assert(sp->ox==3 && sp->oy==4 && sprite_palette(sp)==0x0f010203);
    }
    HdSprite *added=sprites_at(6*17+6,&s_pixels[6*17+6]);
    assert(added[0].has && added[0].index==12 && added[0].transparent);
    assert(added[0].ox==2 && added[0].oy==7 && sprite_palette(added)==0xff212223u);
    assert(!s_pixels[8*17+8].sp[0].has && !memcmp(&s_pixels[9*17+9].sp[0],&original,sizeof original));
    assert(resolve_sprites() && !memcmp(expected,sprites_at(8*17+8,&s_pixels[8*17+8]),sizeof expected));
    uint32_t native[17*240]={0};HdPixel *source=s_pixels;s_pixels=NULL;
    const HdJobFrame *jobs=hdpack_prepare_jobs_from(native,17,probe_pixel_reader,source);
    s_pixels=source;assert(jobs && !s_pixel_reader && !s_pixel_context);
    assert(!memcmp(expected,s_resolved_sprites+(8*17+8)*HD_MAX_SPRITES,sizeof expected));
    added=s_resolved_sprites+(6*17+6)*HD_MAX_SPRITES;
    assert(added[0].has && added[0].index==12 && added[0].ox==2 && added[0].oy==7);
    hdpack_jobs_release(jobs);
    hdpack_unload();
}

static void check_compact_images(void) {
    unsigned w=257,h=259;uint32_t *flat=malloc((size_t)w*h*4);assert(flat);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
        unsigned xx=x%128,yy=y%128,a=(xx+yy)%3?255:128;
        flat[(size_t)y*w+x]=premul((a<<24)|(xx<<16)|(yy<<8)|((xx^yy)&255));
    }
    HdImagePages *pages=compact_image_pages(NULL,flat,w,h);assert(pages && pages->unique_count==4);
    HdJobImage im={flat,w,h,255,NULL};HdJobBackground bg={0,120,26,255,0};
    HdJobPixel *pixels=calloc(17*240,sizeof(*pixels));assert(pixels);
    for(unsigned i=0;i<17*240;i++)pixels[i].base=0xff123456;
    HdJobFrame frame={0};frame.width=17;frame.height=240;frame.source_scale=1;
    frame.images=&im;frame.image_count=1;frame.backgrounds=&bg;
    frame.background_count=frame.under_end=frame.mid_end=1;frame.pixels=pixels;
    frame.pack_generation=UINT64_C(0x7f00000000000000);frame.serial=frame.pack_generation;
    for(unsigned scale=1;scale<=4;scale*=2) {
        size_t count=(size_t)17*240*scale*scale;
        uint32_t *expected=malloc(count*4),*actual=malloc(count*4);assert(expected && actual);
        im.pixels=flat;im.pages=NULL;assert(!hdpack_render_jobs(&frame,scale,expected,17*scale));
        im.pixels=NULL;im.pages=pages;assert(hd_image_storage_valid(&im));
        assert(!hdpack_render_jobs(&frame,scale,actual,17*scale) && !memcmp(expected,actual,count*4));
#ifdef NESRECOMP_HDPACK_GPU_PROBE
        hdpack_gpu_probe_check(&frame,scale,expected,17*scale);
#endif
        free(expected);free(actual);frame.serial++;
    }
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)assert(hd_image_pixel(NULL,pages,w,x,y)==flat[(size_t)y*w+x]);
    HdImagePages invalid=*pages;invalid.columns++;im.pages=&invalid;assert(!hd_image_storage_valid(&im));
    im.pages=pages;image_pages_free(pages);free(pixels);free(flat);
}

static void check_cell_draw_coverage(void) {
    s_active=1;s_is_chr_ram=0;s_version=108;s_scale=2;s_native_w=17;
    s_pixels=calloc(17*240,sizeof(*s_pixels));assert(s_pixels);
    s_opt_debug=s_opt_hide_orig=s_disable_orig=0;
    assert(!hdpack_reuses_pixel_source());
    for(unsigned k=0;k<2;k++) {
        HdTile *t=calloc(1,sizeof(*t));assert(t);
        t->key.tile_index=(int32_t)(4+k);t->key.palette=k?0xff111213u:0x0f010203u;
        t->w=t->h=16;t->brightness=255;t->has_transparent=(int)k;
        t->data=calloc(16*16,4);assert(t->data);
        if(!k)for(unsigned i=0;i<16*16;i++)t->data[i]=0xff0000ffu;
        else for(unsigned y=0;y<2;y++)for(unsigned x=2;x<6;x++)
            t->data[y*16+x]=x<4?0xff00ff00u:(x==4?0x80800000u:0);
        register_tile(t,0);
    }
    uint32_t native[17*240];for(unsigned i=0;i<17*240;i++)native[i]=0xff123456;
    for(unsigned x=0;x<5;x++) {
        HdPixel *p=&s_pixels[x];p->bg_has=1;p->bg_index=4;p->backdrop=0xff123456;
        p->bg_p0=15;p->bg_p1=1;p->bg_p2=2;p->bg_p3=3;
        HdSprite *sp=&p->sp[0];sp->has=1;sp->transparent=1;sp->index=5;
        sp->p0=255;sp->p1=0x11;sp->p2=0x12;sp->p3=0x13;
        sp->ox=(uint8_t)(x<3?x:10-x);sp->hm=x>=3;
    }
    const HdJobFrame *jobs=hdpack_prepare_jobs(native,17);assert(jobs);
    for(unsigned x=0;x<5;x++) {
        unsigned sp=(jobs->pixels[x].flags>>HD_JOB_SP_SHIFT)&HD_JOB_MODE_MASK;
        unsigned bg=(jobs->pixels[x].flags>>HD_JOB_BG_SHIFT)&HD_JOB_MODE_MASK;
        assert(sp==((x==0 || x==3)?HD_JOB_SKIP:HD_JOB_IMAGE));
        assert(bg==((x==1 || x==4)?HD_JOB_SKIP:HD_JOB_IMAGE));
    }
    for(unsigned scale=1;scale<=4;scale*=2) {
        size_t count=(size_t)17*240*scale*scale;uint32_t *out=malloc(count*4);assert(out);
        assert(!hdpack_render_jobs(jobs,scale,out,17*scale));
        assert(out[0]==0xff0000ffu && out[scale]==0xff00ff00u);
        assert(out[3*scale]==0xff0000ffu && out[4*scale]==0xff00ff00u);
        /* A partly covered cell must preserve its opaque background. */
        if(scale>=2)assert(out[3*scale-1]==0xff0000ffu);
        free(out);
    }
    hdpack_jobs_release(jobs);hdpack_unload();
}

static void check_reader_jobs(const uint32_t *native,const uint32_t *expected) {
    HdPixel *source=s_pixels;s_pixels=NULL;reader_calls=0;
    hdpack_invalidate_conditions();
    assert(!hdpack_prepare_jobs_from(native,17,NULL,source));
    assert(!hdpack_prepare_jobs_from(native,-1,probe_pixel_reader,source));
    assert(!s_pixel_reader && !s_pixel_context);
    const HdJobFrame *jobs=hdpack_prepare_jobs_from(native,17,probe_pixel_reader,source);
    assert(jobs && reader_calls>=17*240 && !s_pixel_reader && !s_pixel_context);
    unsigned calls=reader_calls;size_t n=(size_t)17*240*s_scale*s_scale;
    uint32_t *out=malloc(n*4);assert(out);
    assert(!hdpack_render_jobs(jobs,(unsigned)s_scale,out,17*s_scale));
    assert(reader_calls==calls && !memcmp(out,expected,n*4));
    free(out);hdpack_jobs_release(jobs);s_pixels=source;
}

int main(void) {
#ifdef NESRECOMP_HDPACK_CACHE_PROBE
    check_page_cache();
#endif
    for(int k=0;k<3;k++)for(int i=0;i<16;i++)patterns[k][i]=(uint8_t)(k*71+i*29);
    check_addition_resolution();
    check_manifest_priority();
    check_cell_draw_coverage();
    const int scales[]={1,2,3,10};unsigned count=0;
    const HdJobFrame *retained=NULL;uint32_t *retained_pixels=NULL;
    for(int ram=0;ram<2;ram++)for(int ver=0;ver<2;ver++)for(unsigned si=0;si<4;si++) {
        setup(ram,ver?106:99,scales[si]);
        assert(hdpack_reuses_pixel_source());
        uint32_t native[17*240];size_t n=(size_t)17*240*s_scale*s_scale;
        uint32_t *out=malloc(n*4);assert(out);
        for(int q=0;q<12;q++) {
            g_frame_count=(unsigned)q/3;g_ram[0x20]=q%4;
            g_ppuscroll_x=(uint8_t)(q*17);g_ppuscroll_y=(uint8_t)(q*31);g_ppuctrl=q&3;
            s_disable_orig=q&1;s_opt_debug=q%5==0;s_opt_hide_orig=q%3==0;
            for(int y=0;y<240;y++)for(int x=0;x<17;x++) {
                int i=y*17+x,k=(x/5+y/7+q)%3;HdPixel *p=&s_pixels[i];
                memset(p,0,sizeof(*p));native[i]=0xff402010u+(unsigned)i;
                p->bg_has=(x+y+q)%11!=0;p->sp[0].has=(x*3+y+q)%5==0;
                p->bg_index=p->sp[0].index=k+4;p->bg_t16=p->sp[0].t16=patterns[k];
                p->bg_p0=(q%4==2)?0x20:0x0f;p->bg_p1=1;p->bg_p2=2;p->bg_p3=3;
                p->sp[0].p0=0xff;p->sp[0].p1=0x11;p->sp[0].p2=0x12;p->sp[0].p3=0x13;
                p->bg_ox=p->sp[0].ox=x&7;p->bg_oy=p->sp[0].oy=y&7;
                p->sp[0].hm=(q+x)&1;p->sp[0].vm=(q+y)&1;
                p->backdrop=0xff112233;p->bg_argb=0xff678901;p->sp[0].argb=0xffabcdef;
                p->sp[0].transparent=(q+x)%3==0;p->sp[0].behind=(q+y)%7==0;
                for(unsigned z=1;z<HD_MAX_SPRITES;z++)if(p->sp[0].has && (q+x+y+z)%3) {
                    p->sp[z]=p->sp[0];unsigned next=(k+z)%3;
                    p->sp[z].index=next+4;p->sp[z].t16=patterns[next];
                    p->sp[z].behind=(q+y+z)%3==0;p->sp[z].transparent=z&1;
                    p->sp[z].hm^=z&1;p->sp[z].vm^=(z>>1)&1;
                }
            }
            hdpack_invalidate_conditions();
            s_groups_ready=q%7!=0; /* allocation-failure reference fallback */
            s_use_render_plan=q%4!=0; /* simple-pack compositor selection */
            if(q==10)s_plan_epoch=UINT64_MAX;
            int saved_backgrounds=s_bg_count;if(q==11)s_bg_count=0;
            hdpack_upscale(native,17,out);
            check_reader_jobs(native,out);
#if !(defined(NESRECOMP_HDPACK_PLAN_REFERENCE) && NESRECOMP_HDPACK_PLAN_REFERENCE)
            if(q==0 || (ram==0 && ver==1 && si==2 && q==2)) {
                const HdJobFrame *jobs=hdpack_prepare_jobs(native,17);assert(jobs);
                assert(sizeof(HdJobPixel)==8*sizeof(uint32_t));
                assert(s_bgimgs[1]->job_image==HD_JOB_NO_IMAGE);
                assert(jobs->image_count==(uint32_t)s_all_count+3);
                assert(s_bgimgs[3]->job_image==(uint32_t)s_all_count+2);
                if(q==0 && retained && count!=0) {
                    assert(jobs->pack_generation!=retained->pack_generation);
                    check_retained_jobs(retained,retained_pixels);
                    hdpack_jobs_release(retained);retained=NULL;
                    free(retained_pixels);retained_pixels=NULL;
                }
                if(count==0) {
                    retained_pixels=malloc(n*4);assert(retained_pixels);
                    memcpy(retained_pixels,out,n*4);retained=jobs;
                    hdpack_jobs_retain(retained);
                }
                if(ram==0 && ver==1 && si==2 && q==2) check_job_scales(jobs,out);
                hdpack_jobs_release(jobs);
            }
            if(q==1 && retained) check_retained_jobs(retained,retained_pixels);
#endif
#ifdef NESRECOMP_HDPACK_GPU_PROBE
            const HdJobFrame *gpu_jobs=hdpack_prepare_jobs(native,17);assert(gpu_jobs);
            hdpack_gpu_probe_check(gpu_jobs,(unsigned)s_scale,out,(size_t)17*s_scale);
            hdpack_jobs_release(gpu_jobs);
#endif
            uint64_t hash=UINT64_C(1469598103934665603);
            for(size_t i=0;i<n;i++)for(int j=0;j<4;j++)hash=(hash^(uint8_t)(out[i]>>(j*8)))*UINT64_C(1099511628211);
            printf("case %u %016llx\n",count++,(unsigned long long)hash);
            s_bg_count=saved_backgrounds;
        }
        free(out);hdpack_unload();
        if(retained) check_retained_jobs(retained,retained_pixels);
    }
    assert(!retained && !retained_pixels);
    assert(count==192);check_compact_images();return 0;
}
