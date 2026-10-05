/*
 * cyc_ui_menu.c - the windowed cycle host's runtime menu rows (cyc_ui.h).
 *
 * recomp-ui draws and navigates; this file says what the rows are and what
 * they do. Console-specific rows are the host's extra_items: recomp-ui itself
 * knows nothing about disks.
 */
#include "cyc_ui.h"
#include "cyc_settings.h"

#include "cyc_core.h"
#include "cyc_disk_action.h"
#include "cyc_host.h"
#include "cyc_run.h"
#include "recomp_runtime_ui.h"
#include "recomp_launcher.h"
#if NESRECOMP_ENABLE_MODS
#include "mod_runtime.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIDES 16
#define MAX_AXES 8

static RecompRuntimeUi *s_ui;
static CycUiHost s_host;
static RecompRuntimeUiItem s_items[128];
static size_t s_item_count;
static unsigned s_axes;

/* dynamic text the rows point at */
static char s_side_names[MAX_SIDES + 1][24];
static const char *s_side_choices[MAX_SIDES + 1];
static int s_side_values[MAX_SIDES + 1];
static char s_drive_desc[128], s_eject_label[48];
static char s_axis_desc[MAX_AXES][160];
static char s_shortcut_label[CYC_SC_COUNT][96];
static char s_axis_key[MAX_AXES][24];
static char s_shortcut_key[CYC_SC_COUNT][24];
static uint64_t s_quit_armed;

/* ---- Mods: every feature of the mod runtime's catalog (cyc_session.h), with
 * its options, edited live. A change goes to the runtime (the same calls the
 * launcher's Mods screen makes), is committed for the running image - which
 * also persists it - and the plugins activate again (reset callbacks first),
 * so the game applies it at once. ---- */
static void add(const char *key, const char *section, const char *label, const char *desc,
                RecompRuntimeUiItemType type, int min, int max, int step, const char *const *choices,
                size_t choice_count, const int *values);
#define MAX_MOD_ROWS 48
#define MAX_MOD_CHOICES 16
typedef struct {
    char key[24], label[160], desc[512];
    char package[RECOMP_LAUNCHER_MOD_ID_MAX], feature[RECOMP_LAUNCHER_MOD_ID_MAX], option[RECOMP_LAUNCHER_MOD_ID_MAX];
    int  type;                         /* -1: the feature's on/off; RECOMP_MOD_OPTION_* */
    char value[MAX_MOD_CHOICES][RECOMP_LAUNCHER_MOD_VALUE_MAX];
    char name[MAX_MOD_CHOICES][128];
    const char *names[MAX_MOD_CHOICES];
    int  count;
} ModRow;
static ModRow s_mod[MAX_MOD_ROWS];
static int    s_mod_count;

static const RecompLauncherCModProvider *mods(void) { return (const RecompLauncherCModProvider *)s_host.mods; }
static ModRow *mod_row(const RecompRuntimeUiItem *it)
{
    if (!it->key || strncmp(it->key, "cyc.mod.", 8)) return NULL;
    int i = atoi(it->key + 8);
    return i >= 0 && i < s_mod_count ? &s_mod[i] : NULL;
}

static bool mod_feature(const ModRow *m, RecompLauncherCModFeature *out)
{
    const RecompLauncherCModProvider *p = mods();
    int n = p && p->feature_count ? p->feature_count(p->ctx) : 0;
    for (int i = 0; i < n; ++i)
        if (p->feature_get(p->ctx, i, out) && !strcmp(out->package_id, m->package) && !strcmp(out->id, m->feature))
            return true;
    return false;
}

static bool mod_option(const ModRow *m, RecompLauncherCModOption *out)
{
    const RecompLauncherCModProvider *p = mods();
    for (int i = 0; p && p->feature_option_get && p->feature_option_get(p->ctx, m->package, m->feature, i, out); ++i)
        if (!strcmp(out->id, m->option)) return true;
    return false;
}

