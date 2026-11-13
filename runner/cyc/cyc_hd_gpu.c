/* Resident immutable HD jobs. SDL/GL interop follows the in-house GBA backend
 * (ba32c62); only this engine's published jobs and integer blends are used.
 * Unsupported contexts keep the portable consumer under the same contract. */
#include "cyc_hd_gpu.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CYC_WITH_SDL) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
#include <SDL.h>
#if SDL_VERSION_ATLEAST(2, 0, 10)
#define CYC_HD_GL 1
#include <SDL_opengl.h>
#include "cyc_hd_gpu_shader.h"
#include "cyc_hd_gpu_cache.h"
#endif
#endif

#ifdef CYC_HD_GL
#define HD_GL_FUNCTIONS(X) \
 X(const GLubyte *,GetString,(GLenum)) \
 X(void,GetIntegerv,(GLenum,GLint *)) X(void,GetBooleanv,(GLenum,GLboolean *)) \
 X(GLboolean,IsEnabled,(GLenum)) X(GLenum,GetError,(void)) \
 X(void,Enable,(GLenum)) X(void,Disable,(GLenum)) \
 X(void,Viewport,(GLint,GLint,GLsizei,GLsizei)) X(void,Scissor,(GLint,GLint,GLsizei,GLsizei)) \
 X(void,ColorMask,(GLboolean,GLboolean,GLboolean,GLboolean)) X(void,PixelStorei,(GLenum,GLint)) \
 X(void,ActiveTexture,(GLenum)) X(void,GenTextures,(GLsizei,GLuint *)) \
 X(void,DeleteTextures,(GLsizei,const GLuint *)) X(void,BindTexture,(GLenum,GLuint)) \
 X(void,TexParameteri,(GLenum,GLenum,GLint)) \
 X(void,TexImage2D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void *)) \
 X(void,TexSubImage2D,(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void *)) \
 X(void,TexImage3D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void *)) \
 X(void,TexSubImage3D,(GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei,GLenum,GLenum,const void *)) \
 X(void,TexBuffer,(GLenum,GLenum,GLuint)) X(void,GenBuffers,(GLsizei,GLuint *)) \
 X(void,DeleteBuffers,(GLsizei,const GLuint *)) X(void,BindBuffer,(GLenum,GLuint)) \
 X(void,BufferData,(GLenum,GLsizeiptr,const void *,GLenum)) \
 X(void,BufferSubData,(GLenum,GLintptr,GLsizeiptr,const void *)) \
 X(GLuint,CreateShader,(GLenum)) X(void,ShaderSource,(GLuint,GLsizei,const GLchar *const *,const GLint *)) \
 X(void,CompileShader,(GLuint)) X(void,GetShaderiv,(GLuint,GLenum,GLint *)) \
 X(void,GetShaderInfoLog,(GLuint,GLsizei,GLsizei *,GLchar *)) X(void,DeleteShader,(GLuint)) \
 X(GLuint,CreateProgram,(void)) X(void,AttachShader,(GLuint,GLuint)) X(void,LinkProgram,(GLuint)) \
 X(void,BindFragDataLocation,(GLuint,GLuint,const GLchar *)) X(void,GetProgramiv,(GLuint,GLenum,GLint *)) \
 X(void,GetProgramInfoLog,(GLuint,GLsizei,GLsizei *,GLchar *)) X(void,UseProgram,(GLuint)) \
 X(void,DeleteProgram,(GLuint)) X(GLint,GetUniformLocation,(GLuint,const GLchar *)) \
 X(void,Uniform1i,(GLint,GLint)) X(void,GenVertexArrays,(GLsizei,GLuint *)) \
 X(void,DeleteVertexArrays,(GLsizei,const GLuint *)) X(void,BindVertexArray,(GLuint)) \
 X(void,DrawArrays,(GLenum,GLint,GLsizei)) X(void,Finish,(void))
