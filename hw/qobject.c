/*
 * QEMU Object Model
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qobject.h"

#define MAX_INTERFACES 32

typedef struct QInterfaceImpl
{
    const char *parent;
    void (*interface_initfn)(QObjectClass *class);
    QType type;
} QInterfaceImpl;

typedef struct QTypeImpl
{
    const char *name;
    QType type;

    size_t class_size;

    size_t instance_size;

    void (*base_init)(QObjectClass *klass);
    void (*base_finalize)(QObjectClass *klass);

    void (*class_init)(QObjectClass *klass);
    void (*class_finalize)(QObjectClass *klass);

    void (*instance_init)(QObject *obj);
    void (*instance_finalize)(QObject *obj);

    bool abstract;

    const char *parent;

    QObjectClass *class;

    int num_interfaces;
    QInterfaceImpl interfaces[MAX_INTERFACES];
} QTypeImpl;

static int num_types = 1;
static QTypeImpl type_table[1024];

QType qtype_register_static(const QTypeInfo *info)
{
    QType type = num_types++;
    QTypeImpl *ti;

    ti = &type_table[type];

    assert(info->name != NULL);

    ti->name = info->name;
    ti->parent = info->parent;
    ti->type = type;

    ti->class_size = info->class_size;
    ti->instance_size = info->instance_size;

    ti->base_init = info->base_init;
    ti->base_finalize = info->base_finalize;

    ti->class_init = info->class_init;
    ti->class_finalize = info->class_finalize;

    ti->instance_init = info->instance_init;
    ti->instance_finalize = info->instance_finalize;

    ti->abstract = info->abstract;

    if (info->interfaces) {
        int i;

        for (i = 0; info->interfaces[i].type; i++) {
            ti->interfaces[i].parent = info->interfaces[i].type;
            ti->interfaces[i].interface_initfn = info->interfaces[i].interface_initfn;
            ti->num_interfaces++;
        }
    }

    return type;
}

static QType qtype_register_anonymous(const QTypeInfo *info)
{
    QType type = num_types++;
    QTypeImpl *ti;
    char buffer[32];
    static int count;

    ti = &type_table[type];

    snprintf(buffer, sizeof(buffer), "<anonymous-%d>", count++);
    ti->name = g_strdup(buffer);
    ti->parent = g_strdup(info->parent);
    ti->type = type;

    ti->class_size = info->class_size;
    ti->instance_size = info->instance_size;

    ti->base_init = info->base_init;
    ti->base_finalize = info->base_finalize;

    ti->class_init = info->class_init;
    ti->class_finalize = info->class_finalize;

    ti->instance_init = info->instance_init;
    ti->instance_finalize = info->instance_finalize;

    if (info->interfaces) {
        int i;

        for (i = 0; info->interfaces[i].type; i++) {
            ti->interfaces[i].parent = info->interfaces[i].type;
            ti->interfaces[i].interface_initfn = info->interfaces[i].interface_initfn;
            ti->num_interfaces++;
        }
    }

    return type;
}

static QTypeImpl *qtype_get_instance(QType type)
{
    assert(type != 0);
    assert(type < num_types);

    return &type_table[type];
}

QType qtype_get_by_name(const char *name)
{
    int i;

    if (name == NULL) {
        return 0;
    }

    for (i = 1; i < num_types; i++) {
        if (strcmp(name, type_table[i].name) == 0) {
            return i;
        }
    }

    return 0;
}

static void qtype_class_base_init(QTypeImpl *base_ti, const char *typename)
{
    QTypeImpl *ti;

    if (!typename) {
        return;
    }

    ti = qtype_get_instance(qtype_get_by_name(typename));

    qtype_class_base_init(base_ti, ti->parent);

    if (ti->base_init) {
        ti->base_init(base_ti->class);
    }
}

static size_t qtype_class_get_size(QTypeImpl *ti)
{
    if (ti->class_size) {
        return ti->class_size;
    }

    if (ti->parent) {
        return qtype_class_get_size(qtype_get_instance(qtype_get_by_name(ti->parent)));
    }

    return sizeof(QObjectClass);
}

static void qtype_class_interface_init(QTypeImpl *ti, QInterfaceImpl *iface)
{
    QTypeInfo info = {
        .instance_size = sizeof(QInterface),
        .parent = iface->parent,
        .class_size = sizeof(QInterfaceClass),
        .class_init = iface->interface_initfn,
        .abstract = true,
    };

    iface->type = qtype_register_anonymous(&info);
}

static void qtype_class_init(QTypeImpl *ti)
{
    size_t class_size = sizeof(QObjectClass);
    int i;

    if (ti->class) {
        return;
    }

    ti->class_size = qtype_class_get_size(ti);

    ti->class = g_malloc0(ti->class_size);
    ti->class->type = ti->type;

    if (ti->parent) {
        QTypeImpl *ti_parent;

        ti_parent = qtype_get_instance(qtype_get_by_name(ti->parent));

        qtype_class_init(ti_parent);

        class_size = ti_parent->class_size;
        assert(ti_parent->class_size <= ti->class_size);

        memcpy((void *)ti->class + sizeof(QObjectClass),
               (void *)ti_parent->class + sizeof(QObjectClass),
               ti_parent->class_size - sizeof(QObjectClass));
    }

    memset((void *)ti->class + class_size, 0, ti->class_size - class_size);

    qtype_class_base_init(ti, ti->parent);

    for (i = 0; i < ti->num_interfaces; i++) {
        qtype_class_interface_init(ti, &ti->interfaces[i]);
    }

    if (ti->class_init) {
        ti->class_init(ti->class);
    }
}

static void qobject_interface_init(QObject *obj, QInterfaceImpl *iface)
{
    QTypeImpl *ti = qtype_get_instance(iface->type);
    QInterface *iface_obj;

    iface_obj = QINTERFACE(qobject_new(ti->name));
    iface_obj->obj = obj;

    obj->interfaces = g_slist_prepend(obj->interfaces, iface_obj);
}

static void qobject_init(QObject *obj, const char *typename)
{
    QTypeImpl *ti = qtype_get_instance(qtype_get_by_name(typename));
    int i;

    if (ti->parent) {
        qobject_init(obj, ti->parent);
    }

    for (i = 0; i < ti->num_interfaces; i++) {
        qobject_interface_init(obj, &ti->interfaces[i]);
    }

    if (ti->instance_init) {
        ti->instance_init(obj);
    }
}

void qobject_initialize(void *data, const char *typename)
{
    QTypeImpl *ti = qtype_get_instance(qtype_get_by_name(typename));
    QObject *obj = data;

    g_assert(ti->instance_size >= sizeof(QObjectClass));

    qtype_class_init(ti);

    g_assert(ti->abstract == false);

    memset(obj, 0, ti->instance_size);

    obj->class = ti->class;

    qobject_init(obj, typename);
}

static void qobject_deinit(QObject *obj, const char *typename)
{
    QTypeImpl *ti = qtype_get_instance(qtype_get_by_name(typename));

    if (ti->instance_finalize) {
        ti->instance_finalize(obj);
    }

    while (obj->interfaces) {
        QInterface *iface_obj = obj->interfaces->data;
        obj->interfaces = g_slist_delete_link(obj->interfaces, obj->interfaces);
        qobject_delete(QOBJECT(iface_obj));
    }

    if (ti->parent) {
        qobject_init(obj, ti->parent);
    }
}

void qobject_finalize(void *data)
{
    QObject *obj = data;
    QTypeImpl *ti = qtype_get_instance(obj->class->type);

    qobject_deinit(obj, ti->name);
}

const char *qtype_get_name(QType type)
{
    QTypeImpl *ti = qtype_get_instance(type);
    return ti->name;
}

QObject *qobject_new(const char *typename)
{
    QTypeImpl *ti = qtype_get_instance(qtype_get_by_name(typename));
    QObject *obj;

    obj = g_malloc(ti->instance_size);
    qobject_initialize(obj, typename);

    return obj;
}

void qobject_delete(QObject *obj)
{
    qobject_finalize(obj);
    g_free(obj);
}

bool qobject_is_type(QObject *obj, const char *typename)
{
    QType target_type = qtype_get_by_name(typename);
    QType type = obj->class->type;
    GSList *i;

    /* Check if typename is a direct ancestor of type */
    while (type) {
        QTypeImpl *ti = qtype_get_instance(type);

        if (ti->type == target_type) {
            return true;
        }

        type = qtype_get_by_name(ti->parent);
    }

    /* Check if obj has an interface of typename */
    for (i = obj->interfaces; i; i = i->next) {
        QInterface *iface = i->data;

        if (qobject_is_type(QOBJECT(iface), typename)) {
            return true;
        }
    }

    return false;
}

