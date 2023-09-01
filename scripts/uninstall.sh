#!/bin/sh
# because why not
# this is not intended for
# serious parameterizable use

set -x

doas rcctl stop workerd
doas rcctl disable workerd
doas rm -f /etc/rc.d/workerd

# speed hack to support FAST=1
if [ -f /home/_workerd/base.qcow2 ]; then
	mv /home/_workerd/*.qcow2 images
	chown $USER:$USER images/*
	chmod 644 images/*
fi

yes | doas rmuser _workerd
doas rm -rf /home/_workerd
doas rm -rf /var/workerd
doas rm -f /usr/sbin/workerd

doas rm -f /etc/signify/bundled.pub
doas rm -f /etc/workerd.conf

doas rm -f /etc/vm.conf
doas vmctl stop -fwa
doas rcctl stop vmd
doas rcctl start vmd
