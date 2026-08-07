#include "mod_runtime.h"

#include "crc32.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace NESRecomp {
namespace {

constexpr uint64_t kMaxArchiveBytes = 256ull * 1024ull * 1024ull;
constexpr uint32_t kMaxArchiveFiles = 4096;

enum class OptionType {
    Boolean,
    Choice,
    Integer,
};

struct Choice {
    std::string value;
    std::string label;
};

struct Option {
    std::string feature_id;
    std::string id;
    std::string label;
    std::string description;
    std::string group;
    OptionType type = OptionType::Boolean;
    std::string default_value = "false";
    int64_t min_value = 0;
    int64_t max_value = 1;
    int64_t step = 1;
    std::vector<Choice> choices;
};

/*
 * A trusted plugin selected by a feature, optionally conditioned on one of
 * that feature's option values.  Without a condition the plugin activates
 * whenever the feature is enabled (the historical format 1 behavior).  With
 * one, the feature can offer several implementations and let a choice option
 * pick between them, instead of forcing one pseudo-feature per value.
 */
struct PluginRef {
    std::string id;
    std::string when_option;
    std::string when_value;
};

struct Feature {
    std::string id;
    std::string name;
    std::string description;
    std::string group = "General";
    std::string exclusive_group;
    bool default_enabled = false;
    bool camera_controls = false;
    std::vector<PluginRef> plugins;
};

struct Target {
    std::string game_id;
    std::string rom_crc32;
};

struct Package {
    uint32_t format_version = 0;
    std::string id;
    std::string version;
    std::string name;
    std::string author;
    std::string description;
    std::string license;
    fs::path root;
    std::vector<Target> targets;
    std::vector<Feature> features;
    std::vector<Option> options;
};

struct FeatureSelection {
    bool enabled = false;
    bool has_enabled = false;
    std::map<std::string, std::string> values;
};

struct PackageSelection {
    std::string version;
    std::map<std::string, FeatureSelection> features;
};

struct Diagnostic {
    std::string package_id;
    std::string feature_id;
    std::string related_package_id;
    std::string related_feature_id;
    std::string resource;
    std::string message;
};

struct ResolvedPlugin {
    std::string id;
    std::string package_id;
    std::string feature_id;
    NESModActivationCallback callback = nullptr;
};

struct Validation {
    bool ok = true;
    std::vector<Diagnostic> diagnostics;
    std::vector<ResolvedPlugin> plugins;
};

struct RegisteredPlugin {
    NESModActivationCallback activation = nullptr;
};

std::map<std::string, RegisteredPlugin>& registered_plugins() {
    static std::map<std::string, RegisteredPlugin> value;
    return value;
}

std::vector<NESModActivationCallback>& reset_callbacks() {
    static std::vector<NESModActivationCallback> value;
    return value;
}

struct Runtime {
    fs::path root;
    std::string game_id;
    std::string rom_crc32;
    std::map<std::string, std::map<std::string, Package>> packages;
    std::map<std::string, PackageSelection> selections;
    Validation validation;
    Validation committed;
    std::string error;
    bool initialized = false;
};

Runtime& state() {
    static Runtime value;
    return value;
}

void set_error(std::string* out, const std::string& value) {
    if (out) *out = value;
}

std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string strip_comment(const std::string& line) {
    bool quoted = false;
    bool escape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (quoted && c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (c == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

bool parse_string(const std::string& text, std::string& out) {
    const std::string value = trim(text);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;
    out.clear();
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char c = value[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i + 1 >= value.size()) return false;
        switch (value[i]) {
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return false;
        }
    }
    return true;
}

bool parse_bool(const std::string& text, bool& out) {
    const std::string value = trim(text);
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

bool parse_int(const std::string& text, int64_t& out) {
    const std::string value = trim(text);
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno || !end || *end != '\0') return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 96 ||
        value.front() == '.' || value.back() == '.')
        return false;
    for (unsigned char c : value) {
        if (!(std::islower(c) || std::isdigit(c) ||
              c == '.' || c == '-' || c == '_'))
            return false;
    }
    return true;
}

bool valid_crc32(const std::string& value) {
    return value.size() == 8 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) || (c >= 'a' && c <= 'f');
        });
}

const Feature* find_feature(const Package& package,
                            const std::string& feature_id) {
    const auto it = std::find_if(
        package.features.begin(), package.features.end(),
        [&](const Feature& feature) { return feature.id == feature_id; });
    return it == package.features.end() ? nullptr : &*it;
}

const Option* find_option(const Package& package,
                          const std::string& feature_id,
                          const std::string& option_id) {
    const auto it = std::find_if(
        package.options.begin(), package.options.end(),
        [&](const Option& option) {
            return option.feature_id == feature_id && option.id == option_id;
        });
    return it == package.options.end() ? nullptr : &*it;
}

bool validate_option_value(const Option& option, const std::string& value,
                           std::string* error) {
    if (option.type == OptionType::Boolean) {
        if (value == "true" || value == "false") return true;
        set_error(error, option.label + " must be true or false");
        return false;
    }
    if (option.type == OptionType::Choice) {
        const bool found = std::any_of(
            option.choices.begin(), option.choices.end(),
            [&](const Choice& choice) { return choice.value == value; });
        if (found) return true;
        set_error(error, option.label + " has an unknown choice");
        return false;
    }
    int64_t parsed = 0;
    if (!parse_int(value, parsed) ||
        parsed < option.min_value || parsed > option.max_value ||
        option.step <= 0 ||
        (parsed - option.min_value) % option.step != 0) {
        set_error(error, option.label + " is outside its allowed range");
        return false;
    }
    return true;
}

