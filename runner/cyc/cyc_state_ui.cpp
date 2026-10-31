#include "cyc_state_ui.h"
#include "cyc_ui.h"
#include "retcomm_rbengine/snap_ring.h"
#include "recomp_net/rnet_sha1.h"
#include "imgui.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
constexpr int Slots = 12, TW = 192, TH = 108;
constexpr size_t MaxBlob = 64u * 1024u * 1024u, HistoryBudget = 128u * 1024u * 1024u;
struct Thumb {
    std::array<uint32_t, TW * TH> pixels{};
    SDL_Texture *texture = nullptr;
    bool valid = false;
};
struct Entry { uint32_t tick; long frame; size_t bytes; Thumb thumb; };
struct Slot { bool exists = false; std::string date; Thumb thumb; };
CycUiHost host{};
SDL_Renderer *renderer;
ImFont *font;
RbeSnapRing *ring;
/* Entries own large immutable previews. Eviction must destroy only the
 * oldest entry, rather than move every retained preview on every capture. */
std::deque<Entry> history;
std::array<Slot, Slots> slots;
Thumb live;
std::string message;
std::string slot_base;
int mode, selected, cursor, depth = 100, interval = 4, columns = 4;
bool scroll_selection;
bool enabled = true;
long last_capture = -1;

bool available() {
    return host.state_serialize && host.state_restore && (!host.states_available || host.states_available());
}
void release(Thumb &t) { if (t.texture) SDL_DestroyTexture(t.texture); t.texture = nullptr; }
void clear_history() {
    for (auto &e : history) release(e.thumb);
    history.clear();
    if (ring) rbe_snap_ring_clear(ring);
    last_capture = -1;
    cursor = 0;
}
void announce(const char *text) {
    message = text;
    std::printf("[cyc states] f=%ld %s\n", host.frames_done ? host.frames_done() : 0, text);
    std::fflush(stdout);
}
fs::path path_for(int slot) {
    fs::path base = fs::u8path(slot_base);
    char suffix[24]; std::snprintf(suffix, sizeof(suffix), ".slot%02d", slot + 1);
    return base.parent_path() / (base.stem().u8string() + suffix + base.extension().u8string());
}
bool read_file(const fs::path &path, std::vector<uint8_t> &data, size_t maximum = MaxBlob) {
    std::error_code ec;
    auto n = fs::file_size(path, ec);
    if (ec || n == 0 || n > maximum) return false;
    data.resize(static_cast<size_t>(n));
    std::ifstream in(path, std::ios::binary);
    return in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(n)).good();
}
bool atomic_write(const fs::path &path, const uint8_t *data, size_t n) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    auto tmp = path; tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(n))) return false;
    out.close();
    if (!out) return false;
#ifdef _WIN32
    bool ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    fs::rename(tmp, path, ec);
    bool ok = !ec;
