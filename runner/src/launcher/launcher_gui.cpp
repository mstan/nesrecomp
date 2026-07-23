// launcher_gui.cpp — RmlUi pre-boot launcher (see launcher_gui.h).
//
// Structure ported from snesrecomp/runner/src/launcher/launcher_gui.cpp, adapted
// to the NES: iNES-header metadata + CRC32 verification, a Display/Audio settings
// view, per-player input device dropdowns + a controller config / Input Test view,
// and a SAVES panel backed by the shared save_ram file helpers.

#include "launcher_gui.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include "RmlUi_Renderer_GL3.h"
#include "RmlUi_Platform_SDL.h"

#include <functional>
#include <memory>

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>

extern "C" {
#include "crc32.h"
#include "save_ram.h"
// stb_image (read) implementation is already compiled by the runner (chr_codec.c);
// we only need the decls to decode PNG cartridge art. C linkage to match it.
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len,
                                     int* x, int* y, int* channels_in_file, int desired);
void stbi_image_free(void* retval_from_stbi_load);
}

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  include <shellapi.h>
#  include <objbase.h>
#  include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace nes_launcher {
namespace {

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

std::string basename_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string human_size(long bytes) {
    char buf[64];
    if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%ld KB", bytes / 1024);
    else
        std::snprintf(buf, sizeof(buf), "%ld B", bytes);
    return buf;
}

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

// Read a whole file. Returns empty vector on failure.
std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::rewind(f);
    if (sz > 0) {
        data.resize((size_t)sz);
        if (std::fread(data.data(), 1, (size_t)sz, f) != (size_t)sz) data.clear();
    }
    std::fclose(f);
    return data;
}

// ----------------------------------------------------------------------------
// Master game DB (gamedb.json): CRC32 -> canonical name + region. Lets the
// launcher show the real title for a recognised ROM and fall back to the file
// name for anything unknown (rom hacks, variants).
// ----------------------------------------------------------------------------

struct DbEntry { uint32_t crc; Rml::String name; Rml::String region; };
std::vector<DbEntry> g_gamedb;

// Extract the string value of "key":"..." searching forward from `pos`; advances
// `pos` past it. Minimal scanner for our controlled gamedb.json (fields per record
// appear in order crc/name/region). Returns "" when not found.
std::string json_str_after(const std::string& s, size_t& pos, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = s.find(pat, pos);
    if (k == std::string::npos) return "";
    size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos) return "";
    size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    pos = q2 + 1;
    return s.substr(q1 + 1, q2 - q1 - 1);
}

void load_gamedb(const fs::path& assets) {
    g_gamedb.clear();
    std::vector<uint8_t> raw = read_file((assets / "gamedb.json").string());
    if (raw.empty()) return;
    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    size_t pos = 0;
    for (;;) {
        std::string crc = json_str_after(text, pos, "crc");
        if (crc.empty()) break;
        std::string name = json_str_after(text, pos, "name");
        std::string region = json_str_after(text, pos, "region");
        DbEntry e;
        e.crc = (uint32_t)std::strtoul(crc.c_str(), nullptr, 16);
        e.name = name.c_str();
        e.region = region.c_str();
        g_gamedb.push_back(e);
    }
}

const DbEntry* db_lookup(uint32_t crc) {
    for (const DbEntry& e : g_gamedb) if (e.crc == crc) return &e;
    return nullptr;
}

// RenderInterface_GL3 only decodes uncompressed TGA; override LoadTexture to
// decode PNG via stb_image so the dashboard can show cartridge art. RmlUi
// textures are premultiplied-alpha RGBA. (Pattern from the PSX launcher.)
class LauncherRenderInterface : public RenderInterface_GL3 {
public:
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dims, const Rml::String& source) override {
        Rml::FileInterface* fi = Rml::GetFileInterface();
        Rml::FileHandle fh = fi ? fi->Open(source) : Rml::FileHandle(0);
        if (!fh) return RenderInterface_GL3::LoadTexture(dims, source);
        fi->Seek(fh, 0, SEEK_END);
        const size_t sz = (size_t)fi->Tell(fh);
        fi->Seek(fh, 0, SEEK_SET);
        std::vector<unsigned char> buf(sz);
        fi->Read(buf.data(), sz, fh);
        fi->Close(fh);

        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(buf.data(), (int)sz, &w, &h, &comp, 4);
        if (!px) return RenderInterface_GL3::LoadTexture(dims, source);  // maybe a TGA
        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; i++) {  // straight -> premultiplied alpha
            const unsigned a = px[i * 4 + 3];
            px[i * 4 + 0] = (unsigned char)(px[i * 4 + 0] * a / 255);
            px[i * 4 + 1] = (unsigned char)(px[i * 4 + 1] * a / 255);
            px[i * 4 + 2] = (unsigned char)(px[i * 4 + 2] * a / 255);
        }
        dims.x = w; dims.y = h;
        Rml::TextureHandle th = GenerateTexture({px, n * 4}, dims);
        stbi_image_free(px);
        return th;
    }
};

