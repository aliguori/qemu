#include "virtio-fb.h"

static int virtio_fb_realize(VirtIODevice *vdev)
{
    VirtioFB *s = VIRTIO_FB(vdev);

    virtio_init(vdev, "virtio-fb", 0x100A, 0);

    memory_region_init_ram(&s->vram, "virtio-fb.vram", s->vram_size);
    s->con = graphic_console_init(DEVICE(vdev), s->hw_ops, s);

    return 0;
}

void virtio_fb_invalidate(VirtioFB *s)
{
}

void virtio_fb_gfx_update(VirtioFB *s)
{
}

void virtio_fb_text_update(VirtioFB *s, console_ch_t *chardata)
{
}

static const GraphicHwOps virtio_fb_hw_ops = {
    .invalidate = (GraphicHwInvalidate *)virtio_fb_invalidate,
    .gfx_update = (GraphicHwGFXUpdate *)virtio_fb_gfx_update,
    .text_update = (GraphicHwTextUpdate *)virtio_fb_text_update,
};

static void virtio_fb_initfn(Object *obj)
{
    VirtioFB *s = VIRTIO_FB(obj);
    
    s->hw_ops = &virtio_fb_hw_ops;
    s->vram_size = 16 << 20;
}

static void virtio_fb_get_config(VirtIODevice *vdev, uint8_t *config_data)
{
}

static void virtio_fb_set_config(VirtIODevice *vdev, const uint8_t *config_data)
{
}

static uint32_t virtio_fb_get_features(VirtIODevice *vdev, uint32_t flags)
{
    return 0;
}

static void virtio_fb_class_init(ObjectClass *klass, void *data)
{
    VirtioDeviceClass *vc = VIRTIO_DEVICE_CLASS(klass);

    vc->init = virtio_fb_realize;
    vc->get_config = virtio_fb_get_config;
    vc->set_config = virtio_fb_set_config;
    vc->get_features = virtio_fb_get_features;
}

static const TypeInfo virtio_fb_info = {
    .name = TYPE_VIRTIO_FB,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtioFB),
    .instance_init = virtio_fb_initfn,
    .class_init = virtio_fb_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_fb_info);
}

type_init(virtio_register_types)
