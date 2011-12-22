/*
 *  ioapic.c IOAPIC emulation logic
 *
 *  Copyright (c) 2004-2005 Fabrice Bellard
 *
 *  Split the ioapic logic from apic.c
 *  Xiantao Zhang <xiantao.zhang@intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "hw.h"
#include "pc.h"
#include "apic.h"
#include "ioapic.h"
#include "qemu-timer.h"
#include "host-utils.h"
#include "sysbus.h"

//#define DEBUG_IOAPIC

#ifdef DEBUG_IOAPIC
#define DPRINTF(fmt, ...)                                       \
    do { printf("ioapic: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

#define MAX_IOAPICS                     1

#define IOAPIC_VERSION                  0x11

#define IOAPIC_LVT_DEST_SHIFT           56
#define IOAPIC_LVT_MASKED_SHIFT         16
#define IOAPIC_LVT_TRIGGER_MODE_SHIFT   15
#define IOAPIC_LVT_REMOTE_IRR_SHIFT     14
#define IOAPIC_LVT_POLARITY_SHIFT       13
#define IOAPIC_LVT_DELIV_STATUS_SHIFT   12
#define IOAPIC_LVT_DEST_MODE_SHIFT      11
#define IOAPIC_LVT_DELIV_MODE_SHIFT     8

#define IOAPIC_LVT_MASKED               (1 << IOAPIC_LVT_MASKED_SHIFT)
#define IOAPIC_LVT_REMOTE_IRR           (1 << IOAPIC_LVT_REMOTE_IRR_SHIFT)

#define IOAPIC_TRIGGER_EDGE             0
#define IOAPIC_TRIGGER_LEVEL            1

/*io{apic,sapic} delivery mode*/
#define IOAPIC_DM_FIXED                 0x0
#define IOAPIC_DM_LOWEST_PRIORITY       0x1
#define IOAPIC_DM_PMI                   0x2
#define IOAPIC_DM_NMI                   0x4
#define IOAPIC_DM_INIT                  0x5
#define IOAPIC_DM_SIPI                  0x6
#define IOAPIC_DM_EXTINT                0x7
#define IOAPIC_DM_MASK                  0x7

#define IOAPIC_VECTOR_MASK              0xff

#define IOAPIC_IOREGSEL                 0x00
#define IOAPIC_IOWIN                    0x10

#define IOAPIC_REG_ID                   0x00
#define IOAPIC_REG_VER                  0x01
#define IOAPIC_REG_ARB                  0x02
#define IOAPIC_REG_REDTBL_BASE          0x10
#define IOAPIC_ID                       0x00

#define IOAPIC_ID_SHIFT                 24
#define IOAPIC_ID_MASK                  0xf

#define IOAPIC_VER_ENTRIES_SHIFT        16

typedef struct IOAPICState IOAPICState;

struct IOAPICState {
    SysBusDevice busdev;
    MemoryRegion io_memory;
    uint8_t id;
    uint8_t ioregsel;
    uint32_t irr;
    uint64_t ioredtbl[IOAPIC_NUM_PINS];
};

static IOAPICState *ioapics[MAX_IOAPICS];

static void ioapic_service(IOAPICState *s)
{
    uint8_t i;
    uint8_t trig_mode;
    uint8_t vector;
    uint8_t delivery_mode;
    uint32_t mask;
    uint64_t entry;
    uint8_t dest;
    uint8_t dest_mode;

    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        mask = 1 << i;
        if (s->irr & mask) {
            entry = s->ioredtbl[i];
            if (!(entry & IOAPIC_LVT_MASKED)) {
                trig_mode = ((entry >> IOAPIC_LVT_TRIGGER_MODE_SHIFT) & 1);
                dest = entry >> IOAPIC_LVT_DEST_SHIFT;
                dest_mode = (entry >> IOAPIC_LVT_DEST_MODE_SHIFT) & 1;
                delivery_mode =
                    (entry >> IOAPIC_LVT_DELIV_MODE_SHIFT) & IOAPIC_DM_MASK;
                if (trig_mode == IOAPIC_TRIGGER_EDGE) {
                    s->irr &= ~mask;
                } else {
                    s->ioredtbl[i] |= IOAPIC_LVT_REMOTE_IRR;
                }
                if (delivery_mode == IOAPIC_DM_EXTINT) {
                    vector = pic_read_irq(isa_pic);
                } else {
                    vector = entry & IOAPIC_VECTOR_MASK;
                }
                apic_deliver_irq(dest, dest_mode, delivery_mode,
                                 vector, trig_mode);
            }
        }
    }
}

