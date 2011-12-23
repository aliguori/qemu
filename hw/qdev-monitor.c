/*
 *  Dynamic device configuration and creation.
 *
 *  Copyright (c) 2009 CodeSourcery
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qdev.h"
#include "monitor.h"

/*
 * Aliases were a bad idea from the start.  Let's keep them
 * from spreading further.
 */
static const char *qdev_class_get_alias(DeviceClass *dc)
{
    const char *typename = object_class_get_name(OBJECT_CLASS(dc));

    if (strcmp(typename, "virtio-blk-pci") == 0) {
        return "virtio-blk";
    } else if (strcmp(typename, "virtio-net-pci") == 0) {
        return "virtio-net";
    } else if (strcmp(typename, "virtio-serial-pci") == 0) {
        return "virtio-serial";
    } else if (strcmp(typename, "virtio-balloon-pci") == 0) {
        return "virtio-balloon";
    } else if (strcmp(typename, "virtio-blk-s390") == 0) {
        return "virtio-blk";
    } else if (strcmp(typename, "virtio-net-s390") == 0) {
        return "virtio-net";
    } else if (strcmp(typename, "virtio-serial-s390") == 0) {
        return "virtio-serial";
    } else if (strcmp(typename, "lsi53c895a") == 0) {
        return "lsi";
    } else if (strcmp(typename, "ich9-ahci") == 0) {
        return "ahci";
    }

    return NULL;
}

static bool qdev_class_has_alias(DeviceClass *dc)
{
    return (qdev_class_get_alias(dc) != NULL);
}

static void qdev_print_devinfo(ObjectClass *klass, void *opaque)
{
    DeviceClass *dc;
    bool *show_no_user = opaque;

    dc = (DeviceClass *)object_class_dynamic_cast(klass, TYPE_DEVICE);

    if (!dc || (show_no_user && !*show_no_user && dc->no_user)) {
        return;
    }

    error_printf("name \"%s\"", object_class_get_name(klass));
    if (dc->bus_type) {
        error_printf(", bus %s", dc->bus_type);
    }
    if (qdev_class_has_alias(dc)) {
        error_printf(", alias \"%s\"", qdev_class_get_alias(dc));
    }
    if (dc->desc) {
        error_printf(", desc \"%s\"", dc->desc);
    }
    if (dc->no_user) {
        error_printf(", no-user");
    }
    error_printf("\n");
}

static int set_property(const char *name, const char *value, void *opaque)
{
    DeviceState *dev = opaque;

    if (strcmp(name, "driver") == 0)
        return 0;
    if (strcmp(name, "bus") == 0)
        return 0;

    if (qdev_prop_parse(dev, name, value) == -1) {
        return -1;
    }
    return 0;
}

static const char *find_typename_by_alias(const char *alias)
{
    /* I don't think s390 aliasing could have ever worked... */

    if (strcmp(alias, "virtio-blk") == 0) {
        return "virtio-blk-pci";
    } else if (strcmp(alias, "virtio-net") == 0) {
        return "virtio-net-pci";
    } else if (strcmp(alias, "virtio-serial") == 0) {
        return "virtio-serial-pci";
    } else if (strcmp(alias, "virtio-balloon") == 0) {
        return "virtio-balloon-pci";
    } else if (strcmp(alias, "lsi") == 0) {
        return "lsi53c895a";
    } else if (strcmp(alias, "ahci") == 0) {
        return "ich9-ahci";
    }

    return NULL;
}

static void display_property(Object *obj, const char *name,
                             const char *typename, bool read_only,
                             void *opaque)
{
    char *legacy_typename;

    if (!strstart(typename, "legacy<", NULL)) {
        return;
    }

    legacy_typename = g_strdup(&typename[7]);
    legacy_typename[strlen(legacy_typename) - 1] = 0;

    /*
     * TODO Properties without a parser are just for dirty hacks.
     * qdev_prop_ptr is the only such PropertyInfo.  It's marked
     * for removal.  This conditional should be removed along with
     * it.
     */
    if (!read_only) {
        error_printf("%s.%s=%s\n", object_get_typename(obj),
                     &name[7], legacy_typename);
    }

    g_free(legacy_typename);
}

