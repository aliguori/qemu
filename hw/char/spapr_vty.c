/*
 * QEMU PowerPC pSeries Logical Partition (aka sPAPR) hardware System Emulator
 *
 * PAPR Inter-VM Logical Lan, aka ibmveth
 *
 * Copyright IBM, Corp. 2010-2013
 *
 * Authors:
 *   David Gibson <david@gibson.dropbear.id.au>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "hw/qdev.h"
#include "sysemu/char.h"
#include "hw/ppc/spapr.h"
#include "hw/ppc/spapr_vio.h"

#define VTERM_BUFSIZE   16

typedef struct VIOsPAPRVTYDevice {
    VIOsPAPRDevice sdev;
    CharDriverState *chardev;
    uint32_t in, out;
    uint8_t buf[VTERM_BUFSIZE];
} VIOsPAPRVTYDevice;

#define TYPE_VIO_SPAPR_VTY_DEVICE "spapr-vty"
#define VIO_SPAPR_VTY_DEVICE(obj) \
     OBJECT_CHECK(VIOsPAPRVTYDevice, (obj), TYPE_VIO_SPAPR_VTY_DEVICE)

static int spapr_vty_can_receive(void *opaque)
{
    VIOsPAPRVTYDevice *dev = VIO_SPAPR_VTY_DEVICE(opaque);

    return (dev->in - dev->out) < VTERM_BUFSIZE;
}

static void spapr_vty_receive(void *opaque, const uint8_t *buf, int size)
{
    VIOsPAPRVTYDevice *dev = VIO_SPAPR_VTY_DEVICE(opaque);
    int i;

    if ((dev->in == dev->out) && size) {
        /* toggle line to simulate edge interrupt */
        qemu_irq_pulse(spapr_vio_qirq(&dev->sdev));
    }
    for (i = 0; i < size; i++) {
        assert((dev->in - dev->out) < VTERM_BUFSIZE);
        dev->buf[dev->in++ % VTERM_BUFSIZE] = buf[i];
    }
}

static int spapr_vty_getchars(VIOsPAPRVTYDevice *dev, uint8_t *buf, int max)
{
    int n = 0;

    while ((n < max) && (dev->out != dev->in)) {
        buf[n++] = dev->buf[dev->out++ % VTERM_BUFSIZE];
    }

    return n;
}

static void spapr_vty_putchars(VIOsPAPRVTYDevice *dev,
                               const uint8_t *buf, int len)
{
    /* There is no flow control with this interface so we can't really
     * do anything if we are unable to write out data.  So we ignore errors
     * here and silently drop the data.
     *
     * Our only option would be buffering but the kernel already has a buffer
     * so that would only delay the inevitable.
     */
    qemu_chr_fe_write(dev->chardev, buf, len);
}

static int spapr_vty_init(VIOsPAPRDevice *sdev)
{
    VIOsPAPRVTYDevice *dev = VIO_SPAPR_VTY_DEVICE(sdev);

    if (!dev->chardev) {
        fprintf(stderr, "spapr-vty: Can't create vty without a chardev!\n");
        exit(1);
    }

    qemu_chr_add_handlers(dev->chardev, spapr_vty_can_receive,
                          spapr_vty_receive, NULL, dev);

    return 0;
}

/* Forward declaration */
static target_ulong h_put_term_char(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                    target_ulong opcode, target_ulong *args)
{
    target_ulong reg = args[0];
    target_ulong len = args[1];
    VIOsPAPRDevice *sdev;
    uint8_t buf[16];
    int i;

    sdev = spapr_vio_find_by_reg(spapr->vio_bus, reg,
                                 TYPE_VIO_SPAPR_VTY_DEVICE);
    if (!sdev) {
        return H_PARAMETER;
    }

    if (len > 16) {
        return H_PARAMETER;
    }

    for (i = 0; i < 16; i++) {
        uint64_t shift = (7 - (i % 8)) * 8;
        int index = 2 + (i / 8);

        buf[i] = args[index] >> shift;
    }

    spapr_vty_putchars(VIO_SPAPR_VTY_DEVICE(sdev), buf, len);

    return H_SUCCESS;
}

static target_ulong h_get_term_char(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                    target_ulong opcode, target_ulong *args)
{
    VIOsPAPRDevice *sdev;
    target_ulong reg = args[0];
    target_ulong len;
    uint8_t buf[16];
    int i;

    sdev = spapr_vio_find_by_reg(spapr->vio_bus, reg,
                                 TYPE_VIO_SPAPR_VTY_DEVICE);
    if (!sdev) {
        return H_PARAMETER;
    }

    len = spapr_vty_getchars(VIO_SPAPR_VTY_DEVICE(sdev), buf, sizeof(buf));

    args[0] = len;
    args[1] = args[2] = 0;
    for (i = 0; i < len; i++) {
        uint64_t shift = (7 - (i % 8)) * 8;
        int index = 1 + (i / 8);

        args[index] |= (uint64_t)buf[i] << shift;
    }

    return H_SUCCESS;
}

void spapr_vty_create(VIOsPAPRBus *bus, CharDriverState *chardev)
{
    DeviceState *dev;

    dev = qdev_create(&bus->bus, "spapr-vty");
    qdev_prop_set_chr(dev, "chardev", chardev);
    qdev_init_nofail(dev);
}

static Property spapr_vty_properties[] = {
    DEFINE_SPAPR_PROPERTIES(VIOsPAPRVTYDevice, sdev),
    DEFINE_PROP_CHR("chardev", VIOsPAPRVTYDevice, chardev),
    DEFINE_PROP_END_OF_LIST(),
};

static void spapr_vty_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VIOsPAPRDeviceClass *k = VIO_SPAPR_DEVICE_CLASS(klass);

    k->init = spapr_vty_init;
    k->dt_name = "vty";
    k->dt_type = "serial";
    k->dt_compatible = "hvterm1";
    dc->props = spapr_vty_properties;
}

static const TypeInfo spapr_vty_info = {
    .name          = TYPE_VIO_SPAPR_VTY_DEVICE,
    .parent        = TYPE_VIO_SPAPR_DEVICE,
    .instance_size = sizeof(VIOsPAPRVTYDevice),
    .class_init    = spapr_vty_class_init,
};

static void spapr_vty_register_types(void)
{
    spapr_register_hypercall(H_PUT_TERM_CHAR, h_put_term_char);
    spapr_register_hypercall(H_GET_TERM_CHAR, h_get_term_char);
    type_register_static(&spapr_vty_info);
}

type_init(spapr_vty_register_types)
