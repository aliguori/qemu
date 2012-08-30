#!/bin/sh

in_host() {
    machine=`choose pc-0.13 pc-0.14 pc-0.15 pc-1.0`

    tmpdisk=$tmpdir/disk-$$.img
    qemu-img create -f raw $tmpdisk 10G

    # N.B. If you change this invocation line, please update all of the files
    # stored in fingerprints/
    echo "Using machine: $machine"
    if test "$QEMU_TEST_GEN_FINGERPRINT" = "yes"; then
	machine="pc"
    fi
    qemu -nographic -enable-kvm -hda $tmpdisk -M $machine \
         -drive file=$tmpdisk,if=virtio,snapshot=on \
	 -device virtio-balloon-pci \
	 -device virtio-serial \
	 -net nic,model=virtio -net user
    rc=$?

    if test $rc = 0; then
	# this should create one file named fingerprint.txt
	tar xf $tmpdisk
	if test -e fingerprint.txt -a \
	    "$QEMU_TEST_GEN_FINGERPRINT" != "yes"; then
	    grep -v bios_date fingerprints/$machine.x86_64 > a
	    grep -v bios_date fingerprint.txt > b
	    diff -u a b
	    rc=$?
	    if test $rc != 0; then
		echo "Guest fingerprint changed for $machine!"
	    fi
	else
	    rc=1
	fi
    fi

    rm -f $tmpdisk
    if test "$QEMU_TEST_GEN_FINGERPRINT" != "yes"; then
	rm -f fingerprint.txt
    fi
	
    return $rc
}

in_guest() {
    fingerprint > fingerprint.txt
    tar cf /dev/sda fingerprint.txt
}

if test $QEMU_TEST; then
    in_host
else
    in_guest
fi