int qdev_device_help(QemuOpts *opts)
{
    const char *driver;
    ObjectClass *klass;
    DeviceClass *info;
    Object *obj;

    driver = qemu_opt_get(opts, "driver");
    if (driver && !strcmp(driver, "?")) {
        bool show_no_user = false;
        object_class_foreach(qdev_print_devinfo, TYPE_DEVICE,
                             false, &show_no_user);
        return 1;
    }

    if (!driver || !qemu_opt_get(opts, "?")) {
        return 0;
    }

    klass = object_class_by_name(driver);
    if (!klass) {
        const char *typename = find_typename_by_alias(driver);

        if (typename) {
            driver = typename;
            klass = object_class_by_name(driver);
        }
    }

    if (!klass) {
        return 0;
    }
    info = DEVICE_CLASS(klass);

    obj = object_new(driver);
    object_property_foreach(obj, display_property, NULL);
    object_delete(obj);

    return 1;
}

static Object *qdev_get_peripheral(void)
{
    static Object *obj;

    if (obj == NULL) {
        obj = object_new("container");
        object_property_add_child(object_get_root(), "peripheral",
                                  obj, NULL);
    }

    return obj;
}

static Object *qdev_get_peripheral_anon(void)
{
    static Object *obj;

    if (obj == NULL) {
        obj = object_new("container");
        object_property_add_child(object_get_root(), "peripheral-anon",
                                  obj, NULL);
    }

    return obj;
}

static void qbus_list_bus(DeviceState *dev)
{
    BusState *child;
    const char *sep = " ";

    error_printf("child busses at \"%s\":",
                 dev->id ? dev->id : object_get_typename(OBJECT(dev)));
    QLIST_FOREACH(child, &dev->child_bus, sibling) {
        error_printf("%s\"%s\"", sep, child->name);
        sep = ", ";
    }
    error_printf("\n");
}

static void qbus_list_dev(BusState *bus)
{
    BusChild *kid;
    const char *sep = " ";

    error_printf("devices at \"%s\":", bus->name);
    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;
        error_printf("%s\"%s\"", sep, object_get_typename(OBJECT(dev)));
        if (dev->id)
            error_printf("/\"%s\"", dev->id);
        sep = ", ";
    }
    error_printf("\n");
}

static BusState *qbus_find_bus(DeviceState *dev, char *elem)
{
    BusState *child;

    QLIST_FOREACH(child, &dev->child_bus, sibling) {
        if (strcmp(child->name, elem) == 0) {
            return child;
        }
    }
    return NULL;
}

static DeviceState *qbus_find_dev(BusState *bus, char *elem)
{
    BusChild *kid;

    /*
     * try to match in order:
     *   (1) instance id, if present
     *   (2) driver name
     *   (3) driver alias, if present
     */
    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;
        if (dev->id  &&  strcmp(dev->id, elem) == 0) {
            return dev;
        }
    }
    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;
        if (strcmp(object_get_typename(OBJECT(dev)), elem) == 0) {
            return dev;
        }
    }
    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;
        DeviceClass *dc = DEVICE_GET_CLASS(dev);

        if (qdev_class_has_alias(dc) &&
            strcmp(qdev_class_get_alias(dc), elem) == 0) {
            return dev;
        }
    }
    return NULL;
}

static BusState *qbus_find_recursive(BusState *bus, const char *name,
                                     const char *bus_typename)
{
    BusChild *kid;
    BusState *child, *ret;
    int match = 1;

    if (name && (strcmp(bus->name, name) != 0)) {
        match = 0;
    }
    if (bus_typename &&
        (strcmp(object_get_typename(OBJECT(bus)), bus_typename) != 0)) {
        match = 0;
    }
    if (match) {
        return bus;
    }

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;
        QLIST_FOREACH(child, &dev->child_bus, sibling) {
            ret = qbus_find_recursive(child, name, bus_typename);
            if (ret) {
                return ret;
            }
        }
    }
    return NULL;
}

