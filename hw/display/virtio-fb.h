#ifndef QEMU_HW_VIRTIO_FB_H
#define QEMU_HW_VIRTIO_FB_H

#include "hw/virtio/virtio.h"
#include "exec/memory.h"
#include "ui/console.h"

#define TYPE_VIRTIO_FB "virtio-fb"
#define VIRTIO_FB(obj) OBJECT_CHECK(VirtioFB, (obj), TYPE_VIRTIO_FB)

typedef struct VirtioFB
{
    VirtIODevice parent;

    QemuConsole *con;
    const GraphicHwOps *hw_ops;

    MemoryRegion vram;
    int vram_size;
} VirtioFB;

void virtio_fb_invalidate(VirtioFB *s);
void virtio_fb_gfx_update(VirtioFB *s);
void virtio_fb_text_update(VirtioFB *s, console_ch_t *chardata);

#endif
