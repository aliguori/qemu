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

#ifndef QEMU_OBJECT_H
#define QEMU_OBJECT_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include "qemu-queue.h"

#include "qemu/visitor.h"
#include "qemu/error.h"

struct TypeImpl;
typedef struct TypeImpl *Type;

typedef struct ObjectClass ObjectClass;
typedef struct Object Object;

typedef struct TypeInfo TypeInfo;

typedef struct InterfaceClass InterfaceClass;
typedef struct InterfaceInfo InterfaceInfo;

#define TYPE_OBJECT NULL

/**
 * SECTION:object.h
 * @title:Base Object Type System
 * @short_description: interfaces for creating new types and objects
 *
 * The QEMU Object Model provides a framework for registering user creatable
 * types and instantiating objects from those types.  QOM provides the following
 * features:
 *
 *  - System for dynamically registering types
 *  - Support for single-inheritance of types
 *  - Multiple inheritance of stateless interfaces
 *
 * <example>
 *   <title>Creating a minimal type</title>
 *   <programlisting>
 * #include "qdev.h"
 *
 * #define TYPE_MY_DEVICE "my-device"
 *
 * typedef struct MyDevice
 * {
 *     DeviceState parent;
 *
 *     int reg0, reg1, reg2;
 * } MyDevice;
 *
 * static TypeInfo my_device_info = {
 *     .name = TYPE_MY_DEVICE,
 *     .parent = TYPE_DEVICE,
 *     .instance_size = sizeof(MyDevice),
 * };
 *
 * static void my_device_module_init(void)
 * {
 *     type_register_static(&my_device_info);
 * }
 *
 * device_init(my_device_module_init);
 *   </programlisting>
 * </example>
 *
 * In the above example, we create a simple type that is described by #TypeInfo.
 * #TypeInfo describes information about the type including what it inherits
 * from, the instance and class size, and constructor/destructor hooks.
 *
 * Every type has an #ObjectClass associated with it.  #ObjectClass derivatives
 * are instantiated dynamically but there is only ever one instance for any
 * given type.  The #ObjectClass typically holds a table of function pointers
 * for the virtual methods implemented by this type.
 *
 * Using object_new(), a new #Object derivative will be instantiated.  You can
 * cast an #Object to a subclass (or base-class) type using
 * object_dynamic_cast().  You typically want to define a macro wrapper around
 * object_dynamic_cast_assert() to make it easier to convert to a specific type.
 *
 * # Class Initialization #
 *
 * Before an object is initialized, the class for the object must be
 * initialized.  There is only one class object for all instance objects
 * that is created lazily.
 *
 * Classes are initialized by first initializing any parent classes (if
 * necessary).  After the parent class object has initialized, it will be
 * copied into the current class object and any additional storage in the
 * class object is zero filled.
 *
 * The effect of this is that classes automatically inherit any virtual
 * function pointers that the parent class has already initialized.  All
 * other fields will be zero filled.
 *
 * Once all of the parent classes have been initialized, #TypeInfo::class_init
 * is called to let the class being instantiated provide default initialize for
 * it's virtual functions.
 *
 * # Interfaces #
 *
 * Interfaces allow a limited form of multiple inheritance.  Instances are
 * similar to normal types except for the fact that are only defined by
 * their classes and never carry any state.  You can dynamically cast an object
 * to one of its #Interface types and vice versa.
 */


/**
 * ObjectPropertyAccessor:
 * @obj: the object that owns the property
 * @v: the visitor that contains the property data
 * @opaque: the object property opaque
 * @name: the name of the property
 * @errp: a pointer to an Error that is filled if getting/setting fails.
 *
 * Called when trying to get/set a property.
 */
typedef void (ObjectPropertyAccessor)(Object *obj,
                                      Visitor *v,
                                      void *opaque,
                                      const char *name,
                                      Error **errp);

/**
 * ObjectPropertyRelease:
 * @obj: the object that owns the property
 * @name: the name of the property
 * @opaque: the opaque registered with the property
 *
 * Called when a property is removed from a object.
 */
