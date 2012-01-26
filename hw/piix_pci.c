/*
 * QEMU i440FX/PIIX3 PCI Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
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
#include "pci.h"
#include "pci_host.h"
#include "isa.h"
#include "sysbus.h"
#include "range.h"
#include "xen.h"
#include "hpet_emul.h"
#include "mc146818rtc.h"
#include "i8254.h"

/*
 * I440FX chipset data sheet.
 * http://download.intel.com/design/chipsets/datashts/29054901.pdf
 *
 * The I440FX is a package that contains an integrated PCI Host controller,
 * memory controller, and is usually packaged with a PCI-ISA bus and super I/O
 * chipset.
 *
 * The "i440FX" device is the PCI host controller.  On function 0.0, there is a
 * memory controller called the Programmable Memory Controller (PMC).  On
 * function 1.0, there is the PCI-ISA bus/super I/O chip called the PIIX3.
 */

#define PIIX_NUM_PIC_IRQS       16      /* i8259 * 2 */
#define PIIX_NUM_PIRQS          4ULL    /* PIRQ[A-D] */
#define XEN_PIIX_NUM_PIRQS      128ULL
#define PIIX_PIRQC              0x60

#define I440FX_PMC_PCI_HOLE     0xE0000000ULL
#define I440FX_PMC_PCI_HOLE_END 0x100000000ULL

#define TYPE_PIIX3 "PIIX3"
#define PIIX3(obj) OBJECT_CHECK(PIIX3State, (obj), TYPE_PIIX3)

typedef struct PIIX3State {
    PCIDevice dev;

    /*
     * bitmap to track pic levels.
     * The pic level is the logical OR of all the PCI irqs mapped to it
     * So one PIC level is tracked by PIIX_NUM_PIRQS bits.
     *
     * PIRQ is mapped to PIC pins, we track it by
     * PIIX_NUM_PIRQS * PIIX_NUM_PIC_IRQS = 64 bits with
     * pic_irq * PIIX_NUM_PIRQS + pirq
     */
#if PIIX_NUM_PIC_IRQS * PIIX_NUM_PIRQS > 64
#error "unable to encode pic state in 64bit in pic_levels."
#endif
    uint64_t pic_levels;

    bool hpet_enable;

    HPETState hpet;
    RTCState rtc;
    PITState pit;

    ISABus *bus;

    qemu_irq *pic;

    /* This member isn't used. Just for save/load compatibility */
    int32_t pci_irq_levels_vmstate[PIIX_NUM_PIRQS];
} PIIX3State;

#define TYPE_I440FX_PMC "i440FX-PMC"
#define I440FX_PMC(obj) OBJECT_CHECK(I440FXPMCState, (obj), TYPE_I440FX_PMC)

typedef struct PAMMemoryRegion {
    MemoryRegion mem;
    bool initialized;
} PAMMemoryRegion;

struct I440FXPMCState {
    PCIDevice dev;
    MemoryRegion *system_memory;
    MemoryRegion *pci_address_space;
    MemoryRegion *ram_memory;
    MemoryRegion pci_hole;
    MemoryRegion pci_hole_64bit;
    PAMMemoryRegion pam_regions[13];
    MemoryRegion smram_region;
    uint8_t smm_enabled;
    ram_addr_t ram_size;
};

#define TYPE_I440FX "i440FX"
#define I440FX(obj) OBJECT_CHECK(I440FXState, (obj), TYPE_I440FX)

typedef struct I440FXState
{
    PCIHostState parent;

    MemoryRegion *address_space_io;
    MemoryRegion pci_address_space;

    I440FXPMCState pmc;
    PIIX3State piix3;
} I440FXState;

#define I440FX_PAM      0x59
#define I440FX_PAM_SIZE 7
#define I440FX_SMRAM    0x72

static void piix3_set_irq(void *opaque, int pirq, int level);
static void piix3_write_config_xen(PCIDevice *dev,
                               uint32_t address, uint32_t val, int len);

/* return the global irq number corresponding to a given device irq
   pin. We could also use the bus number to have a more precise
   mapping. */