static void add_mod_rows(void)
{
    const RecompLauncherCModProvider *p = mods();
    s_mod_count = 0;
    if (!p || !p->feature_count || !p->feature_get || !p->feature_option_get) return;
    int features = p->feature_count(p->ctx);
    for (int f = 0; f < features && s_mod_count < MAX_MOD_ROWS; ++f) {
        RecompLauncherCModFeature feat;
        if (!p->feature_get(p->ctx, f, &feat) || (feat.hidden && !feat.enabled)) continue;
        ModRow *m = &s_mod[s_mod_count];
        memset(m, 0, sizeof(*m));
        snprintf(m->package, sizeof(m->package), "%s", feat.package_id);
        snprintf(m->feature, sizeof(m->feature), "%s", feat.id);
        snprintf(m->key, sizeof(m->key), "cyc.mod.%d", s_mod_count);
        snprintf(m->label, sizeof(m->label), "%s", feat.name);
        snprintf(m->desc, sizeof(m->desc), "%s", feat.description);
#if NESRECOMP_ENABLE_MODS
        if (nes_mod_feature_requires_restart(feat.package_id, feat.id))
            snprintf(m->desc, sizeof(m->desc), "Change this display mode in the launcher, then restart the game.");
#endif
        m->type = -1;
        add(m->key, "Mods", m->label, m->desc, RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL);
        s_mod_count++;
        RecompLauncherCModOption o;
        for (int i = 0; s_mod_count < MAX_MOD_ROWS && p->feature_option_get(p->ctx, feat.package_id, feat.id, i, &o); ++i) {
            m = &s_mod[s_mod_count];
            memset(m, 0, sizeof(*m));
            snprintf(m->package, sizeof(m->package), "%s", feat.package_id);
            snprintf(m->feature, sizeof(m->feature), "%s", feat.id);
            snprintf(m->option, sizeof(m->option), "%s", o.id);
            snprintf(m->key, sizeof(m->key), "cyc.mod.%d", s_mod_count);
            /* The option's own label (the row value needs the width); the
             * description line names its feature. */
            snprintf(m->label, sizeof(m->label), "%s", o.label);
            if (o.description[0]) snprintf(m->desc, sizeof(m->desc), "%s: %s", feat.name, o.description);
            else snprintf(m->desc, sizeof(m->desc), "%s", feat.name);
            m->type = o.type;
            if (o.type == RECOMP_MOD_OPTION_CHOICE) {
                RecompLauncherCModChoice ch;
                for (int c = 0; m->count < MAX_MOD_CHOICES && p->feature_choice_get &&
                                p->feature_choice_get(p->ctx, feat.package_id, feat.id, o.id, c, &ch); ++c) {
                    snprintf(m->value[m->count], sizeof(m->value[0]), "%s", ch.value);
                    snprintf(m->name[m->count], sizeof(m->name[0]), "%s", ch.label);
                    m->names[m->count] = m->name[m->count];
                    m->count++;
                }
                if (!m->count) continue;
                add(m->key, "Mods", m->label, m->desc, RECOMP_RUNTIME_UI_CHOICE, 0, m->count - 1, 1, m->names,
                    (size_t)m->count, NULL);
            } else if (o.type == RECOMP_MOD_OPTION_BOOLEAN) {
                add(m->key, "Mods", m->label, m->desc, RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL);
            } else if (o.type == RECOMP_MOD_OPTION_INTEGER) {
                add(m->key, "Mods", m->label, m->desc, RECOMP_RUNTIME_UI_INT, (int)o.min_value, (int)o.max_value,
                    o.step > 0 ? (int)o.step : 1, NULL, 0, NULL);
            } else {
                continue;                  /* text options stay on the launcher's Mods screen */
            }
            s_mod_count++;
        }
    }
}