static void ioapic_set_irq(void *opaque, int vector, int level)
{
    IOAPICState *s = opaque;

    /* ISA IRQs map to GSI 1-1 except for IRQ0 which maps
     * to GSI 2.  GSI maps to ioapic 1-1.  This is not
     * the cleanest way of doing it but it should work. */

    DPRINTF("%s: %s vec %x\n", __func__, level ? "raise" : "lower", vector);
    if (vector == 0) {
        vector = 2;
    }
    if (vector >= 0 && vector < IOAPIC_NUM_PINS) {
        uint32_t mask = 1 << vector;
        uint64_t entry = s->ioredtbl[vector];

        if (entry & (1 << IOAPIC_LVT_POLARITY_SHIFT)) {
            level = !level;
        }
        if (((entry >> IOAPIC_LVT_TRIGGER_MODE_SHIFT) & 1) ==
            IOAPIC_TRIGGER_LEVEL) {
            /* level triggered */
            if (level) {
                s->irr |= mask;
                ioapic_service(s);
            } else {
                s->irr &= ~mask;
            }
        } else {
            /* According to the 82093AA manual, we must ignore edge requests
             * if the input pin is masked. */
            if (level && !(entry & IOAPIC_LVT_MASKED)) {
                s->irr |= mask;
                ioapic_service(s);
            }
        }
    }
}

void ioapic_eoi_broadcast(int vector)
{
    IOAPICState *s;
    uint64_t entry;
    int i, n;

    for (i = 0; i < MAX_IOAPICS; i++) {
        s = ioapics[i];
        if (!s) {
            continue;
        }
        for (n = 0; n < IOAPIC_NUM_PINS; n++) {
            entry = s->ioredtbl[n];
            if ((entry & IOAPIC_LVT_REMOTE_IRR)
                && (entry & IOAPIC_VECTOR_MASK) == vector) {
                s->ioredtbl[n] = entry & ~IOAPIC_LVT_REMOTE_IRR;
                if (!(entry & IOAPIC_LVT_MASKED) && (s->irr & (1 << n))) {
                    ioapic_service(s);
                }
            }
        }
    }
}

static uint64_t
ioapic_mem_read(void *opaque, target_phys_addr_t addr, unsigned int size)
{
    IOAPICState *s = opaque;
    int index;
    uint32_t val = 0;

    switch (addr & 0xff) {
    case IOAPIC_IOREGSEL:
        val = s->ioregsel;
        break;
    case IOAPIC_IOWIN:
        if (size != 4) {
            break;
        }
        switch (s->ioregsel) {
        case IOAPIC_REG_ID:
            val = s->id << IOAPIC_ID_SHIFT;
            break;
        case IOAPIC_REG_VER:
            val = IOAPIC_VERSION |
                ((IOAPIC_NUM_PINS - 1) << IOAPIC_VER_ENTRIES_SHIFT);
            break;
        case IOAPIC_REG_ARB:
            val = 0;
            break;
        default:
            index = (s->ioregsel - IOAPIC_REG_REDTBL_BASE) >> 1;
            if (index >= 0 && index < IOAPIC_NUM_PINS) {
                if (s->ioregsel & 1) {
                    val = s->ioredtbl[index] >> 32;
                } else {
                    val = s->ioredtbl[index] & 0xffffffff;
                }
            }
        }
        DPRINTF("read: %08x = %08x\n", s->ioregsel, val);
        break;
    }
    return val;
}

