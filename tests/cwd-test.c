/*
 * QTest testcase for the MC146818 real-time clock
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include "libqtest.h"
#include "hw/pci_ids.h"
#include "hw/pci_regs.h"

#include <glib.h>

static uint32_t pci_config_read(uint8_t bus, uint8_t devfn,
                                uint8_t addr, int size)
{
    outl(0xcf8, (bus << 16) | (devfn << 8) | addr | (1u << 31));
    if (size == 1) {
        return inb(0xcfc);
    } else if (size == 2) {
        return inw(0xcfc);
    }
    return inl(0xcfc);
}

static void pci_config_write(uint8_t bus, uint8_t devfn,
                             uint32_t addr, int size, uint32_t value)
{
    outl(0xcf8, (bus << 16) | (devfn << 8) | addr | (1u << 31));
    if (size == 1) {
        outb(0xcfc, value);
    } else if (size == 2) {
        outw(0xcfc, value);
    } else {
        outl(0xcfc, value);
    }
}

static void cwd_probe(uint8_t bus, uint8_t devfn)
{
    uint32_t bar0 = 0xc0000;

    pci_config_write(bus, devfn, PCI_COMMAND, 2,
                     (PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    pci_config_write(bus, devfn, PCI_BASE_ADDRESS_0, 4, bar0);

    outb(bar0, 0x42);
}

static void basic_init(void)
{
    int slot;

    for (slot = 0; slot < 32; slot++) {
        uint8_t fn;

        for (fn = 0; fn < 8; fn++) {
            uint8_t devfn = (slot << 3) | fn;
            uint16_t device_id;
            uint16_t vendor_id;

            vendor_id = pci_config_read(0, devfn, PCI_VENDOR_ID, 2);
            device_id = pci_config_read(0, devfn, PCI_DEVICE_ID, 2);

            if (vendor_id == 0xFFFF || device_id == 0xFFFF) {
                break;
            }

            if (vendor_id == 0x1af4 && device_id == 0x0101) {
                cwd_probe(0, devfn);
                return;
            }
        }
    }

    g_assert_not_reached();
}

int main(int argc, char **argv)
{
    QTestState *s = NULL;
    int ret;

    g_test_init(&argc, &argv, NULL);

    s = qtest_start("-device cstl-watchdog");

    qtest_add_func("/basic/init", basic_init);

    ret = g_test_run();

    if (s) {
        qtest_quit(s);
    }

    return ret;
}
