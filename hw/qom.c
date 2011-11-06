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
 *
 */
#include "qdev-qom.h"

void qdev_property_add(DeviceState *dev, const char *name, const char *type,
                       DevicePropertyEtter *get, DevicePropertyEtter *set,
                       DevicePropertyRelease *release, void *opaque,
                       Error **errp)
{
    DeviceProperty *prop = g_malloc0(sizeof(*prop));

    prop->name = g_strdup(name);
    prop->type = g_strdup(type);
    prop->get = get;
    prop->set = set;
    prop->release = release;
    prop->opaque = opaque;

    dev->properties = g_slist_append(dev->properties, prop);
}

static DeviceProperty *qdev_property_find(DeviceState *dev, const char *name)
{
    GSList *i;

    for (i = dev->properties; i; i = i->next) {
        DeviceProperty *prop = i->data;

        if (strcmp(prop->name, name) == 0) {
            return prop;
        }
    }

    return NULL;
}

void qdev_property_get(DeviceState *dev, Visitor *v, const char *name,
                       Error **errp)
{
    DeviceProperty *prop = qdev_property_find(dev, name);

    if (prop == NULL) {
        error_set(errp, QERR_PROPERTY_NOT_FOUND, dev->id?:"", name);
        return;
    }

    if (!prop->get) {
        error_set(errp, QERR_PERMISSION_DENIED);
    } else {
        prop->get(dev, v, prop->opaque, name, errp);
    }
}

void qdev_property_set(DeviceState *dev, Visitor *v, const char *name,
                       Error **errp)
{
    DeviceProperty *prop = qdev_property_find(dev, name);

    if (prop == NULL) {
        error_set(errp, QERR_PROPERTY_NOT_FOUND, dev->id?:"", name);
        return;
    }

    if (!prop->set) {
        error_set(errp, QERR_PERMISSION_DENIED);
    } else {
        prop->set(dev, prop->opaque, v, name, errp);
    }
}

const char *qdev_property_get_type(DeviceState *dev, const char *name, Error **errp)
{
    DeviceProperty *prop = qdev_property_find(dev, name);

    if (prop == NULL) {
        error_set(errp, QERR_PROPERTY_NOT_FOUND, dev->id?:"", name);
        return NULL;
    }

    return prop->type;
}

/**
 * Legacy property handling
 */

static void qdev_get_legacy_property(DeviceState *dev, Visitor *v, void *opaque,
                                     const char *name, Error **errp)
{
    Property *prop = opaque;

    if (prop->info->print) {
        char buffer[1024];
        char *ptr = buffer;

        prop->info->print(dev, prop, buffer, sizeof(buffer));
        visit_type_str(v, &ptr, name, errp);
    } else {
        error_set(errp, QERR_PERMISSION_DENIED);
    }
}

static void qdev_set_legacy_property(DeviceState *dev, Visitor *v, void *opaque,
                                     const char *name, Error **errp)
{
    Property *prop = opaque;

    if (dev->state != DEV_STATE_CREATED) {
        error_set(errp, QERR_PERMISSION_DENIED);
        return;
    }

    if (prop->info->parse) {
        Error *local_err = NULL;
        char *ptr = NULL;

        visit_type_str(v, &ptr, name, &local_err);
        if (!local_err) {
            int ret;
            ret = prop->info->parse(dev, prop, ptr);
            if (ret != 0) {
                error_set(errp, QERR_INVALID_PARAMETER_VALUE,
                          name, prop->info->name);
            }
            g_free(ptr);
        } else {
            error_propagate(errp, local_err);
        }
    } else {
        error_set(errp, QERR_PERMISSION_DENIED);
    }
}

/**
 * @qdev_add_legacy_property - adds a legacy property
 *
 * Do not use this is new code!  Properties added through this interface will
 * be given types in the "legacy<>" type namespace.
 *
 * Legacy properties are always processed as strings.  The format of the string
 * depends on the property type.
 */
void qdev_property_add_legacy(DeviceState *dev, Property *prop,
                              Error **errp)
{
    gchar *type;

    type = g_strdup_printf("legacy<%s>", prop->info->name);

    qdev_property_add(dev, prop->name, type,
                      qdev_get_legacy_property,
                      qdev_set_legacy_property,
                      NULL,
                      prop, errp);

    g_free(type);
}

DeviceState *qdev_get_root(void)
{
    static DeviceState *qdev_root;

    if (!qdev_root) {
        qdev_root = qdev_create(NULL, "container");
        qdev_init_nofail(qdev_root);
    }

    return qdev_root;
}

static void qdev_get_child_property(DeviceState *dev, Visitor *v, void *opaque,
                                    const char *name, Error **errp)
{
    DeviceState *child = opaque;
    gchar *path;

    path = qdev_get_canonical_path(child);
    visit_type_str(v, &path, name, errp);
    g_free(path);
}

void qdev_property_add_child(DeviceState *dev, const char *name,
                             DeviceState *child, Error **errp)
{
    gchar *type;

    type = g_strdup_printf("child<%s>", child->info->name);

    qdev_property_add(dev, name, type, qdev_get_child_property,
                      NULL, NULL, child, errp);

    g_assert(child->parent == NULL);
    child->parent = dev;

    g_free(type);
}