static int mod_get(const ModRow *m, int *out)
{
    if (m->type < 0) {
        RecompLauncherCModFeature feat;
        if (!mod_feature(m, &feat)) return 0;
        *out = feat.enabled != 0;
        return 1;
    }
    RecompLauncherCModOption o;
    if (!mod_option(m, &o)) return 0;
    if (m->type == RECOMP_MOD_OPTION_CHOICE) {
        for (int i = 0; i < m->count; ++i)
            if (!strcmp(m->value[i], o.value)) { *out = i; return 1; }
        *out = 0;
    } else if (m->type == RECOMP_MOD_OPTION_BOOLEAN) {
        *out = !strcmp(o.value, "true") || !strcmp(o.value, "1");
    } else {
        *out = atoi(o.value);
    }
    return 1;
}

static int mod_set(const ModRow *m, int v)
{
    const RecompLauncherCModProvider *p = mods();
    int ok;
    if (m->type < 0) {
        ok = p->feature_enable && p->feature_enable(p->ctx, m->package, m->feature, v != 0);
    } else {
        char value[RECOMP_LAUNCHER_MOD_VALUE_MAX];
        if (m->type == RECOMP_MOD_OPTION_CHOICE) {
            if (v < 0 || v >= m->count) return 0;
            snprintf(value, sizeof(value), "%s", m->value[v]);
        } else if (m->type == RECOMP_MOD_OPTION_BOOLEAN) {
            snprintf(value, sizeof(value), "%s", v ? "true" : "false");
        } else {
            snprintf(value, sizeof(value), "%d", v);
        }
        ok = p->feature_set_option && p->feature_set_option(p->ctx, m->package, m->feature, m->option, value);
    }
    if (ok) ok = p->commit && p->commit(p->ctx, s_host.image);
    if (!ok) {
        const char *why = p->last_error ? p->last_error(p->ctx) : NULL;
        recomp_runtime_ui_set_status(s_ui, why && *why ? why : "The mod setting was not applied");
        return 0;
    }
    if (s_host.mods_changed) s_host.mods_changed();
    recomp_runtime_ui_set_status(s_ui, "Mod setting applied");
    return 1;
}

static int mod_enabled(const ModRow *m)
{
#if NESRECOMP_ENABLE_MODS
    if (nes_mod_feature_requires_restart(m->package, m->feature)) return 0;
#endif
    if (m->type < 0) return 1;
    RecompLauncherCModFeature feat;
    RecompLauncherCModOption o;
    return mod_feature(m, &feat) && feat.enabled && mod_option(m, &o) && !o.disabled;
}

static const char *const HLE_CHOICES[] = { "Game default", "On", "Off" };
static const int HLE_VALUES[] = { -1, 1, 0 };

static bool is_key(const RecompRuntimeUiItem *it, const char *k) { return it->key && !strcmp(it->key, k); }
static bool game_row(const RecompRuntimeUiItem *it)
{
    return it->key && strncmp(it->key, "cyc.", 4) && strncmp(it->key, "display.", 8) &&
           strncmp(it->key, "graphics.", 9) && strncmp(it->key, "audio.", 6) && strncmp(it->key, "system.", 7);
}

static int axis_of(const RecompRuntimeUiItem *it)
{
    return it->key && !strncmp(it->key, "cyc.hle.", 8) ? atoi(it->key + 8) : -1;
}

static void add(const char *key, const char *section, const char *label, const char *desc,
                RecompRuntimeUiItemType type, int min, int max, int step, const char *const *choices,
                size_t choice_count, const int *values)
{
    if (s_item_count >= sizeof(s_items) / sizeof(s_items[0])) return;
    RecompRuntimeUiItem *it = &s_items[s_item_count++];
    memset(it, 0, sizeof(*it));
    it->key = key;
    it->section = section;
    it->label = label;
    it->description = desc;
    it->type = type;
    it->minimum = min;
    it->maximum = max;
    it->step = step;
    it->choices = choices;
    it->choice_count = choice_count;
    it->choice_values = values;
}

