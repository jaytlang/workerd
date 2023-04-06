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
static void	conn_echomsg(struct conn *, struct netmsg *);
static void	fork_client(void);
static void	end_test(int, short, void *);

static struct event	endtimer;

int		debug = 1, verbose = 1;

int myproc() { return PROC_ENGINE; }

static void 
end_test(int fd, short event, void *arg)
{
	warnx("server exiting");
	conn_teardownall();
	exit(0);

	(void)fd;
	(void)event;
	(void)arg;
}

static void
conn_accept(struct conn *c)
{
	warnx("connection accepted");
	conn_receive(c, conn_echomsg);
}

static void
conn_echomsg(struct conn *c, struct netmsg *msg)
{
	struct netmsg	*outgoing;

	char		*msglabel;
	char		*msgdata;
	uint64_t	 msgdatasize;
	uint8_t		 msgtype;

	if (msg == NULL) err(1, "conn_echomsg: received unintelligble message");
	else if (strlen(netmsg_error(msg)) > 0)
		errx(1, "conn_echomsg: netmsg is erroneous: %s", netmsg_error(msg));

	msgtype = netmsg_gettype(msg);

	msglabel = netmsg_getlabel(msg);
	if (msglabel == NULL)
		err(1, "conn_echomsg: netmsg_getlabel");

	msgdata = netmsg_getdata(msg, &msgdatasize);
	if (msgdata == NULL)
		err(1, "conn_echomsg: netmsg_getdata");

	warnx("received netmsg: type %u, label %s, data size %llu",
		msgtype, msglabel, msgdatasize);

	outgoing = netmsg_new(msgtype);
	if (outgoing == NULL)
		err(1, "conn_echomsg: netmsg_new");

	if (netmsg_setlabel(outgoing, msglabel) < 0)
		err(1, "conn_echomsg: netmsg_setlabel");

	if (netmsg_setdata(outgoing, msgdata, msgdatasize) < 0)
		err(1, "conn_echomsg: netmsg_setdata");

	conn_send(c, outgoing);

	free(msglabel);
	free(msgdata);
}

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

int
main()
{
	struct timeval	tv;

	event_init();
	conn_listen(conn_accept, VM_CONN_PORT, CONN_MODE_TCP);	

	fork_client();

	tv.tv_sec = 1;
	tv.tv_usec = 0;

	evtimer_set(&endtimer, end_test, NULL);
	evtimer_add(&endtimer, &tv);

	event_dispatch();
}
