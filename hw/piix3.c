/*
 * QEMU PIIX3 PCI-ISA Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori <aliguori@us.ibm.com>
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

#include "piix3.h"
#include "range.h"
#include "kvm.h"
#include "xen.h"
#include "pc.h"

/* PIIX3 PCI to ISA bridge */
static void piix3_set_irq_pic(PIIX3State *piix3, int pic_irq)
{
    qemu_set_irq(piix3->pic[pic_irq],
                 !!(piix3->pic_levels &
                    (((1ULL << PIIX_NUM_PIRQS) - 1) <<
                     (pic_irq * PIIX_NUM_PIRQS))));
}

void piix3_set_irq_level(PIIX3State *piix3, int pirq, int level)
{
    int pic_irq;
    uint64_t mask;

    pic_irq = piix3->dev.config[PIIX_PIRQC + pirq];
    if (pic_irq >= PIIX_NUM_PIC_IRQS) {
        return;
    }

    mask = 1ULL << ((pic_irq * PIIX_NUM_PIRQS) + pirq);
    piix3->pic_levels &= ~mask;
    piix3->pic_levels |= mask * !!level;

    piix3_set_irq_pic(piix3, pic_irq);
}

/* irq routing is changed. so rebuild bitmap */
static void piix3_update_irq_levels(PIIX3State *piix3)
{
    int pirq;

    piix3->pic_levels = 0;
    for (pirq = 0; pirq < PIIX_NUM_PIRQS; pirq++) {
        piix3_set_irq_level(piix3, pirq,
                            pci_bus_get_irq_level(piix3->dev.bus, pirq));
    }
}

static void piix3_write_config(PCIDevice *dev,
                               uint32_t address, uint32_t val, int len)
{
    pci_default_write_config(dev, address, val, len);
    if (ranges_overlap(address, len, PIIX_PIRQC, 4)) {
        PIIX3State *piix3 = DO_UPCAST(PIIX3State, dev, dev);
        int pic_irq;
        piix3_update_irq_levels(piix3);
        for (pic_irq = 0; pic_irq < PIIX_NUM_PIC_IRQS; pic_irq++) {
            piix3_set_irq_pic(piix3, pic_irq);
        }
    }
}

static void piix3_write_config_xen(PCIDevice *dev,
                               uint32_t address, uint32_t val, int len)
{
    xen_piix_pci_write_config_client(address, val, len);
    piix3_write_config(dev, address, val, len);
}

static void piix3_reset(DeviceState *dev)
{
    PIIX3State *d = PIIX3(dev);
    uint8_t *pci_conf = d->dev.config;

    pci_conf[0x04] = 0x07; // master, memory and I/O
    pci_conf[0x05] = 0x00;
    pci_conf[0x06] = 0x00;
    pci_conf[0x07] = 0x02; // PCI_status_devsel_medium
    pci_conf[0x4c] = 0x4d;
    pci_conf[0x4e] = 0x03;
    pci_conf[0x4f] = 0x00;
    pci_conf[0x60] = 0x80;
    pci_conf[0x61] = 0x80;
    pci_conf[0x62] = 0x80;
    pci_conf[0x63] = 0x80;
    pci_conf[0x69] = 0x02;
    pci_conf[0x70] = 0x80;
    pci_conf[0x76] = 0x0c;
    pci_conf[0x77] = 0x0c;
    pci_conf[0x78] = 0x02;
    pci_conf[0x79] = 0x00;
    pci_conf[0x80] = 0x00;
    pci_conf[0x82] = 0x00;
    pci_conf[0xa0] = 0x08;
    pci_conf[0xa2] = 0x00;
    pci_conf[0xa3] = 0x00;
    pci_conf[0xa4] = 0x00;
    pci_conf[0xa5] = 0x00;
    pci_conf[0xa6] = 0x00;
    pci_conf[0xa7] = 0x00;
    pci_conf[0xa8] = 0x0f;
    pci_conf[0xaa] = 0x00;
    pci_conf[0xab] = 0x00;
    pci_conf[0xac] = 0x00;
    pci_conf[0xae] = 0x00;

    d->pic_levels = 0;
}

static int piix3_post_load(void *opaque, int version_id)
{
    PIIX3State *piix3 = opaque;
    piix3_update_irq_levels(piix3);
    return 0;
}

static void piix3_pre_save(void *opaque)
{
    int i;
    PIIX3State *piix3 = opaque;

    for (i = 0; i < ARRAY_SIZE(piix3->pci_irq_levels_vmstate); i++) {
        piix3->pci_irq_levels_vmstate[i] =
            pci_bus_get_irq_level(piix3->dev.bus, i);
    }
}

