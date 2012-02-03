/*
 * QEMU i440FX PCI Host Bridge Emulation
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

#include "i440fx.h"
#include "range.h"
#include "xen.h"
#include "loader.h"
#include "sysemu.h"
#include "pc.h"

#define BIOS_FILENAME "bios.bin"

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

#define I440FX_PMC_PCI_HOLE     0xE0000000ULL
#define I440FX_PMC_PCI_HOLE_END 0x100000000ULL

#define I440FX_PAM      0x59
#define I440FX_PAM_SIZE 7
#define I440FX_SMRAM    0x72

static void piix3_set_irq(void *opaque, int pirq, int level)
{
    PIIX3State *piix3 = opaque;
    piix3_set_irq_level(piix3, pirq, level);
}

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
    int bios_size, isa_bios_size;
    char *filename;
    int ret;

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

    /* BIOS load */
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, s->bios_name);
    if (filename) {
        bios_size = get_image_size(filename);
    } else {
        bios_size = -1;
    }
    if (bios_size <= 0 ||
        (bios_size % 65536) != 0) {
        goto bios_error;
    }
    memory_region_init_ram(&s->bios, "pc.bios", bios_size);
    vmstate_register_ram_global(&s->bios);
    memory_region_set_readonly(&s->bios, true);
    ret = rom_add_file_fixed(s->bios_name, (uint32_t)(-bios_size), -1);
    if (ret != 0) {
    bios_error:
        fprintf(stderr, "qemu: could not load PC BIOS '%s'\n", s->bios_name);
        exit(1);
    }
    if (filename) {
        g_free(filename);
    }

    /* map the last 128KB of the BIOS in ISA space */
    isa_bios_size = bios_size;
    if (isa_bios_size > (128 * 1024)) {
        isa_bios_size = 128 * 1024;
    }
    memory_region_init_alias(&s->isa_bios, "isa-bios", &s->bios,
                             bios_size - isa_bios_size, isa_bios_size);
    memory_region_add_subregion_overlap(&s->pci_address_space,
                                        0x100000 - isa_bios_size,
                                        &s->isa_bios,
                                        1);
    memory_region_set_readonly(&s->isa_bios, true);

    memory_region_init_ram(&s->option_roms, "pc.rom", PC_ROM_SIZE);
    vmstate_register_ram_global(&s->option_roms);
    memory_region_add_subregion_overlap(&s->pci_address_space,
                                        PC_ROM_MIN_VGA,
                                        &s->option_roms,
                                        1);

    /* map all the bios at the top of memory */
    memory_region_add_subregion(&s->pci_address_space,
                                (uint32_t)(-bios_size),
                                &s->bios);

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

    s->bios_name = g_strdup(BIOS_FILENAME);

    memory_region_init(&s->pci_address_space, "pci", INT64_MAX);
}

static int i440fx_pmc_realize(PCIDevice *dev)
{
    I440FXPMCState *d = DO_UPCAST(I440FXPMCState, dev, dev);
    ram_addr_t ram_size;
    uint64_t below_4g_mem_size, above_4g_mem_size;
    uint64_t pci_hole_start, pci_hole_size;
    uint64_t pci_hole64_start, pci_hole64_size;

    g_assert(d->ram_size != 0);
    g_assert(d->system_memory != NULL);
    g_assert(d->pci_address_space != NULL);

    /* Calculate memory geometry from RAM size */
    if (d->ram_size > I440FX_PMC_PCI_HOLE) {
        below_4g_mem_size = I440FX_PMC_PCI_HOLE;
        above_4g_mem_size = d->ram_size - I440FX_PMC_PCI_HOLE;
    } else {
        below_4g_mem_size = d->ram_size;
        above_4g_mem_size = 0;
    }

    /* Allocate RAM.  We allocate it as a single memory region and use
     * aliases to address portions of it, mostly for backwards compatibility
     * with older qemus that used qemu_ram_alloc().
     */
    memory_region_init_ram(&d->ram, "pc.ram",
                           below_4g_mem_size + above_4g_mem_size);
    vmstate_register_ram_global(&d->ram);

    memory_region_init_alias(&d->ram_below_4g, "ram-below-4g", &d->ram,
                             0, below_4g_mem_size);
    memory_region_add_subregion(d->system_memory, 0, &d->ram_below_4g);
    if (above_4g_mem_size > 0) {
        memory_region_init_alias(&d->ram_above_4g, "ram-above-4g", &d->ram,
                                 below_4g_mem_size, above_4g_mem_size);
        memory_region_add_subregion(d->system_memory, 0x100000000ULL,
                                    &d->ram_above_4g);
    }

    pci_hole_start = below_4g_mem_size;
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

static void i440fx_pmc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->no_hotplug = 1;
    k->init = i440fx_pmc_realize;
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
}
device_init(register_devices);
