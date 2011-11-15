USB emulation
=============

QEMU emulates a PCI UHCI USB controller. You can virtually plug virtual USB
devices or real host USB devices (experimental, works only on Linux hosts).
Qemu will automatically create and connect virtual USB hubs as necessary to
connect multiple USB devices.

Connecting USB devices
----------------------

USB devices can be connected with the *-usbdevice* commandline option
or the *usb_add* monitor command.  Available devices are:

 * __mouse__ Virtual Mouse.  This will override the PS/2 mouse emulation when
   activated.

 * __tablet__ Pointer device that uses absolute coordinates (like a
   touchscreen). This means qemu is able to report the mouse position without
   having to grab the mouse.  Also overrides the PS/2 mouse emulation when
   activated.

 * __disk:*file*__ Mass storage device based on *file*

 * __host:*bus.addr*__ Pass through the host device identified by
   *bus.addr*

 * __keyboard__ Standard USB keyboard.  Will override the PS/2 keyboard (if
   present).

 * __serial:[vendorid=vendor_id][,product_id=product_id]:dev__ Serial
   converter. This emulates an FTDI FT232BM chip connected to host character
   device *dev*. The available character devices are the same as for the
   *-serial* option. The *vendorid* and *productid* options can
   be used to override the default 0403:6001. For instance,

       usb_add serial:productid=FA00:tcp:192.168.0.2:4444

   will connect to tcp port 4444 of ip 192.168.0.2, and plug that to the virtual
   serial converter, faking a Matrix Orbital LCD Display (USB ID 0403:FA00).

 * __braille__ Braille device.  This will use BrlAPI to display the braille
   output on a real or fake device.

 * __net:*options*__ Network adapter that supports CDC ethernet and RNDIS
   protocols.  *options* specifies NIC options as with *-net nic,*options* (see
   description). For instance, user-mode networking can be used with

       qemu [...OPTIONS...] -net user,vlan=0 -usbdevice net:vlan=0

   Currently this cannot be used in machines that support PCI NICs.

 * __bt[:*hci-type*]__ Bluetooth dongle whose type is specified in the same
   format as with the *-bt hci* option.  If no type is given, the HCI logic
   corresponds to *-bt hci,vlan=0*. This USB device implements the USB
   Transport Layer of HCI.  Example usage:

       qemu [...OPTIONS...] -usbdevice bt:hci,vlan=3 -bt device:keyboard,vlan=3

(Linux only)

 * __host:*vendor_id:product_id*__ Pass through the host device identified by
   *vendor_id:product_id*

 * __wacom-tablet__ Virtual Wacom PenPartner tablet.  This device is similar to
   the *tablet* above but it can be used with the tslib library because in
   addition to touch coordinates it reports touch pressure.

Using host USB devices on a Linux host
--------------------------------------

WARNING: this is an experimental feature. QEMU will slow down when using it.
USB devices requiring real time streaming (i.e. USB Video Cameras) are not
supported yet.

 * If you use an early Linux 2.4 kernel, verify that no Linux driver is
   actually using the USB device. A simple way to do that is simply to disable
   the corresponding kernel module by renaming it from *mydriver.o* to
   *mydriver.o.disabled*.

 * Verify that */proc/bus/usb* is working (most Linux distributions should
   enable it by default). You should see something like that:

       ls /proc/bus/usb
       001  devices  drivers

 * Since only root can access to the USB devices directly, you can either
   launch QEMU as root or change the permissions of the USB devices you want to
   use. For testing, the following suffices:

       chown -R myuid /proc/bus/usb

 * Launch QEMU and do in the monitor:

       info usbhost
       Device 1.2, speed 480 Mb/s
         Class 00: USB device 1234:5678, USB DISK

   You should see the list of the devices you can use (Never try to use hubs,
   it won't work).

 * Add the device in QEMU by using:

       usb_add host:1234:5678

   Normally the guest OS should report that a new USB device is plugged. You
   can use the option *-usbdevice* to do the same.

 * Now you can try to use the host USB device in QEMU.

   When relaunching QEMU, you may have to unplug and plug again the USB device
   to make it work again (this is a bug).
