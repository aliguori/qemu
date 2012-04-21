/*
 * QEMU VMMouse emulation
 *
 * Copyright (C) 2007 Anthony Liguori <anthony@codemonkey.ws>
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
#include "vmmouse.h"

/* debug only vmmouse */
//#define DEBUG_VMMOUSE

#define VMPORT_CMD_GETVERSION 0x0a
#define VMPORT_CMD_GETRAMSIZE 0x14

#define VMPORT_MAGIC   0x564D5868

/* VMMouse Commands */
#define VMMOUSE_GETVERSION	10
#define VMMOUSE_DATA		39
#define VMMOUSE_STATUS		40
#define VMMOUSE_COMMAND		41

#define VMMOUSE_READ_ID			0x45414552
#define VMMOUSE_DISABLE			0x000000f5
#define VMMOUSE_REQUEST_RELATIVE	0x4c455252
#define VMMOUSE_REQUEST_ABSOLUTE	0x53424152

#define VMMOUSE_VERSION		0x3442554a

#ifdef DEBUG_VMMOUSE
#define DPRINTF(fmt, ...) printf(fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

static void vmport_get_data(uint32_t *data)
{
    CPUX86State *env = cpu_single_env;

    cpu_synchronize_state(env);

    data[0] = env->regs[R_EAX]; data[1] = env->regs[R_EBX];
    data[2] = env->regs[R_ECX]; data[3] = env->regs[R_EDX];
    data[4] = env->regs[R_ESI]; data[5] = env->regs[R_EDI];
}

static void vmport_set_data(const uint32_t *data)
{
    CPUX86State *env = cpu_single_env;

    env->regs[R_EAX] = data[0]; env->regs[R_EBX] = data[1];
    env->regs[R_ECX] = data[2]; env->regs[R_EDX] = data[3];
    env->regs[R_ESI] = data[4]; env->regs[R_EDI] = data[5];
}

static uint32_t vmmouse_get_status(VMMouseState *s)
{
    DPRINTF("vmmouse_get_status()\n");
    return (s->status << 16) | s->nb_queue;
}

static void vmmouse_mouse_event(void *opaque, int x, int y, int dz, int buttons_state)
{
    VMMouseState *s = opaque;
    int buttons = 0;

    if (s->nb_queue > (VMMOUSE_QUEUE_SIZE - 4))
        return;

    DPRINTF("vmmouse_mouse_event(%d, %d, %d, %d)\n",
            x, y, dz, buttons_state);

    if ((buttons_state & MOUSE_EVENT_LBUTTON))
        buttons |= 0x20;
    if ((buttons_state & MOUSE_EVENT_RBUTTON))
        buttons |= 0x10;
    if ((buttons_state & MOUSE_EVENT_MBUTTON))
        buttons |= 0x08;

    if (s->absolute) {
        x <<= 1;
        y <<= 1;
    }

    s->queue[s->nb_queue++] = buttons;
    s->queue[s->nb_queue++] = x;
    s->queue[s->nb_queue++] = y;
    s->queue[s->nb_queue++] = dz;

    /* need to still generate PS2 events to notify driver to
       read from queue */
    i8042_mouse_fake_event(s->ps2_mouse);
}

static void vmmouse_remove_handler(VMMouseState *s)
{
    if (s->entry) {
        qemu_remove_mouse_event_handler(s->entry);
        s->entry = NULL;
    }
}

static void vmmouse_update_handler(VMMouseState *s, int absolute)
{
    if (s->status != 0) {
        return;
    }
    if (s->absolute != absolute) {
        s->absolute = absolute;
        vmmouse_remove_handler(s);
    }
    if (s->entry == NULL) {
        s->entry = qemu_add_mouse_event_handler(vmmouse_mouse_event,
                                                s, s->absolute,
                                                "vmmouse");
        qemu_activate_mouse_event_handler(s->entry);
    }
}

static void vmmouse_read_id(VMMouseState *s)
{
    DPRINTF("vmmouse_read_id()\n");

    if (s->nb_queue == VMMOUSE_QUEUE_SIZE)
        return;

    s->queue[s->nb_queue++] = VMMOUSE_VERSION;
    s->status = 0;
}

static void vmmouse_request_relative(VMMouseState *s)
{
    DPRINTF("vmmouse_request_relative()\n");
    vmmouse_update_handler(s, 0);
}

static void vmmouse_request_absolute(VMMouseState *s)
{
    DPRINTF("vmmouse_request_absolute()\n");
    vmmouse_update_handler(s, 1);
}

static void vmmouse_disable(VMMouseState *s)
{
    DPRINTF("vmmouse_disable()\n");
    s->status = 0xffff;
    vmmouse_remove_handler(s);
}

