/*
 * cyc_input.c - input actions and bindings for the windowed cycle host
 * (cyc_input.h). Needs SDL only for key and controller names and constants.
 */
#include "cyc_input.h"

#include <SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(CYC_INPUT_PAD_AXES == SDL_CONTROLLER_AXIS_MAX, "axis count");
_Static_assert(SDL_CONTROLLER_BUTTON_MAX <= 32, "button mask");
#endif

static const char *const BUTTON_KEYS[CYC_INPUT_BUTTONS] = { "a", "b", "select", "start", "up", "down", "left", "right" };
static const char *const BUTTON_LABELS[CYC_INPUT_BUTTONS] = { "A", "B", "Select", "Start", "Up", "Down", "Left", "Right" };
static const char *const SHORTCUT_KEYS[CYC_SC_COUNT] = { "disk", "menu", "fast_forward", "screenshot", "fullscreen",
                                                          "save_state", "load_state" };
static const char *const SHORTCUT_LABELS[CYC_SC_COUNT] = {
    "Disk (show / swap side)", "Menu", "Fast-forward (hold)", "Screenshot", "Fullscreen", "Save state", "Load state",
};

const char *cyc_button_key(int b) { return b >= 0 && b < CYC_INPUT_BUTTONS ? BUTTON_KEYS[b] : ""; }
const char *cyc_button_label(int b) { return b >= 0 && b < CYC_INPUT_BUTTONS ? BUTTON_LABELS[b] : ""; }
const char *cyc_shortcut_key(int s) { return s >= 0 && s < CYC_SC_COUNT ? SHORTCUT_KEYS[s] : ""; }
const char *cyc_shortcut_label(int s) { return s >= 0 && s < CYC_SC_COUNT ? SHORTCUT_LABELS[s] : ""; }

void cyc_bindings_default(CycBindings *b)
{
    memset(b, 0, sizeof(*b));
    /* nesrecomp's NES keyboard defaults (runner/src/keybinds.c, recomp-ui's
     * consoles/nes/nes_binds.c): Z = A, X = B, Backslash = Select, Return =
     * Start, arrows. Player 1 plays on the keyboard, player 2 on a controller. */
    static const int KEYS[CYC_INPUT_BUTTONS] = {
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    };
    /* The NES pad's B sits left of A: controller X and A (bottom and left face). */
    static const int PAD[CYC_INPUT_BUTTONS] = {
        CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_A), CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_X),
        CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_BACK), CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_START),
        CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_UP), CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_DOWN),
        CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_LEFT), CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
    };
    b->source[0] = 1;
    b->source[1] = 2;
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        if(p>1)b->source[p]=2;
        b->deadzone[p] = 30;
        for (int i = 0; i < CYC_INPUT_BUTTONS; ++i) {
            b->button[p][i].key = p == 0 ? KEYS[i] : 0;
            b->button[p][i].pad = PAD[i];
        }
    }
    /* Shortcuts avoid every default controller key and every key a dev build
     * keeps for itself (F1-F7). The shoulders are free on an NES layout. */
    b->shortcut[CYC_SC_DISK]         = (CycBinding){ SDL_SCANCODE_D, CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) };
    b->shortcut[CYC_SC_MENU]         = (CycBinding){ SDL_SCANCODE_ESCAPE, CYC_PAD_BUTTON(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) };
    b->shortcut[CYC_SC_FAST_FORWARD] = (CycBinding){ SDL_SCANCODE_TAB, CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1) };
    b->shortcut[CYC_SC_SCREENSHOT]   = (CycBinding){ SDL_SCANCODE_F12, 0 };
    b->shortcut[CYC_SC_FULLSCREEN]   = (CycBinding){ SDL_SCANCODE_F11, 0 };
    b->shortcut[CYC_SC_SAVE_STATE]   = (CycBinding){ SDL_SCANCODE_F8, 0 };
    b->shortcut[CYC_SC_LOAD_STATE]   = (CycBinding){ SDL_SCANCODE_F9, 0 };
}

/* ---- text forms ---- */

void cyc_key_text(int sc, char *out, size_t n)
{
    const char *name = sc > 0 && sc < SDL_NUM_SCANCODES ? SDL_GetScancodeName((SDL_Scancode)sc) : NULL;
    snprintf(out, n, "%s", name && *name ? name : "None");
}

