#include "qemu/object.h"
#include "module.h"

static TypeInfo container_info = {
    .name          = "container",
    .instance_size = sizeof(Object),
    .parent        = TYPE_OBJECT,
};

static void container_init(void)
{
    type_register_static(&container_info);
}

device_init(container_init);
