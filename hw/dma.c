/*
 * QEMU DMA emulation
 *
 * Copyright (c) 2003-2004 Vassili Karpov (malc)
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
#include "dma-controller.h"

/* #define DEBUG_DMA */

#ifdef DEBUG_DMA
#define DPRINTF(...) do { fprintf (stderr, "dma: " __VA_ARGS__) } while (0)
#else
#define DPRINTF(...) do { if (0) { fprintf(stderr, "dma: " __VA_ARGS__); } } while (0)
#endif

#define ADDR 0
#define COUNT 1

enum {
    CMD_MEMORY_TO_MEMORY = 0x01,
    CMD_FIXED_ADDRESS    = 0x02,
    CMD_BLOCK_CONTROLLER = 0x04,
    CMD_COMPRESSED_TIME  = 0x08,
    CMD_CYCLIC_PRIORITY  = 0x10,
    CMD_EXTENDED_WRITE   = 0x20,
    CMD_LOW_DREQ         = 0x40,
    CMD_LOW_DACK         = 0x80,
    CMD_NOT_SUPPORTED    = CMD_MEMORY_TO_MEMORY | CMD_FIXED_ADDRESS
    | CMD_COMPRESSED_TIME | CMD_CYCLIC_PRIORITY | CMD_EXTENDED_WRITE
    | CMD_LOW_DREQ | CMD_LOW_DACK

};

static const int channels[8] = {-1, 2, 3, 1, -1, -1, -1, 0};

/* request the emulator to transfer a new DMA memory block ASAP */
static void DMA_schedule(DMAController *d)
{
    qemu_mod_timer_ns(d->dma_timer, qemu_get_clock_ns(vm_clock));
}

static void write_page(void *opaque, uint32_t nport, uint32_t data)
{
    DMAController *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        DPRINTF("invalid channel %#x %#x\n", nport, data);
        return;
    }
    d->regs[ichan].page = data;
}

static void write_pageh(void *opaque, uint32_t nport, uint32_t data)
{
    DMAController *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        DPRINTF("invalid channel %#x %#x\n", nport, data);
        return;
    }
    d->regs[ichan].pageh = data;
}

static uint32_t read_page(void *opaque, uint32_t nport)
{
    DMAController *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        DPRINTF("invalid channel read %#x\n", nport);
        return 0;
    }
    return d->regs[ichan].page;
}

static uint32_t read_pageh(void *opaque, uint32_t nport)
{
    DMAController *d = opaque;
    int ichan;

    ichan = channels[nport & 7];
    if (-1 == ichan) {
        DPRINTF("invalid channel read %#x\n", nport);
        return 0;
    }
    return d->regs[ichan].pageh;
}

static void init_chan(DMAController *d, int ichan)
{
    DMARegisters *r;

    r = d->regs + ichan;
    r->now[ADDR] = r->base[ADDR] << d->dshift;
    r->now[COUNT] = 0;
}

static int getff(DMAController *d)
{
    int ff;

    ff = d->flip_flop;
    d->flip_flop = !ff;
    return ff;
}

static uint32_t read_chan(void *opaque, uint32_t nport)
{
    DMAController *d = opaque;
    int ichan, nreg, iport, ff, val, dir;
    DMARegisters *r;

    iport = (nport >> d->dshift) & 0x0f;
    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;

    dir = ((r->mode >> 5) & 1) ? -1 : 1;
    ff = getff(d);
    if (nreg) {
        val = (r->base[COUNT] << d->dshift) - r->now[COUNT];
    } else {
        val = r->now[ADDR] + r->now[COUNT] * dir;
    }

    DPRINTF("read_chan %#x -> %d\n", iport, val);
    return (val >> (d->dshift + (ff << 3))) & 0xff;
}

