/*
 * QEMU 8253/8254 interval timer emulation
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
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
#include "hw.h"
#include "pc.h"
#include "isa.h"
#include "qemu-timer.h"
#include "i8254.h"
#include "i8254_internal.h"

//#define DEBUG_PIT

#define RW_STATE_LSB 1
#define RW_STATE_MSB 2
#define RW_STATE_WORD0 3
#define RW_STATE_WORD1 4

/* sad, but necessary to maintain compat */
#define TYPE_PIT "isa-pit"
#define PIT(obj) OBJECT_CHECK(PITCommonState, (obj), TYPE_PIT)

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time);

static int pit_get_count(PITChannelState *s)
{
    uint64_t d;
    int counter;

    d = muldiv64(qemu_get_clock_ns(vm_clock) - s->count_load_time, PIT_FREQ,
                 get_ticks_per_sec());
    switch(s->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        counter = (s->count - d) & 0xffff;
        break;
    case 3:
        /* XXX: may be incorrect for odd counts */
        counter = s->count - ((2 * d) % s->count);
        break;
    default:
        counter = s->count - (d % s->count);
        break;
    }
    return counter;
}

/* val must be 0 or 1 */
static void pit_set_channel_gate(PITCommonState *s, PITChannelState *sc,
                                 int val)
{
    switch (sc->mode) {
    default:
    case 0:
    case 4:
        /* XXX: just disable/enable counting */
        break;
    case 1:
    case 5:
        if (sc->gate < val) {
            /* restart counting on rising edge */
            sc->count_load_time = qemu_get_clock_ns(vm_clock);
            pit_irq_timer_update(sc, sc->count_load_time);
        }
        break;
    case 2:
    case 3:
        if (sc->gate < val) {
            /* restart counting on rising edge */
            sc->count_load_time = qemu_get_clock_ns(vm_clock);
            pit_irq_timer_update(sc, sc->count_load_time);
        }
        /* XXX: disable/enable counting */
        break;
    }
    sc->gate = val;
}

static inline void pit_load_count(PITChannelState *s, int val)
{
    if (val == 0)
        val = 0x10000;
    s->count_load_time = qemu_get_clock_ns(vm_clock);
    s->count = val;
    pit_irq_timer_update(s, s->count_load_time);
}

/* if already latched, do not latch again */
static void pit_latch_count(PITChannelState *s)
{
    if (!s->count_latched) {
        s->latched_count = pit_get_count(s);
        s->count_latched = s->rw_mode;
    }
}

static void pit_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    PITCommonState *pit = opaque;
    int channel, access;
    PITChannelState *s;

    addr &= 3;
    if (addr == 3) {
        channel = val >> 6;
        if (channel == 3) {
            /* read back command */
            for(channel = 0; channel < 3; channel++) {
                s = &pit->channels[channel];
                if (val & (2 << channel)) {
                    if (!(val & 0x20)) {
                        pit_latch_count(s);
                    }
                    if (!(val & 0x10) && !s->status_latched) {
                        /* status latch */
                        /* XXX: add BCD and null count */
                        s->status =
                            (pit_get_out(s,
                                         qemu_get_clock_ns(vm_clock)) << 7) |
                            (s->rw_mode << 4) |
                            (s->mode << 1) |
                            s->bcd;
                        s->status_latched = 1;
                    }
                }
            }
        } else {
            s = &pit->channels[channel];
            access = (val >> 4) & 3;
            if (access == 0) {
                pit_latch_count(s);
            } else {
                s->rw_mode = access;
                s->read_state = access;
                s->write_state = access;

                s->mode = (val >> 1) & 7;
                s->bcd = val & 1;
                /* XXX: update irq timer ? */
            }
        }
    } else {
        s = &pit->channels[addr];
        switch(s->write_state) {
        default:
        case RW_STATE_LSB:
            pit_load_count(s, val);
            break;
        case RW_STATE_MSB:
            pit_load_count(s, val << 8);
            break;
        case RW_STATE_WORD0:
            s->write_latch = val;
            s->write_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
            pit_load_count(s, s->write_latch | (val << 8));
            s->write_state = RW_STATE_WORD0;
            break;
        }
    }
}

static uint32_t pit_ioport_read(void *opaque, uint32_t addr)
{
    PITCommonState *pit = opaque;
    int ret, count;
    PITChannelState *s;

    addr &= 3;
    s = &pit->channels[addr];
    if (s->status_latched) {
        s->status_latched = 0;
        ret = s->status;
    } else if (s->count_latched) {
        switch(s->count_latched) {
        default:
        case RW_STATE_LSB:
            ret = s->latched_count & 0xff;
            s->count_latched = 0;
            break;
        case RW_STATE_MSB:
            ret = s->latched_count >> 8;
            s->count_latched = 0;
            break;
        case RW_STATE_WORD0:
            ret = s->latched_count & 0xff;
            s->count_latched = RW_STATE_MSB;
            break;
        }
    } else {
        switch(s->read_state) {
        default:
        case RW_STATE_LSB:
            count = pit_get_count(s);
            ret = count & 0xff;
            break;
        case RW_STATE_MSB:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            break;
        case RW_STATE_WORD0:
            count = pit_get_count(s);
            ret = count & 0xff;
            s->read_state = RW_STATE_WORD1;
            break;
        case RW_STATE_WORD1:
            count = pit_get_count(s);
            ret = (count >> 8) & 0xff;
            s->read_state = RW_STATE_WORD0;
            break;
        }
    }
    return ret;
}

