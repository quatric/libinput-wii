#include "libinput_internal.h"
#include <ogc/usb.h>

int input_screen_w = 640, input_screen_h = 480;

static bool initialized;
static const char *source_name = "";

void input_set_source(const char *name) { source_name = name; }
const char *INPUT_Source(void)          { return source_name; }

s32 INPUT_Init(void)
{
    return INPUT_InitEx(0);
}

s32 INPUT_InitEx(u32 flags)
{
    if (initialized)
        return 0;

    s32 ret = USB_Initialize();
    if (ret < 0)
        return ret;

    input_pads_init(flags);

    if (!(flags & INPUT_INIT_NO_KEYBOARD)) {
        ret = input_keyboard_init();
        if (ret < 0)
            return ret;
    }

    /* A missing mouse driver is not fatal. */
    if (!(flags & INPUT_INIT_NO_MOUSE))
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

void INPUT_SetScreenSize(int width, int height)
{
    if (width < 1 || height < 1)
        return;
    input_screen_w = width;
    input_screen_h = height;
    input_pads_set_screen(width, height);
    input_mouse_set_bounds(width, height);
}

void INPUT_Update(void)
{
    if (!initialized)
        return;

    /* Keyboard and mouse first so their button maps are current. */
    input_keyboard_update();
    input_mouse_update();
    input_pads_update();
}