// Human board name for an iNES mapper number (common NES boards). Not exhaustive;
// a game can override with GameInfo::mapper_board.
const char* mapper_name(int m) {
    switch (m) {
        case 0:  return "NROM";
        case 1:  return "MMC1";
        case 2:  return "UxROM";
        case 3:  return "CNROM";
        case 4:  return "MMC3";
        case 5:  return "MMC5";
        case 7:  return "AxROM";
        case 9:  return "MMC2";
        case 10: return "MMC4";
        case 11: return "Color Dreams";
        case 66: return "GxROM";
        case 69: return "FME-7";
        default: return "";
    }
}

#ifdef _WIN32
bool pick_file(const char* title, const char* filter, char* out, size_t max_len) {
    OPENFILENAMEA ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)max_len;
    ofn.lpstrTitle  = title;
    // OFN_NOCHANGEDIR: keep the dialog from changing the process CWD, which would
    // scatter config/saves next to the picked file instead of the exe.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY
              | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) != 0;
}
// Pick a directory (for choosing an HD-pack folder). SHBrowseForFolder's new
// dialog style needs a COM apartment; SDL usually OleInitialize()s already, but
// init defensively and only uninit if we actually initialised it here.
bool pick_folder(const char* title, char* out, size_t max_len) {
    out[0] = '\0';
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool did_init = SUCCEEDED(hr);   // S_OK (we initialised) or S_FALSE (already)
    BROWSEINFOA bi;
    std::memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = title;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    bool ok = false;
    if (pidl) {
        char path[MAX_PATH] = {0};
        if (SHGetPathFromIDListA(pidl, path)) {
            std::snprintf(out, max_len, "%s", path);
            ok = true;
        }
        CoTaskMemFree(pidl);
    }
    if (did_init) CoUninitialize();
    return ok;
}
// Directory containing the running executable (the default HD-pack root lives at
// <exe_dir>/hdpack, matching hdpack_load_from_config's resolution).
std::string exe_dir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof(buf));
    std::string s(buf, n ? (size_t)n : 0);
    size_t slash = s.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}
#else
bool pick_file(const char*, const char*, char* out, size_t) { out[0] = '\0'; return false; }
bool pick_folder(const char*, char* out, size_t) { out[0] = '\0'; return false; }
std::string exe_dir() { return "."; }
#endif

// Resolve the pack folder shown/validated in the UI: a custom dir if the user
// picked one, otherwise the default <exe_dir>/hdpack.
std::string resolve_hdpack_dir(const NesLauncherSettings& s) {
    return s.hdpack_dir[0] ? std::string(s.hdpack_dir) : (exe_dir() + "/hdpack");
}

// One entry in a controller-source dropdown: a stable token + display label.
struct SrcOption { Rml::String value; Rml::String label; };

// ----------------------------------------------------------------------------
// View model — every variable bound to the RML data model.
// ----------------------------------------------------------------------------

struct Model {
    Rml::String view = "dashboard";

    Rml::String game_name, game_region;

    bool rom_loaded = false;
    bool has_cartridge = false;   // a cartridge.png is bundled beside launcher.rml
    Rml::String rom_title;        // DB-matched game name, else the file name
    Rml::String rom_file, rom_size, rom_mapper, rom_prg, rom_chr, rom_crc;
    bool crc_known = false;     // game advertises an expected CRC (badge applies)
    bool crc_match = false;

    // Names of the SDL game controllers plugged in, index 0..N.
    std::vector<std::string> pad_names;

    Rml::String p1_status = "Enabled", p2_status = "Enabled";
    bool p1_enabled = true, p2_enabled = true;
    std::vector<SrcOption> p1_options, p2_options;
    Rml::String p1_src_value = "kbd", p2_src_value = "pad";

    // SAVES
    bool uses_sram = false;
    Rml::String save_file = "(none)", save_size = "0 KB";

    // Game-specific "password / mantra" save (e.g. Faxanadu). Shown instead of the
    // binary SRAM UI when the game provides a password_save_path. Read-only display
    // (password_value); the edit icon flips password_editing on and binds an input
    // to password_input; pressing Save raises show_password_confirm, and confirming
    // writes the file. password_path is the file the launcher reads/rewrites.
    bool has_password_save = false;
    Rml::String password_label = "Password";
    Rml::String password_value = "(none)";   // current saved password (read-only)
    bool password_editing = false;
    Rml::String password_input;              // edit buffer (two-way bound to <input>)
    bool show_password_confirm = false;
    std::string password_path;               // file backing the password (not bound)

    bool skip_launcher = false;
    bool show_skip_modal = false;

    // settings
    Rml::String scale_label, renderer_label;
    bool integer_scale = true, filter = false;
    int  volume = 100;
    bool widescreen = false, widescreen_supported = false;

