#include "mod_runtime.h"
#include "crc32.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
static void activation() {}
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::vector<uint8_t> read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}
int main(int argc, char** argv) {
    // mode, content-free recipe, catalog, game id, original ROM, raw download.
    if (argc != 7 && argc != 8) return 2;
    try {
        const std::string mode = argv[1];
        const char* package_id = argc == 8 ? argv[7] : "test.mesen";
        const auto original = read(argv[5]);
        check(original.size() >= 16, "fixture ROM");
        char crc[9]; std::snprintf(crc, sizeof(crc), "%08x", crc32_compute(original.data()+16, original.size()-16));
        check(nes_mod_register_activation_plugin("nesrecomp.hd-pack", activation), "plugin registration");
        std::string error;
        check(NESRecomp::mod_runtime_initialize(argv[3], argv[4], crc, &error), error.c_str());
        const auto* provider = NESRecomp::mod_runtime_launcher_provider();
        if (mode != "resume" && mode != "resume-legacy") {
            if (mode != "preloaded") {
                const bool installed = provider->install_archive(provider->ctx, argv[2]);
                check(installed, provider->last_error(provider->ctx));
            }
            const bool stock_committed = provider->commit(provider->ctx, argv[5]);
            check(stock_committed, provider->last_error(provider->ctx));
            check(!nes_mod_hd_pack(), "recipe enabled itself");
            check(provider->feature_resource_count(provider->ctx, package_id, "hd-pack") == 1, "missing Browse resource");
            RecompLauncherCModResource resource{};
            check(provider->feature_resource_get(provider->ctx, package_id, "hd-pack", 0, &resource), "resource info");
            check(resource.required && !resource.verified && std::strstr(resource.file_patterns, "*.zip"), "resource contract");
            check(!provider->feature_enable(provider->ctx, package_id, "hd-pack", 1), "enabled without a download");
            const bool imported = provider->feature_resource_set_path(provider->ctx, package_id, "hd-pack", "source-pack", argv[6]);
            if (mode == "reject" || mode == "legacy") {
                check(!imported, "unsafe or unsupported download accepted");
                check(!nes_mod_hd_pack(), "invalid import exposed assets");
                const auto imports = fs::path(argv[3])/"imports";
                if (fs::is_directory(imports)) {
                    for (const auto& entry : fs::directory_iterator(imports))
                        check(entry.path().filename().string().rfind(".importing-", 0) != 0, "failed extraction leaked staging");
                    if (mode == "legacy") check(fs::is_empty(imports), "legacy pack published into the cache");
                }
                if (mode == "legacy")
                    check(std::strstr(provider->last_error(provider->ctx), "native compatibility provider"),
                          "missing compatibility status");
                check(read(argv[5]) == original, "refused import changed original ROM");
                std::cout << "PASS: unsafe input rejected: " << provider->last_error(provider->ctx) << '\n';
                return 0;
            }
            check(imported, provider->last_error(provider->ctx));
            check(provider->feature_resource_get(provider->ctx, package_id, "hd-pack", 0, &resource) && resource.verified, "Browse did not report import");
            const bool enabled = provider->feature_enable(provider->ctx, package_id, "hd-pack", 1);
            check(enabled, provider->last_error(provider->ctx));
        }
        const bool committed = provider->commit(provider->ctx, argv[5]);
        if (mode == "resume-legacy") {
            check(committed && !nes_mod_hd_pack(), "legacy cache activated or blocked stock play");
            RecompLauncherCModResource resource{};
            check(provider->feature_resource_get(provider->ctx, package_id, "hd-pack", 0, &resource) &&
                  !resource.verified, "legacy cache reported as ready");
            check(std::strstr(resource.status, "native compatibility provider"),
                  "legacy cache lost compatibility status");
            check(!provider->feature_enable(provider->ctx, package_id, "hd-pack", 1),
                  "legacy cache could be enabled");
            check(read(argv[5]) == original, "legacy cache changed original ROM");
            std::cout << "PASS: stored legacy cache refused at Play\n";
            return 0;
        }
        check(committed, provider->last_error(provider->ctx));
        const auto* pack = nes_mod_hd_pack();
        check(pack && fs::is_regular_file(fs::path(pack->directory)/"hires.txt"), "imported assets unavailable");
        check(read(argv[5]) == original, "original ROM was changed");
        check(mode != "bad-base", "unsupported original revision accepted");
        const auto cache = fs::path(pack->directory).parent_path();
        check(fs::is_directory(cache/"notices"), "creator notices not retained");
        nes_mod_lock_hd_pack();
        check(!provider->feature_resource_set_path(provider->ctx, package_id, "hd-pack", "source-pack", argv[6]), "changed running pack");
        check(nes_mod_hd_pack() == pack, "refused live edit changed active pack");
        std::cout << "PASS: raw download imported through content-free recipe; ROM unchanged; stable committed pack\n";
        std::cout << "directory=" << pack->directory << '\n';
        return 0;
    } catch (const std::exception& error) {
        const std::string mode = argv[1];
        const std::string message = error.what();
        if (mode == "bad-base" && message.find("does not support this original ROM revision") != std::string::npos) {
            std::cout << "PASS: incompatible original ROM refused: " << error.what() << '\n'; return 0;
        }
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
