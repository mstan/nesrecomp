/* Optional statically compiled program catalog. No runtime code loading. */
#include "cyc_program.h"
#include "../../common/nes_cart.h"
#include <string.h>

#if CYC_NATIVE_PROGRAMS
const CycProgram *cyc_program_active;
static const CycProgram *catalog;
static size_t catalog_count;
static bool initialized, valid;
static bool no_address(uint16_t address) { (void)address; return false; }
static bool no_byte(uint32_t offset) { (void)offset; return false; }
static void no_run(void) {}
#if NESRECOMP_NATIVE_PRG_GUARDS
static bool no_guard(void) { return false; }
#endif
static const CycProgram empty_program = {
    .has = no_address, .run = no_run, .prg_code_byte = no_byte,
#if NESRECOMP_NATIVE_PRG_GUARDS
    .prg_guard_available = no_guard,
#endif
};

const CycProgram *cyc_program_initialize(void) {
    if (initialized) return cyc_program_active;
    initialized = true;
    catalog = cyc_program_catalog(&catalog_count);
    valid = catalog && catalog_count > 0;
    for (size_t i = 0; valid && i < catalog_count; ++i) {
        const CycProgram *p = &catalog[i];
        valid = p->id && *p->id && p->has && p->run && p->prg_code_byte &&
                (!p->ram_view_count || p->ram_views) &&
                (!p->hook_site_count || p->hook_sites);
#if NESRECOMP_NATIVE_PRG_GUARDS
        valid = valid && p->prg_guard_available;
#endif
        for (size_t j = 0; valid && j < i; ++j)
            if ((p->prg_hash == catalog[j].prg_hash && p->cart_hash == catalog[j].cart_hash) ||
                !strcmp(p->id, catalog[j].id))
                valid = false;
    }
    cyc_program_active = valid ? catalog : &empty_program;
    return cyc_program_active;
}

bool cyc_program_select(uint32_t prg_hash, uint32_t cart_hash) {
    (void)cyc_program_current();
    if (!valid) return false;
    for (size_t i = 0; i < catalog_count; ++i) {
        const CycProgram *p = &catalog[i];
        if (p->prg_hash == prg_hash && p->cart_hash == cart_hash) {
            cyc_program_active = p;
            return true;
        }
    }
    return false;
}

bool cyc_program_catalog_has_hook(const char *id, uint16_t addr) {
    (void)cyc_program_current();
    if (!valid) return false;
    for (size_t p = 0; p < catalog_count; ++p) {
        for (uint32_t i = 0; i < catalog[p].hook_site_count; ++i) {
            const CycHookSite *site = &catalog[p].hook_sites[i];
            if (site->addr == addr &&
                (!site->id || (id && !strcmp(site->id, id)))) return true;
        }
    }
    return false;
}

const CycProgram *cyc_program_match_image(const uint8_t *image, size_t size) {
    NesCartInfo info;
    if (!nes_cart_image(image, size, &info)) return NULL;
    (void)cyc_program_current();
    if (!valid) return NULL;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < info.prg_size; ++i)
        hash = (hash ^ image[info.data_offset + i]) * 16777619u;
    uint32_t cart = nes_cart_identity(&info);
    for (size_t i = 0; i < catalog_count; ++i)
        if (catalog[i].prg_hash == hash && catalog[i].cart_hash == cart)
            return &catalog[i];
    return NULL;
}
#endif
