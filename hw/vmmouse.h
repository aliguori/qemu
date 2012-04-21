#ifndef QEMU_VMMOUSE_H
#define QEMU_VMMOUSE_H

#include "qdev.h"
#include "console.h"
#include "ps2.h"

#define TYPE_VMMOUSE "vmmouse"
#define VMMOUSE(obj) OBJECT_CHECK(VMMouseState, (obj), TYPE_VMMOUSE)

#define VMMOUSE_QUEUE_SIZE	1024

typedef struct VMMouseState
{
    DeviceState parent;

    MemoryRegion io;
    PS2MouseState *ps2_mouse;

    /*< private >*/
    uint32_t queue[VMMOUSE_QUEUE_SIZE];
    int32_t queue_size;
    uint16_t nb_queue;
    uint16_t status;
    uint8_t absolute;
    QEMUPutMouseEntry *entry;
} VMMouseState;

#endif
