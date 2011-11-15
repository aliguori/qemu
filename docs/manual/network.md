Network emulation
=================

QEMU can simulate several network cards (PCI or ISA cards on the PC
target) and can connect them to an arbitrary number of Virtual Local
Area Networks (VLANs). Host TAP devices can be connected to any QEMU
VLAN. VLAN can be connected between separate instances of QEMU to
simulate large networks. For simpler usage, a non privileged user mode
network stack can replace the TAP device to have a basic network
connection.

VLANs
-----

QEMU simulates several VLANs. A VLAN can be symbolised as a virtual
connection between several network devices. These devices can be for
example QEMU virtual Ethernet cards or virtual Host ethernet devices
(TAP devices).

Using TAP network interfaces
----------------------------

This is the standard way to connect QEMU to a real network. QEMU adds
a virtual network device on your host (called *tapN*), and you
can then configure it as if it was a real ethernet card.

Linux host
----------

As an example, you can download the *linux-test-xxx.tar.gz*
archive and copy the script *qemu-ifup* in */etc* and
configure properly *sudo* so that the command *ifconfig*
contained in *qemu-ifup* can be executed as root. You must verify
that your host kernel supports the TAP network interfaces: the
device */dev/net/tun* must be present.

See *sec_invocation* to have examples of command lines using the
TAP network interfaces.

Windows host
------------

There is a virtual ethernet driver for Windows 2000/XP systems, called
TAP-Win32. But it is not included in standard QEMU for Windows,
so you will need to get it separately. It is part of OpenVPN package,
so download [OpenVPN](http://openvpn.net/).

Using the user mode network stack
---------------------------------

By using the option *-net user* (default configuration if no
*-net* option is specified), QEMU uses a completely user mode
network stack (you don't need root privilege to use the virtual
network). The virtual network configuration is the following:

         QEMU VLAN      <------>  Firewall/DHCP server <-----> Internet
                           |          (10.0.2.2)
                           |
                           ---->  DNS server (10.0.2.3)
                           |
                           ---->  SMB server (10.0.2.4)

The QEMU VM behaves as if it was behind a firewall which blocks all
incoming connections. You can use a DHCP client to automatically
configure the network in the QEMU VM. The DHCP server assign addresses
to the hosts starting from 10.0.2.15.

In order to check that the user mode network is working, you can ping
the address 10.0.2.2 and verify that you got an address in the range
10.0.2.x from the QEMU virtual DHCP server.

Note that *ping* is not supported reliably to the internet as it
would require root privileges. It means you can only ping the local
router (10.0.2.2).

When using the built-in TFTP server, the router is also the TFTP
server.

When using the *-redir* option, TCP or UDP connections can be
redirected from the host to the guest. It allows for example to
redirect X11, telnet or SSH connections.

Connecting VLANs between QEMU instances
---------------------------------------

Using the *-net socket* option, it is possible to make VLANs
that span several QEMU instances. See *sec_invocation* to have a
basic example.