QObject *qobject_dynamic_cast(QObject *obj, const char *typename)
{
    GSList *i;

    /* Check if typename is a direct ancestor */
    if (qobject_is_type(obj, typename)) {
        return obj;
    }

    /* Check if obj has an interface of typename */
    for (i = obj->interfaces; i; i = i->next) {
        QInterface *iface = i->data;

        if (qobject_is_type(QOBJECT(iface), typename)) {
            return QOBJECT(iface);
        }
    }

    /* Check if obj is an interface and it's containing object is a direct ancestor of typename */
    if (qobject_is_type(obj, TYPE_QINTERFACE)) {
        QInterface *iface = QINTERFACE(obj);

        if (qobject_is_type(iface->obj, typename)) {
            return iface->obj;
        }
    }

    return NULL;
}


static void register_interface(void)
{
    static QTypeInfo interface_info = {
        .name = TYPE_QINTERFACE,
        .instance_size = sizeof(QInterface),
        .abstract = true,
    };

    qtype_register_static(&interface_info);
}

device_init(register_interface);

QObject *qobject_dynamic_cast_assert(QObject *obj, const char *typename)
{
    QObject *inst;

    inst = qobject_dynamic_cast(obj, typename);

    if (!inst) {
        fprintf(stderr, "Object %p is not an instance of type %s\n", obj, typename);
        abort();
    }

    return inst;
}

QObjectClass *qobject_check_class(QObjectClass *class, const char *typename)
{
    QType target_type = qtype_get_by_name(typename);
    QType type = class->type;

    while (type) {
        QTypeImpl *ti = qtype_get_instance(type);

        if (ti->type == target_type) {
            return class;
        }

        type = qtype_get_by_name(ti->parent);
    }

    fprintf(stderr, "Object %p is not an instance of type %d\n", class, (int)type);
    abort();

    return NULL;
}

const char *qobject_get_type(QObject *obj)
{
    return qtype_get_name(obj->class->type);
}

QObjectClass *qobject_get_class(QObject *obj)
{
    return obj->class;
}

QObjectClass *qobject_get_super(QObject *obj)
{
    return qtype_get_instance(qtype_get_by_name(qtype_get_instance(obj->class->type)->parent))->class;
}