static void
ioapic_mem_write(void *opaque, target_phys_addr_t addr, uint64_t val,
                 unsigned int size)
{
    IOAPICState *s = opaque;
    int index;

    switch (addr & 0xff) {
    case IOAPIC_IOREGSEL:
        s->ioregsel = val;
        break;
    case IOAPIC_IOWIN:
        if (size != 4) {
            break;
        }
        DPRINTF("write: %08x = %08x\n", s->ioregsel, val);
        switch (s->ioregsel) {
        case IOAPIC_REG_ID:
            s->id = (val >> IOAPIC_ID_SHIFT) & IOAPIC_ID_MASK;
            break;
        case IOAPIC_REG_VER:
        case IOAPIC_REG_ARB:
            break;
        default:
            index = (s->ioregsel - IOAPIC_REG_REDTBL_BASE) >> 1;
            if (index >= 0 && index < IOAPIC_NUM_PINS) {
                if (s->ioregsel & 1) {
                    s->ioredtbl[index] &= 0xffffffff;
                    s->ioredtbl[index] |= (uint64_t)val << 32;
                } else {
                    s->ioredtbl[index] &= ~0xffffffffULL;
                    s->ioredtbl[index] |= val;
                }
                ioapic_service(s);
            }
        }
        break;
    }
}

static int ioapic_post_load(void *opaque, int version_id)
{
    IOAPICState *s = opaque;

    if (version_id == 1) {
        /* set sane value */
        s->irr = 0;
    }
    return 0;
}

static const VMStateDescription vmstate_ioapic = {
    .name = "ioapic",
    .version_id = 3,
    .post_load = ioapic_post_load,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(id, IOAPICState),
        VMSTATE_UINT8(ioregsel, IOAPICState),
        VMSTATE_UNUSED_V(2, 8), /* to account for qemu-kvm's v2 format */
        VMSTATE_UINT32_V(irr, IOAPICState, 2),
        VMSTATE_UINT64_ARRAY(ioredtbl, IOAPICState, IOAPIC_NUM_PINS),
        VMSTATE_END_OF_LIST()
    }
};

static void ioapic_reset(DeviceState *d)
{
    IOAPICState *s = DO_UPCAST(IOAPICState, busdev.qdev, d);
    int i;

    s->id = 0;
    s->ioregsel = 0;
    s->irr = 0;
    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        s->ioredtbl[i] = 1 << IOAPIC_LVT_MASKED_SHIFT;
    }
}

static const MemoryRegionOps ioapic_io_ops = {
    .read = ioapic_mem_read,
    .write = ioapic_mem_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int ioapic_init1(SysBusDevice *dev)
{
    IOAPICState *s = FROM_SYSBUS(IOAPICState, dev);
    static int ioapic_no;

    if (ioapic_no >= MAX_IOAPICS) {
        return -1;
    }

    memory_region_init_io(&s->io_memory, &ioapic_io_ops, s, "ioapic", 0x1000);
    sysbus_init_mmio(dev, &s->io_memory);

    qdev_init_gpio_in(&dev->qdev, ioapic_set_irq, IOAPIC_NUM_PINS);

    ioapics[ioapic_no++] = s;

    return 0;
}

static void ioapic_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = ioapic_init1;
}

static DeviceInfo ioapic_info = {
    .name = "ioapic",
    .size = sizeof(IOAPICState),
    .vmsd = &vmstate_ioapic,
    .reset = ioapic_reset,
    .no_user = 1,
    .class_init = ioapic_class_init,
};

static void ioapic_register_devices(void)
{
    sysbus_register_withprop(&ioapic_info);
}

device_init(ioapic_register_devices)