static void qdev_get_link_property(DeviceState *dev, Visitor *v, void *opaque,
                                   const char *name, Error **errp)
{
    DeviceState **child = opaque;
    gchar *path;

    if (*child) {
        path = qdev_get_canonical_path(*child);
        visit_type_str(v, &path, name, errp);
        g_free(path);
    } else {
        path = (gchar *)"";
        visit_type_str(v, &path, name, errp);
    }
}

static void qdev_set_link_property(DeviceState *dev, Visitor *v, void *opaque,
                                   const char *name, Error **errp)
{
    DeviceState **child = opaque;
    bool ambiguous = false;
    const char *type;
    char *path;

    type = qdev_property_get_type(dev, name, NULL);

    visit_type_str(v, &path, name, errp);

    if (strcmp(path, "") != 0) {
        DeviceState *target;

        target = qdev_resolve_path(path, &ambiguous);
        if (target) {
            gchar *target_type;

            target_type = g_strdup_printf("link<%s>", target->info->name);
            if (strcmp(target_type, type) == 0) {
                *child = target;
            } else {
                error_set(errp, QERR_INVALID_PARAMETER_TYPE, name, type);
            }

            g_free(target_type);
        } else {
            error_set(errp, QERR_DEVICE_NOT_FOUND, path);
        }
    } else {
        *child = NULL;
    }

    g_free(path);
}

void qdev_property_add_link(DeviceState *dev, const char *name,
                            const char *type, DeviceState **child,
                            Error **errp)
{
    gchar *full_type;

    full_type = g_strdup_printf("link<%s>", type);

    qdev_property_add(dev, name, full_type,
                      qdev_get_link_property,
                      qdev_set_link_property,
                      NULL, child, errp);

    g_free(full_type);
}

gchar *qdev_get_canonical_path(DeviceState *dev)
{
    DeviceState *root = qdev_get_root();
    char *newpath = NULL, *path = NULL;

    while (dev != root) {
        GSList *i;

        g_assert(dev->parent != NULL);

        for (i = dev->parent->properties; i; i = i->next) {
            DeviceProperty *prop = i->data;

            if (!strstart(prop->type, "child<", NULL)) {
                continue;
            }

            if (prop->opaque == dev) {
                if (path) {
                    newpath = g_strdup_printf("%s/%s", prop->name, path);
                    g_free(path);
                    path = newpath;
                } else {
                    path = g_strdup(prop->name);
                }
                break;
            }
        }

        g_assert(i != NULL);

        dev = dev->parent;
    }

    newpath = g_strdup_printf("/%s", path);
    g_free(path);

    return newpath;
}

static DeviceState *qdev_resolve_abs_path(DeviceState *parent,
                                          gchar **parts,
                                          int index)
{
    DeviceProperty *prop;
    DeviceState *child;

    if (parts[index] == NULL) {
        return parent;
    }

    if (strcmp(parts[index], "") == 0) {
        return qdev_resolve_abs_path(parent, parts, index + 1);
    }

    prop = qdev_property_find(parent, parts[index]);
    if (prop == NULL) {
        return NULL;
    }

    child = NULL;
    if (strstart(prop->type, "link<", NULL)) {
        DeviceState **pchild = prop->opaque;
        if (*pchild) {
            child = *pchild;
        }
    } else if (strstart(prop->type, "child<", NULL)) {
        child = prop->opaque;
    }

    if (!child) {
        return NULL;
    }

    return qdev_resolve_abs_path(child, parts, index + 1);
}

static DeviceState *qdev_resolve_partial_path(DeviceState *parent,
                                              gchar **parts,
                                              bool *ambiguous)
{
    DeviceState *dev;
    GSList *i;

    dev = qdev_resolve_abs_path(parent, parts, 0);

    for (i = parent->properties; i; i = i->next) {
        DeviceProperty *prop = i->data;
        DeviceState *found;

        if (!strstart(prop->type, "child<", NULL)) {
            continue;
        }

        found = qdev_resolve_partial_path(prop->opaque, parts, ambiguous);
        if (found) {
            if (dev) {
                if (ambiguous) {
                    *ambiguous = true;
                }
                return NULL;
            }
            dev = found;
        }

        if (ambiguous && *ambiguous) {
            return NULL;
        }
    }

    return dev;
}

DeviceState *qdev_resolve_path(const char *path, bool *ambiguous)
{
    bool partial_path = true;
    DeviceState *dev;
    gchar **parts;

    parts = g_strsplit(path, "/", 0);
    if (parts == NULL || parts[0] == NULL) {
        return qdev_get_root();
    }

    if (strcmp(parts[0], "") == 0) {
        partial_path = false;
    }

    if (partial_path) {
        dev = qdev_resolve_partial_path(qdev_get_root(), parts, ambiguous);
    } else {
        dev = qdev_resolve_abs_path(qdev_get_root(), parts, 1);
    }

    g_strfreev(parts);

    return dev;
}
