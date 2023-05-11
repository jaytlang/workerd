PYTHON!=	which python3 || true
RCDAEMON=	workerd
FLAGS=		-v
SUDO=		doas

REGRESS_SETUP_ONCE= daemon_start
REGRESS_CLEANUP= daemon_stop

TESTFILES!= ls t_*.py | sed -E 's/(.*)\.py/\1/'
BUILDFILE= build.py

.for TESTFILE in ${TESTFILES}
${TESTFILE}:
	[ -n "${PYTHON}" ] && {			\
		${PYTHON} ${TESTFILE}.py;	\
		pgrep ${RCDAEMON} >/dev/null;	\
	} || echo "SKIPPED"

REGRESS_TARGETS+= ${TESTFILE}
.endfor

.PHONY: daemon_start daemon_stop depend

daemon_start:
	if [ ! -r "/etc/rc.d/${RCDAEMON}" ]; then		\
		echo "SKIPPED";					\
		exit 0;						\
	fi
	doas rcctl enable ${RCDAEMON}
	doas rcctl set ${RCDAEMON} flags -v
	doas rcctl start ${RCDAEMON}
	sleep 20

daemon_stop:
	doas rcctl stop ${RCDAEMON}
	doas rcctl disable ${RCDAEMON}
	doas rm -rf __pycache__
	doas rm -f ${BUILDFILE}

.include <bsd.regress.mk>
