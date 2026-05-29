/*
 * controller.c — SDL2 gamepad support for the NES runner. See controller.h.
 *
 * Button mapping is deliberately forgiving so any pad "just works" without
 * configuration: the two right-hand face buttons both act as NES A and the two
 * left-hand face buttons both act as NES B. The d-pad and the left analog stick
 * both drive the NES d-pad.
 */
#include "controller.h"
#include <stdio.h>

#define MAX_PADS       2       /* NES has two controller ports */
#define STICK_DEADZONE 16000   /* ~half of the 32767 axis range */

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
    uint8_t b = 0;

#define BTN(x) SDL_GameControllerGetButton(gc, (x))
    /* Right two face buttons -> A, left two -> B (layout-agnostic, forgiving). */
    if (BTN(SDL_CONTROLLER_BUTTON_A) || BTN(SDL_CONTROLLER_BUTTON_B)) b |= 0x80;
    if (BTN(SDL_CONTROLLER_BUTTON_X) || BTN(SDL_CONTROLLER_BUTTON_Y)) b |= 0x40;
    if (BTN(SDL_CONTROLLER_BUTTON_BACK))       b |= 0x20;  /* Select */
    if (BTN(SDL_CONTROLLER_BUTTON_START))      b |= 0x10;  /* Start  */
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_UP))    b |= 0x08;
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  b |= 0x04;
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  b |= 0x02;
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= 0x01;
#undef BTN

    /* Left analog stick also drives the d-pad. */
    int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    if (ax < -STICK_DEADZONE) b |= 0x02;  /* left  */
    if (ax >  STICK_DEADZONE) b |= 0x01;  /* right */
    if (ay < -STICK_DEADZONE) b |= 0x08;  /* up    */
    if (ay >  STICK_DEADZONE) b |= 0x04;  /* down  */

    return b;
}

int controller_count(void) { return s_count; }

void controller_shutdown(void) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (s_pads[i]) { SDL_GameControllerClose(s_pads[i]); s_pads[i] = NULL; }
    }
    s_count = 0;
}
