/*
 * CSTL Watch Dog Timer Demo
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "hw.h"
#include "pci.h"

typedef struct CSTLWatchdogState {
    PCIDevice dev;

    MemoryRegion io;
} CSTLWatchdogState;

#define TYPE_CSTL_WATCHDOG "cstl-watchdog"
#define CSTL_WATCHDOG(obj) \
    OBJECT_CHECK(CSTLWatchdogState, (obj), TYPE_CSTL_WATCHDOG)

static uint64_t cwd_io_read(void *opaque, target_phys_addr_t addr,
                            unsigned size)
{
    switch (addr) {
    case 0x00:
        return 0x42;
    default:
        break;
    }

    return 0;
}

static void cwd_io_write(void *opaque, target_phys_addr_t addr,
                         uint64_t val, unsigned size)
{
}

static const MemoryRegionOps cwd_io_ops = {
    .read = cwd_io_read,
    .write = cwd_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const VMStateDescription vmstate_cwd = {
    .name = TYPE_CSTL_WATCHDOG,
    .version_id = 1,
    .fields      = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, CSTLWatchdogState),
        VMSTATE_END_OF_LIST()
    }
};

static int cwd_unrealize(PCIDevice *dev)
{
    return 0;
}

static int cwd_realize(PCIDevice *pci_dev)
{
    CSTLWatchdogState *s = CSTL_WATCHDOG(pci_dev);

    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io);

    return 0;
}

static void cwd_reset(DeviceState *dev)
{
//    CSTLWatchdogState *s = CSTL_WATCHDOG(dev);
}

static void cwd_initfn(Object *obj)
{
    CSTLWatchdogState *s = CSTL_WATCHDOG(obj);

    memory_region_init_io(&s->io, &cwd_io_ops, s, "cstl-watchdog-io", 64);
}

static Property cwd_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void cwd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = cwd_realize;
    k->exit = cwd_unrealize;
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->device_id = 0x0101;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_SYSTEM_OTHER;
    dc->reset = cwd_reset;
    dc->vmsd = &vmstate_cwd;
    dc->props = cwd_properties;
}

static TypeInfo cstl_watchdog_info = {
    .name          = TYPE_CSTL_WATCHDOG,
    .parent        = TYPE_PCI_DEVICE,
    .instance_init = cwd_initfn,
    .instance_size = sizeof(CSTLWatchdogState),
    .class_init    = cwd_class_init,
};

static void register_types(void)
{
    type_register_static(&cstl_watchdog_info);
}

type_init(register_types)