static int pci_slot_get_pirq(PCIDevice *pci_dev, int pci_intx)
{
    int slot_addend;
    slot_addend = (pci_dev->devfn >> 3) - 1;
    return (pci_intx + slot_addend) & 3;
}

static void update_pam(I440FXPMCState *d, uint32_t start, uint32_t end, int r,
                       PAMMemoryRegion *mem)
{
    if (mem->initialized) {
        memory_region_del_subregion(d->system_memory, &mem->mem);
        memory_region_destroy(&mem->mem);
    }

    //    printf("ISA mapping %08x-0x%08x: %d\n", start, end, r);
    switch(r) {
    case 3:
        /* RAM */
        memory_region_init_alias(&mem->mem, "pam-ram", d->ram_memory,
                                 start, end - start);
        break;
    case 1:
        /* ROM (XXX: not quite correct) */
        memory_region_init_alias(&mem->mem, "pam-rom", d->ram_memory,
                                 start, end - start);
        memory_region_set_readonly(&mem->mem, true);
        break;
    case 2:
    case 0:
        /* XXX: should distinguish read/write cases */
        memory_region_init_alias(&mem->mem, "pam-pci", d->pci_address_space,
                                 start, end - start);
        break;
    }
    memory_region_add_subregion_overlap(d->system_memory,
                                        start, &mem->mem, 1);
    mem->initialized = true;
}

static void i440fx_pmc_update_memory_mappings(I440FXPMCState *d)
{
    int i, r;
    uint32_t smram;
    bool smram_enabled;

    memory_region_transaction_begin();
    update_pam(d, 0xf0000, 0x100000, (d->dev.config[I440FX_PAM] >> 4) & 3,
               &d->pam_regions[0]);
    for(i = 0; i < 12; i++) {
        r = (d->dev.config[(i >> 1) + (I440FX_PAM + 1)] >> ((i & 1) * 4)) & 3;
        update_pam(d, 0xc0000 + 0x4000 * i, 0xc0000 + 0x4000 * (i + 1), r,
                   &d->pam_regions[i+1]);
    }
    smram = d->dev.config[I440FX_SMRAM];
    smram_enabled = (d->smm_enabled && (smram & 0x08)) || (smram & 0x40);
    memory_region_set_enabled(&d->smram_region, !smram_enabled);
    memory_region_transaction_commit();
}

static void i440fx_pmc_set_smm(int val, void *arg)
{
    I440FXPMCState *d = arg;

    val = (val != 0);
    if (d->smm_enabled != val) {
        d->smm_enabled = val;
        i440fx_pmc_update_memory_mappings(d);
    }
}


static void i440fx_pmc_write_config(PCIDevice *dev,
                                    uint32_t address, uint32_t val, int len)
{
    I440FXPMCState *d = DO_UPCAST(I440FXPMCState, dev, dev);

    /* XXX: implement SMRAM.D_LOCK */
    pci_default_write_config(dev, address, val, len);
    if (ranges_overlap(address, len, I440FX_PAM, I440FX_PAM_SIZE) ||
        range_covers_byte(address, len, I440FX_SMRAM)) {
        i440fx_pmc_update_memory_mappings(d);
    }
}

static int i440fx_pmc_load_old(QEMUFile* f, void *opaque, int version_id)
{
    I440FXPMCState *d = opaque;
    int ret, i;

    ret = pci_device_load(&d->dev, f);
    if (ret < 0)
        return ret;
    i440fx_pmc_update_memory_mappings(d);
    qemu_get_8s(f, &d->smm_enabled);

    if (version_id == 2) {
        for (i = 0; i < PIIX_NUM_PIRQS; i++) {
            qemu_get_be32(f); /* dummy load for compatibility */
        }
    }

    return 0;
}

static int i440fx_pmc_post_load(void *opaque, int version_id)
{
    I440FXPMCState *d = opaque;

    i440fx_pmc_update_memory_mappings(d);
    return 0;
}

static const VMStateDescription vmstate_i440fx_pmc = {
    .name = "I440FX", /* this is wrong but we can't change it */
    .version_id = 3,
    .minimum_version_id = 3,
    .minimum_version_id_old = 1,
    .load_state_old = i440fx_pmc_load_old,
    .post_load = i440fx_pmc_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, I440FXPMCState),
        VMSTATE_UINT8(smm_enabled, I440FXPMCState),
        VMSTATE_END_OF_LIST()
    }
};