/* Refresh every row whose text follows the machine (before each draw). */
static void refresh(void)
{
    if (s_host.fds) {
        CycDiskToast t;
        cyc_disk_action_toast(cyc_host_disk_action(), s_host.now_ms(), &t);
        const char *save = cyc_host_disk_save_status();
        if (t.waiting_write) snprintf(s_drive_desc, sizeof(s_drive_desc), "Waiting for the disk write to finish.");
        else if (t.swapping) snprintf(s_drive_desc, sizeof(s_drive_desc), "Swapping: the drive is empty for a moment.");
        else if (t.side >= 0) snprintf(s_drive_desc, sizeof(s_drive_desc), "Motor %s.%s%s", t.motor ? "on" : "off",
                                       save && *save ? " Save: " : "", save ? save : "");
        else snprintf(s_drive_desc, sizeof(s_drive_desc), "The drive is empty.");
        if (t.side >= 0 || t.swapping) snprintf(s_eject_label, sizeof(s_eject_label), "Eject the disk");
        else {
            char name[24];
            cyc_disk_side_name(name, sizeof(name), (int)cyc_host_disk_selected() % (int)(t.sides ? t.sides : 1));
            snprintf(s_eject_label, sizeof(s_eject_label), "Insert %s", name);
        }
        unsigned n;
        const NesFdsHleAxis *ax = nes_fds_hle_axes(&n);
        const NesFdsHlePlan *p = cyc_host_hle_plan();
        for (unsigned a = 0; a < s_axes; ++a) {
            const char *from = nes_fds_hle_plan_from(p, &ax[a]);
            if (nes_fds_hle_plan_denied(p, &ax[a]))
                snprintf(s_axis_desc[a], sizeof(s_axis_desc[a]), "Unavailable: %s.", nes_fds_hle_plan_why(p, &ax[a]));
            else
                snprintf(s_axis_desc[a], sizeof(s_axis_desc[a]), "%s Now %s (%s).", ax[a].help,
                         nes_fds_hle_plan_on(p, &ax[a]) ? "on" : "off",
                         !strcmp(from, "game.toml") ? "the game's default" : !strcmp(from, "settings") ? "your setting"
                                                                             : from);
        }
    }
    for (int i = 0; i < CYC_SC_COUNT; ++i) {
        char hint[64];
        cyc_binding_hint(&s_host.settings->bind.shortcut[i], hint, sizeof(hint));
        if (!s_host.settings->bind.shortcut[i].key && !s_host.settings->bind.shortcut[i].pad)
            snprintf(hint, sizeof(hint), "unbound");
        snprintf(s_shortcut_label[i], sizeof(s_shortcut_label[i]), "%s: %s", cyc_shortcut_label(i), hint);
    }
}

static int get_value(void *ctx, const RecompRuntimeUiItem *it, int *out)
{
    (void)ctx;
    const CycSettings *s = s_host.settings;
    const CycHostExtras *x = s_host.extras;
    if (is_key(it, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) *out = s->fullscreen;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) *out = s->window_scale;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_INTEGER_SCALE)) *out = s->integer_scale;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) *out = s->linear_filter;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_AUDIO)) *out = s->audio_enabled;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_VOLUME)) *out = s->volume;
    else if (is_key(it, "cyc.zapper.mouse")) *out = s->zapper_mouse;
    else if (is_key(it, "cyc.zapper.crosshair")) *out = s->zapper_crosshair;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_VIEW_MODE)) *out = x && x->get_view_mode ? x->get_view_mode(x->ctx) : s->view_mode;
    else if (mod_row(it)) return mod_get(mod_row(it), out);
    else if (is_key(it, "cyc.disk.side")) {
        CycDiskToast t;
        cyc_disk_action_toast(cyc_host_disk_action(), s_host.now_ms(), &t);
        *out = t.swapping ? t.target : t.side;
    } else if (axis_of(it) >= 0) *out = *nes_fds_hle_ask_axis(&s_host.settings->fds_hle, &nes_fds_hle_axes(NULL)[axis_of(it)]);
    else if (game_row(it) && x && x->menu_callbacks && x->menu_callbacks->get_value)
        return x->menu_callbacks->get_value(x->menu_callbacks->context, it, out);
    else return 0;
    return 1;
}

