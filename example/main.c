#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <input_wii.h>

int main(void)
{
    VIDEO_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();

    INPUT_Init();
    INPUT_SetScreenSize(rmode->fbWidth, rmode->xfbHeight);

    while (1) {
        VIDEO_WaitVSync();
        INPUT_Update();

        float px = 0, py = 0, sx = 0, sy = 0;
        int mx, my;
        bool ir = INPUT_Pointer(&px, &py);
        INPUT_Stick(INPUT_STICK_LEFT, &sx, &sy, NULL);
        INPUT_MousePosition(&mx, &my);

        printf("\x1b[2J\x1b[2;0H"
               "source: %s\n"
               "buttons: %08x\n"
               "ir: %d (%.0f,%.0f)  stick: %.2f,%.2f\n"
               "mouse: %d (%d,%d) btn %d\n"
               "keyboard: %d\n",
               INPUT_Source(), INPUT_ButtonsHeld(), ir, px, py, sx, sy,
               INPUT_MouseConnected(), mx, my, INPUT_MouseButtonsHeld(),
               INPUT_KeyboardConnected());

        input_key_event ev;
        while (INPUT_KeyPop(&ev))
            if (ev.code == 0x29) /* Esc */
                goto done;

        if (INPUT_ButtonsDown() & WPAD_BUTTON_HOME)
            break;
    }
done:
    INPUT_Shutdown();
    return 0;
}