static int i440fx_realize(SysBusDevice *dev)
{
    I440FXState *s = I440FX(dev);
    PCIHostState *h = PCI_HOST(s);

    g_assert(h->address_space != NULL);
    g_assert(s->address_space_io != NULL);

    h->bus = pci_bus_new(DEVICE(s), NULL, &s->pci_address_space,
                         s->address_space_io, 0);

    memory_region_init_io(&h->conf_mem, &pci_host_conf_le_ops, s,
                          "pci-conf-idx", 4);
    sysbus_add_io(dev, 0xcf8, &h->conf_mem);
    sysbus_init_ioports(&h->busdev, 0xcf8, 4);

    memory_region_init_io(&h->data_mem, &pci_host_data_le_ops, s,
                          "pci-conf-data", 4);
    sysbus_add_io(dev, 0xcfc, &h->data_mem);
    sysbus_init_ioports(&h->busdev, 0xcfc, 4);

    s->pmc.system_memory = h->address_space;
    s->pmc.pci_address_space = &s->pci_address_space;

    qdev_set_parent_bus(DEVICE(&s->pmc), BUS(h->bus));
    qdev_init_nofail(DEVICE(&s->pmc));

    qdev_set_parent_bus(DEVICE(&s->piix3), BUS(h->bus));
    qdev_init_nofail(DEVICE(&s->piix3));

    if (xen_enabled()) {
        pci_bus_irqs(h->bus, xen_piix3_set_irq, xen_pci_slot_get_pirq,
                     &s->piix3, XEN_PIIX_NUM_PIRQS);
    } else {
        pci_bus_irqs(h->bus, piix3_set_irq, pci_slot_get_pirq, &s->piix3,
                PIIX_NUM_PIRQS);
    }

    return 0;
}

static void i440fx_initfn(Object *obj)
{
    I440FXState *s = I440FX(obj);

    object_initialize(&s->pmc, TYPE_I440FX_PMC);
    object_property_add_child(obj, "pmc", OBJECT(&s->pmc), NULL);
    qdev_prop_set_uint32(DEVICE(&s->pmc), "addr", PCI_DEVFN(0, 0));

    /* Xen supports additional interrupt routes from the PCI devices to
     * the IOAPIC: the four pins of each PCI device on the bus are also
     * connected to the IOAPIC directly.
     * These additional routes can be discovered through ACPI. */
    if (xen_enabled()) {
        object_initialize(&s->piix3, "PIIX3-xen");
    } else {
        object_initialize(&s->piix3, "PIIX3");
    }
    object_property_add_child(OBJECT(s), "piix3", OBJECT(&s->piix3), NULL);

    memory_region_init(&s->pci_address_space, "pci", INT64_MAX);
}

static int i440fx_pmc_initfn(PCIDevice *dev)
{
    I440FXPMCState *d = DO_UPCAST(I440FXPMCState, dev, dev);
    ram_addr_t ram_size;
    uint64_t pci_hole_start, pci_hole_size;
    uint64_t pci_hole64_start, pci_hole64_size;

    g_assert(d->ram_size != 0);
    g_assert(d->system_memory != NULL);
    g_assert(d->pci_address_space != NULL);
    g_assert(d->ram_memory != NULL);

    /* Calculate PCI geometry from RAM size */
    if (d->ram_size > I440FX_PMC_PCI_HOLE) {
        pci_hole_start = I440FX_PMC_PCI_HOLE;
    } else {
        pci_hole_start = d->ram_size;
    }
    pci_hole_size = I440FX_PMC_PCI_HOLE_END - pci_hole_start;

    pci_hole64_start = I440FX_PMC_PCI_HOLE_END + d->ram_size - pci_hole_start;
    if (sizeof(target_phys_addr_t) == 4) {
        pci_hole64_size = 0;
    } else {
        pci_hole64_size = (1ULL << 62);
    }

    memory_region_init_alias(&d->pci_hole, "pci-hole", d->pci_address_space,
                             pci_hole_start, pci_hole_size);
    memory_region_add_subregion(d->system_memory, pci_hole_start, &d->pci_hole);
    memory_region_init_alias(&d->pci_hole_64bit, "pci-hole64",
                             d->pci_address_space,
                             pci_hole64_start, pci_hole64_size);
    if (pci_hole64_size) {
        memory_region_add_subregion(d->system_memory, pci_hole64_start,
                                    &d->pci_hole_64bit);
    }
    memory_region_init_alias(&d->smram_region, "smram-region",
                             d->pci_address_space, 0xa0000, 0x20000);
    memory_region_add_subregion_overlap(d->system_memory, 0xa0000,
                                        &d->smram_region, 1);
    memory_region_set_enabled(&d->smram_region, false);

    ram_size = d->ram_size / 8 / 1024 / 1024;
    if (ram_size > 255)
        ram_size = 255;
    d->dev.config[0x57] = ram_size;

    i440fx_pmc_update_memory_mappings(d);

    d->dev.config[I440FX_SMRAM] = 0x02;

    cpu_smm_register(&i440fx_pmc_set_smm, d);
    return 0;
}

