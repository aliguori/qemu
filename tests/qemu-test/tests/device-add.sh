#!/bin/sh

canary="** waiting for hotplug **"

in_host() {
    tmpdisk=$tmpdir/disk.img

    # make sure to test various of -device, anonymous, and named
    extra_arg=`choose "-device virtio-balloon-pci" "-device virtio-balloon-pci,id=balloon0" none`

    if test "$extra_arg" = "none"; then
	extra_arg=""
    fi

    qemu-img create -f qcow2 $tmpdisk 10G

    start_qemu -nographic -enable-kvm $extra_arg

    while qemu_is_okay; do
	if grep "$canary" $tmplog >/dev/null; then
	    out=`hmp drive_add auto file=$tmpdisk,if=none,id=hd0`
	    if test $(echo "$out") != "OK"; then
		echo "drive_add failed!"
		echo "$out"
		rm $tmpdisk
		kill $pid
		return 1
	    fi

	    qmp device_add --driver=virtio-blk-pci --drive=hd0
	    rc=$?
	    if test $rc != 0; then
		echo "device_add failed!"
		rm $tmpdisk
		kill $pid
		return 1
	    fi

	    echo "** waiting for guest to see device **"

	    while qemu_is_okay; do
		sleep 1
	    done

	    break
	fi
	sleep 1
    done

    get_qemu_status
    rc=$?

    rm -f $tmpdisk

    return $rc
}

in_guest() {
    echo
    echo "$canary"
    while ! grep vda /proc/partitions >/dev/null; do
	sleep 1
    done
}

if test $QEMU_TEST; then
    in_host
else
    in_guest
fi
