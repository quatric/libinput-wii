#include "libinput_internal.h"
#include <ogc/usb.h>

int inpw_screen_w = 640, inpw_screen_h = 480;

static bool initialized;
static const char *source_name = "";

void inpw_set_source(const char *name) { source_name = name; }
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

    inpw_pads_init(flags);

    if (!(flags & INPUT_INIT_NO_KEYBOARD)) {
        ret = inpw_keyboard_init();
        if (ret < 0)
            return ret;
    }

    /* A missing mouse driver is not fatal. */
    if (!(flags & INPUT_INIT_NO_MOUSE))
        inpw_mouse_init();

    initialized = true;
    return 0;
}

void INPUT_Shutdown(void)
{
    if (!initialized)
        return;

    inpw_pads_shutdown();
    inpw_keyboard_shutdown();
    inpw_mouse_shutdown();
    initialized = false;
}

void INPUT_SetScreenSize(int width, int height)
{
    if (width < 1 || height < 1)
        return;
    inpw_screen_w = width;
    inpw_screen_h = height;
    inpw_pads_set_screen(width, height);
    inpw_mouse_set_bounds(width, height);
}

void INPUT_Update(void)
{
    if (!initialized)
        return;

    /* Keyboard and mouse first so their button maps are current. */
    inpw_keyboard_update();
    inpw_mouse_update();
    inpw_pads_update();
    inpw_cursor_update();
}