static void write_chan(void *opaque, uint32_t nport, uint32_t data)
{
    DMAController *d = opaque;
    int iport, ichan, nreg;
    DMARegisters *r;

    iport = (nport >> d->dshift) & 0x0f;
    ichan = iport >> 1;
    nreg = iport & 1;
    r = d->regs + ichan;
    if (getff(d)) {
        r->base[nreg] = (r->base[nreg] & 0xff) | ((data << 8) & 0xff00);
        init_chan(d, ichan);
    } else {
        r->base[nreg] = (r->base[nreg] & 0xff00) | (data & 0xff);
    }
}

static void write_cont(void *opaque, uint32_t nport, uint32_t data)
{
    DMAController *d = opaque;
    int iport, ichan = 0;

    iport = (nport >> d->dshift) & 0x0f;
    switch (iport) {
    case 0x08:                  /* command */
        if ((data != 0) && (data & CMD_NOT_SUPPORTED)) {
            DPRINTF("command %#x not supported\n", data);
            return;
        }
        d->command = data;
        break;

    case 0x09:
        ichan = data & 3;
        if (data & 4) {
            d->status |= 1 << (ichan + 4);
        } else {
            d->status &= ~(1 << (ichan + 4));
        }
        d->status &= ~(1 << ichan);
        DMA_schedule(d);
        break;

    case 0x0a:                  /* single mask */
        if (data & 4) {
            d->mask |= 1 << (data & 3);
        } else {
            d->mask &= ~(1 << (data & 3));
        }
        DMA_schedule(d);
        break;

    case 0x0b:                  /* mode */
        {
            ichan = data & 3;
            {
                int op, ai, dir, opmode;
                op = (data >> 2) & 3;
                ai = (data >> 4) & 1;
                dir = (data >> 5) & 1;
                opmode = (data >> 6) & 3;

                DPRINTF("ichan %d, op %d, ai %d, dir %d, opmode %d\n",
                        ichan, op, ai, dir, opmode);
            }
            d->regs[ichan].mode = data;
            break;
        }

    case 0x0c:                  /* clear flip flop */
        d->flip_flop = 0;
        break;

    case 0x0d:                  /* reset */
        d->flip_flop = 0;
        d->mask = ~0;
        d->status = 0;
        d->command = 0;
        break;

    case 0x0e:                  /* clear mask for all channels */
        d->mask = 0;
        DMA_schedule(d);
        break;

    case 0x0f:                  /* write mask for all channels */
        d->mask = data;
        DMA_schedule(d);
        break;

    default:
        DPRINTF("unknown iport %#x\n", iport);
        break;
    }

    if (0xc != iport) {
        DPRINTF("write_cont: nport %#06x, ichan % 2d, val %#06x\n",
                nport, ichan, data);
    }
}

static uint32_t read_cont(void *opaque, uint32_t nport)
{
    DMAController *d = opaque;
    int iport, val;

    iport = (nport >> d->dshift) & 0x0f;
    switch (iport) {
    case 0x08:                  /* status */
        val = d->status;
        d->status &= 0xf0;
        break;
    case 0x0f:                  /* mask */
        val = d->mask;
        break;
    default:
        val = 0;
        break;
    }

    DPRINTF("read_cont: nport %#06x, iport %#04x val %#x\n", nport, iport, val);
    return val;
}

int DMA_get_channel_mode(DMAController *d, int nchan)
{
    return d->regs[nchan & 3].mode;
}

void DMA_hold_DREQ(DMAController *d, int nchan)
{
    int ichan;

    ichan = nchan & 3;
    DPRINTF("held cont=%d chan=%d\n", d->dshift, ichan);
    d->status |= 1 << (ichan + 4);
    DMA_schedule(d);
}

void DMA_release_DREQ(DMAController *d, int nchan)
{
    int ichan;

    ichan = nchan & 3;
    DPRINTF("released cont=%d chan=%d\n", d->dshift, ichan);
    d->status &= ~(1 << (ichan + 4));
    DMA_schedule(d);
}

