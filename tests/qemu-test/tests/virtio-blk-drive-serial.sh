#!/bin/sh

serial="0123456789abcdefghi"

in_host() {
    tmpdisk=$tmpdir/disk.img
    qemu-img create -f qcow2 $tmpdisk 10G

    qemu -nographic -enable-kvm \
    -drive file=$tmpdisk,if=none,id=drive-virtio-disk0,format=raw,cache=none,serial=$serial \
    -device virtio-blk-pci,bus=pci.0,addr=0x4,drive=drive-virtio-disk0,id=virtio-disk0
    rc=$?

    rm $tmpdisk
    return $rc
}

in_guest() {
    sysfspath=/sys/block/vda
    if ! test -e $sysfspath; then
    echo "Device not visible!"
    return 1
    fi

    guest_serial=`cat $sysfspath/serial`

    if test "$guest_serial" != "$serial"; then
    echo "drive has wrong serial!"
    echo "Expected '$serial', got '$guest_serial'"
    return 2
    fi

    return 0
}

if test $QEMU_TEST; then
    in_host
else
    in_guest
fi
