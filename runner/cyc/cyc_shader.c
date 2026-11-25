#include "cyc_shader.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SDL_VERSION_ATLEAST(2, 0, 18) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#include <SDL_opengl.h>

#define GLSL_FUNCTIONS(X) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(void, GetIntegerv, (GLenum, GLint *)) \
    X(GLboolean, IsEnabled, (GLenum)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) \
    X(void, ActiveTexture, (GLenum)) X(void, BindTexture, (GLenum, GLuint)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteShader, (GLuint)) X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, LinkProgram, (GLuint)) X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, UseProgram, (GLuint)) X(void, DeleteProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) X(void, BindVertexArray, (GLuint)) \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, GenSamplers, (GLsizei, GLuint *)) X(void, BindSampler, (GLuint, GLuint)) \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, DeleteSamplers, (GLsizei, const GLuint *)) X(void, DrawArrays, (GLenum, GLint, GLsizei))

struct CycShader {
#define DECLARE(ret, name, args) ret (APIENTRY *name) args;
    GLSL_FUNCTIONS(DECLARE)
#undef DECLARE
    SDL_GLContext context;
    GLuint program, vao, buffer, sampler;
    GLint texture, input_size, texture_size, output_size, frame_count, frame_direction;
    GLint filter, wrap, next_frame;
};

static char *read_shader(const char *path)
{
    SDL_RWops *file = SDL_RWFromFile(path, "rb");
    if (!file) return NULL;
    Sint64 size = SDL_RWsize(file);
    char *text = size > 0 && size <= 128 * 1024 ? malloc((size_t)size + 1) : NULL;
    if (text) {
        if (SDL_RWread(file, text, 1, (size_t)size) == (size_t)size) text[size] = 0;
        else { free(text); text = NULL; }
    }
    SDL_RWclose(file);
    return text;
}

static char *clean(char *text)
{
    text += strspn(text, " \t\r");
    char *end = text + strlen(text);
    while (end > text && strchr(" \t\r", end[-1])) --end;
    *end = 0;
    return text;
}

/* The shared bundled presets use a single viewport-sized pass. Other preset
 * layouts still get the normal SDL path instead of a partial effect. */
static char *load_preset(const char *path, GLint *filter, GLint *wrap)
{
    char *text = read_shader(path);
    if (!text) return NULL;
    const char *suffix = strrchr(path, '.');
    if (suffix && !strcmp(suffix, ".glsl")) return text;
    if (!suffix || strcmp(suffix, ".glslp")) { free(text); return NULL; }

    char *filename = NULL;
    bool single_pass = false, valid = true;
    for (char *line = text; line && valid;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *key = clean(line);
        line = next;
        if (!*key || *key == '#') continue;
        char *value = strchr(key, '=');
        if (!value) { valid = false; break; }
        *value++ = 0;
        key = clean(key);
        value = clean(value);
        size_t length = strlen(value);
        if (length >= 2 && value[0] == '"' && value[length - 1] == '"') {
            value[length - 1] = 0;
            ++value;
        }
        if (!strcmp(key, "shaders")) single_pass = !strcmp(value, "1");
        else if (!strcmp(key, "shader0")) filename = value;
        else if (!strcmp(key, "filter_linear0") && !strcmp(value, "true")) *filter = GL_LINEAR;
        else if (!strcmp(key, "filter_linear0") && !strcmp(value, "false")) *filter = GL_NEAREST;
        else if (!strcmp(key, "wrap_mode0") && !strcmp(value, "clamp_to_border")) *wrap = GL_CLAMP_TO_BORDER;
        else if (!strcmp(key, "wrap_mode0") && !strcmp(value, "clamp_to_edge")) *wrap = GL_CLAMP_TO_EDGE;
        else if (!strcmp(key, "scale_type0") && !strcmp(value, "viewport")) continue;
        else if (!strcmp(key, "scale0") && (!strcmp(value, "1") || !strcmp(value, "1.0"))) continue;
        else valid = false;
    }
    char *shader = NULL;
    if (valid && single_pass && filename && *filename) {
        const char *slash = strrchr(path, '/');
        const char *backslash = strrchr(path, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        size_t directory = slash ? (size_t)(slash - path + 1) : 0;
        char name[1024];
        int written = snprintf(name, sizeof name, "%.*s%s", (int)directory, path, filename);
        if (written >= 0 && (size_t)written < sizeof name) shader = read_shader(name);
    }
    free(text);
    return shader;
}

static GLuint compile(CycShader *s, GLenum kind, const char *text, const char *define)
{
    GLuint shader = s->CreateShader(kind);
    if (!shader) return 0;
    const char *version = "#version 130\n";
    GLint version_length = -1;
    if (!strncmp(text, "#version ", 9)) {
        const char *newline = strchr(text, '\n');
        if (newline) {
            version = text;
            version_length = (GLint)(newline + 1 - text);
            text = newline + 1;
        }
    }
    const char *parts[] = {version, define, text};
    const GLint sizes[] = {version_length, -1, -1};
    s->ShaderSource(shader, 3, parts, sizes);
    s->CompileShader(shader);
    GLint compiled = 0;
    s->GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512] = {0};
        s->GetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "[Video shader] compile: %s\n", log);
        s->DeleteShader(shader);
        return 0;
    }
    return shader;
}

