PROG=	test
BUNDLE=	build.bundle
NOMAN=	noman
SUDO=	doas

TESTUSER?=	_workerd
SRCDIR?=	../../src
RUNFLAGS?=
PYTHON!=	which python3 || true

COPTS+= -O0 -Wall -Wextra -Werror -pedantic -I${SRCDIR}
COPTS+= -Wno-unused-function -Wno-unneeded-internal-declaration
LDADD+= -lutil -levent -lz -ltls

DEBUG+= -g
LDFLAGS += -Wl,-E

${PROG}: ${LIBCRT0} ${OBJS} ${LIBC} ${CRTBEGIN} ${CRTEND} ${DPADD}
	${CC} ${LDFLAGS} ${LDSTATIC} -o ${.TARGET} ${OBJS:S/..\/..\/src\///g} ${LDADD}

.PHONY: run clean
run: ${BUNDLE}
	${SUDO} -u ${TESTUSER} ./${PROG} ${RUNFLAGS}

${BUNDLE}:
	pyf=`ls *.py 2>>/dev/null` &&					\
	txtf=`ls *.txt 2>>/dev/null || true` &&				\
	python3 ../bundle/bundle.py -csvf $@ $$pyf $$txtf || touch $@

clean:
	rm -f a.out [Ee]rrs mklog *.core y.tab.h a.out *.[od] ${PROG} ${BUNDLE}
