/*
 * QEMU PIIX3 PCI-ISA Bridge Emulation
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori <aliguori@us.ibm.com>
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

#ifndef QEMU_PIIX3_H
#define QEMU_PIIX3_H

#include "pci.h"
#include "hpet_emul.h"
#include "mc146818rtc.h"
#include "i8254.h"

#define PIIX_NUM_PIC_IRQS       16      /* i8259 * 2 */
#define PIIX_NUM_PIRQS          4ULL    /* PIRQ[A-D] */
#define XEN_PIIX_NUM_PIRQS      128ULL
#define PIIX_PIRQC              0x60

#define TYPE_PIIX3 "PIIX3"
#define PIIX3(obj) OBJECT_CHECK(PIIX3State, (obj), TYPE_PIIX3)

typedef struct PIIX3State {
    PCIDevice dev;

    /*
     * bitmap to track pic levels.
     * The pic level is the logical OR of all the PCI irqs mapped to it
     * So one PIC level is tracked by PIIX_NUM_PIRQS bits.
     *
     * PIRQ is mapped to PIC pins, we track it by
     * PIIX_NUM_PIRQS * PIIX_NUM_PIC_IRQS = 64 bits with
     * pic_irq * PIIX_NUM_PIRQS + pirq
     */
#if PIIX_NUM_PIC_IRQS * PIIX_NUM_PIRQS > 64
#error "unable to encode pic state in 64bit in pic_levels."
#endif
    uint64_t pic_levels;

    bool hpet_enable;

    HPETState hpet;
    RTCState rtc;
    PITState pit;

    ISABus *bus;

    qemu_irq *pic;

    /* This member isn't used. Just for save/load compatibility */
    int32_t pci_irq_levels_vmstate[PIIX_NUM_PIRQS];
} PIIX3State;

void piix3_set_irq_level(PIIX3State *piix3, int pirq, int level);

#endif
