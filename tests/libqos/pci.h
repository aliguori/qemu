#ifndef LIBQOS_PCI_H
#define LIBQOS_PCI_H

#include <stdint.h>

typedef struct QPCIDevice QPCIDevice;
typedef struct QPCIBus QPCIBus;

struct QPCIBus
{
    uint8_t (*io_readb)(QPCIBus *bus, void *addr);
    uint16_t (*io_readw)(QPCIBus *bus, void *addr);
    uint32_t (*io_readl)(QPCIBus *bus, void *addr);

    void (*io_writeb)(QPCIBus *bus, void *addr, uint8_t value);
    void (*io_writew)(QPCIBus *bus, void *addr, uint16_t value);
    void (*io_writel)(QPCIBus *bus, void *addr, uint32_t value);

    uint8_t (*config_readb)(QPCIBus *bus, int devfn, uint8_t offset);
    uint16_t (*config_readw)(QPCIBus *bus, int devfn, uint8_t offset);
    uint32_t (*config_readl)(QPCIBus *bus, int devfn, uint8_t offset);

    void (*config_writeb)(QPCIBus *bus, int devfn,
                          uint8_t offset, uint8_t value);
    void (*config_writew)(QPCIBus *bus, int devfn,
                          uint8_t offset, uint16_t value);
    void (*config_writel)(QPCIBus *bus, int devfn,
                          uint8_t offset, uint32_t value);

    void *(*iomap)(QPCIBus *bus, QPCIDevice *dev, int barno);
    void (*iounmap)(QPCIBus *bus, void *data);
};

struct QPCIDevice
{
    QPCIBus *bus;
    int devfn;
};

QPCIDevice *pci_device_find(QPCIBus *bus, int devfn);

void pci_device_enable(QPCIDevice *dev);

uint8_t qpci_config_readb(QPCIDevice *dev, uint8_t offset);
uint16_t qpci_config_readw(QPCIDevice *dev, uint8_t offset);
uint32_t qpci_config_readl(QPCIDevice *dev, uint8_t offset);

void qpci_config_writeb(QPCIDevice *dev, uint8_t offset, uint8_t value);
void qpci_config_writew(QPCIDevice *dev, uint8_t offset, uint16_t value);
void qpci_config_writel(QPCIDevice *dev, uint8_t offset, uint32_t value);

uint8_t qpci_io_readb(QPCIDevice *dev, void *data);
uint16_t qpci_io_readw(QPCIDevice *dev, void *data);
uint32_t qpci_io_readl(QPCIDevice *dev, void *data);

void qpci_io_writeb(QPCIDevice *dev, void *data, uint8_t value);
void qpci_io_writew(QPCIDevice *dev, void *data, uint16_t value);
void qpci_io_writel(QPCIDevice *dev, void *data, uint32_t value);

void *qpci_iomap(QPCIDevice *dev, int barno);
void qpci_iounmap(QPCIDevice *dev, void *data);

#endif
