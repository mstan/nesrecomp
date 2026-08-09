#include "mod_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kRomSize = 16u * 1024u * 1024u;
constexpr const char* kPackageId = "test.external-rom";
constexpr const char* kFeatureId = "fighter";
constexpr const char* kPluginId = "test.external-rom.plugin";
constexpr const char* kSha1 = "606e809392c9f1363cfd83b2f80aa66b5bd0e990";

int failures;
int activations;
fs::path expected_activation_path;
bool activation_saw_committed_path;

void activate_test_plugin() {
    ++activations;
    const char* path = nes_mod_external_rom_path(
        kPackageId, kFeatureId, "source-rom");
    activation_saw_committed_path =
        path && fs::path(path) == expected_activation_path;
}

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string toml_string(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') result.push_back('\\');
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}

void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    expect(static_cast<bool>(file), "write fixture " + path.string());
}

void write_catalog(const fs::path& root, const fs::path& resource_path,
                   const std::string& format = "n64",
                   bool duplicate_resource_id = false) {
    const fs::path package =
        root / "packages" / kPackageId / "1.0.0";
    fs::create_directories(package);
    std::ofstream manifest(package / "manifest.toml", std::ios::trunc);
    manifest
        << "format_version = 1\n"
        << "id = \"" << kPackageId << "\"\n"
        << "version = \"1.0.0\"\n"
        << "name = \"External ROM Gate Test\"\n"
        << "resolver = \"declarative\"\n\n"
        << "[[target]]\n"
        << "game_id = \"test-game\"\n"
        << "rom_crc32 = \"00000000\"\n\n"
        << "[[feature]]\n"
        << "id = \"" << kFeatureId << "\"\n"
        << "name = \"Synthetic fighter\"\n"
        << "default_enabled = false\n\n";
    if (duplicate_resource_id) {
        manifest
            << "[[feature]]\n"
            << "id = \"second-fighter\"\n"
            << "name = \"Second synthetic fighter\"\n"
            << "default_enabled = false\n\n";
    }
    auto external_rom = [&](const char* feature, const char* id,
                            const char* identity) {
        manifest
            << "[[external_rom]]\n"
            << "feature = \"" << feature << "\"\n"
            << "id = \"" << id << "\"\n"
            << "label = \"Synthetic source ROM\"\n"
            << "description = \"Generated test data.\"\n"
            << "format = \"" << format << "\"\n"
            << "identity = \"" << identity << "\"\n"
            << "size = " << kRomSize << "\n"
            << "normalized_sha1 = \"" << kSha1 << "\"\n\n";
    };
    external_rom(kFeatureId, "source-rom", "Synthetic canonical N64 image");
    manifest
        << "[[feature]]\n"
        << "id = \"inactive-fighter\"\n"
        << "name = \"Inactive synthetic fighter\"\n"
        << "default_enabled = false\n\n";
    external_rom("inactive-fighter", "uncommitted-source",
                 "Uncommitted synthetic source ROM");
    if (duplicate_resource_id)
        external_rom("second-fighter", "source-rom",
                     "Duplicate package-scoped id");
    manifest
        << "[[plugin]]\n"
        << "feature = \"" << kFeatureId << "\"\n"
        << "id = \"" << kPluginId << "\"\n";
    expect(static_cast<bool>(manifest), "write synthetic manifest");

    if (duplicate_resource_id) return;
    std::ofstream state(root / "state.toml", std::ios::trunc);
    state
        << "format_version = 1\n\n"
        << "[[package]]\n"
        << "id = \"" << kPackageId << "\"\n"
        << "version = \"1.0.0\"\n\n"
        << "[[feature]]\n"
        << "package_id = \"" << kPackageId << "\"\n"
        << "id = \"" << kFeatureId << "\"\n"
        << "enabled = true\n\n"
        << "[[resource]]\n"
        << "package_id = \"" << kPackageId << "\"\n"
        << "id = \"source-rom\"\n"
        << "path = " << toml_string(resource_path.string()) << "\n\n"
        << "[[resource]]\n"
        << "package_id = \"" << kPackageId << "\"\n"
        << "id = \"uncommitted-source\"\n"
        << "path = " << toml_string(resource_path.string()) << "\n";
    expect(static_cast<bool>(state), "write synthetic state");
}

bool initialize(const fs::path& root, std::string& error) {
    return NESRecomp::mod_runtime_initialize(
        root, "test-game", "00000000", &error);
}