    // HD texture pack
    bool hdpack_supported = true;              // gates the whole HD-pack panel
    bool hdpack_enabled = false;
    bool hdpack_valid = false;                 // resolved folder has a hires.txt
    bool hdpack_custom = false;                // a custom folder is set (vs default)
    Rml::String hdpack_name = "hdpack/ (default)";  // folder shown in the UI
    Rml::String hdpack_status = "Place a pack in the default hdpack/ folder, or choose one.";

    // controller config (deadzone only; device is chosen on the dashboard)
    int cfg_player = 0;
    Rml::String cfg_player_label = "1";
    int cfg_deadzone = 30;

    bool status_ready = false;
};

std::vector<std::string> enumerate_pads() {
    std::vector<std::string> names;
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        const char* nm = SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i)
                                                 : SDL_JoystickNameForIndex(i);
        names.emplace_back(nm && *nm ? nm : "Controller");
    }
    return names;
}

void build_src_options(std::vector<SrcOption>& opts, int player,
                       const std::vector<std::string>& pads) {
    opts.clear();
    opts.push_back({ "none", "None" });
    if (player == 0)
        opts.push_back({ "kbd",  "Keyboard" });
    if (player >= 0 && player < (int)pads.size())
        opts.push_back({ "pad", pads[player] });
}

const char* src_to_value(InputSource s) {
    return s == InputSource::Keyboard ? "kbd"
         : s == InputSource::Gamepad  ? "pad"
         : "none";
}
InputSource value_to_src(const Rml::String& v) {
    if (v == "kbd") return InputSource::Keyboard;
    if (v == "pad") return InputSource::Gamepad;
    return InputSource::None;
}

void refresh_settings_labels(Model& m, const NesLauncherSettings& s) {
    char b[32];
    std::snprintf(b, sizeof(b), "%dx", s.window_scale < 1 ? 1 : s.window_scale);
    m.scale_label = b;
    m.renderer_label = s.renderer == 1 ? "Software" : "Accelerated";
    m.widescreen = s.widescreen;
    m.integer_scale = s.integer_scale;
    m.filter = s.linear_filter;
    m.volume = s.volume;
    m.p1_enabled = s.player_src[0] != InputSource::None;
    m.p2_enabled = s.player_src[1] != InputSource::None;
    m.p1_src_value = src_to_value(s.player_src[0]);
    m.p2_src_value = src_to_value(s.player_src[1]);
    m.p1_status = m.p1_enabled ? "Enabled" : "Disabled";
    m.p2_status = m.p2_enabled ? "Enabled" : "Disabled";
}

// Reflect the HD-pack settings into the model: enabled flag, folder basename,
// and a status line that validates the folder actually contains a hires.txt.
void refresh_hdpack(Model& m, const NesLauncherSettings& s) {
    m.hdpack_enabled = s.hdpack_enabled;
    bool custom = s.hdpack_dir[0] != '\0';
    std::string dir = resolve_hdpack_dir(s);
    bool ok = !dir.empty() && fs::exists(fs::path(dir) / "hires.txt");
    m.hdpack_custom = custom;
    m.hdpack_valid = ok;
    m.hdpack_name = custom ? Rml::String(dir.c_str())
                           : Rml::String("hdpack/ (default)");
    if (ok)
        m.hdpack_status = "\xE2\x9C\x93 hires.txt found \xE2\x80\x94 pack ready.";
    else if (custom)
        m.hdpack_status = "No hires.txt in this folder \xE2\x80\x94 not a valid HD pack.";
    else
        m.hdpack_status = "No pack in the default hdpack/ folder yet \xE2\x80\x94 drop one in, or choose a folder.";
}

