#!/bin/sh

in_host() {
    qemu -nographic -enable-kvm
}

in_guest() {
    :
}

if test $QEMU_TEST; then
    in_host
else
    in_guest
fi
