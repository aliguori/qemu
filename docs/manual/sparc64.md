Sparc64 System emulator
=======================

Use the executable *qemu-system-sparc64* to simulate a Sun4u (UltraSPARC
PC-like machine), Sun4v (T1 PC-like machine), or generic Niagara (T1) machine.
The emulator is not usable for anything yet, but it can launch some kernels.

QEMU emulates the following peripherals:

 * UltraSparc IIi APB PCI Bridge
 * PCI VGA compatible card with VESA Bochs Extensions
 * PS/2 mouse and keyboard
 * Non Volatile RAM M48T59
 * PC-compatible serial ports
 * 2 PCI IDE interfaces with hard disk and CD-ROM support
 * Floppy disk

Options
-------

The following options are specific to the Sparc64 emulation:

 * -prom-env *string*

Set OpenBIOS variables in NVRAM, for example:

    qemu-system-sparc64 -prom-env 'auto-boot?=false'

 * -M [sun4u|sun4v|Niagara]

Set the emulated machine type. The default is sun4u.