// Compute and display ROM verification info for `path` (iNES header parse).
void load_rom_info(Model& m, const GameInfo& g, const std::string& path) {
    m.rom_loaded = false;
    m.crc_match = false;
    if (path.empty()) { m.rom_file = "(none)"; m.status_ready = false; return; }

    std::vector<uint8_t> data = read_file(path);
    if (data.size() < 16 ||
        !(data[0]=='N' && data[1]=='E' && data[2]=='S' && data[3]==0x1A)) {
        m.rom_file = basename_of(path) + " (not an iNES ROM)";
        m.status_ready = false;
        return;
    }

    int prg = data[4];
    int chr = data[5];
    int mapper = ((data[6] >> 4) & 0x0F) | (data[7] & 0xF0);
    bool battery = (data[6] & 0x02) != 0;
    // SRAM panel applies when the cartridge is battery-backed, or the game forces
    // it on (synthetic-SRAM enhancement).
    m.uses_sram = g.uses_sram || battery;

    // CRC32 over the post-header bytes (matches launcher.c / game_get_expected_crc32).
    uint32_t crc = crc32_compute(data.data() + 16, data.size() - 16);

    char buf[64];
    m.rom_file = basename_of(path);
    // Title: canonical name from the master DB (by CRC), else the file name (so a
    // rom hack / unknown ROM still shows something sensible).
    const DbEntry* dbe = db_lookup(crc);
    if (dbe) {
        m.rom_title = dbe->name;
        if (!dbe->region.empty()) m.game_region = dbe->region;
    } else {
        m.rom_title = m.rom_file;
        m.game_region = g.region ? g.region : "";
    }
    m.rom_size = human_size((long)data.size());
    if (g.mapper_board && *g.mapper_board) {
        m.rom_mapper = g.mapper_board;
    } else {
        const char* nm = mapper_name(mapper);
        if (*nm) std::snprintf(buf, sizeof(buf), "Mapper %d (%s)", mapper, nm);
        else     std::snprintf(buf, sizeof(buf), "Mapper %d", mapper);
        m.rom_mapper = buf;
    }
    std::snprintf(buf, sizeof(buf), "%d KB", prg * 16);
    m.rom_prg = buf;
    if (chr > 0) { std::snprintf(buf, sizeof(buf), "%d KB", chr * 8); m.rom_chr = buf; }
    else         m.rom_chr = "CHR RAM";
    m.rom_crc = hex32(crc);
    m.crc_known = g.has_expected_crc;
    m.crc_match = g.has_expected_crc && crc == g.expected_crc;
    m.rom_loaded = true;
    m.status_ready = true;
}

// Populate the SAVES panel from the shared save_ram backend (UI-bound to the
// same saves/<basename>.srm the runtime uses).
void refresh_save_info(Model& m) {
    if (!m.uses_sram) {
        m.save_file = "(none)"; m.save_size = "0 KB";
        return;
    }
    const char* p = save_ram_path();
    m.save_file = (p && *p) ? basename_of(p) : "(none)";
    m.save_size = save_ram_exists() ? human_size(save_ram_size()) : Rml::String("0 KB");
}

// Read the first line of a single-line text save, trimming CR/LF and surrounding
// whitespace. Returns "" if the file is missing/empty.
std::string read_password_file(const std::string& path) {
    if (path.empty()) return "";
    std::vector<uint8_t> d = read_file(path);
    std::string s;
    for (uint8_t c : d) { if (c == '\n' || c == '\r') break; s.push_back((char)c); }
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

// Rewrite the single-line text save (value + trailing newline, matching the
// runtime's mantra_save_write format). Returns false if the file can't be opened.
bool write_password_file(const std::string& path, const std::string& value) {
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "%s\n", value.c_str());
    std::fclose(f);
    return true;
}

// Load the current password into the model (read-only display value).
void refresh_password_info(Model& m) {
    if (!m.has_password_save) return;
    std::string v = read_password_file(m.password_path);
    m.password_value = v.empty() ? Rml::String("(none)") : Rml::String(v.c_str());
}

bool load_fonts(const fs::path& assets) {
    bool any = false;
    const char* faces[] = { "fonts/LatoLatin-Regular.ttf", "fonts/LatoLatin-Bold.ttf" };
    for (const char* f : faces) {
        fs::path p = assets / f;
        if (fs::exists(p) && Rml::LoadFontFace(p.generic_string())) any = true;
    }
#ifdef _WIN32
    if (!any) {
        if (Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf")) any = true;
    }
    Rml::LoadFontFace("C:/Windows/Fonts/seguisym.ttf", /*fallback_face=*/true);
#endif
    return any;
}

} // namespace

// ----------------------------------------------------------------------------
// run()
// ----------------------------------------------------------------------------