static void channel_run(DMAController *d, int ichan)
{
    int n;
    DMARegisters *r = &d->regs[ichan];
    int dir, opmode;

    dir = (r->mode >> 5) & 1;
    opmode = (r->mode >> 6) & 3;

    if (dir) {
        DPRINTF("DMA in address decrement mode\n");
    }
    if (opmode != 1) {
        DPRINTF("DMA not in single mode select %#x\n", opmode);
    }

    n = r->transfer_handler(r->opaque, ichan + (d->dshift << 2),
                            r->now[COUNT], (r->base[COUNT] + 1) << d->dshift);
    r->now[COUNT] = n;
    DPRINTF("dma_pos %d size %d\n", n, (r->base[COUNT] + 1) << d->dshift);
}

static void DMA_run_timer(void *opaque)
{
    DMAController *d = opaque;
    int ichan;
    int rearm = 0;

    if (d->running) {
        rearm = 1;
        goto out;
    } else {
        d->running = 1;
    }

    for (ichan = 0; ichan < 4; ichan++) {
        int mask;

        mask = 1 << ichan;

        if ((0 == (d->mask & mask)) && (0 != (d->status & (mask << 4)))) {
            channel_run(d, ichan);
            rearm = 1;
        }
    }

    d->running = 0;
out:
    if (rearm) {
        qemu_mod_timer_ns(d->dma_timer,
                          qemu_get_clock_ns(vm_clock) + 1 * SCALE_MS);
    }
}

void DMA_register_channel(DMAController *d, int nchan,
                          DMA_transfer_handler transfer_handler,
                          void *opaque)
{
    DMARegisters *r;
    int ichan;

    ichan = nchan & 3;

    r = d->regs + ichan;
    r->transfer_handler = transfer_handler;
    r->opaque = opaque;
}

int DMA_read_memory(DMAController *d, int nchan, void *buf, int pos, int len)
{
    DMARegisters *r = &d->regs[nchan & 3];
    target_phys_addr_t addr = ((r->pageh & 0x7f) << 24) | (r->page << 16) | r->now[ADDR];

    if (r->mode & 0x20) {
        int i;
        uint8_t *p = buf;

        cpu_physical_memory_read(addr - pos - len, buf, len);
        /* What about 16bit transfers? */
        for (i = 0; i < len >> 1; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    } else {
        cpu_physical_memory_read(addr + pos, buf, len);
    }

    return len;
}

int DMA_write_memory(DMAController *d, int nchan, void *buf, int pos, int len)
{
    DMARegisters *r = &d->regs[nchan & 3];
    target_phys_addr_t addr = ((r->pageh & 0x7f) << 24) | (r->page << 16) | r->now[ADDR];

    if (r->mode & 0x20) {
        int i;
        uint8_t *p = buf;

        cpu_physical_memory_write(addr - pos - len, buf, len);
        /* What about 16bit transfers? */
        for (i = 0; i < len; i++) {
            uint8_t b = p[len - i - 1];
            p[i] = b;
        }
    } else {
        cpu_physical_memory_write(addr + pos, buf, len);
    }

    return len;
}

static void dma_reset(DeviceState *dev)
{
    DMAController *d = DMA_CONTROLLER(dev);

    write_cont(d, (0x0d << d->dshift), 0);
}

static int dma_phony_handler(void *opaque, int nchan, int dma_pos, int dma_len)
{
    DPRINTF("unregistered DMA channel used nchan=%d dma_pos=%d dma_len=%d\n",
           nchan, dma_pos, dma_len);
    return dma_pos;
}

static void dma_initfn(Object *obj)
{
    DMAController *d = DMA_CONTROLLER(obj);
    int i;

    d->dma_timer = qemu_new_timer_ns(vm_clock, DMA_run_timer, d);

    for (i = 0; i < ARRAY_SIZE (d->regs); ++i) {
        d->regs[i].transfer_handler = dma_phony_handler;
    }
}

static int dma_realize(DeviceState *dev)
{
    return 0;
}

static const VMStateDescription vmstate_dma_regs = {
    .name = "dma_regs",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_INT32_ARRAY(now, DMARegisters, 2),
        VMSTATE_UINT16_ARRAY(base, DMARegisters, 2),
        VMSTATE_UINT8(mode, DMARegisters),
        VMSTATE_UINT8(page, DMARegisters),
        VMSTATE_UINT8(pageh, DMARegisters),
        VMSTATE_UINT8(dack, DMARegisters),
        VMSTATE_UINT8(eop, DMARegisters),
        VMSTATE_END_OF_LIST()
    }
};

