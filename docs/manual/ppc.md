PowerPC System emulator
=======================

Use the executable *qemu-system-ppc* to simulate a complete PREP
or PowerMac PowerPC system.

QEMU emulates the following PowerMac peripherals:

 * UniNorth or Grackle PCI Bridge
 * PCI VGA compatible card with VESA Bochs Extensions
 * 2 PMAC IDE interfaces with hard disk and CD-ROM support
 * NE2000 PCI adapters
 * Non Volatile RAM
 * VIA-CUDA with ADB keyboard and mouse.

QEMU emulates the following PREP peripherals:

 * PCI Bridge
 * PCI VGA compatible card with VESA Bochs Extensions
 * 2 IDE interfaces with hard disk and CD-ROM support
 * Floppy disk
 * NE2000 network adapters
 * Serial port
 * PREP Non Volatile RAM
 * PC compatible keyboard and mouse.

QEMU uses the Open Hack'Ware Open Firmware Compatible BIOS available at[1][]

Since version 0.9.1, QEMU uses [OpenBIOS](http://www.openbios.org/) for the
g3beige and mac99 PowerMac machines. OpenBIOS is a free (GPLv2) portable
firmware implementation. The goal is to implement a 100% IEEE 1275-1994
(referred to as Open Firmware) compliant firmware.

Options
-------

The following options are specific to the PowerPC emulation:

 * -g *WxH[xDEPTH]*

Set the initial VGA graphic mode. The default is 800x600x15.

 * -prom-env *string*

Set OpenBIOS variables in NVRAM, for example:

    qemu-system-ppc -prom-env 'auto-boot?=false' \
     -prom-env 'boot-device=hd:2,\yaboot' \
     -prom-env 'boot-args=conf=hd:2,\yaboot.conf'

These variables are not used by Open Hack'Ware.

More information is available at [2][].

[1]: http://perso.magic.fr/l_indien/OpenHackWare/index.htm
[2]: http://perso.magic.fr/l_indien/qemu-ppc/

