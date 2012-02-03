#ifndef HW_PC_H
#define HW_PC_H

#include "qemu-common.h"
#include "memory.h"
#include "ioport.h"
#include "isa.h"
#include "fdc.h"
#include "net.h"
#include "memory.h"
#include "ioapic.h"

/* PC-style peripherals (also used by other machines).  */

/* serial.c */

SerialState *serial_init(int base, qemu_irq irq, int baudbase,
                         CharDriverState *chr);
SerialState *serial_mm_init(MemoryRegion *address_space,
                            target_phys_addr_t base, int it_shift,
                            qemu_irq irq, int baudbase,
                            CharDriverState *chr, enum device_endian);
static inline bool serial_isa_init(ISABus *bus, int index,
                                   CharDriverState *chr)
{
    ISADevice *dev;

    dev = isa_try_create(bus, "isa-serial");
    if (!dev) {
        return false;
    }
    qdev_prop_set_uint32(&dev->qdev, "index", index);
    qdev_prop_set_chr(&dev->qdev, "chardev", chr);
    if (qdev_init(&dev->qdev) < 0) {
        return false;
    }
    return true;
}

void serial_set_frequency(SerialState *s, uint32_t frequency);

/* parallel.c */
static inline bool parallel_init(ISABus *bus, int index, CharDriverState *chr)
{
    ISADevice *dev;

    dev = isa_try_create(bus, "isa-parallel");
    if (!dev) {
        return false;
    }
    qdev_prop_set_uint32(&dev->qdev, "index", index);
    qdev_prop_set_chr(&dev->qdev, "chardev", chr);
    if (qdev_init(&dev->qdev) < 0) {
        return false;
    }
    return true;
}

bool parallel_mm_init(MemoryRegion *address_space,
                      target_phys_addr_t base, int it_shift, qemu_irq irq,
                      CharDriverState *chr);

/* i8259.c */

extern DeviceState *isa_pic;
qemu_irq *i8259_init(ISABus *bus, qemu_irq parent_irq);
qemu_irq *kvm_i8259_init(ISABus *bus);
int pic_read_irq(DeviceState *d);
int pic_get_output(DeviceState *d);
void pic_info(Monitor *mon);
void irq_info(Monitor *mon);

/* Global System Interrupts */

#define GSI_NUM_PINS IOAPIC_NUM_PINS

typedef struct GSIState {
    qemu_irq i8259_irq[ISA_NUM_IRQS];
    qemu_irq ioapic_irq[IOAPIC_NUM_PINS];
} GSIState;

/* i8254.c */

#define PIT_FREQ 1193182

static inline ISADevice *pit_init(ISABus *bus, int base, int irq)
{
    ISADevice *dev;

    dev = isa_create(bus, "isa-pit");
    qdev_prop_set_uint32(&dev->qdev, "iobase", base);
    qdev_prop_set_uint32(&dev->qdev, "irq", irq);
    qdev_init_nofail(&dev->qdev);

    return dev;
}

void pit_set_gate(ISADevice *dev, int channel, int val);
int pit_get_gate(ISADevice *dev, int channel);
int pit_get_initial_count(ISADevice *dev, int channel);
int pit_get_mode(ISADevice *dev, int channel);
int pit_get_out(ISADevice *dev, int channel, int64_t current_time);

void hpet_pit_disable(void);
void hpet_pit_enable(void);

/* vmport.c */
static inline void vmport_init(ISABus *bus)
{
    isa_create_simple(bus, "vmport");
}
void vmport_register(unsigned char command, IOPortReadFunc *func, void *opaque);
void vmmouse_get_data(uint32_t *data);
void vmmouse_set_data(const uint32_t *data);

/* pckbd.c */

void i8042_init(qemu_irq kbd_irq, qemu_irq mouse_irq, uint32_t io_base);
void i8042_mm_init(qemu_irq kbd_irq, qemu_irq mouse_irq,
                   MemoryRegion *region, ram_addr_t size,
                   target_phys_addr_t mask);
void i8042_isa_mouse_fake_event(void *opaque);
void i8042_setup_a20_line(ISADevice *dev, qemu_irq *a20_out);

/* pc.c */
extern int fd_bootchk;

void pc_cmos_set_s3_resume(void *opaque, int irq, int level);

typedef void (*cpu_set_smm_t)(int smm, void *arg);
void cpu_smm_register(cpu_set_smm_t callback, void *arg);

/* acpi.c */
extern int acpi_enabled;
extern char *acpi_tables;
extern size_t acpi_tables_len;

void acpi_bios_init(void);
int acpi_table_add(const char *table_desc);

/* acpi_piix.c */

i2c_bus *piix4_pm_init(PCIBus *bus, int devfn, uint32_t smb_io_base,
                       qemu_irq sci_irq, qemu_irq cmos_s3, qemu_irq smi_irq,
                       int kvm_enabled);
void piix4_smbus_register_device(SMBusDevice *dev, uint8_t addr);

/* hpet.c */
extern int no_hpet;

/* pcspk.c */
void pcspk_init(ISADevice *pit);
int pcspk_audio_init(ISABus *bus);

/* piix_pci.c */
struct I440FXPMCState;
typedef struct I440FXPMCState I440FXPMCState;

PCIBus *i440fx_init(I440FXPMCState **pi440fx_state, int *piix_devfn,
                    ISABus **isa_bus, qemu_irq *pic,
                    MemoryRegion *address_space_mem,
                    MemoryRegion *address_space_io,
                    ram_addr_t ram_size,
                    MemoryRegion *ram_memory);

/* piix4.c */
extern PCIDevice *piix4_dev;
int piix4_init(PCIBus *bus, ISABus **isa_bus, int devfn);

/* vga.c */
enum vga_retrace_method {
    VGA_RETRACE_DUMB,
    VGA_RETRACE_PRECISE
};

extern enum vga_retrace_method vga_retrace_method;

static inline DeviceState *isa_vga_init(ISABus *bus)
{
    ISADevice *dev;

    dev = isa_try_create(bus, "isa-vga");
    if (!dev) {
        fprintf(stderr, "Warning: isa-vga not available\n");
        return NULL;
    }
    qdev_init_nofail(&dev->qdev);
    return &dev->qdev;
}

DeviceState *pci_vga_init(PCIBus *bus);
int isa_vga_mm_init(target_phys_addr_t vram_base,
                    target_phys_addr_t ctrl_base, int it_shift,
                    MemoryRegion *address_space);

/* cirrus_vga.c */
DeviceState *pci_cirrus_vga_init(PCIBus *bus);

/* ne2000.c */
static inline bool isa_ne2000_init(ISABus *bus, int base, int irq, NICInfo *nd)
{
    ISADevice *dev;

    qemu_check_nic_model(nd, "ne2k_isa");

    dev = isa_try_create(bus, "ne2k_isa");
    if (!dev) {
        return false;
    }
    qdev_prop_set_uint32(&dev->qdev, "iobase", base);
    qdev_prop_set_uint32(&dev->qdev, "irq",    irq);
    qdev_set_nic_properties(&dev->qdev, nd);
    qdev_init_nofail(&dev->qdev);
    return true;
}

/* e820 types */
#define E820_RAM        1
#define E820_RESERVED   2
#define E820_ACPI       3
#define E820_NVS        4
#define E820_UNUSABLE   5

int e820_add_entry(uint64_t, uint64_t, uint32_t);

#endif
