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
#include "virtio-serial.h"
#include "virtio-pci.h"

#define TYPE_VIRTIO_SERIAL_PCI "virtio-serial-pci"
#define VIRTIO_SERIAL_PCI(obj) \
    OBJECT_CHECK(VirtIOSerialPCI, (obj), TYPE_VIRTIO_SERIAL_PCI)

typedef struct VirtIOSerialPCI {
    VirtIOPCIProxy parent_obj;

    virtio_serial_conf serial;
} VirtIOSerialPCI;

static int virtio_serial_init_pci(VirtIOPCIProxy *proxy)
{
    VirtIOSerialPCI *sdev = VIRTIO_SERIAL_PCI(proxy);
    VirtIODevice *vdev;

    if (proxy->class_code != PCI_CLASS_COMMUNICATION_OTHER &&
        proxy->class_code != PCI_CLASS_DISPLAY_OTHER && /* qemu 0.10 */
        proxy->class_code != PCI_CLASS_OTHERS)          /* qemu-kvm  */
        proxy->class_code = PCI_CLASS_COMMUNICATION_OTHER;

    vdev = virtio_serial_init(DEVICE(proxy), &sdev->serial);
    if (!vdev) {
        return -1;
    }
    vdev->nvectors = proxy->nvectors == DEV_NVECTORS_UNSPECIFIED
                                        ? sdev->serial.max_virtserial_ports + 1
                                        : proxy->nvectors;
    proxy->vdev = vdev;
    return 0;
}

static void virtio_serial_exit_pci(VirtIOPCIProxy *proxy)
{
    virtio_serial_exit(proxy->vdev);
}

static Property virtio_serial_properties[] = {
    DEFINE_PROP_BIT("ioeventfd", VirtIOPCIProxy, flags, VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT, true),
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, DEV_NVECTORS_UNSPECIFIED),
    DEFINE_PROP_HEX32("class", VirtIOPCIProxy, class_code, 0),
    DEFINE_VIRTIO_COMMON_FEATURES(VirtIOPCIProxy, host_features),
    DEFINE_PROP_UINT32("max_ports", VirtIOSerialPCI, serial.max_virtserial_ports, 31),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_serial_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    VirtIOPCIProxyClass *p = VIRTIO_PCI_CLASS(klass);

    p->init = virtio_serial_init_pci;
    p->exit = virtio_serial_exit_pci;
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->device_id = PCI_DEVICE_ID_VIRTIO_CONSOLE;
    k->class_id = PCI_CLASS_COMMUNICATION_OTHER;
    dc->props = virtio_serial_properties;
}

static TypeInfo virtio_serial_info = {
    .name          = "virtio-serial-pci",
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOSerialPCI),
    .class_init    = virtio_serial_class_init,
};

static void register_types(void)
{
    type_register_static(&virtio_serial_info);
}

type_init(register_types)
