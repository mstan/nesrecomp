#include "mod_runtime.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static std::vector<char> activated;
static void a() { activated.push_back('a'); }
static void y() { activated.push_back('y'); }
static void z() { activated.push_back('z'); }
static void other() {}
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void package(const fs::path& root, const char* id, const std::string& fields = "", const char* game = "test-game") {
    const auto dir = root / "packages" / id / "1.0.0";
    fs::create_directories(dir);
    std::ofstream f(dir / "manifest.toml");
    f << "format_version=1\nid=\"" << id << "\"\nversion=\"1.0.0\"\nname=\"" << id << "\"\nresolver=\"declarative\"\n"
         "[[target]]\ngame_id=\"" << game << "\"\nrom_crc32=\"00000000\"\n"
         "[[feature]]\nid=\"mode\"\nname=\"" << id << "\"\ndefault_enabled=false\n" << fields <<
         "\n[[plugin]]\nfeature=\"mode\"\nid=\"" << id << "\"\n"
         "[[option]]\nfeature=\"mode\"\nid=\"view\"\nlabel=\"View\"\ntype=\"choice\"\ndefault=\"shared\"\n"
         "[[option.choice]]\nvalue=\"shared\"\nlabel=\"Shared\"\n[[option.choice]]\nvalue=\"split\"\nlabel=\"Split\"\n";
}
static bool enabled(const char* id) { return nes_mod_feature_selected(id, "mode") != 0; }
static void initialize(const fs::path& root) {
    std::string error;
    check(NESRecomp::mod_runtime_initialize(root, "test-game", "00000000", &error), error.c_str());
}
int main() {
    const fs::path root = fs::absolute("feature-dependency-fixtures");
    try {
        check(!fs::exists(root), "fixture directory already exists");
        for (const char* id : {"test.a", "test.y", "test.z", "test.other", "test.old", "test.new"})
            check(nes_mod_register_activation_plugin(id, std::string(id) == "test.a" ? a :
                std::string(id) == "test.y" ? y : std::string(id) == "test.z" ? z : other), "register plugin");
        const auto good = root / "good";
        package(good, "test.a", "requires=[\"test.y/mode\"]");
        package(good, "test.y", "requires=[\"test.z/mode\"]");
        package(good, "test.z", "exclusive_group=\"body\"");
        package(good, "test.other", "exclusive_group=\"body\"");
        initialize(good);
        const auto* p = NESRecomp::mod_runtime_launcher_provider();
        check(p->feature_enable(p->ctx, "test.a", "mode", 1), "enable root and its transitive dependencies");
        check(enabled("test.a") && enabled("test.y") && enabled("test.z"), "required feature remains disabled");
        std::string error;
        check(NESRecomp::mod_runtime_commit({}, &error), error.c_str());
        NESRecomp::mod_runtime_activate_plugins();
        check(activated == std::vector<char>({'z','y','a'}), "dependencies did not activate first");
        bool status = false;
        for (int i = 0; i < p->feature_count(p->ctx); ++i) {
            RecompLauncherCModFeature f{}; check(p->feature_get(p->ctx, i, &f), "feature metadata");
            if (std::string(f.package_id) == "test.z") status = std::string(f.status).find("Required by") == 0;
        }
        check(status, "dependency relationship missing from launcher status");
        check(p->feature_enable(p->ctx, "test.a", "mode", 0), "disable dependent");
        check(!enabled("test.a") && enabled("test.z"), "disabling a dependent removed shared support");
        check(!p->feature_enable(p->ctx, "test.other", "mode", 1), "exclusion silently broke an enabled dependent");
        check(enabled("test.y") && enabled("test.z") && !enabled("test.other"), "failed enable changed selections");
        check(p->feature_enable(p->ctx, "test.a", "mode", 1), "restore root");
        nes_mod_lock_feature("test.a", "mode");
        check(!p->feature_enable(p->ctx, "test.z", "mode", 0), "dependency cascade bypassed a running feature lock");
        check(enabled("test.a") && enabled("test.y") && enabled("test.z"), "failed cascade partially changed selections");
        initialize(good);
        check(p->feature_enable(p->ctx, "test.z", "mode", 0), "disable dependency cascade");
        check(!enabled("test.a") && !enabled("test.y") && !enabled("test.z"), "dependent remained enabled without its requirement");
        check(NESRecomp::mod_runtime_commit({}, &error), error.c_str());
        initialize(good);
        check(!enabled("test.a") && !enabled("test.z"), "disabled dependency graph reenabled itself");
        {
            std::ofstream state(good / "state.toml");
            state << "format_version=1\n[[feature]]\npackage_id=\"test.a\"\nid=\"mode\"\nenabled=true\n"
                     "[[feature]]\npackage_id=\"test.z\"\nid=\"mode\"\nenabled=false\n";
        }
        initialize(good);
        check(enabled("test.a") && enabled("test.y") && enabled("test.z"), "headless persisted requirements were not resolved");
        check(nes_mod_runtime_commit_netplay_c(nullptr), "netplay empty-plan commit failed");
        activated.clear(); NESRecomp::mod_runtime_activate_plugins();
        check(activated.empty() && enabled("test.a") && enabled("test.z"), "netplay commit mutated offline selections");

        for (const auto& kind : {"missing", "cycle", "target", "resource"}) {
            const auto dir = root / kind;
            package(dir, "test.a", "requires=[\"test.z/mode\"]");
            if (std::string(kind) == "cycle") package(dir, "test.z", "requires=[\"test.a/mode\"]");
            if (std::string(kind) == "target") package(dir, "test.z", "", "other-game");
            if (std::string(kind) == "resource") {
                package(dir, "test.z");
                std::ofstream f(dir / "packages/test.z/1.0.0/manifest.toml", std::ios::app);
                f << "[[external_rom]]\nfeature=\"mode\"\nid=\"source\"\nlabel=\"Source\"\nformat=\"raw\"\nidentity=\"Source\"\nsize=1\nnormalized_sha1=\"0000000000000000000000000000000000000000\"\n";
            }
            initialize(dir);
            check(!p->feature_enable(p->ctx, "test.a", "mode", 1), "invalid requirement was accepted");
            check(!enabled("test.a") && !enabled("test.z"), "invalid requirement partially enabled features");
        }
        const auto migration = root / "migration";
        package(migration, "test.old");
        package(migration, "test.new", "requires=[\"test.old/mode\"]\nmigrate_from_option=\"test.old/mode/two-player\"");
        {
            std::ofstream state(migration / "state.toml");
            state << "format_version=1\n[[feature]]\npackage_id=\"test.old\"\nid=\"mode\"\nenabled=true\n"
                     "[feature.values]\ntwo-player=\"true\"\nview=\"split\"\n";
        }
        initialize(migration);
        check(enabled("test.old") && enabled("test.new"), "legacy boolean option was not migrated");
        RecompLauncherCModOption option{};
        check(p->feature_option_get(p->ctx, "test.new", "mode", 0, &option) && std::string(option.value) == "split", "legacy option value lost");
        check(p->feature_enable(p->ctx, "test.new", "mode", 0), "disable migrated feature");
        check(NESRecomp::mod_runtime_commit({}, &error), error.c_str());
        initialize(migration);
        check(!enabled("test.new") && enabled("test.old"), "legacy setting overrode an explicit new disable");
        check(p->feature_option_get(p->ctx, "test.new", "mode", 0, &option) && std::string(option.value) == "split", "migrated option was not persisted");
        package(migration, "test.new", "requires=[\"bad reference\"]");
        check(!NESRecomp::mod_runtime_initialize(migration, "test-game", "00000000", &error), "malformed requirement accepted");
        fs::remove_all(root); // Only the exact fresh fixture directory created by this test.
        std::cout << "PASS: dependency selection, activation order, rollback, locks, persistence and option migration\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
