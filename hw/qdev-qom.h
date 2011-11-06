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

#ifndef QDEV_QOM_H
#define QDEV_QOM_H

#include "qdev.h"

/**
 * Do not include this file directly.  It's meant to be consumed through qdev.h.
 *
 * These functions are split out to isolate the legacy qdev code to make it
 * easier to remove in the future.
 */

/**
 * @qdev_property_add - add a new property to a device
 *
 * @dev - the device to add a property to
 *
 * @name - the name of the property.  This can contain any character except for
 *         a forward slash.  In general, you should use hyphens '-' instead of
 *         underscores '_' when naming properties.
 *
 * @type - the type name of the property.  This namespace is pretty loosely
 *         defined.  Sub namespaces are constructed by using a prefix and then
 *         to angle brackets.  For instance, the type 'virtio-net-pci' in the
 *         'link' namespace would be 'link<virtio-net-pci>'.
 *
 * @get - the getter to be called to read a property.  If this is NULL, then
 *        the property cannot be read.
 *
 * @set - the setter to be called to write a property.  If this is NULL, then
 *        the property cannot be written.
 *
 * @release - called when the property is removed from the device.  This is
 *            meant to allow a property to free its opaque upon device
 *            destruction.  This may be NULL.
 *
 * @opaque - this is user data passed to @get, @set, and @release
 *
 * @errp - returns an error if this function fails
 */
void qdev_property_add(DeviceState *dev, const char *name, const char *type,
                       DevicePropertyEtter *get, DevicePropertyEtter *set,
                       DevicePropertyRelease *release, void *opaque,
                       Error **errp);


/**
 * @qdev_property_get - reads a property from a device
 *
 * @dev - the device
 *
 * @v - the visitor that will receive the property value.  This should be an
 *      Output visitor and the data will be written with @name as the name.
 *
 * @name - the name of the property
 *
 * @errp - returns an error if this function fails
 */
void qdev_property_get(DeviceState *dev, Visitor *v, const char *name,
                       Error **errp);

/**
 * @qdev_property_set - writes a property to a device
 *
 * @dev - the device
 *
 * @v - the visitor that will used to write the property value.  This should be
 *      an Input visitor and the data will be first read with @name as the name
 *      and then written as the property value.
 *
 * @name - the name of the property
 *
 * @errp - returns an error if this function fails
 */
void qdev_property_set(DeviceState *dev, Visitor *v, const char *name,
                       Error **errp);

/**
 * @qdev_property_get_type - returns the type of a property
 *
 * @dev - the device
 *
 * @name - the name of the property
 *
 * @errp - returns an error if this function fails
 *
 * Returns:
 *   The type name of the property.
 */
const char *qdev_property_get_type(DeviceState *dev, const char *name,
                                   Error **errp);

/**
 * @qdev_property_add_legacy - add a legacy @Property to a device
 *
 * DO NOT USE THIS IN NEW CODE!
 */
void qdev_property_add_legacy(DeviceState *dev, Property *prop, Error **errp);

/**
 * @qdev_get_root - returns the root device of the composition tree
 *
 * Returns:
 *   The root of the composition tree.
 */
DeviceState *qdev_get_root(void);

/**
 * @qdev_get_canonical_path - returns the canonical path for a device.  This
 * is the path within the composition tree starting from the root.
 *
 * Returns:
 *   The canonical path in the composition tree.
 */
gchar *qdev_get_canonical_path(DeviceState *dev);

/**
 * @qdev_resolve_path - resolves a path returning a device
 *
 * There are two types of supported paths--absolute paths and partial paths.
 * 
 * Absolute paths are derived from the root device and can follow child<> or
 * link<> properties.  Since they can follow link<> properties, they can be
 * arbitrarily long.  Absolute paths look like absolute filenames and are prefix
 * with a leading slash.
 * 
 * Partial paths are look like relative filenames.  They do not begin with a
 * prefix.  The matching rules for partial paths are subtle but designed to make
 * specifying devices easy.  At each level of the composition tree, the partial
 * path is matched as an absolute path.  The first match is not returned.  At
 * least two matches are searched for.  A successful result is only returned if
 * only one match is founded.  If more than one match is found, a flag is return
 * to indicate that the match was ambiguous.
 *
 * @path - the path to resolve
 *
 * @ambiguous - returns true if the path resolution failed because of an
 *              ambiguous match
 *
 * Returns:
 *   The matched device.
 */
DeviceState *qdev_resolve_path(const char *path, bool *ambiguous);

/**
 * @qdev_property_add_child - Add a child property to a device
 *
 * Child properties form the composition tree.  All devices need to be a child
 * of another device.  Devices can only be a child of one device.
 *
 * There is no way for a child to determine what it's parent is.  It is not
 * a bidirectional relationship.  This is by design.
 *
 * @dev - the device to add a property to
 *
 * @name - the name of the property
 *
 * @child - the child device
 *
 * @errp - if an error occurs, a pointer to an area to store the area
 */
void qdev_property_add_child(DeviceState *dev, const char *name,
                             DeviceState *child, Error **errp);

/**
 * @qdev_property_add_link - Add a link property to a device
 *
 * Links establish relationships between devices.  Links are unidirection
 * although two links can be combined to form a bidirectional relationship
 * between devices.
 *
 * Links form the graph in the device model.
 *
 * @dev - the device to add a property to
 *
 * @name - the name of the property
 *
 * @type - the qdev type of the link
 *
 * @child - a pointer to where the link device reference is stored
 *
 * @errp - if an error occurs, a pointer to an area to store the area
 */
void qdev_property_add_link(DeviceState *dev, const char *name,
                            const char *type, DeviceState **child,
                            Error **errp);
#endif
