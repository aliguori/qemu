#!/bin/sh

canary="** waiting for... **"

in_host() {
    tmpchr=$tmpdir/chr.log

    # Also test alias
    devname=`choose virtio-serial virtio-serial-pci`

    qemu -nographic -enable-kvm -device $devname \
        -device virtserialport,name=org.qemu.test,chardev=chr0 \
        -chardev file,path=$tmpchr,id=chr0
    rc=$?

    if test $rc = 0; then
	if ! grep "$canary" $tmpchr >/dev/null; then
	    echo "Failed to see output from guest!"
	    rc=1
	fi
    fi

    rm -f $tmpchr

    return $rc
}

in_guest() {
    sysfspath=/sys/bus/virtio/devices/virtio0/virtio-ports/vport0p1
    if ! test -e $sysfspath/name; then
	echo "Device not visible!"
	return 1
    fi

    name=`cat $sysfspath/name`

    if test "$name" != "org.qemu.test"; then
	echo "Device has wrong name!"
	echo "Expected 'org.qemu.test', got '$name'"
	return 2
    fi

    echo "$canary" > /dev/vport0p1

    return 0
}

if test $QEMU_TEST; then
    in_host
else
    in_guest
fi
