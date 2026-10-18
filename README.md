# libinput-wii

USB keyboard and mouse input for the Nintendo Wii, built on libogc.

- One call reads every controller and normalises it to `WPAD_BUTTON_*`: Wii Remote,
  Classic Controller, GameCube pad, plus optional Wii U Pro Controller (libwupc),
  Wii U GamePad (libwiidrc) and DualShock 3 (libsicksaxis)
- Background thread keeps the keyboard connected and scanned (hot-plug safe)
- Per-frame polling: held / pressed / released for every USB HID key
- Optional keyboard-to-Wii-Remote mapping (`INPUT_KeyboardPadHeld/Down`)
- Mouse position (clamped to configurable bounds), deltas, wheel and buttons

The keyboard handling was originally part of GlowWii and has been split out
into a reusable library; mouse support is new.

## Build

Requires devkitPro with devkitPPC and libogc.

```sh
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
make            # produces lib/libinput-wii.a
make install    # copies into $DEVKITPRO/portlibs/ppc
```

### Optional controllers

```sh
make controllers      # fetches and builds libwiidrc, libwupc, libsicksaxis into ./portlibs/ppc
make WITH_WIIDRC=1 WITH_WUPC=1 WITH_SICKSAXIS=1
```

Enable the same flags when linking your app, adding `-lwiidrc`, `-lwupc` (with
`-Wl,-wrap,wiiuse_accept`) and `-lsicksaxis` ahead of `-lwiiuse`.

Link with `-linput-wii -lwiikeyboard -logc` (and the usual `-lwiiuse -lbte -lm`).

## Usage

```c
#include <input_wii.h>

INPUT_Init();
INPUT_MouseSetBounds(640, 480);

while (running) {
    VIDEO_WaitVSync();
    INPUT_Update();                       /* once per frame */

    u32 pressed = INPUT_ButtonsDown();   /* any controller or keyboard */
    if (pressed & WPAD_BUTTON_A) { }
    if (INPUT_KeyPressed(0x28)) { /* raw key: Enter */ }

    int x, y;
    INPUT_MousePosition(&x, &y);
    if (INPUT_MouseButtonsPressed() & INPUT_MOUSE_LEFT) { /* click */ }
}

INPUT_Shutdown();
```

See `example/main.c` for a complete program.

## License

AGPL-3.0, see `LICENSE`.
