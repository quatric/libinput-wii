# libinput-wii

USB keyboard and mouse input for the Nintendo Wii, built on libogc.

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

Link with `-linput-wii -lwiikeyboard -logc` (and the usual `-lwiiuse -lbte -lm`).

## Usage

```c
#include <input_wii.h>

INPUT_Init();
INPUT_MouseSetBounds(640, 480);

while (running) {
    VIDEO_WaitVSync();
    INPUT_Update();                       /* once per frame */

    if (INPUT_KeyPressed(0x28)) { /* Enter */ }

    int x, y;
    INPUT_MousePosition(&x, &y);
    if (INPUT_MouseButtonsPressed() & INPUT_MOUSE_LEFT) { /* click */ }
}

INPUT_Shutdown();
```

See `example/main.c` for a complete program.

## License

AGPL-3.0, see `LICENSE`.