static bool same_ci(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

bool cyc_key_parse(const char *text, int *sc)
{
    while (*text == ' ' || *text == '\t') ++text;
    if (!*text || same_ci(text, "none")) { *sc = 0; return true; }
    SDL_Scancode s = SDL_GetScancodeFromName(text);
    /* the runner's keybinds.ini aliases */
    static const struct { const char *name; SDL_Scancode sc; } ALIAS[] = {
        { "enter", SDL_SCANCODE_RETURN }, { "esc", SDL_SCANCODE_ESCAPE }, { "lshift", SDL_SCANCODE_LSHIFT },
        { "rshift", SDL_SCANCODE_RSHIFT }, { "lctrl", SDL_SCANCODE_LCTRL }, { "rctrl", SDL_SCANCODE_RCTRL },
    };
    for (size_t i = 0; s == SDL_SCANCODE_UNKNOWN && i < sizeof(ALIAS) / sizeof(ALIAS[0]); ++i)
        if (same_ci(text, ALIAS[i].name)) s = ALIAS[i].sc;
    if (s == SDL_SCANCODE_UNKNOWN) return false;
    *sc = (int)s;
    return true;
}

void cyc_pad_text(int v, char *out, size_t n)
{
    out[0] = 0;
    if (CYC_PAD_IS_BUTTON(v)) {
        const char *s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)(v - 1));
        snprintf(out, n, "%s", s ? s : "none");
    } else if (CYC_PAD_IS_AXIS(v)) {
        const char *s = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)((v - 100) / 2));
        snprintf(out, n, "%s%c", s ? s : "none", ((v - 100) & 1) ? '+' : '-');
    } else if (CYC_PAD_IS_COMBO(v)) {
        uint32_t mask = (uint32_t)(v - 1000);
        size_t at = 0;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && at < n; ++b) {
            if (!(mask & (1u << b))) continue;
            const char *s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
            int w = snprintf(out + at, n - at, "%s%s", at ? "+" : "", s ? s : "?");
            if (w > 0) at += (size_t)w;
        }
        if (!at) snprintf(out, n, "none");
    } else {
        snprintf(out, n, "none");
    }
}

bool cyc_pad_parse(const char *text, int *v)
{
    char buf[128];
    while (*text == ' ' || *text == '\t') ++text;
    snprintf(buf, sizeof(buf), "%s", text);
    /* the runner's format lists alternatives with commas; one binding here */
    char *comma = strchr(buf, ',');
    if (comma) *comma = 0;
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) buf[--len] = 0;
    for (char *c = buf; *c; ++c) *c = (char)tolower((unsigned char)*c);
    if (!len || !strcmp(buf, "none")) { *v = 0; return true; }
    /* an axis: its name and a direction */
    if (buf[len - 1] == '+' || buf[len - 1] == '-') {
        char dir = buf[len - 1];
        buf[len - 1] = 0;
        SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(buf);
        if (ax == SDL_CONTROLLER_AXIS_INVALID) return false;
        *v = CYC_PAD_AXIS((int)ax, dir == '+');
        return true;
    }
    /* a button, or buttons joined by + (held together) */
    uint32_t mask = 0;
    int count = 0, last = -1;
    for (char *tok = buf; tok && *tok;) {
        char *plus = strchr(tok, '+');
        if (plus) *plus = 0;
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(tok);
        if (b == SDL_CONTROLLER_BUTTON_INVALID) return false;
        if (!(mask & (1u << b))) count++;
        mask |= 1u << b;
        last = (int)b;
        tok = plus ? plus + 1 : NULL;
    }
    if (!count) return false;
    *v = count == 1 ? CYC_PAD_BUTTON(last) : CYC_PAD_COMBO(mask);
    return true;
}

void cyc_binding_hint(const CycBinding *b, char *out, size_t n)
{
    static const struct { int button; const char *hint; } SHORT[] = {
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "LB" }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "RB" },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3" },    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3" },
        { SDL_CONTROLLER_BUTTON_BACK, "BACK" },       { SDL_CONTROLLER_BUTTON_START, "START" },
        { SDL_CONTROLLER_BUTTON_GUIDE, "GUIDE" },
    };
    char key[32] = "", pad[48] = "";
    if (b->key) cyc_key_text(b->key, key, sizeof(key));
    if (CYC_PAD_IS_BUTTON(b->pad)) {
        for (size_t i = 0; i < sizeof(SHORT) / sizeof(SHORT[0]); ++i)
            if (b->pad - 1 == SHORT[i].button) snprintf(pad, sizeof(pad), "%s", SHORT[i].hint);
    }
    if (b->pad && !pad[0]) {
        if (b->pad == CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1)) snprintf(pad, sizeof(pad), "LT");
        else if (b->pad == CYC_PAD_AXIS(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)) snprintf(pad, sizeof(pad), "RT");
        else cyc_pad_text(b->pad, pad, sizeof(pad));
    }
    if (key[0] && pad[0]) snprintf(out, n, "%s / %s", key, pad);
    else snprintf(out, n, "%s", key[0] ? key : pad[0] ? pad : "DISK");
    for (char *c = out; *c; ++c) *c = (char)toupper((unsigned char)*c);
}

/* ---- evaluation ---- */

static bool key_down(const CycInputFrame *f, int sc)
{
    return sc > 0 && f->keys && sc < f->key_count && f->keys[sc];
}

static bool axis_on(const CycInputFrame *f, int pad, int code, bool positive, int threshold)
{
    if (code < 0 || code >= CYC_INPUT_PAD_AXES) return false;
    int v = f->pad_axes[pad][code];
    return positive ? v >= threshold : v <= -threshold;
}

