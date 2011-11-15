Direct Linux Boot
=================

This section explains how to launch a Linux kernel inside QEMU without
having to make a full bootable image. It is very useful for fast Linux
kernel testing.

The syntax is:

    qemu -kernel arch/i386/boot/bzImage -hda root-2.4.20.img -append "root=/dev/hda"

Use *-kernel* to provide the Linux kernel image and *-append* to give the
kernel command line arguments. The *-initrd* option can be used to provide an
INITRD image.

When using the direct Linux boot, a disk image for the first hard disk *hda* is
required because its boot sector is used to launch the Linux kernel.

If you do not need graphical output, you can disable it and redirect the
virtual serial port and the QEMU monitor to the console with the *-nographic*
option. The typical command line is:

    qemu -kernel arch/i386/boot/bzImage -hda root-2.4.20.img \
         -append "root=/dev/hda console=ttyS0" -nographic

Use *Ctrl-a c* to switch between the serial console and the monitor.

