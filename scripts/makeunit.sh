#!/bin/sh
# invoke from unit/
# make a new unit test

[ -n "$1" ] || { echo "usage: $0 testname"; exit 1; }

mkdir $1
echo ".include <bsd.prog.mk>" > $1/Makefile
touch $1/test.c
