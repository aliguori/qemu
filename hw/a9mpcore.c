/*
 * Cortex-A9MPCore internal peripheral emulation.
 *
 * Copyright (c) 2009 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the GPL.
 */

/* 64 external IRQ lines.  */
#define GIC_NIRQ 96
#include "mpcore.c"

static Property mpcore_priv_properties[] = {
    DEFINE_PROP_UINT32("num-cpu", mpcore_priv_state, num_cpu, 1),
    DEFINE_PROP_END_OF_LIST(),
};

static void mpcore_priv_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = mpcore_priv_init;
}

static DeviceInfo mpcore_priv_info = {
    .name = "a9mpcore_priv",
    .size = sizeof(mpcore_priv_state),
    .props = mpcore_priv_properties,
    .class_init = mpcore_priv_class_init,
};

static void a9mpcore_register_devices(void)
{
    sysbus_register_withprop(&mpcore_priv_info);
}

device_init(a9mpcore_register_devices)
