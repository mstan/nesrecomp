/* mod_test_extras.c - a test game's additions (cyc_host_extras.h) for the mod
 * surface tests (tools/cyc/test_cyc_mods.py, fixtures tools/cyc/fds_mod_fixtures.py).
 *
 *   --test-mode none     registered plugins, all disabled: the machine must run
 *                        exactly as a program without them
 *   --test-mode hooks    test.ova counts its fires (overlay A only: the site is
 *                        keyed on its code); test.tail handles the routine,
 *                        storing $77 in $0450 instead of its own $88 in $0451
 *   --test-mode iso      every frame an isolated scope pokes RAM, calls calc and
 *                        dev (a device store), checks calc's result inside and
 *                        that everything is back afterwards: the machine must
 *                        run exactly as with none
 *   --test-mode commit   frame 5: dev committed (refused), then calc committed
 *                        over real RAM poked to 2s: $0440 = 16 for real
 * The plugin's counters are a mod save state record ("test.counters"). At
 * exit it prints "mod-test: ..." with them.
 */
#include "cyc_host_extras.h"
#include "cyc_mod.h"
#include "cyc_core.h"
#include "../../../include/mod_function_hooks.h"
#include "../../../include/mod_savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODE_NONE, MODE_HOOKS, MODE_ISO, MODE_COMMIT, MODE_OBSERVE };
static int s_mode;
static struct {
    uint32_t ova_fires, ova_x_bad, tail_handled, iso_ok, iso_bad, commit_refused, commit_ok, frames;
} c;
static unsigned s_returns,s_observe_ok,s_observe_bad;
static void returned(void) {s_returns++;}

static int ova_hook(uint16_t addr)
{
    (void)addr;
    c.ova_fires++;
    return 0;
}

static int tail_hook(uint16_t addr)
{
    (void)addr;
    CycModRegs r;
    cyc_mod_regs(&r);
    if (r.x != 3) c.ova_x_bad++;              /* the site sees the caller's registers */
    cyc_mod_poke(0x0450, 0x77);
    c.tail_handled++;
    return 1;
}

static uint16_t s_calc = 0x6200, s_dev = 0x6300;

static void iso_frame(void)
{
    const uint8_t *ram = cyc_cpu_ram();
    uint8_t before[0x800];
    memcpy(before, ram, sizeof(before));
    if (!cyc_mod_isolate_begin()) { c.iso_bad++; return; }
    for (int i = 0; i < 8; ++i) cyc_mod_poke((uint16_t)(0x0441 + i), (uint8_t)(i + 1));
    CycModRegs r = { 0, 0, 0, 0xFD, 0x24, 0 };
    bool ok = cyc_mod_call(s_calc, &r) && ram[0x0440] == 36 && r.x == 8;
    CycModRegs d = { 0, 0, 0, 0xF0, 0x24, 0 };
    ok = cyc_mod_call(s_dev, &d) && ram[0x0442] == 1 && ok;   /* a device store is fine inside a scope */
    cyc_mod_isolate_end();
    ok = ok && !memcmp(before, ram, sizeof(before));
    if (ok) c.iso_ok++;
    else c.iso_bad++;
}

static void commit_frame(void)
{
    CycModRegs r = { 0, 0, 0, 0xF0, 0x24, 0 };
    if (!cyc_mod_call_commit(s_dev, &r)) c.commit_refused++;
    for (int i = 0; i < 8; ++i) cyc_mod_poke((uint16_t)(0x0441 + i), 2);
    CycModRegs q = { 0, 0, 0, 0xF0, 0x24, 0 };
    if (cyc_mod_call_commit(s_calc, &q) && cyc_cpu_ram()[0x0440] == 16) c.commit_ok++;
}

static void observe_frame(void) {
    uint8_t before[0x800];memcpy(before,cyc_cpu_ram(),sizeof before);
    unsigned old=s_returns,tail=c.tail_handled;
    if(!cyc_mod_isolate_begin()){s_observe_bad++;return;}
    cyc_mod_allow_isolated_hooks(true);
    nes_mod_set_function_hook_enabled("test.tail",1);
    CycModRegs r={0,3,0,0xfd,0x24,0};
    bool ok=cyc_mod_call(0x6400,&r) && cyc_mod_peek(0x0450)==0x77 && c.tail_handled==tail+1 && s_returns==old+1;
    nes_mod_set_function_hook_enabled("test.tail",0);
    cyc_mod_allow_isolated_hooks(false);
    ok=cyc_mod_call(0x6400,&r) && cyc_mod_peek(0x0451)==0x88 && s_returns==old+1 && ok;
    cyc_mod_allow_isolated_hooks(true);
    ok=cyc_mod_call(s_calc,&r) && s_returns==old+2 && ok;
    cyc_mod_isolate_end();
    ok=!memcmp(before,cyc_cpu_ram(),sizeof before) && ok;
    if(ok)s_observe_ok++;else s_observe_bad++;
}