static void vmmouse_data(VMMouseState *s, uint32_t *data, uint32_t size)
{
    int i;

    DPRINTF("vmmouse_data(%d)\n", size);

    if (size == 0 || size > 6 || size > s->nb_queue) {
        DPRINTF("vmmouse: driver requested too much data %d\n", size);
        s->status = 0xffff;
        vmmouse_remove_handler(s);
        return;
    }

    for (i = 0; i < size; i++)
        data[i] = s->queue[i];

    s->nb_queue -= size;
    if (s->nb_queue)
        memmove(s->queue, &s->queue[size], sizeof(s->queue[0]) * s->nb_queue);
}


static uint64_t vmmouse_read(void *opaque, target_phys_addr_t addr,
                             unsigned size)
{
    VMMouseState *s = opaque;
    uint32_t data[6];
    uint16_t command;

    vmport_get_data(data);

    if (data[0] != VMPORT_MAGIC) {
        return;
    }

    switch ((data[2] & 0xFFFF)) {
    case VMPORT_CMD_GETVERSION:
        data[0] = 6;
        data[1] = VMPORT_MAGIC;
        break;
    case VMPPORT_CMD_GETRAMSIZE:
        data[0] = ram_size;
        data[1] = 0x1177;
        break;
    case VMMOUSE_STATUS:
        data[0] = vmmouse_get_status(s);
        break;
    case VMMOUSE_COMMAND:
        switch (data[1]) {
        case VMMOUSE_DISABLE:
            vmmouse_disable(s);
            break;
        case VMMOUSE_READ_ID:
            vmmouse_read_id(s);
            break;
        case VMMOUSE_REQUEST_RELATIVE:
            vmmouse_request_relative(s);
            break;
        case VMMOUSE_REQUEST_ABSOLUTE:
            vmmouse_request_absolute(s);
            break;
        default:
            printf("vmmouse: unknown command %x\n", data[1]);
            break;
        }
        break;
    case VMMOUSE_DATA:
        vmmouse_data(s, data, data[1]);
        break;
    default:
        printf("vmmouse: unknown command %x\n", command);
        break;
    }

    vmmouse_set_data(data);
}

static void vmmouse_write(void *opaque, target_phys_addr_t addr,
                          uint64_t val, unsigned size)
{
    vmmouse_read(opaque, addr, size);
}

static int vmmouse_post_load(void *opaque, int version_id)
{
    VMMouseState *s = opaque;

    vmmouse_remove_handler(s);
    vmmouse_update_handler(s, s->absolute);
    return 0;
}

static const VMStateDescription vmstate_vmmouse = {
    .name = "vmmouse",
    .version_id = 0,
    .minimum_version_id = 0,
    .minimum_version_id_old = 0,
    .post_load = vmmouse_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_INT32_EQUAL(queue_size, VMMouseState),
        VMSTATE_UINT32_ARRAY(queue, VMMouseState, VMMOUSE_QUEUE_SIZE),
        VMSTATE_UINT16(nb_queue, VMMouseState),
        VMSTATE_UINT16(status, VMMouseState),
        VMSTATE_UINT8(absolute, VMMouseState),
        VMSTATE_END_OF_LIST()
    }
};

static void vmmouse_reset(DeviceState *d)
{
    VMMouseState *s = container_of(d, VMMouseState, dev.qdev);

    s->status = 0xffff;
    s->queue_size = VMMOUSE_QUEUE_SIZE;

    vmmouse_disable(s);
}

static const MemoryRegionPortioOps vmmouse_ops = {
    .read = vmmouse_read,
    .write = vmmouse_write,
}

static void vmmouse_initfn(Object *obj)
{
    VMMouseState *s = VMMOUSE(obj);

    memory_region_init_io(&s->io, &vmmouse_ops, s, "vmmouse", 1);

    object_property_add_link(obj, "ps2_mouse", TYPE_PS2_MOUSE,
                             (Object **)&s->ps2_mouse, NULL);
}

static int vmmouse_realize(DeviceState *dev)
{
    VMMouseState *s = VMMOUSE(dev);

    g_assert(s->ps2_mouse != NULL);

    DPRINTF("vmmouse_init\n");

    return 0;
}

static void vmmouse_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->init = vmmouse_realize;
    dc->reset = vmmouse_reset;
    dc->vmsd = &vmstate_vmmouse;
}

static TypeInfo vmmouse_info = {
    .name          = TYPE_VMMOUSE,
    .parent        = TYPE_DEVICE,
    .instance_init = vmmouse_initfn,
    .instance_size = sizeof(VMMouseState),
    .class_init    = vmmouse_class_initfn,
};

static void vmmouse_register_types(void)
{
    type_register_static(&vmmouse_info);
}

type_init(vmmouse_register_types)