typedef struct {
#define HD_DECLARE(ret,name,args) ret (APIENTRY *name) args;
    HD_GL_FUNCTIONS(HD_DECLARE)
#undef HD_DECLARE
    void (APIENTRY *GenQueries)(GLsizei,GLuint *);
    void (APIENTRY *DeleteQueries)(GLsizei,const GLuint *);
    void (APIENTRY *BeginQuery)(GLenum,GLuint);
    void (APIENTRY *EndQuery)(GLenum);
    void (APIENTRY *GetQueryObjectiv)(GLuint,GLenum,GLint *);
    void (APIENTRY *GetQueryObjectui64v)(GLuint,GLenum,GLuint64 *);
} HdGl;
typedef struct {
    GLint program,active,vao,array_buffer,unpack_buffer,viewport[4],scissor[4];
    GLint textures[5],buffer_textures[5],array_textures[5],stores[8];
    GLboolean mask[4],enabled[8];
} HdGlState;
static const GLenum switches[]={GL_BLEND,GL_SCISSOR_TEST,GL_DEPTH_TEST,GL_STENCIL_TEST,
    GL_CULL_FACE,GL_DITHER,GL_FRAMEBUFFER_SRGB,GL_RASTERIZER_DISCARD};
static const GLenum stores[]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_PIXELS,
    GL_UNPACK_SKIP_ROWS,GL_UNPACK_SWAP_BYTES,GL_UNPACK_LSB_FIRST,GL_UNPACK_IMAGE_HEIGHT,GL_UNPACK_SKIP_IMAGES};
