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
 */

#ifndef QEMU_VIRTIO_PCI_H
#define QEMU_VIRTIO_PCI_H

#include "virtio.h"
#include "pci.h"
#include "virtio-blk.h"

/* Performance improves when virtqueue kick processing is decoupled from the
 * vcpu thread using ioeventfd for some devices. */
#define VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT 1
#define VIRTIO_PCI_FLAG_USE_IOEVENTFD   (1 << VIRTIO_PCI_FLAG_USE_IOEVENTFD_BIT)

#define TYPE_VIRTIO_PCI "virtio-pci"
#define VIRTIO_PCI(obj) \
    OBJECT_CHECK(VirtIOPCIProxy, (obj), TYPE_VIRTIO_PCI)
#define VIRTIO_PCI_CLASS(klass) \
    OBJECT_CLASS_CHECK(VirtIOPCIProxyClass, (klass), TYPE_VIRTIO_PCI)
#define VIRTIO_PCI_GET_CLASS(obj) \
    OBJECT_GET_CLASS(VirtIOPCIProxyClass, (obj), TYPE_VIRTIO_PCI)

typedef struct VirtIOPCIProxy VirtIOPCIProxy;
typedef struct VirtIOPCIProxyClass VirtIOPCIProxyClass;

typedef struct {
    int virq;
    unsigned int users;
} VirtIOIRQFD;

struct VirtIOPCIProxyClass {
    PCIDeviceClass *parent_class;
    int (*init)(VirtIOPCIProxy *dev);
    void (*exit)(VirtIOPCIProxy *dev);
};

struct VirtIOPCIProxy {
    PCIDevice pci_dev;
    VirtIODevice *vdev;
    MemoryRegion bar;
    uint32_t flags;
    uint32_t class_code;
    uint32_t nvectors;
    VirtIOBlkConf blk;
    uint32_t host_features;
#ifdef CONFIG_LINUX
    V9fsConf fsconf;
#endif
    bool ioeventfd_disabled;
    bool ioeventfd_started;
    VirtIOIRQFD *vector_irqfd;
};

int virtio_init_pci(VirtIOPCIProxy *proxy, VirtIODevice *vdev);
void virtio_pci_reset(DeviceState *d);

/* Virtio ABI version, if we increment this, we break the guest driver. */
#define VIRTIO_PCI_ABI_VERSION          0

#endif
