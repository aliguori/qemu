#ifndef HW_SYSBUS_H
#define HW_SYSBUS_H 1

/* Devices attached directly to the main system bus.  */

#include "qdev.h"
#include "memory.h"

#define QDEV_MAX_MMIO 32
#define QDEV_MAX_PIO 32
#define QDEV_MAX_IRQ 512

typedef struct SysBusDevice SysBusDevice;

#define TYPE_SYS_BUS_DEVICE "sys-bus-device"
#define SYS_BUS_DEVICE(obj) \
     OBJECT_CHECK(SysBusDevice, (obj), TYPE_SYS_BUS_DEVICE)
#define SYS_BUS_DEVICE_CLASS(klass) \
     OBJECT_CLASS_CHECK(SysBusDeviceClass, (klass), TYPE_SYS_BUS_DEVICE)
#define SYS_BUS_DEVICE_GET_CLASS(obj) \
     OBJECT_GET_CLASS(SysBusDeviceClass, (obj), TYPE_SYS_BUS_DEVICE)

typedef struct SysBusDeviceClass {
    DeviceClass parent_class;

    int (*init)(SysBusDevice *dev);
} SysBusDeviceClass;

struct SysBusDevice {
    DeviceState qdev;
    int num_irq;
    qemu_irq irqs[QDEV_MAX_IRQ];
    qemu_irq *irqp[QDEV_MAX_IRQ];
    int num_mmio;
    struct {
        target_phys_addr_t addr;
        MemoryRegion *memory;
    } mmio[QDEV_MAX_MMIO];
    int num_pio;
    pio_addr_t pio[QDEV_MAX_PIO];
};

/* Macros to compensate for lack of type inheritance in C.  */
#define sysbus_from_qdev(dev) ((SysBusDevice *)(dev))
#define FROM_SYSBUS(type, dev) DO_UPCAST(type, busdev, dev)

void *sysbus_new(void);
void sysbus_init_mmio(SysBusDevice *dev, MemoryRegion *memory);
MemoryRegion *sysbus_mmio_get_region(SysBusDevice *dev, int n);
void sysbus_init_irq(SysBusDevice *dev, qemu_irq *p);
void sysbus_pass_irq(SysBusDevice *dev, SysBusDevice *target);
void sysbus_init_ioports(SysBusDevice *dev, pio_addr_t ioport, pio_addr_t size);


void sysbus_connect_irq(SysBusDevice *dev, int n, qemu_irq irq);
void sysbus_mmio_map(SysBusDevice *dev, int n, target_phys_addr_t addr);
void sysbus_add_memory(SysBusDevice *dev, target_phys_addr_t addr,
                       MemoryRegion *mem);
void sysbus_add_memory_overlap(SysBusDevice *dev, target_phys_addr_t addr,
                               MemoryRegion *mem, unsigned priority);
void sysbus_del_memory(SysBusDevice *dev, MemoryRegion *mem);
void sysbus_add_io(SysBusDevice *dev, target_phys_addr_t addr,
                   MemoryRegion *mem);
void sysbus_del_io(SysBusDevice *dev, MemoryRegion *mem);
MemoryRegion *sysbus_address_space(SysBusDevice *dev);

/* Legacy helper function for creating devices.  */
DeviceState *sysbus_create_varargs(const char *name,
                                 target_phys_addr_t addr, ...);
DeviceState *sysbus_try_create_varargs(const char *name,
                                       target_phys_addr_t addr, ...);
static inline DeviceState *sysbus_create_simple(const char *name,
                                              target_phys_addr_t addr,
                                              qemu_irq irq)
{
    return sysbus_create_varargs(name, addr, irq, NULL);
}

static inline DeviceState *sysbus_try_create_simple(const char *name,
                                                    target_phys_addr_t addr,
                                                    qemu_irq irq)
{
    return sysbus_try_create_varargs(name, addr, irq, NULL);
}

#endif /* !HW_SYSBUS_H */
