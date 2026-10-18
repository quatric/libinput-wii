#include <string.h>
#include <unistd.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/usbkeyboard.h>
#include "internal.h"

#define KEY_BYTES (256 / 8)

/* Written by the scan thread (via the event callback), under kbd_lock. */
static u8 live[KEY_BYTES];
static u8 pressed_acc[KEY_BYTES];
static u8 released_acc[KEY_BYTES];

/* Snapshot published by input_keyboard_update(). */
static u8 held[KEY_BYTES];
static u8 pressed[KEY_BYTES];
static u8 released[KEY_BYTES];

static mutex_t kbd_lock = LWP_MUTEX_NULL;
static lwp_t kbd_thread_hndl = LWP_THREAD_NULL;
static volatile bool kbd_thread_should_run;
static bool kbd_initialized;

#define BIT_GET(a, k) ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define BIT_SET(a, k) ((a)[(k) >> 3] |= (1 << ((k) & 7)))
#define BIT_CLR(a, k) ((a)[(k) >> 3] &= ~(1 << ((k) & 7)))

static void kb_event(USBKeyboard_event event)
{
    u8 k = event.keyCode;

    if (event.type == USBKEYBOARD_PRESSED) {
        if (!BIT_GET(live, k))
            BIT_SET(pressed_acc, k);
        BIT_SET(live, k);
    } else if (event.type == USBKEYBOARD_RELEASED) {
        if (BIT_GET(live, k))
            BIT_SET(released_acc, k);
        BIT_CLR(live, k);
    } else if (event.type == USBKEYBOARD_DISCONNECTED) {
        for (int i = 0; i < 256; i++) {
            if (BIT_GET(live, i))
                BIT_SET(released_acc, i);
        }
        memset(live, 0, sizeof(live));
    }
}

static void *kbd_thread(void *arg)
{
    (void)arg;

    while (kbd_thread_should_run) {
        if (!USBKeyboard_IsConnected() && USBKeyboard_Open(kb_event)) {
            for (int i = 0; i < 3; i++) {
                USBKeyboard_SetLed(i, 1);
                usleep(250000);
            }
            for (int i = 0; i < 3; i++)
                USBKeyboard_SetLed(i, 0);
        }

        /* The callback runs inside Scan(), so lock around it. */
        LWP_MutexLock(kbd_lock);
        USBKeyboard_Scan();
        LWP_MutexUnlock(kbd_lock);
        usleep(400);
    }

    return NULL;
}

s32 input_keyboard_init(void)
{
    if (kbd_initialized)
        return 0;

    s32 ret = USBKeyboard_Initialize();
    if (ret < 0)
        return ret;

    if (LWP_MutexInit(&kbd_lock, false) < 0) {
        USBKeyboard_Deinitialize();
        return -1;
    }

    memset(live, 0, sizeof(live));
    memset(pressed_acc, 0, sizeof(pressed_acc));
    memset(released_acc, 0, sizeof(released_acc));
    memset(held, 0, sizeof(held));
    memset(pressed, 0, sizeof(pressed));
    memset(released, 0, sizeof(released));

    kbd_thread_should_run = true;
    ret = LWP_CreateThread(&kbd_thread_hndl, kbd_thread, NULL, NULL, 0x4000, 0x7F);
    if (ret < 0) {
        kbd_thread_should_run = false;
        LWP_MutexDestroy(kbd_lock);
        kbd_lock = LWP_MUTEX_NULL;
        USBKeyboard_Deinitialize();
        return ret;
    }

    kbd_initialized = true;
    return 0;
}

void input_keyboard_shutdown(void)
{
    if (!kbd_initialized)
        return;

    kbd_thread_should_run = false;
    if (kbd_thread_hndl != LWP_THREAD_NULL)
        LWP_JoinThread(kbd_thread_hndl, NULL);
    kbd_thread_hndl = LWP_THREAD_NULL;

    USBKeyboard_Close();
    USBKeyboard_Deinitialize();
    LWP_MutexDestroy(kbd_lock);
    kbd_lock = LWP_MUTEX_NULL;
    kbd_initialized = false;
}

void input_keyboard_update(void)
{
    if (!kbd_initialized)
        return;

    LWP_MutexLock(kbd_lock);
    memcpy(held, live, sizeof(held));
    memcpy(pressed, pressed_acc, sizeof(pressed));
    memcpy(released, released_acc, sizeof(released));
    memset(pressed_acc, 0, sizeof(pressed_acc));
    memset(released_acc, 0, sizeof(released_acc));
    LWP_MutexUnlock(kbd_lock);
}

bool INPUT_KeyboardConnected(void)
{
    return kbd_initialized && USBKeyboard_IsConnected();
}

bool INPUT_KeyHeld(u8 keycode)     { return BIT_GET(held, keycode) != 0; }
bool INPUT_KeyPressed(u8 keycode)  { return BIT_GET(pressed, keycode) != 0; }
bool INPUT_KeyReleased(u8 keycode) { return BIT_GET(released, keycode) != 0; }

s32 INPUT_KeyboardSetLed(input_led led, bool on)
{
    if (!kbd_initialized)
        return -1;
    return USBKeyboard_SetLed((USBKeyboard_led)led, on);
}

/* Key layout from Priiloader's input handling. */
static u32 pad_map(const u8 *keys)
{
    static const struct { u8 code; u32 button; } map[] = {
        { 0x52, WPAD_BUTTON_UP },
        { 0x51, WPAD_BUTTON_DOWN },
        { 0x50, WPAD_BUTTON_LEFT },
        { 0x4F, WPAD_BUTTON_RIGHT },
        { 0x28, WPAD_BUTTON_A },     /* Enter */
        { 0x58, WPAD_BUTTON_A },     /* Enter (numpad) */
        { 0x2A, WPAD_BUTTON_B },     /* Backspace */
        { 0x1B, WPAD_BUTTON_1 },     /* X */
        { 0x1C, WPAD_BUTTON_2 },     /* Y */
        { 0x4C, WPAD_BUTTON_MINUS }, /* Delete */
        { 0x29, WPAD_BUTTON_HOME },  /* Esc */
        { 0x4A, WPAD_BUTTON_HOME },  /* Home */
    };
    u32 out = 0;

    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (BIT_GET(keys, map[i].code))
            out |= map[i].button;
    }
    return out;
}

u32 INPUT_KeyboardPadHeld(void) { return pad_map(held); }
u32 INPUT_KeyboardPadDown(void) { return pad_map(pressed); }
