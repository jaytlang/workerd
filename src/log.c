/* workerd logger
 * (c) jay lang 2023
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "workerd.h"

#define LOGTYPE_FATAL	(LOGTYPE_MAX + 1)

static void	_log_vwork(int, int, const char *, va_list);

static void
_log_vwork(int prio, int olderrno, const char *fmt, va_list ap)
{
	char	*nfmt;
	int	 realprio;

	if (prio == LOGTYPE_DEBUG && !verbose) return;

	switch (prio) {
	case LOGTYPE_MSG:
	case LOGTYPE_DEBUG:
		realprio = LOG_INFO;
		break;
	case LOGTYPE_WARN:
		realprio = LOG_WARNING;
		break;
	case LOGTYPE_FATAL:
		realprio = LOG_CRIT;
		break;
	default:
		log_fatalx("unknown logtype %d passed to log_write", prio);
		/* never reached */
	}

	if (debug) {
		if (olderrno == 0) vwarnx(fmt, ap);
		else vwarn(fmt, ap);
		return;
	}

	if (olderrno == 0) {
		vsyslog(realprio, fmt, ap);
		return;
	}

	/* out of memory condition */
	if (asprintf(&nfmt, "%s: %s\n", fmt, strerror(olderrno)) < 0)
		log_fatalx("out of memory");

	vsyslog(realprio, nfmt, ap);
	free(nfmt);
}

void
log_init(void)
{
	openlog(__progname, LOG_PID | LOG_NDELAY, LOG_DAEMON);	
}

void
log_write(int prio, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	_log_vwork(prio, errno, fmt, ap);
	va_end(ap);
}

void
log_writex(int prio, const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	_log_vwork(prio, 0, fmt, ap);
	va_end(ap);
}

__dead void
log_fatal(const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	_log_vwork(LOGTYPE_FATAL, errno, fmt, ap);
	va_end(ap);

	exit(1);
}

__dead void
log_fatalx(const char *fmt, ...)
{
	va_list	ap;

	va_start(ap, fmt);
	_log_vwork(LOGTYPE_FATAL, 0, fmt, ap);
	va_end(ap);

	exit(1);
}

