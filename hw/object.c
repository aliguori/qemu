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

#include "object.h"

#define MAX_INTERFACES 32

typedef struct InterfaceImpl
{
    const char *parent;
    void (*interface_initfn)(ObjectClass *class, void *data);
    Type type;
} InterfaceImpl;

typedef struct TypeImpl
{
    const char *name;
    Type type;

    size_t class_size;

    size_t instance_size;

    void (*base_init)(ObjectClass *klass);
    void (*base_finalize)(ObjectClass *klass);

    void (*class_init)(ObjectClass *klass, void *data);
    void (*class_finalize)(ObjectClass *klass, void *data);

    void *class_data;

    void (*instance_init)(Object *obj);
    void (*instance_finalize)(Object *obj);

    bool abstract;

    const char *parent;

    ObjectClass *class;

    int num_interfaces;
    InterfaceImpl interfaces[MAX_INTERFACES];
} TypeImpl;

#define MAX_TYPES 1024

static int num_types = 1;
static TypeImpl type_table[MAX_TYPES];

Type type_register_static_alias(const TypeInfo *info, const char *alias)
{
    TypeInfo *new_info = g_malloc0(sizeof(*info));

    memcpy(new_info, info, sizeof(*info));
    new_info->name = alias;
    return type_register_static(new_info);
}

