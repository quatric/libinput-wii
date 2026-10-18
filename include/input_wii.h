#ifndef INPUT_WII_H
#define INPUT_WII_H

#include <gccore.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mouse button bits, as reported by INPUT_MouseButtons*() */
#define INPUT_MOUSE_LEFT   (1 << 0)
#define INPUT_MOUSE_RIGHT  (1 << 1)
#define INPUT_MOUSE_MIDDLE (1 << 2)

/* Keyboard LEDs */
typedef enum {
    INPUT_LED_NUM = 0,
    INPUT_LED_CAPS,
    INPUT_LED_SCROLL
} input_led;

/* Initialize all input: GameCube pads, Wii Remotes (incl. Classic Controller),
 * and optional Wii U Pro Controller / Wii U GamePad / DualShock 3 support
 * (see HAVE_WUPC, HAVE_WIIDRC, HAVE_SICKSAXIS), plus USB keyboard and mouse.
 * Calls USB_Initialize(). Calls USB_Initialize() and
 * starts a background thread that keeps the keyboard connected and scanned.
 * Returns 0 on success, negative on failure. Safe to call twice. */
s32 INPUT_Init(void);

/* Stop the background thread and release the USB devices. */
void INPUT_Shutdown(void);

/* Snapshot input state. Call once per frame: INPUT_Key/Mouse*Pressed()
 * report events that happened since the previous call. */
void INPUT_Update(void);

/* ---- Unified controller buttons (WPAD_BUTTON_* masks) ----
 * Every supported controller is normalised to Wii Remote buttons. The
 * *Buttons* variants also include the keyboard mapping. */
u32 INPUT_PadsHeld(void);
u32 INPUT_PadsDown(void);
u32 INPUT_ButtonsHeld(void);
u32 INPUT_ButtonsDown(void);

/* ---- Keyboard (USB HID usage codes, e.g. 0x28 = Enter) ---- */
bool INPUT_KeyboardConnected(void);
bool INPUT_KeyHeld(u8 keycode);
bool INPUT_KeyPressed(u8 keycode);
bool INPUT_KeyReleased(u8 keycode);
s32  INPUT_KeyboardSetLed(input_led led, bool on);

/* Map held / newly pressed keys to a WPAD_BUTTON_* mask so a keyboard can
 * stand in for a Wii Remote: arrows = D-pad, Enter = A, Backspace = B,
 * X = 1, Y = 2, Delete = -, Esc/Home = HOME. */
u32 INPUT_KeyboardPadHeld(void);
u32 INPUT_KeyboardPadDown(void);

/* ---- Mouse ---- */
bool INPUT_MouseConnected(void);

/* Bounds the cursor is clamped to (default 640x480). */
void INPUT_MouseSetBounds(int width, int height);
void INPUT_MousePosition(int *x, int *y);
void INPUT_MouseSetPosition(int x, int y);

/* Movement and wheel delta accumulated since the last INPUT_Update(). */
void INPUT_MouseDelta(int *dx, int *dy, int *wheel);

u8   INPUT_MouseButtonsHeld(void);
u8   INPUT_MouseButtonsPressed(void);
u8   INPUT_MouseButtonsReleased(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_WII_H */
