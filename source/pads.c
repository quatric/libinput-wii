#include "internal.h"
#include <ogc/pad.h>
#include <wiiuse/wpad.h>
#ifdef HAVE_WIIDRC
#include <wiidrc/wiidrc.h>
#endif
#ifdef HAVE_WUPC
#include <wupc/wupc.h>
#endif
#ifdef HAVE_SICKSAXIS
#include <sicksaxis.h>
#endif

static u32 pads_held, pads_down;

#ifdef HAVE_SICKSAXIS
static struct ss_device ds3;
static bool ds3_initialized;
static u32 ds3_previous;
#endif

static u32 map_classic(u32 buttons)
{
    u32 out = 0;
    if (buttons & WPAD_CLASSIC_BUTTON_ZR)    out |= WPAD_BUTTON_PLUS;
    if (buttons & WPAD_CLASSIC_BUTTON_ZL)    out |= WPAD_BUTTON_MINUS;
    if (buttons & WPAD_CLASSIC_BUTTON_PLUS)  out |= WPAD_BUTTON_PLUS;
    if (buttons & WPAD_CLASSIC_BUTTON_MINUS) out |= WPAD_BUTTON_MINUS;
    if (buttons & WPAD_CLASSIC_BUTTON_A)     out |= WPAD_BUTTON_A;
    if (buttons & WPAD_CLASSIC_BUTTON_B)     out |= WPAD_BUTTON_B;
    if (buttons & WPAD_CLASSIC_BUTTON_X)     out |= WPAD_BUTTON_2;
    if (buttons & WPAD_CLASSIC_BUTTON_Y)     out |= WPAD_BUTTON_1;
    if (buttons & WPAD_CLASSIC_BUTTON_HOME)  out |= WPAD_BUTTON_HOME;
    if (buttons & WPAD_CLASSIC_BUTTON_UP)    out |= WPAD_BUTTON_UP;
    if (buttons & WPAD_CLASSIC_BUTTON_DOWN)  out |= WPAD_BUTTON_DOWN;
    if (buttons & WPAD_CLASSIC_BUTTON_LEFT)  out |= WPAD_BUTTON_LEFT;
    if (buttons & WPAD_CLASSIC_BUTTON_RIGHT) out |= WPAD_BUTTON_RIGHT;
    return out;
}

static u32 map_gc(u32 buttons)
{
    u32 out = 0;
    if (buttons & PAD_TRIGGER_R)  out |= WPAD_BUTTON_PLUS;
    if (buttons & PAD_TRIGGER_L)  out |= WPAD_BUTTON_MINUS;
    if (buttons & PAD_BUTTON_A)   out |= WPAD_BUTTON_A;
    if (buttons & PAD_BUTTON_B)   out |= WPAD_BUTTON_B;
    if (buttons & PAD_BUTTON_X)   out |= WPAD_BUTTON_2;
    if (buttons & PAD_BUTTON_Y)   out |= WPAD_BUTTON_1;
    if (buttons & PAD_BUTTON_MENU) out |= WPAD_BUTTON_HOME;
    if (buttons & PAD_BUTTON_UP)  out |= WPAD_BUTTON_UP;
    if (buttons & PAD_BUTTON_DOWN) out |= WPAD_BUTTON_DOWN;
    if (buttons & PAD_BUTTON_LEFT) out |= WPAD_BUTTON_LEFT;
    if (buttons & PAD_BUTTON_RIGHT) out |= WPAD_BUTTON_RIGHT;
    return out;
}

#ifdef HAVE_WIIDRC
static u32 map_drc(u32 buttons)
{
    u32 out = 0;
    if (buttons & (WIIDRC_BUTTON_R | WIIDRC_BUTTON_ZR)) out |= WPAD_BUTTON_PLUS;
    if (buttons & (WIIDRC_BUTTON_L | WIIDRC_BUTTON_ZL)) out |= WPAD_BUTTON_MINUS;
    if (buttons & WIIDRC_BUTTON_PLUS)  out |= WPAD_BUTTON_PLUS;
    if (buttons & WIIDRC_BUTTON_MINUS) out |= WPAD_BUTTON_MINUS;
    if (buttons & WIIDRC_BUTTON_A)     out |= WPAD_BUTTON_A;
    if (buttons & WIIDRC_BUTTON_B)     out |= WPAD_BUTTON_B;
    if (buttons & WIIDRC_BUTTON_X)     out |= WPAD_BUTTON_2;
    if (buttons & WIIDRC_BUTTON_Y)     out |= WPAD_BUTTON_1;
    if (buttons & WIIDRC_BUTTON_HOME)  out |= WPAD_BUTTON_HOME;
    if (buttons & WIIDRC_BUTTON_UP)    out |= WPAD_BUTTON_UP;
    if (buttons & WIIDRC_BUTTON_DOWN)  out |= WPAD_BUTTON_DOWN;
    if (buttons & WIIDRC_BUTTON_LEFT)  out |= WPAD_BUTTON_LEFT;
    if (buttons & WIIDRC_BUTTON_RIGHT) out |= WPAD_BUTTON_RIGHT;
    return out;
}
#endif

