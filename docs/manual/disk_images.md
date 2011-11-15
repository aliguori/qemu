Disk Images
===========

Since version 0.6.1, QEMU supports many disk image formats, including
growable disk images (their size increase as non empty sectors are
written), compressed and encrypted disk images. Version 0.8.3 added
the new qcow2 disk image format which is essential to support VM
snapshots.

Quick start for disk image creation
-----------------------------------

You can create a disk image with the command:

    qemu-img create myimage.img mysize

where *myimage.img* is the disk image filename and *mysize* is its
size in kilobytes. You can add an *M* suffix to give the size in
megabytes and a *G* suffix for gigabytes.

See *qemu_img_invocation* for more information.

Snapshot mode
-------------

If you use the option *-snapshot*, all disk images are
considered as read only. When sectors in written, they are written in
a temporary file created in */tmp*. You can however force the
write back to the raw disk images by using the *commit* monitor
command (or *C-a s* in the serial console).

VM snapshots
------------

VM snapshots are snapshots of the complete virtual machine including
CPU state, RAM, device state and the content of all the writable
disks. In order to use VM snapshots, you must have at least one non
removable and writable block device using the *qcow2* disk image
format. Normally this device is the first virtual hard drive.

Use the monitor command *savevm* to create a new VM snapshot or
replace an existing one. A human readable name can be assigned to each
snapshot in addition to its numerical ID.

Use *loadvm* to restore a VM snapshot and *delvm* to remove
a VM snapshot. *info snapshots* lists the available snapshots
with their associated information:

    (qemu) info snapshots
    Snapshot devices: hda
    Snapshot list (from hda):
    ID        TAG                 VM SIZE                DATE       VM CLOCK
    1         start                   41M 2006-08-06 12:38:02   00:00:14.954
    2                                 40M 2006-08-06 12:43:29   00:00:18.633
    3         msys                    40M 2006-08-06 12:44:04   00:00:23.514

A VM snapshot is made of a VM state info (its size is shown in
*info snapshots*) and a snapshot of every writable disk image.
The VM state info is stored in the first *qcow2* non removable
and writable block device. The disk image snapshots are stored in
every disk image. The size of a snapshot in a disk image is difficult
to evaluate and is not shown by *info snapshots* because the
associated disk sectors are shared among all the snapshots to save
disk space (otherwise each snapshot would need a full copy of all the
disk images).

When using the (unrelated) *-snapshot* option
(*disk_images_snapshot_mode*), you can always make VM snapshots,
but they are deleted as soon as you exit QEMU.

VM snapshots currently have the following known limitations:

 *  They cannot cope with removable devices if they are removed or inserted
    after a snapshot is done.

 * A few device drivers still have incomplete snapshot support so their state
   is not saved or restored properly (in particular USB).

qemu-img Invocation
-------------------

qemu-nbd Invocation
-------------------

Using host drives
-----------------

In addition to disk image files, QEMU can directly access host
devices. We describe here the usage for QEMU version >= 0.8.3.

### Linux

On Linux, you can directly use the host device filename instead of a
disk image filename provided you have enough privileges to access
it. For example, use */dev/cdrom* to access to the CDROM or
*/dev/fd0* for the floppy.

 * __CD__ You can specify a CDROM device even if no CDROM is loaded. QEMU has
   specific code to detect CDROM insertion or removal. CDROM ejection by the
   guest OS is supported. Currently only data CDs are supported.

 * __Floppy__ You can specify a floppy device even if no floppy is loaded.
   Floppy removal is currently not detected accurately (if you change floppy
   without doing floppy access while the floppy is not loaded, the guest OS
   will think that the same floppy is loaded).

 * __Hard disks__ Hard disks can be used. Normally you must specify the whole
   disk (*/dev/hdb* instead of */dev/hdb1*) so that the guest OS can see it as
   a partitioned disk. WARNING: unless you know what you do, it is better to
   only make READ-ONLY accesses to the hard disk otherwise you may corrupt your
   host data (use the *-snapshot* command line option or modify the device
   permissions accordingly).

