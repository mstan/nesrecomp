/*
 * controller.c — SDL2 gamepad support for the NES runner. See controller.h.
 *
 * Button mapping is deliberately forgiving so any pad "just works" without
 * configuration: the two right-hand face buttons both act as NES A and the two
 * left-hand face buttons both act as NES B. The d-pad and the left analog stick
 * both drive the NES d-pad.
 */
#include "controller.h"
#include "keybinds.h"
#include <stdio.h>

#define MAX_PADS 2  /* NES has two controller ports */

static SDL_GameController *s_pads[MAX_PADS];     /* index = player slot (0,1) */
static SDL_JoystickID      s_pad_ids[MAX_PADS];  /* instance id per slot */
static int                 s_count = 0;

static int slot_for_instance(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++)
        if (s_pads[i] && s_pad_ids[i] == id) return i;
    return -1;
}

static int first_free_slot(void) {
    for (int i = 0; i < MAX_PADS; i++)
        if (!s_pads[i]) return i;
    return -1;
}

static void open_device(int device_index) {
    if (!SDL_IsGameController(device_index)) return;  /* not a mapped gamepad */
    /* Dedupe: SDL reports already-connected pads both via init enumeration and
     * via a CONTROLLERDEVICEADDED event, so the same device can arrive twice. */
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    if (id >= 0 && slot_for_instance(id) >= 0) return;
    int slot = first_free_slot();
    if (slot < 0) return;                              /* both ports in use */
    SDL_GameController *gc = SDL_GameControllerOpen(device_index);
    if (!gc) {
        fprintf(stderr, "[Controller] open failed: %s\n", SDL_GetError());
        return;
    }
    s_pads[slot]    = gc;
    s_pad_ids[slot] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    s_count++;
    printf("[Controller] P%d connected: %s\n", slot + 1, SDL_GameControllerName(gc));
    fflush(stdout);
}

static void close_instance(SDL_JoystickID id) {
    int slot = slot_for_instance(id);
    if (slot < 0) return;
    printf("[Controller] P%d disconnected: %s\n",
           slot + 1, SDL_GameControllerName(s_pads[slot]));
    fflush(stdout);
    SDL_GameControllerClose(s_pads[slot]);
    s_pads[slot]    = NULL;
    s_pad_ids[slot] = 0;
    s_count--;
}

void controller_init(void) {
    if (!(SDL_WasInit(0) & SDL_INIT_GAMECONTROLLER)) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
            fprintf(stderr, "[Controller] init failed: %s\n", SDL_GetError());
            return;
        }
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) open_device(i);
    if (s_count == 0)
        printf("[Controller] No gamepad detected; keyboard active. Plug one in anytime.\n");
}

void controller_handle_event(const SDL_Event *ev) {
    if (ev->type == SDL_CONTROLLERDEVICEADDED)
        open_device(ev->cdevice.which);
    else if (ev->type == SDL_CONTROLLERDEVICEREMOVED)
        close_instance(ev->cdevice.which);
}

uint8_t controller_read_player(int player) {
    int slot = player - 1;
    if (slot < 0 || slot >= MAX_PADS || !s_pads[slot]) return 0;
    SDL_GameController *gc = s_pads[slot];
    const GamepadBinds *gb = keybinds_get_pad(player);

    /* NES button bit per btn_mask index (matches keybinds_read_player order). */
    static const uint8_t nes_bit[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t mask = gb->btn_mask[i];
        for (int bit = 0; mask && bit < SDL_CONTROLLER_BUTTON_MAX && bit < 32; bit++) {
            if ((mask & (1u << bit)) &&
                SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)bit)) {
                b |= nes_bit[i];
                break;
            }
        }
    }

    /* Left analog stick also drives the d-pad (configurable). */
    if (gb->analog_dpad) {
        int dz = gb->deadzone;
        int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -dz) b |= 0x02;  /* left  */
        if (ax >  dz) b |= 0x01;  /* right */
        if (ay < -dz) b |= 0x08;  /* up    */
        if (ay >  dz) b |= 0x04;  /* down  */
    }

    return b;
}

int controller_count(void) { return s_count; }

void controller_shutdown(void) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pads[i]) { SDL_GameControllerClose(s_pads[i]); s_pads[i] = NULL; }
    }
    s_count = 0;
}
