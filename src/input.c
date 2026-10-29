// Keyboard and gamepad -> N64 controller 1 (ultra_pads[0]).
#include "input.h"

#include <SDL3/SDL.h>

#include "ultra.h"

// N64 button bits (os.h)
enum {
    N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000,
    N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100,
    N64_L = 0x0020, N64_R = 0x0010,
    N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001,
};
#define STICK_MAX 80  // full deflection of a real N64 stick is about +-80

static SDL_Gamepad *g_pad;

void input_init(void) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    ultra_pads[0].connected = 1;  // the keyboard always counts as controller 1
}

static void open_first_gamepad(void) {
    if (g_pad) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids && count > 0) g_pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
}

static int8_t axis_to_stick(Sint16 v) {
    int s = (int)v * STICK_MAX / 32767;
    return (int8_t)(s > STICK_MAX ? STICK_MAX : s < -STICK_MAX ? -STICK_MAX : s);
}

void input_update(void) {
    open_first_gamepad();
    const bool *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    int x = 0, y = 0;
    // Keyboard: arrows = stick, X = A, C = B, Z = Z, Enter = Start, A/S = L/R, IJKL = C, WASD-free.
    if (k[SDL_SCANCODE_X]) b |= N64_A;
    if (k[SDL_SCANCODE_C]) b |= N64_B;
    if (k[SDL_SCANCODE_Z]) b |= N64_Z;
    if (k[SDL_SCANCODE_RETURN]) b |= N64_START;
    if (k[SDL_SCANCODE_A]) b |= N64_L;
    if (k[SDL_SCANCODE_S]) b |= N64_R;
    if (k[SDL_SCANCODE_I]) b |= N64_CU;
    if (k[SDL_SCANCODE_K]) b |= N64_CD;
    if (k[SDL_SCANCODE_J]) b |= N64_CL;
    if (k[SDL_SCANCODE_L]) b |= N64_CR;
    if (k[SDL_SCANCODE_T]) b |= N64_DU;
    if (k[SDL_SCANCODE_G]) b |= N64_DD;
    if (k[SDL_SCANCODE_F]) b |= N64_DL;
    if (k[SDL_SCANCODE_H]) b |= N64_DR;
    if (k[SDL_SCANCODE_LEFT]) x -= STICK_MAX;
    if (k[SDL_SCANCODE_RIGHT]) x += STICK_MAX;
    if (k[SDL_SCANCODE_UP]) y += STICK_MAX;
    if (k[SDL_SCANCODE_DOWN]) y -= STICK_MAX;

    if (g_pad) {
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_SOUTH)) b |= N64_A;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_WEST)) b |= N64_B;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_START)) b |= N64_START;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) b |= N64_L;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= N64_R;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= N64_DU;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= N64_DD;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= N64_DL;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= N64_DR;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= N64_Z;
        Sint16 rx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX);
        Sint16 ry = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY);
        if (rx < -16000) b |= N64_CL;
        if (rx > 16000) b |= N64_CR;
        if (ry < -16000) b |= N64_CU;
        if (ry > 16000) b |= N64_CD;
        int gx = axis_to_stick(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX));
        int gy = -axis_to_stick(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY));
        if (gx) x = gx;
        if (gy) y = gy;
    }
    ultra_pads[0].buttons = b;
    ultra_pads[0].stick_x = (int8_t)x;
    ultra_pads[0].stick_y = (int8_t)y;
}