static const VMStateDescription vmstate_piix3 = {
    .name = TYPE_PIIX3,
    .version_id = 3,
    .minimum_version_id = 2,
    .minimum_version_id_old = 2,
    .post_load = piix3_post_load,
    .pre_save = piix3_pre_save,
    .fields      = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, PIIX3State),
        VMSTATE_INT32_ARRAY_V(pci_irq_levels_vmstate, PIIX3State,
                              PIIX_NUM_PIRQS, 3),
        VMSTATE_END_OF_LIST()
    }
};

static int piix3_realize(PCIDevice *dev)
{
    PIIX3State *s = PIIX3(dev);
    qemu_irq rtc_irq;

    /* Initialize ISA Bus */
    s->bus = isa_bus_new(DEVICE(dev), pci_address_space_io(dev));
    isa_bus_irqs(s->bus, s->pic);

    /* Realize the RTC */
    qdev_set_parent_bus(DEVICE(&s->rtc), BUS(s->bus));
    qdev_init_nofail(DEVICE(&s->rtc));

    /* Realize HPET */
    if (s->hpet_enable) {
        int i;

        /* We need to introduce a proper IRQ and Memory QOM infrastructure
         * so that the HPET isn't a sysbus device */
        qdev_set_parent_bus(DEVICE(&s->hpet), sysbus_get_default());
        qdev_init_nofail(DEVICE(&s->hpet));

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->hpet), 0, HPET_BASE);
        for (i = 0; i < GSI_NUM_PINS; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->hpet), i, s->pic[i]);
        }

        rtc_irq = qdev_get_gpio_in(DEVICE(&s->hpet), 0);
    } else {
        isa_init_irq(ISA_DEVICE(&s->rtc), &rtc_irq, RTC_ISA_IRQ);
    }

    /* Setup the RTC IRQ */
    s->rtc.irq = rtc_irq;

    /* Realize the PIT */
    qdev_set_parent_bus(DEVICE(&s->pit), BUS(s->bus));
    qdev_init_nofail(DEVICE(&s->pit));

    /* FIXME this should be refactored */
    pcspk_init(ISA_DEVICE(&s->pit));

    return 0;
}

static void piix3_initfn(Object *obj)
{
    PIIX3State *s = PIIX3(obj);

    qdev_prop_set_uint32(DEVICE(s), "addr", PCI_DEVFN(1, 0));
    qdev_prop_set_bit(DEVICE(s), "multifunction", true);

    object_initialize(&s->hpet, TYPE_HPET);
    object_property_add_child(obj, "hpet", OBJECT(&s->hpet), NULL);

    object_initialize(&s->rtc, TYPE_RTC);
    object_property_add_child(obj, "rtc", OBJECT(&s->rtc), NULL);
    qdev_prop_set_int32(DEVICE(&s->rtc), "base_year", 2000);

    object_initialize(&s->pit, TYPE_PIT);
    object_property_add_child(obj, "pit", OBJECT(&s->pit), NULL);
    qdev_prop_set_uint32(DEVICE(&s->pit), "iobase", 0x40);
    qdev_prop_set_uint32(DEVICE(&s->pit), "irq", 0);
}

static void piix3_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    dc->desc        = "ISA bridge";
    dc->vmsd        = &vmstate_piix3;
    dc->no_user     = 1;
    dc->reset       = piix3_reset;
    k->no_hotplug   = 1;
    k->init         = piix3_realize;
    k->config_write = piix3_write_config;
    k->vendor_id    = PCI_VENDOR_ID_INTEL;
    k->device_id    = PCI_DEVICE_ID_INTEL_82371SB_0; // 82371SB PIIX3 PCI-to-ISA bridge (Step A1)
    k->class_id     = PCI_CLASS_BRIDGE_ISA;
}

static TypeInfo piix3_info = {
    .name          = TYPE_PIIX3,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PIIX3State),
    .instance_init = piix3_initfn,
    .class_init    = piix3_class_init,
};

static void piix3_xen_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->config_write = piix3_write_config_xen;
};

static TypeInfo piix3_xen_info = {
    .name          = "PIIX3-xen",
    .parent        = TYPE_PIIX3,
    .instance_size = sizeof(PIIX3State),
    .class_init    = piix3_xen_class_init,
};

static void register_devices(void)
{
    type_register_static(&piix3_info);
    type_register_static(&piix3_xen_info);
}
device_init(register_devices);