static BusState *qbus_find(const char *path)
{
    DeviceState *dev;
    BusState *bus;
    char elem[128];
    int pos, len;

    /* find start element */
    if (path[0] == '/') {
        bus = sysbus_get_default();
        pos = 0;
    } else {
        if (sscanf(path, "%127[^/]%n", elem, &len) != 1) {
            assert(!path[0]);
            elem[0] = len = 0;
        }
        bus = qbus_find_recursive(sysbus_get_default(), elem, NULL);
        if (!bus) {
            qerror_report(QERR_BUS_NOT_FOUND, elem);
            return NULL;
        }
        pos = len;
    }

    for (;;) {
        assert(path[pos] == '/' || !path[pos]);
        while (path[pos] == '/') {
            pos++;
        }
        if (path[pos] == '\0') {
            return bus;
        }

        /* find device */
        if (sscanf(path+pos, "%127[^/]%n", elem, &len) != 1) {
            assert(0);
            elem[0] = len = 0;
        }
        pos += len;
        dev = qbus_find_dev(bus, elem);
        if (!dev) {
            qerror_report(QERR_DEVICE_NOT_FOUND, elem);
            if (!monitor_cur_is_qmp()) {
                qbus_list_dev(bus);
            }
            return NULL;
        }

        assert(path[pos] == '/' || !path[pos]);
        while (path[pos] == '/') {
            pos++;
        }
        if (path[pos] == '\0') {
            /* last specified element is a device.  If it has exactly
             * one child bus accept it nevertheless */
            switch (dev->num_child_bus) {
            case 0:
                qerror_report(QERR_DEVICE_NO_BUS, elem);
                return NULL;
            case 1:
                return QLIST_FIRST(&dev->child_bus);
            default:
                qerror_report(QERR_DEVICE_MULTIPLE_BUSSES, elem);
                if (!monitor_cur_is_qmp()) {
                    qbus_list_bus(dev);
                }
                return NULL;
            }
        }

        /* find bus */
        if (sscanf(path+pos, "%127[^/]%n", elem, &len) != 1) {
            assert(0);
            elem[0] = len = 0;
        }
        pos += len;
        bus = qbus_find_bus(dev, elem);
        if (!bus) {
            qerror_report(QERR_BUS_NOT_FOUND, elem);
            if (!monitor_cur_is_qmp()) {
                qbus_list_bus(dev);
            }
            return NULL;
        }
    }
}

DeviceState *qdev_device_add(QemuOpts *opts)
{
    ObjectClass *obj;
    DeviceClass *k;
    const char *driver, *path, *id;
    DeviceState *qdev;
    BusState *bus;

    driver = qemu_opt_get(opts, "driver");
    if (!driver) {
        qerror_report(QERR_MISSING_PARAMETER, "driver");
        return NULL;
    }

    /* find driver */
    obj = object_class_by_name(driver);
    if (!obj) {
        const char *typename = find_typename_by_alias(driver);

        if (typename) {
            driver = typename;
            obj = object_class_by_name(driver);
        }
    }

    if (!obj) {
        qerror_report(QERR_INVALID_PARAMETER_VALUE, "driver", "device type");
        return NULL;
    }

    k = DEVICE_CLASS(obj);

    /* find bus */
    path = qemu_opt_get(opts, "bus");
    if (path != NULL) {
        bus = qbus_find(path);
        if (!bus) {
            return NULL;
        }
        if (strcmp(object_get_typename(OBJECT(bus)), k->bus_type) != 0){
            qerror_report(QERR_BAD_BUS_FOR_DEVICE,
                          driver, object_get_typename(OBJECT(bus)));
            return NULL;
        }
    } else {
        bus = qbus_find_recursive(sysbus_get_default(), NULL, k->bus_type);
        if (!bus) {
            qerror_report(QERR_NO_BUS_FOR_DEVICE,
                          driver, k->bus_type);
            return NULL;
        }
    }
    if (qdev_hotplug && !bus->allow_hotplug) {
        qerror_report(QERR_BUS_NO_HOTPLUG, bus->name);
        return NULL;
    }

    if (!bus) {
        bus = sysbus_get_default();
    }

    /* create device, set properties */
    qdev = DEVICE(object_new(driver));
    qdev_set_parent_bus(qdev, bus);
    qdev_prop_set_globals(qdev);

    id = qemu_opts_id(opts);
    if (id) {
        qdev->id = id;
        object_property_add_child(qdev_get_peripheral(), qdev->id,
                                  OBJECT(qdev), NULL);
    } else {
        static int anon_count;
        gchar *name = g_strdup_printf("device[%d]", anon_count++);
        object_property_add_child(qdev_get_peripheral_anon(), name,
                                  OBJECT(qdev), NULL);
        g_free(name);
    }        
    if (qemu_opt_foreach(opts, set_property, qdev, 1) != 0) {
        qdev_free(qdev);
        return NULL;
    }
    if (qdev_init(qdev) < 0) {
        qerror_report(QERR_DEVICE_INIT_FAILED, driver);
        return NULL;
    }
    qdev->opts = opts;
    return qdev;
}


