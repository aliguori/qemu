/*
 * QEMU Object Model IRQ interface
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

#ifndef QEMU_PIN_H
#define QEMU_PIN_H

#include "qemu/object.h"
#include "qemu/notify.h"
#include "qemu-common.h"

typedef struct PinClass PinClass;
typedef struct Pin Pin;

#define TYPE_PIN "pin"
#define PIN(obj) OBJECT_CHECK(Pin, (obj), TYPE_PIN)

struct Pin
{
    Object parent;

    /*< private >*/
    bool level;

    NotifierList level_change;
};

bool pin_get_level(Pin *pin);
void pin_set_level(Pin *pin, bool value);

static inline void pin_raise(Pin *pin)
{
    pin_set_level(pin, true);
}

static inline void pin_lower(Pin *pin)
{
    pin_set_level(pin, false);
}

static inline void pin_pulse(Pin *pin)
{
    pin_raise(pin);
    pin_lower(pin);
}

void pin_add_level_change_notifier(Pin *pin, Notifier *notifier);
void pin_del_level_change_notifier(Pin *pin, Notifier *notifier);

void pin_connect_qemu_irq(Pin *out, qemu_irq in);
qemu_irq pin_get_qemu_irq(Pin *in);

#endif
