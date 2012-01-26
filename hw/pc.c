/*
 * QEMU PC System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "hw.h"
#include "pc.h"
#include "apic.h"
#include "fdc.h"
#include "ide.h"
#include "pci.h"
#include "vmware_vga.h"
#include "monitor.h"
#include "fw_cfg.h"
#include "hpet_emul.h"
#include "smbios.h"
#include "loader.h"
#include "elf.h"
#include "multiboot.h"
#include "mc146818rtc.h"
#include "msi.h"
#include "sysbus.h"
#include "sysemu.h"
#include "kvm.h"
#include "blockdev.h"
#include "ui/qemu-spice.h"
#include "memory.h"
#include "exec-memory.h"
#include "kvm/clock.h"
#include "xen.h"
#include "arch_init.h"
#include "usb-uhci.h"
#include "smbus.h"
#include "boards.h"

/* output Bochs bios info messages */
//#define DEBUG_BIOS

/* debug PC/ISA interrupts */
//#define DEBUG_IRQ

#ifdef DEBUG_IRQ
#define DPRINTF(fmt, ...)                                       \
    do { printf("CPUIRQ: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...)
#endif

#define BIOS_FILENAME "bios.bin"

#define PC_MAX_BIOS_SIZE (4 * 1024 * 1024)

/* Leave a chunk of memory at the top of RAM for the BIOS ACPI tables.  */
#define ACPI_DATA_SIZE       0x10000
#define BIOS_CFG_IOPORT 0x510
#define FW_CFG_ACPI_TABLES (FW_CFG_ARCH_LOCAL + 0)
#define FW_CFG_SMBIOS_ENTRIES (FW_CFG_ARCH_LOCAL + 1)
#define FW_CFG_IRQ0_OVERRIDE (FW_CFG_ARCH_LOCAL + 2)
#define FW_CFG_E820_TABLE (FW_CFG_ARCH_LOCAL + 3)
#define FW_CFG_HPET (FW_CFG_ARCH_LOCAL + 4)

#define MSI_ADDR_BASE 0xfee00000

#define E820_NR_ENTRIES		16

#define MAX_IDE_BUS 2

struct e820_entry {
    uint64_t address;
    uint64_t length;
    uint32_t type;
} QEMU_PACKED __attribute((__aligned__(4)));

struct e820_table {
    uint32_t count;
    struct e820_entry entry[E820_NR_ENTRIES];
} QEMU_PACKED __attribute((__aligned__(4)));

static const int ide_iobase[MAX_IDE_BUS] = { 0x1f0, 0x170 };
static const int ide_iobase2[MAX_IDE_BUS] = { 0x3f6, 0x376 };
static const int ide_irq[MAX_IDE_BUS] = { 14, 15 };

static struct e820_table e820_table;
struct hpet_fw_config hpet_cfg = {.count = UINT8_MAX};

static void gsi_handler(void *opaque, int n, int level)
{
    GSIState *s = opaque;

    DPRINTF("pc: %s GSI %d\n", level ? "raising" : "lowering", n);
    if (n < ISA_NUM_IRQS) {
        qemu_set_irq(s->i8259_irq[n], level);
    }
    qemu_set_irq(s->ioapic_irq[n], level);
}

static void ioport80_write(void *opaque, uint32_t addr, uint32_t data)
{
}

/* MSDOS compatibility mode FPU exception support */
static qemu_irq ferr_irq;

static void pc_register_ferr_irq(qemu_irq irq)
{
    ferr_irq = irq;
}

/* XXX: add IGNNE support */
void cpu_set_ferr(CPUX86State *s)
{
    qemu_irq_raise(ferr_irq);
}

static void ioportF0_write(void *opaque, uint32_t addr, uint32_t data)
{
    qemu_irq_lower(ferr_irq);
}

/* TSC handling */
uint64_t cpu_get_tsc(CPUX86State *env)
{
    return cpu_get_ticks();
}

/* SMM support */

static cpu_set_smm_t smm_set;
static void *smm_arg;

void cpu_smm_register(cpu_set_smm_t callback, void *arg)
{
    assert(smm_set == NULL);
    assert(smm_arg == NULL);
    smm_set = callback;
    smm_arg = arg;
}

void cpu_smm_update(CPUState *env)
{
    if (smm_set && smm_arg && env == first_cpu)
        smm_set(!!(env->hflags & HF_SMM_MASK), smm_arg);
}


/* IRQ handling */
int cpu_get_pic_interrupt(CPUState *env)
{
    int intno;

    intno = apic_get_interrupt(env->apic_state);
    if (intno >= 0) {
        return intno;
    }
    /* read the irq from the PIC */
    if (!apic_accept_pic_intr(env->apic_state)) {
        return -1;
    }

    intno = pic_read_irq(isa_pic);
    return intno;
}

static void pic_irq_request(void *opaque, int irq, int level)
{
    CPUState *env = first_cpu;

    DPRINTF("pic_irqs: %s irq %d\n", level? "raise" : "lower", irq);
    if (env->apic_state) {
        while (env) {
            if (apic_accept_pic_intr(env->apic_state)) {
                apic_deliver_pic_intr(env->apic_state, level);
            }
            env = env->next_cpu;
        }
    } else {
        if (level)
            cpu_interrupt(env, CPU_INTERRUPT_HARD);
        else
            cpu_reset_interrupt(env, CPU_INTERRUPT_HARD);
    }
}

/* PC cmos mappings */

#define REG_EQUIPMENT_BYTE          0x14

static int cmos_get_fd_drive_type(FDriveType fd0)
{
    int val;

    switch (fd0) {
    case FDRIVE_DRV_144:
        /* 1.44 Mb 3"5 drive */
        val = 4;
        break;
    case FDRIVE_DRV_288:
        /* 2.88 Mb 3"5 drive */
        val = 5;
        break;
    case FDRIVE_DRV_120:
        /* 1.2 Mb 5"5 drive */
        val = 2;
        break;
    case FDRIVE_DRV_NONE:
    default:
        val = 0;
        break;
    }
    return val;
}

static void cmos_init_hd(int type_ofs, int info_ofs, BlockDriverState *hd,
                         ISADevice *s)
{
    int cylinders, heads, sectors;
    bdrv_get_geometry_hint(hd, &cylinders, &heads, &sectors);
    rtc_set_memory(s, type_ofs, 47);
    rtc_set_memory(s, info_ofs, cylinders);
    rtc_set_memory(s, info_ofs + 1, cylinders >> 8);
    rtc_set_memory(s, info_ofs + 2, heads);
    rtc_set_memory(s, info_ofs + 3, 0xff);
    rtc_set_memory(s, info_ofs + 4, 0xff);
    rtc_set_memory(s, info_ofs + 5, 0xc0 | ((heads > 8) << 3));
    rtc_set_memory(s, info_ofs + 6, cylinders);
    rtc_set_memory(s, info_ofs + 7, cylinders >> 8);
    rtc_set_memory(s, info_ofs + 8, sectors);
}

/* convert boot_device letter to something recognizable by the bios */
static int boot_device2nibble(char boot_device)
{
    switch(boot_device) {
    case 'a':
    case 'b':
        return 0x01; /* floppy boot */
    case 'c':
        return 0x02; /* hard drive boot */
    case 'd':
        return 0x03; /* CD-ROM boot */
    case 'n':
        return 0x04; /* Network boot */
    }
    return 0;
}

static int set_boot_dev(ISADevice *s, const char *boot_device, int fd_bootchk)
{
#define PC_MAX_BOOT_DEVICES 3
    int nbds, bds[3] = { 0, };
    int i;

    nbds = strlen(boot_device);
    if (nbds > PC_MAX_BOOT_DEVICES) {
        error_report("Too many boot devices for PC");
        return(1);
    }
    for (i = 0; i < nbds; i++) {
        bds[i] = boot_device2nibble(boot_device[i]);
        if (bds[i] == 0) {
            error_report("Invalid boot device for PC: '%c'",
                         boot_device[i]);
            return(1);
        }
    }
    rtc_set_memory(s, 0x3d, (bds[1] << 4) | bds[0]);
    rtc_set_memory(s, 0x38, (bds[2] << 4) | (fd_bootchk ? 0x0 : 0x1));
    return(0);
}

static int pc_boot_set(void *opaque, const char *boot_device)
{
    return set_boot_dev(opaque, boot_device, 0);
}

typedef struct pc_cmos_init_late_arg {
    ISADevice *rtc_state;
    BusState *idebus0, *idebus1;
} pc_cmos_init_late_arg;

static void pc_cmos_init_late(void *opaque)
{
    pc_cmos_init_late_arg *arg = opaque;
    ISADevice *s = arg->rtc_state;
    int val;
    BlockDriverState *hd_table[4];
    int i;

    ide_get_bs(hd_table, arg->idebus0);
    ide_get_bs(hd_table + 2, arg->idebus1);

    rtc_set_memory(s, 0x12, (hd_table[0] ? 0xf0 : 0) | (hd_table[1] ? 0x0f : 0));
    if (hd_table[0])
        cmos_init_hd(0x19, 0x1b, hd_table[0], s);
    if (hd_table[1])
        cmos_init_hd(0x1a, 0x24, hd_table[1], s);

    val = 0;
    for (i = 0; i < 4; i++) {
        if (hd_table[i]) {
            int cylinders, heads, sectors, translation;
            /* NOTE: bdrv_get_geometry_hint() returns the physical
                geometry.  It is always such that: 1 <= sects <= 63, 1
                <= heads <= 16, 1 <= cylinders <= 16383. The BIOS
                geometry can be different if a translation is done. */
            translation = bdrv_get_translation_hint(hd_table[i]);
            if (translation == BIOS_ATA_TRANSLATION_AUTO) {
                bdrv_get_geometry_hint(hd_table[i], &cylinders, &heads, &sectors);
                if (cylinders <= 1024 && heads <= 16 && sectors <= 63) {
                    /* No translation. */
                    translation = 0;
                } else {
                    /* LBA translation. */
                    translation = 1;
                }
            } else {
                translation--;
            }
            val |= translation << (i * 2);
        }
    }
    rtc_set_memory(s, 0x39, val);

    qemu_unregister_reset(pc_cmos_init_late, opaque);
}

static void pc_cmos_init(ram_addr_t ram_size, ram_addr_t above_4g_mem_size,
                  const char *boot_device,
                  ISADevice *floppy, BusState *idebus0, BusState *idebus1,
                  ISADevice *s)
{
    int val, nb, nb_heads, max_track, last_sect, i;
    FDriveType fd_type[2] = { FDRIVE_DRV_NONE, FDRIVE_DRV_NONE };
    BlockDriverState *fd[MAX_FD];
    static pc_cmos_init_late_arg arg;

    /* various important CMOS locations needed by PC/Bochs bios */

    /* memory size */
    val = 640; /* base memory in K */
    rtc_set_memory(s, 0x15, val);
    rtc_set_memory(s, 0x16, val >> 8);

    val = (ram_size / 1024) - 1024;
    if (val > 65535)
        val = 65535;
    rtc_set_memory(s, 0x17, val);
    rtc_set_memory(s, 0x18, val >> 8);
    rtc_set_memory(s, 0x30, val);
    rtc_set_memory(s, 0x31, val >> 8);

    if (above_4g_mem_size) {
        rtc_set_memory(s, 0x5b, (unsigned int)above_4g_mem_size >> 16);
        rtc_set_memory(s, 0x5c, (unsigned int)above_4g_mem_size >> 24);
        rtc_set_memory(s, 0x5d, (uint64_t)above_4g_mem_size >> 32);
    }

    if (ram_size > (16 * 1024 * 1024))
        val = (ram_size / 65536) - ((16 * 1024 * 1024) / 65536);
    else
        val = 0;
    if (val > 65535)
        val = 65535;
    rtc_set_memory(s, 0x34, val);
    rtc_set_memory(s, 0x35, val >> 8);

    /* set the number of CPU */
    rtc_set_memory(s, 0x5f, smp_cpus - 1);

    /* set boot devices, and disable floppy signature check if requested */
    if (set_boot_dev(s, boot_device, fd_bootchk)) {
        exit(1);
    }

    /* floppy type */
    if (floppy) {
        fdc_get_bs(fd, floppy);
        for (i = 0; i < 2; i++) {
            if (fd[i] && bdrv_is_inserted(fd[i])) {
                bdrv_get_floppy_geometry_hint(fd[i], &nb_heads, &max_track,
                                              &last_sect, FDRIVE_DRV_NONE,
                                              &fd_type[i]);
            }
        }
    }
    val = (cmos_get_fd_drive_type(fd_type[0]) << 4) |
        cmos_get_fd_drive_type(fd_type[1]);
    rtc_set_memory(s, 0x10, val);

    val = 0;
    nb = 0;
    if (fd_type[0] < FDRIVE_DRV_NONE) {
        nb++;
    }
    if (fd_type[1] < FDRIVE_DRV_NONE) {
        nb++;
    }
    switch (nb) {
    case 0:
        break;
    case 1:
        val |= 0x01; /* 1 drive, ready for boot */
        break;
    case 2:
        val |= 0x41; /* 2 drives, ready for boot */
        break;
    }
    val |= 0x02; /* FPU is there */
    val |= 0x04; /* PS/2 mouse installed */
    rtc_set_memory(s, REG_EQUIPMENT_BYTE, val);

    /* hard drives */
    arg.rtc_state = s;
    arg.idebus0 = idebus0;
    arg.idebus1 = idebus1;
    qemu_register_reset(pc_cmos_init_late, &arg);
}

/* port 92 stuff: could be split off */
typedef struct Port92State {
    ISADevice dev;
    MemoryRegion io;
    uint8_t outport;
    qemu_irq *a20_out;
} Port92State;

static void port92_write(void *opaque, uint32_t addr, uint32_t val)
{
    Port92State *s = opaque;

    DPRINTF("port92: write 0x%02x\n", val);
    s->outport = val;
    qemu_set_irq(*s->a20_out, (val >> 1) & 1);
    if (val & 1) {
        qemu_system_reset_request();
    }
}

static uint32_t port92_read(void *opaque, uint32_t addr)
{
    Port92State *s = opaque;
    uint32_t ret;

    ret = s->outport;
    DPRINTF("port92: read 0x%02x\n", ret);
    return ret;
}

static void port92_init(ISADevice *dev, qemu_irq *a20_out)
{
    Port92State *s = DO_UPCAST(Port92State, dev, dev);

    s->a20_out = a20_out;
}

static const VMStateDescription vmstate_port92_isa = {
    .name = "port92",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT8(outport, Port92State),
        VMSTATE_END_OF_LIST()
    }
};

static void port92_reset(DeviceState *d)
{
    Port92State *s = container_of(d, Port92State, dev.qdev);

    s->outport &= ~1;
}

static const MemoryRegionPortio port92_portio[] = {
    { 0, 1, 1, .read = port92_read, .write = port92_write },
    PORTIO_END_OF_LIST(),
};

static const MemoryRegionOps port92_ops = {
    .old_portio = port92_portio
};

static int port92_initfn(ISADevice *dev)
{
    Port92State *s = DO_UPCAST(Port92State, dev, dev);

    memory_region_init_io(&s->io, &port92_ops, s, "port92", 1);
    isa_register_ioport(dev, &s->io, 0x92);

    s->outport = 0;
    return 0;
}

static void port92_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ISADeviceClass *ic = ISA_DEVICE_CLASS(klass);
    ic->init = port92_initfn;
    dc->no_user = 1;
    dc->reset = port92_reset;
    dc->vmsd = &vmstate_port92_isa;
}

