Other Devices
=============

Inter-VM Shared Memory device
-----------------------------

With KVM enabled on a Linux host, a shared memory device is available.  Guests
map a POSIX shared memory region into the guest as a PCI device that enables
zero-copy communication to the application level of the guests.  The basic
syntax is:

    qemu -device ivshmem,size=<size in format accepted by -m>[,shm=<shm name>]

If desired, interrupts can be sent between guest VMs accessing the same shared
memory region.  Interrupt support requires using a shared memory server and
using a chardev socket to connect to it.  The code for the shared memory server
is qemu.git/contrib/ivshmem-server.  An example syntax when using the shared
memory server is:

    qemu -device ivshmem,size=<size in format accepted by -m>[,chardev=<id>]
                            [,msi=on][,ioeventfd=on][,vectors=n][,role=peer|master]
    qemu -chardev socket,path=<path>,id=<id>

When using the server, the guest will be assigned a VM ID (>=0) that allows
guests using the same server to communicate via interrupts.  Guests can read
their VM ID from a device register (see example code).  Since receiving the
shared memory region from the server is asynchronous, there is a (small) chance
the guest may boot before the shared memory is attached.  To allow an
application to ensure shared memory is attached, the VM ID register will return
-1 (an invalid VM ID) until the memory is attached.  Once the shared memory is
attached, the VM ID will return the guest's valid VM ID.  With these semantics,
the guest application can check to ensure the shared memory is attached to the
guest before proceeding.

The *role* argument can be set to either master or peer and will affect
how the shared memory is migrated.  With *role=master*, the guest will
copy the shared memory on migration to the destination host.  With
*role=peer*, the guest will not be able to migrate with the device attached.
With the *peer* case, the device should be detached and then reattached
after migration using the PCI hotplug support.

