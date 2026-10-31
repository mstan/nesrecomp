/* GPU consumer hook for the existing HD-plan corpus. Readbacks are explicit QA
 * only. Ordinary runtime presentation has no output-sized CPU image. */
#define SDL_MAIN_HANDLED
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <SDL.h>
#include "cyc_hd_gpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *activation;
static CycHdGpu *gpu;
static unsigned cases;
static bool sdl_ready,shutdown_registered;
static void destroy_video(void){
    if(gpu){cyc_hd_gpu_destroy(gpu,false);gpu=NULL;}
    if(activation){SDL_DestroyTexture(activation);activation=NULL;}
    if(renderer){SDL_DestroyRenderer(renderer);renderer=NULL;}
    if(window){SDL_DestroyWindow(window);window=NULL;}
}
static void shutdown_probe(void){
    destroy_video();
    if(sdl_ready){SDL_Quit();sdl_ready=false;}
}
static void initialize(void){
    if(!sdl_ready){
        SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO))exit(77);sdl_ready=true;
    }
    if(!shutdown_registered){assert(!atexit(shutdown_probe));shutdown_registered=true;}
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    window=SDL_CreateWindow("HD job contract",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,64,64,SDL_WINDOW_HIDDEN|SDL_WINDOW_OPENGL);
    int index=-1;
    for(int i=0;i<SDL_GetNumRenderDrivers();++i){SDL_RendererInfo info={0};
        if(!SDL_GetRenderDriverInfo(i,&info) && !strcmp(info.name,"opengl")){index=i;break;}}
    if(window && index>=0)renderer=SDL_CreateRenderer(window,index,SDL_RENDERER_ACCELERATED|SDL_RENDERER_TARGETTEXTURE);
    if(!renderer){fprintf(stderr,"OpenGL fixture unavailable: %s\n",SDL_GetError());exit(77);}
    activation=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,1,1);
    gpu=cyc_hd_gpu_create(renderer,activation);
    if(!gpu){fprintf(stderr,"HD GPU fixture unavailable\n");exit(77);}
}
static void check(const HdJobFrame *f,unsigned scale,const uint32_t *expected,size_t pitch){
    unsigned width=f->width*scale,height=f->height*scale;
    SDL_Texture *output=cyc_hd_gpu_render(gpu,f,scale,0);assert(output);
    assert(cyc_hd_gpu_width(gpu)==width && cyc_hd_gpu_height(gpu)==height);
    uint32_t *actual=(uint32_t *)malloc((size_t)width*height*4);assert(actual);
    assert(cyc_hd_gpu_readback(gpu,actual,(size_t)width*4));
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        if(actual[(size_t)y*width+x]!=expected[(size_t)y*pitch+x]){
            fprintf(stderr,"GPU mismatch case=%u scale=%u x=%u y=%u actual=%08x expected=%08x\n",
                cases,scale,x,y,actual[(size_t)y*width+x],expected[(size_t)y*pitch+x]);abort();}}
    free(actual);
    /* Target textures have SDL's own orientation rules. Compare the actual
     * window copy with a normal CPU streaming texture too, then draw again. */
    SDL_Texture *reference=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,(int)width,(int)height);
    assert(reference);SDL_SetTextureBlendMode(reference,SDL_BLENDMODE_NONE);
    assert(!SDL_UpdateTexture(reference,NULL,expected,(int)(pitch*4)));
    assert(SDL_GetRenderTarget(renderer)==NULL);
    int window_w=0,window_h=0;assert(!SDL_GetRendererOutputSize(renderer,&window_w,&window_h));
    assert(window_w>0 && window_h>0 && (size_t)window_w<=SIZE_MAX/(size_t)window_h/4);
    size_t window_bytes=(size_t)window_w*(size_t)window_h*4;
    uint32_t *cpu_window=(uint32_t *)malloc(window_bytes),*gpu_window=(uint32_t *)malloc(window_bytes);
    assert(cpu_window && gpu_window);
    SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer);SDL_RenderCopy(renderer,reference,NULL,NULL);
    assert(!SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ARGB8888,cpu_window,window_w*4));
    SDL_RenderClear(renderer);SDL_RenderCopy(renderer,output,NULL,NULL);
    assert(!SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ARGB8888,gpu_window,window_w*4));
    assert(!memcmp(cpu_window,gpu_window,window_bytes));
    free(cpu_window);free(gpu_window);SDL_DestroyTexture(reference);
}
static void resize_probe(int width,int height){
    int before_w=0,before_h=0;assert(!SDL_GetRendererOutputSize(renderer,&before_w,&before_h));
    SDL_SetWindowSize(window,width,height);
    SDL_Event event;while(SDL_PollEvent(&event)){}
    int actual_w=0,actual_h=0;SDL_GetWindowSize(window,&actual_w,&actual_h);
    assert(actual_w==width && actual_h==height);
    int output_w=0,output_h=0;assert(!SDL_GetRendererOutputSize(renderer,&output_w,&output_h));
    assert(output_w>0 && output_h>0 && (output_w!=before_w || output_h!=before_h));
}
static void check_lifecycle(const HdJobFrame *f,unsigned scale,const uint32_t *expected,size_t pitch){
    hdpack_jobs_retain(f);
    uint64_t serial=f->serial,generation=f->pack_generation;
    const HdJobPixel *pixels=f->pixels;const HdJobImage *images=f->images;
    resize_probe(109,77);check(f,scale,expected,pitch);
    cyc_hd_gpu_destroy(gpu,false);gpu=NULL;
    gpu=cyc_hd_gpu_create(renderer,activation);assert(gpu);
    check(f,scale,expected,pitch);
    resize_probe(64,64);check(f,scale,expected,pitch);
    destroy_video();destroy_video(); /* idempotent teardown, SDL stays initialized */
    initialize();check(f,scale,expected,pitch);
    assert(f->serial==serial && f->pack_generation==generation && f->pixels==pixels && f->images==images);
    hdpack_jobs_release(f);
    fprintf(stderr,"GPU lifecycle: resize, GPU recreation and renderer/window recreation with retained frame passed\n");
}
void hdpack_gpu_probe_check(const HdJobFrame *f,unsigned scale,const uint32_t *expected,size_t pitch){
    if(!gpu)initialize();
    check(f,scale,expected,pitch);
    if(cases==0)check_lifecycle(f,scale,expected,pitch);
    if(cases==24){
        HdJobFrame wide=*f;wide.width=256;wide.serial+=UINT64_C(1)<<60;
        HdJobPixel *pixels=(HdJobPixel *)malloc((size_t)wide.width*wide.height*sizeof(*pixels));assert(pixels);
        for(unsigned y=0;y<wide.height;++y)for(unsigned x=0;x<wide.width;++x)pixels[(size_t)y*wide.width+x]=f->pixels[(size_t)y*f->width+x%f->width];
        wide.pixels=pixels;size_t count=(size_t)wide.width*wide.height*16*16;
        uint32_t *cpu=(uint32_t *)malloc(count*4);assert(cpu);
        assert(!hdpack_render_jobs(&wide,16,cpu,(size_t)wide.width*16));check(&wide,16,cpu,(size_t)wide.width*16);
        free(cpu);free(pixels);fprintf(stderr,"GPU exact full16x target4096x3840 passed\n");
    }
    ++cases;
}
