/*
 * Virtio PCI Bindings
 *
 * Copyright IBM, Corp. 2007
 * Copyright (c) 2009 CodeSourcery
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Paul Brook        <paul@codesourcery.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "virtio.h"
#include "virtio-pci.h"

#define TYPE_VIRTIO_BALLOON_PCI "virtio-balloon-pci"

static int virtio_balloon_init_pci(VirtIOPCIProxy *proxy)
{
    VirtIODevice *vdev;

    if (proxy->class_code != PCI_CLASS_OTHERS &&
        proxy->class_code != PCI_CLASS_MEMORY_RAM) { /* qemu < 1.1 */
        proxy->class_code = PCI_CLASS_OTHERS;
    }

    vdev = virtio_balloon_init(DEVICE(proxy));
    if (!vdev) {
        return -1;
    }

    proxy->vdev = vdev;
    return 0;
}

static void virtio_balloon_exit_pci(VirtIOPCIProxy *proxy)
{
    virtio_balloon_exit(proxy->vdev);
}

static Property virtio_balloon_properties[] = {
    DEFINE_VIRTIO_COMMON_FEATURES(VirtIOPCIProxy, host_features),
    DEFINE_PROP_HEX32("class", VirtIOPCIProxy, class_code, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_balloon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    VirtIOPCIProxyClass *p = VIRTIO_PCI_CLASS(klass);

    p->init = virtio_balloon_init_pci;
    p->exit = virtio_balloon_exit_pci;
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->device_id = PCI_DEVICE_ID_VIRTIO_BALLOON;
    k->class_id = PCI_CLASS_OTHERS;
    dc->props = virtio_balloon_properties;
}

static TypeInfo virtio_balloon_info = {
    .name          = TYPE_VIRTIO_BALLOON_PCI,
    .parent        = TYPE_VIRTIO_PCI,
    .class_init    = virtio_balloon_class_init,
};

static void register_types(void)
{
    type_register_static(&virtio_balloon_info);
}

type_init(register_types)