#endif
    if (!ok) fs::remove(tmp, ec);
    return ok;
}
void scan_slot(int index) {
    Slot &s = slots[index];
    release(s.thumb); s = Slot{};
    auto path = path_for(index);
    std::error_code ec;
    s.exists = fs::is_regular_file(path, ec);
    if (!s.exists) return;
    auto modified = fs::last_write_time(path, ec);
    if (!ec) {
        auto time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            modified - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t value = std::chrono::system_clock::to_time_t(time);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &value);
#else
        localtime_r(&value, &local);
#endif
        char date[48]; std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local); s.date = date;
    }
    std::vector<uint8_t> blob, preview;
    auto sidecar = path; sidecar += ".thumb";
    if (!read_file(path, blob) || !read_file(sidecar, preview, 20 + sizeof(s.thumb.pixels)) ||
        preview.size() != 20 + sizeof(s.thumb.pixels)) return;
    uint8_t digest[20]; rnet_sha1(blob.data(), blob.size(), digest);
    if (std::memcmp(preview.data(), digest, sizeof(digest))) return;
    std::memcpy(s.thumb.pixels.data(), preview.data() + 20, sizeof(s.thumb.pixels));
    s.thumb.valid = true;
}
void save_slot() {
    if (!available()) { announce("Save states are unavailable during online play"); return; }
    uint8_t *raw = nullptr; size_t n = 0; char error[256]{};
    if (!host.state_serialize(&raw, &n, error, sizeof(error))) { announce(error); return; }
    auto path = path_for(selected);
    bool ok = n && n <= MaxBlob && atomic_write(path, raw, n);
    if (ok && live.valid) {
        std::vector<uint8_t> preview(20 + sizeof(live.pixels));
        rnet_sha1(raw, n, preview.data());
        std::memcpy(preview.data() + 20, live.pixels.data(), sizeof(live.pixels));
        auto sidecar = path; sidecar += ".thumb";
        atomic_write(sidecar, preview.data(), preview.size());
    }
    std::free(raw);
    if (ok) { scan_slot(selected); announce("State saved"); }
    else announce("Could not write this save slot");
}
void load_slot() {
    if (!available()) { announce("Save states are unavailable during online play"); return; }
    std::vector<uint8_t> blob;
    if (!read_file(path_for(selected), blob)) { announce("No readable state in this slot"); return; }
    char error[256]{};
    if (!host.state_restore(blob.data(), blob.size(), error, sizeof(error))) { announce(error); return; }
    clear_history();
    mode = 0;
    announce("State loaded");
}
void commit_rewind() {
    if (!available() || history.empty()) return;
    auto &e = history[cursor];
    size_t n = 0; const uint8_t *blob = rbe_snap_ring_peek(ring, e.tick, &n);
    char error[256]{};
    if (!blob || !host.state_restore(blob, n, error, sizeof(error))) { announce(blob ? error : "History entry unavailable"); return; }
    uint32_t tick = e.tick;
    long frame = e.frame;
    rbe_snap_ring_drop_after(ring, tick);
    while (!history.empty() && history.back().tick > tick) { release(history.back().thumb); history.pop_back(); }
    last_capture = frame;
    mode = 0;
    announce("Resumed from rewind");
}
void move(int direction) {
    if (mode == 1) { selected = std::clamp(selected + direction, 0, Slots - 1); scroll_selection = true; }
    else if (mode == 2 && !history.empty()) cursor = std::clamp(cursor + direction, 0, static_cast<int>(history.size()) - 1);
}
void thumb_image(Thumb &t, ImVec2 size) {
    if (t.valid && !t.texture) {
        t.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, TW, TH);
        if (t.texture) SDL_UpdateTexture(t.texture, nullptr, t.pixels.data(), TW * 4);
    }
    if (t.texture) ImGui::Image(reinterpret_cast<ImTextureID>(t.texture), size);
    else {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x+size.x,p.y+size.y), IM_COL32(25,32,45,255), 4);
        ImGui::Dummy(size);
    }
}
}

