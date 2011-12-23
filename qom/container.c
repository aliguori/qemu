#include "qemu/object.h"
#include "module.h"

static TypeInfo container_info = {
    .name          = "container",
    .parent        = TYPE_OBJECT,
};

static void container_init(void)
{
    type_register_static(&container_info);
}

static void container_exit(void)
{
    type_unregister(&container_info);
}

device_init(container_init);
device_exit(container_exit);