static void pit_irq_timer_update(PITChannelState *s, int64_t current_time)
{
    int64_t expire_time;
    int irq_level;

    if (!s->irq_timer || s->irq_disabled) {
        return;
    }
    expire_time = pit_get_next_transition_time(s, current_time);
    irq_level = pit_get_out(s, current_time);
    pin_set_level(&s->irq, irq_level);
#ifdef DEBUG_PIT
    printf("irq_level=%d next_delay=%f\n",
           irq_level,
           (double)(expire_time - current_time) / get_ticks_per_sec());
#endif
    s->next_transition_time = expire_time;
    if (expire_time != -1)
        qemu_mod_timer(s->irq_timer, expire_time);
    else
        qemu_del_timer(s->irq_timer);
}

static void pit_irq_timer(void *opaque)
{
    PITChannelState *s = opaque;

    pit_irq_timer_update(s, s->next_transition_time);
}

static void pit_reset(DeviceState *dev)
{
    PITCommonState *pit = PIT(dev);
    PITChannelState *s;

    pit_reset_common(pit);

    s = &pit->channels[0];
    if (!s->irq_disabled) {
        qemu_mod_timer(s->irq_timer, s->next_transition_time);
    }
}

/* When HPET is operating in legacy mode, suppress the ignored timer IRQ,
 * reenable it when legacy mode is left again. */
static void pit_irq_control(void *opaque, int n, int enable)
{
    PITCommonState *pit = opaque;
    PITChannelState *s = &pit->channels[0];

    if (enable) {
        s->irq_disabled = 0;
        pit_irq_timer_update(s, qemu_get_clock_ns(vm_clock));
    } else {
        s->irq_disabled = 1;
        qemu_del_timer(s->irq_timer);
    }
}

static const MemoryRegionPortio pit_portio[] = {
    { 0, 4, 1, .write = pit_ioport_write },
    { 0, 3, 1, .read = pit_ioport_read },
    PORTIO_END_OF_LIST()
};

static const MemoryRegionOps pit_ioport_ops = {
    .old_portio = pit_portio
};

static void pit_post_load(PITCommonState *s)
{
    PITChannelState *sc = &s->channels[0];

    if (sc->next_transition_time != -1) {
        qemu_mod_timer(sc->irq_timer, sc->next_transition_time);
    } else {
        qemu_del_timer(sc->irq_timer);
    }
}

static int pit_realize(PITCommonState *pit)
{
    return 0;
}

static void pit_initfn(Object *obj)
{
    PITCommonState *pit = PIT(obj);
    PITChannelState *s;

    s = &pit->channels[0];
    /* the timer 0 is connected to an IRQ */
    s->irq_timer = qemu_new_timer_ns(vm_clock, pit_irq_timer, s);
    
    memory_region_init_io(&pit->ioports, &pit_ioport_ops, pit, "pit", 4);

    /* FIXME: need to refactor RTC to eliminate this */
    qdev_init_gpio_in(DEVICE(pit), pit_irq_control, 1);
}

PITCommonState *pit_init(ISABus *bus, int base, int isa_irq, qemu_irq alt_irq)
{
    PITCommonState *pit;
    PITChannelState *s;

    pit = PIT(object_new(TYPE_PIT));
    qdev_prop_set_globals(DEVICE(pit));
    qdev_init_nofail(DEVICE(pit));
    
    s = &pit->channels[0];

    memory_region_add_subregion(bus->address_space_io, base, &pit->ioports);

    if (isa_irq >= 0) {
        pin_connect_pin(&s->irq, isa_get_pin(bus, isa_irq));
    } else {
        pin_connect_qemu_irq(&s->irq, alt_irq);
    }

    return pit;
}

static void pit_class_initfn(ObjectClass *klass, void *data)
{
    PITCommonClass *k = PIT_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->init = pit_realize;
    k->set_channel_gate = pit_set_channel_gate;
    k->get_channel_info = pit_get_channel_info_common;
    k->post_load = pit_post_load;
    dc->reset = pit_reset;
}

static TypeInfo pit_info = {
    .name          = TYPE_PIT,
    .parent        = TYPE_PIT_COMMON,
    .instance_init = pit_initfn,
    .instance_size = sizeof(PITCommonState),
    .class_init    = pit_class_initfn,
};

static void pit_register_types(void)
{
    type_register_static(&pit_info);
}

type_init(pit_register_types)