typedef void (ObjectPropertyRelease)(Object *obj,
                                     const char *name,
                                     void *opaque);

typedef struct ObjectProperty
{
    gchar *name;
    gchar *type;
    ObjectPropertyAccessor *get;
    ObjectPropertyAccessor *set;
    ObjectPropertyRelease *release;
    void *opaque;

    QTAILQ_ENTRY(ObjectProperty) node;
} ObjectProperty;

/**
 * ObjectClass:
 *
 * The base for all classes.  The only thing that #ObjectClass contains is an
 * integer type handle.
 */
struct ObjectClass
{
    /*< private >*/
    Type type;
};

/**
 * Object:
 *
 * The base for all objects.  The first member of this object is a pointer to
 * a #ObjectClass.  Since C guarantees that the first member of a structure
 * always begins at byte 0 of that structure, as long as any sub-object places
 * its parent as the first member, we can cast directly to a #Object.
 *
 * As a result, #Object contains a reference to the objects type as its
 * first member.  This allows identification of the real type of the object at
 * run time.
 *
 * #Object also contains a list of #Interfaces that this object
 * implements.
 */
struct Object
{
    /*< private >*/
    ObjectClass *class;
    GSList *interfaces;
    QTAILQ_HEAD(, ObjectProperty) properties;
    uint32_t ref;
    Object *parent;
};

/**
 * TypeInfo:
 * @name: The name of the type.
 * @parent: The name of the parent type.
 * @instance_size: The size of the object (derivative of #Object).  If
 *   @instance_size is 0, then the size of the object will be the size of the
 *   parent object.
 * @instance_init: This function is called to initialize an object.  The parent
 *   class will have already been initialized so the type is only responsible
 *   for initializing its own members.
 * @instance_finalize: This function is called during object destruction.  This
 *   is called before the parent @instance_finalize function has been called.
 *   An object should only free the members that are unique to its type in this
 *   function.
 * @abstract: If this field is true, then the class is considered abstract and
 *   cannot be directly instantiated.
 * @class_size: The size of the class object (derivative of #ObjectClass)
 *   for this object.  If @class_size is 0, then the size of the class will be
 *   assumed to be the size of the parent class.  This allows a type to avoid
 *   implementing an explicit class type if they are not adding additional
 *   virtual functions.
 * @class_init: This function is called after all parent class initialization
 *   has occured to allow a class to set its default virtual method pointers.  
 *   This is also the function to use to override virtual methods from a parent
 *   class.
 * @class_finalize: This function is called during class destruction and is
 *   meant to release and dynamic parameters allocated by @class_init.
 * @class_data: Data to pass to the @class_init and @class_finalize functions.
 *   This can be useful when building dynamic classes.
 * @interfaces: The list of interfaces associated with this type.  This
 *   should point to a static array that's terminated with a zero filled
 *   element.
 */
struct TypeInfo
{
    const char *name;
    const char *parent;

    size_t instance_size;
    void (*instance_init)(Object *obj);
    void (*instance_finalize)(Object *obj);

    bool abstract;
    size_t class_size;

    void (*class_init)(ObjectClass *klass, void *data);
    void (*class_finalize)(ObjectClass *klass, void *data);
    void *class_data;

    InterfaceInfo *interfaces;
};

/**
 * OBJECT:
 * @obj: A derivative of #Object
 *
 * Converts an object to a #Object.  Since all objects are #Objects,
 * this function will always succeed.
 */
#define OBJECT(obj) \
    ((Object *)(obj))

/**
 * OBJECT_CHECK:
 * @type: The C type to use for the return value.
 * @obj: A derivative of @type to cast.
 * @name: The QOM typename of @type
 *
 * A type safe version of @object_dynamic_cast_assert.  Typically each class
 * will define a macro based on this type to perform type safe dynamic_casts to
 * this object type.
 *
 * If an invalid object is passed to this function, a run time assert will be
 * generated.
 */
