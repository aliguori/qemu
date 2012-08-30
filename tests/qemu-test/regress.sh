#!/bin/bash

#qemu=~/build/qemu/x86_64-softmmu/qemu-system-x86_64
qemu=~/build/qemu/qemu-1.2.0-rc2/x86_64-softmmu/qemu-system-x86_64

for ((i=0;i<50;i++)); do
    for t in tests/*.sh; do
	echo "Running $t"
	./qemu-test $qemu $t || exit 1
    done
done