bool cyc_shader_available(void) { return true; }

CycShader *cyc_shader_create(SDL_Renderer *renderer, const char *path)
{
    SDL_RendererInfo info = {0};
    if (SDL_GetRendererInfo(renderer, &info) || !info.name || strcmp(info.name, "opengl") ||
        SDL_RenderFlush(renderer) || !SDL_GL_GetCurrentContext()) return NULL;
    CycShader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
#define LOAD(ret, name, args) \
    s->name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!s->name) { free(s); return NULL; }
    GLSL_FUNCTIONS(LOAD)
#undef LOAD
    int major = 0, minor = 0;
    const char *version = (const char *)s->GetString(GL_VERSION);
    if (!version || sscanf(version, "%d.%d", &major, &minor) != 2 ||
        major < 3 || (major == 3 && minor < 3)) { free(s); return NULL; }
    s->context = SDL_GL_GetCurrentContext();
    s->filter = 0;
    s->wrap = GL_CLAMP_TO_EDGE;
    char *text = NULL;
    if (!strncmp(path, "assets/shaders/", 15)) {
        char *base = SDL_GetBasePath();
        if (base) {
            char bundled[1024];
            int length = snprintf(bundled, sizeof bundled, "%s%s", base, path);
            if (length >= 0 && (size_t)length < sizeof bundled)
                text = load_preset(bundled, &s->filter, &s->wrap);
            SDL_free(base);
        }
    }
    if (!text) text = load_preset(path, &s->filter, &s->wrap);
    if (!text) { free(s); return NULL; }
    GLuint vertex = compile(s, GL_VERTEX_SHADER, text, "#define VERTEX\n");
    GLuint fragment = compile(s, GL_FRAGMENT_SHADER, text, "#define FRAGMENT\n");
    free(text);
    GLint linked = 0;
    if (vertex && fragment) {
        s->program = s->CreateProgram();
        s->AttachShader(s->program, vertex);
        s->AttachShader(s->program, fragment);
        s->BindAttribLocation(s->program, 0, "VertexCoord");
        s->BindAttribLocation(s->program, 1, "TexCoord");
        s->LinkProgram(s->program);
        s->GetProgramiv(s->program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512] = {0};
            s->GetProgramInfoLog(s->program, sizeof log, NULL, log);
            fprintf(stderr, "[Video shader] link: %s\n", log);
        }
    }
    if (vertex) s->DeleteShader(vertex);
    if (fragment) s->DeleteShader(fragment);
    if (!linked) { cyc_shader_destroy(s, false); return NULL; }

    /* SDL samples top-down textures. Vertices are in bottom-up GL order. */
    static const GLfloat vertices[] = {0,0,0,1, 1,0,1,1, 0,1,0,0, 1,1,1,0};
    GLint saved_vao = 0, saved_buffer = 0;
    s->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao);
    s->GetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_buffer);
    s->GenVertexArrays(1, &s->vao);
    s->GenBuffers(1, &s->buffer);
    s->GenSamplers(1, &s->sampler);
    if (!s->vao || !s->buffer || !s->sampler) { cyc_shader_destroy(s, false); return NULL; }
    s->BindVertexArray(s->vao);
    s->BindBuffer(GL_ARRAY_BUFFER, s->buffer);
    s->BufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    s->VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);
    s->VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)(2 * sizeof(GLfloat)));
    s->EnableVertexAttribArray(0);
    s->EnableVertexAttribArray(1);
    s->BindBuffer(GL_ARRAY_BUFFER, (GLuint)saved_buffer);
    s->BindVertexArray((GLuint)saved_vao);
    s->SamplerParameteri(s->sampler, GL_TEXTURE_WRAP_S, s->wrap);
    s->SamplerParameteri(s->sampler, GL_TEXTURE_WRAP_T, s->wrap);
    s->texture = s->GetUniformLocation(s->program, "Texture");
    s->input_size = s->GetUniformLocation(s->program, "InputSize");
    s->texture_size = s->GetUniformLocation(s->program, "TextureSize");
    s->output_size = s->GetUniformLocation(s->program, "OutputSize");
    s->frame_count = s->GetUniformLocation(s->program, "FrameCount");
    s->frame_direction = s->GetUniformLocation(s->program, "FrameDirection");
    return s;
}

