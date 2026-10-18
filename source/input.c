#include <ogc/usb.h>
#include "internal.h"

static bool initialized;

s32 INPUT_Init(void)
{
    if (initialized)
        return 0;

    s32 ret = USB_Initialize();
    if (ret < 0)
        return ret;

    ret = input_keyboard_init();
    if (ret < 0)
        return ret;

    input_pads_init();

    /* A missing mouse driver is not fatal; keyboard still works. */
    input_mouse_init();

    initialized = true;
    return 0;
}

void INPUT_Shutdown(void)
{
    if (!initialized)
        return;

    input_pads_shutdown();
    input_keyboard_shutdown();
    input_mouse_shutdown();
    initialized = false;
}

void INPUT_Update(void)
{
    if (!initialized)
        return;

    input_pads_update();
    input_keyboard_update();
    input_mouse_update();
}
