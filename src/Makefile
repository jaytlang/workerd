PROG=		workerd
NOMAN=		noman
BINDIR?=	/usr/sbin

SRCS=	buffer.c	\
	conn.c		\
	engine.c	\
	frontend.c	\
	ipcmsg.c	\
	log.c		\
	msgqueue.c	\
	netmsg.c	\
	proc.c		\
	vm.c		\
	wbfile.c	\
	workerd.c

COPTS+= -Wall -Wextra -Werror -pedantic -I..
COPTS+= -Wno-unused-function -Wno-unneeded-internal-declaration 
LDADD+= -lutil -levent -lz -ltls

DEBUG+=	-g

.ifdef DEBUG
LDFLAGS+= -Wl,-E
.endif

.include <bsd.prog.mk>