bool read_manifest(const fs::path& path, Package& out, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        set_error(error, "cannot open manifest: " + path.string());
        return false;
    }

    enum class Section {
        Package,
        Target,
        Feature,
        Option,
        Choice,
        Plugin,
    };
    Section section = Section::Package;
    Target* target = nullptr;
    Feature* feature = nullptr;
    Option* option = nullptr;
    Choice* choice = nullptr;
    std::string plugin_feature;
    std::string plugin_id;
    std::string plugin_when_option;
    std::string plugin_when_value;

    auto finish_plugin = [&]() -> bool {
        if (plugin_feature.empty() && plugin_id.empty() &&
            plugin_when_option.empty() && plugin_when_value.empty())
            return true;
        Feature* owner = nullptr;
        for (Feature& candidate : out.features) {
            if (candidate.id == plugin_feature) {
                owner = &candidate;
                break;
            }
        }
        if (!owner || !valid_id(plugin_id)) {
            set_error(error, "plugin has an invalid feature or id");
            return false;
        }
        /* A condition needs both halves; one alone is always a mistake. */
        if (plugin_when_option.empty() != plugin_when_value.empty()) {
            set_error(error, "plugin condition requires both when_option "
                             "and when_value");
            return false;
        }
        owner->plugins.push_back(
            PluginRef{plugin_id, plugin_when_option, plugin_when_value});
        plugin_feature.clear();
        plugin_id.clear();
        plugin_when_option.clear();
        plugin_when_value.clear();
        return true;
    };

    std::string raw;
    size_t line_number = 0;
    while (std::getline(file, raw)) {
        ++line_number;
        std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line.rfind("[[", 0) == 0 &&
            line.size() >= 4 &&
            line.substr(line.size() - 2) == "]]") {
            if (section == Section::Plugin && !finish_plugin()) return false;
            const std::string name = trim(line.substr(2, line.size() - 4));
            target = nullptr;
            feature = nullptr;
            option = nullptr;
            choice = nullptr;
            if (name == "target") {
                section = Section::Target;
                out.targets.emplace_back();
                target = &out.targets.back();
            } else if (name == "feature") {
                section = Section::Feature;
                out.features.emplace_back();
                feature = &out.features.back();
            } else if (name == "option") {
                section = Section::Option;
                out.options.emplace_back();
                option = &out.options.back();
            } else if (name == "option.choice") {
                if (out.options.empty()) {
                    set_error(error, "option.choice appears before option");
                    return false;
                }
                section = Section::Choice;
                out.options.back().choices.emplace_back();
                option = &out.options.back();
                choice = &option->choices.back();
            } else if (name == "plugin") {
                section = Section::Plugin;
            } else {
                set_error(error, "unsupported manifest section [[" + name + "]]");
                return false;
            }
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            set_error(error, "invalid manifest line " +
                             std::to_string(line_number));
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::string string_value;
        int64_t int_value = 0;
        bool bool_value = false;

        auto string_field = [&](std::string& field) -> bool {
            if (!parse_string(value, string_value)) return false;
            field = string_value;
            return true;
        };

        bool known = true;
        bool parsed = true;
        switch (section) {
            case Section::Package:
                if (key == "format_version") {
                    parsed = parse_int(value, int_value) &&
                             int_value >= 1 && int_value <= 1;
                    if (parsed) out.format_version = (uint32_t)int_value;
                } else if (key == "id") parsed = string_field(out.id);
                else if (key == "version") parsed = string_field(out.version);
                else if (key == "name") parsed = string_field(out.name);
                else if (key == "author") parsed = string_field(out.author);
                else if (key == "description") parsed = string_field(out.description);
                else if (key == "license") parsed = string_field(out.license);
                else if (key == "resolver") {
                    parsed = parse_string(value, string_value) &&
                             string_value == "declarative";
                } else if (key == "save_compatibility") {
                    parsed = parse_string(value, string_value);
                } else known = false;
                break;
            case Section::Target:
                if (!target) parsed = false;
                else if (key == "game_id") parsed = string_field(target->game_id);
                else if (key == "rom_crc32") parsed = string_field(target->rom_crc32);
                else known = false;
                break;
            case Section::Feature:
                feature = out.features.empty() ? nullptr : &out.features.back();
                if (!feature) parsed = false;
                else if (key == "id") parsed = string_field(feature->id);
                else if (key == "name") parsed = string_field(feature->name);
                else if (key == "description") parsed = string_field(feature->description);
                else if (key == "group") parsed = string_field(feature->group);
                else if (key == "exclusive_group") {
                    parsed = string_field(feature->exclusive_group);
                }
                else if (key == "default_enabled") {
                    parsed = parse_bool(value, bool_value);
                    if (parsed) feature->default_enabled = bool_value;
                }
                else if (key == "camera_controls") {
                    parsed = parse_bool(value, bool_value);
                    if (parsed) feature->camera_controls = bool_value;
                } else known = false;
                break;
            case Section::Option:
                option = out.options.empty() ? nullptr : &out.options.back();
                if (!option) parsed = false;
                else if (key == "feature") parsed = string_field(option->feature_id);
                else if (key == "id") parsed = string_field(option->id);
                else if (key == "label") parsed = string_field(option->label);
                else if (key == "description") parsed = string_field(option->description);
                else if (key == "group") parsed = string_field(option->group);
                else if (key == "type") {
                    parsed = parse_string(value, string_value);
                    if (parsed && string_value == "boolean")
                        option->type = OptionType::Boolean;
                    else if (parsed && string_value == "choice")
                        option->type = OptionType::Choice;
                    else if (parsed && string_value == "integer")
                        option->type = OptionType::Integer;
                    else parsed = false;
                } else if (key == "default") {
                    if (parse_string(value, string_value))
                        option->default_value = string_value;
                    else if (parse_bool(value, bool_value))
                        option->default_value = bool_value ? "true" : "false";
                    else if (parse_int(value, int_value))
                        option->default_value = std::to_string(int_value);
                    else parsed = false;
                } else if (key == "min") {
                    parsed = parse_int(value, option->min_value);
                } else if (key == "max") {
                    parsed = parse_int(value, option->max_value);
                } else if (key == "step") {
                    parsed = parse_int(value, option->step);
                } else known = false;
                break;
            case Section::Choice:
                option = out.options.empty() ? nullptr : &out.options.back();
                choice = (!option || option->choices.empty())
                    ? nullptr : &option->choices.back();
                if (!choice) parsed = false;
                else if (key == "value") parsed = string_field(choice->value);
                else if (key == "label") parsed = string_field(choice->label);
                else known = false;
                break;
            case Section::Plugin:
                if (key == "feature") parsed = string_field(plugin_feature);
                else if (key == "id") parsed = string_field(plugin_id);
                else if (key == "when_option")
                    parsed = string_field(plugin_when_option);
                else if (key == "when_value")
                    parsed = string_field(plugin_when_value);
                else known = false;
                break;
        }
        if (!known || !parsed) {
            set_error(error, "invalid or unsupported manifest field '" + key +
                             "' on line " + std::to_string(line_number));
            return false;
        }
    }
    if (section == Section::Plugin && !finish_plugin()) return false;

    if (out.format_version != 1 || !valid_id(out.id) ||
        out.version.empty() || out.name.empty() || out.targets.empty() ||
        out.features.empty()) {
        set_error(error, "manifest is missing required package fields");
        return false;
    }
    std::set<std::string> feature_ids;
    for (const Target& item : out.targets) {
        if (item.game_id.empty() || !valid_crc32(item.rom_crc32)) {
            set_error(error, "manifest has an invalid target");
            return false;
        }
    }
    for (const Feature& item : out.features) {
        if (!valid_id(item.id) || item.name.empty() ||
            (!item.exclusive_group.empty() &&
             !valid_id(item.exclusive_group)) ||
            !feature_ids.insert(item.id).second) {
            set_error(error, "manifest has an invalid or duplicate feature");
            return false;
        }
    }
    std::set<std::pair<std::string, std::string>> option_ids;
    for (const Option& item : out.options) {
        if (!find_feature(out, item.feature_id) || !valid_id(item.id) ||
            item.label.empty() ||
            !option_ids.insert({item.feature_id, item.id}).second ||
            !validate_option_value(item, item.default_value, error)) {
            if (error && error->empty())
                *error = "manifest has an invalid option";
            return false;
        }
        if (item.type == OptionType::Choice &&
            (item.choices.empty() ||
             std::any_of(item.choices.begin(), item.choices.end(),
                         [](const Choice& c) {
                             return c.value.empty() || c.label.empty();
                         }))) {
            set_error(error, "choice option has no valid choices");
            return false;
        }
    }
    /*
     * Plugin conditions are resolved against declared options at load time,
     * not at activation, so a typo in when_option/when_value surfaces as a
     * manifest diagnostic instead of a feature that silently does nothing.
     */
    for (const Feature& item : out.features) {
        for (const PluginRef& plugin : item.plugins) {
            if (plugin.when_option.empty()) continue;
            const Option* conditioned =
                find_option(out, item.id, plugin.when_option);
            if (!conditioned) {
                set_error(error, "plugin condition references unknown option '" +
                                 plugin.when_option + "'");
                return false;
            }
            if (!validate_option_value(*conditioned, plugin.when_value,
                                       error)) {
                if (error && error->empty())
                    *error = "plugin condition has an invalid option value";
                return false;
            }
        }
    }
    return true;
}