/* Whether controller `pad` satisfies binding v; *mask gets the buttons it holds. */
static bool pad_down(const CycInputFrame *f, int pad, int v, int threshold, uint32_t *mask)
{
    uint32_t held = f->pad_buttons[pad];
    if (CYC_PAD_IS_BUTTON(v)) {
        uint32_t m = 1u << (v - 1);
        if (mask) *mask = m;
        return (held & m) != 0;
    }
    if (CYC_PAD_IS_COMBO(v)) {
        uint32_t m = (uint32_t)(v - 1000);
        if (mask) *mask = m;
        return m && (held & m) == m;
    }
    if (mask) *mask = 0;
    if (CYC_PAD_IS_AXIS(v)) return axis_on(f, pad, (v - 100) / 2, ((v - 100) & 1) != 0, threshold);
    return false;
}

void cyc_input_assign(const CycBindings *b, const CycInputFrame *f, int player_pad[CYC_INPUT_PLAYERS])
{
    bool taken[CYC_INPUT_MAX_PADS] = { false };
    int pads = f->pad_count < CYC_INPUT_MAX_PADS ? f->pad_count : CYC_INPUT_MAX_PADS;
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) player_pad[p] = -1;
    /* players naming a connected controller get it first */
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        if (b->source[p] != 2 || !b->device[p][0]) continue;
        for (int i = 0; i < pads; ++i)
            if (!taken[i] && f->pad_guid[i] && !strcmp(f->pad_guid[i], b->device[p])) {
                player_pad[p] = i;
                taken[i] = true;
                break;
            }
    }
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        if (b->source[p] != 2 || player_pad[p] >= 0) continue;
        for (int i = 0; i < pads; ++i)
            if (!taken[i]) { player_pad[p] = i; taken[i] = true; break; }
    }
}

void cyc_input_eval(const CycBindings *b, const CycInputFrame *f, CycInputState *out)
{
    memset(out, 0, sizeof(*out));
    int pads = f->pad_count < CYC_INPUT_MAX_PADS ? f->pad_count : CYC_INPUT_MAX_PADS;
    const int SHORTCUT_THRESHOLD = 16384;

    /* Shortcuts first: what they hold is theirs. */
    bool key_taken[SDL_NUM_SCANCODES] = { false };
    uint32_t pad_taken[CYC_INPUT_MAX_PADS] = { 0 };
    int axis_taken[CYC_INPUT_MAX_PADS] = { 0 };      /* the axis bindings a held shortcut uses */
    for (int s = 0; s < CYC_SC_COUNT; ++s) {
        const CycBinding *sb = &b->shortcut[s];
        if (key_down(f, sb->key)) {
            out->shortcut[s] = true;
            if (sb->key < SDL_NUM_SCANCODES) key_taken[sb->key] = true;
        }
        for (int i = 0; i < pads; ++i) {
            uint32_t m;
            if (sb->pad && pad_down(f, i, sb->pad, SHORTCUT_THRESHOLD, &m)) {
                out->shortcut[s] = true;
                pad_taken[i] |= m;
                if (CYC_PAD_IS_AXIS(sb->pad)) axis_taken[i] = sb->pad;
            }
        }
    }

    cyc_input_assign(b, f, out->player_pad);
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        uint8_t bits = 0;
        int dz = b->deadzone[p] < 5 ? 5 : b->deadzone[p] > 95 ? 95 : b->deadzone[p];
        int threshold = dz * 32767 / 100;
        int pad = out->player_pad[p];
        for (int i = 0; i < CYC_INPUT_BUTTONS; ++i) {
            const CycBinding *pb = &b->button[p][i];
            bool on = false;
            if (b->source[p] == 1) {
                on = key_down(f, pb->key) && !key_taken[pb->key];
            } else if (b->source[p] == 2 && pad >= 0 && pb->pad) {
                uint32_t m;
                on = pad_down(f, pad, pb->pad, threshold, &m) && !(m & pad_taken[pad]) &&
                     !(CYC_PAD_IS_AXIS(pb->pad) && axis_taken[pad] == pb->pad);
            }
            if (on) bits |= (uint8_t)(0x80 >> i);
        }
        if (b->source[p] == 2 && pad >= 0) {
            /* the left stick also works the D-pad */
            if (axis_on(f, pad, SDL_CONTROLLER_AXIS_LEFTY, false, threshold)) bits |= 0x08;
            if (axis_on(f, pad, SDL_CONTROLLER_AXIS_LEFTY, true, threshold)) bits |= 0x04;
            if (axis_on(f, pad, SDL_CONTROLLER_AXIS_LEFTX, false, threshold)) bits |= 0x02;
            if (axis_on(f, pad, SDL_CONTROLLER_AXIS_LEFTX, true, threshold)) bits |= 0x01;
        }
        /* The NES pad cannot press opposite directions at once. */
        if ((bits & 0x0C) == 0x0C) bits &= (uint8_t)~0x0C;
        if ((bits & 0x03) == 0x03) bits &= (uint8_t)~0x03;
        out->buttons[p] = bits;
    }
}