typedef struct {
    GLuint queries[2]; bool pending,composite,present;
    uint64_t serial,prepare_ns,submit_ns,upload_bytes,resident_bytes;
    unsigned scale,width,height;
} HdQuery;
struct CycHdGpu {
    HdGl gl; SDL_Renderer *renderer; SDL_GLContext context; SDL_Texture *output;
    GLuint program,vao,textures[5],buffers[2];
    unsigned width,height,requested_scale,max_texture,max_buffer,plane_w,plane_h,bg_w,bg_h;
    uint64_t pack_generation,serial,asset_bytes,budget,frames,attempts;
    unsigned cache_slots;
    HdGpuPageCache cache;
    bool failed,profile; unsigned query_cursor,profile_lines; HdQuery query[4];
};
static uint64_t nanoseconds(uint64_t begin) {
    return (uint64_t)((SDL_GetPerformanceCounter()-begin)*1000000000.0/SDL_GetPerformanceFrequency());
}
static bool load_gl(HdGl *g) {
#define HD_LOAD(ret,name,args) g->name=(ret(APIENTRY *)args)SDL_GL_GetProcAddress("gl" #name);if(!g->name)return false;
    HD_GL_FUNCTIONS(HD_LOAD)
#undef HD_LOAD
#define HD_OPTIONAL(name) *(void **)(&g->name)=SDL_GL_GetProcAddress("gl" #name)
    HD_OPTIONAL(GenQueries); HD_OPTIONAL(DeleteQueries); HD_OPTIONAL(BeginQuery);
    HD_OPTIONAL(EndQuery); HD_OPTIONAL(GetQueryObjectiv); HD_OPTIONAL(GetQueryObjectui64v);
#undef HD_OPTIONAL
    return true;
}
static void state_take(HdGl *g,HdGlState *s) {
    g->GetIntegerv(GL_CURRENT_PROGRAM,&s->program); g->GetIntegerv(GL_ACTIVE_TEXTURE,&s->active);
    g->GetIntegerv(GL_VERTEX_ARRAY_BINDING,&s->vao);g->GetIntegerv(GL_ARRAY_BUFFER_BINDING,&s->array_buffer);
    g->GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&s->unpack_buffer);
    g->GetIntegerv(GL_VIEWPORT,s->viewport);g->GetIntegerv(GL_SCISSOR_BOX,s->scissor);
    g->GetBooleanv(GL_COLOR_WRITEMASK,s->mask);
    for(unsigned i=0;i<8;++i){s->enabled[i]=g->IsEnabled(switches[i]);g->Disable(switches[i]);}
    for(unsigned i=0;i<8;++i)g->GetIntegerv(stores[i],&s->stores[i]);
    for(unsigned i=0;i<5;++i){g->ActiveTexture(GL_TEXTURE0+i);g->GetIntegerv(GL_TEXTURE_BINDING_2D,&s->textures[i]);
        g->GetIntegerv(GL_TEXTURE_BINDING_BUFFER,&s->buffer_textures[i]);
        g->GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY,&s->array_textures[i]);}
    g->BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    for(unsigned i=0;i<8;++i)g->PixelStorei(stores[i],i==0?1:0);
    g->ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
}
static void state_restore(HdGl *g,const HdGlState *s) {
    g->UseProgram((GLuint)s->program);g->BindVertexArray((GLuint)s->vao);
    g->BindBuffer(GL_ARRAY_BUFFER,(GLuint)s->array_buffer);g->BindBuffer(GL_PIXEL_UNPACK_BUFFER,(GLuint)s->unpack_buffer);
    for(unsigned i=0;i<8;++i)g->PixelStorei(stores[i],s->stores[i]);
    for(unsigned i=0;i<5;++i){g->ActiveTexture(GL_TEXTURE0+i);g->BindTexture(GL_TEXTURE_2D,(GLuint)s->textures[i]);
        g->BindTexture(GL_TEXTURE_BUFFER,(GLuint)s->buffer_textures[i]);
        g->BindTexture(GL_TEXTURE_2D_ARRAY,(GLuint)s->array_textures[i]);}
    g->ActiveTexture((GLenum)s->active);g->Viewport(s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3]);
    g->Scissor(s->scissor[0],s->scissor[1],s->scissor[2],s->scissor[3]);
    g->ColorMask(s->mask[0],s->mask[1],s->mask[2],s->mask[3]);
    for(unsigned i=0;i<8;++i){if(s->enabled[i])g->Enable(switches[i]);else g->Disable(switches[i]);}
}
static GLuint shader(HdGl *g,GLenum kind,const char *source) {
    GLuint sh=g->CreateShader(kind);g->ShaderSource(sh,1,&source,NULL);g->CompileShader(sh);
    GLint good=0;g->GetShaderiv(sh,GL_COMPILE_STATUS,&good);
    if(!good){char log[4096]={0};g->GetShaderInfoLog(sh,sizeof(log),NULL,log);
        fprintf(stderr,"[HD GPU] shader: %s\n",log);g->DeleteShader(sh);return 0;}return sh;
}
static void texture(CycHdGpu *s,unsigned unit,GLenum target) {
    s->gl.ActiveTexture(GL_TEXTURE0+unit);s->gl.BindTexture(target,s->textures[unit]);
}
static void image(CycHdGpu *s,unsigned unit,GLint format,unsigned w,unsigned h,GLenum type,const void *data) {
    texture(s,unit,GL_TEXTURE_2D);
    s->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    s->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    s->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    s->gl.TexImage2D(GL_TEXTURE_2D,0,format,(GLsizei)w,(GLsizei)h,0,GL_RGBA_INTEGER,type,data);
}
static bool frame_valid(CycHdGpu *s,const HdJobFrame *f,uint64_t *asset_bytes) {
    if(!f || !f->pixels || (!f->images && f->image_count) || !f->width || f->width>1280 || f->height!=240 ||
        !f->source_scale || f->source_scale>64 || f->width*2>s->max_texture ||
        f->behind_end>f->under_end || f->has_behind>1 || f->under_end>f->mid_end || f->mid_end>f->background_count || (uint64_t)f->background_count*2+f->sprite_count>(uint64_t)s->max_texture*s->max_texture ||
        (uint64_t)f->sprite_count>(uint64_t)f->width*f->height*3 ||
        (f->background_count && !f->backgrounds))return false;
    for(unsigned i=0;i<f->background_count;++i){const HdJobBackground *bg=&f->backgrounds[i];
        if(bg->offset_x<-(1<<20) || bg->offset_x>(1<<20) || bg->offset_y<-(1<<20) || bg->offset_y>(1<<20))return false;
    }
    if(!cyc_hd_gpu_cache_prepare(&s->cache,f,s->cache_slots,s->max_buffer))return false;
    *asset_bytes=(uint64_t)s->cache_slots*HD_GPU_PAGE_SIZE*HD_GPU_PAGE_SIZE*4+
        (uint64_t)(s->cache.image_count?s->cache.image_count:1)*16+
        (uint64_t)(s->cache.page_count?s->cache.page_count:1)*4;
    return *asset_bytes<s->budget;
}
static bool upload_assets(CycHdGpu *s,const HdJobFrame *f,uint64_t bytes,uint64_t *uploaded) {
    HdGl *g=&s->gl;HdGpuPageCache *cache=&s->cache;
    bool new_pack=s->pack_generation!=f->pack_generation,dirty=new_pack;
    *uploaded=0;
    if(new_pack){
        const uint32_t zero[4]={0};size_t count=cache->image_count?cache->image_count:1;
        g->BindBuffer(GL_ARRAY_BUFFER,s->buffers[0]);
        g->BufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(count*16),cache->image_count?cache->image_meta:zero,GL_STATIC_DRAW);
        texture(s,2,GL_TEXTURE_BUFFER);g->TexBuffer(GL_TEXTURE_BUFFER,GL_RGBA32UI,s->buffers[0]);
        *uploaded+=count*16;
    }
    for(unsigned i=0;i<cache->needed_count;++i){
        uint32_t page=cache->needed_ids[i],slot=0;bool upload=false;
        if(!cyc_hd_gpu_cache_reserve(cache,page,&slot,&upload))return false;
        if(!upload)continue;
        uint32_t image_id=cache->page_image[page];const HdJobImage *im=&f->images[image_id];
        uint32_t local=page-cache->image_meta[(size_t)image_id*4];
        uint32_t cols=(im->width+HD_GPU_PAGE_SIZE-1)/HD_GPU_PAGE_SIZE;
        uint32_t x=(local%cols)*HD_GPU_PAGE_SIZE,y=(local/cols)*HD_GPU_PAGE_SIZE;
        uint32_t w=im->width-x,h=im->height-y;
        if(w>HD_GPU_PAGE_SIZE)w=HD_GPU_PAGE_SIZE;
        if(h>HD_GPU_PAGE_SIZE)h=HD_GPU_PAGE_SIZE;
        texture(s,1,GL_TEXTURE_2D_ARRAY);g->PixelStorei(GL_UNPACK_ROW_LENGTH,(GLint)im->width);
        g->TexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,(GLint)slot,(GLsizei)w,(GLsizei)h,1,
            GL_RED_INTEGER,GL_UNSIGNED_INT,im->pixels+(size_t)y*im->width+x);
        *uploaded+=(uint64_t)w*h*4;dirty=true;
    }
    g->PixelStorei(GL_UNPACK_ROW_LENGTH,0);
    if(dirty){
        uint32_t absent=HD_GPU_PAGE_ABSENT;size_t count=cache->page_count?cache->page_count:1;
        g->BindBuffer(GL_ARRAY_BUFFER,s->buffers[1]);
        if(new_pack)g->BufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(count*4),cache->page_count?cache->page_slots:&absent,GL_DYNAMIC_DRAW);
        else g->BufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)(count*4),cache->page_slots);
        texture(s,4,GL_TEXTURE_BUFFER);g->TexBuffer(GL_TEXTURE_BUFFER,GL_R32UI,s->buffers[1]);
        *uploaded+=count*4;
    }
    s->pack_generation=f->pack_generation;s->asset_bytes=bytes;return true;
}
static void query_collect(CycHdGpu *s,HdQuery *q) {
    if(!q->pending || !q->composite || !q->present)return;
    GLint ready=0;s->gl.GetQueryObjectiv(q->queries[1],GL_QUERY_RESULT_AVAILABLE,&ready);if(!ready)return;
    GLuint64 composite=0,present=0;s->gl.GetQueryObjectui64v(q->queries[0],GL_QUERY_RESULT,&composite);
    s->gl.GetQueryObjectui64v(q->queries[1],GL_QUERY_RESULT,&present);
    if(s->profile_lines++<1200){fflush(stdout);fprintf(stderr,
        "hd_gpu_sample {\"serial\":%llu,\"scale\":%u,\"width\":%u,\"height\":%u,\"prepare_ns\":%llu,\"submit_ns\":%llu,\"composite_ns\":%llu,\"present_ns\":%llu,\"upload_bytes\":%llu,\"resident_bytes\":%llu}\n",
        (unsigned long long)q->serial,q->scale,q->width,q->height,(unsigned long long)q->prepare_ns,
        (unsigned long long)q->submit_ns,(unsigned long long)composite,(unsigned long long)present,
        (unsigned long long)q->upload_bytes,(unsigned long long)q->resident_bytes);}
    q->pending=false;
}
CycHdGpu *cyc_hd_gpu_create(SDL_Renderer *renderer,SDL_Texture *activation) {
    const uint32_t little_endian=1;
    if(*(const unsigned char *)&little_endian!=1)return NULL;
    SDL_RendererInfo info={0};
    if(!renderer || !activation || SDL_GetRendererInfo(renderer,&info) || !info.name || strcmp(info.name,"opengl") ||
        !(info.flags&SDL_RENDERER_TARGETTEXTURE) || SDL_GL_BindTexture(activation,NULL,NULL))return NULL;
    SDL_GL_UnbindTexture(activation);
    CycHdGpu *s=(CycHdGpu *)calloc(1,sizeof(*s));if(!s)return NULL;
    if(!load_gl(&s->gl)){free(s);return NULL;}
    s->renderer=renderer;s->context=SDL_GL_GetCurrentContext();HdGl *g=&s->gl;
    const char *version=(const char *)g->GetString(GL_VERSION);int major=0,minor=0;
    if(!version || sscanf(version,"%d.%d",&major,&minor)!=2 || major<3 || (major==3 && minor<1)){free(s);return NULL;}
    GLint units=0,limit=0,buffer_limit=0,layers=0;g->GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&units);
    g->GetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);g->GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE,&buffer_limit);
    g->GetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS,&layers);
    if(units<5 || limit<1024 || buffer_limit<65536 || layers<16){free(s);return NULL;}
    s->max_texture=(unsigned)limit;s->max_buffer=(unsigned)buffer_limit;s->budget=256ull*1024*1024;
    const char *extensions=(const char *)g->GetString(GL_EXTENSIONS);
    if(extensions && strstr(extensions,"GL_NVX_gpu_memory_info")){
        GLint available=0;g->GetIntegerv(0x9049,&available);
        if(available>0){uint64_t budget=(uint64_t)available*1024/4;
            if(budget>1536ull*1024*1024)budget=1536ull*1024*1024;
            s->budget=budget;}
    }
    GLuint vs=shader(g,GL_VERTEX_SHADER,hd_gpu_vertex),fs=shader(g,GL_FRAGMENT_SHADER,hd_gpu_fragment);
    if(!vs || !fs){if(vs)g->DeleteShader(vs);if(fs)g->DeleteShader(fs);cyc_hd_gpu_destroy(s,false);return NULL;}
    s->program=g->CreateProgram();g->AttachShader(s->program,vs);g->AttachShader(s->program,fs);
    g->BindFragDataLocation(s->program,0,"result");g->LinkProgram(s->program);g->DeleteShader(vs);g->DeleteShader(fs);
    GLint linked=0;g->GetProgramiv(s->program,GL_LINK_STATUS,&linked);
    if(!linked){char log[4096]={0};g->GetProgramInfoLog(s->program,sizeof(log),NULL,log);
        fprintf(stderr,"[HD GPU] link: %s\n",log);cyc_hd_gpu_destroy(s,false);return NULL;}
    g->GenTextures(5,s->textures);g->GenBuffers(2,s->buffers);g->GenVertexArrays(1,&s->vao);
    s->cache_slots=(unsigned)layers<HD_GPU_PAGE_MAX_SLOTS?(unsigned)layers:HD_GPU_PAGE_MAX_SLOTS;
    while(s->cache_slots>16 && (uint64_t)s->cache_slots*HD_GPU_PAGE_SIZE*HD_GPU_PAGE_SIZE*4>s->budget/4)s->cache_slots/=2;
    HdGlState restore;state_take(g,&restore);texture(s,1,GL_TEXTURE_2D_ARRAY);
    g->TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    g->TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    g->TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    g->TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    g->TexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_R32UI,HD_GPU_PAGE_SIZE,HD_GPU_PAGE_SIZE,(GLsizei)s->cache_slots,
        0,GL_RED_INTEGER,GL_UNSIGNED_INT,NULL);
    state_restore(g,&restore);
    if(g->GetError()!=GL_NO_ERROR){cyc_hd_gpu_destroy(s,false);return NULL;}
    bool timer_capable=major>3 || (major==3 && minor>=3) ||
        (extensions && strstr(extensions,"GL_ARB_timer_query"));
    s->profile=getenv("NESRECOMP_GPU_PROFILE") && timer_capable && g->GenQueries && g->DeleteQueries && g->BeginQuery &&
        g->EndQuery && g->GetQueryObjectiv && g->GetQueryObjectui64v;
    if(s->profile)for(unsigned i=0;i<4;++i)g->GenQueries(2,s->query[i].queries);
    fprintf(stderr,"[HD GPU] vendor=%s renderer=%s GL=%s max_texture=%u buffer_texels=%u budget_bytes=%llu profile=%d\n",
        g->GetString(GL_VENDOR),g->GetString(GL_RENDERER),version,s->max_texture,s->max_buffer,(unsigned long long)s->budget,s->profile);
    return s;
}
SDL_Texture *cyc_hd_gpu_render(CycHdGpu *s,const HdJobFrame *f,unsigned requested,uint64_t prepare_ns) {
    if(!s)return NULL;
    ++s->attempts;
    if(s->failed)return NULL;
    uint64_t begin=SDL_GetPerformanceCounter(),asset_bytes=0;if(!frame_valid(s,f,&asset_bytes))return NULL;
    if(!requested)requested=f->source_scale;
    if(requested>64)requested=64;
    /* Reserve a second source generation for driver-managed buffer replacement.
     * A target replacement releases its old storage before requesting the new. */
    uint64_t aux=(uint64_t)f->background_count*2+f->sprite_count;
    if(aux<2)aux=2;
    unsigned aux_w=(unsigned)(aux>2048?2048:aux);
    if(aux_w>s->max_texture)aux_w=s->max_texture;
    aux=((aux+aux_w-1)/aux_w)*aux_w;
    uint64_t fixed=2*asset_bytes+(uint64_t)f->width*f->height*sizeof(HdJobPixel)+aux*16;
    unsigned scale=requested;
    while(scale && ((uint64_t)f->width*scale>s->max_texture || (uint64_t)f->height*scale>s->max_texture ||
        fixed+(uint64_t)f->width*f->height*scale*scale*4>s->budget))scale/=2;
    if(!scale)return NULL;
    /* A driver allocation refusal is also an admission decision. Keep the
     * working admitted size until the request, source geometry or pack changes. */
    if(s->output && s->requested_scale==requested && s->plane_w==f->width &&
        s->plane_h==f->height && s->pack_generation==f->pack_generation)
        scale=s->width/f->width;
    unsigned width=f->width*scale,height=f->height*scale;
    if(!s->output || width!=s->width || height!=s->height){
        if(s->output){SDL_DestroyTexture(s->output);s->output=NULL;s->width=s->height=0;}
        SDL_Texture *replacement=NULL;
        while(scale && !replacement){
            width=f->width*scale;height=f->height*scale;
            replacement=SDL_CreateTexture(s->renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,(int)width,(int)height);
            if(!replacement)scale/=2;
        }
        if(!replacement){s->failed=true;return NULL;}
        SDL_SetTextureBlendMode(replacement,SDL_BLENDMODE_NONE);
        s->output=replacement;s->width=width;s->height=height;
        fprintf(stderr,"[HD GPU] requested_scale=%u allocated_scale=%u requested=%ux%u allocated=%ux%u resident_bytes=%llu\n",
            requested,scale,f->width*requested,f->height*requested,width,height,(unsigned long long)(fixed-asset_bytes+(uint64_t)width*height*4));
    }
    s->requested_scale=requested;
    SDL_Texture *old=SDL_GetRenderTarget(s->renderer);
    if(SDL_SetRenderTarget(s->renderer,s->output))return NULL;
    if(SDL_RenderFlush(s->renderer)){SDL_SetRenderTarget(s->renderer,old);return NULL;}
    if(SDL_GL_GetCurrentContext()!=s->context){
        SDL_SetRenderTarget(s->renderer,old);s->failed=true;return NULL;
    }
    HdGl *g=&s->gl;HdGlState restore;state_take(g,&restore);
    bool new_assets=s->pack_generation!=f->pack_generation;uint64_t upload_bytes=0;
    uint64_t source_upload=0;bool good=upload_assets(s,f,asset_bytes,&source_upload);
    if(good && (s->serial!=f->serial || !s->plane_w || new_assets)){
        upload_bytes=(uint64_t)f->width*f->height*sizeof(HdJobPixel);
        if(s->plane_w!=f->width || s->plane_h!=f->height){
            image(s,0,GL_RGBA32UI,f->width*2,f->height,GL_UNSIGNED_INT,f->pixels);s->plane_w=f->width;s->plane_h=f->height;
        }else{texture(s,0,GL_TEXTURE_2D);g->TexSubImage2D(GL_TEXTURE_2D,0,0,0,(GLsizei)(f->width*2),(GLsizei)f->height,GL_RGBA_INTEGER,GL_UNSIGNED_INT,f->pixels);}
        uint64_t total=(uint64_t)f->background_count*2+f->sprite_count;
        if(total<2)total=2;
        unsigned w=(unsigned)(total>2048?2048:total);
        if(w>s->max_texture)w=s->max_texture;
        unsigned h=(unsigned)((total+w-1)/w);
        int32_t *rows=(int32_t *)calloc((size_t)w*h*4,sizeof(int32_t));
        if(!rows)good=false;
        else{
            for(unsigned i=0;i<f->background_count;++i){const HdJobBackground *b=&f->backgrounds[i];
                rows[i*8]=(int32_t)b->image;rows[i*8+1]=b->offset_x;rows[i*8+2]=b->offset_y;
                rows[i*8+3]=(int32_t)b->brightness;rows[i*8+4]=(int32_t)b->blend;}
            if(f->sprite_count)memcpy(rows+(size_t)f->background_count*8,f->sprites,(size_t)f->sprite_count*sizeof(HdJobSprite));
            if(s->bg_w!=w || s->bg_h!=h){image(s,3,GL_RGBA32I,w,h,GL_INT,rows);s->bg_w=w;s->bg_h=h;}
            else{texture(s,3,GL_TEXTURE_2D);g->TexSubImage2D(GL_TEXTURE_2D,0,0,0,(GLsizei)w,(GLsizei)h,GL_RGBA_INTEGER,GL_INT,rows);}
            upload_bytes+=(uint64_t)w*h*16;free(rows);
        }
        s->serial=f->serial;
    }
    upload_bytes+=source_upload;
    if(good){
        texture(s,0,GL_TEXTURE_2D);texture(s,1,GL_TEXTURE_2D_ARRAY);texture(s,2,GL_TEXTURE_BUFFER);texture(s,3,GL_TEXTURE_2D);texture(s,4,GL_TEXTURE_BUFFER);
        g->UseProgram(s->program);g->BindVertexArray(s->vao);
        const char *names[]={"jobs","source_pages","images","backgrounds","page_map"};
        for(unsigned i=0;i<5;++i)g->Uniform1i(g->GetUniformLocation(s->program,names[i]),(GLint)i);
#define HD_UNIFORM(name,value) g->Uniform1i(g->GetUniformLocation(s->program,name),(GLint)(value))
        HD_UNIFORM("output_scale",scale);HD_UNIFORM("source_scale",f->source_scale);HD_UNIFORM("image_count",f->image_count);
        HD_UNIFORM("aux_width",s->bg_w);HD_UNIFORM("behind_end",f->behind_end);HD_UNIFORM("has_behind",f->has_behind);HD_UNIFORM("under_end",f->under_end);HD_UNIFORM("mid_end",f->mid_end);HD_UNIFORM("bg_count",f->background_count);HD_UNIFORM("hud_rows",f->hud_rows);
#undef HD_UNIFORM
        g->Viewport(0,0,(GLsizei)width,(GLsizei)height);
        HdQuery *q=&s->query[s->query_cursor];
        if(s->profile){query_collect(s,q);if(!q->pending){q->serial=f->serial;q->prepare_ns=prepare_ns;q->upload_bytes=upload_bytes;
            q->resident_bytes=fixed-asset_bytes+(uint64_t)width*height*4;q->scale=scale;q->width=width;q->height=height;
            q->present=false;q->composite=true;g->BeginQuery(GL_TIME_ELAPSED,q->queries[0]);}}
        g->DrawArrays(GL_TRIANGLES,0,3);
        if(s->profile && !q->pending && q->composite){g->EndQuery(GL_TIME_ELAPSED);q->submit_ns=nanoseconds(begin);}
        GLenum error=g->GetError();good=error==GL_NO_ERROR;
        if(!good)fprintf(stderr,"[HD GPU] render error=0x%x; portable consumer\n",(unsigned)error);
    }
    state_restore(g,&restore);if(SDL_SetRenderTarget(s->renderer,old))good=false;
    if(!good){s->failed=true;return NULL;}++s->frames;return s->output;
}
void cyc_hd_gpu_present_begin(CycHdGpu *s){
    if(!s || !s->profile || s->failed)return;
    HdQuery *q=&s->query[s->query_cursor];
    if(!q->pending && q->composite){SDL_RenderFlush(s->renderer);s->gl.BeginQuery(GL_TIME_ELAPSED,q->queries[1]);q->present=true;}
}
void cyc_hd_gpu_present_end(CycHdGpu *s){
    if(!s || !s->profile || s->failed)return;
    HdQuery *q=&s->query[s->query_cursor];
    if(!q->pending && q->composite && q->present){SDL_RenderFlush(s->renderer);s->gl.EndQuery(GL_TIME_ELAPSED);q->pending=true;
        s->query_cursor=(s->query_cursor+1)%4;}
}
bool cyc_hd_gpu_readback(CycHdGpu *s,uint32_t *pixels,size_t pitch){
    if(!s || !s->output || !pixels || pitch<(size_t)s->width*4 || pitch>INT_MAX)return false;
    SDL_Texture *old=SDL_GetRenderTarget(s->renderer);if(SDL_SetRenderTarget(s->renderer,s->output))return false;
    bool good=SDL_RenderReadPixels(s->renderer,NULL,SDL_PIXELFORMAT_ARGB8888,pixels,(int)pitch)==0;
    if(SDL_SetRenderTarget(s->renderer,old))good=false;
    return good;
}
unsigned cyc_hd_gpu_width(const CycHdGpu *s){return s?s->width:0;}
unsigned cyc_hd_gpu_height(const CycHdGpu *s){return s?s->height:0;}
void cyc_hd_gpu_destroy(CycHdGpu *s,bool lost){
    if(!s)return;
    if(!lost && s->context && SDL_GL_GetCurrentContext()==s->context){
        if(s->profile){
            bool pending=false;for(unsigned i=0;i<4;++i)pending|=s->query[i].pending;
            /* Explicit profiling teardown only: complete the bounded query
             * tail before collection. Ordinary rendering never waits here. */
            if(pending)s->gl.Finish();
            for(unsigned i=0;i<4;++i){query_collect(s,&s->query[i]);s->gl.DeleteQueries(2,s->query[i].queries);}
        }
        s->gl.DeleteTextures(5,s->textures);s->gl.DeleteBuffers(2,s->buffers);
        if(s->vao)s->gl.DeleteVertexArrays(1,&s->vao);
        if(s->program)s->gl.DeleteProgram(s->program);
    }
    if(s->output)SDL_DestroyTexture(s->output);
    cyc_hd_gpu_cache_destroy(&s->cache);
    fflush(stdout);
    fprintf(stderr,"hd_gpu_summary {\"attempted\":%llu,\"rendered\":%llu,\"fallback\":%llu}\n",
        (unsigned long long)s->attempts,(unsigned long long)s->frames,
        (unsigned long long)(s->attempts-s->frames));
    free(s);
}
#else
struct CycHdGpu { int unavailable; };
CycHdGpu *cyc_hd_gpu_create(SDL_Renderer *r,SDL_Texture *t){(void)r;(void)t;return NULL;}
void cyc_hd_gpu_destroy(CycHdGpu *g,bool lost){(void)lost;free(g);}
SDL_Texture *cyc_hd_gpu_render(CycHdGpu *g,const HdJobFrame *f,unsigned scale,uint64_t ns){(void)g;(void)f;(void)scale;(void)ns;return NULL;}
void cyc_hd_gpu_present_begin(CycHdGpu *g){(void)g;}
void cyc_hd_gpu_present_end(CycHdGpu *g){(void)g;}
bool cyc_hd_gpu_readback(CycHdGpu *g,uint32_t *p,size_t pitch){(void)g;(void)p;(void)pitch;return false;}
unsigned cyc_hd_gpu_width(const CycHdGpu *g){(void)g;return 0;}
unsigned cyc_hd_gpu_height(const CycHdGpu *g){(void)g;return 0;}
#endif
