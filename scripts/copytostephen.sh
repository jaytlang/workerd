#!/bin/sh
# give stephen the code

set -x

if [ `whoami` != "root" ]; then
	echo "must be root to run this script"
	exit 1
fi

hd="/home/skandeh"
wd="$hd/jayscode"

mkdir -p $wd
cp src/*.[ch] src/Makefile $wd/

chown skandeh:skandeh $wd
chmod 755 $wd
find $wd/* | xargs chmod 644