const Package* selected_package(const Runtime& runtime,
                                const std::string& id) {
    const auto packages = runtime.packages.find(id);
    if (packages == runtime.packages.end() || packages->second.empty())
        return nullptr;
    const auto selection = runtime.selections.find(id);
    if (selection != runtime.selections.end() &&
        !selection->second.version.empty()) {
        const auto version =
            packages->second.find(selection->second.version);
        if (version != packages->second.end()) return &version->second;
    }
    return &packages->second.rbegin()->second;
}

PackageSelection& package_selection(Runtime& runtime,
                                    const Package& package) {
    PackageSelection& selection = runtime.selections[package.id];
    if (selection.version.empty()) selection.version = package.version;
    return selection;
}

bool feature_enabled(Runtime& runtime, const Package& package,
                     const Feature& feature) {
    PackageSelection& package_state = package_selection(runtime, package);
    FeatureSelection& selection = package_state.features[feature.id];
    if (!selection.has_enabled)
        selection.enabled = feature.default_enabled;
    return selection.enabled;
}

std::string option_value(Runtime& runtime, const Package& package,
                         const Feature& feature, const Option& option) {
    PackageSelection& package_state = package_selection(runtime, package);
    FeatureSelection& selection = package_state.features[feature.id];
    const auto value = selection.values.find(option.id);
    return value == selection.values.end() ? option.default_value :
                                             value->second;
}

bool target_matches(const Package& package, const Runtime& runtime) {
    return std::any_of(
        package.targets.begin(), package.targets.end(),
        [&](const Target& target) {
            return target.game_id == runtime.game_id &&
                   target.rom_crc32 == runtime.rom_crc32;
        });
}

Validation validate(Runtime& runtime) {
    Validation result;
    std::map<std::string, ResolvedPlugin> claimed;
    std::map<std::string, std::pair<std::string, std::string>>
        claimed_exclusive_groups;
    for (const auto& [id, versions] : runtime.packages) {
        (void)versions;
        const Package* package = selected_package(runtime, id);
        if (!package) continue;
        for (const Feature& feature : package->features) {
            if (!feature_enabled(runtime, *package, feature)) continue;
            if (!target_matches(*package, runtime)) {
                result.ok = false;
                result.diagnostics.push_back({
                    package->id, feature.id, {}, {},
                    "target:" + runtime.game_id,
                    "This feature does not support the selected stock ROM."
                });
                continue;
            }
            if (!feature.exclusive_group.empty()) {
                const auto prior = claimed_exclusive_groups.find(
                    feature.exclusive_group);
                if (prior != claimed_exclusive_groups.end()) {
                    result.ok = false;
                    result.diagnostics.push_back({
                        package->id, feature.id,
                        prior->second.first, prior->second.second,
                        "exclusive-group:" + feature.exclusive_group,
                        "Only one feature in this exclusive group may be enabled."
                    });
                    result.diagnostics.push_back({
                        prior->second.first, prior->second.second,
                        package->id, feature.id,
                        "exclusive-group:" + feature.exclusive_group,
                        "Only one feature in this exclusive group may be enabled."
                    });
                } else {
                    claimed_exclusive_groups.emplace(
                        feature.exclusive_group,
                        std::make_pair(package->id, feature.id));
                }
            }
            for (const PluginRef& plugin : feature.plugins) {
                /*
                 * Skip variants the player did not select.  This runs before
                 * the duplicate-claim check below so sibling variants of one
                 * choice option never collide with each other.
                 */
                if (!plugin.when_option.empty()) {
                    const Option* conditioned =
                        find_option(*package, feature.id, plugin.when_option);
                    if (!conditioned) continue;
                    if (option_value(runtime, *package, feature,
                                     *conditioned) != plugin.when_value)
                        continue;
                }
                const std::string& plugin_id = plugin.id;
                const auto registered = registered_plugins().find(plugin_id);
                if (registered == registered_plugins().end() ||
                    !registered->second.activation) {
                    result.ok = false;
                    result.diagnostics.push_back({
                        package->id, feature.id, {}, {},
                        "plugin:" + plugin_id,
                        "The executable does not provide this trusted plugin."
                    });
                    continue;
                }
                ResolvedPlugin candidate{
                    plugin_id, package->id, feature.id,
                    registered->second.activation
                };
                const auto prior = claimed.find(plugin_id);
                if (prior != claimed.end()) {
                    result.ok = false;
                    result.diagnostics.push_back({
                        package->id, feature.id,
                        prior->second.package_id, prior->second.feature_id,
                        "plugin:" + plugin_id,
                        "Two enabled features claim the same trusted plugin."
                    });
                    result.diagnostics.push_back({
                        prior->second.package_id, prior->second.feature_id,
                        package->id, feature.id,
                        "plugin:" + plugin_id,
                        "Two enabled features claim the same trusted plugin."
                    });
                    continue;
                }
                claimed.emplace(plugin_id, candidate);
            }
        }
    }
    for (const auto& [id, plugin] : claimed) {
        (void)id;
        result.plugins.push_back(plugin);
    }
    return result;
}

bool scan(Runtime& runtime, std::string* error) {
    runtime.packages.clear();
    const fs::path packages_root = runtime.root / "packages";
    std::error_code ec;
    fs::create_directories(packages_root, ec);
    if (ec) {
        set_error(error, "cannot create mods directory: " + ec.message());
        return false;
    }
    for (const fs::directory_entry& package_dir :
         fs::directory_iterator(packages_root, ec)) {
        if (ec) break;
        if (!package_dir.is_directory()) continue;
        for (const fs::directory_entry& version_dir :
             fs::directory_iterator(package_dir.path(), ec)) {
            if (ec) break;
            if (!version_dir.is_directory()) continue;
            const fs::path manifest = version_dir.path() / "manifest.toml";
            if (!fs::is_regular_file(manifest)) continue;
            Package package;
            if (!read_manifest(manifest, package, error)) return false;
            if (package.id != package_dir.path().filename().string() ||
                package.version != version_dir.path().filename().string()) {
                set_error(error, "package directory does not match manifest: " +
                                 manifest.string());
                return false;
            }
            package.root = version_dir.path();
            runtime.packages[package.id][package.version] = std::move(package);
        }
        if (ec) break;
    }
    if (ec) {
        set_error(error, "cannot scan mods directory: " + ec.message());
        return false;
    }
    return true;
}