### Windows

 * __CD__ The preferred syntax is the drive letter (e.g. *d:*). The alternate
   syntax *\\.\d:* is supported. */dev/cdrom* is supported as an alias to the
   first CDROM drive.

   Currently there is no specific code to handle removable media, so it is
   better to use the *change* or *eject* monitor commands to change or eject
   media.

 * __Hard disks__ Hard disks can be used with the syntax:
   *\\.\PhysicalDrive@var{N*} where *N* is the drive number (0 is the first
   hard disk).

   WARNING: unless you know what you do, it is better to only make READ-ONLY
   accesses to the hard disk otherwise you may corrupt your host data (use the
   *-snapshot* command line so that the modifications are written in a
   temporary file).

### Mac OS X

*/dev/cdrom* is an alias to the first CDROM.

Currently there is no specific code to handle removable media, so it
is better to use the *change* or *eject* monitor commands to
change or eject media.

Virtual FAT disk images
-----------------------

QEMU can automatically create a virtual FAT disk image from a
directory tree. In order to use it, just type:

    qemu linux.img -hdb fat:/my_directory

Then you access access to all the files in the */my_directory*
directory without having to copy them in a disk image or to export
them via SAMBA or NFS. The default access is *read-only*.

Floppies can be emulated with the *:floppy:* option:

    qemu linux.img -fda fat:floppy:/my_directory

A read/write support is available for testing (beta stage) with the
*:rw:* option:

    qemu linux.img -fda fat:floppy:rw:/my_directory

What you should *never* do:

 * use non-ASCII filenames ;
 * use "-snapshot" together with ":rw:" ;
 * expect it to work when loadvm'ing ;
 * write to the FAT directory on the host system while accessing it with the
   guest system.

NBD access
----------

QEMU can access directly to block device exported using the Network Block Device
protocol.

    qemu linux.img -hdb nbd:my_nbd_server.mydomain.org:1024

If the NBD server is located on the same host, you can use an unix socket
instead of an inet socket:

    qemu linux.img -hdb nbd:unix:/tmp/my_socket

In this case, the block device must be exported using qemu-nbd:

    qemu-nbd --socket=/tmp/my_socket my_disk.qcow2

The use of qemu-nbd allows to share a disk between several guests:

    qemu-nbd --socket=/tmp/my_socket --share=2 my_disk.qcow2

and then you can use it with two guests:

    qemu linux1.img -hdb nbd:unix:/tmp/my_socket
    qemu linux2.img -hdb nbd:unix:/tmp/my_socket

If the nbd-server uses named exports (since NBD 2.9.18), you must use the
"exportname" option:

    qemu -cdrom nbd:localhost:exportname=debian-500-ppc-netinst
    qemu -cdrom nbd:localhost:exportname=openSUSE-11.1-ppc-netinst

Sheepdog disk images
--------------------

Sheepdog is a distributed storage system for QEMU.  It provides highly
available block level storage volumes that can be attached to QEMU-based
virtual machines.

You can create a Sheepdog disk image with the command:

    qemu-img create sheepdog:*image* *size*

where *image* is the Sheepdog image name and *size* is its size.

To import the existing *filename* to Sheepdog, you can use a convert command.

    qemu-img convert *filename* sheepdog:*image*

You can boot from the Sheepdog disk image with the command:

    qemu sheepdog:*image*

You can also create a snapshot of the Sheepdog image like qcow2.

    qemu-img snapshot -c *tag* sheepdog:*image*

where *tag* is a tag name of the newly created snapshot.

To boot from the Sheepdog snapshot, specify the tag name of the snapshot.

    qemu sheepdog:*image*:*tag*

You can create a cloned image from the existing snapshot.

    qemu-img create -b sheepdog:*base*:*tag* sheepdog:*image*

where *base* is a image name of the source snapshot and *tag* is its tag name.

If the Sheepdog daemon doesn't run on the local host, you need to
specify one of the Sheepdog servers to connect to.

    qemu-img create sheepdog:*hostname*:*port*:*image* *size*
    qemu sheepdog:*hostname*:*port*:*image*

