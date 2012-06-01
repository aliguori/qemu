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

#define DEBUG_CSTL_WATCHDOG

#ifdef DEBUG_CSTL_WATCHDOG
#define dprintf(fmt, ...) \
    do { fprintf(stderr, "cstl-watchdog: " fmt, ## __VA_ARGS__); } while (0)
#else
#define dprintf(fmt, ...) do { } while (0)
#endif

typedef struct CSTLWatchdogState {
    PCIDevice dev;

    uint8_t activated;

    uint8_t triggered;

    uint32_t missed_ticks;

    QEMUTimer *watchdog_timer;

    MemoryRegion io;
} CSTLWatchdogState;

#define TYPE_CSTL_WATCHDOG "cstl-watchdog"
#define CSTL_WATCHDOG(obj) \
    OBJECT_CHECK(CSTLWatchdogState, (obj), TYPE_CSTL_WATCHDOG)

static void cwd_timer_event(void *opaque)
{
    CSTLWatchdogState *s = CSTL_WATCHDOG(opaque);

    (void)s;

    dprintf("watch dog fire!\n");

    if (s->activated) {
        dprintf("rearming\n");
        qemu_mod_timer(s->watchdog_timer,
                       qemu_get_clock_ms(rt_clock) + 1000);
    }
}

static uint64_t cwd_io_read(void *opaque, target_phys_addr_t addr,
                            unsigned size)
{
    CSTLWatchdogState *s = CSTL_WATCHDOG(opaque);

    switch (addr) {
    case 0x00:
        return 0x42;
    case 0x01:
        return s->activated;
    default:
        break;
    }

    return 0;
}

static void cwd_io_write(void *opaque, target_phys_addr_t addr,
                         uint64_t val, unsigned size)
{
    CSTLWatchdogState *s = CSTL_WATCHDOG(opaque);

    switch (addr) {
    case 0x00:
        /* read-only */
        break;
    case 0x01:
        s->activated = !!val;

        if (s->activated) {
            dprintf("Activated!\n");
            cwd_timer_event(s);
        } else {
            dprintf("Deactivated!\n");
            qemu_del_timer(s->watchdog_timer);
        }
        break;
    default:
        break;
    }
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
        VMSTATE_UINT8(activated, CSTLWatchdogState),
        VMSTATE_TIMER(watchdog_timer, CSTLWatchdogState),
        VMSTATE_UINT8(triggered, CSTLWatchdogState),
        VMSTATE_UINT32(missed_ticks, CSTLWatchdogState),
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
    CSTLWatchdogState *s = CSTL_WATCHDOG(dev);

    s->activated = 0;
    s->triggered = 0;
    s->missed_ticks = 0;
}

static void cwd_initfn(Object *obj)
{
    CSTLWatchdogState *s = CSTL_WATCHDOG(obj);

    memory_region_init_io(&s->io, &cwd_io_ops, s, "cstl-watchdog-io", 64);

    s->watchdog_timer = qemu_new_timer_ms(rt_clock, cwd_timer_event, s);
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
