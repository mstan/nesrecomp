/* Compare the production plan with the retained independent pixel compositor.
 * No assets or game ROM required. Assertions stay active in release builds. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define NESRECOMP_GAME_NO_HDPACK 1
#define NESRECOMP_HDPACK_LAYER_PLANS 1
#include "../../src/hdpack.c"

uint8_t g_ram[0x800], g_ppuscroll_x, g_ppuscroll_y, g_ppuctrl;
uint64_t g_frame_count;
unsigned char *stbi_load(const char *p,int *w,int *h,int *c,int n) {
    (void)p;(void)w;(void)h;(void)c;(void)n;return NULL;
}
void stbi_image_free(void *p) { free(p); }
const char *stbi_failure_reason(void) { return "no file I/O in this fixture"; }

static uint8_t patterns[3][16];
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
        tile(k,pal,1,0,NULL,NULL,0);tile(k,pal,2,0,pos,NULL,0);
        tile(k,pal,3,0,mem,NULL,0);tile(k,pal,4,0,origin,mem,0);
        tile(k,pal,5,0,near,at,0);tile(k,pal,6,0,pos,mem,1);
        int sp=version<100?0x111213:(int)0xff111213u;
        tile(k,sp,7,0,flip,NULL,0);tile(k,sp,8,0,NULL,NULL,0);
        tile(k,sp,9,0,pos,mem,0);
    }
    /* Wildcard fallback intentionally keeps the reference's alias rule even
     * when its defining tile's conditional gate fails. */
    tile(0,0x12345678,10,1,mem,NULL,1);
    s_bg_count=6;s_bgs=calloc(6,sizeof(*s_bgs));
    s_bg_sorted=calloc(6,sizeof(*s_bg_sorted));s_actbg=calloc(6,sizeof(*s_actbg));
    s_bgimg_count=3;s_bgimgs=calloc(3,sizeof(*s_bgimgs));
    assert(s_bgs&&s_bg_sorted&&s_actbg&&s_bgimgs);
    for(int i=0;i<3;i++) {
        HdBgImg *im=calloc(1,sizeof(*im));assert(im);s_bgimgs[i]=im;
        im->w=(17+9)*scale;im->h=(32+i*109)*scale;
        im->px=malloc((size_t)im->w*im->h*4);assert(im->px);
        for(int y=0;y<im->h;y++)for(int x=0;x<im->w;x++) {
            unsigned a=(x+y+i)%7==0?0:((x^y)&1)?64:255;
            im->px[y*im->w+x]=premul((a<<24)|((unsigned)(x*9+i*51)&255)<<16|
                ((unsigned)(y*7+i*31)&255)<<8|((unsigned)(x+y*3)&255));
        }
    }
    static const int priorities[]={0,19,20,29,30,39};
    for(int i=0;i<6;i++) {
        HdBg *b=&s_bgs[i];s_bg_sorted[i]=b;b->img=s_bgimgs[i%3];
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

int main(void) {
    for(int k=0;k<3;k++)for(int i=0;i<16;i++)patterns[k][i]=(uint8_t)(k*71+i*29);
    const int scales[]={1,2,3,10};unsigned count=0;
    for(int ram=0;ram<2;ram++)for(int ver=0;ver<2;ver++)for(unsigned si=0;si<4;si++) {
        setup(ram,ver?106:99,scales[si]);
        uint32_t native[17*240];size_t n=(size_t)17*240*s_scale*s_scale;
        uint32_t *out=malloc(n*4);assert(out);
        for(int q=0;q<12;q++) {
            g_frame_count=(unsigned)q/3;g_ram[0x20]=q%4;
            g_ppuscroll_x=(uint8_t)(q*17);g_ppuscroll_y=(uint8_t)(q*31);g_ppuctrl=q&3;
            s_disable_orig=q&1;s_opt_debug=q%5==0;s_opt_hide_orig=q%3==0;
            for(int y=0;y<240;y++)for(int x=0;x<17;x++) {
                int i=y*17+x,k=(x/5+y/7+q)%3;HdPixel *p=&s_pixels[i];
                memset(p,0,sizeof(*p));native[i]=0xff402010u+(unsigned)i;
                p->bg_has=(x+y+q)%11!=0;p->sp_has=(x*3+y+q)%5==0;
                p->bg_index=p->sp_index=k+4;p->bg_t16=p->sp_t16=patterns[k];
                p->bg_p0=(q%4==2)?0x20:0x0f;p->bg_p1=1;p->bg_p2=2;p->bg_p3=3;
                p->sp_p1=0x11;p->sp_p2=0x12;p->sp_p3=0x13;
                p->bg_ox=p->sp_ox=x&7;p->bg_oy=p->sp_oy=y&7;
                p->sp_hm=(q+x)&1;p->sp_vm=(q+y)&1;
                p->backdrop=0xff112233;p->bg_argb=0xff678901;p->sp_argb=0xffabcdef;
            }
            hdpack_invalidate_conditions();
            s_groups_ready=q%7!=0; /* allocation-failure reference fallback */
            s_use_render_plan=q%4!=0; /* simple-pack compositor selection */
            if(q==10)s_plan_epoch=UINT64_MAX;
            hdpack_upscale(native,17,out);
            uint64_t hash=UINT64_C(1469598103934665603);
            for(size_t i=0;i<n;i++)for(int j=0;j<4;j++)hash=(hash^(uint8_t)(out[i]>>(j*8)))*UINT64_C(1099511628211);
            printf("case %u %016llx\n",count++,(unsigned long long)hash);
        }
        free(out);hdpack_unload();
    }
    assert(count==192);return 0;
}
