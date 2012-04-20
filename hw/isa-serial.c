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
#include "isa-serial.h"
#include "pc.h"
#include "sysemu.h"

/**
 * UART on an ISA card
 */

static const int isa_serial_io[MAX_SERIAL_PORTS] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
static const int isa_serial_irq[MAX_SERIAL_PORTS] = { 4, 3, 4, 3 };

static int serial_isa_realize(ISADevice *dev)
{
    static int index;
    ISASerialState *isa = ISA_SERIAL(dev);
    SerialState *s = &isa->state;
    int err;

    qdev_prop_set_chr(DEVICE(&isa->state), "chardev", isa->chr);
    qdev_prop_set_uint32(DEVICE(&isa->state), "wakeup", isa->wakeup);

    err = qdev_init(DEVICE(&isa->state));
    if (err < 0) {
        return err;
    }

    if (isa->index == -1) {
        isa->index = index;
    }
    if (isa->index >= MAX_SERIAL_PORTS) {
        return -1;
    }
    if (isa->iobase == -1) {
        isa->iobase = isa_serial_io[isa->index];
    }
    if (isa->isairq == -1) {
        isa->isairq = isa_serial_irq[isa->index];
    }
    index++;

    isa_init_irq(dev, serial_get_irq(s), isa->isairq);
    qdev_set_legacy_instance_id(&dev->qdev, isa->iobase, 3);
    isa_register_ioport(dev, serial_get_io(s), isa->iobase);

    return 0;
}

static Property serial_isa_properties[] = {
    DEFINE_PROP_UINT32("index", ISASerialState, index,   -1),
    DEFINE_PROP_HEX32("iobase", ISASerialState, iobase,  -1),
    DEFINE_PROP_UINT32("irq",   ISASerialState, isairq,  -1),
    DEFINE_PROP_CHR("chardev",  ISASerialState, chr),
    DEFINE_PROP_UINT32("wakeup", ISASerialState, wakeup, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void serial_isa_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISADeviceClass *ic = ISA_DEVICE_CLASS(klass);
    ic->init = serial_isa_realize;
    dc->props = serial_isa_properties;
}

static void serial_isa_initfn(Object *obj)
{
    ISASerialState *isa = ISA_SERIAL(obj);

    object_initialize(&isa->state, TYPE_SERIAL);
    qdev_prop_set_globals(DEVICE(&isa->state));

    object_property_add_child(obj, "uart", OBJECT(&isa->state), NULL);
}

static TypeInfo serial_isa_info = {
    .name          = TYPE_ISA_SERIAL,
    .parent        = TYPE_ISA_DEVICE,
    .instance_init = serial_isa_initfn,
    .instance_size = sizeof(ISASerialState),
    .class_init    = serial_isa_class_initfn,
};

static void register_types(void)
{
    type_register_static(&serial_isa_info);
}

type_init(register_types)
