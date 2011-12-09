#include "sysbus.h"

static int container_initfn(SysBusDevice *dev)
{
    return 0;
}

static void container_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = container_initfn;
}

static DeviceInfo container_info = {
    .name = "container",
    .size = sizeof(SysBusDevice),
    .no_user = 1,
    .class_init = container_class_init,
};

static void container_init(void)
{
    qdev_register_subclass(&container_info, TYPE_SYS_BUS_DEVICE);
}

device_init(container_init);
