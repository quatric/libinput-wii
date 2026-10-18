#ifndef LIBINPUT_INTERNAL_H
#define LIBINPUT_INTERNAL_H

#include "input_wii.h"

extern int input_screen_w, input_screen_h;

void input_set_source(const char *name);

s32  input_pads_init(u32 flags);
void input_pads_shutdown(void);
void input_pads_update(void);
void input_pads_set_screen(int w, int h);

s32  input_keyboard_init(void);
void input_keyboard_shutdown(void);
void input_keyboard_update(void);

s32  input_mouse_init(void);
void input_mouse_shutdown(void);
void input_mouse_update(void);
void input_mouse_set_bounds(int w, int h);

#endif
