PROG=		workerd
NOMAN=		noman
BINDIR?=	/usr/sbin

SRCS=	buffer.c	\
	conn.c		\
	log.c		\
	msgqueue.c	\
	netmsg.c	\

COPTS+= -Wall -Wextra -Werror -pedantic -I..
COPTS+= -Wno-unused-function -Wno-unneeded-internal-declaration 
LDADD+= -lutil -levent -lz -ltls

DEBUG+=	-g

.ifdef DEBUG
LDFLAGS+= -Wl,-E
.endif

.include <bsd.prog.mk>
