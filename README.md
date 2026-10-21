# libwiiinput

USB keyboard and mouse input for the Nintendo Wii, built on libogc.

- One call reads every controller and normalises it to `WPAD_BUTTON_*`: Wii Remote,
  Classic Controller, GameCube pad, plus optional Wii U Pro Controller (libwupc),
  Wii U GamePad (libwiidrc) and DualShock 3 (libsicksaxis)
- Background thread keeps the keyboard connected and scanned (hot-plug safe)
- Per-frame polling: held / pressed / released for every USB HID key
- Optional keyboard-to-Wii-Remote mapping (`INPUT_KeyboardPadHeld/Down`)
- Mouse position (clamped to configurable bounds, optional speed multiplier), deltas, wheel and buttons
- Wii Speak microphone (`input_wiispeak.h`): boots the mic and streams 16-bit mono samples
- USB microphones (`input_usbmic.h`): any class-compliant (USB Audio Class 1.0) mic, streamed as 16-bit mono

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
* `INPUT_SetCursorMode(INPUT_CURSOR_IR | INPUT_CURSOR_MOUSE | INPUT_CURSOR_STICK)` picks
  which devices drive the pointer cursor (`INPUT_CursorActive/Position/Moved`),
  so the Wii pointer is optional per app. Draw your own cursor sprite.
* `INPUT_Source()` names the device used last, handy for on-screen hints.
* `INPUT_KeyboardMapSet/Clear` change the keyboard-as-controller mapping.
* `INPUT_GameCubeMapSet/Clear` do the same for the GameCube pad.
* `INPUT_IgnoreWiimoteButtons` leaves a channel's own Wii Remote buttons to another layer.
* `INPUT_StickRaw` gives a stick before its length is clamped, for apps with their own scaling.

### Wii Speak

```c
#include <input_wiispeak.h>

WS_SetLog(printf);                         /* optional: show the boot steps */
if (WS_Open(firmware, firmware_size) == WS_OK) {
    WS_SetFreq(WS_FREQ_16KHZ);
    WS_SetSignalProcessing(false);
    WS_Start();
    /* each frame: */
    s16 buf[1024];
    u32 n = WS_Read(buf, 1024);            /* host-endian samples */
    WS_Stop();
    WS_Close();
}
```

The Wii Speak (`057e:0308`) is not a USB audio class device. The driver reimplements the RVL SDK's
`PMIC` library over `/dev/usb/oh0`, and was taken from RecordMii. The mic has no firmware of its own:
`WS_Open` uploads the 97,480-byte `PMIC_FIRM_DATA` blob, which is Nintendo's and is **not** included.
Apps have to get it from the user (RecordMii's `tools/extract_pmic_firmware.py` pulls it out of an
RVL SDK `pmic.a`). Dolphin's emulated Wii Speak accepts any non-empty blob and records from the host
microphone. Not yet tested on real hardware.

### USB microphones

```c
#include <input_usbmic.h>

um_info mic;

UM_SetLog(printf);                         /* optional: show the scan */
if (UM_Open(&mic) == UM_OK) {              /* mic.rate, mic.name, ... */
    UM_SetVolume(75);                      /* if mic.has_volume */
    UM_Start();
    /* each frame: */
    s16 buf[1024];
    u32 n = UM_Read(buf, 1024);            /* mono, at mic.rate */
    UM_Stop();
    UM_Close();
}
```

Works with the plug-and-play USB mics sold for PCs, which follow the USB Audio Class 1.0 spec. The
driver reads the format list from the mic itself and picks 16-bit PCM at 48 kHz when offered (then
44.1 kHz, then whatever is closest); stereo and 24-bit streams are folded to 16-bit mono. It unmutes
the mic and exposes its volume control as a percentage. Full-speed only, which is all the Wii's
ports run at; mics that only stream at high speed (USB Audio Class 2.0) are not supported. Needs an
IOS with `/dev/usb/oh0` or `/dev/usb/ven`. Not yet tested on real hardware.

### Using it from an app

Add it as a git submodule and compile its sources with your own flags, so the
`HAVE_*` options apply to it too:

```sh
git submodule add https://github.com/quatric/libwiiinput.git libwiiinput
```

```make
SOURCES  += libwiiinput/source
CFLAGS   += -Ilibwiiinput/include
```

See `example/main.c` for a complete program.

## License

AGPL-3.0, see `LICENSE`.