bool cyc_shader_draw(CycShader *s, SDL_Window *window, SDL_Renderer *renderer,
                     SDL_Texture *source, const SDL_Rect *d, bool linear)
{
    if (!s || SDL_GetRenderTarget(renderer) || SDL_RenderIsClipEnabled(renderer) ||
        SDL_RenderFlush(renderer) || SDL_GL_GetCurrentContext() != s->context) return false;
    int left, top, right, bottom, width, height, window_w, window_h, source_w, source_h;
    SDL_RenderLogicalToWindow(renderer, (float)d->x, (float)d->y, &left, &top);
    SDL_RenderLogicalToWindow(renderer, (float)(d->x + d->w), (float)(d->y + d->h), &right, &bottom);
    SDL_GetWindowSize(window, &window_w, &window_h);
    if (window_w <= 0 || window_h <= 0 || SDL_GetRendererOutputSize(renderer, &width, &height) ||
        SDL_QueryTexture(source, NULL, NULL, &source_w, &source_h)) return false;
    left = (int)((Sint64)left * width / window_w);
    right = (int)((Sint64)right * width / window_w);
    top = (int)((Sint64)top * height / window_h);
    bottom = (int)((Sint64)bottom * height / window_h);
    if (right <= left || bottom <= top) return false;

    GLint viewport[4], program, vao, active, texture, sampler;
    GLboolean blending = s->IsEnabled(GL_BLEND), scissor = s->IsEnabled(GL_SCISSOR_TEST);
    s->GetIntegerv(GL_VIEWPORT, viewport);
    s->GetIntegerv(GL_CURRENT_PROGRAM, &program);
    s->GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    s->GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    s->ActiveTexture(GL_TEXTURE0);
    s->GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    s->GetIntegerv(GL_SAMPLER_BINDING, &sampler);
    const GLboolean texture_enabled = s->IsEnabled(GL_TEXTURE_2D);
    float u = 0, v = 0;
    bool bound = SDL_GL_BindTexture(source, &u, &v) == 0;
    bool drawn = bound && u == 1.0f && v == 1.0f;
    if (drawn) {
        GLint sampling = s->filter ? s->filter : linear ? GL_LINEAR : GL_NEAREST;
        s->SamplerParameteri(s->sampler, GL_TEXTURE_MIN_FILTER, sampling);
        s->SamplerParameteri(s->sampler, GL_TEXTURE_MAG_FILTER, sampling);
        s->BindSampler(0, s->sampler);
        s->Disable(GL_BLEND);
        s->Disable(GL_SCISSOR_TEST);
        s->Viewport(left, height - bottom, right - left, bottom - top);
        s->UseProgram(s->program);
        s->BindVertexArray(s->vao);
        s->Uniform1i(s->texture, 0);
        s->Uniform2f(s->input_size, (float)source_w, (float)source_h);
        s->Uniform2f(s->texture_size, (float)source_w, (float)source_h);
        s->Uniform2f(s->output_size, (float)(right - left), (float)(bottom - top));
        s->Uniform1i(s->frame_count, s->next_frame);
        s->next_frame = s->next_frame == INT_MAX ? 0 : s->next_frame + 1;
        s->Uniform1i(s->frame_direction, 1);
        s->DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (bound) SDL_GL_UnbindTexture(source);
    s->ActiveTexture(GL_TEXTURE0);
    s->BindSampler(0, (GLuint)sampler);
    s->BindTexture(GL_TEXTURE_2D, (GLuint)texture);
    if (texture_enabled) s->Enable(GL_TEXTURE_2D);
    else s->Disable(GL_TEXTURE_2D);
    s->ActiveTexture((GLenum)active);
    if (blending) s->Enable(GL_BLEND);
    if (scissor) s->Enable(GL_SCISSOR_TEST);
    s->BindVertexArray((GLuint)vao);
    s->UseProgram((GLuint)program);
    s->Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    return drawn;
}

void cyc_shader_destroy(CycShader *s, bool context_lost)
{
    if (!s) return;
    if (!context_lost && SDL_GL_GetCurrentContext() == s->context) {
        if (s->sampler) s->DeleteSamplers(1, &s->sampler);
        if (s->buffer) s->DeleteBuffers(1, &s->buffer);
        if (s->vao) s->DeleteVertexArrays(1, &s->vao);
        if (s->program) s->DeleteProgram(s->program);
    }
    free(s);
}

#else
bool cyc_shader_available(void) { return false; }
CycShader *cyc_shader_create(SDL_Renderer *r, const char *path)
{ (void)r; (void)path; return NULL; }
bool cyc_shader_draw(CycShader *shader, SDL_Window *window, SDL_Renderer *r,
                     SDL_Texture *s, const SDL_Rect *d, bool linear)
{ (void)shader; (void)window; (void)r; (void)s; (void)d; (void)linear; return false; }
void cyc_shader_destroy(CycShader *s, bool lost) { (void)s; (void)lost; }
#endif
