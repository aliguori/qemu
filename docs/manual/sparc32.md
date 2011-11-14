Sparc32 System emulator
=======================

Use the executable *qemu-system-sparc* to simulate the following Sun4m
architecture machines:

 * SPARCstation 4
 * SPARCstation 5
 * SPARCstation 10
 * SPARCstation 20
 * SPARCserver 600MP
 * SPARCstation LX
 * SPARCstation Voyager
 * SPARCclassic
 * SPARCbook

The emulation is somewhat complete. SMP up to 16 CPUs is supported,
but Linux limits the number of usable CPUs to 4.

It's also possible to simulate a SPARCstation 2 (sun4c architecture),
SPARCserver 1000, or SPARCcenter 2000 (sun4d architecture), but these
emulators are not usable yet.

QEMU emulates the following sun4m/sun4c/sun4d peripherals:

 * IOMMU or IO-UNITs
 * TCX Frame buffer
 * Lance (Am7990) Ethernet
 * Non Volatile RAM M48T02/M48T08
 * Slave I/O: timers, interrupt controllers, Zilog serial ports, keyboard and
   power/reset logic
 * ESP SCSI controller with hard disk and CD-ROM support
 * Floppy drive (not on SS-600MP)
 * CS4231 sound device (only on SS-5, not working yet)

The number of peripherals is fixed in the architecture.  Maximum memory size
depends on the machine type, for SS-5 it is 256MB and for others 2047MB.

Since version 0.8.2, QEMU uses [OpenBIOS](http://www.openbios.org/). OpenBIOS
is a free (GPL v2) portable firmware implementation. The goal is to implement a
100% IEEE 1275-1994 (referred to as Open Firmware) compliant firmware.

A sample Linux 2.6 series kernel and ram disk image are available on the QEMU
web site. There are still issues with NetBSD and OpenBSD, but some kernel
versions work. Please note that currently Solaris kernels don't work probably
due to interface issues between OpenBIOS and Solaris.

Options
-------

The following options are specific to the Sparc32 emulation:

 * -g *WxHx[xDEPTH]*

Set the initial TCX graphic mode. The default is 1024x768x8, currently the only
other possible mode is 1024x768x24.

 * -prom-env *string*

Set OpenBIOS variables in NVRAM, for example:

    qemu-system-sparc -prom-env 'auto-boot?=false' \
     -prom-env 'boot-device=sd(0,2,0):d' -prom-env 'boot-args=linux single'

 * -M [SS-4|SS-5|SS-10|SS-20|SS-600MP|LX|Voyager|SPARCClassic] [|SPARCbook|SS-2|SS-1000|SS-2000]

Set the emulated machine type. Default is SS-5.