PCIBus *i440fx_init(I440FXPMCState **pi440fx_state, int *piix3_devfn,
                    ISABus **isa_bus, qemu_irq *pic,
                    MemoryRegion *address_space_mem,
                    MemoryRegion *address_space_io,
                    ram_addr_t ram_size,
                    MemoryRegion *ram_memory)

{
    I440FXState *s;
    PCIHostState *h;

    s = I440FX(object_new(TYPE_I440FX));
    h = PCI_HOST(s);

    /* FIXME make a properties */
    h->address_space = address_space_mem;
    s->address_space_io = address_space_io;
    s->piix3.pic = pic;
    /* FIXME pmc should create ram_memory */
    s->pmc.ram_memory = ram_memory;
    s->pmc.ram_size = ram_size;

    qdev_set_parent_bus(DEVICE(s), sysbus_get_default());
    qdev_init_nofail(DEVICE(s));

    object_property_add_child(object_get_root(), "i440fx", OBJECT(s), NULL);

    *isa_bus = s->piix3.bus;
    *pi440fx_state = &s->pmc;
    *piix3_devfn = s->piix3.dev.devfn;

    return h->bus;
}

/* PIIX3 PCI to ISA bridge */
static void piix3_set_irq_pic(PIIX3State *piix3, int pic_irq)
{
    qemu_set_irq(piix3->pic[pic_irq],
                 !!(piix3->pic_levels &
                    (((1ULL << PIIX_NUM_PIRQS) - 1) <<
                     (pic_irq * PIIX_NUM_PIRQS))));
}

static void piix3_set_irq_level(PIIX3State *piix3, int pirq, int level)
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

static void piix3_set_irq(void *opaque, int pirq, int level)
{
    PIIX3State *piix3 = opaque;
    piix3_set_irq_level(piix3, pirq, level);
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

static void i440fx_pmc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->no_hotplug = 1;
    k->init = i440fx_pmc_initfn;
    k->config_write = i440fx_pmc_write_config;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = PCI_DEVICE_ID_INTEL_82441;
    k->revision = 0x02;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "Host bridge";
    dc->no_user = 1;
    dc->vmsd = &vmstate_i440fx_pmc;
}

static TypeInfo i440fx_pmc_info = {
    .name          = TYPE_I440FX_PMC,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(I440FXPMCState),
    .class_init    = i440fx_pmc_class_init,
};

static void i440fx_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = i440fx_realize;
    dc->fw_name = "pci";
    dc->no_user = 1;
}

static TypeInfo i440fx_info = {
    .name          = TYPE_I440FX,
    .parent        = TYPE_PCI_HOST,
    .instance_size = sizeof(I440FXState),
    .instance_init = i440fx_initfn,
    .class_init    = i440fx_class_init,
};

static void register_devices(void)
{
    type_register_static(&i440fx_info);
    type_register_static(&i440fx_pmc_info);
    type_register_static(&piix3_info);
    type_register_static(&piix3_xen_info);
}
device_init(register_devices);
