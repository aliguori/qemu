#!/bin/sh

in_host() {
    nic=`named_choose nic tier2 rtl8139 e1000 virtio`
    if test "$nic" = "tier2"; then
	nic=`named_choose nic.tier2 ne2k_pci i82551 i82557b i82559er pcnet`
    fi
    echo "Using networking card: $nic"
    qemu -nographic -enable-kvm -net user -net nic,model=$nic
}

in_guest() {
    udhcpc -i eth0 -f -n -q
    wget -O /dev/null http://www.qemu.org
}

if test $QEMU_TEST; then
    in_host
else
    in_guest
fi
