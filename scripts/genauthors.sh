#!/bin/sh
#
# authors.c generator
#
# Copyright IBM, Corp. 2013
#
# Authors:
#  Anthony Liguori <aliguori@us.ibm.com>
#
# This work is licensed under the terms of the GNU GPLv2 or later.
# See the COPYING file in the top-level directory.

# Beginning of git history.  author name is meaningless in the SVN days.
start=d6dc3d424e26b49e1f37f1ef2598eacf3bc4eac7

echo "#include \"qemu/authors.h\""
echo
echo "const char *qemu_authors[] = {"
git log --format="%an" $start..HEAD | sort -u | while read AUTHOR; do
    COUNT=`git log --author="$AUTHOR" --format=oneline $start..HEAD | wc -l`
    echo "$COUNT $AUTHOR"
done | sort -rg | while read COUNT AUTHOR; do
    echo "    \"$AUTHOR\",  /* $COUNT */"
done
echo "    NULL"
echo "};"