static int set_value(void *ctx, const RecompRuntimeUiItem *it, int v)
{
    (void)ctx;
    CycSettings *s = s_host.settings;
    const CycHostExtras *x = s_host.extras;
    if (is_key(it, RECOMP_RUNTIME_UI_KEY_FULLSCREEN)) s->fullscreen = v < 0 ? 0 : v > 2 ? 2 : v;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) s->window_scale = v < 1 ? 1 : v > 8 ? 8 : v;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_INTEGER_SCALE)) s->integer_scale = v != 0;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER)) s->linear_filter = v != 0;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_AUDIO)) s->audio_enabled = v != 0;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_VOLUME)) s->volume = v < 0 ? 0 : v > 100 ? 100 : v;
    else if (is_key(it, "cyc.zapper.mouse")) s->zapper_mouse = v != 0;
    else if (is_key(it, "cyc.zapper.crosshair")) s->zapper_crosshair = v != 0;
    else if (is_key(it, RECOMP_RUNTIME_UI_KEY_VIEW_MODE)) {
        if (!x || !x->set_view_mode || !x->set_view_mode(x->ctx, v)) return 0;
        s->view_mode = v;
    } else if (mod_row(it)) {
        return mod_set(mod_row(it), v);
    } else if (is_key(it, "cyc.disk.side")) {
        long f = s_host.frames_done();
        if (v < 0) return cyc_host_disk_eject(f);
        return cyc_host_disk_choose(s_host.now_ms(), f, (unsigned)v) != CYC_DISK_PRESS_NONE;
    } else if (axis_of(it) >= 0) {
        int a = axis_of(it);
        *nes_fds_hle_ask_axis(&s->fds_hle, &nes_fds_hle_axes(NULL)[a]) = (int8_t)v;
        cyc_host_hle_user_set((unsigned)a, (int8_t)v);
    } else if (game_row(it) && x && x->menu_callbacks && x->menu_callbacks->set_value)
        return x->menu_callbacks->set_value(x->menu_callbacks->context, it, v);
    else return 0;
    if (s_host.apply) s_host.apply();
    return 1;
}

static int run_action(void *ctx, const RecompRuntimeUiItem *it)
{
    (void)ctx;
    const CycHostExtras *x = s_host.extras;
    long f = s_host.frames_done();
    if (is_key(it, RECOMP_RUNTIME_UI_KEY_RESUME)) { recomp_runtime_ui_close(s_ui); return 1; }
    if (is_key(it, "cyc.disk.eject")) {
        CycDiskToast t;
        cyc_disk_action_toast(cyc_host_disk_action(), s_host.now_ms(), &t);
        if (t.side >= 0) return cyc_host_disk_eject(f);
        if (t.swapping) {                       /* the drive is already out: keep it out */
            cyc_disk_action_cancel(cyc_host_disk_action());
            return 1;
        }
        unsigned n = cyc_fds_side_count();
        return n && cyc_host_disk_insert(f, cyc_host_disk_selected() % n);
    }
    if (is_key(it, "cyc.disk.next")) {
        /* the Disk action's second press, whatever the toast says */
        CycDiskAction *a = cyc_host_disk_action();
        uint64_t now = s_host.now_ms();
        CycDiskToast t;
        if (!cyc_disk_action_toast(a, now, &t)) cyc_host_disk_press(now, f);
        return cyc_host_disk_press(now, f) != CYC_DISK_PRESS_NONE;
    }
    if (is_key(it, RECOMP_RUNTIME_UI_KEY_SAVE_STATE) && s_host.save_state) {
        bool ok = s_host.save_state();
        recomp_runtime_ui_set_status(s_ui, ok ? "State saved" : "The state was not saved");
        return ok;
    }
    if (is_key(it, RECOMP_RUNTIME_UI_KEY_LOAD_STATE) && s_host.load_state) {
        bool ok = s_host.load_state();
        recomp_runtime_ui_set_status(s_ui, ok ? "State loaded" : "No state to load for this game");
        if (ok) recomp_runtime_ui_close(s_ui);
        return ok;
    }
    if (is_key(it, "cyc.quit")) {
        uint64_t now = s_host.now_ms();
        if (s_quit_armed && now - s_quit_armed < 3000) { s_host.quit(); return 1; }
        s_quit_armed = now;
        recomp_runtime_ui_set_status(s_ui, "Press again to quit");
        return 0;
    }
    if (game_row(it) && x && x->menu_callbacks && x->menu_callbacks->run_action)
        return x->menu_callbacks->run_action(x->menu_callbacks->context, it);
    return 0;
}