static void frame_end(void *ctx)
{
    (void)ctx;
    c.frames++;
    if (s_mode == MODE_ISO && cyc_cpu_ram()[0x0460]) iso_frame();   /* once the program runs */
    if (s_mode == MODE_COMMIT && c.frames == 5) commit_frame();
    if (s_mode == MODE_OBSERVE && cyc_cpu_ram()[0x0460]) observe_frame();
}

static void report(void)
{
    printf("mod-test: mode=%d frames=%u ova_fires=%u tail_handled=%u regs_bad=%u iso_ok=%u iso_bad=%u "
           "commit_refused=%u commit_ok=%u\n", s_mode, c.frames, c.ova_fires, c.tail_handled, c.ova_x_bad, c.iso_ok,
           c.iso_bad, c.commit_refused, c.commit_ok);
    fflush(stdout);
    if(s_mode==MODE_OBSERVE)printf("return-test: ok=%u bad=%u returns=%u\n",s_observe_ok,s_observe_bad,s_returns);
}

static void power_on(void *ctx)
{
    (void)ctx;
    static bool once;
    if (!once) { once = true; atexit(report); }
}

static const CycHostOption OPTIONS[] = {
    { "--test-mode", true, "none | hooks | iso | commit" },
    { "--test-calc", true, "calc routine address (hex)" },
};

static bool option(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    if (!strcmp(name, "--test-calc")) { s_calc = (uint16_t)strtoul(value, NULL, 16); s_dev = (uint16_t)(s_calc + 0x100); return true; }
    if (strcmp(name, "--test-mode")) return false;
    s_mode = !strcmp(value, "hooks") ? MODE_HOOKS : !strcmp(value, "iso") ? MODE_ISO
           : !strcmp(value, "commit") ? MODE_COMMIT : !strcmp(value,"observe") ? MODE_OBSERVE : !strcmp(value, "none") ? MODE_NONE : -1;
    if (s_mode < 0) return false;
    nes_mod_set_function_hook_enabled("test.ova", s_mode == MODE_HOOKS);
    nes_mod_set_function_hook_enabled("test.tail", s_mode == MODE_HOOKS);
    cyc_mod_set_return_hook(s_mode==MODE_OBSERVE?returned:NULL);
    return true;
}

static const CycHostExtras EXTRAS = {
    .power_on = power_on,
    .frame_end = frame_end,
    .options = OPTIONS,
    .option_count = 2,
    .option = option,
};

const CycHostExtras *cyc_host_extras(void) { return &EXTRAS; }

static int save(uint8_t *buf, int cap)
{
    if (cap < (int)sizeof(c)) return -1;
    memcpy(buf, &c, sizeof(c));
    return (int)sizeof(c);
}

static int load(const uint8_t *buf, int len)
{
    if (!len) { memset(&c, 0, sizeof(c)); return 1; }
    if (len != (int)sizeof(c)) return 0;
    memcpy(&c, buf, sizeof(c));
    return 1;
}

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
static void __cdecl register_test(void);
__declspec(allocate(".CRT$XCU")) void (__cdecl *register_test_constructor)(void) = register_test;
#pragma comment(linker, "/include:register_test_constructor")
static void __cdecl register_test(void)
#else
static void register_test(void) __attribute__((constructor));
static void register_test(void)
#endif
{
    if (!nes_mod_register_function_entry_plugin("test.ova", 0x7000, ova_hook) ||
        !nes_mod_register_function_entry_plugin("test.tail", 0x6400, tail_hook) ||
        !nes_mod_register_savestate_hook("test.counters", save, load)
#ifdef MOD_TEST_BAD_SITE
        || !nes_mod_register_function_entry_plugin("test.bad", 0x6123, ova_hook)
#endif
        )
        fprintf(stderr, "mod-test: registration failed\n");
}
