/*
 * ARM Versatile/PB PCI host controller
 *
 * Copyright (c) 2006-2009 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the LGPL.
 */

#include "sysbus.h"
#include "pci.h"
#include "pci_host.h"
#include "exec-memory.h"

typedef struct {
    SysBusDevice busdev;
    qemu_irq irq[4];
    int realview;
    MemoryRegion mem_config;
    MemoryRegion mem_config2;
    MemoryRegion isa;
} PCIVPBState;

static inline uint32_t vpb_pci_config_addr(target_phys_addr_t addr)
{
    return addr & 0xffffff;
}

static void pci_vpb_config_write(void *opaque, target_phys_addr_t addr,
                                 uint64_t val, unsigned size)
{
    pci_data_write(opaque, vpb_pci_config_addr(addr), val, size);
}

static uint64_t pci_vpb_config_read(void *opaque, target_phys_addr_t addr,
                                    unsigned size)
{
    uint32_t val;
    val = pci_data_read(opaque, vpb_pci_config_addr(addr), size);
    return val;
}

static const MemoryRegionOps pci_vpb_config_ops = {
    .read = pci_vpb_config_read,
    .write = pci_vpb_config_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int pci_vpb_map_irq(PCIDevice *d, int irq_num)
{
    return irq_num;
}

static void pci_vpb_set_irq(void *opaque, int irq_num, int level)
{
    qemu_irq *pic = opaque;

    qemu_set_irq(pic[irq_num], level);
}

static int pci_vpb_init(SysBusDevice *dev)
{
    PCIVPBState *s = FROM_SYSBUS(PCIVPBState, dev);
    PCIBus *bus;
    int i;

    for (i = 0; i < 4; i++) {
        sysbus_init_irq(dev, &s->irq[i]);
    }
    bus = pci_register_bus(&dev->qdev, "pci",
                           pci_vpb_set_irq, pci_vpb_map_irq, s->irq,
                           get_system_memory(), get_system_io(),
                           PCI_DEVFN(11, 0), 4);

    /* ??? Register memory space.  */

    /* Our memory regions are:
     * 0 : PCI self config window
     * 1 : PCI config window
     * 2 : PCI IO window (realview_pci only)
     */
    memory_region_init_io(&s->mem_config, &pci_vpb_config_ops, bus,
                          "pci-vpb-selfconfig", 0x1000000);
    sysbus_init_mmio_region(dev, &s->mem_config);
    memory_region_init_io(&s->mem_config2, &pci_vpb_config_ops, bus,
                          "pci-vpb-config", 0x1000000);
    sysbus_init_mmio_region(dev, &s->mem_config2);
    if (s->realview) {
        isa_mmio_setup(&s->isa, 0x0100000);
        sysbus_init_mmio_region(dev, &s->isa);
    }

    pci_create_simple(bus, -1, "versatile_pci_host");
    return 0;
}

static int pci_realview_init(SysBusDevice *dev)
{
    PCIVPBState *s = FROM_SYSBUS(PCIVPBState, dev);
    s->realview = 1;
    return pci_vpb_init(dev);
}

static int versatile_pci_host_init(PCIDevice *d)
{
    pci_set_word(d->config + PCI_STATUS,
		 PCI_STATUS_66MHZ | PCI_STATUS_DEVSEL_MEDIUM);
    pci_set_byte(d->config + PCI_LATENCY_TIMER, 0x10);
    return 0;
}

static void versatile_pci_host_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = versatile_pci_host_init;
    k->vendor_id = PCI_VENDOR_ID_XILINX;
    k->device_id = PCI_DEVICE_ID_XILINX_XC2VP30;
    k->class_id = PCI_CLASS_PROCESSOR_CO;
}

static DeviceInfo versatile_pci_host_info = {
    .name = "versatile_pci_host",
    .size = sizeof(PCIDevice),
    .class_init = versatile_pci_host_class_init,
};

static void pci_vpb_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);

    sdc->init = pci_vpb_init;
}

static DeviceInfo pci_vpb_info = {
    .name = "versatile_pci",
    .size = sizeof(PCIVPBState),
    .class_init = pci_vpb_class_init,
};

static void pci_realview_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sdc = SYS_BUS_DEVICE_CLASS(klass);

    sdc->init = pci_realview_init;
}

static DeviceInfo pci_realview_info = {
    .name = "realview_pci",
    .size = sizeof(PCIVPBState),
    .class_init = pci_realview_class_init,
};

static void versatile_pci_register_devices(void)
{
    sysbus_register_withprop(&pci_vpb_info, TYPE_SYS_BUS_DEVICE);
    sysbus_register_withprop(&pci_realview_info, TYPE_SYS_BUS_DEVICE);
    pci_qdev_register(&versatile_pci_host_info, TYPE_PCI_DEVICE);
}

device_init(versatile_pci_register_devices)
