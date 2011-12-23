#include "qemu/object.h"
#include "module.h"

#include <glib.h>
#include <stdio.h>

#define TYPE_MY_DEVICE "my-device"
#define MY_DEVICE(obj) OBJECT_CHECK(MyDevice, (obj), TYPE_MY_DEVICE)

typedef struct MyDevice
{
    Object parent;
} MyDevice;

static TypeInfo my_device_info = {
    .name = TYPE_MY_DEVICE,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(MyDevice),
};

static void register_my_device(void)
{
    type_register_static(&my_device_info);
}

static void unregister_my_device(void)
{
    type_unregister(&my_device_info);
}

device_init(register_my_device);
device_exit(unregister_my_device);

/** module infrastructure **/

typedef void (module_init_fn)(void);
static module_init_fn *init_fns[1024];
static int nb_init_fns = 0;

static module_init_fn *exit_fns[1024];
static int nb_exit_fns = 0;

void register_module_init(module_init_fn *fn, module_init_type type)
{
    init_fns[nb_init_fns++] = fn;
}

void register_module_exit(module_init_fn *fn, module_init_type type)
{
    exit_fns[nb_exit_fns++] = fn;
}

static void init_modules(void)
{
    int i;

    for (i = 0; i < nb_init_fns; i++) {
        init_fns[i]();
    }
}

static void exit_modules(void)
{
    int i;

    for (i = 0; i < nb_exit_fns; i++) {
        exit_fns[i]();
    }
}

/** test **/

int main(int argc, char **argv)
{
    MyDevice *dev;
    MyDevice dev1;

    init_modules();

    dev = MY_DEVICE(object_new(TYPE_MY_DEVICE));

    object_delete(OBJECT(dev));

    object_initialize(&dev1, TYPE_MY_DEVICE);
    object_ref(OBJECT(&dev1));

    object_unref(OBJECT(&dev1));

    exit_modules();

    return 0;
}