Result run(SDL_Window* window, void* /*gl_context*/,
           NesLauncherSettings& io, const GameInfo& game,
           const char* assets_dir, const char* initial_rom,
           char* out_rom_path, size_t out_rom_path_len) {

    if (out_rom_path && out_rom_path_len) out_rom_path[0] = '\0';

    Rml::String gl_msg;
    if (!RmlGL3::Initialize(&gl_msg)) {
        std::fprintf(stderr, "launcher: RmlGL3::Initialize failed: %s\n", gl_msg.c_str());
        return Result::Unavailable;
    }

    SystemInterface_SDL system_interface;
    system_interface.SetWindow(window);
    LauncherRenderInterface render_interface;

    Rml::SetSystemInterface(&system_interface);
    Rml::SetRenderInterface(&render_interface);
    if (!Rml::Initialise()) {
        std::fprintf(stderr, "launcher: Rml::Initialise failed\n");
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    const fs::path assets = assets_dir ? fs::path(assets_dir) : fs::current_path();
    if (!load_fonts(assets))
        std::fprintf(stderr, "launcher: warning — no font face loaded; text will not render\n");
    load_gamedb(assets);

    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) { win_w = 1024; win_h = 768; }
    render_interface.SetViewport(win_w, win_h);

    Rml::Context* context = Rml::CreateContext("launcher", Rml::Vector2i(win_w, win_h));
    if (!context) {
        std::fprintf(stderr, "launcher: CreateContext failed\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    // ---- model ----
    Model m;
    m.game_name = game.name ? game.name : "NES Game";
    m.game_region = game.region ? game.region : "";
    m.uses_sram = game.uses_sram;
    m.widescreen_supported = game.widescreen_supported;
    m.hdpack_supported = game.hdpack_supported;
    m.has_cartridge = fs::exists(assets / "cartridge.png");

    // Game-specific password/mantra save (e.g. Faxanadu). When the game supplies a
    // path, the SAVES panel shows the password text instead of the binary SRAM UI.
    m.has_password_save = (game.password_save_path && *game.password_save_path);
    if (m.has_password_save) {
        m.password_path = game.password_save_path;
        if (game.password_save_label && *game.password_save_label)
            m.password_label = game.password_save_label;
    }

    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    m.pad_names = enumerate_pads();
    std::vector<SDL_GameController*> open_pads;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            if (SDL_GameController* gc = SDL_GameControllerOpen(i))
                open_pads.push_back(gc);
    // P2 no longer shares P1's keyboard. Normalize old configs that selected it,
    // then validate gamepad assignments against the connected devices.
    if (io.player_src[1] == InputSource::Keyboard)
        io.player_src[1] = InputSource::None;
    // A "Gamepad" source only makes sense when a controller is actually plugged
    // in; otherwise fall back to Keyboard (P1) / None (P2).
    for (int p = 0; p < 2; p++) {
        if (io.player_src[p] == InputSource::Gamepad && p >= (int)m.pad_names.size())
            io.player_src[p] = (p == 0) ? InputSource::Keyboard : InputSource::None;
    }
    build_src_options(m.p1_options, 0, m.pad_names);
    build_src_options(m.p2_options, 1, m.pad_names);

    // Bind the SAVES panel to the same save file the runtime backend will use
    // (path only; the panel shows/hides per the loaded ROM's battery bit).
    if (game.save_basename && *game.save_basename)
        save_ram_ui_bind(game.save_basename);

    refresh_settings_labels(m, io);
    refresh_hdpack(m, io);

    std::string rom_path = initial_rom ? initial_rom : "";
    load_rom_info(m, game, rom_path);
    refresh_save_info(m);
    refresh_password_info(m);
    m.skip_launcher = io.skip_launcher;

    Rml::DataModelConstructor c = context->CreateDataModel("launcher");
    if (!c) {
        std::fprintf(stderr, "launcher: CreateDataModel returned an invalid constructor\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    if (auto sh = c.RegisterStruct<SrcOption>()) {
        sh.RegisterMember("value", &SrcOption::value);
        sh.RegisterMember("label", &SrcOption::label);
    }
    c.RegisterArray<std::vector<SrcOption>>();

    c.Bind("view", &m.view);
    c.Bind("game_name", &m.game_name);
    c.Bind("game_region", &m.game_region);
    c.Bind("rom_loaded", &m.rom_loaded);
    c.Bind("has_cartridge", &m.has_cartridge);
    c.Bind("rom_title", &m.rom_title);
    c.Bind("rom_file", &m.rom_file);
    c.Bind("rom_size", &m.rom_size);
    c.Bind("rom_mapper", &m.rom_mapper);
    c.Bind("rom_prg", &m.rom_prg);
    c.Bind("rom_chr", &m.rom_chr);
    c.Bind("rom_crc", &m.rom_crc);
    c.Bind("crc_known", &m.crc_known);
    c.Bind("crc_match", &m.crc_match);
    c.Bind("p1_options", &m.p1_options);
    c.Bind("p2_options", &m.p2_options);
    c.Bind("p1_src_value", &m.p1_src_value);
    c.Bind("p2_src_value", &m.p2_src_value);
    c.Bind("p1_status", &m.p1_status);
    c.Bind("p2_status", &m.p2_status);
    c.Bind("p1_enabled", &m.p1_enabled);
    c.Bind("p2_enabled", &m.p2_enabled);
    c.Bind("uses_sram", &m.uses_sram);
    c.Bind("save_file", &m.save_file);
    c.Bind("save_size", &m.save_size);
    c.Bind("has_password_save", &m.has_password_save);
    c.Bind("password_label", &m.password_label);
    c.Bind("password_value", &m.password_value);
    c.Bind("password_editing", &m.password_editing);
    c.Bind("password_input", &m.password_input);
    c.Bind("show_password_confirm", &m.show_password_confirm);
    c.Bind("skip_launcher", &m.skip_launcher);
    c.Bind("show_skip_modal", &m.show_skip_modal);
    c.Bind("scale_label", &m.scale_label);
    c.Bind("renderer_label", &m.renderer_label);
    c.Bind("widescreen", &m.widescreen);
    c.Bind("widescreen_supported", &m.widescreen_supported);
    c.Bind("hdpack_supported", &m.hdpack_supported);
    c.Bind("integer_scale", &m.integer_scale);
    c.Bind("filter", &m.filter);
    c.Bind("volume", &m.volume);
    c.Bind("hdpack_enabled", &m.hdpack_enabled);
    c.Bind("hdpack_valid", &m.hdpack_valid);
    c.Bind("hdpack_custom", &m.hdpack_custom);
    c.Bind("hdpack_name", &m.hdpack_name);
    c.Bind("hdpack_status", &m.hdpack_status);
    c.Bind("cfg_player_label", &m.cfg_player_label);
    c.Bind("cfg_deadzone", &m.cfg_deadzone);

    Rml::DataModelHandle handle = c.GetModelHandle();
    Result result = Result::Quit;
    bool running = true;
    auto dirty_all = [&]() { handle.DirtyAllVariables(); };

    // ---- navigation ----
    c.BindEventCallback("show_dashboard", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.view = "dashboard"; dirty_all();
    });
    c.BindEventCallback("show_settings", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.view = "settings"; dirty_all();
    });
    auto open_cfg = [&](int p) {
        m.cfg_player = p;
        m.cfg_player_label = p == 0 ? "1" : "2";
        m.cfg_deadzone = io.deadzone[p];
        m.view = "controller"; dirty_all();
    };
    c.BindEventCallback("config_p1", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { open_cfg(0); });
    c.BindEventCallback("config_p2", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { open_cfg(1); });

    // ---- ROM ----
    c.BindEventCallback("change_rom", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        char buf[1024];
        if (pick_file("Select NES ROM", "NES ROMs (*.nes)\0*.nes\0All Files (*.*)\0*.*\0", buf, sizeof(buf))) {
            rom_path = buf;
            load_rom_info(m, game, rom_path);
            refresh_save_info(m);
            dirty_all();
        }
    });

    // ---- saves ----
    c.BindEventCallback("save_import", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!m.uses_sram) return;
        char buf[1024];
        if (!pick_file("Import SRAM Save", "NES saves (*.srm;*.sav)\0*.srm;*.sav\0All Files (*.*)\0*.*\0",
                       buf, sizeof(buf)))
            return;
        save_ram_import(buf);
        refresh_save_info(m);
        dirty_all();
    });
    c.BindEventCallback("save_clear", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!m.uses_sram) return;
        save_ram_clear();
        refresh_save_info(m);
        dirty_all();
    });

    // ---- password / mantra save (game-specific; e.g. Faxanadu) ----
    // Read-only by default; the edit icon enters edit mode seeded from the current
    // value, and saving always routes through a confirmation modal — the file is
    // never rewritten without the user confirming.
    c.BindEventCallback("password_edit", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!m.has_password_save) return;
        m.password_input = (m.password_value == "(none)") ? Rml::String("") : m.password_value;
        m.password_editing = true;
        dirty_all();
    });
    c.BindEventCallback("password_edit_cancel", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.password_editing = false;
        dirty_all();
    });
    c.BindEventCallback("password_request_save", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!m.has_password_save) return;
        m.show_password_confirm = true;     // confirm before it takes effect
        dirty_all();
    });
    c.BindEventCallback("password_confirm_ok", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        std::string v = m.password_input.c_str();
        size_t a = v.find_first_not_of(" \t");
        size_t b = v.find_last_not_of(" \t");
        v = (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
        write_password_file(m.password_path, v);
        refresh_password_info(m);
        m.password_editing = false;
        m.show_password_confirm = false;
        dirty_all();
    });
    c.BindEventCallback("password_confirm_cancel", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.show_password_confirm = false;
        dirty_all();
    });

    // ---- skip launcher (confirm modal on enable) ----
    c.BindEventCallback("toggle_skip_launcher", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (io.skip_launcher) { io.skip_launcher = false; m.skip_launcher = false; }
        else                  { m.show_skip_modal = true; }
        dirty_all();
    });
    c.BindEventCallback("skip_modal_confirm", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.skip_launcher = true; m.skip_launcher = true; m.show_skip_modal = false; dirty_all();
    });
    c.BindEventCallback("skip_modal_cancel", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        m.show_skip_modal = false; dirty_all();
    });

    // ---- display / audio settings ----
    c.BindEventCallback("cycle_scale", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.window_scale = io.window_scale >= 6 ? 1 : io.window_scale + 1; refresh_settings_labels(m, io); dirty_all();
    });
    c.BindEventCallback("cycle_renderer", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.renderer = io.renderer ? 0 : 1; refresh_settings_labels(m, io); dirty_all();
    });
    c.BindEventCallback("toggle_integer_scale", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.integer_scale = !io.integer_scale; m.integer_scale = io.integer_scale; dirty_all();
    });
    c.BindEventCallback("toggle_widescreen", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.widescreen = !io.widescreen; m.widescreen = io.widescreen; dirty_all();
    });
    c.BindEventCallback("toggle_filter", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.linear_filter = !io.linear_filter; m.filter = io.linear_filter; dirty_all();
    });
    c.BindEventCallback("vol_up", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.volume = io.volume >= 100 ? 100 : io.volume + 5; m.volume = io.volume; dirty_all();
    });
    c.BindEventCallback("vol_down", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.volume = io.volume <= 0 ? 0 : io.volume - 5; m.volume = io.volume; dirty_all();
    });

    // ---- HD texture pack ----
    c.BindEventCallback("toggle_hdpack", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.hdpack_enabled = !io.hdpack_enabled; refresh_hdpack(m, io); dirty_all();
    });
    c.BindEventCallback("choose_hdpack", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        char buf[512];
        if (pick_folder("Select HD Texture Pack Folder", buf, sizeof(buf))) {
            std::snprintf(io.hdpack_dir, sizeof(io.hdpack_dir), "%s", buf);
            io.hdpack_enabled = true;          // picking a pack implies enabling it
            refresh_hdpack(m, io);
            dirty_all();
        }
    });
    c.BindEventCallback("clear_hdpack", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        io.hdpack_dir[0] = '\0';   // back to the default <exe>/hdpack (stays enabled)
        refresh_hdpack(m, io); dirty_all();
    });

    // ---- controller config ----
    c.BindEventCallback("cfg_dz_up", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        int p = m.cfg_player; io.deadzone[p] = io.deadzone[p] >= 100 ? 100 : io.deadzone[p] + 5;
        m.cfg_deadzone = io.deadzone[p]; dirty_all();
    });
    c.BindEventCallback("cfg_dz_down", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        int p = m.cfg_player; io.deadzone[p] = io.deadzone[p] <= 0 ? 0 : io.deadzone[p] - 5;
        m.cfg_deadzone = io.deadzone[p]; dirty_all();
    });

    // ---- play / quit ----
    c.BindEventCallback("play", [&](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (rom_path.empty()) {
            char buf[1024];
            if (pick_file("Select NES ROM", "NES ROMs (*.nes)\0*.nes\0All Files (*.*)\0*.*\0", buf, sizeof(buf))) {
                rom_path = buf; load_rom_info(m, game, rom_path); refresh_save_info(m);
            } else { return; }
        }
        if (out_rom_path && out_rom_path_len)
            std::snprintf(out_rom_path, out_rom_path_len, "%s", rom_path.c_str());
        result = Result::Launch;
        running = false;
    });

    Rml::ElementDocument* doc = context->LoadDocument((assets / "launcher.rml").generic_string());
    if (!doc) {
        std::fprintf(stderr, "launcher: failed to load launcher.rml — booting without launcher\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    doc->Show();

    // ---- populate the controller <select> dropdowns programmatically ----
    struct SelListener : Rml::EventListener {
        std::function<void()> on_change;
        void ProcessEvent(Rml::Event&) override { if (on_change) on_change(); }
    };
    std::vector<std::unique_ptr<SelListener>> sel_listeners;
    auto setup_select = [&](const char* id, int p) {
        auto* sel = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(doc->GetElementById(id));
        if (!sel) return;
        sel->RemoveAll();
        const std::vector<SrcOption>& opts = (p == 0) ? m.p1_options : m.p2_options;
        Rml::String cur = src_to_value(io.player_src[p]);
        int selected = 0;
        for (int i = 0; i < (int)opts.size(); i++) {
            sel->Add(opts[i].label, opts[i].value);
            if (opts[i].value == cur) selected = i;
        }
        sel->SetSelection(selected);
        auto lis = std::make_unique<SelListener>();
        lis->on_change = [&, sel, p]() {
            io.player_src[p] = value_to_src(sel->GetValue());
            refresh_settings_labels(m, io);
            dirty_all();
        };
        sel->AddEventListener(Rml::EventId::Change, lis.get());
        sel_listeners.push_back(std::move(lis));
    };
    setup_select("p1src", 0);
    setup_select("p2src", 1);

    if (auto* pb = doc->GetElementById("play")) pb->Focus();

    // ---- gamepad navigation ----
    auto nav_back = [&]() { if (m.view != "dashboard") { m.view = "dashboard"; dirty_all(); } };
    auto pad_move = [&](int dir) {
        context->ProcessKeyDown(Rml::Input::KI_TAB, dir < 0 ? (int)Rml::Input::KM_SHIFT : 0);
    };
    int pad_zone_x = 0, pad_zone_y = 0;
    auto handle_pad = [&](const SDL_Event& e) {
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: pad_move(+1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP:
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  pad_move(-1); break;
                case SDL_CONTROLLER_BUTTON_A:
                    context->ProcessKeyDown(Rml::Input::KI_RETURN, 0); break;
                case SDL_CONTROLLER_BUTTON_B: nav_back(); break;
                case SDL_CONTROLLER_BUTTON_START:
                    if (auto* pb = doc->GetElementById("play")) pb->Click(); break;
                default: break;
            }
        } else if (e.type == SDL_CONTROLLERAXISMOTION) {
            const int TH = 18000;
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                int z = e.caxis.value > TH ? 1 : e.caxis.value < -TH ? -1 : 0;
                if (z != pad_zone_y) { pad_zone_y = z; if (z) pad_move(z); }
            } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                int z = e.caxis.value > TH ? 1 : e.caxis.value < -TH ? -1 : 0;
                if (z != pad_zone_x) { pad_zone_x = z; if (z) pad_move(z); }
            }
        }
    };

    // ---- main loop ----
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { result = Result::Quit; running = false; }
            else if (ev.type == SDL_WINDOWEVENT &&
                     (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                      ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
                SDL_GL_GetDrawableSize(window, &win_w, &win_h);
                render_interface.SetViewport(win_w, win_h);
                context->SetDimensions(Rml::Vector2i(win_w, win_h));
                RmlSDL::InputEventHandler(context, ev);
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN ||
                       ev.type == SDL_CONTROLLERAXISMOTION) {
                handle_pad(ev);
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                if (SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which))
                    open_pads.push_back(gc);
            } else {
                RmlSDL::InputEventHandler(context, ev);
            }
        }

        context->Update();

        render_interface.Clear();
        render_interface.BeginFrame();
        context->Render();
        render_interface.EndFrame();
        SDL_GL_SwapWindow(window);
        SDL_Delay(8);
    }

    for (SDL_GameController* gc : open_pads) SDL_GameControllerClose(gc);

    Rml::Shutdown();
    RmlGL3::Shutdown();
    return result;
}

} // namespace nes_launcher

