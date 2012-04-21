#ifndef QEMU_PCKBD_H
#define QEMU_PCKBD_H

#include "qdev.h"
#include "isa.h"
#include "qemu/pin.h"
#include "ps2.h"

#define TYPE_I8042 "i8042"
#define I8042(obj) OBJECT_CHECK(KBDState, (obj), TYPE_I8042)

typedef struct KBDState
{
    DeviceState parent;

    PS2KbdState kbd;
    PS2MouseState mouse;

    Pin irq_kbd;
    Pin irq_mouse;
    Pin a20_out;

    MemoryRegion io;

    /*< private >*/
    uint8_t write_cmd; /* if non zero, write data to port 60 is expected */
    uint8_t status;
    uint8_t mode;
    uint8_t outport;
    /* Bitmask of devices with data available.  */
    uint8_t pending;

    Notifier kbd_notifier;
    Notifier mouse_notifier;

    int32_t it_shift;
    int32_t addr_size;

    target_phys_addr_t mask;
} KBDState;

KBDState *i8042_init(ISABus *isa_bus, int base, qemu_irq a20_line);
void i8042_mm_init(MemoryRegion *address_space,
                   qemu_irq kbd_irq, qemu_irq mouse_irq,
                   target_phys_addr_t base, ram_addr_t size,
                   int32_t it_shift);
void i8042_mouse_fake_event(KBDState *s);

#endif
