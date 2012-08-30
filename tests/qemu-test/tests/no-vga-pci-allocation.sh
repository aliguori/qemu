#!/bin/sh

# This test case reproduces an issue discovered in libguestfs that happened due
# to a PCI allocation bug in SeaBIOS.
#
# It only reproduced with libguestfs because libguestfs uses -nodefaults which
# prevents a VGA device from being initialized.  It also required the presence
# of at least a few PCI devices.
#
# See http://mid.gmane.org/20110930131148.GA4294@amd.home.annexia.org

in_host() {
    tmpdisk=$tmpdir/disk-$$.img
    tmpsock=$tmpdir/channel-$$.sock

    qemu-img create -f qcow2 $tmpdisk 10G

    qemu -drive file=$tmpdisk,if=none,snapshot=on,id=hd0 \
	-device virtio-balloon-pci,addr=03.0 \
	-device virtio-blk-pci,addr=04.0,drive=hd0 \
	-nographic -nodefconfig -m 1G -no-reboot -no-hpet \
	-device virtio-serial \
	-chardev socket,path=$tmpsock,id=channel0,server,nowait \
	-device virtserialport,chardev=channel0,name=org.libguestfs.channel.0 \
	-nodefaults -serial stdio -enable-kvm
    rc=$?

    rm -f $tmpdisk $tmpsock

    return $rc
}

in_guest() {
    :
}

if test $QEMU_TEST; then
    in_host
else
    in_guest
fi