static int is_enabled(void *ctx, const RecompRuntimeUiItem *it)
{
    (void)ctx;
    const CycHostExtras *x = s_host.extras;
    if (it->key && !strncmp(it->key, "cyc.shortcut.", 13)) return 0;   /* information rows */
    if (axis_of(it) >= 0) {
        const NesFdsHleAxis *ax = &nes_fds_hle_axes(NULL)[axis_of(it)];
        const NesFdsHlePlan *p = cyc_host_hle_plan();
        const char *from = nes_fds_hle_plan_from(p, ax);
        /* a source above the saved setting decides; so does a refusal */
        if (!strcmp(from, "env") || !strcmp(from, "cli") || !strcmp(from, "toggle")) return 0;
        return !nes_fds_hle_plan_denied(p, ax) || *nes_fds_hle_ask_axis(&s_host.settings->fds_hle, ax) > 0;
    }
    if (is_key(it, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE)) return s_host.settings->fullscreen == 0;
    if (mod_row(it)) return mod_enabled(mod_row(it));
    if (game_row(it) && x && x->menu_callbacks && x->menu_callbacks->is_enabled)
        return x->menu_callbacks->is_enabled(x->menu_callbacks->context, it);
    return 1;
}

static void save(void *ctx)
{
    (void)ctx;
    if (s_host.save) s_host.save();
}

static void visibility(void *ctx, int open)
{
    (void)ctx;
    s_quit_armed = 0;
    if (open) refresh();
}

