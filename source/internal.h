#ifndef INPUT_INTERNAL_H
#define INPUT_INTERNAL_H

#include "input_wii.h"

s32  input_keyboard_init(void);
void input_keyboard_shutdown(void);
void input_keyboard_update(void);

s32  input_mouse_init(void);
void input_mouse_shutdown(void);
void input_mouse_update(void);

#endif
