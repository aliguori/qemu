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
#ifndef QEMU_SERIAL_H
#define QEMU_SERIAL_H

#include "qemu-common.h"
#include "qdev.h"
#include "memory.h"
#include "qemu-char.h"
#include "qemu/pin.h"
#include "qemu-timer.h"
#include "exec-memory.h"

#define UART_FIFO_LENGTH    16      /* 16550A Fifo Length */

#define TYPE_SERIAL "serial"
#define SERIAL(obj) OBJECT_CHECK(SerialState, (obj), TYPE_SERIAL)

typedef struct SerialFIFO {
    uint8_t data[UART_FIFO_LENGTH];
    uint8_t count;
    uint8_t itl;                        /* Interrupt Trigger Level */
    uint8_t tail;
    uint8_t head;
} SerialFIFO;

struct SerialState {
    DeviceState parent;

    uint16_t divider;
    uint8_t rbr; /* receive register */
    uint8_t thr; /* transmit holding register */
    uint8_t tsr; /* transmit shift register */
    uint8_t ier;
    uint8_t iir; /* read only */
    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr; /* read only */
    uint8_t msr; /* read only */
    uint8_t scr;
    uint8_t fcr;
    uint8_t fcr_vmstate; /* we can't write directly this value
                            it has side effects */
    /* NOTE: this hidden state is necessary for tx irq generation as
       it can be reset while reading iir */
    int thr_ipending;
    Pin irq;
    CharDriverState *chr;
    int last_break_enable;
    int32_t it_shift;
    int32_t baudbase;
    int tsr_retry;
    uint32_t wakeup;

    /* Time when the last byte was successfully sent out of the tsr */
    uint64_t last_xmit_ts;
    SerialFIFO recv_fifo;
    SerialFIFO xmit_fifo;

    struct QEMUTimer *fifo_timeout_timer;
    /* timeout interrupt pending state */
    int timeout_ipending;
    struct QEMUTimer *transmit_timer;

    /* time to transmit a char in ticks*/
    uint64_t char_transmit_time;
    int poll_msl;

    struct QEMUTimer *modem_status_poll;
    MemoryRegion io;
};

void serial_set_frequency(SerialState *s, uint32_t frequency);

Pin *serial_get_irq(SerialState *s);

MemoryRegion *serial_get_io(SerialState *s);


/**
 * Legacy compat contructors.
 *
 * Do not use in new code!
 **/
static inline SerialState *serial_mm_init(MemoryRegion *address_space,
                                          target_phys_addr_t base, int it_shift,
                                          qemu_irq irq, int baudbase,
                                          CharDriverState *chr,
                                          enum device_endian end)
{
    SerialState *s;
    DeviceState *dev;

    s = SERIAL(object_new(TYPE_SERIAL));
    dev = DEVICE(s);

    qdev_prop_set_globals(dev);
    qdev_prop_set_int32(dev, "it_shift", it_shift);
    qdev_prop_set_int32(dev, "baudbase", baudbase);
    qdev_prop_set_chr(dev, "chardev", chr);
    qdev_init_nofail(dev);

    pin_connect_qemu_irq(&s->irq, irq);
    memory_region_add_subregion(address_space, base, &s->io);

    return s;
}

static inline SerialState *serial_init(int base, qemu_irq irq, int baudbase,
                                       CharDriverState *chr)
{
    return serial_mm_init(get_system_io(), base, 0, irq, baudbase, chr, 0);
}

#endif