bool load_state(Runtime& runtime, std::string* error) {
    runtime.selections.clear();
    const fs::path path = runtime.root / "state.toml";
    if (!fs::exists(path)) return true;
    std::ifstream file(path);
    if (!file) {
        set_error(error, "cannot read mod state");
        return false;
    }
    enum class Section { Root, Package, Feature, Values };
    Section section = Section::Root;
    std::string current_package;
    std::string current_feature;
    std::string raw;
    while (std::getline(file, raw)) {
        const std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;
        if (line == "[[package]]") {
            section = Section::Package;
            current_package.clear();
            current_feature.clear();
            continue;
        }
        if (line == "[[feature]]") {
            section = Section::Feature;
            current_package.clear();
            current_feature.clear();
            continue;
        }
        if (line == "[feature.values]") {
            section = Section::Values;
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::string parsed;
        bool enabled = false;
        if (section == Section::Root) continue;
        if (section == Section::Package) {
            if (key == "id" && parse_string(value, current_package)) {
                runtime.selections[current_package];
            } else if (key == "version" && !current_package.empty() &&
                       parse_string(value, parsed)) {
                runtime.selections[current_package].version = parsed;
            }
            continue;
        }
        if (section == Section::Feature) {
            if (key == "package_id" && parse_string(value, current_package)) {
                runtime.selections[current_package];
            } else if (key == "id" && parse_string(value, current_feature)) {
                if (!current_package.empty())
                    runtime.selections[current_package].features[current_feature];
            } else if (key == "enabled" && !current_package.empty() &&
                       !current_feature.empty() &&
                       parse_bool(value, enabled)) {
                FeatureSelection& selection =
                    runtime.selections[current_package].features[current_feature];
                selection.enabled = enabled;
                selection.has_enabled = true;
            }
            continue;
        }
        if (section == Section::Values && !current_package.empty() &&
            !current_feature.empty() && parse_string(value, parsed)) {
            runtime.selections[current_package]
                .features[current_feature].values[key] = parsed;
        }
    }
    return true;
}

std::string quote_toml(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool save_state(Runtime& runtime, std::string* error) {
    std::error_code ec;
    fs::create_directories(runtime.root, ec);
    const fs::path temp = runtime.root / "state.toml.tmp";
    const fs::path output = runtime.root / "state.toml";
    std::ofstream file(temp, std::ios::trunc);
    if (!file) {
        set_error(error, "cannot write mod state");
        return false;
    }
    file << "format_version = 1\n";
    for (const auto& [package_id, selection] : runtime.selections) {
        file << "\n[[package]]\nid = " << quote_toml(package_id)
             << "\nversion = " << quote_toml(selection.version) << "\n";
    }
    for (const auto& [package_id, selection] : runtime.selections) {
        for (const auto& [feature_id, feature] : selection.features) {
            file << "\n[[feature]]\npackage_id = " << quote_toml(package_id)
                 << "\nid = " << quote_toml(feature_id)
                 << "\nenabled = " << (feature.enabled ? "true" : "false")
                 << "\n";
            if (!feature.values.empty()) {
                file << "\n[feature.values]\n";
                for (const auto& [key, value] : feature.values)
                    file << key << " = " << quote_toml(value) << "\n";
            }
        }
    }
    file.close();
    if (!file) {
        set_error(error, "cannot finish writing mod state");
        return false;
    }
    fs::rename(temp, output, ec);
    if (ec) {
        fs::remove(output, ec);
        ec.clear();
        fs::rename(temp, output, ec);
    }
    if (ec) {
        set_error(error, "cannot publish mod state: " + ec.message());
        return false;
    }
    return true;
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out,
               std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open " + path.string());
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || (uint64_t)size > kMaxArchiveBytes) {
        set_error(error, "archive is too large");
        return false;
    }
    in.seekg(0);
    out.resize((size_t)size);
    if (!out.empty() && !in.read((char*)out.data(), size)) {
        set_error(error, "cannot read " + path.string());
        return false;
    }
    return true;
}

uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed_size = 0;
    uint32_t size = 0;
    uint32_t local_offset = 0;
    bool directory = false;
};

bool safe_archive_name(const std::string& name) {
    if (name.empty() || name.size() > 512 ||
        name[0] == '/' || name[0] == '\\')
        return false;
    if (name.size() >= 2 &&
        std::isalpha((unsigned char)name[0]) && name[1] == ':')
        return false;
    const fs::path path = fs::path(name).lexically_normal();
    for (const auto& part : path) {
        const std::string text = part.string();
        if (text == ".." || text == "." || text.empty()) return false;
    }
    return true;
}

bool parse_zip(const std::vector<uint8_t>& bytes,
               std::vector<ZipEntry>& entries, std::string* error) {
    if (bytes.size() < 22) {
        set_error(error, "not a ZIP archive");
        return false;
    }
    size_t eocd = std::string::npos;
    const size_t floor = bytes.size() > 65557 ? bytes.size() - 65557 : 0;
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (le32(bytes.data() + pos) == 0x06054b50u) {
            eocd = pos;
            break;
        }
        if (pos == floor) break;
    }
    if (eocd == std::string::npos) {
        set_error(error, "ZIP end record is missing");
        return false;
    }
    const uint16_t count = le16(bytes.data() + eocd + 10);
    const uint32_t central_size = le32(bytes.data() + eocd + 12);
    const uint32_t central_offset = le32(bytes.data() + eocd + 16);
    if (count > kMaxArchiveFiles ||
        (uint64_t)central_offset + central_size > bytes.size()) {
        set_error(error, "ZIP central directory is invalid");
        return false;
    }
    size_t at = central_offset;
    uint64_t expanded = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 46 > bytes.size() ||
            le32(bytes.data() + at) != 0x02014b50u) {
            set_error(error, "ZIP entry record is invalid");
            return false;
        }
        const uint16_t flags = le16(bytes.data() + at + 8);
        ZipEntry entry;
        entry.method = le16(bytes.data() + at + 10);
        entry.crc = le32(bytes.data() + at + 16);
        entry.compressed_size = le32(bytes.data() + at + 20);
        entry.size = le32(bytes.data() + at + 24);
        const uint16_t name_len = le16(bytes.data() + at + 28);
        const uint16_t extra_len = le16(bytes.data() + at + 30);
        const uint16_t comment_len = le16(bytes.data() + at + 32);
        entry.local_offset = le32(bytes.data() + at + 42);
        if ((flags & 1u) || (entry.method != 0 && entry.method != 8)) {
            set_error(error, (flags & 1u)
                ? "encrypted ZIP entries are not supported"
                : "ZIP compression method is not supported");
            return false;
        }
        if (at + 46ull + name_len + extra_len + comment_len > bytes.size()) {
            set_error(error, "ZIP entry name is truncated");
            return false;
        }
        entry.name.assign((const char*)bytes.data() + at + 46, name_len);
        std::replace(entry.name.begin(), entry.name.end(), '\\', '/');
        entry.directory = !entry.name.empty() && entry.name.back() == '/';
        const std::string checked = entry.directory
            ? entry.name.substr(0, entry.name.size() - 1) : entry.name;
        if (!safe_archive_name(checked)) {
            set_error(error, "unsafe ZIP path: " + entry.name);
            return false;
        }
        expanded += entry.size;
        if (expanded > kMaxArchiveBytes) {
            set_error(error, "expanded archive exceeds the size limit");
            return false;
        }
        entries.push_back(std::move(entry));
        at += 46ull + name_len + extra_len + comment_len;
    }
    return true;
}

struct DeflateBits {
    const uint8_t* at = nullptr;
    const uint8_t* end = nullptr;
    uint64_t hold = 0;
    unsigned bits = 0;

    bool read(unsigned count, uint32_t& out) {
        while (bits < count) {
            if (at == end) return false;
            hold |= (uint64_t)*at++ << bits;
            bits += 8;
        }
        out = count == 32 ? (uint32_t)hold :
              (uint32_t)(hold & ((1ull << count) - 1));
        hold >>= count;
        bits -= count;
        return true;
    }
    void align_byte() {
        const unsigned drop = bits & 7u;
        hold >>= drop;
        bits -= drop;
    }
};

struct DeflateHuffman {
    std::array<uint16_t, 16> count{};
    std::vector<uint16_t> symbols;
};

