// cyc_ui_imgui.cpp - Dear ImGui for recomp-ui's runtime menu and toast over
// the cycle host's SDL_Renderer (cyc_ui.h). The host keeps the ImGui context;
// recomp-ui only draws into it. Everything here is drawn after the picture's
// texture is copied to the renderer, at the window's full resolution: the
// picture itself (cyc_frame_argb, screenshots, hashes) never contains it.
#include "cyc_ui.h"
#include "cyc_state_ui.h"

#include "recomp_runtime_ui.h"

#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <cstdio>

extern "C" {
RecompRuntimeUi *cyc_ui_menu_create(const CycUiHost *host);
void cyc_ui_menu_destroy(void);
void cyc_ui_menu_refresh(void);
RecompRuntimeUi *cyc_ui_runtime(void);
}

namespace {
ImGuiContext *s_ctx;
SDL_Renderer *s_ren;
bool s_ready;
const CycHostExtras *s_game_extras;
}

extern "C" bool cyc_ui_init(SDL_Window *win, SDL_Renderer *ren, const CycUiHost *host)
{
    s_game_extras = host ? host->extras : nullptr;
    if (!cyc_ui_menu_create(host)) {
        std::fprintf(stderr, "cyc ui: the runtime menu could not be created\n");
        return false;
    }
    IMGUI_CHECKVERSION();
    s_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(s_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL2_InitForSDLRenderer(win, ren)) {
        std::fprintf(stderr, "cyc ui: ImGui SDL2 platform initialization failed\n");
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
        return false;
    }
    if (!ImGui_ImplSDLRenderer2_Init(ren)) {
        std::fprintf(stderr, "cyc ui: ImGui SDL_Renderer2 initialization failed\n");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
        return false;
    }
    s_ren = ren;
    cyc_state_ui_init(ren, host);
    s_ready = true;
    return true;
}

extern "C" void cyc_ui_shutdown(void)
{
    if (s_ready) {
        cyc_state_ui_shutdown();
        ImGui::SetCurrentContext(s_ctx);
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(s_ctx);
        s_ctx = nullptr;
        s_ready = false;
    }
    cyc_ui_menu_destroy();
}

extern "C" void cyc_ui_process_event(const SDL_Event *ev)
{
    if (!s_ready) return;
    ImGui::SetCurrentContext(s_ctx);
    ImGui_ImplSDL2_ProcessEvent(ev);
    cyc_state_ui_event(ev);
}

extern "C" void cyc_ui_render(void)
{
    RecompRuntimeUi *ui = cyc_ui_runtime();
    if (!s_ready || !ui) return;
    const bool game_open = s_game_extras && s_game_extras->ui_is_open && s_game_extras->ui_is_open(s_game_extras->ctx);
    if (!recomp_runtime_ui_is_open(ui) && !recomp_runtime_ui_toast_visible(ui) && !cyc_state_ui_is_open()) {
        if (!game_open) return;
    }
    cyc_ui_menu_refresh();
    // The picture uses a logical size; the menu uses the whole drawable, then
    // the picture's logical size and viewport come back for the next frame.
    int lw = 0, lh = 0;
    SDL_Rect viewport{};
    SDL_RenderGetLogicalSize(s_ren, &lw, &lh);
    SDL_RenderGetViewport(s_ren, &viewport);
    SDL_RenderSetLogicalSize(s_ren, 0, 0);
    SDL_RenderSetViewport(s_ren, nullptr);
    ImGui::SetCurrentContext(s_ctx);
    // The standard menu consumes translated host navigation itself. A game
    // overlay consumes the raw SDL events through ImGui instead.
    ImGuiIO &io = ImGui::GetIO();
    const ImGuiConfigFlags nav = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (game_open) io.ConfigFlags |= nav;
    else io.ConfigFlags &= ~nav;
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    cyc_state_ui_render();
    if (s_game_extras && s_game_extras->ui_render) s_game_extras->ui_render(s_game_extras->ctx);
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), s_ren);
    SDL_RenderSetLogicalSize(s_ren, lw, lh);
    SDL_RenderSetViewport(s_ren, &viewport);
    SDL_SetRenderDrawBlendMode(s_ren, SDL_BLENDMODE_NONE);
}