extern "C" void cyc_state_ui_init(SDL_Renderer *ren, const CycUiHost *h) {
    renderer = ren; host = *h; slot_base = h->state_path ? h->state_path : "";
    ring = rbe_snap_ring_create(static_cast<uint32_t>(depth));
    if (char *base = SDL_GetBasePath()) {
        std::string path = std::string(base) + "assets/fonts/LatoLatin-Regular.ttf";
        std::error_code ec;
        if (fs::is_regular_file(fs::u8path(path), ec)) font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 20);
        SDL_free(base);
    }
}
extern "C" void cyc_state_ui_shutdown(void) {
    clear_history(); rbe_snap_ring_destroy(ring); ring = nullptr;
    for (auto &s : slots) release(s.thumb);
    release(live); live.valid = false; mode = 0; host = {}; renderer = nullptr; font = nullptr;
}
extern "C" void cyc_state_ui_configure(int on, int count, int cadence) {
    count = std::clamp(count, 4, 240); cadence = std::clamp(cadence, 1, 60);
    if (enabled == (on != 0) && count == depth && cadence == interval) return;
    clear_history();
    enabled = on != 0; depth = count; interval = cadence;
    if (ring) { rbe_snap_ring_destroy(ring); ring = rbe_snap_ring_create(static_cast<uint32_t>(depth)); }
}
extern "C" void cyc_state_ui_reset(void) { clear_history(); }
extern "C" void cyc_state_ui_picture(const uint32_t *pixels, int w, int h) {
    if (!pixels || w < 1 || h < 1) return;
    live.pixels.fill(0xFF111722u);
    double scale = std::min(static_cast<double>(TW) / w, static_cast<double>(TH) / h);
    int dw = std::max(1, static_cast<int>(w * scale)), dh = std::max(1, static_cast<int>(h * scale));
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x)
        live.pixels[static_cast<size_t>(y + (TH-dh)/2) * TW + x + (TW-dw)/2] =
            pixels[static_cast<size_t>(y * h / dh) * w + x * w / dw] | 0xFF000000u;
    live.valid = true;
}
extern "C" bool cyc_state_ui_capture_due(long frame) {
    return enabled && ring && !mode && available() && (last_capture < 0 || frame < last_capture || frame - last_capture >= interval);
}
extern "C" void cyc_state_ui_frame(long frame) {
    if (!available()) { clear_history(); return; }
    if (!cyc_state_ui_capture_due(frame)) return;
    if (frame < last_capture) clear_history();
    uint8_t *blob = nullptr; size_t n = 0; char error[256]{};
    if (!host.state_serialize(&blob, &n, error, sizeof(error))) { announce(error); return; }
    if (!n || n > MaxBlob) { std::free(blob); announce("Rewind snapshot exceeds the memory limit"); return; }
    size_t bytes = n; for (const auto &e : history) bytes += e.bytes;
    while (!history.empty() && (bytes > HistoryBudget || history.size() >= static_cast<size_t>(depth))) {
        bytes -= history.front().bytes; rbe_snap_ring_drop_tick(ring, history.front().tick);
        release(history.front().thumb); history.pop_front();
    }
    uint32_t tick = static_cast<uint32_t>(frame);
    if (!rbe_snap_ring_store(ring, tick, blob, n)) { std::free(blob); announce("Could not retain rewind snapshot"); return; }
    Entry e{tick, frame, n, {}};
    e.thumb.pixels = live.pixels; e.thumb.valid = live.valid;
    history.push_back(std::move(e)); last_capture = frame;
}
extern "C" bool cyc_state_ui_open(int rewind) {
    if (!available()) { announce("Save states and rewind are unavailable during online play"); return false; }
    int requested = rewind ? 2 : 1;
    if (mode == requested) { cyc_state_ui_close(); return true; }
    mode = requested; message.clear(); scroll_selection = true;
    if (rewind) cursor = std::max(0, static_cast<int>(history.size()) - 1);
    else for (int i = 0; i < Slots; ++i) scan_slot(i);
    announce(rewind ? "Rewind opened" : "Save-state browser opened");
    return true;
}
extern "C" void cyc_state_ui_close(void) { mode = 0; announce("Closed without loading a state"); }
extern "C" bool cyc_state_ui_is_open(void) { return mode != 0; }
extern "C" void cyc_state_ui_event(const SDL_Event *ev) {
    if (!mode) return;
    if (ev->type == SDL_KEYDOWN) {
        auto key = ev->key.keysym.scancode;
        if (ev->key.repeat && key != SDL_SCANCODE_LEFT && key != SDL_SCANCODE_RIGHT && key != SDL_SCANCODE_UP && key != SDL_SCANCODE_DOWN) return;
        if (key == SDL_SCANCODE_ESCAPE || key == SDL_SCANCODE_BACKSPACE) cyc_state_ui_close();
        else if (key == SDL_SCANCODE_LEFT) move(-1);
        else if (key == SDL_SCANCODE_RIGHT) move(1);
        else if (key == SDL_SCANCODE_UP) move(mode == 1 ? -columns : -1);
        else if (key == SDL_SCANCODE_DOWN) move(mode == 1 ? columns : 1);
        else if (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER || key == SDL_SCANCODE_SPACE) { if (mode == 1) load_slot(); else commit_rewind(); }
        else if (key == SDL_SCANCODE_S && mode == 1) save_slot();
    } else if (ev->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (ev->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move(1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move(mode == 1 ? -columns : -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(mode == 1 ? columns : 1); break;
        case SDL_CONTROLLER_BUTTON_A: if (mode == 1) load_slot(); else commit_rewind(); break;
        case SDL_CONTROLLER_BUTTON_X: if (mode == 1) save_slot(); break;
        case SDL_CONTROLLER_BUTTON_B: cyc_state_ui_close(); break;
        default: break;
        }
    }
}
extern "C" void cyc_state_ui_status(char *out, size_t n) {
    long selected_frame = mode == 2 && !history.empty() ? history[cursor].frame : -1;
    std::snprintf(out, n, "\"state_panel\":\"%s\",\"state_slot\":%d,\"rewind_count\":%zu,\"rewind_frame\":%ld,\"rewind_enabled\":%s",
        mode == 1 ? "states" : mode == 2 ? "rewind" : "closed", selected + 1, history.size(), selected_frame, enabled ? "true" : "false");
}
extern "C" void cyc_state_ui_render(void) {
    if (!mode) return;
    if (font) ImGui::PushFont(font);
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 size(std::min(vp->WorkSize.x * .96f, 1200.f), vp->WorkSize.y * .94f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x/2, vp->WorkPos.y + vp->WorkSize.y/2), ImGuiCond_Always, ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.055f,.07f,.10f,1));
    bool visible = true;
    ImGui::Begin(mode == 1 ? "Save states" : "Rewind", &visible, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    if (!visible) { mode = 0; ImGui::End(); ImGui::PopStyleColor(); if (font) ImGui::PopFont(); return; }
    if (mode == 1) {
        ImGui::TextWrapped("Enter / A: load   S / X: save   Escape / B: close");
        ImGui::Separator();
        ImGui::BeginChild("slots", ImVec2(0, -100), true);
        columns = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / 220), 1, 4);
        float width = (ImGui::GetContentRegionAvail().x - (columns-1)*12.f) / columns;
        for (int i = 0; i < Slots; ++i) {
            ImGui::PushID(i);
            if (i % columns) ImGui::SameLine();
            ImGui::BeginGroup();
            char label[32]; std::snprintf(label, sizeof(label), "Slot %02d%s", i+1, selected==i ? "  selected" : "");
            if (ImGui::Selectable(label, selected==i, 0, ImVec2(width, 20))) selected=i;
            thumb_image(slots[i].thumb, ImVec2(width, width*TH/TW));
            if (selected == i) {
                auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(lo, hi, IM_COL32(100,178,255,255), 3, 0, 2);
                if (scroll_selection) ImGui::SetScrollHereY(.5f);
            }
            if (ImGui::IsItemClicked()) selected=i;
            ImGui::TextUnformatted(slots[i].exists ? slots[i].date.c_str() : "Empty");
            ImGui::EndGroup(); ImGui::PopID();
        }
        ImGui::EndChild();
        scroll_selection = false;
        float button = std::min(190.f, (ImGui::GetContentRegionAvail().x-16)/3);
        if (ImGui::Button("Save slot", ImVec2(button,36))) save_slot();
        ImGui::SameLine();
        if (ImGui::Button("Load slot", ImVec2(button,36))) load_slot();
    } else {
        ImGui::TextWrapped("Left / Right: choose a moment   Enter / A: resume   Escape / B: cancel");
        ImGui::Separator();
        if (history.empty()) {
            ImGui::TextWrapped(enabled ? "Play a little longer to build rewind history." : "Rewind is disabled. Enable it in the launcher's settings.");
        } else {
            auto &e = history[cursor];
            double seconds = (host.frames_done() - e.frame) * host.frame_seconds;
            ImGui::Text("%.1f seconds ago", seconds);
            ImGui::SliderInt("History", &cursor, 0, static_cast<int>(history.size())-1, "");
            float preview_w = std::min(ImGui::GetContentRegionAvail().x, std::max(100.f, (ImGui::GetContentRegionAvail().y - 200) * TW/TH));
            thumb_image(history[cursor].thumb, ImVec2(preview_w, preview_w*TH/TW));
            float tile = std::min(160.f, (ImGui::GetContentRegionAvail().x-40)/5);
            for (int i = std::max(0,cursor-2), end=std::min(static_cast<int>(history.size()),cursor+3); i<end; ++i) {
                if (i > std::max(0,cursor-2)) ImGui::SameLine();
                ImGui::PushID(i); ImGui::BeginGroup();
                thumb_image(history[i].thumb, ImVec2(tile,tile*TH/TW));
                if (ImGui::IsItemClicked()) cursor=i;
                ImGui::Text("%s%.1fs", i==cursor ? "> " : "", (host.frames_done()-history[i].frame)*host.frame_seconds);
                ImGui::EndGroup(); ImGui::PopID();
            }
            if (ImGui::Button("Resume here", ImVec2(190,36))) commit_rewind();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110,36))) cyc_state_ui_close();
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::End(); ImGui::PopStyleColor();
    if (font) ImGui::PopFont();
}
