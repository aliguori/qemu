#include "hw/virtio/virtio-pci.h"
#include "hw/display/virtio-fb.h"

#define TYPE_VIRTIO_FB_PCI "virtio-fb-pci"
#define VIRTIO_FB_PCI(obj) OBJECT_CHECK(VirtioFBPCI, (obj), TYPE_VIRTIO_FB_PCI)

typedef struct VirtioFBPCI
{
    VirtIOPCIProxy parent;
    VirtioFB fb;
} VirtioFBPCI;

static int virtio_fb_pci_init(VirtIOPCIProxy *vdev)
{
    VirtioFBPCI *s = VIRTIO_FB_PCI(vdev);

    qdev_set_parent_bus(DEVICE(&s->fb), BUS(&vdev->bus));
    if (qdev_init(DEVICE(&s->fb)) < 0) {
        return -1;
    }
    return 0;
}

static void virtio_fb_pci_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    VirtioPCIClass *vpc = VIRTIO_PCI_CLASS(klass);

    pc->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pc->device_id = 0x100A;
    pc->revision = VIRTIO_PCI_ABI_VERSION;
    pc->class_id = PCI_CLASS_DISPLAY_OTHER;
    vpc->init = virtio_fb_pci_init;
}

static void virtio_fb_pci_initfn(Object *obj)
{
    VirtioFBPCI *s = VIRTIO_FB_PCI(obj);

    object_initialize(OBJECT(&s->fb), TYPE_VIRTIO_FB);
    object_property_add_child(obj, "virtio-backend", OBJECT(&s->fb), NULL);
}

static const TypeInfo virtio_fb_pci_info = {
    .name          = TYPE_VIRTIO_FB_PCI,
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtioFBPCI),
    .instance_init = virtio_fb_pci_initfn,
    .class_init    = virtio_fb_pci_class_init,
};

static void register_types(void)
{
    type_register_static(&virtio_fb_pci_info);
}

type_init(register_types);