bool build_huffman(const std::vector<uint8_t>& lengths,
                   DeflateHuffman& out) {
    out = {};
    for (uint8_t length : lengths) {
        if (length > 15) return false;
        out.count[length]++;
    }
    if (out.count[0] == lengths.size()) return false;
    int left = 1;
    for (int length = 1; length <= 15; ++length) {
        left <<= 1;
        left -= out.count[(size_t)length];
        if (left < 0) return false;
    }
    std::array<uint16_t, 16> offsets{};
    for (size_t length = 1; length < 15; ++length)
        offsets[length + 1] = offsets[length] + out.count[length];
    out.symbols.resize(lengths.size() - out.count[0]);
    for (uint16_t symbol = 0; symbol < lengths.size(); ++symbol)
        if (lengths[symbol])
            out.symbols[offsets[lengths[symbol]]++] = symbol;
    return true;
}

bool decode_symbol(DeflateBits& bits, const DeflateHuffman& table,
                   uint16_t& symbol) {
    uint32_t code = 0;
    uint32_t first = 0;
    uint32_t index = 0;
    for (uint32_t length = 1; length <= 15; ++length) {
        uint32_t bit = 0;
        if (!bits.read(1, bit)) return false;
        code |= bit;
        const uint32_t count = table.count[length];
        if (code < first + count) {
            const uint32_t slot = index + code - first;
            if (slot >= table.symbols.size()) return false;
            symbol = table.symbols[slot];
            return true;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return false;
}

bool inflate_deflate(const uint8_t* data, size_t size, size_t expected,
                     std::vector<uint8_t>& out) {
    static const uint16_t length_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
        115,131,163,195,227,258};
    static const uint8_t length_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distance_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
        1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t distance_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,
        12,12,13,13};
    DeflateBits input{data, data + size};
    out.clear();
    out.reserve(expected);
    bool final = false;
    while (!final) {
        uint32_t final_bit = 0;
        uint32_t type = 0;
        if (!input.read(1, final_bit) || !input.read(2, type)) return false;
        final = final_bit != 0;
        if (type == 0) {
            input.align_byte();
            uint32_t length = 0;
            uint32_t complement = 0;
            if (!input.read(16, length) || !input.read(16, complement) ||
                (length ^ 0xffffu) != complement ||
                out.size() + length > expected)
                return false;
            for (uint32_t i = 0; i < length; ++i) {
                uint32_t byte = 0;
                if (!input.read(8, byte)) return false;
                out.push_back((uint8_t)byte);
            }
            continue;
        }
        if (type == 3) return false;
        std::vector<uint8_t> literal_lengths;
        std::vector<uint8_t> distance_lengths;
        if (type == 1) {
            literal_lengths.resize(288);
            for (size_t i = 0; i <= 143; ++i) literal_lengths[i] = 8;
            for (size_t i = 144; i <= 255; ++i) literal_lengths[i] = 9;
            for (size_t i = 256; i <= 279; ++i) literal_lengths[i] = 7;
            for (size_t i = 280; i <= 287; ++i) literal_lengths[i] = 8;
            distance_lengths.assign(32, 5);
        } else {
            uint32_t hlit = 0;
            uint32_t hdist = 0;
            uint32_t hclen = 0;
            if (!input.read(5, hlit) || !input.read(5, hdist) ||
                !input.read(4, hclen))
                return false;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            static const uint8_t order[19] =
                {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            std::vector<uint8_t> code_lengths(19, 0);
            for (uint32_t i = 0; i < hclen; ++i) {
                uint32_t value = 0;
                if (!input.read(3, value)) return false;
                code_lengths[order[i]] = (uint8_t)value;
            }
            DeflateHuffman code_table;
            if (!build_huffman(code_lengths, code_table)) return false;
            std::vector<uint8_t> lengths;
            lengths.reserve(hlit + hdist);
            while (lengths.size() < hlit + hdist) {
                uint16_t symbol = 0;
                if (!decode_symbol(input, code_table, symbol)) return false;
                if (symbol <= 15) {
                    lengths.push_back((uint8_t)symbol);
                    continue;
                }
                uint32_t repeat = 0;
                uint32_t extra = 0;
                uint8_t value = 0;
                if (symbol == 16) {
                    if (lengths.empty() || !input.read(2, extra)) return false;
                    repeat = extra + 3;
                    value = lengths.back();
                } else if (symbol == 17) {
                    if (!input.read(3, extra)) return false;
                    repeat = extra + 3;
                } else if (symbol == 18) {
                    if (!input.read(7, extra)) return false;
                    repeat = extra + 11;
                } else {
                    return false;
                }
                if (lengths.size() + repeat > hlit + hdist) return false;
                lengths.insert(lengths.end(), repeat, value);
            }
            literal_lengths.assign(lengths.begin(), lengths.begin() + hlit);
            distance_lengths.assign(lengths.begin() + hlit, lengths.end());
        }
        DeflateHuffman literals;
        DeflateHuffman distances;
        if (!build_huffman(literal_lengths, literals) ||
            !build_huffman(distance_lengths, distances))
            return false;
        for (;;) {
            uint16_t symbol = 0;
            if (!decode_symbol(input, literals, symbol)) return false;
            if (symbol < 256) {
                if (out.size() >= expected) return false;
                out.push_back((uint8_t)symbol);
                continue;
            }
            if (symbol == 256) break;
            if (symbol < 257 || symbol > 285) return false;
            const unsigned length_index = symbol - 257;
            uint32_t extra = 0;
            if (!input.read(length_extra[length_index], extra)) return false;
            const size_t length = length_base[length_index] + extra;
            uint16_t distance_symbol = 0;
            if (!decode_symbol(input, distances, distance_symbol) ||
                distance_symbol >= 30)
                return false;
            if (!input.read(distance_extra[distance_symbol], extra)) return false;
            const size_t distance = distance_base[distance_symbol] + extra;
            if (distance == 0 || distance > out.size() ||
                out.size() + length > expected)
                return false;
            for (size_t i = 0; i < length; ++i)
                out.push_back(out[out.size() - distance]);
        }
    }
    return out.size() == expected;
}

bool extract_zip(const std::vector<uint8_t>& bytes,
                 const std::vector<ZipEntry>& entries,
                 const fs::path& target, std::string* error) {
    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        set_error(error, "cannot create staging directory: " + ec.message());
        return false;
    }
    for (const ZipEntry& entry : entries) {
        const fs::path output = target / fs::path(entry.name);
        if (entry.directory) {
            fs::create_directories(output, ec);
            if (ec) {
                set_error(error, "cannot create archive directory: " +
                                 ec.message());
                return false;
            }
            continue;
        }
        if ((uint64_t)entry.local_offset + 30 > bytes.size() ||
            le32(bytes.data() + entry.local_offset) != 0x04034b50u) {
            set_error(error, "ZIP local entry is invalid");
            return false;
        }
        const uint16_t name_len =
            le16(bytes.data() + entry.local_offset + 26);
        const uint16_t extra_len =
            le16(bytes.data() + entry.local_offset + 28);
        const uint64_t data_at =
            (uint64_t)entry.local_offset + 30 + name_len + extra_len;
        if (data_at + entry.compressed_size > bytes.size()) {
            set_error(error, "ZIP entry payload is invalid");
            return false;
        }
        const uint8_t* compressed = bytes.data() + data_at;
        std::vector<uint8_t> expanded;
        const uint8_t* data = compressed;
        if (entry.method == 0) {
            if (entry.compressed_size != entry.size) {
                set_error(error, "stored ZIP entry has inconsistent size");
                return false;
            }
        } else {
            if (!inflate_deflate(compressed, entry.compressed_size,
                                 entry.size, expanded)) {
                set_error(error, "cannot inflate ZIP entry: " + entry.name);
                return false;
            }
            data = expanded.data();
        }
        if (crc32_compute(data, entry.size) != entry.crc) {
            set_error(error, "ZIP entry checksum failed: " + entry.name);
            return false;
        }
        fs::create_directories(output.parent_path(), ec);
        if (ec) {
            set_error(error, "cannot create archive parent directory: " +
                             ec.message());
            return false;
        }
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file || (entry.size &&
                      !file.write((const char*)data, entry.size))) {
            set_error(error, "cannot extract archive entry: " + entry.name);
            return false;
        }
    }
    return true;
}