#define qdev_printf(fmt, ...) monitor_printf(mon, "%*s" fmt, indent, "", ## __VA_ARGS__)
static void qbus_print(Monitor *mon, BusState *bus, int indent);

typedef struct QTreePrinter
{
    Visitor visitor;
    Monitor *mon;
    int indent;
    const char *prefix;
} QTreePrinter;

static void qtree_printer_visit_str(Visitor *v, char **obj,
                                    const char *name, Error **errp)
{
    QTreePrinter *p = (QTreePrinter *)v;

    monitor_printf(p->mon, "%*sdev-prop: %s = %s\n",
                   p->indent, "", &name[7], *obj);
}

static void qdev_print_prop(Object *obj, const char *name,
                             const char *typename, bool read_only,
                             void *opaque)
{
    QTreePrinter *printer = opaque;

    if (strstart(typename, "legacy<", NULL)) {
        object_property_get(obj, &printer->visitor, name, NULL);
    }
}

static void qdev_print_dev(DeviceState *dev, Monitor *mon, int indent)
{
    DeviceClass *dc = DEVICE_GET_CLASS(dev);

    if (dc->print_dev) {
        dc->print_dev(dev, mon, indent);
    }
}

static void qdev_print(Monitor *mon, DeviceState *dev, int indent)
{
    QTreePrinter printer = {
        .visitor.type_str = qtree_printer_visit_str,
        .mon = mon,
        .indent = indent + 2,
    };
    BusState *child;

    qdev_printf("dev: %s, id \"%s\"\n", object_get_typename(OBJECT(dev)),
                dev->id ? dev->id : "");
    if (dev->num_gpio_in) {
        qdev_printf("gpio-in %d\n", dev->num_gpio_in);
    }
    if (dev->num_gpio_out) {
        qdev_printf("gpio-out %d\n", dev->num_gpio_out);
    }
    object_property_foreach(OBJECT(dev), qdev_print_prop, &printer);
    qdev_print_dev(dev, mon, indent + 2);
    QLIST_FOREACH(child, &dev->child_bus, sibling) {
        qbus_print(mon, child, indent + 2);
    }
}

static void qbus_print(Monitor *mon, BusState *bus, int indent)
{
    BusChild *kid;

    qdev_printf("bus: %s\n", bus->name);
    indent += 2;
    qdev_printf("type %s\n", object_get_typename(OBJECT(bus)));
    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;
        qdev_print(mon, dev, indent);
    }
}
#undef qdev_printf

void do_info_qtree(Monitor *mon)
{
    if (sysbus_get_default())
        qbus_print(mon, sysbus_get_default(), 0);
}

void do_info_qdm(Monitor *mon)
{
    object_class_foreach(qdev_print_devinfo, TYPE_DEVICE, false, NULL);
}

int do_device_add(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    QemuOpts *opts;

    opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict);
    if (!opts) {
        return -1;
    }
    if (!monitor_cur_is_qmp() && qdev_device_help(opts)) {
        qemu_opts_del(opts);
        return 0;
    }
    if (!qdev_device_add(opts)) {
        qemu_opts_del(opts);
        return -1;
    }
    return 0;
}

int do_device_del(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *id = qdict_get_str(qdict, "id");
    DeviceState *dev;

    dev = qdev_find_recursive(sysbus_get_default(), id);
    if (NULL == dev) {
        qerror_report(QERR_DEVICE_NOT_FOUND, id);
        return -1;
    }
    return qdev_unplug(dev);
}

void qdev_machine_init(void)
{
    qdev_get_peripheral_anon();
    qdev_get_peripheral();
}
