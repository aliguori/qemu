#!/bin/sh

src=$1
shift

tmpdir=$(pwd)/.tmp-$$

mkdir $tmpdir

(gzip -d < $src | (cd $tmpdir; cpio -i --make-directories; cd ..)) 2>/dev/null

mkdir -p $tmpdir/tests
for i in "$@"; do
    cp $i $tmpdir/tests/
done

cd $tmpdir
(find . -print | cpio -H newc -o | gzip) 2>/dev/null
cd ..

rm -rf $tmpdir