bool install_archive(Runtime& runtime, const fs::path& archive,
                     std::string* installed_id,
                     std::string* installed_version,
                     std::string* error) {
    if (archive.extension() != ".nesmod") {
        set_error(error, "NES mod archives must use the .nesmod extension");
        return false;
    }
    std::vector<uint8_t> bytes;
    std::vector<ZipEntry> entries;
    if (!read_file(archive, bytes, error) ||
        !parse_zip(bytes, entries, error))
        return false;
    const bool has_manifest = std::any_of(
        entries.begin(), entries.end(),
        [](const ZipEntry& entry) {
            return entry.name == "manifest.toml" && !entry.directory;
        });
    if (!has_manifest) {
        set_error(error, "archive has no root manifest.toml");
        return false;
    }
    const uint64_t nonce = (uint64_t)
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path staging =
        runtime.root / (".staging-" + std::to_string(nonce));
    std::error_code ec;
    auto cleanup = [&]() {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
    };
    if (!extract_zip(bytes, entries, staging, error)) {
        cleanup();
        return false;
    }
    Package package;
    if (!read_manifest(staging / "manifest.toml", package, error)) {
        cleanup();
        return false;
    }
    const fs::path destination =
        runtime.root / "packages" / package.id / package.version;
    if (fs::exists(destination)) {
        cleanup();
        set_error(error, package.id + " " + package.version +
                         " is already installed");
        return false;
    }
    fs::create_directories(destination.parent_path(), ec);
    if (!ec) fs::rename(staging, destination, ec);
    if (ec) {
        cleanup();
        set_error(error, "cannot publish installed package: " + ec.message());
        return false;
    }
    if (installed_id) *installed_id = package.id;
    if (installed_version) *installed_version = package.version;
    return true;
}

bool crc32_file(const fs::path& path, std::string& out,
                std::string* error) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes, error)) return false;
    if (bytes.size() <= 16 ||
        bytes[0] != 'N' || bytes[1] != 'E' ||
        bytes[2] != 'S' || bytes[3] != 0x1a) {
        set_error(error, "selected image is not a valid iNES ROM");
        return false;
    }
    char digest[9];
    std::snprintf(digest, sizeof(digest), "%08x",
                  crc32_compute(bytes.data() + 16, bytes.size() - 16));
    out = digest;
    return true;
}

#if defined(RECOMP_LAUNCHER)
template <size_t N>
void copy_text(char (&out)[N], const std::string& value) {
    std::snprintf(out, N, "%s", value.c_str());
}

std::vector<const Package*> selected_packages(Runtime& runtime) {
    std::vector<const Package*> result;
    for (const auto& [id, versions] : runtime.packages) {
        (void)versions;
        const Package* package = selected_package(runtime, id);
        if (package) result.push_back(package);
    }
    return result;
}

struct FeatureRef {
    const Package* package = nullptr;
    const Feature* feature = nullptr;
};

std::vector<FeatureRef> selected_features(Runtime& runtime) {
    std::vector<FeatureRef> result;
    for (const Package* package : selected_packages(runtime))
        for (const Feature& feature : package->features)
            result.push_back({package, &feature});
    return result;
}

void refresh_validation() {
    state().validation = validate(state());
}

int provider_package_count(void*) {
    return (int)selected_packages(state()).size();
}

int provider_package_get(void*, int index,
                         RecompLauncherCModPackage* out) {
    if (!out || index < 0) return 0;
    const auto packages = selected_packages(state());
    if ((size_t)index >= packages.size()) return 0;
    const Package& package = *packages[(size_t)index];
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, package.id);
    copy_text(out->version, package.version);
    copy_text(out->name, package.name);
    copy_text(out->author, package.author);
    copy_text(out->description, package.description);
    copy_text(out->license, package.license);
    out->removable = 1;
    out->option_count = 0;
    for (const Feature& feature : package.features) {
        if (feature_enabled(state(), package, feature)) out->enabled = 1;
        out->option_count += (int)std::count_if(
            package.options.begin(), package.options.end(),
            [&](const Option& option) {
                return option.feature_id == feature.id;
            });
    }
    out->has_error = std::any_of(
        state().validation.diagnostics.begin(),
        state().validation.diagnostics.end(),
        [&](const Diagnostic& diagnostic) {
            return diagnostic.package_id == package.id;
        });
    copy_text(out->status, out->has_error ? "Needs attention" :
                   (out->enabled ? "Enabled" : "Installed"));
    return 1;
}

int provider_option_get(void*, const char*, int,
                        RecompLauncherCModOption*) {
    return 0;
}

int provider_choice_get(void*, const char*, const char*, int,
                        RecompLauncherCModChoice*) {
    return 0;
}

int provider_version_count(void*, const char* package_id) {
    if (!package_id) return 0;
    const auto package = state().packages.find(package_id);
    return package == state().packages.end() ? 0 :
                                              (int)package->second.size();
}

int provider_version_get(void*, const char* package_id, int index,
                         RecompLauncherCModVersion* out) {
    if (!package_id || !out || index < 0) return 0;
    const auto package = state().packages.find(package_id);
    if (package == state().packages.end() ||
        (size_t)index >= package->second.size())
        return 0;
    auto version = package->second.begin();
    std::advance(version, index);
    std::memset(out, 0, sizeof(*out));
    copy_text(out->version, version->first);
    const Package* selected = selected_package(state(), package_id);
    out->selected = selected && selected->version == version->first;
    out->removable = !out->selected;
    if (out->selected) {
        out->removable = std::none_of(
            selected->features.begin(), selected->features.end(),
            [&](const Feature& feature) {
                return feature_enabled(state(), *selected, feature);
            });
    }
    return 1;
}

int provider_install(void*, const char* archive_path) {
    if (!archive_path) return 0;
    std::string id;
    std::string version;
    std::string error;
    if (!install_archive(state(), archive_path, &id, &version, &error) ||
        !scan(state(), &error)) {
        state().error = error;
        return 0;
    }
    state().selections[id].version = version;
    refresh_validation();
    state().error.clear();
    return 1;
}

int provider_remove(void*, const char* package_id,
                    const char* version) {
    if (!package_id || !version) return 0;
    const Package* selected = selected_package(state(), package_id);
    if (selected && selected->version == version &&
        std::any_of(selected->features.begin(), selected->features.end(),
                    [&](const Feature& feature) {
                        return feature_enabled(state(), *selected, feature);
                    })) {
        state().error = "Disable this package's features before removing it.";
        return 0;
    }
    std::error_code ec;
    const fs::path path =
        state().root / "packages" / package_id / version;
    if (!fs::exists(path) || !fs::remove_all(path, ec) || ec) {
        state().error = "Cannot remove package version.";
        return 0;
    }
    std::string error;
    if (!scan(state(), &error)) {
        state().error = error;
        return 0;
    }
    refresh_validation();
    state().error.clear();
    return 1;
}

