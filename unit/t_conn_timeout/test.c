#include <sys/types.h>
#include <sys/time.h>

#include <err.h>
#include <event.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "workerd.h"

#define	PYTHON3	"/usr/local/bin/python3"	

static void	conn_accept(struct conn *);
static void	conn_handletimeout(struct conn *);
static void	conn_dieonreceipt(struct conn *, struct netmsg *);

static void	fork_client(void);
static void	end_test(int, short, void *);

static struct event	endtimer;

int		debug = 1, verbose = 1;

int myproc() { return PROC_ENGINE; }

static void
fork_client(void)
{
	pid_t 	pid;

	pid = fork();

	if (pid < 0) log_fatal("fork");
	else if (pid == 0) {
		execl(PYTHON3, PYTHON3, "testclient.py", NULL);
		err(1, "exec");
	}
}

static void 
end_test(int fd, short event, void *arg)
{
	conn_teardownall();
	errx(1, "test timed out (no timeout handling occurred)");

	(void)fd;
	(void)event;
	(void)arg;
}

static void
conn_accept(struct conn *c)
{
	struct timeval	tv;

	warnx("connection accepted");
	conn_receive(c, conn_dieonreceipt);

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	conn_settimeout(c, &tv, conn_handletimeout);
}

static void
conn_dieonreceipt(struct conn *c, struct netmsg *msg)
{
	conn_teardownall();
	errx(1, "did not expect to receive message in this test");

	(void)c;
	(void)msg;
}

static void
conn_handletimeout(struct conn *c)
{
	conn_teardownall();
	warnx("caught timeout successfully");
	exit(0);

	(void)c;
}


int
main()
{
	struct timeval	tv;

	event_init();
	conn_listen(conn_accept, VM_CONN_PORT, CONN_MODE_TCP);	

	fork_client();

	tv.tv_sec = 2;
	tv.tv_usec = 0;

	evtimer_set(&endtimer, end_test, NULL);
	evtimer_add(&endtimer, &tv);

	event_dispatch();
}