Type type_register_static(const TypeInfo *info)
{
    Type type = num_types++;
    TypeImpl *ti;

    assert(num_types < MAX_TYPES);

    ti = &type_table[type];

    assert(info->name != NULL);

    printf("Added type[%ld] %s -> %s\n", type, info->name, info->parent);

    ti->name = info->name;
    ti->parent = info->parent;
    ti->type = type;

    ti->class_size = info->class_size;
    ti->instance_size = info->instance_size;

    ti->base_init = info->base_init;
    ti->base_finalize = info->base_finalize;

    ti->class_init = info->class_init;
    ti->class_finalize = info->class_finalize;
    ti->class_data = info->class_data;

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

static Type type_register_anonymous(const TypeInfo *info)
{
    Type type = num_types++;
    TypeImpl *ti;
    char buffer[32];
    static int count;

    assert(num_types < MAX_TYPES);

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
    ti->class_data = info->class_data;

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

static TypeImpl *type_get_instance(Type type)
{
    assert(type != 0);
    assert(type < num_types);

    return &type_table[type];
}

Type type_get_by_name(const char *name)
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

static void type_class_base_init(TypeImpl *base_ti, const char *typename)
{
    TypeImpl *ti;

    if (!typename) {
        return;
    }

    ti = type_get_instance(type_get_by_name(typename));

    type_class_base_init(base_ti, ti->parent);

    if (ti->base_init) {
        ti->base_init(base_ti->class);
    }
}

static size_t type_class_get_size(TypeImpl *ti)
{
    if (ti->class_size) {
        return ti->class_size;
    }

    if (ti->parent) {
        return type_class_get_size(type_get_instance(type_get_by_name(ti->parent)));
    }

    return sizeof(ObjectClass);
}

static void type_class_interface_init(TypeImpl *ti, InterfaceImpl *iface)
{
    TypeInfo info = {
        .instance_size = sizeof(Interface),
        .parent = iface->parent,
        .class_size = sizeof(InterfaceClass),
        .class_init = iface->interface_initfn,
        .abstract = true,
    };

    iface->type = type_register_anonymous(&info);
}

static void type_class_init(TypeImpl *ti)
{
    size_t class_size = sizeof(ObjectClass);
    int i;

    if (ti->class) {
        return;
    }

    ti->class_size = type_class_get_size(ti);

    ti->class = g_malloc0(ti->class_size);
    ti->class->type = ti->type;

    if (ti->parent) {
        TypeImpl *ti_parent;

        ti_parent = type_get_instance(type_get_by_name(ti->parent));

        type_class_init(ti_parent);

        class_size = ti_parent->class_size;
        assert(ti_parent->class_size <= ti->class_size);

        memcpy((void *)ti->class + sizeof(ObjectClass),
               (void *)ti_parent->class + sizeof(ObjectClass),
               ti_parent->class_size - sizeof(ObjectClass));
    }

    memset((void *)ti->class + class_size, 0, ti->class_size - class_size);

    type_class_base_init(ti, ti->parent);

    for (i = 0; i < ti->num_interfaces; i++) {
        type_class_interface_init(ti, &ti->interfaces[i]);
    }

    if (ti->class_init) {
        ti->class_init(ti->class, ti->class_data);
    }
}

static void object_interface_init(Object *obj, InterfaceImpl *iface)
{
    TypeImpl *ti = type_get_instance(iface->type);
    Interface *iface_obj;

    iface_obj = INTERFACE(object_new(ti->name));
    iface_obj->obj = obj;

    obj->interfaces = g_slist_prepend(obj->interfaces, iface_obj);
}

static void object_init(Object *obj, const char *typename)
{
    TypeImpl *ti = type_get_instance(type_get_by_name(typename));
    int i;

    if (ti->parent) {
        object_init(obj, ti->parent);
    }

    for (i = 0; i < ti->num_interfaces; i++) {
        object_interface_init(obj, &ti->interfaces[i]);
    }

    if (ti->instance_init) {
        ti->instance_init(obj);
    }
}

void object_initialize(void *data, const char *typename)
{
    TypeImpl *ti = type_get_instance(type_get_by_name(typename));
    Object *obj = data;

    g_assert(ti->instance_size >= sizeof(ObjectClass));

    type_class_init(ti);

    g_assert(ti->abstract == false);

    memset(obj, 0, ti->instance_size);

    obj->class = ti->class;

    object_init(obj, typename);
}

static void object_deinit(Object *obj, const char *typename)
{
    TypeImpl *ti = type_get_instance(type_get_by_name(typename));

    if (ti->instance_finalize) {
        ti->instance_finalize(obj);
    }

    while (obj->interfaces) {
        Interface *iface_obj = obj->interfaces->data;
        obj->interfaces = g_slist_delete_link(obj->interfaces, obj->interfaces);
        object_delete(OBJECT(iface_obj));
    }

    if (ti->parent) {
        object_init(obj, ti->parent);
    }
}

void object_finalize(void *data)
{
    Object *obj = data;
    TypeImpl *ti = type_get_instance(obj->class->type);

    object_deinit(obj, ti->name);
}

const char *type_get_name(Type type)
{
    TypeImpl *ti = type_get_instance(type);
    return ti->name;
}

Object *object_new(const char *typename)
{
    TypeImpl *ti = type_get_instance(type_get_by_name(typename));
    Object *obj;

    obj = g_malloc(ti->instance_size);
    object_initialize(obj, typename);

    return obj;
}

void object_delete(Object *obj)
{
    object_finalize(obj);
    g_free(obj);
}

bool object_is_type(Object *obj, const char *typename)
{
    Type target_type = type_get_by_name(typename);
    Type type = obj->class->type;
    GSList *i;

    /* Check if typename is a direct ancestor of type */
    while (type) {
        TypeImpl *ti = type_get_instance(type);

        if (ti->type == target_type) {
            return true;
        }

        type = type_get_by_name(ti->parent);
    }

    /* Check if obj has an interface of typename */
    for (i = obj->interfaces; i; i = i->next) {
        Interface *iface = i->data;

        if (object_is_type(OBJECT(iface), typename)) {
            return true;
        }
    }

    return false;
}

Object *object_dynamic_cast(Object *obj, const char *typename)
{
    GSList *i;

    /* Check if typename is a direct ancestor */
    if (object_is_type(obj, typename)) {
        return obj;
    }

    /* Check if obj has an interface of typename */
    for (i = obj->interfaces; i; i = i->next) {
        Interface *iface = i->data;

        if (object_is_type(OBJECT(iface), typename)) {
            return OBJECT(iface);
        }
    }

    /* Check if obj is an interface and it's containing object is a direct ancestor of typename */
    if (object_is_type(obj, TYPE_INTERFACE)) {
        Interface *iface = INTERFACE(obj);

        if (object_is_type(iface->obj, typename)) {
            return iface->obj;
        }
    }

    return NULL;
}


static void register_interface(void)
{
    static TypeInfo interface_info = {
        .name = TYPE_INTERFACE,
        .instance_size = sizeof(Interface),
        .abstract = true,
    };

    type_register_static(&interface_info);
}

device_init(register_interface);

Object *object_dynamic_cast_assert(Object *obj, const char *typename)
{
    Object *inst;

    inst = object_dynamic_cast(obj, typename);

    if (!inst) {
        fprintf(stderr, "Object %p is not an instance of type %s\n", obj, typename);
        abort();
    }

    return inst;
}

ObjectClass *object_class_dynamic_cast_assert(ObjectClass *class, const char *typename)
{
    if (!object_class_dynamic_cast(class, typename)) {
        fprintf(stderr, "Object %p is not an instance of type %s\n", class, typename);
        abort();
    }

    return class;
}

ObjectClass *object_class_dynamic_cast(ObjectClass *class, const char *typename)
{
    Type target_type = type_get_by_name(typename);
    Type type = class->type;

    while (type) {
        TypeImpl *ti = type_get_instance(type);

        if (ti->type == target_type) {
            return class;
        }

        type = type_get_by_name(ti->parent);
    }

    return NULL;
}

const char *object_get_type(Object *obj)
{
    return type_get_name(obj->class->type);
}

ObjectClass *object_get_class(Object *obj)
{
    return obj->class;
}

ObjectClass *object_get_super(Object *obj)
{
    return type_get_instance(type_get_by_name(type_get_instance(obj->class->type)->parent))->class;
}

ObjectClass *object_class_by_name(const char *typename)
{
    TypeImpl *ti = type_get_instance(type_get_by_name(typename));
    type_class_init(ti);
    return ti->class;
}

const char *object_class_get_name(ObjectClass *klass)
{
    return type_get_name(klass->type);
}

void object_class_foreach(void (*fn)(ObjectClass *klass, void *data),
                          void *data)
{
    int i;

    for (i = 1; i < num_types; i++) {
        if (!type_table[i].abstract) {
            type_class_init(&type_table[i]);
            fn(type_table[i].class, data);
        }
    }
}