// ----------------------------------------------------------------------------
// C entry point (launcher_capi.h) — owns the launcher window/GL context.
// ----------------------------------------------------------------------------

#include "launcher_capi.h"

extern "C" int nes_launcher_run_window(const char* window_title,
                                       NesLauncherCSettings* io,
                                       const NesLauncherCGameInfo* game,
                                       const char* assets_dir,
                                       const char* initial_rom,
                                       char* out_rom_path,
                                       size_t out_rom_path_len) {
    using namespace nes_launcher;
    if (!io || !game) return 2;

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "launcher: SDL video init failed: %s\n", SDL_GetError());
        return 2;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* win = SDL_CreateWindow(
        window_title ? window_title : "NES Launcher",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,  // 4:3
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::fprintf(stderr, "launcher: window creation failed: %s\n", SDL_GetError());
        return 2;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        std::fprintf(stderr, "launcher: GL context creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        return 2;
    }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);

    NesLauncherSettings s;
    s.window_scale  = io->window_scale;
    s.fullscreen    = io->fullscreen;
    s.integer_scale = io->integer_scale != 0;
    s.linear_filter = io->linear_filter != 0;
    s.renderer      = io->renderer;
    s.widescreen    = io->widescreen != 0;
    s.volume        = io->volume;
    s.player_src[0] = (InputSource)io->player_src[0];
    s.player_src[1] = (InputSource)io->player_src[1];
    s.deadzone[0]   = io->deadzone[0];
    s.deadzone[1]   = io->deadzone[1];
    s.skip_launcher = io->skip_launcher != 0;
    s.hdpack_enabled = io->hdpack_enabled != 0;
    std::snprintf(s.hdpack_dir, sizeof(s.hdpack_dir), "%s", io->hdpack_dir);

    GameInfo g;
    g.name             = game->name;
    g.region           = game->region;
    g.expected_crc     = game->expected_crc;
    g.has_expected_crc = game->has_expected_crc != 0;
    g.mapper_board     = game->mapper_board;
    g.uses_sram        = game->uses_sram != 0;
    g.save_basename    = game->save_basename;
    g.widescreen_supported = game->widescreen_supported != 0;
    g.hdpack_supported     = game->hdpack_supported != 0;
    g.password_save_path   = game->password_save_path;
    g.password_save_label  = game->password_save_label;

    Result r = run(win, ctx, s, g, assets_dir, initial_rom,
                   out_rom_path, out_rom_path_len);

    io->window_scale  = s.window_scale;
    io->fullscreen    = s.fullscreen;
    io->integer_scale = s.integer_scale;
    io->linear_filter = s.linear_filter;
    io->renderer      = s.renderer;
    io->widescreen    = s.widescreen;
    io->volume        = s.volume;
    io->player_src[0] = (int)s.player_src[0];
    io->player_src[1] = (int)s.player_src[1];
    io->deadzone[0]   = s.deadzone[0];
    io->deadzone[1]   = s.deadzone[1];
    io->skip_launcher = s.skip_launcher;
    io->hdpack_enabled = s.hdpack_enabled;
    std::snprintf(io->hdpack_dir, sizeof(io->hdpack_dir), "%s", s.hdpack_dir);

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_GL_ResetAttributes();

    return r == Result::Launch ? 0 : (r == Result::Quit ? 1 : 2);
}
