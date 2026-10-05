/* cyc_session.c - see cyc_session.h. */
#include "cyc_session.h"

#include "cyc_hooks.h"
#include "cyc_mod.h"
#include "cyc_render.h"

#include <stdlib.h>
#include <string.h>

#ifndef NESRECOMP_ENABLE_MODS
#define NESRECOMP_ENABLE_MODS 0
#endif
#if NESRECOMP_ENABLE_MODS
#include "../include/mod_runtime.h"
#ifndef CYC_MOD_GAME_ID
#error "a cycle game built with mods defines CYC_MOD_GAME_ID (nesrecomp_add_cycle_game MODS GAME_ID)"
#endif
#ifndef CYC_MOD_ROM_CRC32
#error "a cycle game built with mods defines CYC_MOD_ROM_CRC32 (prepare_project.py)"
#endif
#endif

static const CycHostExtras *s_extras;
static bool s_extras_read;

const CycHostExtras *cyc_session_extras(void)
{
    if (!s_extras_read) {
        s_extras = cyc_host_extras();
        s_extras_read = true;
    }
    return s_extras;
}

/* ---- the game's options ---- */

typedef struct { const char *name, *value; } Taken;
static Taken  *s_taken;
static size_t  s_taken_n;

bool cyc_session_take_option(int argc, char **argv, int *i, bool *bad)
{
    const CycHostExtras *x = cyc_session_extras();
    *bad = false;
    if (!x || !x->options) return false;
    for (size_t k = 0; k < x->option_count; ++k) {
        const CycHostOption *o = &x->options[k];
        if (strcmp(argv[*i], o->name)) continue;
        const char *value = NULL;
        if (o->takes_value) {
            if (*i + 1 >= argc) { *bad = true; return true; }
            value = argv[++*i];
        }
        Taken *grown = (Taken *)realloc(s_taken, sizeof(Taken) * (s_taken_n + 1));
        if (!grown) { *bad = true; return true; }
        s_taken = grown;
        s_taken[s_taken_n].name = o->name;
        s_taken[s_taken_n].value = value;
        s_taken_n++;
        return true;
    }
    return false;
}

void cyc_session_print_options(FILE *f)
{
    const CycHostExtras *x = cyc_session_extras();
    if (!x || !x->options || !x->option_count) return;
    fprintf(f, "       game options:\n");
    for (size_t k = 0; k < x->option_count; ++k)
        fprintf(f, "         %s%s  %s\n", x->options[k].name, x->options[k].takes_value ? " VALUE" : "",
                x->options[k].help ? x->options[k].help : "");
}

/* ---- mods ---- */

static bool s_mods;

bool cyc_session_mods_init(const char *root, char *err, size_t err_len)
{
#if NESRECOMP_ENABLE_MODS
    if (!root) return true;
    if (!nes_mod_runtime_initialize_c(root, CYC_MOD_GAME_ID, CYC_MOD_ROM_CRC32)) {
        snprintf(err, err_len, "cannot read the mod catalog %s: %s", root, nes_mod_runtime_last_error_c());
        return false;
    }
    s_mods = true;
    return true;
#else
    (void)err;
    (void)err_len;
    if (root) fprintf(stderr, "[mods] this program is built without mod support; %s is not read\n", root);
    return true;
#endif
}

bool cyc_session_mods_enabled(void) { return s_mods; }

const void *cyc_session_mods_provider(void)
{
#if NESRECOMP_ENABLE_MODS
    return s_mods ? (const void *)nes_mod_runtime_launcher_provider_c() : NULL;
#else
    return NULL;
#endif
}

bool cyc_session_mods_start(const char *image_path, char *err, size_t err_len)
{
#if NESRECOMP_ENABLE_MODS
    if (!s_mods) return true;
    if (!nes_mod_runtime_commit_c(image_path)) {
        snprintf(err, err_len, "mods: %s", nes_mod_runtime_last_error_c());
        return false;
    }
    nes_mod_runtime_activate_plugins_c();
    return true;
#else
    (void)image_path;
    (void)err;
    (void)err_len;
    return true;
#endif
}

void cyc_session_mods_reapply(void)
{
#if NESRECOMP_ENABLE_MODS
    if (s_mods) nes_mod_runtime_activate_plugins_c();
#endif
}

/* ---- the run ---- */

bool cyc_session_start(void)
{
    if (!cyc_hooks_validate()) {
        fprintf(stderr, "[cyc hooks] a mod plugin is registered for a hook site this program does not declare; "
                        "fix game.toml [[mod_function_hook]] or the plugin\n");
        return false;
    }
    const CycHostExtras *x = cyc_session_extras();
    for (size_t k = 0; k < s_taken_n; ++k) {
        if (!x->option || !x->option(x->ctx, s_taken[k].name, s_taken[k].value)) {
            fprintf(stderr, "%s: invalid value%s%s\n", s_taken[k].name, s_taken[k].value ? " " : "",
                    s_taken[k].value ? s_taken[k].value : "");
            return false;
        }
    }
    if (x && x->power_on) x->power_on(x->ctx);
    cyc_render_frame_done();
    return true;
}

uint8_t g_logical_input[4];
uint8_t nes_input_seat(int seat) {
    return seat >= 1 && seat <= 4 ? g_logical_input[seat-1] : 0;
}
void cyc_session_logical_input(const uint8_t *buttons, unsigned seats) {
    memset(g_logical_input,0,sizeof g_logical_input);
    if (seats>4) seats=4;
    if (buttons) memcpy(g_logical_input,buttons,seats);
}
void cyc_session_input(uint8_t buttons[2]) {
    g_logical_input[0]=buttons[0]; g_logical_input[1]=buttons[1];
    const CycHostExtras *x = cyc_session_extras();
    if (x && x->input) x->input(x->ctx, buttons);
}
void cyc_session_event(const void *event, int player) {
    const CycHostExtras *x = cyc_session_extras();
    if (x && x->event) x->event(x->ctx, event, player);
}

void cyc_session_frame_begin(void)
{
    const CycHostExtras *x = cyc_session_extras();
    if (x && x->frame_begin) x->frame_begin(x->ctx);
}

void cyc_session_frame_end(void)
{
    /* A new picture first: the game's frame_end may compose it
     * (cyc_render_present), the one the window presents next. */
    cyc_render_frame_done();
    const CycHostExtras *x = cyc_session_extras();
    if (x && x->frame_end) x->frame_end(x->ctx);
    cyc_mod_frame_end();          /* the ring's summary of the calls it made */
}

void cyc_session_state_loaded(void)
{
    const CycHostExtras *x=cyc_session_extras();
    if(x&&x->state_loaded)x->state_loaded(x->ctx);
    cyc_render_frame_done();
}
unsigned cyc_session_audio_rate(unsigned fallback) {
    const CycHostExtras *x=cyc_session_extras();
    return x&&x->audio_rate?x->audio_rate:fallback;
}
void cyc_session_audio_mix(int16_t *samples,size_t count) {
    const CycHostExtras *x=cyc_session_extras();
    if(x&&x->audio_mix)x->audio_mix(x->ctx,samples,count);
}
