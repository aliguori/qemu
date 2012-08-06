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

#include "virtio-pci.h"
#include "virtio-blk.h"
#include "blockdev.h"

#define TYPE_VIRTIO_BLK_PCI "virtio-blk-pci"
#define VIRTIO_BLK_PCI(obj) \
    OBJECT_CHECK(VirtIOBlkPCI, (obj), TYPE_VIRTIO_BLK_PCI)

typedef struct VirtIOBlkPCI {
    VirtIOPCIProxy proxy;

    VirtIOBlkConf blk;
} VirtIOBlkPCI;

static int virtio_blk_init_pci(VirtIOPCIProxy *proxy)
{
    VirtIOBlkPCI *bdev = VIRTIO_BLK_PCI(proxy);
    VirtIODevice *vdev;

    if (proxy->class_code != PCI_CLASS_STORAGE_SCSI &&
        proxy->class_code != PCI_CLASS_STORAGE_OTHER) {
        proxy->class_code = PCI_CLASS_STORAGE_SCSI;
    }

    vdev = virtio_blk_init(DEVICE(proxy), &bdev->blk);
    if (!vdev) {
        return -1;
    }
    vdev->nvectors = proxy->nvectors;
    proxy->vdev = vdev;
    return 0;
}

static void virtio_blk_exit_pci(VirtIOPCIProxy *proxy)
{
    virtio_blk_exit(proxy->vdev);
}

static Property virtio_blk_properties[] = {
    DEFINE_PROP_HEX32("class", VirtIOPCIProxy, class_code, 0),
    DEFINE_BLOCK_PROPERTIES(VirtIOBlkPCI, blk.conf),
    DEFINE_BLOCK_CHS_PROPERTIES(VirtIOBlkPCI, blk.conf),
    DEFINE_PROP_STRING("serial", VirtIOBlkPCI, blk.serial),
#ifdef __linux__
    DEFINE_PROP_BIT("scsi", VirtIOBlkPCI, blk.scsi, 0, true),
#endif
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags, VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 2),
    DEFINE_VIRTIO_BLK_FEATURES(VirtIOPCIProxy, host_features),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_blk_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    VirtIOPCIProxyClass *p = VIRTIO_PCI_CLASS(klass);

    p->init = virtio_blk_init_pci;
    p->exit = virtio_blk_exit_pci;
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->device_id = PCI_DEVICE_ID_VIRTIO_BLOCK;
    k->class_id = PCI_CLASS_STORAGE_SCSI;
    dc->props = virtio_blk_properties;
}

static TypeInfo virtio_blk_info = {
    .name          = "virtio-blk-pci",
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOBlkPCI),
    .class_init    = virtio_blk_class_init,
};

static void register_types(void)
{
    type_register_static(&virtio_blk_info);
}

type_init(register_types)
