#include "hw/virtio/virtio-pci.h"
#include "ui/console.h"
/* FIXME this should include ^ */
#include "hw/display/vga_int.h"

/* FIXME move to common header */
#define PCI_VGA_IOPORT_OFFSET 0x400
#define PCI_VGA_IOPORT_SIZE   (0x3e0 - 0x3c0)
#define PCI_VGA_BOCHS_OFFSET  0x500
#define PCI_VGA_BOCHS_SIZE    (0x0b * 2)
#define PCI_VGA_MMIO_SIZE     0x1000

#define TYPE_VIRTIO_VGA "virtio-vga"
#define VIRTIO_VGA(obj) OBJECT_CHECK(VirtioVGA, (obj), TYPE_VIRTIO_VGA)

typedef struct VirtioVGA
{
    VirtIOPCIProxy parent;
    VGACommonState vga;

    MemoryRegion mmio;
    MemoryRegion ioport;
    MemoryRegion bochs;
} VirtioVGA;

static uint64_t virtio_vga_bochs_read(void *ptr, hwaddr addr,
                                      unsigned size)
{
    VirtioVGA *s = VIRTIO_VGA(ptr);
    int index = addr >> 1;

    vbe_ioport_write_index(&s->vga, 0, index);
    return vbe_ioport_read_data(&s->vga, 0);
}

static void virtio_vga_bochs_write(void *ptr, hwaddr addr,
                                   uint64_t val, unsigned size)
{
    VirtioVGA *s = VIRTIO_VGA(ptr);
    int index = addr >> 1;

    vbe_ioport_write_index(&s->vga, 0, index);
    vbe_ioport_write_data(&s->vga, 0, val);
}

static const MemoryRegionOps virtio_vga_bochs_ops = {
    .read = virtio_vga_bochs_read,
    .write = virtio_vga_bochs_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t virtio_vga_ioport_read(void *ptr, hwaddr addr,
                                       unsigned size)
{
    VirtioVGA *s = VIRTIO_VGA(ptr);
    uint64_t ret = 0;

    switch (size) {
    case 1:
        ret = vga_ioport_read(&s->vga, addr);
        break;
    case 2:
        ret  = vga_ioport_read(&s->vga, addr);
        ret |= vga_ioport_read(&s->vga, addr+1) << 8;
        break;
    }
    return ret;
}

static void virtio_vga_ioport_write(void *ptr, hwaddr addr,
                                    uint64_t val, unsigned size)
{
    VirtioVGA *s = VIRTIO_VGA(ptr);

    switch (size) {
    case 1:
        vga_ioport_write(&s->vga, addr + 0x3c0, val);
        break;
    case 2:
        /*
         * Update bytes in little endian order.  Allows to update
         * indexed registers with a single word write because the
         * index byte is updated first.
         */
        vga_ioport_write(&s->vga, addr + 0x3c0, val & 0xff);
        vga_ioport_write(&s->vga, addr + 0x3c1, (val >> 8) & 0xff);
        break;
    }
}

static const MemoryRegionOps virtio_vga_ioport_ops = {
    .read = virtio_vga_ioport_read,
    .write = virtio_vga_ioport_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static int virtio_vga_realize(VirtIOPCIProxy *vdev)
{
    VirtioVGA *s = VIRTIO_VGA(vdev);
    PCIDevice *pcidev = PCI_DEVICE(s);
    VGACommonState *vga = &s->vga;

    vga_common_init(vga);
    vga_init(vga, pci_address_space(pcidev),
             pci_address_space_io(pcidev), true);
    vga->con = graphic_console_init(DEVICE(pcidev), vga->hw_ops, vga);

    memory_region_init(&s->mmio, "virtio-vga.mmio", 4096);
    memory_region_init_io(&s->ioport, &virtio_vga_ioport_ops, s,
                          "vga ioports remapped", PCI_VGA_IOPORT_SIZE);
    memory_region_init_io(&s->bochs, &virtio_vga_bochs_ops, s,
                          "bochs dispi remapped", PCI_VGA_BOCHS_SIZE);

    memory_region_add_subregion(&s->mmio, PCI_VGA_IOPORT_OFFSET, &s->ioport);
    memory_region_add_subregion(&s->mmio, PCI_VGA_BOCHS_OFFSET, &s->bochs);

    pci_register_bar(pcidev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    pci_register_bar(pcidev, 3, PCI_BASE_ADDRESS_MEM_PREFETCH, &vga->vram);

    return 0;
}

static void virtio_vga_class_init(ObjectClass *klass, void *data)
{
    VirtioPCIClass *vpc = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pc->device_id = 0x100A;
    pc->class_id = PCI_CLASS_DISPLAY_VGA;
    pc->revision = 0x00;
    pc->no_hotplug = 1;
    pc->romfile = "vgabios-virtio.bin";

    vpc->init = virtio_vga_realize;
}

static TypeInfo virtio_vga_info = {
    .name = TYPE_VIRTIO_VGA,
    .parent = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtioVGA),
    .class_init = virtio_vga_class_init,
};

static void register_types(void)
{
    type_register_static(&virtio_vga_info);
}

type_init(register_types);