RecompRuntimeUi *cyc_ui_menu_create(const CycUiHost *host)
{
    s_host = *host;
    s_item_count = 0;
    const CycHostExtras *x = host->extras;
    if (host->fds) {
        unsigned sides = cyc_fds_side_count();
        if (sides > MAX_SIDES) sides = MAX_SIDES;
        snprintf(s_side_names[0], sizeof(s_side_names[0]), "Empty");
        s_side_choices[0] = s_side_names[0];
        s_side_values[0] = -1;
        for (unsigned i = 0; i < sides; ++i) {
            snprintf(s_side_names[i + 1], sizeof(s_side_names[i + 1]), "Disk %u Side %c", i / 2 + 1, 'A' + (int)(i % 2));
            s_side_choices[i + 1] = s_side_names[i + 1];
            s_side_values[i + 1] = (int)i;
        }
        add("cyc.disk.side", "Disk Drive", "In the drive", s_drive_desc, RECOMP_RUNTIME_UI_CHOICE, -1,
            (int)sides - 1, 1, s_side_choices, sides + 1, s_side_values);
        add("cyc.disk.next", "Disk Drive", "Swap to the next side", "Eject, turn the disk over, insert.",
            RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL);
        add("cyc.disk.eject", "Disk Drive", s_eject_label, "Take the disk out, or put the selected side in.",
            RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL);
        unsigned n;
        const NesFdsHleAxis *ax = nes_fds_hle_axes(&n);
        s_axes = n < MAX_AXES ? n : MAX_AXES;
        for (unsigned a = 0; a < s_axes; ++a) {
            snprintf(s_axis_key[a], sizeof(s_axis_key[a]), "cyc.hle.%u", a);
            add(s_axis_key[a], "Disk Drive", ax[a].label, s_axis_desc[a], RECOMP_RUNTIME_UI_CHOICE, -1, 1, 1,
                HLE_CHOICES, 3, HLE_VALUES);
        }
    }
    for (int i = 0; i < CYC_SC_COUNT; ++i) {
        if (i == CYC_SC_DISK && !host->fds) continue;
        snprintf(s_shortcut_key[i], sizeof(s_shortcut_key[i]), "cyc.shortcut.%d", i);
        add(s_shortcut_key[i], "Controls", s_shortcut_label[i], "Change bindings in the launcher's Controls page.",
            RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, NULL, 0, NULL);
    }
    if (x && x->menu_items)
        /* Game-defined additions follow the host's controls. */
        for (size_t i = 0; i < x->menu_item_count; ++i)
            if (s_item_count < sizeof(s_items) / sizeof(s_items[0])) s_items[s_item_count++] = x->menu_items[i];
    add_mod_rows();
    CycZapperState gun;
    cyc_zapper_state(&gun);
    if (gun.port) {
        add("cyc.zapper.mouse", "Zapper", "Mouse aiming", "Aim with the mouse and fire with the left button.",
            RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL);
        add("cyc.zapper.crosshair", "Zapper", "Crosshair", "Show the aiming marker.",
            RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, NULL, 0, NULL);
    }
    add("cyc.quit", "System", "Quit", "Close the game (the disk save is written first).", RECOMP_RUNTIME_UI_ACTION,
        0, 0, 0, NULL, 0, NULL);
    refresh();

    RecompRuntimeUiStandardConfig std;
    memset(&std, 0, sizeof(std));
    std.menu.title = host->title;
    std.menu.subtitle = host->fds ? "FAMICOM DISK SYSTEM" : "NES";
    std.menu.theme = "nes";
    std.menu.accept_label = "Enter / A";
    std.menu.back_label = "Backspace / B";
    std.menu.callbacks.get_value = get_value;
    std.menu.callbacks.set_value = set_value;
    std.menu.callbacks.run_action = run_action;
    std.menu.callbacks.is_enabled = is_enabled;
    std.menu.callbacks.save = save;
    std.menu.callbacks.visibility_changed = visibility;
    std.features = RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN | RECOMP_RUNTIME_UI_STANDARD_WINDOW_SCALE |
                   RECOMP_RUNTIME_UI_STANDARD_INTEGER_SCALE | RECOMP_RUNTIME_UI_STANDARD_LINEAR_FILTER |
                   RECOMP_RUNTIME_UI_STANDARD_AUDIO | RECOMP_RUNTIME_UI_STANDARD_VOLUME |
                   RECOMP_RUNTIME_UI_STANDARD_RESUME;
    if (host->save_state && host->load_state)
        std.features |= RECOMP_RUNTIME_UI_STANDARD_SAVE_STATE | RECOMP_RUNTIME_UI_STANDARD_LOAD_STATE;
    if (x && x->view_modes && x->set_view_mode) {
        std.features |= RECOMP_RUNTIME_UI_STANDARD_VIEW_MODE;
        std.view_modes = x->view_modes;
    }
    std.extra_items = s_items;
    std.extra_item_count = s_item_count;
    s_ui = recomp_runtime_ui_create_standard(&std);
    return s_ui;
}

void cyc_ui_menu_destroy(void)
{
    recomp_runtime_ui_destroy(s_ui);
    s_ui = NULL;
}

void cyc_ui_menu_refresh(void)
{
    if (s_ui) refresh();
}

bool cyc_ui_menu_open(void) { return s_ui && recomp_runtime_ui_is_open(s_ui); }

void cyc_ui_toggle_menu(void)
{
    if (!s_ui) return;
    if (recomp_runtime_ui_is_open(s_ui)) recomp_runtime_ui_close(s_ui);
    else recomp_runtime_ui_open(s_ui);
}

void cyc_ui_nav(int input, bool repeat)
{
    if (s_ui) recomp_runtime_ui_handle_input(s_ui, (RecompRuntimeUiInput)input, 1, repeat ? 1 : 0);
}

void cyc_ui_set_toast(const char *title, const char *body)
{
    if (s_ui) recomp_runtime_ui_set_toast(s_ui, title, body);
}

RecompRuntimeUi *cyc_ui_runtime(void) { return s_ui; }
