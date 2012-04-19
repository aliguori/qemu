#include "qemu/pin.h"
#include "hw/irq.h"
#include "module.h"

bool pin_get_level(Pin *pin)
{
    return pin->level;
}

void pin_set_level(Pin *pin, bool value)
{
    if (pin->level == value) {
        return;
    }

    pin->level = value;

    notifier_list_notify(&pin->level_change, pin);
}

void pin_add_level_change_notifier(Pin *pin, Notifier *notifier)
{
    notifier_list_add(&pin->level_change, notifier);
}

void pin_del_level_change_notifier(Pin *pin, Notifier *notifier)
{
    notifier_list_remove(&pin->level_change, notifier);
}

typedef struct QemuIrqConnector
{
    qemu_irq in;
    Pin *out;
    Notifier notifier;
} QemuIrqConnector;

static void qic_update(Notifier *notifier, void *opaque)
{
    QemuIrqConnector *c;

    c = container_of(notifier, QemuIrqConnector, notifier);
    qemu_set_irq(c->in, pin_get_level(c->out));
}

static void qic_release(Notifier *notifier)
{
    QemuIrqConnector *c;

    c = container_of(notifier, QemuIrqConnector, notifier);
    g_free(c);
}

void pin_connect_qemu_irq(Pin *out, qemu_irq in)
{
    QemuIrqConnector *c;

    c = g_malloc0(sizeof(*c));
    c->notifier.notify = qic_update;
    c->notifier.release = qic_release;
    c->out = out;
    c->in = in;

    pin_add_level_change_notifier(out, &c->notifier);
}

static void pin_initfn(Object *obj)
{
    Pin *pin = PIN(obj);

    notifier_list_init(&pin->level_change);
    object_property_add_bool(obj, "level",
                             (BoolPropertyGetter *)pin_get_level,
                             (BoolPropertySetter *)pin_set_level,
                             NULL);
}

static void pin_fini(Object *obj)
{
    Pin *pin = PIN(obj);

    notifier_list_destroy(&pin->level_change);
}

static TypeInfo pin_info = {
    .name = TYPE_PIN,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(Pin),
    .instance_init = pin_initfn,
    .instance_finalize = pin_fini,
};

static void register_devices(void)
{
    type_register_static(&pin_info);
}

type_init(register_devices);
