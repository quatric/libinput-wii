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
INPUT_SetScreenSize(640, 480);

while (running) {
    VIDEO_WaitVSync();
    INPUT_Update();                        /* once per frame */

    u32 down = INPUT_ButtonsDown();        /* WPAD_BUTTON_* from any device */
    if (down & WPAD_BUTTON_A) { }

    float x, y;
    if (INPUT_Pointer(&x, &y)) { }         /* Wii Remote IR */
    INPUT_Stick(INPUT_STICK_LEFT, &x, &y, NULL);

    input_key_event ev;
    while (INPUT_KeyPop(&ev)) { /* ev.ch is typed ASCII */ }

    if (INPUT_MouseButtonsPressed() & INPUT_MOUSE_LEFT) { }
}

INPUT_Shutdown();
```

* Buttons are `WPAD_BUTTON_*` bits plus `INPUT_BUTTON_Z` / `INPUT_BUTTON_C` for the
  Nunchuk. Classic Controller, GameCube, Wii U Pro, GamePad and DualShock 3 are
  folded onto the Wii Remote layout.
* Sticks are -1..1, y up, no deadzone; the strongest one across devices wins.
* `INPUT_Source()` names the device used last, handy for on-screen hints.
* `INPUT_KeyboardMapSet/Clear` change the keyboard-as-controller mapping.

### Using it from an app

Add it as a git submodule and compile its sources with your own flags, so the
`HAVE_*` options apply to it too:

```sh
git submodule add https://github.com/quatric/libinput-wii.git libinput-wii
```

```make
SOURCES  += libinput-wii/source
CFLAGS   += -Ilibinput-wii/include
```

See `example/main.c` for a complete program.

## License

AGPL-3.0, see `LICENSE`.