int provider_enable(void*, const char* package_id, int enabled) {
    if (!package_id) return 0;
    const Package* package = selected_package(state(), package_id);
    if (!package) return 0;
    for (const Feature& feature : package->features) {
        FeatureSelection& selection =
            package_selection(state(), *package).features[feature.id];
        selection.enabled = enabled != 0;
        selection.has_enabled = true;
    }
    refresh_validation();
    state().error.clear();
    return 1;
}

int provider_select(void*, const char* package_id,
                    const char* version) {
    if (!package_id || !version) return 0;
    const auto package = state().packages.find(package_id);
    if (package == state().packages.end() ||
        package->second.find(version) == package->second.end()) {
        state().error = "Unknown package version.";
        return 0;
    }
    state().selections[package_id].version = version;
    refresh_validation();
    state().error.clear();
    return 1;
}

int provider_set_option(void*, const char*, const char*,
                        const char*) {
    state().error = "Package-level options are not used; edit the feature.";
    return 0;
}

int provider_commit(void*, const char* image_path) {
    std::string error;
    if (!mod_runtime_commit(
            image_path ? fs::path(image_path) : fs::path(), &error)) {
        state().error = error;
        return 0;
    }
    state().error.clear();
    return 1;
}

int provider_commit_netplay(void*, const char* image_path) {
    Runtime& runtime = state();
    if (image_path && image_path[0]) {
        std::string digest;
        if (!crc32_file(image_path, digest, &runtime.error) ||
            digest != runtime.rom_crc32) {
            if (runtime.error.empty())
                runtime.error =
                    "The selected ROM does not match this mod catalog target.";
            return 0;
        }
    }
    /* Netplay is deliberately vanilla without overwriting the user's saved
     * offline selections. The next ordinary commit rebuilds this plan. */
    runtime.committed = {};
    runtime.error.clear();
    return 1;
}

const char* provider_error(void*) {
    return state().error.c_str();
}

int provider_feature_count(void*) {
    return (int)selected_features(state()).size();
}

int provider_feature_get(void*, int index,
                         RecompLauncherCModFeature* out) {
    if (!out || index < 0) return 0;
    const auto features = selected_features(state());
    if ((size_t)index >= features.size()) return 0;
    const Package& package = *features[(size_t)index].package;
    const Feature& feature = *features[(size_t)index].feature;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, feature.id);
    copy_text(out->package_id, package.id);
    copy_text(out->package_version, package.version);
    copy_text(out->package_name, package.name);
    copy_text(out->name, feature.name);
    copy_text(out->author, package.author);
    copy_text(out->description, feature.description);
    copy_text(out->group, feature.group);
    out->enabled = feature_enabled(state(), package, feature);
    out->camera_controls = feature.camera_controls ? 1 : 0;
    out->option_count = (int)std::count_if(
        package.options.begin(), package.options.end(),
        [&](const Option& option) {
            return option.feature_id == feature.id;
        });
    out->has_error = std::any_of(
        state().validation.diagnostics.begin(),
        state().validation.diagnostics.end(),
        [&](const Diagnostic& diagnostic) {
            return diagnostic.package_id == package.id &&
                   diagnostic.feature_id == feature.id;
        });
    copy_text(out->status, out->has_error ? "Needs attention" :
                   (out->enabled ? "Enabled" : "Disabled"));
    return 1;
}

int provider_feature_option_get(
    void*, const char* package_id, const char* feature_id, int index,
    RecompLauncherCModOption* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    const Package* package = selected_package(state(), package_id);
    if (!package || !find_feature(*package, feature_id)) return 0;
    std::vector<const Option*> options;
    for (const Option& option : package->options)
        if (option.feature_id == feature_id) options.push_back(&option);
    if ((size_t)index >= options.size()) return 0;
    const Option& option = *options[(size_t)index];
    const Feature& feature = *find_feature(*package, feature_id);
    std::memset(out, 0, sizeof(*out));
    copy_text(out->id, option.id);
    copy_text(out->label, option.label);
    copy_text(out->description, option.description);
    copy_text(out->group, option.group);
    copy_text(out->value, option_value(state(), *package, feature, option));
    copy_text(out->default_value, option.default_value);
    out->type = option.type == OptionType::Boolean
        ? RECOMP_MOD_OPTION_BOOLEAN
        : option.type == OptionType::Choice
            ? RECOMP_MOD_OPTION_CHOICE : RECOMP_MOD_OPTION_INTEGER;
    out->min_value = option.min_value;
    out->max_value = option.max_value;
    out->step = option.step;
    out->choice_count = (int)option.choices.size();
    return 1;
}

int provider_feature_choice_get(
    void*, const char* package_id, const char* feature_id,
    const char* option_id, int index, RecompLauncherCModChoice* out) {
    if (!package_id || !feature_id || !option_id ||
        !out || index < 0)
        return 0;
    const Package* package = selected_package(state(), package_id);
    if (!package) return 0;
    const Option* option =
        find_option(*package, feature_id, option_id);
    if (!option || (size_t)index >= option->choices.size()) return 0;
    std::memset(out, 0, sizeof(*out));
    copy_text(out->value, option->choices[(size_t)index].value);
    copy_text(out->label, option->choices[(size_t)index].label);
    return 1;
}

int provider_feature_enable(void*, const char* package_id,
                            const char* feature_id, int enabled) {
    if (!package_id || !feature_id) return 0;
    const Package* package = selected_package(state(), package_id);
    const Feature* feature =
        package ? find_feature(*package, feature_id) : nullptr;
    if (!package || !feature) return 0;
    if (enabled && !feature->exclusive_group.empty()) {
        for (const FeatureRef& other : selected_features(state())) {
            if (other.package->id == package->id &&
                other.feature->id == feature->id)
                continue;
            if (other.feature->exclusive_group != feature->exclusive_group)
                continue;
            FeatureSelection& other_selection =
                package_selection(state(), *other.package)
                    .features[other.feature->id];
            other_selection.enabled = false;
            other_selection.has_enabled = true;
        }
    }
    FeatureSelection& selection =
        package_selection(state(), *package).features[feature_id];
    selection.enabled = enabled != 0;
    selection.has_enabled = true;
    refresh_validation();
    state().error.clear();
    return 1;
}

int provider_feature_set_option(
    void*, const char* package_id, const char* feature_id,
    const char* option_id, const char* value) {
    if (!package_id || !feature_id || !option_id || !value) return 0;
    const Package* package = selected_package(state(), package_id);
    if (!package) return 0;
    const Feature* feature = find_feature(*package, feature_id);
    const Option* option =
        find_option(*package, feature_id, option_id);
    if (!feature || !option) return 0;
    std::string error;
    if (!validate_option_value(*option, value, &error)) {
        state().error = error;
        return 0;
    }
    package_selection(state(), *package)
        .features[feature->id].values[option->id] = value;
    refresh_validation();
    state().error.clear();
    return 1;
}

int provider_diagnostic_count(void*, const char* package_id,
                              const char* feature_id) {
    if (!package_id || !feature_id) return 0;
    return (int)std::count_if(
        state().validation.diagnostics.begin(),
        state().validation.diagnostics.end(),
        [&](const Diagnostic& diagnostic) {
            return diagnostic.package_id == package_id &&
                   diagnostic.feature_id == feature_id;
        });
}

