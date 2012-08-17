#include "libqos/pci.h"

#include "hw/pci_regs.h"
#include <glib.h>

#include <stdio.h>

QPCIDevice *qpci_device_find(QPCIBus *bus, int devfn)
{
    QPCIDevice *dev;

    dev = g_malloc0(sizeof(*dev));
    dev->bus = bus;
    dev->devfn = devfn;

    if (qpci_config_readw(dev, PCI_VENDOR_ID) == 0xFFFF) {
        printf("vendor id is %x\n", qpci_config_readw(dev, PCI_VENDOR_ID));
        g_free(dev);
        return NULL;
    }

    return dev;
}

void qpci_device_enable(QPCIDevice *dev)
{
    uint16_t cmd;

    /* FIXME -- does this need to be a bus callout? */
    cmd = qpci_config_readw(dev, PCI_COMMAND);
    cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    qpci_config_writew(dev, PCI_COMMAND, cmd);
}

uint8_t qpci_config_readb(QPCIDevice *dev, uint8_t offset)
{
    return dev->bus->config_readb(dev->bus, dev->devfn, offset);
}

uint16_t qpci_config_readw(QPCIDevice *dev, uint8_t offset)
{
    return dev->bus->config_readw(dev->bus, dev->devfn, offset);
}

uint32_t qpci_config_readl(QPCIDevice *dev, uint8_t offset)
{
    return dev->bus->config_readl(dev->bus, dev->devfn, offset);
}


void qpci_config_writeb(QPCIDevice *dev, uint8_t offset, uint8_t value)
{
    dev->bus->config_writeb(dev->bus, dev->devfn, offset, value);
}

void qpci_config_writew(QPCIDevice *dev, uint8_t offset, uint16_t value)
{
    dev->bus->config_writew(dev->bus, dev->devfn, offset, value);
}

void qpci_config_writel(QPCIDevice *dev, uint8_t offset, uint32_t value)
{
    dev->bus->config_writew(dev->bus, dev->devfn, offset, value);
}


uint8_t qpci_io_readb(QPCIDevice *dev, void *data)
{
    return dev->bus->io_readb(dev->bus, data);
}

uint16_t qpci_io_readw(QPCIDevice *dev, void *data)
{
    return dev->bus->io_readw(dev->bus, data);
}

uint32_t qpci_io_readl(QPCIDevice *dev, void *data)
{
    return dev->bus->io_readl(dev->bus, data);
}


void qpci_io_writeb(QPCIDevice *dev, void *data, uint8_t value)
{
    dev->bus->io_writeb(dev->bus, data, value);
}

void qpci_io_writew(QPCIDevice *dev, void *data, uint16_t value)
{
    dev->bus->io_writew(dev->bus, data, value);
}

void qpci_io_writel(QPCIDevice *dev, void *data, uint32_t value)
{
    dev->bus->io_writel(dev->bus, data, value);
}

void *qpci_iomap(QPCIDevice *dev, int barno)
{
    return dev->bus->iomap(dev->bus, dev, barno);
}

void qpci_iounmap(QPCIDevice *dev, void *data)
{
    dev->bus->iounmap(dev->bus, data);
}