#define OBJECT_CHECK(type, obj, name) \
    ((type *)object_dynamic_cast_assert((Object *)(obj), (name)))

/**
 * OBJECT_CLASS_CHECK:
 * @class: The C type to use for the return value.
 * @obj: A derivative of @type to cast.
 * @name: the QOM typename of @class.
 *
 * A type safe version of @object_check_class.  This macro is typically wrapped
 * by each type to perform type safe casts of a class to a specific class type.
 */
#define OBJECT_CLASS_CHECK(class, obj, name) \
    ((class *)object_class_dynamic_cast_assert((ObjectClass *)(obj), (name)))

/**
 * OBJECT_GET_CLASS:
 * @class: The C type to use for the return value.
 * @obj: The object to obtain the class for.
 * @name: The QOM typename of @obj.
 *
 * This function will return a specific class for a given object.  Its generally
 * used by each type to provide a type safe macro to get a specific class type
 * from an object.
 */
#define OBJECT_GET_CLASS(class, obj, name) \
    OBJECT_CLASS_CHECK(class, object_get_class(OBJECT(obj)), name)

#define OBJECT_CLASS(class) \
    ((ObjectClass *)(class))

/**
 * InterfaceClass:
 * @parent_class: the base class
 *
 * The class for all interfaces.  Subclasses of this class should only add
 * virtual methods.
 */
struct InterfaceClass
{
    ObjectClass parent_class;
};

/**
 * InterfaceInfo:
 * @type: The name of the interface.
 * @interface_initfn: This method is called during class initialization and is
 *   used to initialize an interface associated with a class.  This function
 *   should initialize any default virtual functions for a class and/or override
 *   virtual functions in a parent class.
 *
 * The information associated with an interface.
 */
struct InterfaceInfo
{
    const char *type;

    void (*interface_initfn)(ObjectClass *class, void *data);
};

#define TYPE_INTERFACE "interface"

/**
 * object_new:
 * @typename: The name of the type of the object to instantiate.
 *
 * This function will initialize a new object using heap allocated memory.  This
 * function should be paired with object_delete() to free the resources
 * associated with the object.
 *
 * Returns: The newly allocated and instantiated object.
 */
Object *object_new(const char *typename);

/**
 * object_new_with_type:
 * @type: The type of the object to instantiate.
 *
 * This function will initialize a new object using heap allocated memory.  This
 * function should be paired with object_delete() to free the resources
 * associated with the object.
 *
 * Returns: The newly allocated and instantiated object.
 */
Object *object_new_with_type(Type type);

/**
 * object_delete:
 * @obj: The object to free.
 *
 * Finalize an object and then free the memory associated with it.  This should
 * be paired with object_new() to free the resources associated with an object.
 */
void object_delete(Object *obj);

/**
 * object_initialize_with_type:
 * @obj: A pointer to the memory to be used for the object.
 * @type: The type of the object to instantiate.
 *
 * This function will initialize an object.  The memory for the object should
 * have already been allocated.
 */
void object_initialize_with_type(void *data, Type type);

/**
 * object_initialize:
 * @obj: A pointer to the memory to be used for the object.
 * @typename: The name of the type of the object to instantiate.
 *
 * This function will initialize an object.  The memory for the object should
 * have already been allocated.
 */
void object_initialize(void *obj, const char *typename);

/**
 * object_finalize:
 * @obj: The object to finalize.
 *
 * This function destroys and object without freeing the memory associated with
 * it.
 */
void object_finalize(void *obj);

/**
 * object_dynamic_cast:
 * @obj: The object to cast.
 * @typename: The @typename to cast to.
 *
 * This function will determine if @obj is-a @typename.  @obj can refer to an
 * object or an interface associated with an object.
 *
 * Returns: This function returns @obj on success or #NULL on failure.
 */
Object *object_dynamic_cast(Object *obj, const char *typename);

/**
 * @object_dynamic_cast_assert:
 *
 * See object_dynamic_cast() for a description of the parameters of this
 * function.  The only difference in behavior is that this function asserts
 * instead of returning #NULL on failure.
 */