#ifdef HAVE_SICKSAXIS
static void poll_ds3(u32 *held, u32 *down)
{
    if (!ds3_initialized)
        return;
    if (!ss_is_connected(&ds3)) {
        ds3_previous = 0;
        if (ss_open(&ds3) > 0)
            ss_start_reading(&ds3);
        else
            return;
    }

    const struct SS_BUTTONS *b = &ds3.pad.buttons;
    u32 now = 0;
    if (b->start || b->R1 || b->R2)  now |= WPAD_BUTTON_PLUS;
    if (b->select || b->L1 || b->L2) now |= WPAD_BUTTON_MINUS;
    if (b->cross)    now |= WPAD_BUTTON_A;
    if (b->circle)   now |= WPAD_BUTTON_B;
    if (b->triangle) now |= WPAD_BUTTON_2;
    if (b->square)   now |= WPAD_BUTTON_1;
    if (b->PS)       now |= WPAD_BUTTON_HOME;
    if (b->up)       now |= WPAD_BUTTON_UP;
    if (b->down)     now |= WPAD_BUTTON_DOWN;
    if (b->left)     now |= WPAD_BUTTON_LEFT;
    if (b->right)    now |= WPAD_BUTTON_RIGHT;

    *held |= now;
    *down |= now & ~ds3_previous;
    ds3_previous = now;
}
#endif

s32 input_pads_init(void)
{
    PAD_Init();
#ifdef HAVE_WUPC
    WUPC_Init();
#endif
    WPAD_Init();
#ifdef HAVE_WIIDRC
    WiiDRC_Init();
#endif
#ifdef HAVE_SICKSAXIS
    if (ss_init() >= 0 && ss_initialize(&ds3) >= 0) {
        ds3_initialized = true;
        if (ss_open(&ds3) > 0)
            ss_start_reading(&ds3);
    }
#endif
    return 0;
}

void input_pads_shutdown(void)
{
#ifdef HAVE_SICKSAXIS
    if (ds3_initialized) {
        if (ss_is_connected(&ds3))
            ss_stop_reading(&ds3);
        ss_close(&ds3);
        ds3_initialized = false;
    }
#endif
#ifdef HAVE_WUPC
    WUPC_Shutdown();
#endif
}

void input_pads_update(void)
{
    u32 held = 0, down = 0;

#ifdef HAVE_WUPC
    WUPC_UpdateButtonStats();
    u32 b = 0;
    for (int chan = 0; chan < 4; chan++)
        b |= WUPC_ButtonsHeld(chan);
    held |= map_classic(b);
    b = 0;
    for (int chan = 0; chan < 4; chan++)
        b |= WUPC_ButtonsDown(chan);
    down |= map_classic(b);
#endif

    if (WPAD_ScanPads() > WPAD_ERR_NONE) {
        u32 h = 0, d = 0;
        for (int chan = 0; chan < 4; chan++) {
            h |= WPAD_ButtonsHeld(chan);
            d |= WPAD_ButtonsDown(chan);
        }
        held |= h | map_classic(h);
        down |= d | map_classic(d);
    }

#ifdef HAVE_WIIDRC
    if (WiiDRC_Inited() && WiiDRC_Connected() && WiiDRC_ScanPads()) {
        held |= map_drc(WiiDRC_ButtonsHeld());
        down |= map_drc(WiiDRC_ButtonsDown());
    }
#endif

#ifdef HAVE_SICKSAXIS
    poll_ds3(&held, &down);
#endif

    if (PAD_ScanPads() > PAD_ERR_NONE) {
        u32 h = 0, d = 0;
        for (int chan = 0; chan < 4; chan++) {
            h |= PAD_ButtonsHeld(chan);
            d |= PAD_ButtonsDown(chan);
        }
        held |= map_gc(h);
        down |= map_gc(d);
    }

    pads_held = held;
    pads_down = down;
}

u32 INPUT_PadsHeld(void) { return pads_held; }
u32 INPUT_PadsDown(void) { return pads_down; }

u32 INPUT_ButtonsHeld(void) { return pads_held | INPUT_KeyboardPadHeld(); }
u32 INPUT_ButtonsDown(void) { return pads_down | INPUT_KeyboardPadDown(); }
