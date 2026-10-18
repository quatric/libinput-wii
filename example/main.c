#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>
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
    INPUT_MouseSetBounds(rmode->fbWidth, rmode->xfbHeight);

    while (1) {
        VIDEO_WaitVSync();
        INPUT_Update();

        int x, y;
        INPUT_MousePosition(&x, &y);
        printf("\x1b[2J\x1b[2;0Hkbd:%d mouse:%d pos:%3d,%3d btn:%d\n",
               INPUT_KeyboardConnected(), INPUT_MouseConnected(), x, y,
               INPUT_MouseButtonsHeld());

        if (INPUT_KeyPressed(0x29)) /* Esc */
            break;
    }

    INPUT_Shutdown();
    return 0;
}
