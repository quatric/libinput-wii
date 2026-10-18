#include "libinput_internal.h"
#include <ogc/usbmouse.h>

#define RETRY_FRAMES 180

static bool mouse_wanted;
static int bound_w = 640, bound_h = 480;
static int pos_x = 320, pos_y = 240;
static int delta_x, delta_y, delta_wheel;
static u8 buttons_held, buttons_pressed, buttons_released;
static int retry;
static int speed = 1;

s32 inpw_mouse_init(void)
{
    mouse_wanted = true;
    bound_w = inpw_screen_w;
    bound_h = inpw_screen_h;
    pos_x = bound_w / 2;
    pos_y = bound_h / 2;
    /* Not fatal if nothing is plugged in yet; update() keeps retrying. */
    MOUSE_Init();
    return 0;
}

void inpw_mouse_shutdown(void)
{
    if (!mouse_wanted)
        return;

    MOUSE_Deinit();
    mouse_wanted = false;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void inpw_mouse_update(void)
{
    mouse_event ev;
    u8 prev = buttons_held;

    delta_x = delta_y = delta_wheel = 0;
    buttons_pressed = buttons_released = 0;

    if (!mouse_wanted)
        return;

    if (!MOUSE_IsConnected()) {
        buttons_held = 0;
        buttons_released = prev;
        if (++retry >= RETRY_FRAMES) {
            retry = 0;
            MOUSE_Init();
        }
        return;
    }

    /* Drain everything queued since the last frame. */
    int guard = 0;
    while (guard++ < 64 && MOUSE_GetEvent(&ev) > 0) {
        delta_x += ev.rx;
        delta_y += ev.ry;
        delta_wheel += ev.rz;
        buttons_held = ev.button & 7;
    }

    pos_x = clamp(pos_x + delta_x * speed, 0, bound_w - 1);
    pos_y = clamp(pos_y + delta_y * speed, 0, bound_h - 1);

    buttons_pressed = buttons_held & ~prev;
    buttons_released = prev & ~buttons_held;

    if (delta_x || delta_y || delta_wheel || buttons_pressed)
        inpw_set_source("Mouse");
}

bool INPUT_MouseConnected(void)
{
    return mouse_wanted && MOUSE_IsConnected();
}

void inpw_mouse_set_bounds(int width, int height)
{
    bound_w = width;
    bound_h = height;
    pos_x = clamp(pos_x, 0, bound_w - 1);
    pos_y = clamp(pos_y, 0, bound_h - 1);
}

void INPUT_MousePosition(int *x, int *y)
{
    if (x) *x = pos_x;
    if (y) *y = pos_y;
}

void INPUT_MouseSetSpeed(int multiplier)
{
    speed = multiplier > 0 ? multiplier : 1;
}

void INPUT_MouseSetPosition(int x, int y)
{
    pos_x = clamp(x, 0, bound_w - 1);
    pos_y = clamp(y, 0, bound_h - 1);
}

void INPUT_MouseDelta(int *dx, int *dy, int *wheel)
{
    if (dx) *dx = delta_x;
    if (dy) *dy = delta_y;
    if (wheel) *wheel = delta_wheel;
}

u8 INPUT_MouseButtonsHeld(void)     { return buttons_held; }
u8 INPUT_MouseButtonsPressed(void)  { return buttons_pressed; }
u8 INPUT_MouseButtonsReleased(void) { return buttons_released; }
