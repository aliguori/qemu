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

device_init(container_init);