Object *object_dynamic_cast_assert(Object *obj, const char *typename);

/**
 * object_get_class:
 * @obj: A derivative of #Object
 *
 * Returns: The #ObjectClass of the type associated with @obj.
 */
ObjectClass *object_get_class(Object *obj);

/**
 * object_get_typename:
 * @obj: A derivative of #Object.
 *
 * Returns: The QOM typename of @obj.
 */
const char *object_get_typename(Object *obj);

/**
 * type_register_static:
 * @info: The #TypeInfo of the new type.
 *
 * @info and all of the strings it points to should exist for the life time
 * that the type is registered.
 *
 * Returns: 0 on failure, the new #Type on success.
 */
Type type_register_static(const TypeInfo *info);

void type_unregister(const TypeInfo *info);

/**
 * type_register:
 * @info: The #TypeInfo of the new type
 *
 * Unlike type_register_static(), this call does not require @info or it's
 * string members to continue to exist after the call returns.
 *
 * Returns: 0 on failure, the new #Type on success.
 */
Type type_register(const TypeInfo *info);

/**
 * object_class_dynamic_cast_assert:
 * @klass: The #ObjectClass to attempt to cast.
 * @typename: The QOM typename of the class to cast to.
 *
 * Returns: This function always returns @klass and asserts on failure.
 */
ObjectClass *object_class_dynamic_cast_assert(ObjectClass *klass,
                                              const char *typename);

ObjectClass *object_class_dynamic_cast(ObjectClass *klass,
                                       const char *typename);

/**
 * object_class_get_name:
 * @klass: The class to obtain the QOM typename for.
 *
 * Returns: The QOM typename for @klass.
 */
const char *object_class_get_name(ObjectClass *klass);

ObjectClass *object_class_by_name(const char *typename);

void object_class_foreach(void (*fn)(ObjectClass *klass, void *opaque),
                          const char *implements_type, bool include_abstract,
                          void *opaque);
/**
 * object_ref:
 * @obj: the object
 *
 * Increase the reference count of a object.  A object cannot be freed as long
 * as its reference count is greater than zero.
 */
void object_ref(Object *obj);

/**
 * qdef_unref:
 * @obj: the object
 *
 * Decrease the reference count of a object.  A object cannot be freed as long
 * as its reference count is greater than zero.
 */
void object_unref(Object *obj);

/**
 * object_property_add:
 * @obj: the object to add a property to
 * @name: the name of the property.  This can contain any character except for
 *  a forward slash.  In general, you should use hyphens '-' instead of
 *  underscores '_' when naming properties.
 * @type: the type name of the property.  This namespace is pretty loosely
 *   defined.  Sub namespaces are constructed by using a prefix and then
 *   to angle brackets.  For instance, the type 'virtio-net-pci' in the
 *   'link' namespace would be 'link<virtio-net-pci>'.
 * @get: The getter to be called to read a property.  If this is NULL, then
 *   the property cannot be read.
 * @set: the setter to be called to write a property.  If this is NULL,
 *   then the property cannot be written.
 * @release: called when the property is removed from the object.  This is
 *   meant to allow a property to free its opaque upon object
 *   destruction.  This may be NULL.
 * @opaque: an opaque pointer to pass to the callbacks for the property
 * @errp: returns an error if this function fails
 */
void object_property_add(Object *obj, const char *name, const char *type,
                         ObjectPropertyAccessor *get,
                         ObjectPropertyAccessor *set,
                         ObjectPropertyRelease *release,
                         void *opaque, Error **errp);

void object_property_del(Object *obj, const char *name, Error **errp);

/**
 * object_property_get:
 * @obj: the object
 * @v: the visitor that will receive the property value.  This should be an
 *   Output visitor and the data will be written with @name as the name.
 * @name: the name of the property
 * @errp: returns an error if this function fails
 *
 * Reads a property from a object.
 */
void object_property_get(Object *obj, Visitor *v, const char *name,
                         Error **errp);