static TypeInfo port92_info = {
    .name          = "port92",
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(Port92State),
    .class_init    = port92_class_initfn,
};

static void port92_register(void)
{
    type_register_static(&port92_info);
}
device_init(port92_register)

static void handle_a20_line_change(void *opaque, int irq, int level)
{
    CPUState *cpu = opaque;

    /* XXX: send to all CPUs ? */
    /* XXX: add logic to handle multiple A20 line sources */
    cpu_x86_set_a20(cpu, level);
}

/***********************************************************/
/* Bochs BIOS debug ports */

static void bochs_bios_write(void *opaque, uint32_t addr, uint32_t val)
{
    static const char shutdown_str[8] = "Shutdown";
    static int shutdown_index = 0;

    switch(addr) {
        /* Bochs BIOS messages */
    case 0x400:
    case 0x401:
        /* used to be panic, now unused */
        break;
    case 0x402:
    case 0x403:
#ifdef DEBUG_BIOS
        fprintf(stderr, "%c", val);
#endif
        break;
    case 0x8900:
        /* same as Bochs power off */
        if (val == shutdown_str[shutdown_index]) {
            shutdown_index++;
            if (shutdown_index == 8) {
                shutdown_index = 0;
                qemu_system_shutdown_request();
            }
        } else {
            shutdown_index = 0;
        }
        break;

        /* LGPL'ed VGA BIOS messages */
    case 0x501:
    case 0x502:
        exit((val << 1) | 1);
    case 0x500:
    case 0x503:
#ifdef DEBUG_BIOS
        fprintf(stderr, "%c", val);
#endif
        break;
    }
}

int e820_add_entry(uint64_t address, uint64_t length, uint32_t type)
{
    int index = le32_to_cpu(e820_table.count);
    struct e820_entry *entry;

    if (index >= E820_NR_ENTRIES)
        return -EBUSY;
    entry = &e820_table.entry[index++];

    entry->address = cpu_to_le64(address);
    entry->length = cpu_to_le64(length);
    entry->type = cpu_to_le32(type);

    e820_table.count = cpu_to_le32(index);
    return index;
}

static void *bochs_bios_init(void)
{
    void *fw_cfg;
    uint8_t *smbios_table;
    size_t smbios_len;
    uint64_t *numa_fw_cfg;
    int i, j;

    register_ioport_write(0x400, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x401, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x402, 1, 1, bochs_bios_write, NULL);
    register_ioport_write(0x403, 1, 1, bochs_bios_write, NULL);
    register_ioport_write(0x8900, 1, 1, bochs_bios_write, NULL);

    register_ioport_write(0x501, 1, 1, bochs_bios_write, NULL);
    register_ioport_write(0x501, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x502, 1, 2, bochs_bios_write, NULL);
    register_ioport_write(0x500, 1, 1, bochs_bios_write, NULL);
    register_ioport_write(0x503, 1, 1, bochs_bios_write, NULL);

    fw_cfg = fw_cfg_init(BIOS_CFG_IOPORT, BIOS_CFG_IOPORT + 1, 0, 0);

    fw_cfg_add_i32(fw_cfg, FW_CFG_ID, 1);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)ram_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_ACPI_TABLES, (uint8_t *)acpi_tables,
                     acpi_tables_len);
    fw_cfg_add_i32(fw_cfg, FW_CFG_IRQ0_OVERRIDE, kvm_allows_irq0_override());

    smbios_table = smbios_get_table(&smbios_len);
    if (smbios_table)
        fw_cfg_add_bytes(fw_cfg, FW_CFG_SMBIOS_ENTRIES,
                         smbios_table, smbios_len);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_E820_TABLE, (uint8_t *)&e820_table,
                     sizeof(struct e820_table));

    fw_cfg_add_bytes(fw_cfg, FW_CFG_HPET, (uint8_t *)&hpet_cfg,
                     sizeof(struct hpet_fw_config));
    /* allocate memory for the NUMA channel: one (64bit) word for the number
     * of nodes, one word for each VCPU->node and one word for each node to
     * hold the amount of memory.
     */
    numa_fw_cfg = g_malloc0((1 + max_cpus + nb_numa_nodes) * 8);
    numa_fw_cfg[0] = cpu_to_le64(nb_numa_nodes);
    for (i = 0; i < max_cpus; i++) {
        for (j = 0; j < nb_numa_nodes; j++) {
            if (node_cpumask[j] & (1 << i)) {
                numa_fw_cfg[i + 1] = cpu_to_le64(j);
                break;
            }
        }
    }
    for (i = 0; i < nb_numa_nodes; i++) {
        numa_fw_cfg[max_cpus + 1 + i] = cpu_to_le64(node_mem[i]);
    }
    fw_cfg_add_bytes(fw_cfg, FW_CFG_NUMA, (uint8_t *)numa_fw_cfg,
                     (1 + max_cpus + nb_numa_nodes) * 8);

    return fw_cfg;
}

static long get_file_size(FILE *f)
{
    long where, size;

    /* XXX: on Unix systems, using fstat() probably makes more sense */

    where = ftell(f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, where, SEEK_SET);

    return size;
}

static void load_linux(void *fw_cfg,
                       const char *kernel_filename,
		       const char *initrd_filename,
		       const char *kernel_cmdline,
                       target_phys_addr_t max_ram_size)
{
    uint16_t protocol;
    int setup_size, kernel_size, initrd_size = 0, cmdline_size;
    uint32_t initrd_max;
    uint8_t header[8192], *setup, *kernel, *initrd_data;
    target_phys_addr_t real_addr, prot_addr, cmdline_addr, initrd_addr = 0;
    FILE *f;
    char *vmode;

    /* Align to 16 bytes as a paranoia measure */
    cmdline_size = (strlen(kernel_cmdline)+16) & ~15;

    /* load the kernel header */
    f = fopen(kernel_filename, "rb");
    if (!f || !(kernel_size = get_file_size(f)) ||
	fread(header, 1, MIN(ARRAY_SIZE(header), kernel_size), f) !=
	MIN(ARRAY_SIZE(header), kernel_size)) {
	fprintf(stderr, "qemu: could not load kernel '%s': %s\n",
		kernel_filename, strerror(errno));
	exit(1);
    }

    /* kernel protocol version */
#if 0
    fprintf(stderr, "header magic: %#x\n", ldl_p(header+0x202));
#endif
    if (ldl_p(header+0x202) == 0x53726448)
	protocol = lduw_p(header+0x206);
    else {
	/* This looks like a multiboot kernel. If it is, let's stop
	   treating it like a Linux kernel. */
        if (load_multiboot(fw_cfg, f, kernel_filename, initrd_filename,
                           kernel_cmdline, kernel_size, header))
            return;
	protocol = 0;
    }

    if (protocol < 0x200 || !(header[0x211] & 0x01)) {
	/* Low kernel */
	real_addr    = 0x90000;
	cmdline_addr = 0x9a000 - cmdline_size;
	prot_addr    = 0x10000;
    } else if (protocol < 0x202) {
	/* High but ancient kernel */
	real_addr    = 0x90000;
	cmdline_addr = 0x9a000 - cmdline_size;
	prot_addr    = 0x100000;
    } else {
	/* High and recent kernel */
	real_addr    = 0x10000;
	cmdline_addr = 0x20000;
	prot_addr    = 0x100000;
    }

#if 0
    fprintf(stderr,
	    "qemu: real_addr     = 0x" TARGET_FMT_plx "\n"
	    "qemu: cmdline_addr  = 0x" TARGET_FMT_plx "\n"
	    "qemu: prot_addr     = 0x" TARGET_FMT_plx "\n",
	    real_addr,
	    cmdline_addr,
	    prot_addr);
#endif

    /* highest address for loading the initrd */
    if (protocol >= 0x203)
	initrd_max = ldl_p(header+0x22c);
    else
	initrd_max = 0x37ffffff;

    if (initrd_max >= max_ram_size-ACPI_DATA_SIZE)
    	initrd_max = max_ram_size-ACPI_DATA_SIZE-1;

    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_ADDR, cmdline_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_SIZE, strlen(kernel_cmdline)+1);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_CMDLINE_DATA,
                     (uint8_t*)strdup(kernel_cmdline),
                     strlen(kernel_cmdline)+1);

    if (protocol >= 0x202) {
	stl_p(header+0x228, cmdline_addr);
    } else {
	stw_p(header+0x20, 0xA33F);
	stw_p(header+0x22, cmdline_addr-real_addr);
    }

    /* handle vga= parameter */
    vmode = strstr(kernel_cmdline, "vga=");
    if (vmode) {
        unsigned int video_mode;
        /* skip "vga=" */
        vmode += 4;
        if (!strncmp(vmode, "normal", 6)) {
            video_mode = 0xffff;
        } else if (!strncmp(vmode, "ext", 3)) {
            video_mode = 0xfffe;
        } else if (!strncmp(vmode, "ask", 3)) {
            video_mode = 0xfffd;
        } else {
            video_mode = strtol(vmode, NULL, 0);
        }
        stw_p(header+0x1fa, video_mode);
    }

    /* loader type */
    /* High nybble = B reserved for Qemu; low nybble is revision number.
       If this code is substantially changed, you may want to consider
       incrementing the revision. */
    if (protocol >= 0x200)
	header[0x210] = 0xB0;

    /* heap */
    if (protocol >= 0x201) {
	header[0x211] |= 0x80;	/* CAN_USE_HEAP */
	stw_p(header+0x224, cmdline_addr-real_addr-0x200);
    }

    /* load initrd */
    if (initrd_filename) {
	if (protocol < 0x200) {
	    fprintf(stderr, "qemu: linux kernel too old to load a ram disk\n");
	    exit(1);
	}

	initrd_size = get_image_size(initrd_filename);
        if (initrd_size < 0) {
            fprintf(stderr, "qemu: error reading initrd %s\n",
                    initrd_filename);
            exit(1);
        }

        initrd_addr = (initrd_max-initrd_size) & ~4095;

        initrd_data = g_malloc(initrd_size);
        load_image(initrd_filename, initrd_data);

        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_addr);
        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_INITRD_DATA, initrd_data, initrd_size);

	stl_p(header+0x218, initrd_addr);
	stl_p(header+0x21c, initrd_size);
    }

    /* load kernel and setup */
    setup_size = header[0x1f1];
    if (setup_size == 0)
	setup_size = 4;
    setup_size = (setup_size+1)*512;
    kernel_size -= setup_size;

    setup  = g_malloc(setup_size);
    kernel = g_malloc(kernel_size);
    fseek(f, 0, SEEK_SET);
    if (fread(setup, 1, setup_size, f) != setup_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    if (fread(kernel, 1, kernel_size, f) != kernel_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    fclose(f);
    memcpy(setup, header, MIN(sizeof(header), setup_size));

    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, prot_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_KERNEL_DATA, kernel, kernel_size);

    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_ADDR, real_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_SIZE, setup_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_SETUP_DATA, setup, setup_size);

    option_rom[nb_option_roms].name = "linuxboot.bin";
    option_rom[nb_option_roms].bootindex = 0;
    nb_option_roms++;
}

#define NE2000_NB_MAX 6

static const int ne2000_io[NE2000_NB_MAX] = { 0x300, 0x320, 0x340, 0x360,
                                              0x280, 0x380 };
static const int ne2000_irq[NE2000_NB_MAX] = { 9, 10, 11, 3, 4, 5 };

static const int parallel_io[MAX_PARALLEL_PORTS] = { 0x378, 0x278, 0x3bc };
static const int parallel_irq[MAX_PARALLEL_PORTS] = { 7, 7, 7 };

static void pc_init_ne2k_isa(ISABus *bus, NICInfo *nd)
{
    static int nb_ne2k = 0;

    if (nb_ne2k == NE2000_NB_MAX)
        return;
    isa_ne2000_init(bus, ne2000_io[nb_ne2k],
                    ne2000_irq[nb_ne2k], nd);
    nb_ne2k++;
}

int cpu_is_bsp(CPUState *env)
{
    /* We hard-wire the BSP to the first CPU. */
    return env->cpu_index == 0;
}

DeviceState *cpu_get_current_apic(void)
{
    if (cpu_single_env) {
        return cpu_single_env->apic_state;
    } else {
        return NULL;
    }
}

static DeviceState *apic_init(void *env, uint8_t apic_id)
{
    DeviceState *dev;
    static int apic_mapped;

    if (kvm_enabled() && kvm_irqchip_in_kernel()) {
        dev = qdev_create(NULL, "kvm-apic");
    } else {
        dev = qdev_create(NULL, "apic");
    }
    qdev_prop_set_uint8(dev, "id", apic_id);
    qdev_prop_set_ptr(dev, "cpu_env", env);
    qdev_init_nofail(dev);

    /* XXX: mapping more APICs at the same memory location */
    if (apic_mapped == 0) {
        /* NOTE: the APIC is directly connected to the CPU - it is not
           on the global memory bus. */
        /* XXX: what if the base changes? */
        sysbus_mmio_map(sysbus_from_qdev(dev), 0, MSI_ADDR_BASE);
        apic_mapped = 1;
    }

    /* KVM does not support MSI yet. */
    if (!kvm_enabled() || !kvm_irqchip_in_kernel()) {
        msi_supported = true;
    }

    return dev;
}

/* set CMOS shutdown status register (index 0xF) as S3_resume(0xFE)
   BIOS will read it and start S3 resume at POST Entry */
void pc_cmos_set_s3_resume(void *opaque, int irq, int level)
{
    ISADevice *s = opaque;

    if (level) {
        rtc_set_memory(s, 0xF, 0xFE);
    }
}

static void pc_acpi_smi_interrupt(void *opaque, int irq, int level)
{
    CPUState *s = opaque;

    if (level) {
        cpu_interrupt(s, CPU_INTERRUPT_SMI);
    }
}

static void pc_cpu_reset(void *opaque)
{
    CPUState *env = opaque;

    cpu_reset(env);
    env->halted = !cpu_is_bsp(env);
}

static CPUState *pc_new_cpu(const char *cpu_model)
{
    CPUState *env;

    env = cpu_init(cpu_model);
    if (!env) {
        fprintf(stderr, "Unable to find x86 CPU definition\n");
        exit(1);
    }
    if ((env->cpuid_features & CPUID_APIC) || smp_cpus > 1) {
        env->apic_state = apic_init(env, env->cpuid_apic_id);
    }
    qemu_register_reset(pc_cpu_reset, env);
    pc_cpu_reset(env);
    return env;
}

static void pc_cpus_init(const char *cpu_model)
{
    int i;

    /* init CPUs */
    if (cpu_model == NULL) {
#ifdef TARGET_X86_64
        cpu_model = "qemu64";
#else
        cpu_model = "qemu32";
#endif
    }

    for(i = 0; i < smp_cpus; i++) {
        pc_new_cpu(cpu_model);
    }
}

static void pc_memory_init(MemoryRegion *system_memory,
                    const char *kernel_filename,
                    const char *kernel_cmdline,
                    const char *initrd_filename,
                    ram_addr_t below_4g_mem_size,
                    ram_addr_t above_4g_mem_size,
                    MemoryRegion *rom_memory,
                    MemoryRegion **ram_memory)
{
    char *filename;
    int ret, linux_boot, i;
    MemoryRegion *ram, *bios, *isa_bios, *option_rom_mr;
    MemoryRegion *ram_below_4g, *ram_above_4g;
    int bios_size, isa_bios_size;
    void *fw_cfg;

    linux_boot = (kernel_filename != NULL);

    /* Allocate RAM.  We allocate it as a single memory region and use
     * aliases to address portions of it, mostly for backwards compatibility
     * with older qemus that used qemu_ram_alloc().
     */
    ram = g_malloc(sizeof(*ram));
    memory_region_init_ram(ram, "pc.ram",
                           below_4g_mem_size + above_4g_mem_size);
    vmstate_register_ram_global(ram);
    *ram_memory = ram;
    ram_below_4g = g_malloc(sizeof(*ram_below_4g));
    memory_region_init_alias(ram_below_4g, "ram-below-4g", ram,
                             0, below_4g_mem_size);
    memory_region_add_subregion(system_memory, 0, ram_below_4g);
    if (above_4g_mem_size > 0) {
        ram_above_4g = g_malloc(sizeof(*ram_above_4g));
        memory_region_init_alias(ram_above_4g, "ram-above-4g", ram,
                                 below_4g_mem_size, above_4g_mem_size);
        memory_region_add_subregion(system_memory, 0x100000000ULL,
                                    ram_above_4g);
    }

    /* BIOS load */
    if (bios_name == NULL)
        bios_name = BIOS_FILENAME;
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = get_image_size(filename);
    } else {
        bios_size = -1;
    }
    if (bios_size <= 0 ||
        (bios_size % 65536) != 0) {
        goto bios_error;
    }
    bios = g_malloc(sizeof(*bios));
    memory_region_init_ram(bios, "pc.bios", bios_size);
    vmstate_register_ram_global(bios);
    memory_region_set_readonly(bios, true);
    ret = rom_add_file_fixed(bios_name, (uint32_t)(-bios_size), -1);
    if (ret != 0) {
    bios_error:
        fprintf(stderr, "qemu: could not load PC BIOS '%s'\n", bios_name);
        exit(1);
    }
    if (filename) {
        g_free(filename);
    }
    /* map the last 128KB of the BIOS in ISA space */
    isa_bios_size = bios_size;
    if (isa_bios_size > (128 * 1024))
        isa_bios_size = 128 * 1024;
    isa_bios = g_malloc(sizeof(*isa_bios));
    memory_region_init_alias(isa_bios, "isa-bios", bios,
                             bios_size - isa_bios_size, isa_bios_size);
    memory_region_add_subregion_overlap(rom_memory,
                                        0x100000 - isa_bios_size,
                                        isa_bios,
                                        1);
    memory_region_set_readonly(isa_bios, true);

    option_rom_mr = g_malloc(sizeof(*option_rom_mr));
    memory_region_init_ram(option_rom_mr, "pc.rom", PC_ROM_SIZE);
    vmstate_register_ram_global(option_rom_mr);
    memory_region_add_subregion_overlap(rom_memory,
                                        PC_ROM_MIN_VGA,
                                        option_rom_mr,
                                        1);

    /* map all the bios at the top of memory */
    memory_region_add_subregion(rom_memory,
                                (uint32_t)(-bios_size),
                                bios);

    fw_cfg = bochs_bios_init();
    rom_set_fw(fw_cfg);

    if (linux_boot) {
        load_linux(fw_cfg, kernel_filename, initrd_filename, kernel_cmdline, below_4g_mem_size);
    }

    for (i = 0; i < nb_option_roms; i++) {
        rom_add_option(option_rom[i].name, option_rom[i].bootindex);
    }
}

static qemu_irq *pc_allocate_cpu_irq(void)
{
    return qemu_allocate_irqs(pic_irq_request, NULL, 1);
}

static DeviceState *pc_vga_init(ISABus *isa_bus, PCIBus *pci_bus)
{
    DeviceState *dev = NULL;

    if (cirrus_vga_enabled) {
        if (pci_bus) {
            dev = pci_cirrus_vga_init(pci_bus);
        } else {
            dev = &isa_create_simple(isa_bus, "isa-cirrus-vga")->qdev;
        }
    } else if (vmsvga_enabled) {
        if (pci_bus) {
            dev = pci_vmsvga_init(pci_bus);
        } else {
            fprintf(stderr, "%s: vmware_vga: no PCI bus\n", __FUNCTION__);
        }
#ifdef CONFIG_SPICE
    } else if (qxl_enabled) {
        if (pci_bus) {
            dev = &pci_create_simple(pci_bus, -1, "qxl-vga")->qdev;
        } else {
            fprintf(stderr, "%s: qxl: no PCI bus\n", __FUNCTION__);
        }
#endif
    } else if (std_vga_enabled) {
        if (pci_bus) {
            dev = pci_vga_init(pci_bus);
        } else {
            dev = isa_vga_init(isa_bus);
        }
    }

    return dev;
}

static void cpu_request_exit(void *opaque, int irq, int level)
{
    CPUState *env = cpu_single_env;

    if (env && level) {
        cpu_exit(env);
    }
}

static void pc_basic_device_init(ISABus *isa_bus, qemu_irq *gsi,
                                 ISADevice **floppy,
                                 bool no_vmport)
{
    int i;
    DriveInfo *fd[MAX_FD];
    qemu_irq *a20_line;
    ISADevice *i8042, *port92, *vmmouse, *pit;
    qemu_irq *cpu_exit_irq;

    register_ioport_write(0x80, 1, 1, ioport80_write, NULL);

    register_ioport_write(0xf0, 1, 1, ioportF0_write, NULL);

    pit = pit_init(isa_bus, 0x40, 0);
    pcspk_init(pit);

    for(i = 0; i < MAX_SERIAL_PORTS; i++) {
        if (serial_hds[i]) {
            serial_isa_init(isa_bus, i, serial_hds[i]);
        }
    }

    for(i = 0; i < MAX_PARALLEL_PORTS; i++) {
        if (parallel_hds[i]) {
            parallel_init(isa_bus, i, parallel_hds[i]);
        }
    }

    a20_line = qemu_allocate_irqs(handle_a20_line_change, first_cpu, 2);
    i8042 = isa_create_simple(isa_bus, "i8042");
    i8042_setup_a20_line(i8042, &a20_line[0]);
    if (!no_vmport) {
        vmport_init(isa_bus);
        vmmouse = isa_try_create(isa_bus, "vmmouse");
    } else {
        vmmouse = NULL;
    }
    if (vmmouse) {
        qdev_prop_set_ptr(&vmmouse->qdev, "ps2_mouse", i8042);
        qdev_init_nofail(&vmmouse->qdev);
    }
    port92 = isa_create_simple(isa_bus, "port92");
    port92_init(port92, &a20_line[1]);

    cpu_exit_irq = qemu_allocate_irqs(cpu_request_exit, NULL, 1);
    DMA_init(0, cpu_exit_irq);

    for(i = 0; i < MAX_FD; i++) {
        fd[i] = drive_get(IF_FLOPPY, 0, i);
    }
    *floppy = fdctrl_init_isa(isa_bus, fd);
}

static void pc_pci_device_init(PCIBus *pci_bus)
{
    int max_bus;
    int bus;

    max_bus = drive_get_max_bus(IF_SCSI);
    for (bus = 0; bus <= max_bus; bus++) {
        pci_create_simple(pci_bus, -1, "lsi53c895a");
    }
}

static void kvm_piix3_setup_irq_routing(bool pci_enabled)
{
#ifdef CONFIG_KVM
    KVMState *s = kvm_state;
    int ret, i;

    if (kvm_check_extension(s, KVM_CAP_IRQ_ROUTING)) {
        for (i = 0; i < 8; ++i) {
            if (i == 2) {
                continue;
            }
            kvm_irqchip_add_route(s, i, KVM_IRQCHIP_PIC_MASTER, i);
        }
        for (i = 8; i < 16; ++i) {
            kvm_irqchip_add_route(s, i, KVM_IRQCHIP_PIC_SLAVE, i - 8);
        }
        if (pci_enabled) {
            for (i = 0; i < 24; ++i) {
                if (i == 0) {
                    kvm_irqchip_add_route(s, i, KVM_IRQCHIP_IOAPIC, 2);
                } else if (i != 2) {
                    kvm_irqchip_add_route(s, i, KVM_IRQCHIP_IOAPIC, i);
                }
            }
        }
        ret = kvm_irqchip_commit_routes(s);
        if (ret < 0) {
            hw_error("KVM IRQ routing setup failed");
        }
    }
#endif /* CONFIG_KVM */
}

static void kvm_piix3_gsi_handler(void *opaque, int n, int level)
{
    GSIState *s = opaque;

    if (n < ISA_NUM_IRQS) {
        /* Kernel will forward to both PIC and IOAPIC */
        qemu_set_irq(s->i8259_irq[n], level);
    } else {
        qemu_set_irq(s->ioapic_irq[n], level);
    }
}

static void ioapic_init(GSIState *gsi_state)
{
    DeviceState *dev;
    SysBusDevice *d;
    unsigned int i;

    if (kvm_enabled() && kvm_irqchip_in_kernel()) {
        dev = qdev_create(NULL, "kvm-ioapic");
    } else {
        dev = qdev_create(NULL, "ioapic");
    }
    qdev_init_nofail(dev);
    d = sysbus_from_qdev(dev);
    sysbus_mmio_map(d, 0, 0xfec00000);

    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        gsi_state->ioapic_irq[i] = qdev_get_gpio_in(dev, i);
    }
}

/* PC hardware initialisation */
static void pc_init1(MemoryRegion *system_memory,
                     MemoryRegion *system_io,
                     ram_addr_t ram_size,
                     const char *boot_device,
                     const char *kernel_filename,
                     const char *kernel_cmdline,
                     const char *initrd_filename,
                     const char *cpu_model,
                     int pci_enabled,
                     int kvmclock_enabled)
{
    int i;
    ram_addr_t below_4g_mem_size, above_4g_mem_size;
    PCIBus *pci_bus;
    ISABus *isa_bus;
    PCII440FXState *i440fx_state;
    int piix3_devfn = -1;
    qemu_irq *cpu_irq;
    qemu_irq *gsi;
    qemu_irq *i8259;
    qemu_irq *cmos_s3;
    qemu_irq *smi_irq;
    GSIState *gsi_state;
    DriveInfo *hd[MAX_IDE_BUS * MAX_IDE_DEVS];
    BusState *idebus[MAX_IDE_BUS];
    ISADevice *rtc_state;
    ISADevice *floppy;
    MemoryRegion *ram_memory = NULL;
    MemoryRegion *pci_memory;
    MemoryRegion *rom_memory;
    DeviceState *dev;

    pc_cpus_init(cpu_model);

    if (kvmclock_enabled) {
        kvmclock_create();
    }

    if (ram_size >= 0xe0000000 ) {
        above_4g_mem_size = ram_size - 0xe0000000;
        below_4g_mem_size = 0xe0000000;
    } else {
        above_4g_mem_size = 0;
        below_4g_mem_size = ram_size;
    }

    if (pci_enabled) {
        pci_memory = g_new(MemoryRegion, 1);
        memory_region_init(pci_memory, "pci", INT64_MAX);
        rom_memory = pci_memory;
    } else {
        pci_memory = NULL;
        rom_memory = system_memory;
    }

    /* allocate ram and load rom/bios */
    if (!xen_enabled()) {
        pc_memory_init(system_memory,
                       kernel_filename, kernel_cmdline, initrd_filename,
                       below_4g_mem_size, above_4g_mem_size,
                       pci_enabled ? rom_memory : system_memory, &ram_memory);
    }

    gsi_state = g_malloc0(sizeof(*gsi_state));
    if (kvm_enabled() && kvm_irqchip_in_kernel()) {
        kvm_piix3_setup_irq_routing(pci_enabled);
        gsi = qemu_allocate_irqs(kvm_piix3_gsi_handler, gsi_state,
                                 GSI_NUM_PINS);
    } else {
        gsi = qemu_allocate_irqs(gsi_handler, gsi_state, GSI_NUM_PINS);
    }

    if (pci_enabled) {
        pci_bus = i440fx_init(&i440fx_state, &piix3_devfn, &isa_bus, gsi,
                              system_memory, system_io, ram_size,
                              below_4g_mem_size,
                              0x100000000ULL - below_4g_mem_size,
                              0x100000000ULL + above_4g_mem_size,
                              (sizeof(target_phys_addr_t) == 4
                               ? 0
                               : ((uint64_t)1 << 62)),
                              pci_memory, ram_memory);
    } else {
        pci_bus = NULL;
        i440fx_state = NULL;
        isa_bus = isa_bus_new(NULL, system_io);
        no_hpet = 1;
    }

    if (kvm_enabled() && kvm_irqchip_in_kernel()) {
        i8259 = kvm_i8259_init(isa_bus);
    } else if (xen_enabled()) {
        i8259 = xen_interrupt_controller_init();
    } else {
        cpu_irq = pc_allocate_cpu_irq();
        i8259 = i8259_init(isa_bus, cpu_irq[0]);
    }

    for (i = 0; i < ISA_NUM_IRQS; i++) {
        gsi_state->i8259_irq[i] = i8259[i];
    }
    if (pci_enabled) {
        ioapic_init(gsi_state);
    }

    pc_register_ferr_irq(gsi[13]);

    dev = pc_vga_init(isa_bus, pci_enabled ? pci_bus : NULL);
    if (dev) {
        object_property_add_child(object_get_root(), "vga", OBJECT(dev), NULL);
    }

    if (xen_enabled()) {
        pci_create_simple(pci_bus, -1, "xen-platform");
    }

    /* init basic PC hardware */
    pc_basic_device_init(isa_bus, gsi, &floppy, xen_enabled());

    for(i = 0; i < nb_nics; i++) {
        NICInfo *nd = &nd_table[i];

        if (!pci_enabled || (nd->model && strcmp(nd->model, "ne2k_isa") == 0))
            pc_init_ne2k_isa(isa_bus, nd);
        else
            pci_nic_init_nofail(nd, "e1000", NULL);
    }

    ide_drive_get(hd, MAX_IDE_BUS);
    if (pci_enabled) {
        PCIDevice *dev;
        if (xen_enabled()) {
            dev = pci_piix3_xen_ide_init(pci_bus, hd, piix3_devfn + 1);
        } else {
            dev = pci_piix3_ide_init(pci_bus, hd, piix3_devfn + 1);
        }
        idebus[0] = qdev_get_child_bus(&dev->qdev, "ide.0");
        idebus[1] = qdev_get_child_bus(&dev->qdev, "ide.1");
    } else {
        for(i = 0; i < MAX_IDE_BUS; i++) {
            ISADevice *dev;
            dev = isa_ide_init(isa_bus, ide_iobase[i], ide_iobase2[i],
                               ide_irq[i],
                               hd[MAX_IDE_DEVS * i], hd[MAX_IDE_DEVS * i + 1]);
            idebus[i] = qdev_get_child_bus(&dev->qdev, "ide.0");
        }
    }

    /* FIXME */
    rtc_state = ISA_DEVICE(object_resolve_path("rtc", NULL));
    qemu_register_boot_set(pc_boot_set, rtc_state);

    audio_init(isa_bus, pci_enabled ? pci_bus : NULL);

    pc_cmos_init(below_4g_mem_size, above_4g_mem_size, boot_device,
                 floppy, idebus[0], idebus[1], rtc_state);

    if (pci_enabled && usb_enabled) {
        usb_uhci_piix3_init(pci_bus, piix3_devfn + 2);
    }

    if (pci_enabled && acpi_enabled) {
        i2c_bus *smbus;

        if (!xen_enabled()) {
            cmos_s3 = qemu_allocate_irqs(pc_cmos_set_s3_resume, rtc_state, 1);
        } else {
            cmos_s3 = qemu_allocate_irqs(xen_cmos_set_s3_resume, rtc_state, 1);
        }
        smi_irq = qemu_allocate_irqs(pc_acpi_smi_interrupt, first_cpu, 1);
        /* TODO: Populate SPD eeprom data.  */
        smbus = piix4_pm_init(pci_bus, piix3_devfn + 3, 0xb100,
                              gsi[9], *cmos_s3, *smi_irq,
                              kvm_enabled());
        smbus_eeprom_init(smbus, 8, NULL, 0);
    }

    if (pci_enabled) {
        pc_pci_device_init(pci_bus);
    }
}

static void pc_init_pci(ram_addr_t ram_size,
                        const char *boot_device,
                        const char *kernel_filename,
                        const char *kernel_cmdline,
                        const char *initrd_filename,
                        const char *cpu_model)
{
    pc_init1(get_system_memory(),
             get_system_io(),
             ram_size, boot_device,
             kernel_filename, kernel_cmdline,
             initrd_filename, cpu_model, 1, 1);
}

static void pc_init_pci_no_kvmclock(ram_addr_t ram_size,
                                    const char *boot_device,
                                    const char *kernel_filename,
                                    const char *kernel_cmdline,
                                    const char *initrd_filename,
                                    const char *cpu_model)
{
    pc_init1(get_system_memory(),
             get_system_io(),
             ram_size, boot_device,
             kernel_filename, kernel_cmdline,
             initrd_filename, cpu_model, 1, 0);
}

static void pc_init_isa(ram_addr_t ram_size,
                        const char *boot_device,
                        const char *kernel_filename,
                        const char *kernel_cmdline,
                        const char *initrd_filename,
                        const char *cpu_model)
{
    if (cpu_model == NULL)
        cpu_model = "486";
    pc_init1(get_system_memory(),
             get_system_io(),
             ram_size, boot_device,
             kernel_filename, kernel_cmdline,
             initrd_filename, cpu_model, 0, 1);
}

#ifdef CONFIG_XEN
static void pc_xen_hvm_init(ram_addr_t ram_size,
                            const char *boot_device,
                            const char *kernel_filename,
                            const char *kernel_cmdline,
                            const char *initrd_filename,
                            const char *cpu_model)
{
    if (xen_hvm_init() != 0) {
        hw_error("xen hardware virtual machine initialisation failed");
    }
    pc_init_pci_no_kvmclock(ram_size, boot_device,
                            kernel_filename, kernel_cmdline,
                            initrd_filename, cpu_model);
    xen_vcpu_init();
}
#endif

static QEMUMachine pc_machine_v1_0 = {
    .name = "pc-1.0",
    .alias = "pc",
    .desc = "Standard PC",
    .init = pc_init_pci,
    .max_cpus = 255,
    .is_default = 1,
};

static QEMUMachine pc_machine_v0_15 = {
    .name = "pc-0.15",
    .desc = "Standard PC",
    .init = pc_init_pci,
    .max_cpus = 255,
    .is_default = 1,
};

static QEMUMachine pc_machine_v0_14 = {
    .name = "pc-0.14",
    .desc = "Standard PC",
    .init = pc_init_pci,
    .max_cpus = 255,
    .compat_props = (GlobalProperty[]) {
        {
            .driver   = "qxl",
            .property = "revision",
            .value    = stringify(2),
        },{
            .driver   = "qxl-vga",
            .property = "revision",
            .value    = stringify(2),
        },{
            .driver   = "virtio-blk-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-serial-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-net-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-balloon-pci",
            .property = "event_idx",
            .value    = "off",
        },
        { /* end of list */ }
    },
};

static QEMUMachine pc_machine_v0_13 = {
    .name = "pc-0.13",
    .desc = "Standard PC",
    .init = pc_init_pci_no_kvmclock,
    .max_cpus = 255,
    .compat_props = (GlobalProperty[]) {
        {
            .driver   = "virtio-9p-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "VGA",
            .property = "rombar",
            .value    = stringify(0),
        },{
            .driver   = "vmware-svga",
            .property = "rombar",
            .value    = stringify(0),
        },{
            .driver   = "PCI",
            .property = "command_serr_enable",
            .value    = "off",
        },{
            .driver   = "virtio-blk-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-serial-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-net-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-balloon-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "AC97",
            .property = "use_broken_id",
            .value    = stringify(1),
        },
        { /* end of list */ }
    },
};

static QEMUMachine pc_machine_v0_12 = {
    .name = "pc-0.12",
    .desc = "Standard PC",
    .init = pc_init_pci_no_kvmclock,
    .max_cpus = 255,
    .compat_props = (GlobalProperty[]) {
        {
            .driver   = "virtio-serial-pci",
            .property = "max_ports",
            .value    = stringify(1),
        },{
            .driver   = "virtio-serial-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "VGA",
            .property = "rombar",
            .value    = stringify(0),
        },{
            .driver   = "vmware-svga",
            .property = "rombar",
            .value    = stringify(0),
        },{
            .driver   = "PCI",
            .property = "command_serr_enable",
            .value    = "off",
        },{
            .driver   = "virtio-blk-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-serial-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-net-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-balloon-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "AC97",
            .property = "use_broken_id",
            .value    = stringify(1),
        },
        { /* end of list */ }
    }
};

static QEMUMachine pc_machine_v0_11 = {
    .name = "pc-0.11",
    .desc = "Standard PC, qemu 0.11",
    .init = pc_init_pci_no_kvmclock,
    .max_cpus = 255,
    .compat_props = (GlobalProperty[]) {
        {
            .driver   = "virtio-blk-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "virtio-serial-pci",
            .property = "max_ports",
            .value    = stringify(1),
        },{
            .driver   = "virtio-serial-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "ide-drive",
            .property = "ver",
            .value    = "0.11",
        },{
            .driver   = "scsi-disk",
            .property = "ver",
            .value    = "0.11",
        },{
            .driver   = "PCI",
            .property = "rombar",
            .value    = stringify(0),
        },{
            .driver   = "PCI",
            .property = "command_serr_enable",
            .value    = "off",
        },{
            .driver   = "virtio-blk-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-serial-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-net-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-balloon-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "AC97",
            .property = "use_broken_id",
            .value    = stringify(1),
        },
        { /* end of list */ }
    }
};

static QEMUMachine pc_machine_v0_10 = {
    .name = "pc-0.10",
    .desc = "Standard PC, qemu 0.10",
    .init = pc_init_pci_no_kvmclock,
    .max_cpus = 255,
    .compat_props = (GlobalProperty[]) {
        {
            .driver   = "virtio-blk-pci",
            .property = "class",
            .value    = stringify(PCI_CLASS_STORAGE_OTHER),
        },{
            .driver   = "virtio-serial-pci",
            .property = "class",
            .value    = stringify(PCI_CLASS_DISPLAY_OTHER),
        },{
            .driver   = "virtio-serial-pci",
            .property = "max_ports",
            .value    = stringify(1),
        },{
            .driver   = "virtio-serial-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "virtio-net-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "virtio-blk-pci",
            .property = "vectors",
            .value    = stringify(0),
        },{
            .driver   = "ide-drive",
            .property = "ver",
            .value    = "0.10",
        },{
            .driver   = "scsi-disk",
            .property = "ver",
            .value    = "0.10",
        },{
            .driver   = "PCI",
            .property = "rombar",
            .value    = stringify(0),
        },{
            .driver   = "PCI",
            .property = "command_serr_enable",
            .value    = "off",
        },{
            .driver   = "virtio-blk-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-serial-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-net-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "virtio-balloon-pci",
            .property = "event_idx",
            .value    = "off",
        },{
            .driver   = "AC97",
            .property = "use_broken_id",
            .value    = stringify(1),
        },
        { /* end of list */ }
    },
};

static QEMUMachine isapc_machine = {
    .name = "isapc",
    .desc = "ISA-only PC",
    .init = pc_init_isa,
    .max_cpus = 1,
};

#ifdef CONFIG_XEN
static QEMUMachine xenfv_machine = {
    .name = "xenfv",
    .desc = "Xen Fully-virtualized PC",
    .init = pc_xen_hvm_init,
    .max_cpus = HVM_MAX_VCPUS,
    .default_machine_opts = "accel=xen",
};
#endif

static void pc_machine_init(void)
{
    qemu_register_machine(&pc_machine_v1_0);
    qemu_register_machine(&pc_machine_v0_15);
    qemu_register_machine(&pc_machine_v0_14);
    qemu_register_machine(&pc_machine_v0_13);
    qemu_register_machine(&pc_machine_v0_12);
    qemu_register_machine(&pc_machine_v0_11);
    qemu_register_machine(&pc_machine_v0_10);
    qemu_register_machine(&isapc_machine);
#ifdef CONFIG_XEN
    qemu_register_machine(&xenfv_machine);
#endif
}

machine_init(pc_machine_init);
