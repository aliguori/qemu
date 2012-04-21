/*
 * QEMU PS/2 keyboard/mouse emulation
 *
 * Copyright (C) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_PS2_H
#define HW_PS2_H

#include "hw/qdev.h"
#include "qemu/pin.h"

#define PS2_QUEUE_SIZE 256

#define TYPE_PS2_DEVICE "ps2-device"
#define PS2_DEVICE(obj) OBJECT_CHECK(PS2State, (obj), TYPE_PS2_DEVICE)

#define TYPE_PS2_KBD "ps2-kbd"
#define PS2_KBD(obj) OBJECT_CHECK(PS2KbdState, (obj), TYPE_PS2_KBD)

#define TYPE_PS2_MOUSE "ps2-mouse"
#define PS2_MOUSE(obj) OBJECT_CHECK(PS2MouseState, (obj), TYPE_PS2_MOUSE)

typedef struct {
    uint8_t data[PS2_QUEUE_SIZE];
    int rptr, wptr, count;
} PS2Queue;

typedef struct {
    DeviceState parent;
    PS2Queue queue;
    int32_t write_cmd;
    Pin irq;
} PS2State;

typedef struct {
    PS2State common;
    int scan_enabled;
    /* QEMU uses translated PC scancodes internally.  To avoid multiple
       conversions we do the translation (if any) in the PS/2 emulation
       not the keyboard controller.  */
    int translate;
    int scancode_set; /* 1=XT, 2=AT, 3=PS/2 */
    int ledstate;
} PS2KbdState;

typedef struct {
    PS2State common;
    uint8_t mouse_status;
    uint8_t mouse_resolution;
    uint8_t mouse_sample_rate;
    uint8_t mouse_wrap;
    uint8_t mouse_type; /* 0 = PS2, 3 = IMPS/2, 4 = IMEX */
    uint8_t mouse_detect_state;
    int mouse_dx; /* current values, needed for 'poll' mode */
    int mouse_dy;
    int mouse_dz;
    uint8_t mouse_buttons;
} PS2MouseState;

/* ps2.c */
void ps2_write_mouse(PS2MouseState *s, int val);
void ps2_write_keyboard(PS2KbdState *s, int val);
uint32_t ps2_read_data(PS2State *s);
void ps2_queue(PS2State *s, int b);
void ps2_keyboard_set_translation(PS2KbdState *s, int mode);
void ps2_mouse_fake_event(PS2MouseState *s);

#endif /* !HW_PS2_H */
