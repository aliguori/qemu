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
#include "virtio-net.h"
#include "net.h"

#define TYPE_VIRTIO_NET_PCI "virtio-net-pci"
#define VIRTIO_NET_PCI(obj) \
    OBJECT_CHECK(VirtIONetPCI, (obj), TYPE_VIRTIO_NET_PCI)

typedef struct VirtIONetPCI {
    VirtIOPCIProxy parent_obj;

    NICConf nic;
    virtio_net_conf net;
} VirtIONetPCI;

static int virtio_net_init_pci(VirtIOPCIProxy *proxy)
{
    VirtIONetPCI *ndev = VIRTIO_NET_PCI(proxy);
    VirtIODevice *vdev;

    vdev = virtio_net_init(DEVICE(proxy), &ndev->nic, &ndev->net);
    vdev->nvectors = proxy->nvectors;
    proxy->vdev = vdev;
    return 0;
}

static void virtio_net_exit_pci(VirtIOPCIProxy *proxy)
{
    virtio_net_exit(proxy->vdev);
}

static Property virtio_net_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags, VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, false),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 3),
    DEFINE_VIRTIO_NET_FEATURES(VirtIOPCIProxy, host_features),
    DEFINE_NIC_PROPERTIES(VirtIONetPCI, nic),
    DEFINE_PROP_UINT32("x-txtimer", VirtIONetPCI, net.txtimer, TX_TIMER_INTERVAL),
    DEFINE_PROP_INT32("x-txburst", VirtIONetPCI, net.txburst, TX_BURST),
    DEFINE_PROP_STRING("tx", VirtIONetPCI, net.tx),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_net_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    VirtIOPCIProxyClass *p = VIRTIO_PCI_CLASS(klass);

    p->init = virtio_net_init_pci;
    p->exit = virtio_net_exit_pci;
    k->romfile = "pxe-virtio.rom";
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->device_id = PCI_DEVICE_ID_VIRTIO_NET;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    dc->reset = virtio_pci_reset;
    dc->props = virtio_net_properties;
}

static TypeInfo virtio_net_info = {
    .name          = TYPE_VIRTIO_NET_PCI,
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOPCIProxy),
    .class_init    = virtio_net_class_init,
};

static void register_types(void)
{
    type_register_static(&virtio_net_info);
}

type_init(register_types)