/**
 * object_property_set:
 * @obj: the object
 * @v: the visitor that will be used to write the property value.  This should
 *   be an Input visitor and the data will be first read with @name as the
 *   name and then written as the property value.
 * @name: the name of the property
 * @errp: returns an error if this function fails
 *
 * Writes a property to a object.
 */
void object_property_set(Object *obj, Visitor *v, const char *name,
                         Error **errp);

/**
 * @object_property_get_type:
 * @obj: the object
 * @name: the name of the property
 * @errp: returns an error if this function fails
 *
 * Returns:  The type name of the property.
 */
const char *object_property_get_type(Object *obj, const char *name,
                                     Error **errp);

/**
 * object_get_root:
 *
 * Returns: the root object of the composition tree
 */
Object *object_get_root(void);

/**
 * object_get_canonical_path:
 *
 * Returns: The canonical path for a object.  This is the path within the
 * composition tree starting from the root.
 */
gchar *object_get_canonical_path(Object *obj);

/**
 * object_resolve_path:
 * @path: the path to resolve
 * @ambiguous: returns true if the path resolution failed because of an
 *   ambiguous match
 *
 * There are two types of supported paths--absolute paths and partial paths.
 * 
 * Absolute paths are derived from the root object and can follow child<> or
 * link<> properties.  Since they can follow link<> properties, they can be
 * arbitrarily long.  Absolute paths look like absolute filenames and are
 * prefixed with a leading slash.
 * 
 * Partial paths look like relative filenames.  They do not begin with a
 * prefix.  The matching rules for partial paths are subtle but designed to make
 * specifying objects easy.  At each level of the composition tree, the partial
 * path is matched as an absolute path.  The first match is not returned.  At
 * least two matches are searched for.  A successful result is only returned if
 * only one match is founded.  If more than one match is found, a flag is
 * return to indicate that the match was ambiguous.
 *
 * Returns: The matched object or NULL on path lookup failure.
 */
Object *object_resolve_path(const char *path, bool *ambiguous);

/**
 * object_property_add_child:
 * @obj: the object to add a property to
 * @name: the name of the property
 * @child: the child object
 * @errp: if an error occurs, a pointer to an area to store the area
 *
 * Child properties form the composition tree.  All objects need to be a child
 * of another object.  Objects can only be a child of one object.
 *
 * There is no way for a child to determine what its parent is.  It is not
 * a bidirectional relationship.  This is by design.
 */
void object_property_add_child(Object *obj, const char *name,
                               Object *child, Error **errp);

/**
 * object_property_add_link:
 * @obj: the object to add a property to
 * @name: the name of the property
 * @type: the qobj type of the link
 * @child: a pointer to where the link object reference is stored
 * @errp: if an error occurs, a pointer to an area to store the area
 *
 * Links establish relationships between objects.  Links are unidirectional
 * although two links can be combined to form a bidirectional relationship
 * between objects.
 *
 * Links form the graph in the object model.
 */
void object_property_add_link(Object *obj, const char *name,
                              const char *type, Object **child,
                              Error **errp);

typedef char *(StringGetter)(Object *, Error **);
typedef void (StringSetter)(Object *, const char *, Error **);

/**
 * object_property_add_str:
 * @obj: the object to add a property to
 * @name: the name of the property
 * @get: the getter or NULL if the property is write-only.  This function must
 *   return a string to be freed by g_free().
 * @set: the setter or NULL if the property is read-only
 * @errp: if an error occurs, a pointer to an area to store the error
 *
 * Add a string property using getters/setters.  This function will add a
 * property of type 'string'.
 */
void object_property_add_str(Object *obj, const char *name,
                             StringGetter *get, StringSetter *set,
                             Error **errp);

typedef void (ObjectPropertyEnumerator)(Object *obj,
                                        const char *name,
                                        const char *typename,
                                        bool read_only,
                                        void *opaque);

void object_property_foreach(Object *obj, ObjectPropertyEnumerator *fn,
                             void *opaque);

#endif
