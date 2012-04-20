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

static void wire_initfn(Object *obj)
{
    Wire *s = WIRE(obj);

    object_property_add_link(obj, "in", TYPE_PIN, (Object **)&s->in, NULL);
    object_property_add_link(obj, "out", TYPE_PIN, (Object **)&s->out, NULL);
}

static void wire_notify_update(Notifier *notifier, void *opaque)
{
    Wire *s = container_of(notifier, Wire, out_level_change);

    pin_set_level(s->in, pin_get_level(s->out));
}

static void wire_notify_release(Notifier *notifier)
{
    Wire *s = container_of(notifier, Wire, out_level_change);

    s->connected = false;
}

void wire_realize(Wire *s)
{
    g_assert(s->in && s->out);

    s->out_level_change.notify = wire_notify_update;
    s->out_level_change.release = wire_notify_release;
    pin_add_level_change_notifier(s->out, &s->out_level_change);
    s->connected = true;
}

static void wire_finalize(Object *obj)
{
    Wire *s = WIRE(obj);

    if (s->connected) {
        pin_del_level_change_notifier(s->out, &s->out_level_change);
        s->connected = false;
    }
}

static TypeInfo wire_info = {
    .name = TYPE_WIRE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(Wire),
    .instance_init = wire_initfn,
    .instance_finalize = wire_finalize,
};

void pin_connect_pin(Pin *out, Pin *in)
{
    /* FIXME: need to figure out how/when to free this */
    Wire *s = WIRE(object_new(TYPE_WIRE));

    s->in = in;
    s->out = out;

    wire_realize(s);
}

static void register_devices(void)
{
    type_register_static(&pin_info);
    type_register_static(&wire_info);
}

type_init(register_devices);
