#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace NESRecomp {

/*
 * Initialize the package catalog rooted at <exe>/mods for one verified game.
 * The ROM digest is the canonical lowercase CRC32 of the payload after the
 * 16-byte iNES header.
 */
bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            const std::string& rom_crc32,
                            std::string* error = nullptr);

/*
 * Resolve staged feature selections, validate the selected stock ROM, persist
 * state, and prepare the trusted-plugin activation plan.
 */
bool mod_runtime_commit(const std::filesystem::path& rom_path = {},
                        std::string* error = nullptr);

/* Invoke the trusted, statically linked plugins selected by the committed plan. */
void mod_runtime_activate_plugins();

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
#endif

}  // namespace NESRecomp
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*NESModActivationCallback)(void);

struct RecompLauncherCModProvider;

int nes_mod_runtime_initialize_c(const char* root,
                                 const char* game_id,
                                 const char* rom_crc32);
int nes_mod_runtime_commit_c(const char* rom_path);
void nes_mod_runtime_activate_plugins_c(void);
const struct RecompLauncherCModProvider*
nes_mod_runtime_launcher_provider_c(void);
const char* nes_mod_runtime_last_error_c(void);

/*
 * Register a trusted implementation. A .nesmod archive may select only this
 * stable id; archives never provide native code, symbols, or library paths.
 */
int nes_mod_register_activation_plugin(const char* id,
                                       NESModActivationCallback callback);

/*
 * Register game-owned stock policy. Reset callbacks run before active plugins,
 * so disabling a feature reliably restores stock behavior every launch.
 */
int nes_mod_register_reset_callback(NESModActivationCallback callback);

/* Read a persisted option for an active trusted plugin. Returns fallback when
 * the package, feature, option, or integer value is unavailable. */
int nes_mod_get_option_int(const char* package_id,
                           const char* feature_id,
                           const char* option_id,
                           int fallback);

/*
 * Read the committed value of one of this package's declared options, as the
 * player left it in the launcher (or the manifest default when untouched).
 * Writes a NUL-terminated string into `out` and returns 1; returns 0 with
 * out[0] = '\0' when the plan is not committed, the ids do not resolve, or the
 * value does not fit — the caller then applies its own default rather than
 * treating an empty string as a selection.
 *
 * Why this exists: the manifest schema already carries typed, validated,
 * launcher-rendered, persisted options ([[option]] boolean/choice/integer),
 * and nes_mod_get_option_int can read the integer ones, but a choice value had
 * no accessor at all — so a parameterised feature had to be modelled as one
 * feature per value. This closes that gap: one feature, one option, the plugin
 * reads what was chosen. Mirrors psx_mod_option_value in psxrecomp.
 *
 * Prefer when_option/when_value on [[plugin]] when the choice merely selects
 * which implementation runs; use this when the plugin must act on the value.
 *
 * Ids are passed explicitly because registration is by plugin id alone and the
 * activation callback carries no package/feature context.
 */
int nes_mod_option_value(const char* package_id,
                         const char* feature_id,
                         const char* option_id,
                         char* out,
                         uint32_t out_size);

/*
 * Return the path selected for a required external ROM, but only when that
 * exact package/feature/resource is part of the successfully committed
 * trusted-plugin plan. The resource is revalidated at commit time; pending
 * launcher selections and resources belonging to inactive features are never
 * exposed. Returns NULL before a successful commit or when the ids do not
 * name a committed resource. The returned storage belongs to the runtime and
 * remains valid until the next runtime initialize/commit operation.
 *
 * This exposes a path only. Plugins open the owner-supplied file themselves;
 * the mod runtime never supplies ROM bytes.
 */
const char* nes_mod_external_rom_path(const char* package_id,
                                      const char* feature_id,
                                      const char* resource_id);

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#if defined(_M_IX86)
#define NES_MOD_LINKER_PREFIX "_"
#else
#define NES_MOD_LINKER_PREFIX ""
#endif
#define NES_MOD_CONSTRUCTOR(name)                                           \
    static void __cdecl name(void);                                         \
    __pragma(comment(linker, "/include:" NES_MOD_LINKER_PREFIX               \
                     #name "_constructor"))                                  \
    __declspec(allocate(".CRT$XCU"))                                        \
    void (__cdecl* name##_constructor)(void) = name;                        \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define NES_MOD_CONSTRUCTOR(name)                                           \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "NES mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif
