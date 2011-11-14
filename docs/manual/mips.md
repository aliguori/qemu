MIPS System emulator
====================

Four executables cover simulation of 32 and 64-bit MIPS systems in both endian
options, *qemu-system-mips*, *qemu-system-mipsel*, *qemu-system-mips64* and
*qemu-system-mips64el*.

Five different machine types are emulated:

 * A generic ISA PC-like machine "mips"
 * The MIPS Malta prototype board "malta"
 * An ACER Pica "pica61". This machine needs the 64-bit emulator.
 * MIPS emulator pseudo board "mipssim"
 * A MIPS Magnum R4000 machine "magnum". This machine needs the 64-bit emulator.

The generic emulation is supported by Debian 'Etch' and is able to install
Debian into a virtual disk image. The following devices are emulated:

 * A range of MIPS CPUs, default is the 24Kf
 * PC style serial port
 * PC style IDE disk
 * NE2000 network card

The Malta emulation supports the following devices:

 * Core board with MIPS 24Kf CPU and Galileo system controller
 * PIIX4 PCI/USB/SMbus controller
 * The Multi-I/O chip's serial device
 * PCI network cards (PCnet32 and others)
 * Malta FPGA serial device
 * Cirrus (default) or any other PCI VGA graphics card

The ACER Pica emulation supports:

 * MIPS R4000 CPU
 * PC-style IRQ and DMA controllers
 * PC Keyboard
 * IDE controller

The mipssim pseudo board emulation provides an environment similiar to what the
proprietary MIPS emulator uses for running Linux. It supports:

 * A range of MIPS CPUs, default is the 24Kf
 * PC style serial port
 * MIPSnet network emulation

The MIPS Magnum R4000 emulation supports:

 * MIPS R4000 CPU
 * PC-style IRQ controller
 * PC Keyboard
 * SCSI controller
 * G364 framebuffer
