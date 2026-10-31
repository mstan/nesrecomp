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
static void shutdown_probe(void){
    cyc_hd_gpu_destroy(gpu,false);SDL_DestroyTexture(activation);
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();
}
static void initialize(void){
    SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO))exit(77);
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
    if(!gpu){fprintf(stderr,"HD GPU fixture unavailable\n");exit(77);}atexit(shutdown_probe);
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
    uint32_t cpu_window[64*64],gpu_window[64*64];
    SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer);SDL_RenderCopy(renderer,reference,NULL,NULL);
    assert(!SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ARGB8888,cpu_window,64*4));
    SDL_RenderClear(renderer);SDL_RenderCopy(renderer,output,NULL,NULL);
    assert(!SDL_RenderReadPixels(renderer,NULL,SDL_PIXELFORMAT_ARGB8888,gpu_window,64*4));
    assert(!memcmp(cpu_window,gpu_window,sizeof(cpu_window)));SDL_DestroyTexture(reference);
}
void hdpack_gpu_probe_check(const HdJobFrame *f,unsigned scale,const uint32_t *expected,size_t pitch){
    if(!gpu)initialize();
    check(f,scale,expected,pitch);
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