void expect_valid_activation(const fs::path& base, const std::string& name,
                             const fs::path& resource,
                             const std::string& format = "n64") {
    const fs::path root = base / name;
    write_catalog(root, resource, format);
    std::string error;
    activations = 0;
    activation_saw_committed_path = false;
    expected_activation_path = resource;
    expect(initialize(root, error), name + " initializes: " + error);
    expect(nes_mod_external_rom_path(
               kPackageId, kFeatureId, "source-rom") == nullptr,
           name + " does not expose an owner ROM before commit");
    error.clear();
    expect(NESRecomp::mod_runtime_commit({}, &error),
           name + " commits: " + error);
    const char* committed_path = nes_mod_external_rom_path(
        kPackageId, kFeatureId, "source-rom");
    expect(committed_path && fs::path(committed_path) == resource,
           name + " exposes exactly its committed verified owner ROM path");
    expect(nes_mod_external_rom_path(
               kPackageId, "other-feature", "source-rom") == nullptr,
           name + " cannot expose an unrelated feature resource");
    expect(nes_mod_external_rom_path(
               "other-package", kFeatureId, "source-rom") == nullptr,
           name + " cannot expose an unrelated package resource");
    expect(nes_mod_external_rom_path(
               kPackageId, "inactive-fighter", "uncommitted-source") == nullptr,
           name + " cannot expose a selected but uncommitted feature resource");
    NESRecomp::mod_runtime_activate_plugins();
    expect(activations == 1, name + " activates exactly one plugin");
    expect(activation_saw_committed_path,
           name + " exposes the path to its trusted activation callback");
}

void expect_invalid_resource(const fs::path& base, const std::string& name,
                             const fs::path& resource) {
    const fs::path root = base / name;
    write_catalog(root, resource);
    std::string error;
    activations = 0;
    expect(initialize(root, error),
           name + " initializes fail-closed instead of aborting catalog: " +
               error);
    error.clear();
    expect(NESRecomp::mod_runtime_commit({}, &error),
           name + " commits a vanilla plan: " + error);
    NESRecomp::mod_runtime_activate_plugins();
    expect(activations == 0, name + " cannot activate its plugin");
}

}  // namespace

int main() {
    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() /
        ("nesrecomp_external_rom_gate_" + std::to_string(nonce));
    fs::create_directories(base);

    expect(nes_mod_register_activation_plugin(kPluginId,
                                              activate_test_plugin) == 1,
           "register synthetic activation plugin");

    std::vector<uint8_t> bytes(kRomSize, 0);
    bytes[0] = 0x80;
    bytes[1] = 0x37;
    bytes[2] = 0x12;
    bytes[3] = 0x40;
    const fs::path z64 = base / "valid.z64";
    write_bytes(z64, bytes);

    std::vector<uint8_t> reordered = bytes;
    for (size_t i = 0; i + 1 < reordered.size(); i += 2)
        std::swap(reordered[i], reordered[i + 1]);
    const fs::path v64 = base / "valid.v64";
    write_bytes(v64, reordered);

    reordered = bytes;
    for (size_t i = 0; i + 3 < reordered.size(); i += 4) {
        std::swap(reordered[i], reordered[i + 3]);
        std::swap(reordered[i + 1], reordered[i + 2]);
    }
    const fs::path n64 = base / "valid.n64";
    write_bytes(n64, reordered);

    std::vector<uint8_t> wrong = bytes;
    wrong[4096] ^= 1;
    const fs::path wrong_path = base / "wrong.z64";
    write_bytes(wrong_path, wrong);
    const fs::path missing = base / "missing.z64";

    expect_valid_activation(base, "z64", z64);
    expect_valid_activation(base, "v64", v64);
    expect_valid_activation(base, "n64", n64);
    expect_valid_activation(base, "raw", z64, "raw");
    expect_invalid_resource(base, "missing", missing);
    expect_invalid_resource(base, "wrong", wrong_path);

    {
        const fs::path race = base / "race.z64";
        write_bytes(race, bytes);
        const fs::path root = base / "toctou";
        write_catalog(root, race);
        std::string error;
        activations = 0;
        expect(initialize(root, error), "TOCTOU initializes: " + error);
        error.clear();
        expect(NESRecomp::mod_runtime_commit({}, &error),
               "TOCTOU first commit succeeds: " + error);
        NESRecomp::mod_runtime_activate_plugins();
        expect(activations == 1, "TOCTOU first plan activates");
        activations = 0;
        {
            std::fstream file(race,
                              std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(4096);
            file.put(1);
        }
        error.clear();
        expect(!NESRecomp::mod_runtime_commit({}, &error),
               "TOCTOU changed file fails recommit");
        expect(nes_mod_external_rom_path(
                   kPackageId, kFeatureId, "source-rom") == nullptr,
               "failed recommit clears the previously exposed owner ROM path");
        NESRecomp::mod_runtime_activate_plugins();
        expect(activations == 0,
               "failed recommit clears the previously committed plugin plan");
    }

    {
        const fs::path root = base / "duplicate-id";
        write_catalog(root, z64, "n64", true);
        std::string error;
        expect(!initialize(root, error),
               "duplicate package-scoped resource id rejects catalog");
        expect(error.find("invalid external ROM resource") != std::string::npos,
               "duplicate id reports external ROM manifest error");
    }

    std::error_code cleanup_error;
    fs::remove_all(base, cleanup_error);
    if (cleanup_error) {
        ++failures;
        std::cerr << "FAIL: cannot remove test directory: "
                  << cleanup_error.message() << '\n';
    }
    if (failures) {
        std::cerr << failures << " external ROM gate assertion(s) failed\n";
        return 1;
    }
    std::cout << "external ROM gate self-test passed\n";
    return 0;
}
