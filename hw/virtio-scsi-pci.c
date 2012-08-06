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
#include "virtio-scsi.h"
#include "virtio-blk.h"

#define TYPE_VIRTIO_SCSI_PCI "virtio-scsi-pci"
#define VIRTIO_SCSI_PCI(obj) \
    OBJECT_CHECK(VirtIOSCSIPCI, (obj), TYPE_VIRTIO_SCSI_PCI)

typedef struct VirtIOSCSIPCI {
    VirtIOPCIProxy parent_obj;

    /*< private >*/
    VirtIOBlkConf blk;
    VirtIOSCSIConf scsi;
} VirtIOSCSIPCI;

static int virtio_scsi_init_pci(VirtIOPCIProxy *proxy)
{
    VirtIOSCSIPCI *vscsi = VIRTIO_SCSI_PCI(proxy);
    VirtIODevice *vdev;

    vdev = virtio_scsi_init(DEVICE(proxy), &vscsi->scsi);
    if (!vdev) {
        return -EINVAL;
    }

    vdev->nvectors = proxy->nvectors == DEV_NVECTORS_UNSPECIFIED
                                        ? vscsi->scsi.num_queues + 3
                                        : proxy->nvectors;

    proxy->vdev = vdev;
    return 0;
}

static void virtio_scsi_exit_pci(VirtIOPCIProxy *proxy)
{
    virtio_scsi_exit(proxy->vdev);
}

static Property virtio_scsi_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags, VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, DEV_NVECTORS_UNSPECIFIED),
    DEFINE_VIRTIO_SCSI_PROPERTIES(VirtIOSCSIPCI, parent_obj.host_features, scsi),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_scsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    VirtIOPCIProxyClass *p = VIRTIO_PCI_CLASS(klass);

    p->init = virtio_scsi_init_pci;
    p->exit = virtio_scsi_exit_pci;
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->device_id = PCI_DEVICE_ID_VIRTIO_SCSI;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_STORAGE_SCSI;
    dc->props = virtio_scsi_properties;
}

static TypeInfo virtio_scsi_info = {
    .name          = "virtio-scsi-pci",
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOPCIProxy),
    .class_init    = virtio_scsi_class_init,
};

static void register_types(void)
{
    type_register_static(&virtio_scsi_info);
}

type_init(register_types)
