SUBDIR+=	src
SUDO?=		doas

# Note: you will need to set up some firewall rules
# to make sure the backend can only connect to VMs, not
# the outside world. See workerd.h and the port number
# #defines for more information

# TODO: move a lot of this to a configuration file
# for now, using #defines in the project for these variables

RCDAEMON=	workerd
USER=		_workerd
GECOS=		"Student Job Scheduler Daemon"

CA=		mitcca.pem
CERT=		jaytlang.pem
KEY=		jaytlang.key

SPUB=		bundled.pub

CHROOT=		${DESTDIR}/var/workerd
DISKS=		${CHROOT}/disks
FMESSAGES=	${CHROOT}/fmessages
EMESSAGES=	${CHROOT}/emessages
WRITEBACK=	${CHROOT}/writeback

# base images live in /home, because /home
# installs to a _much larger_ partition than /var
# where dynamic data will live. the install rule will
# copy these to /home/${USER}...
BASEIMAGE=	base.qcow2
BASEVIVADO=	vivado.qcow2

CONFIG=		workerd.conf
VMCONFIG=	vm.conf

SERVERSEC=	/etc/ssl/private/server.key
SERVERCA=	/etc/ssl/authority/serverchain.pem
SERVERPUB=	/etc/ssl/server.pem

# Build configuration ends here

REHASH=		scripts/rehash.sh
UGOUID=		's/.*\(....\)/\1/'

checkcerts:
	[ -f "${SERVERPUB}" ] || { 					\
		cat << EOF;						\
Makefile configuration has SERVERPUB='${SERVERPUB}', but the file	\
${SERVERPUB} does not exist; install cannot continue.			\
EOF									\
		exit 1;							\
	}
	[ -f "${SERVERCA}" ] || {					\
		cat << EOF;						\
Makefile configuration has SERVERCA='${SERVERCA}', but the		\
file ${SERVERCA} does not exist; install cannot continue.		\
EOF									\
		exit 1;							\
	}
	doas [ -f "${SERVERSEC}" ] || {					\
		cat << EOF;						\
Makefile configuration has SERVERSEC='${SERVERSEC}', but the		\
file ${SERVERSEC} does not exist; install cannot continue.		\
EOF									\
		exit 1;							\
	}
	if [ `stat -f "%p%u" ${SERVERPUB} | sed ${UGOUID}` != "4440" ];	\
	then								\
		echo "${SERVERPUB} has incorrect permissions";		\
		exit 1;							\
	fi
	if [ `stat -f "%p%u" ${SERVERCA} | sed ${UGOUID}` != "4440" ];	\
	then								\
		echo "${SERVERCA} has incorrect permissions";		\
		exit 1;							\
	fi
	if [ `stat -f "%p%u" ${SERVERSEC} | sed ${UGOUID}` != "4000" ];	\
	then								\
		echo "${SERVERSEC} has incorrect permissions";		\
		exit 1;							\
	fi



checkroot:
	[ `whoami` = "root" ] || {					\
		echo "install should be run as root";			\
		exit 1;							\
	}

beforeinstall: checkroot checkcerts
	if [ -d "${CHROOT}" ]; then					\
		echo "install already ran";				\
		exit 1;							\
	fi

afterinstall:
	useradd -c ${GECOS} -k /var/empty -s /sbin/nologin -d /home/${USER}	\
		-m ${USER} 2>/dev/null
	${INSTALL} -o root -g wheel -m 755 -d /home/${USER}
	${INSTALL} -o root -g daemon -m 755 -d ${CHROOT}
	${INSTALL} -o root -g ${USER} -m 775 -d ${WRITEBACK}		 	\
		${DISKS} ${FMESSAGES} ${EMESSAGES}
	cd etc;									\
	${INSTALL} -o root -g wheel -m 555 ${RCDAEMON} 				\
		${DESTDIR}/etc/rc.d;						\
	${INSTALL} -o root -g wheel -m 444 ${CA} ${DESTDIR}/etc/ssl/authority;	\
	${INSTALL} -o root -g wheel -m 444 ${CERT} ${DESTDIR}/etc/ssl;		\
	${INSTALL} -o root -g wheel -m 400 ${KEY} ${DESTDIR}/etc/ssl/private;	\
	${INSTALL} -o root -g ${USER} -m 644 ${SPUB} ${DESTDIR}/etc/signify;	\
	${INSTALL} -o root -g wheel -m 644 ${VMCONFIG} ${DESTDIR}/etc;		\
	${INSTALL} -o root -g wheel -m 644 ${CONFIG} ${DESTDIR}/etc
.ifndef FULL
	cd images;								\
	mv ${BASEIMAGE} ${DESTDIR}/home/${USER};				\
	mv ${BASEVIVADO} ${DESTDIR}/home/${USER};				\
	chown root:wheel ${DESTDIR}/home/${USER}/*;				\
	chmod 444 ${DESTDIR}/home/${USER}/*
.else
	echo "THIS COPY WILL TAKE A WHILE! Don't turn off the power"
	cd images;									\
	${INSTALL} -o root -g wheel -m 444 ${BASEIMAGE} ${DESTDIR}/home/${USER};	\
	${INSTALL} -o root -g wheel -m 444 ${BASEVIVADO} ${DESTDIR}/home/${USER}
.endif
	rcctl stop vmd && rcctl start vmd
	sh scripts/rehash.sh /etc/ssl/authority

.PHONY: uninstall reinstall
uninstall: checkroot
	${SUDO} ./scripts/uninstall.sh
reinstall: uninstall install

.include <bsd.subdir.mk>
