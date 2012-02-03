/*
 * QEMU i440FX PCI Host Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef QEMU_I440FX_H
#define QEMU_I440FX_H

#include "pci_host.h"
#include "piix3.h"

#define TYPE_I440FX_PMC "i440FX-PMC"
#define I440FX_PMC(obj) OBJECT_CHECK(I440FXPMCState, (obj), TYPE_I440FX_PMC)

typedef struct PAMMemoryRegion {
    MemoryRegion mem;
    bool initialized;
} PAMMemoryRegion;

typedef struct I440FXPMCState {
    PCIDevice dev;
    MemoryRegion *system_memory;
    MemoryRegion *pci_address_space;
    MemoryRegion *ram_memory;
    MemoryRegion pci_hole;
    MemoryRegion pci_hole_64bit;
    PAMMemoryRegion pam_regions[13];
    MemoryRegion smram_region;
    uint8_t smm_enabled;

    ram_addr_t ram_size;
    MemoryRegion ram;
    MemoryRegion ram_below_4g;
    MemoryRegion ram_above_4g;
} I440FXPMCState;

#define TYPE_I440FX "i440FX"
#define I440FX(obj) OBJECT_CHECK(I440FXState, (obj), TYPE_I440FX)

typedef struct I440FXState
{
    PCIHostState parent;

    MemoryRegion *address_space_io;
    MemoryRegion pci_address_space;

    I440FXPMCState pmc;
    PIIX3State piix3;

    /* Is this more appropriate for the PMC? */
    MemoryRegion bios;
    MemoryRegion isa_bios;
    MemoryRegion option_roms;

    char *bios_name;
} I440FXState;

#endif
