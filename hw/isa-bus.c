/*
 * isa bus support for qdev.
 *
 * Copyright (c) 2009 Gerd Hoffmann <kraxel@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "hw.h"
#include "monitor.h"
#include "sysbus.h"
#include "isa.h"
#include "exec-memory.h"

static ISABus *isabus;
target_phys_addr_t isa_mem_base = 0;

#define TYPE_ISA_BUS "ISA"

static TypeInfo isa_bus_info = {
    .name = TYPE_ISA_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(ISABus),
};

ISABus *isa_bus_new(DeviceState *dev, MemoryRegion *address_space_io)
{
    if (isabus) {
        fprintf(stderr, "Can't create a second ISA bus\n");
        return NULL;
    }
    if (NULL == dev) {
        dev = qdev_create(NULL, "isabus-bridge");
        qdev_init_nofail(dev);
    }

    isabus = FROM_QBUS(ISABus, qbus_create(TYPE_ISA_BUS, dev, NULL));
    isabus->address_space_io = address_space_io;
    return isabus;
}

void isa_bus_irqs(ISABus *bus, qemu_irq *irqs)
{
    if (!bus) {
        hw_error("Can't set isa irqs with no isa bus present.");
    }
    bus->irqs = irqs;
}

/*
 * isa_get_irq() returns the corresponding qemu_irq entry for the i8259.
 *
 * This function is only for special cases such as the 'ferr', and
 * temporary use for normal devices until they are converted to qdev.
 */
qemu_irq isa_get_irq(ISADevice *dev, int isairq)
{
    assert(!dev || DO_UPCAST(ISABus, qbus, dev->qdev.parent_bus) == isabus);
    if (isairq < 0 || isairq > 15) {
        hw_error("isa irq %d invalid", isairq);
    }
    return isabus->irqs[isairq];
}

void isa_init_irq(ISADevice *dev, qemu_irq *p, int isairq)
{
    assert(dev->nirqs < ARRAY_SIZE(dev->isairq));
    dev->isairq[dev->nirqs] = isairq;
    *p = isa_get_irq(dev, isairq);
    dev->nirqs++;
}

static inline void isa_init_ioport(ISADevice *dev, uint16_t ioport)
{
    if (dev && (dev->ioport_id == 0 || ioport < dev->ioport_id)) {
        dev->ioport_id = ioport;
    }
}

void isa_register_ioport(ISADevice *dev, MemoryRegion *io, uint16_t start)
{
    memory_region_add_subregion(isabus->address_space_io, start, io);
    isa_init_ioport(dev, start);
}

void isa_register_portio_list(ISADevice *dev, uint16_t start,
                              const MemoryRegionPortio *pio_start,
                              void *opaque, const char *name)
{
    PortioList *piolist = g_new(PortioList, 1);

    /* START is how we should treat DEV, regardless of the actual
       contents of the portio array.  This is how the old code
       actually handled e.g. the FDC device.  */
    isa_init_ioport(dev, start);

    portio_list_init(piolist, pio_start, opaque, name);
    portio_list_add(piolist, isabus->address_space_io, start);
}

static int isa_qdev_init(DeviceState *qdev)
{
    ISADevice *dev = ISA_DEVICE(qdev);
    ISADeviceClass *klass = ISA_DEVICE_GET_CLASS(dev);

    dev->isairq[0] = -1;
    dev->isairq[1] = -1;

    if (klass->init) {
        return klass->init(dev);
    }

    return 0;
}

ISADevice *isa_create(ISABus *bus, const char *name)
{
    DeviceState *dev;

    if (!bus) {
        hw_error("Tried to create isa device %s with no isa bus present.",
                 name);
    }
    dev = qdev_create(&bus->qbus, name);
    return ISA_DEVICE(dev);
}

ISADevice *isa_try_create(ISABus *bus, const char *name)
{
    DeviceState *dev;

    if (!bus) {
        hw_error("Tried to create isa device %s with no isa bus present.",
                 name);
    }
    dev = qdev_try_create(&bus->qbus, name);
    return ISA_DEVICE(dev);
}

ISADevice *isa_create_simple(ISABus *bus, const char *name)
{
    ISADevice *dev;

    dev = isa_create(bus, name);
    qdev_init_nofail(&dev->qdev);
    return dev;
}

static int isabus_bridge_init(SysBusDevice *dev)
{
    /* nothing */
    return 0;
}

static void isabus_bridge_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = isabus_bridge_init;
    dc->fw_name = "isa";
    dc->no_user = 1;
}

static TypeInfo isabus_bridge_info = {
    .name          = "isabus-bridge",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SysBusDevice),
    .class_init    = isabus_bridge_class_init,
};

static char *isa_device_get_fw_dev_path(DeviceState *dev)
{
    ISADevice *d = (ISADevice*)dev;
    char path[40];
    int off;

    off = snprintf(path, sizeof(path), "%s", qdev_fw_name(dev));
    if (d->ioport_id) {
        snprintf(path + off, sizeof(path) - off, "@%04x", d->ioport_id);
    }

    return strdup(path);
}

static void isa_qdev_dev_print(DeviceState *dev, Monitor *mon, int indent)
{
    ISADevice *d = ISA_DEVICE(dev);

    if (d->isairq[1] != -1) {
        monitor_printf(mon, "%*sisa irqs %d,%d\n", indent, "",
                       d->isairq[0], d->isairq[1]);
    } else if (d->isairq[0] != -1) {
        monitor_printf(mon, "%*sisa irq %d\n", indent, "",
                       d->isairq[0]);
    }
}

static void isa_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);
    k->init = isa_qdev_init;
    k->bus_type = TYPE_ISA_BUS;
    k->get_fw_dev_path = isa_device_get_fw_dev_path;
    k->print_dev = isa_qdev_dev_print;
}

static TypeInfo isa_device_type_info = {
    .name = TYPE_ISA_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ISADevice),
    .abstract = true,
    .class_size = sizeof(ISADeviceClass),
    .class_init = isa_device_class_init,
};

static void isabus_register_devices(void)
{
    type_register_static(&isa_bus_info);
    type_register_static(&isabus_bridge_info);
    type_register_static(&isa_device_type_info);
}

MemoryRegion *isa_address_space(ISADevice *dev)
{
    return get_system_memory();
}

device_init(isabus_register_devices)