int provider_diagnostic_get(
    void*, const char* package_id, const char* feature_id,
    int index, RecompLauncherCModDiagnostic* out) {
    if (!package_id || !feature_id || !out || index < 0) return 0;
    for (const Diagnostic& diagnostic : state().validation.diagnostics) {
        if (diagnostic.package_id != package_id ||
            diagnostic.feature_id != feature_id)
            continue;
        if (index-- != 0) continue;
        std::memset(out, 0, sizeof(*out));
        out->severity = RECOMP_MOD_DIAGNOSTIC_ERROR;
        copy_text(out->resource, diagnostic.resource);
        copy_text(out->message, diagnostic.message);
        copy_text(out->related_package_id,
                  diagnostic.related_package_id);
        copy_text(out->related_feature_id,
                  diagnostic.related_feature_id);
        return 1;
    }
    return 0;
}

RecompLauncherCModProvider provider = {
    nullptr,
    provider_package_count,
    provider_package_get,
    provider_option_get,
    provider_choice_get,
    provider_version_count,
    provider_version_get,
    provider_install,
    provider_remove,
    provider_enable,
    provider_select,
    provider_set_option,
    provider_commit,
    provider_error,
    provider_feature_count,
    provider_feature_get,
    provider_feature_option_get,
    provider_feature_choice_get,
    provider_feature_enable,
    provider_feature_set_option,
    provider_diagnostic_count,
    provider_diagnostic_get,
    ".nesmod",
    "NESRecomp mod package (.nesmod)",
    provider_commit_netplay,
};
#endif

}  // namespace

bool mod_runtime_initialize(const fs::path& root,
                            const std::string& game_id,
                            const std::string& rom_crc32,
                            std::string* error) {
    Runtime& runtime = state();
    runtime = {};
    runtime.root = root;
    runtime.game_id = game_id;
    runtime.rom_crc32 = rom_crc32;
    if (game_id.empty() || !valid_crc32(rom_crc32)) {
        set_error(error, "invalid NES mod target identity");
        return false;
    }
    if (!scan(runtime, &runtime.error) ||
        !load_state(runtime, &runtime.error)) {
        set_error(error, runtime.error);
        return false;
    }
    runtime.validation = validate(runtime);
    runtime.initialized = true;
    runtime.error.clear();
    return true;
}

bool mod_runtime_commit(const fs::path& rom_path, std::string* error) {
    Runtime& runtime = state();
    if (!runtime.initialized) return true;
    if (!rom_path.empty()) {
        std::string digest;
        if (!crc32_file(rom_path, digest, &runtime.error)) {
            set_error(error, runtime.error);
            return false;
        }
        if (digest != runtime.rom_crc32) {
            runtime.error =
                "The selected ROM does not match this mod catalog target.";
            set_error(error, runtime.error);
            return false;
        }
    }
    runtime.validation = validate(runtime);
    if (!runtime.validation.ok) {
        for (const Diagnostic& diagnostic : runtime.validation.diagnostics) {
            std::fprintf(stderr, "[Mods] %s/%s: %s (%s)\n",
                         diagnostic.package_id.c_str(),
                         diagnostic.feature_id.c_str(),
                         diagnostic.message.c_str(),
                         diagnostic.resource.c_str());
        }
        runtime.error = "Resolve the highlighted mod conflicts before Play.";
        set_error(error, runtime.error);
        return false;
    }
    if (!save_state(runtime, &runtime.error)) {
        set_error(error, runtime.error);
        return false;
    }
    runtime.committed = runtime.validation;
    runtime.error.clear();
    return true;
}

void mod_runtime_activate_plugins() {
    Runtime& runtime = state();
    if (!runtime.initialized) return;
    for (NESModActivationCallback callback : reset_callbacks())
        if (callback) callback();
    for (const ResolvedPlugin& plugin : runtime.committed.plugins)
        if (plugin.callback) plugin.callback();
}

#if defined(RECOMP_LAUNCHER)
const RecompLauncherCModProvider* mod_runtime_launcher_provider() {
    return &provider;
}
#endif

}  // namespace NESRecomp

extern "C" int nes_mod_register_activation_plugin(
    const char* id, NESModActivationCallback callback) {
    if (!id || !callback || !NESRecomp::valid_id(id)) return 0;
    auto& plugins = NESRecomp::registered_plugins();
    const auto prior = plugins.find(id);
    if (prior != plugins.end() && prior->second.activation != callback)
        return 0;
    plugins[id].activation = callback;
    return 1;
}

extern "C" int nes_mod_register_reset_callback(
    NESModActivationCallback callback) {
    if (!callback) return 0;
    auto& callbacks = NESRecomp::reset_callbacks();
    if (std::find(callbacks.begin(), callbacks.end(), callback) ==
        callbacks.end())
        callbacks.push_back(callback);
    return 1;
}

extern "C" int nes_mod_get_option_int(
    const char* package_id, const char* feature_id,
    const char* option_id, int fallback) {
    if (!package_id || !feature_id || !option_id) return fallback;
    NESRecomp::Runtime& runtime = NESRecomp::state();
    const NESRecomp::Package* package =
        NESRecomp::selected_package(runtime, package_id);
    if (!package) return fallback;
    const NESRecomp::Feature* feature =
        NESRecomp::find_feature(*package, feature_id);
    const NESRecomp::Option* option =
        NESRecomp::find_option(*package, feature_id, option_id);
    int64_t parsed = 0;
    if (!feature || !option ||
        !NESRecomp::parse_int(
            NESRecomp::option_value(runtime, *package, *feature, *option),
            parsed) ||
        parsed < INT32_MIN || parsed > INT32_MAX)
        return fallback;
    return (int)parsed;
}

extern "C" int nes_mod_option_value(
    const char* package_id, const char* feature_id,
    const char* option_id, char* out, uint32_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!package_id || !feature_id || !option_id) return 0;
    NESRecomp::Runtime& runtime = NESRecomp::state();
    const NESRecomp::Package* package =
        NESRecomp::selected_package(runtime, package_id);
    if (!package) return 0;
    const NESRecomp::Feature* feature =
        NESRecomp::find_feature(*package, feature_id);
    const NESRecomp::Option* option =
        NESRecomp::find_option(*package, feature_id, option_id);
    if (!feature || !option) return 0;
    const std::string value =
        NESRecomp::option_value(runtime, *package, *feature, *option);
    /* Truncation is a failure, not a shortened selection: the caller must be
     * able to fall back to its own default rather than act on a partial id. */
    if (value.size() + 1 > (size_t)out_size) return 0;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return 1;
}

extern "C" int nes_mod_runtime_initialize_c(
    const char* root, const char* game_id, const char* rom_crc32) {
    std::string error;
    return NESRecomp::mod_runtime_initialize(
        root ? fs::path(root) : fs::path("mods"),
        game_id ? game_id : "",
        rom_crc32 ? rom_crc32 : "", &error) ? 1 : 0;
}

extern "C" int nes_mod_runtime_commit_c(const char* rom_path) {
    std::string error;
    return NESRecomp::mod_runtime_commit(
        rom_path ? fs::path(rom_path) : fs::path(), &error) ? 1 : 0;
}

extern "C" void nes_mod_runtime_activate_plugins_c(void) {
    NESRecomp::mod_runtime_activate_plugins();
}

extern "C" const char* nes_mod_runtime_last_error_c(void) {
    return NESRecomp::state().error.c_str();
}

extern "C" const RecompLauncherCModProvider*
nes_mod_runtime_launcher_provider_c(void) {
#if defined(RECOMP_LAUNCHER)
    return NESRecomp::state().initialized
        ? NESRecomp::mod_runtime_launcher_provider() : nullptr;
#else
    return nullptr;
#endif
}