static int dma_post_load(void *opaque, int version_id)
{
    DMAController *d = opaque;

    DMA_schedule(d);

    return 0;
}

static const VMStateDescription vmstate_dma = {
    .name = "dma",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .post_load = dma_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_UINT8(command, DMAController),
        VMSTATE_UINT8(mask, DMAController),
        VMSTATE_UINT8(flip_flop, DMAController),
        VMSTATE_INT32(dshift, DMAController),
        VMSTATE_STRUCT_ARRAY(regs, DMAController, 4, 1, vmstate_dma_regs, DMARegisters),
        VMSTATE_END_OF_LIST()
    }
};

static Property dma_properties[] = {
    DEFINE_PROP_INT32("dshift", DMAController, dshift, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void dma_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->init = dma_realize;
    dc->reset = dma_reset;
    dc->vmsd = &vmstate_dma;
    dc->props = dma_properties;
}

static TypeInfo dma_info = {
    .name = TYPE_DMA_CONTROLLER,
    .parent = TYPE_DEVICE,
    .class_init = dma_class_initfn,
    .instance_size = sizeof(DMAController),
    .instance_init = dma_initfn,
};

static void register_types(void)
{
    type_register_static(&dma_info);
}

type_init(register_types);    

/* dshift = 0: 8 bit DMA, 1 = 16 bit DMA */
static void dma_init2(DMAController *d, int base, int dshift,
                      int page_base, int pageh_base)
{
    static const int page_port_list[] = { 0x1, 0x2, 0x3, 0x7 };
    int i;

    object_initialize(d, TYPE_DMA_CONTROLLER);
    qdev_prop_set_globals(DEVICE(d));
    qdev_prop_set_int32(DEVICE(d), "dshift", dshift);

    qdev_init_nofail(DEVICE(d));

    for (i = 0; i < 8; i++) {
        register_ioport_write(base + (i << dshift), 1, 1, write_chan, d);
        register_ioport_read(base + (i << dshift), 1, 1, read_chan, d);
    }
    for (i = 0; i < ARRAY_SIZE(page_port_list); i++) {
        register_ioport_write(page_base + page_port_list[i], 1, 1,
                              write_page, d);
        register_ioport_read(page_base + page_port_list[i], 1, 1,
                             read_page, d);
        if (pageh_base >= 0) {
            register_ioport_write(pageh_base + page_port_list[i], 1, 1,
                                  write_pageh, d);
            register_ioport_read(pageh_base + page_port_list[i], 1, 1,
                                 read_pageh, d);
        }
    }
    for (i = 0; i < 8; i++) {
        register_ioport_write(base + ((i + 8) << dshift), 1, 1,
                              write_cont, d);
        register_ioport_read(base + ((i + 8) << dshift), 1, 1,
                             read_cont, d);
    }
}

DMAController *DMA_init(int high_page_enable)
{
    DMAController *dma_controllers = g_malloc0(sizeof(*dma_controllers) * 2);

    dma_init2(&dma_controllers[0], 0x00, 0, 0x80,
              high_page_enable ? 0x480 : -1);
    dma_init2(&dma_controllers[1], 0xc0, 1, 0x88,
              high_page_enable ? 0x488 : -1);
    vmstate_register(NULL, 0, &vmstate_dma, &dma_controllers[0]);
    vmstate_register(NULL, 1, &vmstate_dma, &dma_controllers[1]);

    return dma_controllers;
}
