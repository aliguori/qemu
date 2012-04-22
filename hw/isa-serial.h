/*
 * QEMU 16550A UART emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2008 Citrix Systems, Inc.
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
#ifndef QEMU_ISA_SERIAL_H
#define QEMU_ISA_SERIAL_H

#include "isa.h"
#include "serial.h"

#define TYPE_ISA_SERIAL "isa-serial"
#define ISA_SERIAL(obj) OBJECT_CHECK(ISASerialState, obj, TYPE_ISA_SERIAL)

typedef struct ISASerialState {
    ISADevice dev;
    uint32_t index;
    uint32_t iobase;
    uint32_t isairq;
    CharDriverState *chr;
    uint32_t wakeup;
    SerialState state;
} ISASerialState;

static inline bool serial_isa_init(ISABus *bus, int index,
                                   CharDriverState *chr)
{
    ISADevice *dev;

    dev = isa_try_create(bus, "isa-serial");
    if (!dev) {
        return false;
    }
    qdev_prop_set_uint32(&dev->qdev, "index", index);
    qdev_prop_set_chr(&dev->qdev, "chardev", chr);
    if (qdev_init(&dev->qdev) < 0) {
        return false;
    }
    return true;
}

#endif